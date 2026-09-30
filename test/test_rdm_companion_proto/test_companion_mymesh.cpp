// The real companion firmware (MyMesh with WITH_RELIABLE_DM) in the simulator, driven through the companion protocol:
// the frames and rules WP6 adds on top of rdm::Node (06 par. 3.13-3.14, 03 par. 1d and 11).
// The simulator gives the companion a SimFileIO on its flash (hook H5, CompanionNode::createMesh).

#include <gtest/gtest.h>

#include <CompanionFrames.h>
#include <CompanionNode.h>
#include <RdmCompanionProto.h>
#include <RdmSimMailbox.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using namespace sim;
using namespace sim::frames;
using namespace rdm::companion;
using Status = ChatNode::Status;

namespace {

typedef std::vector<uint8_t> Frame;

// A companion whose emulated app can be switched off, so a test reads every frame on the link itself: the app keeps
// only the first reply frame of a command, and skips channel messages when it syncs.
class RawCompanion : public CompanionNode {
public:
  using CompanionNode::CompanionNode;
  bool raw = false;

  void goRaw(Simulator& s) {
    app().disconnect();
    s.run_for(100);   // MyMesh sees the link drop
    raw = true;
    link().setConnected(true);
    s.run_for(100);
  }

  // Sends one frame and collects everything the radio writes until done() holds.
  std::vector<std::vector<uint8_t>> exchange(Simulator& s, const std::vector<uint8_t>& f,
                                             const std::function<bool(const std::vector<std::vector<uint8_t>>&)>& done) {
    std::vector<std::vector<uint8_t>> got;
    link().to_radio.push_back(f);
    s.run_until(
        [&] {
          while (!link().to_app.empty()) {
            got.push_back(link().to_app.front());
            link().to_app.pop_front();
          }
          return done(got);
        },
        5000);
    return got;
  }

protected:
  void onLoop() override {
    if (!raw) CompanionNode::onLoop();
  }
};

void introduce(Simulator& s, CompanionNode& a, CompanionNode& b) {
  a.app().connect();
  a.app().advert(true);
  s.run_for(5000);
  b.app().connect();
  b.app().advert(true);
  s.run_for(5000);
}

struct Pair {
  Simulator s;
  RawCompanion& bob;
  RawCompanion& alice;
  explicit Pair(uint64_t seed) : s(seed), bob(s.add<RawCompanion>("bob")), alice(s.add<RawCompanion>("alice")) {
    s.link(bob, alice);
    introduce(s, alice, bob);
  }
};

// Sends a command and waits for its reply frame (codes below 0x80).
Frame command(Simulator& s, CompanionNode& n, const Frame& f) {
  Frame reply;
  bool got = false;
  n.app().send_command(f, [&](const Frame& r) {
    reply = r;
    got = true;
  });
  s.run_until([&] { return got; }, 5000);
  return reply;
}

void enable(Simulator& s, CompanionNode& n) {
  uint8_t f[2];
  Frame r = command(s, n, Frame(f, f + buildEnable(f)));
  ASSERT_EQ(r, Frame{RESP_CODE_OK});
}

std::vector<rdm::UserStatus> statusesFor(const CompanionApp& app, uint32_t app_ack, size_t from = 0) {
  std::vector<rdm::UserStatus> out;
  for (size_t i = from; i < app.push_log().size(); i++) {
    const Frame& f = app.push_log()[i];
    StatusPush p;
    if (decodeStatus(f.data(), f.size(), p) && p.app_ack == app_ack) out.push_back(p.status);
  }
  return out;
}

size_t confirmsFor(const CompanionApp& app, uint32_t app_ack) {
  size_t n = 0;
  for (const Frame& f : app.push_log()) n += f.size() >= 5 && f[0] == PUSH_CODE_SEND_CONFIRMED && rdm::get32(&f[1]) == app_ack;
  return n;
}

bool endsWith(const std::vector<Frame>& frames, uint8_t code) { return !frames.empty() && frames.back()[0] == code; }

// CMD_RDM_LIST_OUTBOX on the raw link: START, the entries, END.
std::vector<OutboxRow> listOutbox(Simulator& s, RawCompanion& n, uint8_t& start_count) {
  uint8_t cmd[1];
  std::vector<Frame> frames = n.exchange(s, Frame(cmd, cmd + buildListOutbox(cmd)),
                                         [](const std::vector<Frame>& g) { return endsWith(g, RESP_CODE_RDM_OUTBOX_END); });
  std::vector<OutboxRow> rows;
  start_count = 0xFF;
  bool started = false, ended = false;
  for (const Frame& f : frames) {
    if (f[0] >= 0x80) continue;   // pushes may interleave
    EXPECT_FALSE(ended) << "frame after END";
    OutboxRow r;
    if (!started) {
      EXPECT_TRUE(decodeOutboxStart(f.data(), f.size(), start_count)) << "first reply frame is START";
      started = true;
    } else if (decodeOutboxEntry(f.data(), f.size(), r)) {
      rows.push_back(r);
    } else {
      EXPECT_EQ(f, Frame{RESP_CODE_RDM_OUTBOX_END});
      ended = true;
    }
  }
  EXPECT_TRUE(ended);
  return rows;
}

}

TEST(RdmCompanion, NodeRunsOnTheCompanionFlash) {
  Pair p(601);
  ASSERT_TRUE(p.bob.firmware().rdm().enabled()) << "simulator hook H5 missing: DataStore::setRdmIO(SimFileIO)";
  ASSERT_TRUE(p.alice.firmware().rdm().enabled());
}

// K9 and decision 6: a standard app never sees 0x91, and gets SEND_CONFIRMED with the ack RESP_CODE_SENT gave it.
TEST(RdmCompanion, StandardAppGetsSendConfirmedButNoStatusPushes) {
  Pair p(602);
  int id = p.bob.app().send_text(p.alice, "no enable here");
  ASSERT_TRUE(p.s.run_until([&] { return p.bob.app().sent(id).status == Status::DELIVERED; }, 30000));
  uint32_t app_ack = p.bob.app().sent(id).expected_ack;
  EXPECT_NE(app_ack, 0u);
  EXPECT_EQ(confirmsFor(p.bob.app(), app_ack), 1u);
  p.s.run_for(20000);
  EXPECT_EQ(p.bob.app().push_count(PUSH_CODE_RDM_STATUS), 0u);
  EXPECT_EQ(p.alice.app().inbox_count("no enable here"), 1u);
}

// G1: QUEUED, ON_RADIO, DELIVERED for a client with CMD_RDM_ENABLE; the inbox record reaches Alice's app once.
TEST(RdmCompanion, RdmClientSeesQueuedOnRadioDelivered) {
  Pair p(603);
  enable(p.s, p.bob);
  int id = p.bob.app().send_text(p.alice, "tracked");
  ASSERT_TRUE(p.s.run_until([&] { return p.bob.app().sent(id).answered; }, 5000));
  uint32_t app_ack = p.bob.app().sent(id).expected_ack;
  ASSERT_TRUE(p.s.run_until(
      [&] {
        auto st = statusesFor(p.bob.app(), app_ack);
        return !st.empty() && st.back() == rdm::UserStatus::DELIVERED;
      },
      120000));
  std::vector<rdm::UserStatus> expect = {rdm::UserStatus::QUEUED, rdm::UserStatus::ON_RADIO, rdm::UserStatus::DELIVERED};
  EXPECT_EQ(statusesFor(p.bob.app(), app_ack), expect);
  EXPECT_EQ(confirmsFor(p.bob.app(), app_ack), 1u);
  EXPECT_EQ(p.bob.app().sent(id).status, Status::DELIVERED);
  EXPECT_EQ(p.alice.app().inbox_count("tracked"), 1u);
}

// G1: the first 0x91 of a message follows RESP_CODE_SENT, so the client knows the app_ack it refers to.
TEST(RdmCompanion, FirstStatusPushFollowsRespCodeSent) {
  Pair p(604);
  enable(p.s, p.bob);
  p.s.unlink(p.bob, p.alice);
  uint32_t ts = p.s.wall_epoch();
  Frame f = buildSendTxt(p.alice.identity().pub_key, ts, 0, "order");
  size_t pushes_at_reply = SIZE_MAX;
  bool got = false;
  Frame reply;
  p.bob.app().send_command(f, [&](const Frame& r) {
    reply = r;
    pushes_at_reply = p.bob.app().push_log().size();
    got = true;
  });
  ASSERT_TRUE(p.s.run_until([&] { return got; }, 5000));
  SentReply sent;
  ASSERT_TRUE(decodeSent(reply, sent));
  uint32_t app_ack = sent.expected_ack;
  for (size_t i = 0; i < pushes_at_reply; i++) {
    StatusPush sp;
    const Frame& pf = p.bob.app().push_log()[i];
    EXPECT_FALSE(decodeStatus(pf.data(), pf.size(), sp) && sp.app_ack == app_ack) << "0x91 before RESP_CODE_SENT";
  }
  ASSERT_TRUE(p.s.run_until([&] { return !statusesFor(p.bob.app(), app_ack).empty(); }, 2000));
  EXPECT_EQ(statusesFor(p.bob.app(), app_ack).front(), rdm::UserStatus::QUEUED);
}

// I8 (a): an app retry with the same timestamp and text lands on the same outbox entry, with the ACK of its attempt.
TEST(RdmCompanion, AppRetryLandsOnTheSameOutboxEntry) {
  Pair p(605);
  p.s.unlink(p.bob, p.alice);
  uint32_t ts = p.s.wall_epoch();
  int a0 = p.bob.app().send_text(p.alice, "retry me", 0, ts);
  int a1 = p.bob.app().send_text(p.alice, "retry me", 1, ts);
  ASSERT_TRUE(p.s.run_until([&] { return p.bob.app().sent(a1).answered; }, 10000));
  EXPECT_NE(p.bob.app().sent(a0).expected_ack, p.bob.app().sent(a1).expected_ack);

  uint32_t ack1 = p.bob.app().sent(a1).expected_ack;
  p.bob.goRaw(p.s);
  uint8_t count;
  std::vector<OutboxRow> rows = listOutbox(p.s, p.bob, count);
  EXPECT_EQ(count, 1);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].idx, 0);
  EXPECT_EQ(rows[0].ts, ts);
  EXPECT_EQ(rows[0].app_ack, ack1);
  EXPECT_EQ(rows[0].status, rdm::UserStatus::QUEUED);
  EXPECT_EQ(memcmp(rows[0].pub_prefix, p.alice.identity().pub_key, 6), 0);
}

TEST(RdmCompanion, ListOfAnEmptyOutboxIsStartAndEnd) {
  Pair p(606);
  p.bob.goRaw(p.s);
  uint8_t cmd[1];
  std::vector<Frame> frames = p.bob.exchange(p.s, Frame(cmd, cmd + buildListOutbox(cmd)),
                                             [](const std::vector<Frame>& g) { return endsWith(g, RESP_CODE_RDM_OUTBOX_END); });
  EXPECT_EQ(frames, (std::vector<Frame>{{RESP_CODE_RDM_OUTBOX_START, 0}, {RESP_CODE_RDM_OUTBOX_END}}));
}

// More entries than the ESP32 BLE send queue holds: every row arrives, in order, and the list ends with END.
TEST(RdmCompanion, ListStreamsEveryEntry) {
  Pair p(612);
  p.s.unlink(p.bob, p.alice);
  const int n = 6;
  for (int i = 0; i < n; i++) p.bob.app().send_text(p.alice, "queued #" + std::to_string(i));
  ASSERT_TRUE(p.s.run_until([&] { return p.bob.app().idle(); }, 20000));
  p.bob.goRaw(p.s);
  uint8_t count;
  std::vector<OutboxRow> rows = listOutbox(p.s, p.bob, count);
  EXPECT_EQ(count, n);
  ASSERT_EQ(rows.size(), (size_t)n);
  for (int i = 0; i < n; i++) EXPECT_EQ(rows[i].idx, i);
}

TEST(RdmCompanion, EnableRejectsAnUnknownVersion) {
  Pair p(607);
  EXPECT_EQ(command(p.s, p.bob, Frame{CMD_RDM_ENABLE, 2}), (Frame{RESP_CODE_ERR, ERR_CODE_ILLEGAL_ARG}));
  EXPECT_EQ(command(p.s, p.bob, Frame{CMD_RDM_ENABLE}), (Frame{RESP_CODE_ERR, ERR_CODE_ILLEGAL_ARG}));
}

TEST(RdmCompanion, SetMailboxTakesKeysOrNothing) {
  Pair p(608);
  uint8_t mbx[32], k[16], f[SET_MAILBOX_LEN];
  for (int i = 0; i < 32; i++) mbx[i] = (uint8_t)(0x30 + i);
  for (int i = 0; i < 16; i++) k[i] = (uint8_t)i;
  EXPECT_EQ(command(p.s, p.alice, Frame(f, f + buildSetMailbox(f, mbx, k))), Frame{RESP_CODE_OK});
  EXPECT_EQ(command(p.s, p.alice, Frame(f, f + buildSetMailbox(f, nullptr, nullptr))), Frame{RESP_CODE_OK});
  EXPECT_EQ(command(p.s, p.alice, Frame(f, f + 10)), (Frame{RESP_CODE_ERR, ERR_CODE_ILLEGAL_ARG}));
}

// G6: the clock is trusted once the app set it since boot, not after bootstrapRTCfromContacts().
TEST(RdmCompanion, ClockIsTrustedOnlyAfterTheAppSetsIt) {
  Pair p(609);
  EXPECT_TRUE(p.bob.firmware().rtcTrusted());   // introduce() connected the app, which sends CMD_SET_DEVICE_TIME
  p.bob.power_off();
  p.bob.power_on();
  EXPECT_FALSE(p.bob.firmware().rtcTrusted());
  p.bob.app().connect();
  ASSERT_TRUE(p.s.run_until([&] { return p.bob.app().idle(); }, 5000));
  EXPECT_TRUE(p.bob.firmware().rtcTrusted());
  p.bob.firmware().setHardwareRTC(true);
  p.bob.power_off();
  p.bob.power_on();
  EXPECT_FALSE(p.bob.firmware().rtcTrusted()) << "the hardware flag is per boot, set by the board";
}

// G5: statuses that happen while the app is away: SEND_CONFIRMED at the next app start, 0x91 only after CMD_RDM_ENABLE.
TEST(RdmCompanion, StatusWhileAwayIsReplayedOnReconnect) {
  Pair p(610);
  enable(p.s, p.bob);
  int id = p.bob.app().send_text(p.alice, "while away");
  ASSERT_TRUE(p.s.run_until([&] { return p.bob.app().sent(id).answered; }, 5000));
  uint32_t app_ack = p.bob.app().sent(id).expected_ack;
  p.bob.app().disconnect();
  ASSERT_TRUE(p.s.run_until([&] { return p.alice.app().inbox_count("while away") == 1; }, 60000));
  p.s.run_for(30000);   // ACK_S after Alice's sync

  size_t from = p.bob.app().push_log().size();
  p.bob.app().connect();
  ASSERT_TRUE(p.s.run_until([&] { return p.bob.app().idle(); }, 5000));
  p.s.run_for(2000);
  size_t confirms = 0, status = 0;
  for (size_t i = from; i < p.bob.app().push_log().size(); i++) {
    const Frame& f = p.bob.app().push_log()[i];
    confirms += f[0] == PUSH_CODE_SEND_CONFIRMED && rdm::get32(&f[1]) == app_ack;
    status += f[0] == PUSH_CODE_RDM_STATUS;
  }
  EXPECT_EQ(confirms, 1u);
  EXPECT_EQ(status, 0u) << "no 0x91 before CMD_RDM_ENABLE on this connection";

  enable(p.s, p.bob);
  ASSERT_TRUE(p.s.run_until([&] { return !statusesFor(p.bob.app(), app_ack, from).empty(); }, 5000));
  std::vector<rdm::UserStatus> expect = {rdm::UserStatus::DELIVERED};   // only the current state is replayed
  EXPECT_EQ(statusesFor(p.bob.app(), app_ack, from), expect);
}

// A text too long for the cap trailer goes out the upstream way; its TOO_BIG status carries the ack of RESP_CODE_SENT.
TEST(RdmCompanion, TooLongTextIsSentUpstreamWithTooBigStatus) {
  Pair p(611);
  enable(p.s, p.bob);
  std::string text(rdm::MAX_TEXT + 1, 'z');
  int id = p.bob.app().send_text(p.alice, text);
  ASSERT_TRUE(p.s.run_until([&] { return p.bob.app().sent(id).status == Status::DELIVERED; }, 30000));
  uint32_t app_ack = p.bob.app().sent(id).expected_ack;
  auto st = statusesFor(p.bob.app(), app_ack);
  ASSERT_EQ(st.size(), 1u);
  EXPECT_EQ(st[0], rdm::UserStatus::TOO_BIG);
  EXPECT_EQ(p.alice.app().inbox_count(text), 1u);
}

// ---- D57: the app_ack in a TOO_BIG / NO_OUTBOX status is the ack RESP_CODE_SENT carries ----

namespace {

constexpr uint8_t MAX_TEXT_LEN_UPSTREAM = 160;   // MAX_TEXT_LEN (BaseChatMesh.h)

struct RawSend {
  Frame reply;
  std::vector<StatusPush> pushes;   // 0x91 frames for this message, in order
};

// Sends CMD_SEND_TXT_MSG on the link and collects the reply plus the status pushes for that timestamp.
RawSend rawSend(Simulator& s, RawCompanion& n, const SimNode& to, uint32_t ts, uint8_t attempt, const std::string& text) {
  RawSend out;
  bool got = false;
  size_t from = n.app().push_log().size();
  n.app().send_command(buildSendTxt(to.identity().pub_key, ts, attempt, text), [&](const Frame& r) {
    out.reply = r;
    got = true;
  });
  EXPECT_TRUE(s.run_until([&] { return got; }, 5000));
  s.run_for(1000);
  for (size_t i = from; i < n.app().push_log().size(); i++) {
    const Frame& f = n.app().push_log()[i];
    StatusPush sp;
    if (decodeStatus(f.data(), f.size(), sp) && sp.ts == ts && memcmp(sp.pub_prefix, to.identity().pub_key, 6) == 0)
      out.pushes.push_back(sp);
  }
  return out;
}

}

// Text lengths around the fork limit (MAX_TEXT = 157) up to what the frame allows (163): the outbox keeps 156 and 157
// (QUEUED), 158-160 go out the upstream way with TOO_BIG, above 160 upstream refuses the message. Whatever the
// status, its app_ack is the expected_ack of RESP_CODE_SENT: both are SHA256(ts | attempt & 3 | text | self_pub).
TEST(RdmCompanion, TooBigStatusCarriesTheAckOfRespCodeSent) {
  Pair p(613);
  enable(p.s, p.bob);
  p.s.unlink(p.bob, p.alice);
  uint32_t ts = p.s.wall_epoch();
  for (size_t len = rdm::MAX_TEXT - 1; len <= MAX_FRAME_SIZE - 13; len++) {
    SCOPED_TRACE("text length " + std::to_string(len));
    ts++;
    RawSend r = rawSend(p.s, p.bob, p.alice, ts, 0, std::string(len, 'a' + (char)(len % 26)));
    ASSERT_EQ(r.pushes.size(), 1u);
    if (len <= rdm::MAX_TEXT) {
      EXPECT_EQ(r.pushes[0].status, rdm::UserStatus::QUEUED);
    } else {
      EXPECT_EQ(r.pushes[0].status, rdm::UserStatus::TOO_BIG);
    }
    if (len <= MAX_TEXT_LEN_UPSTREAM) {
      SentReply sent;
      ASSERT_TRUE(decodeSent(r.reply, sent)) << "reply code " << (int)r.reply[0];
      EXPECT_EQ(r.pushes[0].app_ack, sent.expected_ack);
    } else {
      EXPECT_EQ(r.reply[0], RESP_CODE_ERR) << "upstream refuses a text above MAX_TEXT_LEN";
      EXPECT_NE(r.pushes[0].app_ack, 0u);
    }
  }
  // attempt above 3: upstream keeps only the low two bits for the ack, and refuses a text above MAX_TEXT_LEN - 2
  ts++;
  RawSend r = rawSend(p.s, p.bob, p.alice, ts, 5, std::string(MAX_TEXT_LEN_UPSTREAM - 2, 'q'));
  ASSERT_EQ(r.pushes.size(), 1u);
  EXPECT_EQ(r.pushes[0].status, rdm::UserStatus::TOO_BIG);
  SentReply sent;
  ASSERT_TRUE(decodeSent(r.reply, sent));
  EXPECT_EQ(r.pushes[0].app_ack, sent.expected_ack);
  ts++;
  r = rawSend(p.s, p.bob, p.alice, ts, 5, std::string(MAX_TEXT_LEN_UPSTREAM - 1, 'q'));
  ASSERT_EQ(r.pushes.size(), 1u);
  EXPECT_EQ(r.pushes[0].status, rdm::UserStatus::TOO_BIG);
  EXPECT_EQ(r.reply[0], RESP_CODE_ERR);
}

TEST(RdmCompanion, NoOutboxStatusCarriesTheAckOfRespCodeSent) {
  Pair p(614);
  enable(p.s, p.bob);
  p.s.unlink(p.bob, p.alice);
  std::vector<int> ids;
  for (int i = 0; i < RDM_OUTBOX_SLOTS_MAX; i++) ids.push_back(p.bob.app().send_text(p.alice, "fill #" + std::to_string(i)));
  ASSERT_TRUE(p.s.run_until([&] { return p.bob.app().idle(); }, 60000));
  p.s.run_for(2000);   // the QUEUED pushes leave the frame queue from the main loop
  for (int id : ids) ASSERT_EQ(statusesFor(p.bob.app(), p.bob.app().sent(id).expected_ack), std::vector<rdm::UserStatus>{rdm::UserStatus::QUEUED});

  uint32_t ts = p.s.wall_epoch() + 100;
  RawSend r = rawSend(p.s, p.bob, p.alice, ts, 0, "one too many");
  ASSERT_EQ(r.pushes.size(), 1u);
  EXPECT_EQ(r.pushes[0].status, rdm::UserStatus::NO_OUTBOX);
  SentReply sent;
  ASSERT_TRUE(decodeSent(r.reply, sent));
  EXPECT_EQ(r.pushes[0].app_ack, sent.expected_ack);
}

#ifdef RDM_STATUS_CHANNEL
// ---- local status channel "rdm-status" for the standard app (env native_rdm_status) ----

namespace {

bool hasReply(const std::vector<Frame>& g) {
  for (const Frame& f : g)
    if (f[0] < 0x80) return true;
  return false;
}

Frame reply(const std::vector<Frame>& g) {
  for (const Frame& f : g)
    if (f[0] < 0x80) return f;
  return Frame();
}

int channelIndex(Simulator& s, RawCompanion& n, const char* name) {
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    Frame r = reply(n.exchange(s, Frame{CMD_GET_CHANNEL, (uint8_t)i}, hasReply));
    if (r.size() >= 34 && r[0] == RESP_CODE_CHANNEL_INFO && strncmp((const char*)&r[2], name, 32) == 0) return i;
  }
  return -1;
}

// Drains the offline queue over the raw link and returns the texts of the channel messages on `channel_idx`.
std::vector<std::string> channelLines(Simulator& s, RawCompanion& n, int channel_idx) {
  std::vector<std::string> lines;
  for (int guard = 0; guard < 300; guard++) {
    Frame r = reply(n.exchange(s, Frame{CMD_SYNC_NEXT_MESSAGE}, hasReply));
    if (r.empty() || r[0] == RESP_CODE_NO_MORE_MESSAGES) break;
    if (r[0] == RESP_CODE_CHANNEL_MSG_RECV_V3 && r.size() >= 11 && r[4] == channel_idx) {
      lines.emplace_back(r.begin() + 11, r.end());
    } else if (r[0] == RESP_CODE_CHANNEL_MSG_RECV && r.size() >= 8 && r[1] == channel_idx) {
      lines.emplace_back(r.begin() + 8, r.end());
    }
  }
  return lines;
}

size_t countContaining(const std::vector<std::string>& lines, const std::string& part) {
  size_t n = 0;
  for (const std::string& l : lines) n += l.find(part) != std::string::npos;
  return n;
}

}

// The standard app sees the delivery states as lines in channel "rdm-status": no QUEUED line, and the 0x91 replay
// after CMD_RDM_ENABLE does not post the same state a second time.
TEST(RdmStatusChannel, OnRadioAndDeliveredOnceNoQueuedNoReplayDuplicate) {
  Simulator s(620);
  auto& bob = s.add<RawCompanion>("bob");
  auto& alice = s.add<RawCompanion>("alice");
  s.link(bob, alice);
  bob.app().auto_sync = false;   // the emulated app skips channel messages; this test reads them itself
  introduce(s, alice, bob);

  int id = bob.app().send_text(alice, "status line please");
  ASSERT_TRUE(s.run_until([&] { return bob.app().sent(id).answered; }, 5000));
  uint32_t app_ack = bob.app().sent(id).expected_ack;
  ASSERT_TRUE(s.run_until([&] { return alice.app().inbox_count("status line please") == 1; }, 60000));
  s.run_for(30000);   // ACK_S after Alice's sync

  enable(s, bob);   // replays the unreported DELIVERED as 0x91
  ASSERT_TRUE(s.run_until(
      [&] {
        auto st = statusesFor(bob.app(), app_ack);
        return !st.empty() && st.back() == rdm::UserStatus::DELIVERED;
      },
      5000));
  s.run_for(2000);

  bob.goRaw(s);
  int ch = channelIndex(s, bob, STATUS_CHANNEL_NAME);
  ASSERT_GE(ch, 0) << "channel rdm-status exists";
  std::vector<std::string> lines = channelLines(s, bob, ch);
  EXPECT_EQ(countContaining(lines, ": on radio (sent"), 1u);
  EXPECT_EQ(countContaining(lines, ": delivered (sent"), 1u);
  EXPECT_EQ(countContaining(lines, ": queued"), 0u);
  EXPECT_EQ(lines.size(), 2u);
}
#endif

// ---- K3: a mailbox that became a contact through auto-add leaves the contact list once it is a hidden peer ----

namespace {

// CMD_ADD_UPDATE_CONTACT as the app sends it when the user marks a favourite (MBX_INFO only goes to favourites).
void markFavourite(Simulator& s, CompanionNode& n, const SimNode& other) {
  Frame f(1 + 32 + 3 + MAX_PATH_SIZE + 32 + 4, 0);
  f[0] = CMD_ADD_UPDATE_CONTACT;
  memcpy(&f[1], other.identity().pub_key, 32);
  f[33] = ADV_TYPE_CHAT;
  f[34] = 0x01;
  f[35] = OUT_PATH_UNKNOWN;
  if (ContactInfo* c = n.firmware().lookupContactByPubKey(other.identity().pub_key, PUB_KEY_SIZE)) {
    f[35] = c->out_path_len;
    memcpy(&f[36], c->out_path, MAX_PATH_SIZE);
  }
  strncpy((char*)&f[36 + MAX_PATH_SIZE], other.name().c_str(), 31);
  EXPECT_EQ(command(s, n, f), Frame{RESP_CODE_OK});
}

bool isContact(CompanionNode& n, const SimNode& other) {
  return n.firmware().lookupContactByPubKey(other.identity().pub_key, PUB_KEY_SIZE) != nullptr;
}

size_t pushesWithKey(const CompanionApp& app, uint8_t code, const uint8_t pub[32], size_t from = 0) {
  size_t n = 0;
  for (size_t i = from; i < app.push_log().size(); i++) {
    const Frame& f = app.push_log()[i];
    n += f.size() >= 1 + 32 && f[0] == code && memcmp(&f[1], pub, 32) == 0;
  }
  return n;
}

bool fileContains(SimNode& n, const char* path, const uint8_t* bytes, size_t len) {
  auto it = n.fs().files.find(path);
  if (it == n.fs().files.end()) return false;
  const std::vector<uint8_t>& d = it->second;
  return std::search(d.begin(), d.end(), bytes, bytes + len) != d.end();
}

}

TEST(RdmCompanion, MailboxLeavesTheContactsWhenItBecomesAHiddenPeer) {
  Simulator s(630);
  auto& bob = s.add<RawCompanion>("bob");
  auto& alice = s.add<RawCompanion>("alice");
  auto& mbx = s.add<RdmSimMailbox>("mailbox");
  s.link(bob, alice);
  s.link(bob, mbx);
  s.link(alice, mbx);
  introduce(s, alice, bob);
  markFavourite(s, alice, bob);
  markFavourite(s, bob, alice);

  // Bob hears M before any MBX_INFO: standard auto-add makes it a contact, persisted like any other.
  mbx.advert(true);
  ASSERT_TRUE(s.run_until([&] { return isContact(bob, mbx); }, 60000)) << "auto-add of the mailbox advert";
  s.run_for(10000);   // lazy contacts write
  const uint8_t* m_pub = mbx.identity().pub_key;
  ASSERT_TRUE(fileContains(bob, "/contacts3", m_pub, 32));

  // Warm-up DM first: the paths (and their lazy contacts write) are settled, so only the hook can save the contacts
  // after MBX_INFO.
  int w = bob.app().send_text(alice, "warm-up");
  ASSERT_TRUE(s.run_until([&] { return bob.app().sent(w).status == Status::DELIVERED; }, 60000));
  s.run_for(30000);

  uint8_t k_owner[16];
  for (int i = 0; i < 16; i++) k_owner[i] = (uint8_t)(0x60 + i);
  ASSERT_TRUE(mbx.backend().addOwner(alice.identity().pub_key, k_owner));
  uint8_t f[SET_MAILBOX_LEN];
  ASSERT_EQ(command(s, alice, Frame(f, f + buildSetMailbox(f, m_pub, k_owner))), Frame{RESP_CODE_OK});

  // Bob's fork DM makes Alice send MBX_INFO to her favourite; once Bob takes it, M is a hidden peer.
  size_t from = bob.app().push_log().size();
  bob.app().send_text(alice, "hello, what is your mailbox?");
  ASSERT_TRUE(s.run_until([&] { return bob.firmware().rdm().hiddenPeerCount() > 0; }, 10 * 60000)) << "no MBX_INFO at Bob";
  ASSERT_EQ(memcmp(bob.firmware().rdm().hiddenPeerPub(0), m_pub, 32), 0);
  ASSERT_TRUE(s.run_until([&] { return !isContact(bob, mbx); }, 5000)) << "M still a contact after MBX_INFO";
  s.run_for(10000);   // lazy contacts write
  EXPECT_EQ(pushesWithKey(bob.app(), PUSH_CODE_CONTACT_DELETED, m_pub, from), 1u);
  EXPECT_FALSE(fileContains(bob, "/contacts3", m_pub, 32)) << "M left the contacts file";

  // A later advert of M reaches only the RDM triggers: no contact, no new-advert push.
  from = bob.app().push_log().size();
  mbx.advert(true);
  s.run_for(60000);
  EXPECT_FALSE(isContact(bob, mbx));
  EXPECT_EQ(pushesWithKey(bob.app(), PUSH_CODE_NEW_ADVERT, m_pub, from), 0u);
  EXPECT_EQ(pushesWithKey(bob.app(), PUSH_CODE_CONTACT_DELETED, m_pub, from), 0u);
}
