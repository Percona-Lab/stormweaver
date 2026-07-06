#include <catch2/catch_test_macros.hpp>

#include <fmt/format.h>

#include "metadata/catalog.hpp"
#include "random.hpp"

namespace {
struct Widget : metadata::ObjectBase {
  int payload = 0;
};

struct Gadget : metadata::ObjectBase {};

Widget makeWidget(metadata::ObjectId id, std::string name, int payload = 0) {
  Widget w;
  w.id = id;
  w.name = std::move(name);
  w.payload = payload;
  return w;
}
} // namespace

using metadata::Catalog;
using metadata::Registry;

TEST_CASE("Catalog insert and lookup", "[catalog]") {
  Catalog<Widget> catalog;

  REQUIRE(catalog.insert(makeWidget(1, "first", 42)));
  REQUIRE(catalog.size() == 1);

  auto byId = catalog.byId(1);
  REQUIRE(byId != nullptr);
  REQUIRE(byId->name == "first");
  REQUIRE(byId->payload == 42);
  REQUIRE(byId->version == 0);

  REQUIRE(catalog.byName("first") == byId);
  REQUIRE(catalog.byId(2) == nullptr);
  REQUIRE(catalog.byName("nope") == nullptr);
}

TEST_CASE("Catalog rejects id 0 and duplicate ids", "[catalog]") {
  Catalog<Widget> catalog;

  REQUIRE_FALSE(catalog.insert(makeWidget(0, "zero")));
  REQUIRE(catalog.insert(makeWidget(1, "first")));
  REQUIRE_FALSE(catalog.insert(makeWidget(1, "again")));
  REQUIRE(catalog.size() == 1);
}

TEST_CASE("Catalog update applies delta to current record", "[catalog]") {
  Catalog<Widget> catalog;
  REQUIRE(catalog.insert(makeWidget(1, "first", 1)));

  REQUIRE(catalog.update(1, [](Widget &w) {
    w.payload = 2;
    return true;
  }));

  auto rec = catalog.byId(1);
  REQUIRE(rec->payload == 2);
  REQUIRE(rec->version == 1);
}

TEST_CASE("Catalog update on missing id returns false", "[catalog]") {
  Catalog<Widget> catalog;
  REQUIRE_FALSE(catalog.update(1, [](Widget &) { return true; }));

  REQUIRE(catalog.insert(makeWidget(1, "first")));
  REQUIRE(catalog.erase(1));
  REQUIRE_FALSE(catalog.update(1, [](Widget &) { return true; }));
}

TEST_CASE("Catalog delta returning false keeps old record", "[catalog]") {
  Catalog<Widget> catalog;
  REQUIRE(catalog.insert(makeWidget(1, "first", 1)));

  REQUIRE_FALSE(catalog.update(1, [](Widget &w) {
    w.payload = 99;
    return false;
  }));

  auto rec = catalog.byId(1);
  REQUIRE(rec->payload == 1);
  REQUIRE(rec->version == 0);
}

TEST_CASE("Catalog update cannot change identity", "[catalog]") {
  Catalog<Widget> catalog;
  REQUIRE(catalog.insert(makeWidget(1, "first")));

  REQUIRE(catalog.update(1, [](Widget &w) {
    w.id = 42;
    return true;
  }));

  REQUIRE(catalog.byId(1) != nullptr);
  REQUIRE(catalog.byId(1)->id == 1);
  REQUIRE(catalog.byId(42) == nullptr);
}

TEST_CASE("Catalog rename maintains name index", "[catalog]") {
  Catalog<Widget> catalog;
  REQUIRE(catalog.insert(makeWidget(1, "first")));

  REQUIRE(catalog.update(1, [](Widget &w) {
    w.name = "second";
    return true;
  }));

  REQUIRE(catalog.byName("first") == nullptr);
  REQUIRE(catalog.byName("second") != nullptr);
  REQUIRE(catalog.byName("second")->id == 1);
}

TEST_CASE("Name reuse after rename points to new object", "[catalog]") {
  Catalog<Widget> catalog;
  REQUIRE(catalog.insert(makeWidget(1, "orig")));
  REQUIRE(catalog.update(1, [](Widget &w) {
    w.name = "moved";
    return true;
  }));
  REQUIRE(catalog.insert(makeWidget(2, "orig")));

  REQUIRE(catalog.byName("orig")->id == 2);
  REQUIRE(catalog.byId(1)->name == "moved");
}

TEST_CASE("Name collision: last publish wins, displaced stays by id",
          "[catalog]") {
  Catalog<Widget> catalog;
  REQUIRE(catalog.insert(makeWidget(1, "clash")));
  REQUIRE(catalog.insert(makeWidget(2, "clash")));

  REQUIRE(catalog.byName("clash")->id == 2);
  REQUIRE(catalog.byId(1) != nullptr);

  // erasing the displaced object must not remove the winner's name entry
  REQUIRE(catalog.erase(1));
  REQUIRE(catalog.byName("clash") != nullptr);
  REQUIRE(catalog.byName("clash")->id == 2);
}

TEST_CASE("Catalog erase removes object, name and sampling entry",
          "[catalog]") {
  Catalog<Widget> catalog;
  REQUIRE(catalog.insert(makeWidget(1, "a")));
  REQUIRE(catalog.insert(makeWidget(2, "b")));
  REQUIRE(catalog.insert(makeWidget(3, "c")));

  REQUIRE(catalog.erase(2));
  REQUIRE_FALSE(catalog.erase(2));
  REQUIRE(catalog.size() == 2);
  REQUIRE(catalog.byId(2) == nullptr);
  REQUIRE(catalog.byName("b") == nullptr);

  auto all = catalog.snapshotAll();
  REQUIRE(all.size() == 2);

  ps_random rand;
  for (int i = 0; i < 20; ++i) {
    auto pick = catalog.randomPick(rand);
    REQUIRE(pick != nullptr);
    REQUIRE(pick->id != 2);
  }
}

TEST_CASE("randomPick on empty catalog returns nullptr", "[catalog]") {
  Catalog<Widget> catalog;
  ps_random rand;
  REQUIRE(catalog.randomPick(rand) == nullptr);
}

TEST_CASE("snapshotAll returns every record", "[catalog]") {
  Catalog<Widget> catalog;
  for (metadata::ObjectId id = 1; id <= 5; ++id) {
    REQUIRE(catalog.insert(makeWidget(id, fmt::format("w{}", id))));
  }
  auto all = catalog.snapshotAll();
  REQUIRE(all.size() == 5);
}

TEST_CASE("Catalog reset clears everything", "[catalog]") {
  Catalog<Widget> catalog;
  REQUIRE(catalog.insert(makeWidget(1, "a")));
  catalog.reset();
  REQUIRE(catalog.size() == 0);
  REQUIRE(catalog.byId(1) == nullptr);
  REQUIRE(catalog.byName("a") == nullptr);
}

TEST_CASE("Registry hands out monotonic ids and typed catalogs", "[registry]") {
  Registry<Widget> registry;

  auto first = registry.nextId();
  auto second = registry.nextId();
  REQUIRE(first == 1);
  REQUIRE(second == 2);

  REQUIRE(registry.get<Widget>().insert(makeWidget(first, "a")));
  REQUIRE(registry.get<Widget>().size() == 1);

  registry.reset();
  REQUIRE(registry.get<Widget>().size() == 0);
  // ids are never reused, reset does not rewind the counter
  REQUIRE(registry.nextId() == 3);
}

TEST_CASE("Update-driven name collision: last publish wins", "[catalog]") {
  Catalog<Widget> catalog;
  REQUIRE(catalog.insert(makeWidget(1, "a")));
  REQUIRE(catalog.insert(makeWidget(2, "b")));

  REQUIRE(catalog.update(2, [](Widget &w) {
    w.name = "a";
    return true;
  }));

  REQUIRE(catalog.byName("a")->id == 2);
  // displaced by rename, still reachable by id
  REQUIRE(catalog.byId(1) != nullptr);
  REQUIRE(catalog.byId(1)->name == "a");
}

TEST_CASE("byName after update without rename sees new record", "[catalog]") {
  Catalog<Widget> catalog;
  REQUIRE(catalog.insert(makeWidget(1, "a", 1)));

  REQUIRE(catalog.update(1, [](Widget &w) {
    w.payload = 7;
    return true;
  }));

  auto rec = catalog.byName("a");
  REQUIRE(rec != nullptr);
  REQUIRE(rec->payload == 7);
  REQUIRE(rec->version == 1);
}

TEST_CASE("Registry with two kinds keeps separate catalogs", "[registry]") {
  Registry<Widget, Gadget> registry;

  REQUIRE(registry.get<Widget>().insert(makeWidget(registry.nextId(), "w")));
  Gadget g;
  g.id = registry.nextId();
  g.name = "g";
  REQUIRE(registry.get<Gadget>().insert(std::move(g)));

  REQUIRE(registry.get<Widget>().size() == 1);
  REQUIRE(registry.get<Gadget>().size() == 1);

  registry.reset();
  REQUIRE(registry.get<Widget>().size() == 0);
  REQUIRE(registry.get<Gadget>().size() == 0);
}
