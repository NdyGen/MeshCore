#include <gtest/gtest.h>

#include <helpers/rdm/RdmClock.h>
#include <helpers/rdm/RdmConfig.h>

#include <MemFileIO.h>

#include <memory>

using namespace rdm;

namespace {

const uint32_t RTC_2026 = 1790640000;
const uint32_t PERSIST_MS = RDM_CLOCK_PERSIST_S * 1000;

// One boot of the device: the meta file on shared flash, a fresh Clock in RAM.
struct Boot {
  RecordFile meta;
  Clock clock;
  explicit Boot(MemFileIO& io) : meta(io, "/rdm/meta", 1, 12, 1, true), clock(meta) {}
};

}

TEST(RdmClock, FirstBootStartsAtZeroWithStoreId) {
  MemFileIO io;
  Boot b(io);
  ASSERT_TRUE(b.clock.begin(5000, false, 0xABCD1234));
  EXPECT_EQ(b.clock.storeId(), 0xABCD1234u) << "a new meta file is a new store";
  EXPECT_EQ(b.clock.now(5000, 0, false), 0u);
  EXPECT_EQ(b.clock.now(5999, 0, false), 0u);
  EXPECT_EQ(b.clock.now(6000, 0, false), 1u);
  EXPECT_EQ(b.clock.now(65000, 0, false), 60u);
}

TEST(RdmClock, StoreIdIsNeverZero) {
  MemFileIO io;
  Boot b(io);
  ASSERT_TRUE(b.clock.begin(0, false, 0));
  EXPECT_NE(b.clock.storeId(), 0u);
}

TEST(RdmClock, BeginFailsWithoutStorage) {
  MemFileIO io;
  io.failNextCreate();
  Boot b(io);
  EXPECT_FALSE(b.clock.begin(0, false, 1));
}

TEST(RdmClock, NewStoreIdOnlyOnRecreate) {
  MemFileIO io;
  {
    Boot b(io);
    ASSERT_TRUE(b.clock.begin(0, false, 111));
  }
  {
    Boot b(io);
    ASSERT_TRUE(b.clock.begin(0, false, 222));
    EXPECT_EQ(b.clock.storeId(), 111u) << "plain reboot keeps store_id";
  }
  {
    Boot b(io);
    ASSERT_TRUE(b.clock.begin(0, true, 333));
    EXPECT_EQ(b.clock.storeId(), 333u) << "inbox/register recreated";
  }
  {
    Boot b(io);
    ASSERT_TRUE(b.clock.begin(0, false, 444));
    EXPECT_EQ(b.clock.storeId(), 333u) << "the new store_id is persistent";
  }
  io.wipeAll();
  Boot b(io);
  ASSERT_TRUE(b.clock.begin(0, false, 555));
  EXPECT_EQ(b.clock.storeId(), 555u) << "meta lost: store unknown, so a new one";
}

// D1: without a trusted clock RDM time counts on-time only, and never runs back across reboots.
TEST(RdmClock, MonotonicOverRebootWithoutRtc) {
  MemFileIO io;
  uint32_t last = 0;
  for (int boot = 0; boot < 4; boot++) {
    Boot b(io);
    uint32_t start_ms = 1000 + boot * 777;
    ASSERT_TRUE(b.clock.begin(start_ms, false, 1));
    uint32_t t0 = b.clock.now(start_ms, 0, false);
    EXPECT_GE(t0, last) << "boot " << boot;
    for (uint32_t s = 1; s <= 1800; s++) {
      uint32_t ms = start_ms + s * 1000;
      uint32_t t = b.clock.now(ms, 0, false);
      EXPECT_EQ(t, t0 + s);
      b.clock.maybePersist(ms, false);
    }
    last = b.clock.now(start_ms + 1800 * 1000, 0, false);
    b.clock.maybePersist(start_ms + 1800 * 1000, true);   // state transition just before power loss
  }
  EXPECT_EQ(last, 4u * 1800) << "on-time only, nothing lost with a forced persist";
}

TEST(RdmClock, UnpersistedTimeIsLostButNeverNegative) {
  MemFileIO io;
  {
    Boot b(io);
    ASSERT_TRUE(b.clock.begin(0, false, 1));
    b.clock.maybePersist(PERSIST_MS, false);          // persists 600
    EXPECT_EQ(b.clock.now(PERSIST_MS + 300000, 0, false), 900u);
  }
  Boot b(io);
  ASSERT_TRUE(b.clock.begin(0, false, 1));
  EXPECT_EQ(b.clock.now(0, 0, false), 600u);
}

TEST(RdmClock, TrustedRtcAheadTakesOverBehindIsIgnored) {
  MemFileIO io;
  Boot b(io);
  ASSERT_TRUE(b.clock.begin(0, false, 1));
  EXPECT_EQ(b.clock.now(10000, RTC_2026, false), 10u) << "untrusted RTC is ignored";
  EXPECT_EQ(b.clock.now(11000, RTC_2026, true), RTC_2026) << "trusted and ahead: takes over";
  EXPECT_EQ(b.clock.now(12000, RTC_2026 - 3600, true), RTC_2026 + 1) << "trusted but behind: ignored";
  EXPECT_EQ(b.clock.now(13000, 0, false), RTC_2026 + 2) << "keeps counting from the RTC time";
  EXPECT_EQ(b.clock.now(14000, RTC_2026 + 100, true), RTC_2026 + 100);
  b.clock.maybePersist(14000, true);

  Boot b2(io);
  ASSERT_TRUE(b2.clock.begin(0, false, 1));
  EXPECT_EQ(b2.clock.now(0, 1715770351, true), RTC_2026 + 100) << "RTC reset to 2024 after reboot is ignored";
}

TEST(RdmClock, SurvivesMillisWrap) {
  MemFileIO io;
  Boot b(io);
  ASSERT_TRUE(b.clock.begin(0xFFFFF000u, false, 1));
  EXPECT_EQ(b.clock.now(0xFFFFFFFFu, 0, false), 4u);
  EXPECT_EQ(b.clock.now(0x00001000u, 0, false), 8u);
  EXPECT_EQ(b.clock.now(0x00002000u, 0, false), 12u);
}

TEST(RdmClock, PersistInterval) {
  MemFileIO io;
  Boot b(io);
  ASSERT_TRUE(b.clock.begin(1234, false, 1));
  EXPECT_EQ(b.clock.nextPersistMillis(), 1234u + PERSIST_MS);

  uint32_t writes = io.writeCalls();
  b.clock.maybePersist(1234 + PERSIST_MS - 1, false);
  EXPECT_EQ(io.writeCalls(), writes) << "one ms early: no write";
  b.clock.maybePersist(1234 + PERSIST_MS, false);
  EXPECT_GT(io.writeCalls(), writes) << "exactly at nextPersistMillis: write";
  EXPECT_EQ(b.clock.nextPersistMillis(), 1234u + 2 * PERSIST_MS);

  writes = io.writeCalls();
  b.clock.maybePersist(1234 + PERSIST_MS + 5, true);
  EXPECT_GT(io.writeCalls(), writes) << "force writes at once";
  EXPECT_EQ(b.clock.nextPersistMillis(), 1234u + PERSIST_MS + 5 + PERSIST_MS);

  Boot b2(io);
  ASSERT_TRUE(b2.clock.begin(0, false, 1));
  EXPECT_EQ(b2.clock.now(0, 0, false), RDM_CLOCK_PERSIST_S) << "the persisted value is the RDM time at that moment";
}

TEST(RdmClock, NextReqTimestampStrictlyIncreasingOverReboot) {
  MemFileIO io;
  uint32_t last = 0;
  {
    Boot b(io);
    ASSERT_TRUE(b.clock.begin(0, false, 1));
    last = b.clock.nextReqTimestamp(RTC_2026);
    EXPECT_EQ(last, RTC_2026);
    for (int i = 0; i < 5; i++) {
      uint32_t t = b.clock.nextReqTimestamp(RTC_2026);   // same second
      EXPECT_EQ(t, last + 1);
      last = t;
    }
    EXPECT_EQ(b.clock.nextReqTimestamp(RTC_2026 + 100), RTC_2026 + 100) << "RTC ahead wins";
    last = RTC_2026 + 100;
  }
  // K2: after a reboot without app the RTC falls back; the mailbox would reject older timestamps.
  Boot b(io);
  ASSERT_TRUE(b.clock.begin(0, false, 1));
  uint32_t t = b.clock.nextReqTimestamp(1715770351);
  EXPECT_EQ(t, last + 1);
}

// H4: the simulator sleeps until millisUntil(); waking late would break schedules, early is allowed.
TEST(RdmClock, MillisUntilIsExactWithoutRtcAndNeverLate) {
  MemFileIO io;
  Boot b(io);
  const uint32_t start = 123457;
  ASSERT_TRUE(b.clock.begin(start, false, 1));
  for (uint32_t m = start; m < start + 3100; m += 7) {
    Clock cur = b.clock;
    uint32_t t = cur.now(m, 0, false);
    for (uint32_t ahead : {0u, 1u, 2u, 5u, 3600u}) {
      uint32_t due = t + ahead;
      uint32_t w = cur.millisUntil(due, m);
      Clock at = cur;
      EXPECT_GE(at.now(m + w, 0, false), due) << "late: m=" << m << " due=+" << ahead;
      if (w > 0) {
        Clock before = cur;
        EXPECT_LT(before.now(m + w - 1, 0, false), due) << "early without RTC: m=" << m << " due=+" << ahead;
      }
    }
    EXPECT_EQ(cur.millisUntil(t, m), 0u) << "already due";
    EXPECT_EQ(cur.millisUntil(t > 0 ? t - 1 : 0, m), 0u);
  }
}

TEST(RdmClock, MillisUntilAfterRtcJumpIsEarlyNotLate) {
  MemFileIO io;
  Boot b(io);
  ASSERT_TRUE(b.clock.begin(0, false, 1));
  uint32_t t = b.clock.now(5500, 0, false);
  uint32_t due = t + 100;
  uint32_t w = b.clock.millisUntil(due, 5500);
  EXPECT_EQ(b.clock.now(6000, RTC_2026, true), RTC_2026) << "app sets the clock";
  EXPECT_GE(b.clock.now(5500 + w, 0, false), due);
  EXPECT_EQ(b.clock.millisUntil(due, 6000), 0u);
}

TEST(RdmClock, MillisUntilFarAndNothingDue) {
  MemFileIO io;
  Boot b(io);
  ASSERT_TRUE(b.clock.begin(0, false, 1));
  uint32_t far = b.clock.millisUntil(RDM_WATCH_S, 0);   // 90 days: more than 32 bits of ms
  EXPECT_GT(far, 0u);
  EXPECT_LE(far, 0x7FFFFFFFu) << "capped so millis arithmetic stays safe; waking early is allowed";
  EXPECT_EQ(b.clock.millisUntil(UINT32_MAX, 0), UINT32_MAX) << "nothing due";
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
