#include <catch2/catch_test_macros.hpp>

#include "metadata/context.hpp"
#include "random.hpp"

using metadata::Catalog;
using metadata::Context;
using metadata::Table;
using metadata::TableRegistry;
using metadata::TxnBuffer;

namespace {
Table makeTable(metadata::ObjectId id, std::string name) {
  Table t;
  t.id = id;
  t.name = std::move(name);
  return t;
}
} // namespace

TEST_CASE("Context without buffer forwards to catalog", "[context]") {
  TableRegistry reg;
  Context ctx(reg);

  auto tables = ctx.get<Table>();
  REQUIRE(tables.insert(makeTable(ctx.nextId(), "t1")));
  REQUIRE(tables.size() == 1);
  REQUIRE(reg.get<Table>().byName("t1") != nullptr); // published immediately

  REQUIRE(tables.update(reg.get<Table>().byName("t1")->id, [](Table &t) {
    t.name = "t2";
    return true;
  }));
  REQUIRE(reg.get<Table>().byName("t2") != nullptr);
  REQUIRE(tables.erase(reg.get<Table>().byName("t2")->id));
  REQUIRE(reg.get<Table>().size() == 0);
}

TEST_CASE("Buffered insert invisible globally until publish", "[context]") {
  TableRegistry reg;
  TxnBuffer<Table> txn;
  Context ctx(reg, &txn);
  auto tables = ctx.get<Table>();

  const auto id = ctx.nextId();
  REQUIRE(tables.insert(makeTable(id, "local")));

  // visible through the view
  REQUIRE(tables.size() == 1);
  REQUIRE(tables.byId(id) != nullptr);
  REQUIRE(tables.byName("local") != nullptr);
  // invisible globally
  REQUIRE(reg.get<Table>().size() == 0);

  txn.publishAll(reg.get<Table>());
  REQUIRE(reg.get<Table>().byName("local") != nullptr);
}

TEST_CASE("Buffered erase hides global record", "[context]") {
  TableRegistry reg;
  const auto id = reg.nextId();
  REQUIRE(reg.get<Table>().insert(makeTable(id, "gone")));

  TxnBuffer<Table> txn;
  Context ctx(reg, &txn);
  auto tables = ctx.get<Table>();

  REQUIRE(tables.erase(id));
  REQUIRE(tables.byId(id) == nullptr);
  REQUIRE(tables.byName("gone") == nullptr);
  REQUIRE(tables.size() == 0);
  // still in the global catalog
  REQUIRE(reg.get<Table>().byId(id) != nullptr);

  txn.publishAll(reg.get<Table>());
  REQUIRE(reg.get<Table>().byId(id) == nullptr);
}

TEST_CASE("Buffered update shadows global and masks stale name", "[context]") {
  TableRegistry reg;
  const auto id = reg.nextId();
  REQUIRE(reg.get<Table>().insert(makeTable(id, "old")));

  TxnBuffer<Table> txn;
  Context ctx(reg, &txn);
  auto tables = ctx.get<Table>();

  REQUIRE(tables.update(id, [](Table &t) {
    t.name = "renamed";
    return true;
  }));

  REQUIRE(tables.byName("renamed") != nullptr);
  REQUIRE(tables.byName("old") == nullptr); // stale global name masked
  REQUIRE(tables.byId(id)->name == "renamed");
  REQUIRE(reg.get<Table>().byId(id)->name == "old"); // global untouched

  txn.publishAll(reg.get<Table>());
  REQUIRE(reg.get<Table>().byId(id)->name == "renamed");
  REQUIRE(reg.get<Table>().byName("old") == nullptr);
}

TEST_CASE("rollbackTo truncates ops and rebuilds overlay", "[context]") {
  TableRegistry reg;
  const auto gid = reg.nextId();
  REQUIRE(reg.get<Table>().insert(makeTable(gid, "base")));

  TxnBuffer<Table> txn;
  Context ctx(reg, &txn);
  auto tables = ctx.get<Table>();

  const auto m0 = txn.mark();
  const auto id1 = ctx.nextId();
  REQUIRE(tables.insert(makeTable(id1, "one")));
  const auto m1 = txn.mark();
  REQUIRE(tables.erase(gid));
  REQUIRE(tables.size() == 1); // one, base erased

  txn.rollbackTo(m1, reg.get<Table>());
  REQUIRE(tables.byId(gid) != nullptr); // erase undone
  REQUIRE(tables.byId(id1) != nullptr); // insert kept
  REQUIRE(tables.size() == 2);

  txn.rollbackTo(m0, reg.get<Table>());
  REQUIRE(tables.byId(id1) == nullptr);
  REQUIRE(tables.size() == 1);

  txn.publishAll(reg.get<Table>());
  REQUIRE(reg.get<Table>().size() == 1); // nothing net happened
}

TEST_CASE("create-alter-drop inside one trx nets to nothing", "[context]") {
  TableRegistry reg;
  TxnBuffer<Table> txn;
  Context ctx(reg, &txn);
  auto tables = ctx.get<Table>();

  const auto id = ctx.nextId();
  REQUIRE(tables.insert(makeTable(id, "tmp")));
  REQUIRE(tables.update(id, [](Table &t) {
    t.name = "tmp2";
    return true;
  }));
  REQUIRE(tables.erase(id));
  REQUIRE(tables.size() == 0);

  txn.publishAll(reg.get<Table>());
  REQUIRE(reg.get<Table>().size() == 0);
}

TEST_CASE("publish replay tolerates losing a race", "[context]") {
  TableRegistry reg;
  const auto id = reg.nextId();
  REQUIRE(reg.get<Table>().insert(makeTable(id, "victim")));

  TxnBuffer<Table> txn;
  Context ctx(reg, &txn);
  auto tables = ctx.get<Table>();
  REQUIRE(tables.update(id, [](Table &t) {
    t.name = "mine";
    return true;
  }));

  // concurrent worker drops it before we commit
  REQUIRE(reg.get<Table>().erase(id));

  txn.publishAll(reg.get<Table>()); // update lands on erased id: discarded
  REQUIRE(reg.get<Table>().size() == 0);
}

TEST_CASE("randomPick sees merged view", "[context]") {
  TableRegistry reg;
  const auto gid = reg.nextId();
  REQUIRE(reg.get<Table>().insert(makeTable(gid, "global")));

  TxnBuffer<Table> txn;
  Context ctx(reg, &txn);
  auto tables = ctx.get<Table>();
  const auto lid = ctx.nextId();
  REQUIRE(tables.insert(makeTable(lid, "local")));
  REQUIRE(tables.erase(gid));

  ps_random rand;
  for (int i = 0; i < 20; ++i) {
    auto pick = tables.randomPick(rand);
    REQUIRE(pick != nullptr);
    REQUIRE(pick->id == lid); // only the local one is pickable
  }
}

TEST_CASE("snapshotAll merges overlay", "[context]") {
  TableRegistry reg;
  const auto g1 = reg.nextId();
  const auto g2 = reg.nextId();
  REQUIRE(reg.get<Table>().insert(makeTable(g1, "a")));
  REQUIRE(reg.get<Table>().insert(makeTable(g2, "b")));

  TxnBuffer<Table> txn;
  Context ctx(reg, &txn);
  auto tables = ctx.get<Table>();
  REQUIRE(tables.erase(g1));
  const auto l1 = ctx.nextId();
  REQUIRE(tables.insert(makeTable(l1, "c")));

  auto all = tables.snapshotAll();
  REQUIRE(all.size() == 2);
}

TEST_CASE("rollbackTo mid-chain of updates on local record", "[context]") {
  TableRegistry reg;
  TxnBuffer<Table> txn;
  Context ctx(reg, &txn);
  auto tables = ctx.get<Table>();

  const auto id = ctx.nextId();
  REQUIRE(tables.insert(makeTable(id, "v0")));
  const auto m1 = txn.mark();
  REQUIRE(tables.update(id, [](Table &t) {
    t.name = "v1";
    return true;
  }));
  const auto m2 = txn.mark();
  REQUIRE(tables.update(id, [](Table &t) {
    t.name = "v2";
    return true;
  }));
  REQUIRE(tables.byId(id)->name == "v2");

  txn.rollbackTo(m2, reg.get<Table>());
  REQUIRE(tables.byId(id)->name == "v1"); // second update undone

  txn.rollbackTo(m1, reg.get<Table>());
  REQUIRE(tables.byId(id)->name == "v0"); // back to the insert
}

TEST_CASE("byName masking is id-keyed: reused old name resolves", "[context]") {
  TableRegistry reg;
  const auto gid = reg.nextId();
  REQUIRE(reg.get<Table>().insert(makeTable(gid, "orig")));

  TxnBuffer<Table> txn;
  Context ctx(reg, &txn);
  auto tables = ctx.get<Table>();
  REQUIRE(tables.update(gid, [](Table &t) {
    t.name = "renamed";
    return true;
  }));
  REQUIRE(tables.byName("orig") == nullptr); // stale name masked

  // concurrent worker reuses the old name with a new id
  const auto nid = reg.nextId();
  REQUIRE(reg.get<Table>().insert(makeTable(nid, "orig")));

  auto rec = tables.byName("orig");
  REQUIRE(rec != nullptr); // new record not masked
  REQUIRE(rec->id == nid);
  REQUIRE(tables.byName("renamed")->id == gid);
}

TEST_CASE("Context reports transaction presence", "[context]") {
  TableRegistry reg;

  Context direct(reg);
  REQUIRE(!direct.inTransaction());

  TxnBuffer<Table> txn;
  Context buffered(reg, &txn);
  REQUIRE(buffered.inTransaction());
}
