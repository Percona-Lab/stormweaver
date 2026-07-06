#include <catch2/catch_test_macros.hpp>

#include "metadata/table.hpp"

using metadata::Column;
using metadata::Ref;
using metadata::Table;
using metadata::TableRegistry;

namespace {

metadata::Index makeIndex(std::string name, std::string column,
                          metadata::IndexOrdering ordering, bool unique) {
  metadata::Index idx;
  idx.name = std::move(name);
  idx.unique = unique;
  idx.fields.push_back(metadata::IndexColumn{std::move(column), ordering});
  return idx;
}

Table makeTable(TableRegistry &reg, std::string name,
                metadata::ObjectId fkTarget = 0) {
  Table t;
  t.id = reg.nextId();
  t.name = std::move(name);

  Column id;
  id.name = "id";
  id.type = metadata::ColumnType::INT;
  id.primary_key = true;
  id.nullable = false;
  t.columns.push_back(id);

  Column payload;
  payload.name = "payload";
  payload.type = metadata::ColumnType::VARCHAR;
  payload.length = 32;
  if (fkTarget != 0) {
    payload.foreign_key_references = Ref<Table>{fkTarget};
  }
  t.columns.push_back(payload);

  return t;
}

} // namespace

TEST_CASE("Table reference helpers work by id", "[table]") {
  TableRegistry reg;
  auto target = makeTable(reg, "target");
  const auto targetId = target.id;
  reg.get<Table>().insert(std::move(target));

  auto referrer = makeTable(reg, "referrer", targetId);
  REQUIRE(referrer.hasReferenceTo(targetId));
  REQUIRE_FALSE(referrer.hasReferenceTo(targetId + 1000));

  REQUIRE(referrer.removeReferencesTo(targetId));
  REQUIRE_FALSE(referrer.hasReferenceTo(targetId));
  REQUIRE_FALSE(referrer.removeReferencesTo(targetId));
}

TEST_CASE("normalize is id-independent and resolves refs to names", "[table]") {
  TableRegistry a;
  {
    auto t1 = makeTable(a, "alpha");
    const auto t1id = t1.id;
    a.get<Table>().insert(std::move(t1));
    a.get<Table>().insert(makeTable(a, "beta", t1id));
  }

  TableRegistry b;
  {
    // different insertion order and different ids, same structure
    b.nextId();
    b.nextId(); // burn ids so they differ from registry a
    auto t1 = makeTable(b, "alpha");
    const auto t1id = t1.id;
    b.get<Table>().insert(makeTable(b, "beta", t1id));
    b.get<Table>().insert(std::move(t1));
  }

  REQUIRE(metadata::normalize(a) == metadata::normalize(b));
}

TEST_CASE("normalize is order-independent for columns, indexes and ranges",
          "[table]") {
  auto makeColumn = [](std::string name) {
    Column c;
    c.name = std::move(name);
    return c;
  };

  TableRegistry a;
  {
    Table t;
    t.id = a.nextId();
    t.name = "t";
    t.columns.push_back(makeColumn("id"));
    t.columns.push_back(makeColumn("payload"));
    t.indexes.push_back(
        makeIndex("idx_a", "id", metadata::IndexOrdering::asc, false));
    t.indexes.push_back(
        makeIndex("idx_b", "payload", metadata::IndexOrdering::desc, true));
    t.partitioning = metadata::RangePartitioning{10, {{0}, {1}, {2}}};
    a.get<Table>().insert(std::move(t));
  }

  TableRegistry b;
  {
    // identical content, permuted order everywhere
    Table t;
    t.id = b.nextId();
    t.name = "t";
    t.columns.push_back(makeColumn("payload"));
    t.columns.push_back(makeColumn("id"));
    t.indexes.push_back(
        makeIndex("idx_b", "payload", metadata::IndexOrdering::desc, true));
    t.indexes.push_back(
        makeIndex("idx_a", "id", metadata::IndexOrdering::asc, false));
    t.partitioning = metadata::RangePartitioning{10, {{2}, {0}, {1}}};
    b.get<Table>().insert(std::move(t));
  }

  REQUIRE(metadata::normalize(a) == metadata::normalize(b));
}

TEST_CASE("normalize detects index differences", "[table]") {
  auto addIndexed = [](TableRegistry &reg, bool unique,
                       metadata::IndexOrdering ordering) {
    auto t = makeTable(reg, "t");
    t.indexes.push_back(makeIndex("idx", "id", ordering, unique));
    reg.get<Table>().insert(std::move(t));
  };

  TableRegistry a;
  addIndexed(a, false, metadata::IndexOrdering::asc);

  TableRegistry uniqueDiffers;
  addIndexed(uniqueDiffers, true, metadata::IndexOrdering::asc);
  REQUIRE_FALSE(metadata::normalize(a) == metadata::normalize(uniqueDiffers));

  TableRegistry orderingDiffers;
  addIndexed(orderingDiffers, false, metadata::IndexOrdering::desc);
  REQUIRE_FALSE(metadata::normalize(a) == metadata::normalize(orderingDiffers));
}

TEST_CASE("normalize detects structural difference", "[table]") {
  TableRegistry a;
  a.get<Table>().insert(makeTable(a, "alpha"));

  TableRegistry b;
  auto t = makeTable(b, "alpha");
  Column extra;
  extra.name = "extra";
  t.columns.push_back(extra);
  b.get<Table>().insert(std::move(t));

  REQUIRE_FALSE(metadata::normalize(a) == metadata::normalize(b));
}

TEST_CASE("normalize tolerates dangling references", "[table]") {
  TableRegistry a;
  auto target = makeTable(a, "target");
  const auto targetId = target.id;
  a.get<Table>().insert(std::move(target));
  a.get<Table>().insert(makeTable(a, "referrer", targetId));
  a.get<Table>().erase(targetId);

  auto normalized = metadata::normalize(a);
  REQUIRE(normalized.size() == 1);

  // dangling must differ from both "no ref" and a resolvable ref
  TableRegistry b;
  b.get<Table>().insert(makeTable(b, "referrer"));
  REQUIRE_FALSE(normalized == metadata::normalize(b));

  TableRegistry c;
  auto live = makeTable(c, "target");
  const auto liveId = live.id;
  c.get<Table>().insert(std::move(live));
  c.get<Table>().insert(makeTable(c, "referrer", liveId));
  auto normalizedC = metadata::normalize(c);
  REQUIRE_FALSE(normalized == normalizedC);
  // element-level: dangling differs from resolvable on the referrer itself
  REQUIRE(normalizedC[0].name == "referrer");
  REQUIRE_FALSE(normalized[0] == normalizedC[0]);
}

TEST_CASE("debug_dump renders resolved reference names", "[table]") {
  TableRegistry reg;
  auto target = makeTable(reg, "target");
  const auto targetId = target.id;
  reg.get<Table>().insert(std::move(target));
  reg.get<Table>().insert(makeTable(reg, "referrer", targetId));

  const auto dump = metadata::debug_dump(reg);
  REQUIRE(dump.find("referrer") != std::string::npos);
  REQUIRE(dump.find("REFERENCES target") != std::string::npos);
}
