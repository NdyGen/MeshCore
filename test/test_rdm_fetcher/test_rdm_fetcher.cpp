#include <gtest/gtest.h>

#include <helpers/rdm/RdmConfig.h>
#include <helpers/rdm/RdmFetcher.h>

#include <MemFileIO.h>

#include <memory>
#include <string>
#include <vector>


using namespace rdm;

namespace {

constexpr uint32_t T0 = 50000;
constexpr uint32_t STORE_ID = 0xA1B2C3D4;

struct SentFetch {
  uint32_t at;
  uint8_t flags;
  uint32_t store_id;
  std::vector<Report> reports;
};

class Rig : public InboxHost, public FetcherHost {
public:
  uint32_t now = T0;
  bool has_mbx = true;
  std::vector<SentFetch> fetches;
  Fetcher* fetcher = nullptr;

  // InboxHost: Node forwards a queued report to the Fetcher
  void onInboxChanged() override {}
  bool sendAckS(const uint8_t*, const uint8_t*) override { return true; }
  void onReportQueued() override {
    if (fetcher) fetcher->onReportQueued(now);
  }

  // FetcherHost
  uint32_t rnd = 0;   // 0: no jitter, so schedule asserts stay exact
  uint32_t random32() override { return rnd; }
  bool ownMailbox(uint8_t mbx_pub_out[32]) override {
    if (has_mbx) memset(mbx_pub_out, 0x4D, 32);
    return has_mbx;
  }
  bool sendFetch(uint8_t flags, uint32_t store_id, const Report* r, uint8_t n, uint32_t& est_timeout_ms) override {
    fetches.push_back(SentFetch{now, flags, store_id, std::vector<Report>(r, r + n)});
    est_timeout_ms = 4000;
    return true;
  }
};

struct Sender {
  uint8_t pub[32];
  explicit Sender(uint8_t seed) {
    for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(seed * 13 + i);
  }
};

class FetcherTest : public ::testing::Test {
protected:
  MemFileIO io;
  Rig rig;
  std::unique_ptr<RecordFile> inbox_file, reg_file, contacts_file;
  std::unique_ptr<ContactTable> contacts;
  std::unique_ptr<Inbox> inbox;
  std::unique_ptr<Fetcher> fetcher;
  std::vector<std::string> texts;
  Sender alice{1}, bob{2}, carol{3}, dave{4};

  void SetUp() override {
    inbox_file.reset(new RecordFile(io, "/rdm/inbox", 1, 188, 8, false));
    reg_file.reset(new RecordFile(io, "/rdm/register", 1, 36, 16, true));
    contacts_file.reset(new RecordFile(io, "/rdm/contacts", 1, 52, 16, true));
    contacts.reset(new ContactTable(*contacts_file));
    ASSERT_TRUE(contacts->begin());
    inbox.reset(new Inbox(*inbox_file, *reg_file, *contacts, rig));
    bool recreated;
    ASSERT_TRUE(inbox->begin(recreated));
    fetcher.reset(new Fetcher(*inbox, rig));
    rig.fetcher = fetcher.get();
  }

  void at(uint32_t t) {
    rig.now = t;
    fetcher->loop(t);
  }

  // H4 contract: loop(nextDue - 1) sends nothing, loop(nextDue) sends. Returns the due time.
  uint32_t expectFetchExactlyAtDue() {
    uint32_t due = fetcher->nextDue(rig.now);
    EXPECT_NE(due, UINT32_MAX);
    EXPECT_GE(due, rig.now);
    size_t n = rig.fetches.size();
    if (due > rig.now) {
      at(due - 1);
      EXPECT_EQ(rig.fetches.size(), n) << "loop(nextDue - 1) must not send";
    }
    at(due);
    EXPECT_EQ(rig.fetches.size(), n + 1) << "loop(nextDue) must send";
    return due;
  }

  void reply(uint8_t remaining, uint8_t reports_ok, bool had_payload, uint32_t t) {
    rig.now = t;
    fetcher->onFetchReply(remaining, reports_ok, had_payload, t);
  }

  // A copy from the mailbox, as Node hands it over while processing a FETCH reply.
  RecvResult mailboxCopy(const Sender& s, uint32_t ts, const std::string& text, uint8_t hash_byte) {
    texts.push_back(text);
    uint8_t hash[8];
    memset(hash, hash_byte, 8);
    RecvInput in;
    memset(&in, 0, sizeof(in));
    in.sender_pub = s.pub;
    in.ts = ts;
    in.text = texts.back().c_str();
    in.text_len = (uint8_t)text.size();
    in.sender_cap = true;
    in.via_mailbox = true;
    in.mbx_hash = hash;
    return inbox->onMessage(in, rig.now).result;
  }

  RecvResult direct(const Sender& s, uint32_t ts, const std::string& text) {
    texts.push_back(text);
    RecvInput in;
    memset(&in, 0, sizeof(in));
    in.sender_pub = s.pub;
    in.ts = ts;
    in.text = texts.back().c_str();
    in.text_len = (uint8_t)text.size();
    return inbox->onMessage(in, rig.now).result;
  }

  // Boot FETCH without jitter, answered empty.
  void bootAndSettle() {
    fetcher->begin(T0, STORE_ID, 0);
    at(T0);
    ASSERT_EQ(rig.fetches.size(), 1u);
    reply(0, 0, false, T0 + 2);
  }
};

}  // namespace

TEST_F(FetcherTest, BootFetchAfterJitterOfZeroToSixtySeconds) {
  const uint32_t randoms[] = {0, 1, 59, 60, 61, 12345, 0xFFFFFFFFu};
  for (uint32_t r : randoms) {
    SetUp();
    rig.fetches.clear();
    rig.now = T0;
    fetcher->begin(T0, STORE_ID, r);
    uint32_t due = fetcher->nextDue(T0);
    EXPECT_GE(due, T0);
    EXPECT_LE(due, T0 + RDM_FETCH_BOOT_JITTER_S) << "random " << r;
    EXPECT_EQ(due, T0 + r % (RDM_FETCH_BOOT_JITTER_S + 1));
    expectFetchExactlyAtDue();
  }
}

TEST_F(FetcherTest, NothingBeforeBegin) {
  EXPECT_EQ(fetcher->nextDue(T0), UINT32_MAX);
  at(T0 + 100000);
  EXPECT_TRUE(rig.fetches.empty());
}

TEST_F(FetcherTest, RegularFetchEverySixtyMinutes) {
  bootAndSettle();
  EXPECT_EQ(fetcher->nextDue(rig.now), T0 + RDM_FETCH_INTERVAL_S);
  expectFetchExactlyAtDue();
  reply(0, 0, false, rig.now + 3);
  EXPECT_EQ(fetcher->nextDue(rig.now), T0 + 2 * RDM_FETCH_INTERVAL_S);
  expectFetchExactlyAtDue();
}

TEST_F(FetcherTest, StoreIdInEveryFetch) {
  bootAndSettle();
  expectFetchExactlyAtDue();
  reply(1, 0, true, rig.now + 2);
  expectFetchExactlyAtDue();
  ASSERT_EQ(rig.fetches.size(), 3u);
  for (auto& f : rig.fetches) EXPECT_EQ(f.store_id, STORE_ID);
}

TEST_F(FetcherTest, FetchRightAfterSyncCarriesTheSyncedReport) {
  bootAndSettle();
  rig.now = T0 + 100;
  ASSERT_EQ(mailboxCopy(alice, 10, "hi", 0x11), RecvResult::NEW);
  uint32_t due = expectFetchExactlyAtDue();
  EXPECT_EQ(due, T0 + 100) << "the ON_RADIO report goes out at once";
  ASSERT_EQ(rig.fetches.back().reports.size(), 1u);
  EXPECT_EQ(rig.fetches.back().reports[0].result, ReportResult::ON_RADIO);
  reply(0, 1, false, T0 + 102);

  rig.now = T0 + 900;   // the app syncs
  InRecord rec;
  uint16_t slot;
  ASSERT_TRUE(inbox->nextForApp(rec, slot));
  inbox->onHandedToApp(slot);
  inbox->onNextSyncRequest(rig.now);
  due = expectFetchExactlyAtDue();
  EXPECT_EQ(due, T0 + 900);
  ASSERT_EQ(rig.fetches.back().reports.size(), 1u);
  EXPECT_EQ(rig.fetches.back().reports[0].result, ReportResult::SYNCED);
  reply(0, 1, false, T0 + 902);
  EXPECT_EQ(fetcher->nextDue(rig.now), T0 + 900 + RDM_FETCH_INTERVAL_S);
}

TEST_F(FetcherTest, DirectSyncDoesNotFetch) {
  bootAndSettle();
  rig.now = T0 + 100;
  ASSERT_EQ(direct(alice, 10, "direct"), RecvResult::NEW);
  InRecord rec;
  uint16_t slot;
  ASSERT_TRUE(inbox->nextForApp(rec, slot));
  inbox->onHandedToApp(slot);
  inbox->onNextSyncRequest(rig.now);
  EXPECT_EQ(fetcher->nextDue(rig.now), T0 + RDM_FETCH_INTERVAL_S);
}

TEST_F(FetcherTest, MailboxAdvertTriggersAtMostEveryTenMinutes) {
  bootAndSettle();
  rig.now = T0 + RDM_MBX_ADVERT_MIN_S - 1;
  fetcher->onMailboxAdvert(rig.now);
  EXPECT_EQ(fetcher->nextDue(rig.now), T0 + RDM_FETCH_INTERVAL_S) << "advert within 10 min of the last FETCH";

  rig.now = T0 + RDM_MBX_ADVERT_MIN_S;
  fetcher->onMailboxAdvert(rig.now);
  EXPECT_EQ(expectFetchExactlyAtDue(), T0 + RDM_MBX_ADVERT_MIN_S);
  reply(0, 0, false, rig.now + 2);
  EXPECT_EQ(fetcher->nextDue(rig.now), T0 + RDM_MBX_ADVERT_MIN_S + RDM_FETCH_INTERVAL_S)
      << "the interval restarts at the last FETCH";

  rig.now += 60;
  fetcher->onMailboxAdvert(rig.now);
  at(rig.now);
  EXPECT_EQ(rig.fetches.size(), 2u);
}

TEST_F(FetcherTest, ContinuesDirectlyWhileTheMailboxHasMore) {
  bootAndSettle();
  expectFetchExactlyAtDue();
  uint32_t first = rig.now;
  rig.now = first + 2;
  ASSERT_EQ(mailboxCopy(alice, 1, "one", 0x21), RecvResult::NEW);
  reply(2, 0, true, first + 2);
  uint32_t due = expectFetchExactlyAtDue();
  EXPECT_EQ(due, first + RDM_MBX_REQ_GAP_S) << "respects the mailbox rate limit";
  ASSERT_EQ(rig.fetches.back().reports.size(), 1u) << "the report rides along";

  uint32_t second = rig.now;
  reply(0, 1, false, second + 2);
  EXPECT_EQ(fetcher->nextDue(rig.now), second + RDM_FETCH_INTERVAL_S) << "resterend 0: back to the interval";
}

TEST_F(FetcherTest, NoPayloadWhenTheInboxIsFull) {
  const Sender* s[4] = {&alice, &bob, &carol, &dave};
  for (int i = 0; i < 8; i++) ASSERT_EQ(direct(*s[i / 2], 100 + i, "fill"), RecvResult::NEW);
  ASSERT_EQ(inbox->freeSlots(), 0);
  fetcher->begin(T0, STORE_ID, 0);
  at(T0);
  ASSERT_EQ(rig.fetches.size(), 1u);
  EXPECT_EQ(rig.fetches[0].flags & FETCH_FLAG_NO_PAYLOAD, FETCH_FLAG_NO_PAYLOAD);
  reply(3, 0, false, T0 + 2);
  EXPECT_EQ(fetcher->nextDue(rig.now), T0 + RDM_FETCH_INTERVAL_S) << "no point asking for more";
}

TEST_F(FetcherTest, PayloadAllowedWhenTheInboxHasRoom) {
  bootAndSettle();
  EXPECT_EQ(rig.fetches[0].flags & FETCH_FLAG_NO_PAYLOAD, 0);
}

TEST_F(FetcherTest, RejectedCopyDoesNotLoopOnTheSameCopy) {
  bootAndSettle();
  rig.now = T0 + 10;
  ASSERT_EQ(direct(alice, 1, "a"), RecvResult::NEW);
  ASSERT_EQ(direct(alice, 2, "b"), RecvResult::NEW);   // alice's quota is used up
  expectFetchExactlyAtDue();
  rig.now += 2;
  ASSERT_EQ(mailboxCopy(alice, 3, "c", 0x31), RecvResult::INBOX_FULL);
  reply(4, 0, true, rig.now);
  EXPECT_EQ(fetcher->nextDue(rig.now), rig.fetches.back().at + RDM_FETCH_INTERVAL_S);
  expectFetchExactlyAtDue();
  ASSERT_EQ(rig.fetches.back().reports.size(), 1u);
  EXPECT_EQ(rig.fetches.back().reports[0].result, ReportResult::INBOX_FULL);
}

TEST_F(FetcherTest, ReportsStayUntilConfirmed) {
  bootAndSettle();
  rig.now = T0 + 50;
  ASSERT_EQ(mailboxCopy(alice, 1, "a", 0x41), RecvResult::NEW);
  ASSERT_EQ(mailboxCopy(bob, 2, "b", 0x42), RecvResult::NEW);
  expectFetchExactlyAtDue();
  ASSERT_EQ(rig.fetches.back().reports.size(), 2u);
  reply(0, 0, false, rig.now + 2);   // the mailbox took none
  EXPECT_EQ(fetcher->nextDue(rig.now), rig.fetches.back().at + RDM_FETCH_INTERVAL_S)
      << "no retry storm when the mailbox does not take reports";
  expectFetchExactlyAtDue();
  ASSERT_EQ(rig.fetches.back().reports.size(), 2u);
  reply(0, 1, false, rig.now + 2);
  expectFetchExactlyAtDue();
  ASSERT_EQ(rig.fetches.back().reports.size(), 1u);
  EXPECT_EQ(rig.fetches.back().reports[0].pkt_hash[0], 0x42);
  reply(0, 1, false, rig.now + 2);
  expectFetchExactlyAtDue();
  EXPECT_TRUE(rig.fetches.back().reports.empty());
}

TEST_F(FetcherTest, NoOwnMailboxNoFetch) {
  rig.has_mbx = false;
  fetcher->begin(T0, STORE_ID, 0);
  EXPECT_EQ(fetcher->nextDue(T0), UINT32_MAX);
  at(T0 + 2 * RDM_FETCH_INTERVAL_S);
  EXPECT_TRUE(rig.fetches.empty());
  rig.has_mbx = true;
  EXPECT_EQ(fetcher->nextDue(rig.now), rig.now) << "an overdue FETCH goes as soon as a mailbox is set";
  at(rig.now);
  EXPECT_EQ(rig.fetches.size(), 1u);
}

TEST_F(FetcherTest, OneFetchInFlightAtATime) {
  fetcher->begin(T0, STORE_ID, 0);
  at(T0);
  ASSERT_EQ(rig.fetches.size(), 1u);
  rig.now = T0 + 1;
  ASSERT_EQ(mailboxCopy(alice, 1, "a", 0x51), RecvResult::NEW);
  fetcher->onMailboxAdvert(rig.now);
  at(T0 + 2);
  EXPECT_EQ(rig.fetches.size(), 1u) << "waits for the reply";
  reply(0, 0, true, T0 + 3);
  uint32_t due = expectFetchExactlyAtDue();
  EXPECT_EQ(due, T0 + RDM_MBX_REQ_GAP_S);
  EXPECT_EQ(rig.fetches.back().reports.size(), 1u);
}

TEST_F(FetcherTest, G17_TimeoutGivesOneRepeatThenTheInterval) {
  fetcher->begin(T0, STORE_ID, 0);
  at(T0);
  uint32_t timeout = fetcher->nextDue(T0);
  EXPECT_GE(timeout, T0 + RDM_WAIT_ACK_MIN_S) << "max(2 x est, 30 s)";
  at(timeout - 1);
  at(timeout);
  EXPECT_EQ(rig.fetches.size(), 1u) << "no immediate FETCH at the time-out";
  EXPECT_EQ(fetcher->nextDue(rig.now), timeout + RDM_RETRY_FIRST_MIN_S);
  EXPECT_EQ(expectFetchExactlyAtDue(), timeout + RDM_RETRY_FIRST_MIN_S);
  uint32_t timeout2 = fetcher->nextDue(rig.now);
  at(timeout2);
  EXPECT_EQ(fetcher->nextDue(rig.now), T0 + RDM_FETCH_INTERVAL_S)
      << "the repeat is not repeated, the interval counts from the original FETCH";
  EXPECT_EQ(expectFetchExactlyAtDue(), T0 + RDM_FETCH_INTERVAL_S);
}

TEST_F(FetcherTest, G17_RepeatWaitsSixtyToOneHundredTwentySeconds) {
  const uint32_t randoms[] = {60, 0xFFFFFFFFu, 12345};
  for (uint32_t r : randoms) {
    SetUp();
    rig.fetches.clear();
    rig.rnd = r;
    rig.now = T0;
    fetcher->begin(T0, STORE_ID, 0);
    at(T0);
    uint32_t timeout = fetcher->nextDue(T0);
    at(timeout);
    uint32_t repeat = fetcher->nextDue(rig.now);
    EXPECT_GE(repeat, timeout + RDM_RETRY_FIRST_MIN_S) << r;
    EXPECT_LE(repeat, timeout + RDM_RETRY_FIRST_MAX_S) << r;
    EXPECT_EQ(repeat, timeout + RDM_RETRY_FIRST_MIN_S + r % 61) << r;
  }
}

TEST_F(FetcherTest, G17_AnsweredRepeatClearsAndLateReplyCancelsIt) {
  fetcher->begin(T0, STORE_ID, 0);
  at(T0);
  uint32_t timeout = fetcher->nextDue(T0);
  at(timeout);
  reply(0, 0, false, timeout + 5);   // late reply: the mailbox is there
  EXPECT_EQ(fetcher->nextDue(rig.now), T0 + RDM_FETCH_INTERVAL_S);
}

TEST_F(FetcherTest, G17_IntervalIsJitteredUpToSixtySeconds) {
  bootAndSettle();
  EXPECT_EQ(fetcher->nextDue(rig.now), T0 + RDM_FETCH_INTERVAL_S) << "random 0: exactly the interval";

  const uint32_t randoms[] = {60, 0xFFFFFFFFu, 7, 1000003};
  for (uint32_t r : randoms) {
    SetUp();
    rig.fetches.clear();
    rig.rnd = r;
    rig.now = T0;
    fetcher->begin(T0, STORE_ID, 0);
    at(T0);
    uint32_t due = fetcher->nextDue(rig.now);
    reply(0, 0, false, T0 + 2);
    due = fetcher->nextDue(rig.now);
    EXPECT_GE(due, T0 + 3600) << r;
    EXPECT_LE(due, T0 + 3660) << r;
    EXPECT_EQ(due, T0 + 3600 + r % 61) << r;
    expectFetchExactlyAtDue();
  }
}

TEST_F(FetcherTest, G17_TwoNodesOnTheSameSecondDrift) {
  bootAndSettle();
  uint32_t a = fetcher->nextDue(rig.now);
  rig.rnd = 41;
  SetUp();
  rig.fetches.clear();
  rig.now = T0;
  bootAndSettle();
  EXPECT_NE(fetcher->nextDue(rig.now), a) << "same start, other random value, other FETCH second";
}

TEST_F(FetcherTest, HostTimeoutAndLateReply) {
  fetcher->begin(T0, STORE_ID, 0);
  rig.now = T0;
  ASSERT_EQ(mailboxCopy(alice, 1, "a", 0x61), RecvResult::NEW);
  at(T0);
  ASSERT_EQ(rig.fetches.size(), 1u);
  ASSERT_EQ(rig.fetches[0].reports.size(), 1u);
  rig.now = T0 + 10;
  fetcher->onFetchTimeout(rig.now);
  EXPECT_EQ(fetcher->nextDue(rig.now), T0 + 10 + RDM_RETRY_FIRST_MIN_S) << "G17b repeat";
  reply(0, 1, false, T0 + 12);   // arrives after all: the mailbox did take the report
  EXPECT_EQ(fetcher->nextDue(rig.now), T0 + RDM_FETCH_INTERVAL_S) << "late reply cancels the repeat";
  Report r[MAX_BATCH];
  EXPECT_EQ(inbox->pendingReports(r, MAX_BATCH), 0);
  reply(0, 1, false, T0 + 13);   // a duplicate reply confirms nothing twice
}

TEST_F(FetcherTest, NextDueIsExactAfterEveryTransition) {
  fetcher->begin(T0, STORE_ID, 17);
  expectFetchExactlyAtDue();                           // boot
  reply(1, 0, true, rig.now + 2);
  expectFetchExactlyAtDue();                           // continuation
  reply(0, 0, false, rig.now + 2);
  expectFetchExactlyAtDue();                           // interval
  reply(0, 0, false, rig.now + 2);
  rig.now += RDM_MBX_ADVERT_MIN_S;
  fetcher->onMailboxAdvert(rig.now);
  expectFetchExactlyAtDue();                           // advert
  reply(0, 0, false, rig.now + 1);
  rig.now += 1;
  ASSERT_EQ(mailboxCopy(alice, 1, "a", 0x71), RecvResult::NEW);
  expectFetchExactlyAtDue();                           // report queued, inside the rate-limit gap
  uint32_t t = rig.now;
  uint32_t timeout = fetcher->nextDue(t);
  at(timeout - 1);
  EXPECT_EQ(fetcher->nextDue(rig.now), timeout) << "still in flight";
  at(timeout);
  EXPECT_EQ(fetcher->nextDue(rig.now), timeout + RDM_RETRY_FIRST_MIN_S);
  expectFetchExactlyAtDue();                           // G17b repeat
  at(fetcher->nextDue(rig.now));
  EXPECT_EQ(fetcher->nextDue(rig.now), t + RDM_FETCH_INTERVAL_S);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
