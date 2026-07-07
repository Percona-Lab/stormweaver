#include <catch2/catch_test_macros.hpp>

#include "action/action_registry.hpp"
#include "action/ddl.hpp"
#include "action/transaction.hpp"
#include "metadata/context.hpp"
#include "metadata_populator.hpp"
#include "schema_discovery.hpp"
#include "sql.hpp"
#include "sql_variant/postgresql.hpp"

#include <memory>

namespace {

struct Fixture {
  mutable metadata::TableRegistry reg;
  mutable metadata::Context ctx{reg};
  mutable ps_random rand{20260707}; // fixed: sections must be reproducible
  mutable action::AllConfig config;

  void seedTables(std::size_t count) const {
    for (std::size_t i = 0; i < count; ++i) {
      action::CreateTable ct(config.ddl, metadata::Table::Type::normal);
      REQUIRE_NOTHROW(ct.execute(ctx, rand, sqlConnection.get()));
    }
  }

  // drift oracle: rediscover the schema into a fresh registry and compare
  void requireMatchesDatabase() const {
    metadata::TableRegistry fresh;
    auto discovery =
        schema_discovery::make_schema_discovery(sqlConnection.get());
    metadata_populator::MetadataPopulator populator(fresh);
    populator.populateFromExistingDatabase(*discovery);
    REQUIRE(metadata::normalize(reg) == metadata::normalize(fresh));
  }
};

} // namespace

TEST_CASE_PERSISTENT_FIXTURE(Fixture, "transactions") {

  SECTION("committed transactions publish buffered deltas") {
    testutil::resetTestSchema();
    reg.reset();
    seedTables(5);

    config.transaction.commit_probability = 100;
    action::TransactionAction trx(
        config, action::default_registry(sqlConnection->serverInfo().flavor_));
    for (int i = 0; i < 100; ++i) {
      REQUIRE_NOTHROW(trx.execute(ctx, rand, sqlConnection.get()));
    }
    requireMatchesDatabase();
  }

  SECTION("rolled back transactions leave no trace") {
    testutil::resetTestSchema();
    reg.reset();
    seedTables(5);

    // pg rolls all DDL back, so the catalog and database both stay at the
    // seeded state regardless of what got picked - a strong check that the
    // buffer is discarded for every action type.
    config.transaction.commit_probability = 0; // always ROLLBACK
    action::TransactionAction trx(
        config, action::default_registry(sqlConnection->serverInfo().flavor_));
    for (int i = 0; i < 50; ++i) {
      REQUIRE_NOTHROW(trx.execute(ctx, rand, sqlConnection.get()));
    }
    requireMatchesDatabase();
  }

  SECTION("savepoint mode tolerates failing statements") {
    testutil::resetTestSchema();
    reg.reset();
    seedTables(3);

    // a guaranteed-failing custom action mixed into the full pool
    action::ActionRegistry pool;
    pool.use(action::default_registry(sqlConnection->serverInfo().flavor_));
    pool.makeCustomSqlAction("boom", "SELECT * FROM no_such_table_ever;", 500);

    // explicit: the fixture persists across sections, don't rely on defaults
    config.transaction.error_mode =
        action::TransactionConfig::ErrorMode::savepoint;
    config.transaction.commit_probability = 100;
    action::TransactionAction trx(config, pool);
    for (int i = 0; i < 100; ++i) {
      REQUIRE_NOTHROW(trx.execute(ctx, rand, sqlConnection.get()));
    }
    // savepoints recovered every failure; survivors committed; no drift,
    // and - critically on pg - no 25P02 poisoned-transaction cascade
    requireMatchesDatabase();
  }

  SECTION("abort mode rolls back on first failure") {
    testutil::resetTestSchema();
    reg.reset();
    seedTables(3);

    action::ActionRegistry pool;
    pool.makeCustomSqlAction("boom", "SELECT * FROM no_such_table_ever;", 1000);

    config.transaction.error_mode = action::TransactionConfig::ErrorMode::abort;
    action::TransactionAction trx(config, pool);
    REQUIRE_THROWS_AS(trx.execute(ctx, rand, sqlConnection.get()),
                      sql_variant::SqlException);
    // connection must be usable again immediately (guard rolled back)
    REQUIRE(sqlConnection->executeQuery("SELECT 1;").success());
    requireMatchesDatabase();
  }

  SECTION("mysql mirror mode: no-op DDL must not end the transaction") {
    if (!testutil::isMysql()) {
      SKIP("mysql-specific");
    }
    // regression: the mirror-mode implicit-commit flush used to fire on the
    // DDL *type* before the sub ran; a no-op DDL (here: DropTable skipping
    // at min_table_count) disarmed the guard and skipped the final COMMIT,
    // leaking an open transaction onto the shared connection.
    testutil::resetTestSchema();
    reg.reset();
    seedTables(2);
    sqlConnection->executeQuery("CREATE TABLE leakprobe (id INT PRIMARY KEY);")
        .maybeThrow();

    action::ActionRegistry pool;
    pool.insert(action::default_registry(
        sqlConnection->serverInfo().flavor_)["drop_table"]);

    config.ddl.min_table_count = 5; // > seeded count: DropTable always no-ops
    config.transaction.error_mode =
        action::TransactionConfig::ErrorMode::savepoint;
    config.transaction.mysql_ddl_mode =
        action::TransactionConfig::MysqlDdlMode::mirror;
    config.transaction.commit_probability = 100;
    action::TransactionAction trx(config, pool);
    for (int i = 0; i < 10; ++i) {
      REQUIRE_NOTHROW(trx.execute(ctx, rand, sqlConnection.get()));
    }
    config.ddl.min_table_count = action::DdlConfig{}.min_table_count;

    // leak probe: in autocommit the INSERT is durable immediately and the
    // ROLLBACK is a no-op; inside a leaked transaction the ROLLBACK erases
    // it. (a leaked trx with no statements is invisible to innodb_trx, so
    // this behavioral check is the reliable one.)
    sqlConnection->executeQuery("INSERT INTO leakprobe VALUES (1);")
        .maybeThrow();
    std::ignore = sqlConnection->executeQuery("ROLLBACK;");
    auto cnt =
        sqlConnection->querySingleValue("SELECT COUNT(*) FROM leakprobe;");
    REQUIRE(cnt.has_value());
    REQUIRE(*cnt == "1");
  }

  SECTION("mysql mirror mode: DDL implicit commit flushes buffer") {
    if (!testutil::isMysql()) {
      SKIP("mysql-specific");
    }
    testutil::resetTestSchema();
    reg.reset();
    seedTables(3);

    // DDL-only pool guarantees the implicit-commit path fires with real DDL
    action::ActionRegistry pool;
    pool.insert(action::default_registry(
        sqlConnection->serverInfo().flavor_)["create_normal_table"]);

    // rollback decision at the end must not matter: server committed already
    config.transaction.commit_probability = 0;
    config.transaction.mysql_ddl_mode =
        action::TransactionConfig::MysqlDdlMode::mirror;
    config.transaction.error_mode =
        action::TransactionConfig::ErrorMode::savepoint;
    action::TransactionAction trx(config, pool);
    for (int i = 0; i < 10; ++i) {
      REQUIRE_NOTHROW(trx.execute(ctx, rand, sqlConnection.get()));
    }
    requireMatchesDatabase(); // created tables ARE in catalog and DB
  }

  SECTION("mysql exclude mode: no DDL picked inside transactions") {
    if (!testutil::isMysql()) {
      SKIP("mysql-specific");
    }
    testutil::resetTestSchema();
    reg.reset();
    seedTables(3);
    const auto before = reg.get<metadata::Table>().size();

    config.transaction.commit_probability = 100;
    config.transaction.mysql_ddl_mode =
        action::TransactionConfig::MysqlDdlMode::exclude;
    config.transaction.error_mode =
        action::TransactionConfig::ErrorMode::savepoint;
    action::TransactionAction trx(
        config, action::default_registry(sqlConnection->serverInfo().flavor_));
    for (int i = 0; i < 30; ++i) {
      REQUIRE_NOTHROW(trx.execute(ctx, rand, sqlConnection.get()));
    }
    REQUIRE(reg.get<metadata::Table>().size() == before); // DML only
    requireMatchesDatabase();
  }

  SECTION("pg: serialization failure classifies as conflict") {
    if (testutil::isMysql()) {
      SKIP("pg-specific");
    }
    testutil::resetTestSchema();
    sqlConnection->executeQuery("CREATE TABLE cc (id INT PRIMARY KEY, v INT);")
        .maybeThrow();
    sqlConnection->executeQuery("INSERT INTO cc VALUES (1, 0);").maybeThrow();

    auto second = std::make_unique<sql_variant::LoggedSQL>(
        std::make_unique<sql_variant::PostgreSQL>(globalConnParams),
        "conflict-test");

    sqlConnection->executeQuery("BEGIN ISOLATION LEVEL REPEATABLE READ;")
        .maybeThrow();
    sqlConnection->executeQuery("SELECT v FROM cc WHERE id = 1;").maybeThrow();
    second->executeQuery("UPDATE cc SET v = 1 WHERE id = 1;").maybeThrow();
    auto res = sqlConnection->executeQuery("UPDATE cc SET v = 2 WHERE id = 1;");
    REQUIRE_FALSE(res.success());
    REQUIRE(res.errorInfo.errorClass == sql_variant::ErrorClass::conflict);
    std::ignore = sqlConnection->executeQuery("ROLLBACK;");
  }
}
