#include <gtest/gtest.h>

#include <helpers/rdm/RdmConfig.h>
#include <helpers/rdm/RdmContacts.h>

#include <MemFileIO.h>

using namespace rdm;

namespace {

const char* PATH = "/rdm/contacts";

struct Boot {
  RecordFile file;
  ContactTable table;
  Boot(MemFileIO& io, uint16_t slots = 4) : file(io, PATH, 1, 52, slots, true), table(file) {}
};

struct Prefix {
  uint8_t b[6];
  explicit Prefix(uint8_t id) { for (int i = 0; i < 6; i++) b[i] = (uint8_t)(id + i * 16); }
};

}

TEST(RdmContacts, BeginFailsWithoutStorage) {
  MemFileIO io;
  io.failNextCreate();
  Boot b(io);
  EXPECT_FALSE(b.table.begin());
}

TEST(RdmContacts, BeginRejectsWrongRecordSize) {
  MemFileIO io;
  RecordFile f(io, PATH, 1, 40, 4, true);
  ContactTable t(f);
  EXPECT_FALSE(t.begin());
}

TEST(RdmContacts, AddFindAndPersist) {
  MemFileIO io;
  {
    Boot b(io);
    ASSERT_TRUE(b.table.begin());
    EXPECT_EQ(b.table.find(Prefix(1).b), nullptr);
    ContactRdm* c = b.table.findOrAdd(Prefix(1).b);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(0, memcmp(c->pub_prefix, Prefix(1).b, 6));
    EXPECT_EQ(c->flags, 0);
    EXPECT_EQ(c->watermark, 0u);
    EXPECT_EQ(b.table.findOrAdd(Prefix(1).b), c) << "existing entry, not a second one";
    EXPECT_EQ(b.table.find(Prefix(1).b), c);

    c->flags = CR_HAS_MBX | CR_MBX_INFO_SENT;
    c->watermark = 0x01020304;
    memset(c->mbx_pub, 0x77, 32);
    memset(c->token, 0x88, 8);
    ASSERT_TRUE(b.table.save(*c));
  }
  Boot b(io);
  ASSERT_TRUE(b.table.begin());
  ContactRdm* c = b.table.find(Prefix(1).b);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->flags, CR_HAS_MBX | CR_MBX_INFO_SENT);
  EXPECT_EQ(c->watermark, 0x01020304u);
  EXPECT_EQ(c->mbx_pub[31], 0x77);
  EXPECT_EQ(c->token[7], 0x88);
}

// 52-byte record written field by field, little-endian (06 par. 3 and 3.7), never a struct memcpy.
TEST(RdmContacts, RecordLayout) {
  MemFileIO io;
  Boot b(io, 1);
  ASSERT_TRUE(b.table.begin());
  ContactRdm c{};
  memcpy(c.pub_prefix, Prefix(9).b, 6);
  c.flags = CR_CAP;
  c.watermark = 0xA1B2C3D4;
  for (int i = 0; i < 32; i++) c.mbx_pub[i] = (uint8_t)i;
  for (int i = 0; i < 8; i++) c.token[i] = (uint8_t)(0xF0 + i);
  ASSERT_TRUE(b.table.save(c));

  uint8_t rec[52];
  ASSERT_TRUE(b.file.read(0, rec));
  EXPECT_EQ(0, memcmp(rec, Prefix(9).b, 6));
  EXPECT_EQ(rec[6], CR_CAP);
  EXPECT_EQ(rec[7], 0);
  const uint8_t wm[4] = {0xD4, 0xC3, 0xB2, 0xA1};
  EXPECT_EQ(0, memcmp(&rec[8], wm, 4));
  EXPECT_EQ(rec[12], 0);
  EXPECT_EQ(rec[43], 31);
  EXPECT_EQ(rec[44], 0xF0);
  EXPECT_EQ(rec[51], 0xF7);
}

TEST(RdmContacts, SaveOfUnknownContactAddsIt) {
  MemFileIO io;
  Boot b(io);
  ASSERT_TRUE(b.table.begin());
  ContactRdm c{};
  memcpy(c.pub_prefix, Prefix(3).b, 6);
  c.flags = CR_CAP;
  ASSERT_TRUE(b.table.save(c));
  ContactRdm* f = b.table.find(Prefix(3).b);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->flags, CR_CAP);
}

TEST(RdmContacts, CapSetAndClear) {
  MemFileIO io;
  {
    Boot b(io);
    ASSERT_TRUE(b.table.begin());
    ContactRdm* c = b.table.findOrAdd(Prefix(2).b);
    ASSERT_NE(c, nullptr);
    c->flags |= CR_CAP;
    ASSERT_TRUE(b.table.save(*c));
  }
  {
    Boot b(io);
    ASSERT_TRUE(b.table.begin());
    ContactRdm* c = b.table.find(Prefix(2).b);
    ASSERT_NE(c, nullptr);
    EXPECT_TRUE(c->flags & CR_CAP);
    c->flags &= (uint8_t)~CR_CAP;   // G15: ACK_R without cap byte
    ASSERT_TRUE(b.table.save(*c));
  }
  Boot b(io);
  ASSERT_TRUE(b.table.begin());
  ContactRdm* c = b.table.find(Prefix(2).b);
  ASSERT_NE(c, nullptr);
  EXPECT_FALSE(c->flags & CR_CAP);
}

// Evict the least recently used entry that carries nothing costly to lose: no mailbox, no watermark.
TEST(RdmContacts, EvictionRule) {
  MemFileIO io;
  Boot b(io, 4);
  ASSERT_TRUE(b.table.begin());
  for (uint8_t i = 1; i <= 4; i++) ASSERT_NE(b.table.findOrAdd(Prefix(i).b), nullptr);

  ContactRdm* mbx = b.table.find(Prefix(1).b);
  mbx->flags |= CR_HAS_MBX;
  ASSERT_TRUE(b.table.save(*mbx));
  ContactRdm* wm = b.table.find(Prefix(2).b);
  wm->watermark = 5;
  ASSERT_TRUE(b.table.save(*wm));
  ASSERT_NE(b.table.find(Prefix(3).b), nullptr);   // 3 used after 4: 4 is now the oldest evictable

  ContactRdm* added = b.table.findOrAdd(Prefix(5).b);
  ASSERT_NE(added, nullptr);
  EXPECT_EQ(b.table.find(Prefix(4).b), nullptr);
  EXPECT_NE(b.table.find(Prefix(1).b), nullptr);
  EXPECT_NE(b.table.find(Prefix(2).b), nullptr);
  EXPECT_NE(b.table.find(Prefix(3).b), nullptr);

  ASSERT_NE(b.table.findOrAdd(Prefix(6).b), nullptr);
  EXPECT_EQ(b.table.find(Prefix(5).b), nullptr) << "5 was the oldest evictable after the finds above";

  ContactRdm* c3 = b.table.find(Prefix(3).b);
  c3->flags |= CR_HAS_MBX;
  ASSERT_TRUE(b.table.save(*c3));
  ContactRdm* c6 = b.table.find(Prefix(6).b);
  c6->watermark = 1;
  ASSERT_TRUE(b.table.save(*c6));
  EXPECT_EQ(b.table.findOrAdd(Prefix(7).b), nullptr) << "nothing evictable";
  ContactRdm c7{};
  memcpy(c7.pub_prefix, Prefix(7).b, 6);
  EXPECT_FALSE(b.table.save(c7));

  Boot again(io, 4);
  ASSERT_TRUE(again.table.begin());
  EXPECT_EQ(again.table.find(Prefix(4).b), nullptr) << "eviction is persistent";
  EXPECT_EQ(again.table.find(Prefix(5).b), nullptr);
  EXPECT_NE(again.table.find(Prefix(6).b), nullptr);
}

TEST(RdmContacts, CapOnlyEntriesAreEvictable) {
  MemFileIO io;
  Boot b(io, 1);
  ASSERT_TRUE(b.table.begin());
  ContactRdm* c = b.table.findOrAdd(Prefix(1).b);
  c->flags = CR_CAP | CR_MBX_INFO_SENT;
  ASSERT_TRUE(b.table.save(*c));
  EXPECT_NE(b.table.findOrAdd(Prefix(2).b), nullptr);
  EXPECT_EQ(b.table.find(Prefix(1).b), nullptr);
}

TEST(RdmContacts, WriteFailureLeavesTableUnchanged) {
  MemFileIO io;
  Boot b(io, 2);
  ASSERT_TRUE(b.table.begin());
  ASSERT_NE(b.table.findOrAdd(Prefix(1).b), nullptr);
  io.failWritesAfter(0);
  EXPECT_EQ(b.table.findOrAdd(Prefix(2).b), nullptr);
  EXPECT_EQ(b.table.find(Prefix(2).b), nullptr);
  io.clearFaults();
  EXPECT_NE(b.table.findOrAdd(Prefix(2).b), nullptr);
}

TEST(RdmContacts, FindByMailbox) {
  MemFileIO io;
  Boot b(io);
  ASSERT_TRUE(b.table.begin());
  ContactRdm* a = b.table.findOrAdd(Prefix(1).b);
  ContactRdm* c = b.table.findOrAdd(Prefix(2).b);
  memset(a->mbx_pub, 0x11, 32);
  a->flags |= CR_HAS_MBX;
  ASSERT_TRUE(b.table.save(*a));
  memset(c->mbx_pub, 0x22, 32);   // stale mailbox key without CR_HAS_MBX (after MBX_REVOKE)
  ASSERT_TRUE(b.table.save(*c));

  uint8_t m1[6], m2[6], m3[6];
  memset(m1, 0x11, 6);
  memset(m2, 0x22, 6);
  memset(m3, 0x33, 6);
  EXPECT_EQ(b.table.findByMailbox(m1), a);
  EXPECT_EQ(b.table.findByMailbox(m2), nullptr);
  EXPECT_EQ(b.table.findByMailbox(m3), nullptr);
}

TEST(RdmContacts, TableIsCappedAtSlotsMax) {
  MemFileIO io;
  Boot b(io, RDM_CONTACT_SLOTS_MAX + 4);
  ASSERT_TRUE(b.table.begin());
  for (int i = 0; i <= RDM_CONTACT_SLOTS_MAX; i++) {
    uint8_t p[6] = {(uint8_t)i, (uint8_t)(i >> 8), 0xC0, 0xFF, 0xEE, 0x01};
    ASSERT_NE(b.table.findOrAdd(p), nullptr) << i;
  }
  uint8_t first[6] = {0, 0, 0xC0, 0xFF, 0xEE, 0x01};
  EXPECT_EQ(b.table.find(first), nullptr) << "RAM table holds RDM_CONTACT_SLOTS_MAX entries";
}

// Iteration lets the inbox clear watermarks when the register is recreated (WP4 open point 3).
TEST(RdmContacts, IterateSlots) {
  MemFileIO io;
  {
    Boot b(io, 4);
    ASSERT_TRUE(b.table.begin());
    EXPECT_EQ(b.table.count(), 4);
    for (uint16_t i = 0; i < 4; i++) EXPECT_EQ(b.table.at(i), nullptr) << "empty table";
    ASSERT_NE(b.table.findOrAdd(Prefix(1).b), nullptr);
    ContactRdm* c = b.table.findOrAdd(Prefix(2).b);
    c->watermark = 9;
    ASSERT_TRUE(b.table.save(*c));
  }
  Boot b(io, 4);
  ASSERT_TRUE(b.table.begin());
  int seen = 0;
  for (uint16_t i = 0; i < b.table.count(); i++) {
    ContactRdm* c = b.table.at(i);
    if (!c) continue;
    seen++;
    if (c->watermark) {
      c->watermark = 0;
      ASSERT_TRUE(b.table.save(*c));
    }
  }
  EXPECT_EQ(seen, 2);
  EXPECT_EQ(b.table.at(4), nullptr) << "out of range";
  Boot again(io, 4);
  ASSERT_TRUE(again.table.begin());
  EXPECT_EQ(again.table.find(Prefix(2).b)->watermark, 0u);
}

TEST(RdmContacts, CountIsCappedAtSlotsMax) {
  MemFileIO io;
  Boot b(io, RDM_CONTACT_SLOTS_MAX + 4);
  ASSERT_TRUE(b.table.begin());
  EXPECT_EQ(b.table.count(), RDM_CONTACT_SLOTS_MAX);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
