#include <catch2/catch_test_macros.hpp>

#include <fmt/format.h>

#include "action/action_registry.hpp"
#include "action/dml.hpp"
#include "metadata/context.hpp"
#include "metadata_populator.hpp"
#include "querygen/generator.hpp"
#include "querygen/render.hpp"
#include "schema_discovery.hpp"
#include "sql.hpp"
#include "sql_dialect/dialect.hpp"

namespace {

// three tables with FK chain t3 -> t2 -> t1, mixed types, some rows;
// table-level FOREIGN KEY: mysql silently ignores inline REFERENCES
void seedSchema() {
  testutil::resetTestSchema();
  auto exec = [](std::string const &sql) {
    sqlConnection->executeQuery(sql).maybeThrow();
  };
  exec("CREATE TABLE t1 (id " + testutil::autoIncPk() +
       " PRIMARY KEY, num INT, txt VARCHAR(32), flag BOOLEAN)");
  exec("CREATE TABLE t2 (id " + testutil::autoIncPk() +
       " PRIMARY KEY, ref INT, val REAL, "
       "FOREIGN KEY (ref) REFERENCES t1 (id) ON DELETE CASCADE)");
  exec("CREATE TABLE t3 (id " + testutil::autoIncPk() +
       " PRIMARY KEY, ref INT, note TEXT, "
       "FOREIGN KEY (ref) REFERENCES t2 (id) ON DELETE CASCADE)");
  exec("INSERT INTO t1 (num, txt, flag) VALUES (1, 'a', true), "
       "(2, 'b', false), (3, 'c', true)");
  exec("INSERT INTO t2 (ref, val) VALUES (1, 1.5), (1, 2.5), (2, 3.5)");
  exec("INSERT INTO t3 (ref, note) VALUES (1, 'xx'), (2, 'yy')");
}

void discoverCatalog(metadata::TableRegistry &reg) {
  auto discovery = schema_discovery::make_schema_discovery(sqlConnection.get());
  metadata_populator::MetadataPopulator pop(reg);
  pop.populateFromExistingDatabase(*discovery);
}

} // namespace

TEST_CASE("select_query action runs clean on seeded schema",
          "[querygen][sql]") {
  seedSchema();
  metadata::TableRegistry reg;
  discoverCatalog(reg);
  metadata::Context ctx(reg);

  action::AllConfig config;
  auto const &registry =
      action::default_registry(sqlConnection->serverInfo().flavor_);
  auto factory = registry["select_query"];
  ps_random rand(1234);

  for (int i = 0; i < 100; ++i) {
    auto act = factory.builder(
        action::BuildContext{.config = config, .registry = registry});
    REQUIRE_NOTHROW(act->execute(ctx, rand, sqlConnection.get()));
  }
}

TEST_CASE("dml actions with generated predicates run clean",
          "[querygen][sql]") {
  seedSchema();
  metadata::TableRegistry reg;
  discoverCatalog(reg);
  metadata::Context ctx(reg);

  action::AllConfig config;
  config.querygen.dml_predicate_prob = 100;
  auto const &registry =
      action::default_registry(sqlConnection->serverInfo().flavor_);
  ps_random rand(77);

  for (auto const &name : {"delete_some_data", "update_one_row"}) {
    auto factory = registry[name];
    for (int i = 0; i < 100; ++i) {
      auto act = factory.builder(
          action::BuildContext{.config = config, .registry = registry});
      REQUIRE_NOTHROW(act->execute(ctx, rand, sqlConnection.get()));
    }
  }
}

TEST_CASE("select-then-modify with generated pk-select runs clean",
          "[querygen][sql]") {
  seedSchema();
  metadata::TableRegistry reg;
  discoverCatalog(reg);
  metadata::Context ctx(reg);

  action::AllConfig config;
  config.querygen.dml_pk_select_prob = 100;
  auto const &registry =
      action::default_registry(sqlConnection->serverInfo().flavor_);
  ps_random rand(88);

  SECTION("autocommit") {
    for (auto const &name : {"delete_selected", "update_selected"}) {
      auto factory = registry[name];
      for (int i = 0; i < 100; ++i) {
        auto act = factory.builder(
            action::BuildContext{.config = config, .registry = registry});
        REQUIRE_NOTHROW(act->execute(ctx, rand, sqlConnection.get()));
      }
    }
  }

  SECTION("inside explicit transaction") {
    // buffer-attached context: the action skips its own BEGIN/COMMIT
    metadata::TxnBuffer<metadata::Table> txn;
    metadata::Context trxCtx(reg, &txn);
    for (auto const &name : {"delete_selected", "update_selected"}) {
      auto factory = registry[name];
      for (int i = 0; i < 100; ++i) {
        sqlConnection->executeQuery("BEGIN;").maybeThrow();
        auto act = factory.builder(
            action::BuildContext{.config = config, .registry = registry});
        REQUIRE_NOTHROW(act->execute(trxCtx, rand, sqlConnection.get()));
        sqlConnection->executeQuery("COMMIT;").maybeThrow();
      }
    }
  }
}

TEST_CASE("bulk generation sweep executes clean", "[querygen][sql]") {
  seedSchema();
  metadata::TableRegistry reg;
  discoverCatalog(reg);
  metadata::Context ctx(reg);

  querygen::QueryGenConfig cfg;
  cfg.join_prob = 80;
  cfg.subquery_prob = 60;
  cfg.aggregate_prob = 50;
  cfg.cte_prob = 50;
  cfg.setop_prob = 50;
  cfg.window_prob = 50;
  cfg.correlation_prob = 60;
  cfg.max_expr_depth = 6;
  cfg.max_joins = 4;

  auto const info = sqlConnection->serverInfo();
  auto const &dialect = sql_dialect::dialect_for(info);
  ps_random rand(20260709);

  auto tables = ctx.get<metadata::Table>().snapshotAll();
  REQUIRE(!tables.empty());

  for (int i = 0; i < 300; ++i) {
    querygen::Generator gen(ctx, rand, cfg, info);
    auto q = gen.generate(querygen::Purpose::standalone, nullptr);
    REQUIRE(q.has_value());
    auto sql = querygen::render(*q, dialect);
    INFO(sql);
    REQUIRE_NOTHROW(sqlConnection->executeQuery(sql).maybeThrow());
  }
  for (int i = 0; i < 200; ++i) {
    querygen::Generator gen(ctx, rand, cfg, info);
    auto const &target = tables[i % tables.size()];
    auto q = gen.generatePkSelect(target, {.limit = 10});
    REQUIRE(q.has_value());
    auto sql = querygen::render(*q, dialect);
    INFO(sql);
    REQUIRE_NOTHROW(sqlConnection->executeQuery(sql).maybeThrow());
  }
  for (int i = 0; i < 200; ++i) {
    querygen::Generator gen(ctx, rand, cfg, info);
    auto const &target = tables[i % tables.size()];
    auto pred = gen.generatePredicate(target, target->name);
    auto sql = fmt::format("SELECT COUNT(*) FROM {} WHERE {}", target->name,
                           querygen::render(pred, dialect));
    INFO(sql);
    REQUIRE_NOTHROW(sqlConnection->executeQuery(sql).maybeThrow());
  }
}

TEST_CASE("select_query throws empty-metadata on empty catalog",
          "[querygen][sql]") {
  metadata::TableRegistry reg;
  metadata::Context ctx(reg);
  action::SelectQuery act{querygen::QueryGenConfig{}};
  ps_random rand(1);
  try {
    act.execute(ctx, rand, sqlConnection.get());
    FAIL("expected ActionException");
  } catch (action::ActionException const &e) {
    CHECK(e.getErrorName() == "empty-metadata");
  }
}
