#include <gtest/gtest.h>

#include <helpers/rdm/RdmInbox.h>

#include <MemFileIO.h>
#include <SHA256.h>
#include <Utils.h>

#include <memory>
#include <string>
#include <vector>


using namespace rdm;

namespace {

const char* const INBOX_PATH = "/rdm/inbox";
const char* const REG_PATH = "/rdm/register";
const char* const CONTACTS_PATH = "/rdm/contacts";
constexpr uint16_t IN_PAYLOAD = 188;
constexpr uint16_t REG_PAYLOAD = 36;
constexpr uint16_t CONTACT_PAYLOAD = 52;

// MemFileIO plus one path whose writes fail, to break exactly one of the two files.
class PathFaultIO : public MemFileIO {
public:
  std::string fail_path;
  bool write(const char* path, uint32_t off, const uint8_t* buf, uint32_t len) override {
    if (fail_path == path) return false;
    return MemFileIO::write(path, off, buf, len);
  }
};

struct SentAckS { uint8_t prefix[6]; uint8_t ack[6]; };

class Host : public InboxHost {
public:
  int changed = 0;
  int reports_queued = 0;
  std::vector<SentAckS> ack_s;
  void onInboxChanged() override { changed++; }
  bool sendAckS(const uint8_t prefix[6], const uint8_t ack[6]) override {
    SentAckS s;
    memcpy(s.prefix, prefix, 6);
    memcpy(s.ack, ack, 6);
    ack_s.push_back(s);
    return true;
  }
  void onReportQueued() override { reports_queued++; }
};

struct Sender {
  uint8_t pub[32];
  explicit Sender(uint8_t seed) {
    for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(seed * 31 + i * 7 + 1);
  }
};

// Reference formulas from 03 par. 1a, computed here independently of the code under test.
void upstreamAckR(uint8_t out[4], uint32_t ts, uint8_t attempt, const std::string& text, const uint8_t pub[32]) {
  uint8_t temp[5 + 200];
  memcpy(temp, &ts, 4);
  temp[4] = attempt & 3;
  memcpy(&temp[5], text.data(), text.size());
  mesh::Utils::sha256(out, 4, temp, 5 + (int)text.size(), pub, 32);
}

void refHash(uint8_t* out, size_t n, const std::string& head, const std::string& text, const uint8_t pub[32]) {
  SHA256 sha;
  uint8_t full[32];
  sha.update(head.data(), head.size());
  sha.update(text.data(), text.size());
  sha.update(pub, 32);
  sha.finalize(full, 32);
  memcpy(out, full, n);
}

std::string le32(uint32_t v) {
  return std::string{(char)(v & 0xFF), (char)((v >> 8) & 0xFF), (char)((v >> 16) & 0xFF), (char)(v >> 24)};
}

void refAckS(uint8_t out[6], uint32_t ts, const std::string& text, const uint8_t pub[32]) {
  refHash(out, 6, std::string("RDMS") + le32(ts) + std::string(1, '\0'), text, pub);
}

void refKey(uint8_t out[4], uint32_t ts, const std::string& text, const uint8_t pub[32]) {
  refHash(out, 4, le32(ts), text, pub);
}

class InboxTest : public ::testing::Test {
protected:
  PathFaultIO io;
  Host host;
  uint16_t in_slots = 8;
  uint16_t reg_slots = 16;
  std::unique_ptr<RecordFile> inbox_file, reg_file, contacts_file;
  std::unique_ptr<ContactTable> contacts;
  std::unique_ptr<Inbox> inbox;
  bool recreated = false;
  std::vector<std::string> texts;   // keeps RecvInput::text alive

  Sender alice{1}, bob{2}, carol{3}, dave{4}, erin{5};
  uint8_t hash1[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  uint8_t hash2[8] = {2, 2, 2, 2, 2, 2, 2, 2};

  void SetUp() override { boot(); }

  // A reboot: new objects on the same flash.
  void boot() {
    inbox.reset();
    contacts.reset();
    inbox_file.reset(new RecordFile(io, INBOX_PATH, 1, IN_PAYLOAD, in_slots, false));
    reg_file.reset(new RecordFile(io, REG_PATH, 1, REG_PAYLOAD, reg_slots, true));
    contacts_file.reset(new RecordFile(io, CONTACTS_PATH, 1, CONTACT_PAYLOAD, 16, true));
    contacts.reset(new ContactTable(*contacts_file));
    ASSERT_TRUE(contacts->begin());
    inbox.reset(new Inbox(*inbox_file, *reg_file, *contacts, host));
    ASSERT_TRUE(inbox->begin(recreated));
  }

  RecvInput msg(const Sender& s, uint32_t ts, const std::string& text, uint8_t attempt = 0, bool cap = true,
                const uint8_t* mbx_hash = nullptr) {
    texts.push_back(text);
    RecvInput in;
    memset(&in, 0, sizeof(in));
    in.sender_pub = s.pub;
    in.ts = ts;
    in.flags = attempt & 3;
    in.txt_type = 0;
    in.text = texts.back().c_str();
    in.text_len = (uint8_t)text.size();
    in.sender_cap = cap;
    in.via_mailbox = mbx_hash != nullptr;
    in.mbx_hash = mbx_hash;
    in.path_len = 2;
    in.snr_x4 = -12;
    return in;
  }

  RecvDecision recv(const Sender& s, uint32_t ts, const std::string& text, uint32_t now = 1000, uint8_t attempt = 0,
                    bool cap = true, const uint8_t* mbx_hash = nullptr) {
    return inbox->onMessage(msg(s, ts, text, attempt, cap, mbx_hash), now);
  }

  QueryReply query(const Sender& s, uint32_t ts, const std::string& text) {
    QueryItem q;
    q.ts = ts;
    refKey(q.key, ts, text, s.pub);
    return inbox->query(s.pub, q);
  }

  // App reads the next message and confirms it with its following CMD_SYNC_NEXT_MESSAGE.
  bool syncOne(uint32_t now = 2000) {
    InRecord rec;
    uint16_t slot;
    if (!inbox->nextForApp(rec, slot)) return false;
    inbox->onHandedToApp(slot);
    inbox->onNextSyncRequest(now);
    return true;
  }

  std::vector<Report> reports() {
    Report r[MAX_BATCH];
    uint8_t n = inbox->pendingReports(r, MAX_BATCH);
    return std::vector<Report>(r, r + n);
  }
};

void expectAck6(const uint8_t ack6[6], const uint8_t ack4[4]) {
  EXPECT_EQ(0, memcmp(ack6, ack4, 4));
  EXPECT_EQ(ack6[4], 0);
  EXPECT_EQ(ack6[5], 0);
}

}  // namespace

TEST_F(InboxTest, FreshStorageReportsRecreatedAndReopenDoesNot) {
  EXPECT_TRUE(recreated);
  EXPECT_EQ(inbox->freeSlots(), in_slots);
  boot();
  EXPECT_FALSE(recreated);
}

TEST_F(InboxTest, NewMessageIsPersistedBeforeAckAndOfferedToApp) {
  RecvDecision d = recv(alice, 100, "hello", 1234, 2);
  EXPECT_EQ(d.result, RecvResult::NEW);
  EXPECT_TRUE(d.send_ack_r);
  EXPECT_TRUE(d.cap_byte);
  EXPECT_FALSE(d.send_ack_s);
  uint8_t ack[4];
  upstreamAckR(ack, 100, 2, "hello", alice.pub);
  EXPECT_EQ(0, memcmp(d.ack_r, ack, 4)) << "ACK_R is the upstream ack of the received attempt class";
  EXPECT_EQ(host.changed, 1);
  EXPECT_EQ(inbox->freeSlots(), in_slots - 1);

  boot();   // everything that justified the ACK is on flash
  InRecord rec;
  uint16_t slot;
  ASSERT_TRUE(inbox->nextForApp(rec, slot));
  EXPECT_EQ(0, memcmp(rec.sender_prefix, alice.pub, 6));
  EXPECT_EQ(rec.ts, 100u);
  EXPECT_EQ(rec.recv_time, 1234u);
  EXPECT_EQ(rec.path_len, 2);
  EXPECT_EQ(rec.snr_x4, -12);
  EXPECT_EQ(rec.flags, IF_SENDER_CAP);
  ASSERT_EQ(rec.text_len, 5);
  EXPECT_STREQ(rec.text, "hello");
  EXPECT_EQ(recv(alice, 100, "hello").result, RecvResult::DUPLICATE);
}

TEST_F(InboxTest, RetryWithOtherAttemptIsDuplicateAckedWithItsOwnClass) {
  ASSERT_EQ(recv(alice, 100, "hi", 1000, 0).result, RecvResult::NEW);
  RecvDecision d = recv(alice, 100, "hi", 1001, 253);   // firmware attempt 252 + 1
  EXPECT_EQ(d.result, RecvResult::DUPLICATE);
  EXPECT_TRUE(d.send_ack_r);
  EXPECT_FALSE(d.send_ack_s) << "not synced yet";
  uint8_t ack[4];
  upstreamAckR(ack, 100, 1, "hi", alice.pub);
  EXPECT_EQ(0, memcmp(d.ack_r, ack, 4));
  EXPECT_EQ(inbox->freeSlots(), in_slots - 1);
  EXPECT_EQ(host.changed, 1);
  EXPECT_TRUE(syncOne());
  InRecord rec;
  uint16_t slot;
  EXPECT_FALSE(inbox->nextForApp(rec, slot)) << "shown once";
}

TEST_F(InboxTest, SameTimestampOtherTextIsANewMessage) {
  ASSERT_EQ(recv(alice, 100, "a").result, RecvResult::NEW);
  EXPECT_EQ(recv(alice, 100, "b").result, RecvResult::NEW);
  EXPECT_EQ(recv(bob, 100, "a").result, RecvResult::NEW);
}

TEST_F(InboxTest, DuplicateOfSyncedMessageSendsAckSOnlyToDirectCapSender) {
  ASSERT_EQ(recv(alice, 100, "x").result, RecvResult::NEW);
  ASSERT_TRUE(syncOne());
  RecvDecision d = recv(alice, 100, "x");
  EXPECT_EQ(d.result, RecvResult::DUPLICATE);
  EXPECT_TRUE(d.send_ack_r);
  EXPECT_TRUE(d.send_ack_s);
  uint8_t ack_s[6];
  refAckS(ack_s, 100, "x", alice.pub);
  EXPECT_EQ(0, memcmp(d.ack_s, ack_s, 6));

  EXPECT_FALSE(recv(alice, 100, "x", 1000, 0, false).send_ack_s) << "sender without cap";
  RecvDecision m = recv(alice, 100, "x", 1000, 0, true, hash1);
  EXPECT_FALSE(m.send_ack_r) << "mailbox copy: proofs go via the mailbox";
  EXPECT_FALSE(m.send_ack_s);
}

TEST_F(InboxTest, InboxFullGivesNoAck) {
  const Sender* s[4] = {&alice, &bob, &carol, &dave};
  for (int i = 0; i < 8; i++) ASSERT_EQ(recv(*s[i / 2], 10 + i, "m").result, RecvResult::NEW) << i;
  EXPECT_EQ(inbox->freeSlots(), 0);
  RecvDecision d = recv(erin, 50, "late");
  EXPECT_EQ(d.result, RecvResult::INBOX_FULL);
  EXPECT_FALSE(d.send_ack_r);
  EXPECT_FALSE(d.send_ack_s);
  EXPECT_EQ(query(erin, 50, "late").state, QueryState::UNKNOWN);
  EXPECT_EQ(host.changed, 8);

  ASSERT_TRUE(syncOne());
  EXPECT_EQ(recv(erin, 50, "late").result, RecvResult::NEW) << "a sync frees a slot";
}

TEST_F(InboxTest, QuotaIsAQuarterOfTheInboxPerSender) {
  ASSERT_EQ(recv(alice, 1, "a").result, RecvResult::NEW);
  ASSERT_EQ(recv(alice, 2, "b").result, RecvResult::NEW);
  RecvDecision d = recv(alice, 3, "c");
  EXPECT_EQ(d.result, RecvResult::INBOX_FULL);
  EXPECT_FALSE(d.send_ack_r);
  EXPECT_EQ(recv(bob, 3, "c").result, RecvResult::NEW) << "other senders keep their share";
  EXPECT_EQ(recv(alice, 1, "a").result, RecvResult::DUPLICATE) << "quota never blocks dedup";
  ASSERT_TRUE(syncOne());
  EXPECT_EQ(recv(alice, 3, "c").result, RecvResult::NEW) << "only unsynced messages count";
}

TEST_F(InboxTest, WriteFailureGivesNoAckAndLeavesNoTrace) {
  io.failWritesAfter(0);
  RecvDecision d = recv(alice, 100, "lost?");
  EXPECT_EQ(d.result, RecvResult::STORE_ERROR);
  EXPECT_FALSE(d.send_ack_r);
  EXPECT_FALSE(d.send_ack_s);
  EXPECT_EQ(host.changed, 0);
  io.clearFaults();
  EXPECT_EQ(inbox->freeSlots(), in_slots);
  EXPECT_EQ(query(alice, 100, "lost?").state, QueryState::UNKNOWN) << "sender keeps retrying";
  EXPECT_EQ(recv(alice, 100, "lost?").result, RecvResult::NEW);
}

TEST_F(InboxTest, TornInboxWriteGivesNoAck) {
  io.failWritesAfter(10);
  EXPECT_EQ(recv(alice, 100, "torn").result, RecvResult::STORE_ERROR);
  io.clearFaults();
  boot();
  InRecord rec;
  uint16_t slot;
  EXPECT_FALSE(inbox->nextForApp(rec, slot));
  EXPECT_EQ(query(alice, 100, "torn").state, QueryState::UNKNOWN);
}

// Only the inbox write fails, the register write would succeed: still no ACK_R for a message that is not stored (#3518).
TEST_F(InboxTest, InboxWriteFailureAloneGivesNoAck) {
  io.fail_path = INBOX_PATH;
  RecvDecision d = recv(alice, 100, "not stored");
  EXPECT_EQ(d.result, RecvResult::STORE_ERROR);
  EXPECT_FALSE(d.send_ack_r);
  EXPECT_EQ(host.changed, 0);
  EXPECT_EQ(inbox->freeSlots(), in_slots);
  io.fail_path.clear();
  EXPECT_EQ(query(alice, 100, "not stored").state, QueryState::UNKNOWN) << "sender keeps retrying";
  EXPECT_EQ(recv(alice, 100, "not stored").result, RecvResult::NEW);
}

TEST_F(InboxTest, RegisterWriteFailureRollsBackTheInboxRecord) {
  io.fail_path = REG_PATH;
  RecvDecision d = recv(alice, 100, "half");
  EXPECT_EQ(d.result, RecvResult::STORE_ERROR);
  EXPECT_FALSE(d.send_ack_r);
  EXPECT_EQ(inbox->freeSlots(), in_slots);
  io.fail_path.clear();
  boot();
  InRecord rec;
  uint16_t slot;
  EXPECT_FALSE(inbox->nextForApp(rec, slot)) << "no half-stored message after reboot";
  EXPECT_EQ(recv(alice, 100, "half").result, RecvResult::NEW);
}

TEST_F(InboxTest, StockSenderTextUpToUpstreamMaximumIsStoredAndAcked) {
  std::string longest(INBOX_TEXT_MAX, 'x');   // 160: upstream MAX_TEXT_LEN, longer than a fork DM with trailer
  RecvDecision d = recv(alice, 100, longest, 1000, 0, false);
  EXPECT_EQ(d.result, RecvResult::NEW);
  EXPECT_TRUE(d.send_ack_r);
  uint8_t ack[4];
  upstreamAckR(ack, 100, 0, longest, alice.pub);
  EXPECT_EQ(0, memcmp(d.ack_r, ack, 4));
  boot();
  InRecord rec;
  uint16_t slot;
  ASSERT_TRUE(inbox->nextForApp(rec, slot));
  EXPECT_EQ(rec.text_len, INBOX_TEXT_MAX);
  EXPECT_EQ(std::string(rec.text), longest);
}

TEST_F(InboxTest, TextBeyondUpstreamMaximumIsNotAcked) {
  RecvDecision d = recv(alice, 100, std::string(INBOX_TEXT_MAX + 1, 'x'));
  EXPECT_EQ(d.result, RecvResult::STORE_ERROR);
  EXPECT_FALSE(d.send_ack_r);
}

TEST_F(InboxTest, SyncedOnlyByTheNextSyncRequest) {
  ASSERT_EQ(recv(alice, 100, "s").result, RecvResult::NEW);
  InRecord rec;
  uint16_t slot;
  ASSERT_TRUE(inbox->nextForApp(rec, slot));
  inbox->onHandedToApp(slot);
  EXPECT_EQ(query(alice, 100, "s").state, QueryState::ON_RADIO) << "handed is not synced";
  EXPECT_TRUE(host.ack_s.empty());
  EXPECT_EQ(inbox->freeSlots(), in_slots - 1);

  inbox->onNextSyncRequest(2000);
  QueryReply r = query(alice, 100, "s");
  EXPECT_EQ(r.state, QueryState::SYNCED);
  uint8_t ack_s[6];
  refAckS(ack_s, 100, "s", alice.pub);
  EXPECT_EQ(0, memcmp(r.ack, ack_s, 6));
  ASSERT_EQ(host.ack_s.size(), 1u);
  EXPECT_EQ(0, memcmp(host.ack_s[0].prefix, alice.pub, 6));
  EXPECT_EQ(0, memcmp(host.ack_s[0].ack, ack_s, 6));
  EXPECT_EQ(inbox->freeSlots(), in_slots);
  EXPECT_EQ(host.reports_queued, 0) << "direct message: no mailbox report";

  inbox->onNextSyncRequest(2001);
  EXPECT_EQ(host.ack_s.size(), 1u) << "a sync request confirms only what was handed";
  boot();
  EXPECT_EQ(query(alice, 100, "s").state, QueryState::SYNCED);
}

TEST_F(InboxTest, SyncRequestWithoutHandedMessageDoesNothing) {
  ASSERT_EQ(recv(alice, 100, "s").result, RecvResult::NEW);
  inbox->onNextSyncRequest(2000);
  EXPECT_EQ(query(alice, 100, "s").state, QueryState::ON_RADIO);
}

TEST_F(InboxTest, SenderWithoutCapGetsNoAckS) {
  ASSERT_EQ(recv(alice, 100, "s", 1000, 0, false).result, RecvResult::NEW);
  ASSERT_TRUE(syncOne());
  EXPECT_TRUE(host.ack_s.empty());
  EXPECT_EQ(query(alice, 100, "s").state, QueryState::SYNCED);
}

TEST_F(InboxTest, LostConnectionReoffersTheMessage) {
  ASSERT_EQ(recv(alice, 100, "r").result, RecvResult::NEW);
  InRecord rec;
  uint16_t slot;
  ASSERT_TRUE(inbox->nextForApp(rec, slot));
  inbox->onHandedToApp(slot);
  EXPECT_FALSE(inbox->nextForApp(rec, slot)) << "not offered twice on one connection";
  inbox->onClientDisconnected();
  inbox->onNextSyncRequest(2000);   // first request of the new connection
  EXPECT_EQ(query(alice, 100, "r").state, QueryState::ON_RADIO);
  ASSERT_TRUE(inbox->nextForApp(rec, slot));
  EXPECT_EQ(rec.ts, 100u);
}

TEST_F(InboxTest, OffersOldestFirstAndEachOnce) {
  ASSERT_EQ(recv(alice, 900, "second", 2000).result, RecvResult::NEW);
  ASSERT_EQ(recv(bob, 800, "first", 1000).result, RecvResult::NEW);   // older, but in the higher slot
  ASSERT_TRUE(syncOne());                                            // syncs "first", frees slot 1
  ASSERT_EQ(recv(carol, 700, "third", 3000).result, RecvResult::NEW);   // newest, reuses slot 1

  std::vector<std::string> order;
  InRecord rec;
  uint16_t slot;
  while (inbox->nextForApp(rec, slot)) {
    order.push_back(rec.text);
    inbox->onHandedToApp(slot);
  }
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], "second");
  EXPECT_EQ(order[1], "third");
}

TEST_F(InboxTest, QueryStatesCarryTheRightAck) {
  EXPECT_EQ(query(alice, 100, "q").state, QueryState::UNKNOWN);
  ASSERT_EQ(recv(alice, 100, "q", 1000, 2).result, RecvResult::NEW);
  QueryReply r = query(alice, 100, "q");
  EXPECT_EQ(r.state, QueryState::ON_RADIO);
  uint8_t ack_r[4];
  upstreamAckR(ack_r, 100, 2, "q", alice.pub);
  expectAck6(r.ack, ack_r);

  ASSERT_EQ(recv(alice, 100, "q", 1001, 3).result, RecvResult::DUPLICATE);
  expectAck6(query(alice, 100, "q").ack, ack_r);   // class Alice received first

  QueryItem foreign;
  foreign.ts = 100;
  refKey(foreign.key, 100, "q", alice.pub);
  EXPECT_EQ(inbox->query(bob.pub, foreign).state, QueryState::UNKNOWN) << "only the sender's own messages";
}

TEST_F(InboxTest, EvictsOnlySyncedLowestTimestampAndSetsWatermark) {
  reg_slots = 4;
  io.wipeAll();
  boot();
  ASSERT_EQ(recv(alice, 10, "a10").result, RecvResult::NEW);
  ASSERT_EQ(recv(alice, 20, "a20").result, RecvResult::NEW);
  ASSERT_TRUE(syncOne());
  ASSERT_TRUE(syncOne());
  ASSERT_EQ(recv(bob, 5, "b5").result, RecvResult::NEW);   // unsynced, lowest ts: must survive
  ASSERT_EQ(recv(bob, 6, "b6").result, RecvResult::NEW);

  ASSERT_EQ(recv(carol, 30, "c30").result, RecvResult::NEW);
  EXPECT_EQ(query(alice, 10, "a10").state, QueryState::EVICTED);
  EXPECT_EQ(query(alice, 20, "a20").state, QueryState::SYNCED);
  EXPECT_EQ(query(bob, 5, "b5").state, QueryState::ON_RADIO);
  ContactRdm* c = contacts->find(alice.pub);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->watermark, 10u);

  EXPECT_EQ(query(alice, 9, "never sent").state, QueryState::EVICTED) << "unknown key at or below the watermark";
  EXPECT_EQ(query(alice, 11, "never sent").state, QueryState::UNKNOWN);
  EXPECT_EQ(query(bob, 1, "never sent").state, QueryState::UNKNOWN) << "watermark is per sender";

  boot();
  EXPECT_EQ(query(alice, 10, "a10").state, QueryState::EVICTED) << "watermark is persistent";
}

TEST_F(InboxTest, RegisterFullOfUnsyncedMessagesGivesInboxFull) {
  reg_slots = 2;
  io.wipeAll();
  boot();
  ASSERT_EQ(recv(alice, 1, "a").result, RecvResult::NEW);
  ASSERT_EQ(recv(bob, 2, "b").result, RecvResult::NEW);
  RecvDecision d = recv(carol, 3, "c");
  EXPECT_EQ(d.result, RecvResult::INBOX_FULL);
  EXPECT_FALSE(d.send_ack_r);
  EXPECT_EQ(query(alice, 1, "a").state, QueryState::ON_RADIO);
}

TEST_F(InboxTest, MailboxCopyQueuesOnRadioReportUntilConfirmed) {
  RecvDecision d = recv(alice, 100, "via M", 1000, 1, true, hash1);
  EXPECT_EQ(d.result, RecvResult::NEW);
  EXPECT_FALSE(d.send_ack_r) << "no direct ACK for a mailbox copy (Y1)";
  EXPECT_EQ(host.reports_queued, 1);

  uint8_t ack_r[4];
  upstreamAckR(ack_r, 100, 1, "via M", alice.pub);
  std::vector<Report> r = reports();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(0, memcmp(r[0].pkt_hash, hash1, 8));
  EXPECT_EQ(r[0].result, ReportResult::ON_RADIO);
  expectAck6(r[0].ack, ack_r);

  EXPECT_EQ(reports().size(), 1u) << "reading does not consume";
  inbox->onReportsConfirmed(r.data(), 0);
  boot();
  ASSERT_EQ(reports().size(), 1u) << "unconfirmed report survives a reboot";
  inbox->onReportsConfirmed(r.data(), 1);
  EXPECT_TRUE(reports().empty());
  boot();
  EXPECT_TRUE(reports().empty());

  InRecord rec;
  uint16_t slot;
  ASSERT_TRUE(inbox->nextForApp(rec, slot));
  EXPECT_EQ(rec.flags, IF_VIA_MAILBOX | IF_SENDER_CAP);
  EXPECT_EQ(0, memcmp(rec.mbx_hash, hash1, 8));
}

TEST_F(InboxTest, MailboxCopySyncReportsSyncedViaMailboxOnly) {
  ASSERT_EQ(recv(alice, 100, "via M", 1000, 0, true, hash1).result, RecvResult::NEW);
  std::vector<Report> r = reports();
  inbox->onReportsConfirmed(r.data(), 1);
  ASSERT_TRUE(syncOne());
  EXPECT_TRUE(host.ack_s.empty()) << "G11: no separate ACK_S for a mailbox copy";
  EXPECT_EQ(host.reports_queued, 2) << "FETCH after sync";
  r = reports();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].result, ReportResult::SYNCED);
  uint8_t ack_s[6];
  refAckS(ack_s, 100, "via M", alice.pub);
  EXPECT_EQ(0, memcmp(r[0].ack, ack_s, 6));
  EXPECT_EQ(0, memcmp(r[0].pkt_hash, hash1, 8));
}

TEST_F(InboxTest, StaleConfirmationKeepsTheNewerReport) {
  ASSERT_EQ(recv(alice, 100, "via M", 1000, 0, true, hash1).result, RecvResult::NEW);
  std::vector<Report> sent = reports();   // ON_RADIO goes out in a FETCH
  ASSERT_TRUE(syncOne());                 // app syncs before the reply
  inbox->onReportsConfirmed(sent.data(), 1);
  std::vector<Report> r = reports();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].result, ReportResult::SYNCED);
}

TEST_F(InboxTest, MailboxDuplicateOfUnsyncedMessageReportsDuplicate) {
  ASSERT_EQ(recv(alice, 100, "both", 1000, 0).result, RecvResult::NEW);   // direct first (I4)
  RecvDecision d = recv(alice, 100, "both", 1100, 1, true, hash1);
  EXPECT_EQ(d.result, RecvResult::DUPLICATE);
  EXPECT_FALSE(d.send_ack_r);
  EXPECT_EQ(host.reports_queued, 1);
  std::vector<Report> r = reports();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].result, ReportResult::DUPLICATE);
  EXPECT_EQ(0, memcmp(r[0].pkt_hash, hash1, 8));
  uint8_t ack_r[4];
  upstreamAckR(ack_r, 100, 0, "both", alice.pub);
  expectAck6(r[0].ack, ack_r);

  inbox->onReportsConfirmed(r.data(), 1);
  ASSERT_TRUE(syncOne());
  ASSERT_EQ(host.ack_s.size(), 1u) << "first copy came direct: ACK_S direct";
  r = reports();
  ASSERT_EQ(r.size(), 1u) << "and the mailbox learns SYNCED too";
  EXPECT_EQ(r[0].result, ReportResult::SYNCED);
}

TEST_F(InboxTest, MailboxDuplicateOfSyncedMessageReportsSynced) {
  ASSERT_EQ(recv(alice, 100, "old", 1000, 0).result, RecvResult::NEW);
  ASSERT_TRUE(syncOne());
  ASSERT_EQ(recv(alice, 100, "old", 1100, 0, true, hash1).result, RecvResult::DUPLICATE);
  std::vector<Report> r = reports();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].result, ReportResult::SYNCED) << "otherwise the mailbox waits for a SYNCED that never comes";
  uint8_t ack_s[6];
  refAckS(ack_s, 100, "old", alice.pub);
  EXPECT_EQ(0, memcmp(r[0].ack, ack_s, 6));
}

TEST_F(InboxTest, SecondMailboxCopyWhileAReportIsPendingKeepsBothReports) {
  ASSERT_EQ(recv(alice, 100, "twice", 1000, 0, true, hash1).result, RecvResult::NEW);
  ASSERT_EQ(recv(alice, 100, "twice", 1100, 1, true, hash2).result, RecvResult::DUPLICATE);
  std::vector<Report> r = reports();
  ASSERT_EQ(r.size(), 2u);
  bool seen1 = false, seen2 = false;
  for (auto& x : r) {
    if (memcmp(x.pkt_hash, hash1, 8) == 0) seen1 = x.result == ReportResult::ON_RADIO;
    if (memcmp(x.pkt_hash, hash2, 8) == 0) seen2 = x.result == ReportResult::DUPLICATE;
  }
  EXPECT_TRUE(seen1);
  EXPECT_TRUE(seen2);
  inbox->onReportsConfirmed(r.data(), 2);
  EXPECT_TRUE(reports().empty());
}

TEST_F(InboxTest, MailboxCopyThatDoesNotFitReportsInboxFull) {
  ASSERT_EQ(recv(alice, 1, "a").result, RecvResult::NEW);
  ASSERT_EQ(recv(alice, 2, "b").result, RecvResult::NEW);
  RecvDecision d = recv(alice, 3, "c", 1000, 0, true, hash1);
  EXPECT_EQ(d.result, RecvResult::INBOX_FULL);
  EXPECT_FALSE(d.send_ack_r);
  EXPECT_EQ(host.reports_queued, 0) << "no immediate FETCH: it would fetch the same copy again";
  std::vector<Report> r = reports();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].result, ReportResult::INBOX_FULL);
  EXPECT_EQ(0, memcmp(r[0].pkt_hash, hash1, 8));
  inbox->onReportsConfirmed(r.data(), 1);
  EXPECT_TRUE(reports().empty());
}

TEST_F(InboxTest, UndecryptableCopyIsReportedUntilConfirmed) {
  inbox->onUndecryptable(hash1);
  EXPECT_EQ(host.reports_queued, 1);
  std::vector<Report> r = reports();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].result, ReportResult::UNDECRYPTABLE);
  EXPECT_EQ(0, memcmp(r[0].pkt_hash, hash1, 8));
  const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
  EXPECT_EQ(0, memcmp(r[0].ack, zero, 6));
  inbox->onUndecryptable(hash1);
  EXPECT_EQ(reports().size(), 1u) << "the same copy is reported once";
  inbox->onReportsConfirmed(r.data(), 0);
  EXPECT_EQ(reports().size(), 1u);
  inbox->onReportsConfirmed(r.data(), 1);
  EXPECT_TRUE(reports().empty());
  EXPECT_EQ(inbox->freeSlots(), in_slots) << "nothing stored";
}

TEST_F(InboxTest, PartialConfirmationKeepsTheRest) {
  ASSERT_EQ(recv(alice, 1, "a", 1000, 0, true, hash1).result, RecvResult::NEW);
  ASSERT_EQ(recv(bob, 2, "b", 1000, 0, true, hash2).result, RecvResult::NEW);
  std::vector<Report> r = reports();
  ASSERT_EQ(r.size(), 2u);
  inbox->onReportsConfirmed(r.data(), 1);
  std::vector<Report> left = reports();
  ASSERT_EQ(left.size(), 1u);
  EXPECT_EQ(0, memcmp(left[0].pkt_hash, r[1].pkt_hash, 8));
}

TEST_F(InboxTest, PendingReportsRespectMax) {
  ASSERT_EQ(recv(alice, 1, "a", 1000, 0, true, hash1).result, RecvResult::NEW);
  ASSERT_EQ(recv(bob, 2, "b", 1000, 0, true, hash2).result, RecvResult::NEW);
  Report r[1];
  EXPECT_EQ(inbox->pendingReports(r, 1), 1);
}

TEST_F(InboxTest, EntryWithPendingReportIsNotEvicted) {
  reg_slots = 1;
  io.wipeAll();
  boot();
  ASSERT_EQ(recv(alice, 1, "a", 1000, 0, true, hash1).result, RecvResult::NEW);
  ASSERT_TRUE(syncOne());   // synced, SYNCED report still unconfirmed
  EXPECT_EQ(recv(bob, 2, "b").result, RecvResult::INBOX_FULL);
  std::vector<Report> r = reports();
  inbox->onReportsConfirmed(r.data(), 1);
  EXPECT_EQ(recv(bob, 2, "b").result, RecvResult::NEW);
  EXPECT_EQ(query(alice, 1, "a").state, QueryState::EVICTED);
}

TEST_F(InboxTest, LostInboxDropsUnsyncedRegisterRowsSoTheSenderResends) {
  ASSERT_EQ(recv(alice, 1, "synced").result, RecvResult::NEW);
  ASSERT_TRUE(syncOne());
  ASSERT_EQ(recv(alice, 2, "pending").result, RecvResult::NEW);
  io.remove(INBOX_PATH);
  boot();
  EXPECT_TRUE(recreated) << "new store_id: the mailbox re-delivers (G12)";
  EXPECT_EQ(query(alice, 2, "pending").state, QueryState::UNKNOWN) << "D2: resend over loss";
  EXPECT_EQ(query(alice, 1, "synced").state, QueryState::SYNCED);
  EXPECT_EQ(recv(alice, 2, "pending").result, RecvResult::NEW);
}

TEST_F(InboxTest, LostRegisterKeepsUnsyncedMessagesForTheApp) {
  ASSERT_EQ(recv(alice, 1, "keep me").result, RecvResult::NEW);
  io.remove(REG_PATH);
  boot();
  EXPECT_TRUE(recreated);
  InRecord rec;
  uint16_t slot;
  ASSERT_TRUE(inbox->nextForApp(rec, slot)) << "Alice never deletes an unsynced message herself";
  EXPECT_STREQ(rec.text, "keep me");
  inbox->onHandedToApp(slot);
  inbox->onNextSyncRequest(2000);
  EXPECT_EQ(inbox->freeSlots(), in_slots);
}

TEST_F(InboxTest, NewRegisterClearsWatermarks) {
  reg_slots = 2;
  io.wipeAll();
  boot();
  ASSERT_EQ(recv(alice, 10, "a10").result, RecvResult::NEW);
  ASSERT_TRUE(syncOne());
  ASSERT_EQ(recv(bob, 20, "b20").result, RecvResult::NEW);
  ASSERT_EQ(recv(carol, 30, "c30").result, RecvResult::NEW);   // evicts a10
  ASSERT_EQ(query(alice, 5, "old").state, QueryState::EVICTED);

  io.remove(REG_PATH);
  boot();
  EXPECT_TRUE(recreated);
  ContactRdm* c = contacts->find(alice.pub);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->watermark, 0u);
  EXPECT_EQ(query(alice, 5, "old").state, QueryState::UNKNOWN) << "D2: resend over a false EVICTED";
  boot();
  EXPECT_EQ(contacts->find(alice.pub)->watermark, 0u) << "reset is persistent";
}

TEST_F(InboxTest, ReopenedRegisterKeepsWatermarks) {
  reg_slots = 1;
  io.wipeAll();
  boot();
  ASSERT_EQ(recv(alice, 10, "a10").result, RecvResult::NEW);
  ASSERT_TRUE(syncOne());
  ASSERT_EQ(recv(bob, 20, "b20").result, RecvResult::NEW);
  boot();
  EXPECT_FALSE(recreated);
  EXPECT_EQ(query(alice, 10, "a10").state, QueryState::EVICTED);
}

TEST_F(InboxTest, InterruptedSyncIsCompletedAtBoot) {
  ASSERT_EQ(recv(alice, 1, "x").result, RecvResult::NEW);
  InRecord rec;
  uint16_t slot;
  ASSERT_TRUE(inbox->nextForApp(rec, slot));
  inbox->onHandedToApp(slot);
  io.fail_path = INBOX_PATH;   // register says SYNCED, freeing the inbox record fails
  inbox->onNextSyncRequest(2000);
  io.fail_path.clear();
  boot();
  EXPECT_FALSE(inbox->nextForApp(rec, slot)) << "not offered again";
  EXPECT_EQ(inbox->freeSlots(), in_slots);
  EXPECT_EQ(query(alice, 1, "x").state, QueryState::SYNCED);
}

TEST_F(InboxTest, FailedSyncWriteKeepsTheMessageUnsynced) {
  ASSERT_EQ(recv(alice, 1, "x").result, RecvResult::NEW);
  InRecord rec;
  uint16_t slot;
  ASSERT_TRUE(inbox->nextForApp(rec, slot));
  inbox->onHandedToApp(slot);
  io.fail_path = REG_PATH;
  inbox->onNextSyncRequest(2000);
  io.fail_path.clear();
  EXPECT_TRUE(host.ack_s.empty()) << "no ACK_S without a persisted SYNCED";
  EXPECT_EQ(query(alice, 1, "x").state, QueryState::ON_RADIO);
  inbox->onClientDisconnected();
  EXPECT_TRUE(inbox->nextForApp(rec, slot)) << "D2: offered again";
}

TEST_F(InboxTest, WrongRecordSizesDisableTheInbox) {
  RecordFile bad(io, "/rdm/bad", 1, 100, 8, false);
  Inbox other(bad, *reg_file, *contacts, host);
  bool rec;
  EXPECT_FALSE(other.begin(rec));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
