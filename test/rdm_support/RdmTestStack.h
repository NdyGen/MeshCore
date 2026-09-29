#pragma once

#include <helpers/rdm/RdmInbox.h>

#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <string>
#include <string.h>

// An Inbox with its register and contact table on the paths and record sizes Node uses, for module tests.
// Calling boot() again on the same FileIO is a reboot; change the slot counts before it to resize a file.
struct RdmTestStack {
  static constexpr uint8_t FORMAT_VER = 1;

  rdm::FileIO& io;
  uint16_t in_slots = 8;
  uint16_t reg_slots = 16;
  uint16_t contact_slots = 16;
  std::unique_ptr<rdm::RecordFile> inbox_file, reg_file, contacts_file;
  std::unique_ptr<rdm::ContactTable> contacts;
  std::unique_ptr<rdm::Inbox> inbox;
  bool recreated = false;

  explicit RdmTestStack(rdm::FileIO& fio) : io(fio) {}

  void boot(rdm::InboxHost& host) {
    inbox.reset();
    contacts.reset();
    inbox_file.reset(new rdm::RecordFile(io, rdm::Inbox::PATH, FORMAT_VER, rdm::Inbox::RECORD_SIZE, in_slots, false));
    reg_file.reset(new rdm::RecordFile(io, rdm::Inbox::REG_PATH, FORMAT_VER, rdm::Inbox::REG_RECORD_SIZE, reg_slots, true));
    contacts_file.reset(new rdm::RecordFile(io, rdm::ContactTable::PATH, FORMAT_VER, rdm::ContactTable::RECORD_SIZE,
                                            contact_slots, true));
    contacts.reset(new rdm::ContactTable(*contacts_file));
    ASSERT_TRUE(contacts->begin());
    inbox.reset(new rdm::Inbox(*inbox_file, *reg_file, *contacts, host));
    ASSERT_TRUE(inbox->begin(recreated));
  }

  // A received DM as Node hands it to the Inbox. The text lives as long as the stack.
  rdm::RecvInput makeRecv(const uint8_t sender_pub[32], uint32_t ts, const std::string& text, bool cap,
                          const uint8_t* mbx_hash = nullptr, uint8_t attempt = 0, uint8_t path_len = 0,
                          int8_t snr_x4 = 0) {
    _texts.push_back(text);
    rdm::RecvInput in;
    memset(&in, 0, sizeof(in));
    in.sender_pub = sender_pub;
    in.ts = ts;
    in.flags = attempt & 3;
    in.txt_type = 0;
    in.text = _texts.back().c_str();
    in.text_len = (uint8_t)text.size();
    in.sender_cap = cap;
    in.via_mailbox = mbx_hash != nullptr;
    in.mbx_hash = mbx_hash;
    in.path_len = path_len;
    in.snr_x4 = snr_x4;
    return in;
  }

private:
  std::deque<std::string> _texts;   // deque: pointers into earlier texts stay valid
};
