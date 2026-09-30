#include "Scenario.h"

#include <helpers/rdm/RdmCrypto.h>

namespace sim {

World::World(const WorldOptions& o) : s(o.seed), opt(o) {
  bob = &s.add<CompanionNode>("bob");
  if (o.layout == Layout::VIA_REPEATER) rep = &s.add<RepeaterNode>("rep");
  alice = &s.add<CompanionNode>("alice");
  if (o.alice_rdm_flash) {
    SimFS& fs = alice->fs();
    for (auto it = fs.files.begin(); it != fs.files.end();) it = it->first.compare(0, 5, "/rdm/") == 0 ? fs.files.erase(it) : std::next(it);
    fs.capacity_bytes = fs.used_bytes() + o.alice_rdm_flash;
    alice->reboot();
  }

  for (int i = 0; i < o.mailboxes; i++) {
    for (int k = 0; k < 16; k++) k_owner[i][k] = (uint8_t)(0x40 + 16 * i + k);
    std::string name = "mbx" + std::to_string(i + 1);
    if (i == 0 && o.real_mbxd) {
      const char* base = getenv("TMPDIR");
      std::string tmpl = std::string(base && *base ? base : "/tmp") + "/rdm_s07_XXXXXX";
      std::vector<char> buf(tmpl.begin(), tmpl.end());
      buf.push_back(0);
      if (mkdtemp(buf.data())) mbxd_dir = buf.data();
      std::string daemon = SubprocessBackend::locateMbxd();
      EXPECT_FALSE(daemon.empty()) << SubprocessBackend::missingDaemon();
      mbxd.reset(new SubprocessBackend(daemon, mbxd_dir + "/mbxd.db", [this] { return s.wall_epoch(); }));
      SubprocessBackend::Owner ow;
      memcpy(ow.pub, alice->identity().pub_key, 32);
      memcpy(ow.k_owner, k_owner[i], 16);
      EXPECT_TRUE(mbxd->ownerAdd(ow)) << mbxd->lastError();
      EXPECT_TRUE(mbxd->start()) << mbxd->lastError();
      // the radio says HELLO to the daemon at every boot; replies still queued keep fast-forward from jumping
      mbx.push_back(&s.add<RdmSimMailbox>(name, *mbxd, [this] { if (mbxd->running()) mbxd->hello(); },
                                          [this] { return mbxd->pendingReplies() > 0; }));
    } else {
      mbx.push_back(&s.add<RdmSimMailbox>(name));
      addAliceAsOwner(i);
    }
  }

  if (rep) {
    s.link(*bob, *rep);
    s.link(*rep, *alice);
    for (SimNode* m : mbx) s.link(*m, *rep);
  } else {
    if (o.ab_link) s.link(*bob, *alice);
    for (SimNode* m : mbx) {
      s.link(*m, *bob);
      s.link(*m, *alice);
    }
  }
  wires.addNode(*bob);
  wires.addNode(*alice);
  if (rep) wires.addNode(*rep);
  for (SimNode* m : mbx) wires.addNode(*m);

  bob_app.reset(new RdmSimApp(s, *bob));
  bob_app->rdm_client = o.bob_rdm_client;
  alice_app.reset(new RdmSimApp(s, *alice));
  bob_app->connect();
  alice_app->connect();

  introduce();
  if (o.mailboxes > 0) {
    alice_app->setMailbox(mbxPub(0), k_owner[0]);
    EXPECT_TRUE(runUntil([&] { return alice_app->mailboxResult() >= 0; }, 10000));
    EXPECT_EQ(1, alice_app->mailboxResult()) << "CMD_RDM_SET_MAILBOX";
  }

  if (o.warmup) {
    bool temp_link = !rep && !o.ab_link;
    if (temp_link) s.link(*bob, *alice);
    int w = bobSends("warm-up");
    // A reported final state frees the slot at once (G2), so the standard app's view is the outbox itself. The loose
    // ACK_S can collide at R with Bob's reciprocal PATH (07 par. 6); the ON_RADIO query after 1 h recovers it.
    s.set_fast_forward(60000);
    EXPECT_TRUE(runUntil([&] {
      return bob_app->hasStatus(w, rdm::UserStatus::DELIVERED) ||
             (alice_app->inboxCount("warm-up") == 1 && bobState(w, rdm::OutState::DELIVERED));
    }, 2 * 3600 * 1000))
        << "warm-up not delivered: " << trace(w) << wires.dump();
    if (o.mailboxes > 0) {
      EXPECT_TRUE(runUntil([&] { return bobKnowsAliceMailbox(0); }, 30 * 60000)) << "no MBX_INFO at Bob\n"
                                                                                  << wires.dump();
    }
    runFor(30000);
    if (temp_link) s.unlink(*bob, *alice);
  }
  s.set_fast_forward(60000);
}

World::~World() {
  if (mbxd) mbxd->stop();
  if (!mbxd_dir.empty()) (void)system(("rm -rf '" + mbxd_dir + "'").c_str());
}

void World::introduce() {
  bob->app().advert(true);
  runFor(5000);
  alice->app().advert(true);
  runFor(5000);
  addContact(*bob, *alice, true);
  addContact(*alice, *bob, true);
  runFor(1000);
}

void World::tickApps() {
  bob_app->tick();
  alice_app->tick();
}

int World::bobSends(const std::string& text, AppRetry policy) {
  int id = bob_app->send(*alice, text, policy);
  EXPECT_GE(id, 0) << "Bob's app is not connected";
  if (id >= 0) EXPECT_TRUE(runUntil([&] { return bob_app->answered(id); }, 10000)) << "no RESP_CODE_SENT";
  return id;
}

void World::runFor(uint64_t ms) {
  s.run_until([&] {
    tickApps();
    return false;
  }, ms);
}

bool World::runUntil(const std::function<bool()>& pred, uint64_t timeout_ms) {
  return s.run_until([&] {
    tickApps();
    return pred();
  }, timeout_ms);
}

void World::addAliceAsOwner(int i) {
  EXPECT_TRUE(memBackend(i).addOwner(alice->identity().pub_key, k_owner[i]));
  mbx.at((size_t)i)->reboot();
}

void World::registerBobAt(int i) {
  uint8_t token[8];
  rdm::crypto::tokenB(token, k_owner[i], bob->identity().pub_key);
  MemMailboxBackend& be = memBackend(i);
  EXPECT_TRUE(be.reg(0xFFFFFF00u, bob->identity().pub_key, alice->identity().pub_key, token));
  rdm::BackendReply r;
  while (be.poll(r)) EXPECT_EQ(rdm::MbxCode::OK, r.code);
  mbx.at((size_t)i)->reboot();
}

bool World::bobKnowsAliceMailbox(int i) {
  ContactRdmView v;
  if (!readContactRdm(bob->fs(), alice->identity().pub_key, v)) return false;
  return (v.flags & rdm::CR_HAS_MBX) && memcmp(v.mbx_pub, mbxPub(i), 32) == 0;
}

void World::wipeAliceInbox() {
  EXPECT_FALSE(alice->powered());
  alice->fs().files.erase("/rdm/inbox");
  alice->fs().files.erase("/rdm/register");
}

std::string World::trace(int bob_msg) {
  std::string t = "bob 0x91 " + toString(bob_app->statuses(bob_msg)) + ", confirms " +
                  std::to_string(bob_app->confirmCount(bob_msg)) + ", alice app shows " +
                  std::to_string(alice_app->inboxCount(bob_app->sent(bob_msg).text)) + "x\n";
  return t;
}

}
