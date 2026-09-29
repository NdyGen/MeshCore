#include <gtest/gtest.h>

#include <helpers/rdm/RdmStorage.h>

#include <MemFileIO.h>

#include <vector>

using namespace rdm;
using Open = RecordFile::Open;

namespace {

const char* PATH = "/rdm/test";
const uint16_t PS = 20;

std::vector<uint8_t> val(uint8_t seed, uint16_t size = PS) {
  std::vector<uint8_t> v(size);
  for (uint16_t i = 0; i < size; i++) v[i] = (uint8_t)(seed * 31 + i);
  return v;
}

bool readEq(const RecordFile& f, uint16_t slot, const std::vector<uint8_t>& expect) {
  std::vector<uint8_t> buf(f.payloadSize());
  return f.read(slot, buf.data()) && buf == expect;
}

}

TEST(RdmStorage, FileSize) {
  EXPECT_EQ(RecordFile::fileSize(52, 64, true), 16u + 64u * 2 * (8 + 52));
  EXPECT_EQ(RecordFile::fileSize(185, 256, false), 16u + 256u * (8 + 185));
  EXPECT_EQ(RecordFile::fileSize(12, 1, true), 16u + 2 * 20);
}

TEST(RdmStorage, CreateOpenReopen) {
  MemFileIO io;
  {
    RecordFile f(io, PATH, 1, PS, 4, true);
    EXPECT_EQ(f.open(), Open::CREATED);
    EXPECT_EQ(io.size(PATH), (int32_t)RecordFile::fileSize(PS, 4, true));
    EXPECT_EQ(f.slots(), 4);
    EXPECT_EQ(f.payloadSize(), PS);
    uint8_t buf[PS];
    for (uint16_t s = 0; s < 4; s++) EXPECT_FALSE(f.read(s, buf)) << "new file is empty";
    ASSERT_TRUE(f.write(2, val(7).data()));
    EXPECT_TRUE(readEq(f, 2, val(7)));
    EXPECT_FALSE(f.read(4, buf)) << "slot out of range";
    EXPECT_FALSE(f.write(4, val(1).data()));
  }
  RecordFile again(io, PATH, 1, PS, 4, true);
  EXPECT_EQ(again.open(), Open::OPENED);
  EXPECT_TRUE(readEq(again, 2, val(7)));
  uint8_t buf[PS];
  EXPECT_FALSE(again.read(1, buf));
}

TEST(RdmStorage, HeaderLayout) {
  MemFileIO io;
  RecordFile f(io, PATH, 3, 0x0102, 0x0304, true);
  ASSERT_EQ(f.open(), Open::CREATED);
  const std::vector<uint8_t>& b = io.files[PATH];
  const uint8_t expect[16] = {'R', 'D', 'M', 'F', 3, 1, 0x02, 0x01, 0x04, 0x03, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(0, memcmp(b.data(), expect, 16));
}

TEST(RdmStorage, AlternatingWritesKeepLatest) {
  MemFileIO io;
  RecordFile f(io, PATH, 1, PS, 2, true);
  ASSERT_EQ(f.open(), Open::CREATED);
  for (uint8_t i = 1; i <= 9; i++) {
    ASSERT_TRUE(f.write(0, val(i).data()));
    EXPECT_TRUE(readEq(f, 0, val(i)));
    RecordFile r(io, PATH, 1, PS, 2, true);
    ASSERT_EQ(r.open(), Open::OPENED);
    EXPECT_TRUE(readEq(r, 0, val(i))) << "after reopen, write " << (int)i;
  }
}

TEST(RdmStorage, EraseEmptiesSlotAndPersists) {
  for (bool ab : {true, false}) {
    MemFileIO io;
    RecordFile f(io, PATH, 1, PS, 3, ab);
    ASSERT_EQ(f.open(), Open::CREATED);
    ASSERT_TRUE(f.write(1, val(1).data()));
    ASSERT_TRUE(f.write(1, val(2).data()));
    ASSERT_TRUE(f.erase(1));
    uint8_t buf[PS];
    EXPECT_FALSE(f.read(1, buf)) << "ab=" << ab;
    RecordFile r(io, PATH, 1, PS, 3, ab);
    ASSERT_EQ(r.open(), Open::OPENED);
    EXPECT_FALSE(r.read(1, buf)) << "an erased slot never falls back to an older copy";
    ASSERT_TRUE(r.write(1, val(3).data()));
    EXPECT_TRUE(readEq(r, 1, val(3)));
  }
}

// Power loss at every byte of a write (and of an erase): the previous value stays readable, the file stays usable.
TEST(RdmStorage, TornWriteAtEveryByteKeepsPreviousValue) {
  for (int prior_writes : {1, 2, 3}) {
    MemFileIO ref;
    {
      RecordFile f(ref, PATH, 1, PS, 3, true);
      ASSERT_EQ(f.open(), Open::CREATED);
      for (int i = 1; i <= prior_writes; i++) ASSERT_TRUE(f.write(1, val((uint8_t)i).data()));
      ASSERT_TRUE(f.write(0, val(50).data()));
      ASSERT_TRUE(f.write(2, val(60).data()));
    }
    const std::vector<uint8_t> old = val((uint8_t)prior_writes), next = val(99);

    uint64_t before = ref.bytesWritten();
    {
      MemFileIO probe = ref;
      RecordFile f(probe, PATH, 1, PS, 3, true);
      ASSERT_EQ(f.open(), Open::OPENED);
      before = probe.bytesWritten();
      ASSERT_TRUE(f.write(1, next.data()));
      ASSERT_EQ(probe.bytesWritten() - before, 8u + PS) << "one copy per write";
    }

    for (long k = 0; k < 8 + PS; k++) {
      MemFileIO io = ref;
      RecordFile f(io, PATH, 1, PS, 3, true);
      ASSERT_EQ(f.open(), Open::OPENED);
      io.failWritesAfter(k);
      EXPECT_FALSE(f.write(1, next.data())) << "k=" << k;
      io.clearFaults();

      RecordFile r(io, PATH, 1, PS, 3, true);
      ASSERT_EQ(r.open(), Open::OPENED);
      EXPECT_TRUE(readEq(r, 1, old)) << "prior=" << prior_writes << " k=" << k;
      EXPECT_TRUE(readEq(r, 0, val(50)));
      EXPECT_TRUE(readEq(r, 2, val(60)));
      ASSERT_TRUE(r.write(1, next.data())) << "the torn copy is the next write target";
      EXPECT_TRUE(readEq(r, 1, next));
      RecordFile r2(io, PATH, 1, PS, 3, true);
      ASSERT_EQ(r2.open(), Open::OPENED);
      EXPECT_TRUE(readEq(r2, 1, next));
    }

    for (long k = 0; k < 8 + PS; k++) {
      MemFileIO io = ref;
      RecordFile f(io, PATH, 1, PS, 3, true);
      ASSERT_EQ(f.open(), Open::OPENED);
      io.failWritesAfter(k);
      EXPECT_FALSE(f.erase(1));
      io.clearFaults();
      RecordFile r(io, PATH, 1, PS, 3, true);
      ASSERT_EQ(r.open(), Open::OPENED);
      EXPECT_TRUE(readEq(r, 1, old)) << "torn erase, k=" << k;
    }
  }
}

// Without A/B a torn write may lose that record, but never yields a mix and never touches other slots.
TEST(RdmStorage, TornWriteSingleCopyLosesAtMostThatRecord) {
  MemFileIO ref;
  {
    RecordFile f(ref, PATH, 1, PS, 3, false);
    ASSERT_EQ(f.open(), Open::CREATED);
    for (uint16_t s = 0; s < 3; s++) ASSERT_TRUE(f.write(s, val((uint8_t)(s + 1)).data()));
  }
  for (long k = 0; k < 8 + PS; k++) {
    MemFileIO io = ref;
    RecordFile f(io, PATH, 1, PS, 3, false);
    ASSERT_EQ(f.open(), Open::OPENED);
    io.failWritesAfter(k);
    EXPECT_FALSE(f.write(1, val(77).data()));
    io.clearFaults();
    RecordFile r(io, PATH, 1, PS, 3, false);
    ASSERT_EQ(r.open(), Open::OPENED);
    std::vector<uint8_t> buf(PS);
    if (r.read(1, buf.data())) {
      EXPECT_TRUE(buf == val(2) || buf == val(77)) << "k=" << k;
    }
    EXPECT_TRUE(readEq(r, 0, val(1)));
    EXPECT_TRUE(readEq(r, 2, val(3)));
  }
}

TEST(RdmStorage, CrcErrorInOneRecordLeavesTheRest) {
  for (bool ab : {false, true}) {
    MemFileIO io;
    RecordFile f(io, PATH, 1, PS, 4, ab);
    ASSERT_EQ(f.open(), Open::CREATED);
    for (uint16_t s = 0; s < 4; s++) ASSERT_TRUE(f.write(s, val((uint8_t)(s + 10)).data()));
    uint32_t copy = 8 + PS;
    uint32_t slot2 = 16 + 2 * copy * (ab ? 2 : 1);
    io.corrupt(PATH, slot2 + 8 + 5, 0x40);
    if (ab) io.corrupt(PATH, slot2 + copy + 8 + 5, 0x40);   // both copies

    RecordFile r(io, PATH, 1, PS, 4, ab);
    ASSERT_EQ(r.open(), Open::OPENED) << "a bad record does not recreate the file";
    uint8_t buf[PS];
    EXPECT_FALSE(r.read(2, buf)) << "ab=" << ab;
    EXPECT_TRUE(readEq(r, 0, val(10)));
    EXPECT_TRUE(readEq(r, 1, val(11)));
    EXPECT_TRUE(readEq(r, 3, val(13)));
    ASSERT_TRUE(r.write(2, val(12).data()));
    EXPECT_TRUE(readEq(r, 2, val(12)));
  }
}

TEST(RdmStorage, CorruptNewestCopyFallsBackToPrevious) {
  MemFileIO io;
  RecordFile f(io, PATH, 1, PS, 1, true);
  ASSERT_EQ(f.open(), Open::CREATED);
  ASSERT_TRUE(f.write(0, val(1).data()));   // copy 0
  ASSERT_TRUE(f.write(0, val(2).data()));   // copy 1
  io.corrupt(PATH, 16 + (8 + PS) + 8, 0x01);
  EXPECT_TRUE(readEq(f, 0, val(1)));
}

TEST(RdmStorage, BadHeaderOrSizeRecreates) {
  MemFileIO io;
  {
    RecordFile f(io, PATH, 1, PS, 2, true);
    ASSERT_EQ(f.open(), Open::CREATED);
    ASSERT_TRUE(f.write(0, val(1).data()));
  }
  io.corrupt(PATH, 0, 0xFF);
  RecordFile f(io, PATH, 1, PS, 2, true);
  EXPECT_EQ(f.open(), Open::CREATED);
  uint8_t buf[PS];
  EXPECT_FALSE(f.read(0, buf));

  ASSERT_TRUE(f.write(0, val(1).data()));
  io.files[PATH].resize(io.files[PATH].size() - 1);
  RecordFile g(io, PATH, 1, PS, 2, true);
  EXPECT_EQ(g.open(), Open::CREATED) << "truncated file";
  EXPECT_EQ(io.size(PATH), (int32_t)RecordFile::fileSize(PS, 2, true));
}

TEST(RdmStorage, CreateFailureIsFailed) {
  MemFileIO io;
  io.failNextCreate();
  RecordFile f(io, PATH, 1, PS, 2, true);
  EXPECT_EQ(f.open(), Open::FAILED);
  uint8_t buf[PS] = {};
  EXPECT_FALSE(f.write(0, buf));
  EXPECT_FALSE(f.read(0, buf));

  MemFileIO small(RecordFile::fileSize(PS, 2, true) - 1);
  RecordFile g(small, PATH, 1, PS, 2, true);
  EXPECT_EQ(g.open(), Open::FAILED) << "no room";
}

TEST(RdmStorage, OtherVersionWithoutMigrateIsCreated) {
  MemFileIO io;
  {
    RecordFile f(io, PATH, 1, PS, 2, true);
    ASSERT_EQ(f.open(), Open::CREATED);
    ASSERT_TRUE(f.write(0, val(1).data()));
  }
  RecordFile f(io, PATH, 2, PS, 2, true);
  EXPECT_EQ(f.open(), Open::CREATED);
  uint8_t buf[PS];
  EXPECT_FALSE(f.read(0, buf));
}

TEST(RdmStorage, OtherPayloadSizeWithoutMigrateIsCreated) {
  MemFileIO io;
  {
    RecordFile f(io, PATH, 1, PS, 2, true);
    ASSERT_EQ(f.open(), Open::CREATED);
    ASSERT_TRUE(f.write(0, val(1).data()));
  }
  RecordFile f(io, PATH, 1, PS + 4, 2, true);
  EXPECT_EQ(f.open(), Open::CREATED);
}

// Slot counts follow the free flash at boot; a different count must not throw records away (G7).
TEST(RdmStorage, SlotCountChangeKeepsRecords) {
  MemFileIO io;
  {
    RecordFile f(io, PATH, 1, PS, 4, true);
    ASSERT_EQ(f.open(), Open::CREATED);
    ASSERT_TRUE(f.write(0, val(1).data()));
    ASSERT_TRUE(f.write(3, val(4).data()));
  }
  {
    RecordFile f(io, PATH, 1, PS, 8, false);
    ASSERT_EQ(f.open(), Open::MIGRATED);
    EXPECT_EQ(io.size(PATH), (int32_t)RecordFile::fileSize(PS, 8, false));
    EXPECT_TRUE(readEq(f, 0, val(1)));
    EXPECT_TRUE(readEq(f, 3, val(4)));
    uint8_t buf[PS];
    EXPECT_FALSE(f.read(1, buf));
    EXPECT_FALSE(f.read(7, buf));
  }
  RecordFile f(io, PATH, 1, PS, 2, true);
  ASSERT_EQ(f.open(), Open::MIGRATED);
  EXPECT_TRUE(readEq(f, 0, val(1))) << "shrinking keeps the slots that still exist";
  EXPECT_EQ(io.files.size(), 1u) << "no temporary file left";
}

namespace {

// v1: counter(4) | name(16). v2 inserts a flags byte and grows the name to 20 bytes.
bool migrateV1toV2(uint8_t from_ver, const uint8_t* in, uint16_t in_size, uint8_t* out) {
  if (from_ver != 1 || in_size != 20) return false;
  if (in[0] == 0xEE) return false;   // unconvertible record: dropped
  memcpy(out, in, 4);
  out[4] = 0x5A;
  memset(&out[5], 0, 20);
  memcpy(&out[5], &in[4], 16);
  return true;
}

std::vector<uint8_t> v2of(const std::vector<uint8_t>& v1) {
  std::vector<uint8_t> out(25, 0);
  memcpy(out.data(), v1.data(), 4);
  out[4] = 0x5A;
  memcpy(&out[5], &v1[4], 16);
  return out;
}

MemFileIO makeV1() {
  MemFileIO io;
  RecordFile f(io, PATH, 1, 20, 3, true);
  f.open();
  f.write(0, val(1).data());
  f.write(0, val(2).data());   // latest wins
  f.write(2, val(3).data());
  std::vector<uint8_t> bad = val(4);
  bad[0] = 0xEE;
  f.write(1, bad.data());
  return io;
}

}

TEST(RdmStorage, MigrateV1ToV2) {
  MemFileIO io = makeV1();
  {
    RecordFile f(io, PATH, 2, 25, 3, true);
    ASSERT_EQ(f.open(migrateV1toV2), Open::MIGRATED);
    EXPECT_TRUE(readEq(f, 0, v2of(val(2))));
    EXPECT_TRUE(readEq(f, 2, v2of(val(3))));
    uint8_t buf[25];
    EXPECT_FALSE(f.read(1, buf)) << "record the migration refused is dropped, the rest stays";
    EXPECT_EQ(io.files.size(), 1u);
  }
  RecordFile f(io, PATH, 2, 25, 3, true);
  EXPECT_EQ(f.open(migrateV1toV2), Open::OPENED);
  EXPECT_TRUE(readEq(f, 0, v2of(val(2))));
}

// Power loss at every byte of the migration: the next boot finishes it (or redoes it) without losing records.
TEST(RdmStorage, MigrationSurvivesPowerLossAtEveryByte) {
  uint64_t total;
  {
    MemFileIO io = makeV1();
    uint64_t before = io.bytesWritten();
    RecordFile f(io, PATH, 2, 25, 3, true);
    ASSERT_EQ(f.open(migrateV1toV2), Open::MIGRATED);
    total = io.bytesWritten() - before;
  }
  ASSERT_GT(total, 0u);
  for (long k = 0; k < (long)total; k++) {
    MemFileIO io = makeV1();
    io.failWritesAfter(k);
    {
      RecordFile f(io, PATH, 2, 25, 3, true);
      EXPECT_EQ(f.open(migrateV1toV2), Open::FAILED) << "k=" << k;
    }
    io.clearFaults();
    RecordFile f(io, PATH, 2, 25, 3, true);
    Open o = f.open(migrateV1toV2);
    EXPECT_TRUE(o == Open::MIGRATED || o == Open::OPENED) << "k=" << k;
    EXPECT_TRUE(readEq(f, 0, v2of(val(2)))) << "k=" << k;
    EXPECT_TRUE(readEq(f, 2, v2of(val(3)))) << "k=" << k;
    EXPECT_EQ(io.files.size(), 1u) << "k=" << k;
  }
}

TEST(RdmStorage, MigrationWithoutRoomFailsAndKeepsOriginal) {
  MemFileIO io = makeV1();
  io.setCapacity(io.usedBytes() + 10);
  {
    RecordFile f(io, PATH, 2, 25, 3, true);
    EXPECT_EQ(f.open(migrateV1toV2), Open::FAILED);
  }
  io.setCapacity(1024 * 1024);
  RecordFile old(io, PATH, 1, 20, 3, true);
  ASSERT_EQ(old.open(), Open::OPENED);
  EXPECT_TRUE(readEq(old, 0, val(2)));
}

// A leftover temporary file (its removal failed after a completed migration) must never overwrite newer records.
TEST(RdmStorage, StaleTemporaryFileIsIgnoredOnceMigrationCompleted) {
  MemFileIO io = makeV1();
  {
    RecordFile f(io, PATH, 2, 25, 3, true);
    ASSERT_EQ(f.open(migrateV1toV2), Open::MIGRATED);
    ASSERT_TRUE(f.write(0, std::vector<uint8_t>(25, 0x33).data()));
  }
  const std::string tmp = std::string(PATH) + ".mig";
  {
    MemFileIO again = makeV1();
    RecordFile f(again, PATH, 2, 25, 3, true);
    ASSERT_EQ(f.open(migrateV1toV2), Open::MIGRATED);
    io.files[tmp] = again.files[PATH];   // a complete temporary file holding the older migrated records
  }
  RecordFile f(io, PATH, 2, 25, 3, true);
  EXPECT_EQ(f.open(migrateV1toV2), Open::OPENED);
  EXPECT_TRUE(readEq(f, 0, std::vector<uint8_t>(25, 0x33)));
  EXPECT_FALSE(io.exists(tmp.c_str())) << "leftover removed";
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
