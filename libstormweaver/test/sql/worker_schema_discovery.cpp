#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <spdlog/spdlog.h>

#include "sql.hpp"
#include "workload.hpp"

// Fixture to ensure clean database state for each test
class WorkerSchemaDiscoveryFixture {
public:
  WorkerSchemaDiscoveryFixture() {
    // Recreate public schema to ensure clean state
    sqlConnection->executeQuery("DROP SCHEMA IF EXISTS public CASCADE")
        .maybeThrow();
    sqlConnection->executeQuery("CREATE SCHEMA public").maybeThrow();
    sqlConnection->executeQuery("GRANT ALL ON SCHEMA public TO public")
        .maybeThrow();
  }

  ~WorkerSchemaDiscoveryFixture() {
    // Clean up is automatic via schema recreation in next test
  }
};

TEST_CASE_METHOD(WorkerSchemaDiscoveryFixture,
                 "Worker - Schema discovery basic workflow",
                 "[worker_schema_discovery]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_worker_basic (
            id SERIAL PRIMARY KEY,
            name VARCHAR(100) NOT NULL,
            price REAL,
            active BOOLEAN DEFAULT TRUE
        )
    )")
      .maybeThrow();

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_worker_indexed (
            id SERIAL PRIMARY KEY,
            email VARCHAR(255) UNIQUE,
            name VARCHAR(100),
            age INT
        )
    )")
      .maybeThrow();

  sqlConnection
      ->executeQuery(
          "CREATE INDEX idx_worker_name ON test_worker_indexed (name)")
      .maybeThrow();
  sqlConnection
      ->executeQuery("CREATE INDEX idx_worker_age_desc ON test_worker_indexed "
                     "(name, age DESC)")
      .maybeThrow();

  sql_variant::ServerParams params{"sql_tests",   "127.0.0.1", "",
                                   "stormweaver", "",          25432};

  SqlFactory::on_connect_t empty_callback;
  SqlFactory factory(params, empty_callback);

  auto logger =
      spdlog::get("test") ? spdlog::get("test") : spdlog::default_logger();
  LuaContext luaCtx(logger);

  auto metadata = std::make_shared<metadata::Metadata>();
  WorkloadParams wp;

  auto worker = std::make_unique<Worker>(
      "test-worker",
      [&factory, &luaCtx]() { return factory.connect("test-worker", luaCtx); },
      wp, metadata);

  REQUIRE(metadata->size() == 0);

  worker->discover_existing_schema();

  REQUIRE(metadata->size() == 2);

  auto find_table =
      [&metadata](const std::string &name) -> metadata::table_cptr {
    for (std::size_t i = 0; i < metadata->size(); ++i) {
      auto table = (*metadata)[i];
      if (table && table->name == name) {
        return table;
      }
    }
    return nullptr;
  };

  auto basic_table = find_table("test_worker_basic");
  REQUIRE(basic_table != nullptr);
  REQUIRE(basic_table->columns.size() == 4);
  REQUIRE(basic_table->engine == "");

  auto indexed_table = find_table("test_worker_indexed");
  REQUIRE(indexed_table != nullptr);
  REQUIRE(indexed_table->columns.size() == 4);
  REQUIRE(indexed_table->indexes.size() ==
          3); // unique constraint + 2 explicit indexes

  auto find_column = [](const metadata::table_cptr &table,
                        const std::string &name) -> const metadata::Column * {
    auto it = std::find_if(
        table->columns.begin(), table->columns.end(),
        [&name](const metadata::Column &col) { return col.name == name; });
    return it != table->columns.end() ? &(*it) : nullptr;
  };

  auto id_col = find_column(basic_table, "id");
  REQUIRE(id_col != nullptr);
  REQUIRE(id_col->primary_key == true);
  REQUIRE(id_col->auto_increment == true);
  REQUIRE(id_col->nullable == false);

  auto find_index = [](const metadata::table_cptr &table,
                       const std::string &name) -> const metadata::Index * {
    auto it = std::find_if(
        table->indexes.begin(), table->indexes.end(),
        [&name](const metadata::Index &idx) { return idx.name == name; });
    return it != table->indexes.end() ? &(*it) : nullptr;
  };

  auto desc_index = find_index(indexed_table, "idx_worker_age_desc");
  REQUIRE(desc_index != nullptr);
  REQUIRE(desc_index->fields.size() == 2);
  REQUIRE(desc_index->fields[0].column_name == "name");
  REQUIRE(desc_index->fields[1].column_name == "age");
  REQUIRE(desc_index->fields[0].ordering == metadata::IndexOrdering::asc);
  REQUIRE(desc_index->fields[1].ordering == metadata::IndexOrdering::desc);
}

TEST_CASE_METHOD(WorkerSchemaDiscoveryFixture,
                 "Worker - Schema discovery with partitioned table",
                 "[worker_schema_discovery]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_worker_partitioned (
            id SERIAL,
            partition_key INT,
            data TEXT
        ) PARTITION BY RANGE (partition_key)
    )")
      .maybeThrow();

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_worker_partitioned_p0 PARTITION OF test_worker_partitioned 
        FOR VALUES FROM (0) TO (1000)
    )")
      .maybeThrow();

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_worker_partitioned_p1 PARTITION OF test_worker_partitioned 
        FOR VALUES FROM (1000) TO (2000)
    )")
      .maybeThrow();

  sql_variant::ServerParams params{"sql_tests",   "127.0.0.1", "",
                                   "stormweaver", "",          25432};

  SqlFactory::on_connect_t empty_callback;
  SqlFactory factory(params, empty_callback);

  auto logger =
      spdlog::get("test") ? spdlog::get("test") : spdlog::default_logger();
  LuaContext luaCtx(logger);
  auto metadata = std::make_shared<metadata::Metadata>();
  WorkloadParams wp;

  auto worker = std::make_unique<Worker>(
      "test-worker-partitioned",
      [&factory, &luaCtx]() {
        return factory.connect("test-worker-partitioned", luaCtx);
      },
      wp, metadata);

  worker->discover_existing_schema();

  REQUIRE(metadata->size() == 1);

  auto table = (*metadata)[0];
  REQUIRE(table != nullptr);
  REQUIRE(table->name == "test_worker_partitioned");

  REQUIRE(table->partitioning.has_value());
  REQUIRE(table->partitioning->ranges.size() == 2);

  bool found_p0 = false, found_p1 = false;
  for (const auto &range : table->partitioning->ranges) {
    if (range.rangebase == 0)
      found_p0 = true;
    if (range.rangebase == 1)
      found_p1 = true;
  }
  REQUIRE(found_p0);
  REQUIRE(found_p1);
}

TEST_CASE_METHOD(WorkerSchemaDiscoveryFixture,
                 "Worker - Schema discovery with empty database",
                 "[worker_schema_discovery]") {
  REQUIRE(sqlConnection != nullptr);

  sql_variant::ServerParams params{"sql_tests",   "127.0.0.1", "",
                                   "stormweaver", "",          25432};

  SqlFactory::on_connect_t empty_callback;
  SqlFactory factory(params, empty_callback);

  auto logger =
      spdlog::get("test") ? spdlog::get("test") : spdlog::default_logger();
  LuaContext luaCtx(logger);
  auto metadata = std::make_shared<metadata::Metadata>();
  WorkloadParams wp;

  auto worker = std::make_unique<Worker>(
      "test-worker-empty",
      [&factory, &luaCtx]() {
        return factory.connect("test-worker-empty", luaCtx);
      },
      wp, metadata);

  worker->discover_existing_schema();

  REQUIRE(metadata->size() == 0);
}

TEST_CASE_METHOD(WorkerSchemaDiscoveryFixture,
                 "Worker - Schema discovery successful workflow",
                 "[worker_schema_discovery]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_worker_simple (
            id SERIAL PRIMARY KEY,
            name VARCHAR(100)
        )
    )")
      .maybeThrow();

  sql_variant::ServerParams params{"sql_tests",   "127.0.0.1", "",
                                   "stormweaver", "",          25432};

  SqlFactory::on_connect_t empty_callback;
  SqlFactory factory(params, empty_callback);

  auto logger =
      spdlog::get("test") ? spdlog::get("test") : spdlog::default_logger();
  LuaContext luaCtx(logger);
  auto metadata = std::make_shared<metadata::Metadata>();
  WorkloadParams wp;

  auto worker = std::make_unique<Worker>(
      "test-worker-simple",
      [&factory, &luaCtx]() {
        return factory.connect("test-worker-simple", luaCtx);
      },
      wp, metadata);

  REQUIRE_NOTHROW(worker->discover_existing_schema());

  REQUIRE(metadata->size() == 1);
  auto table = (*metadata)[0];
  REQUIRE(table != nullptr);
  REQUIRE(table->name == "test_worker_simple");
}

TEST_CASE("Worker - Reset metadata functionality", "[worker_reset_metadata]") {
  sql_variant::ServerParams params{"sql_tests",   "127.0.0.1", "",
                                   "stormweaver", "",          25432};

  SqlFactory::on_connect_t empty_callback;
  SqlFactory factory(params, empty_callback);

  auto logger =
      spdlog::get("test") ? spdlog::get("test") : spdlog::default_logger();
  LuaContext luaCtx(logger);
  auto metadata = std::make_shared<metadata::Metadata>();
  WorkloadParams wp;

  auto worker = std::make_unique<Worker>(
      "test-worker-reset",
      [&factory, &luaCtx]() {
        return factory.connect("test-worker-reset", luaCtx);
      },
      wp, metadata);

  auto connection = worker->sql_connection();
  REQUIRE(connection != nullptr);

  connection
      ->executeQuery(R"(
        CREATE TABLE IF NOT EXISTS test_reset_table (
            id SERIAL PRIMARY KEY,
            name VARCHAR(100)
        )
    )")
      .maybeThrow();

  worker->discover_existing_schema();
  REQUIRE(metadata->size() >= 1);

  worker->reset_metadata();
  REQUIRE(metadata->size() == 0);

  worker->discover_existing_schema();
  REQUIRE(metadata->size() >= 1);
}

TEST_CASE("Worker - Metadata validation functionality",
          "[worker_validate_metadata]") {
  // Note: This test uses a different port to avoid conflicts with other tests
  sql_variant::ServerParams params{"sql_tests",   "127.0.0.1", "",
                                   "stormweaver", "",          25432};

  SqlFactory::on_connect_t empty_callback;
  SqlFactory factory(params, empty_callback);

  auto logger =
      spdlog::get("test") ? spdlog::get("test") : spdlog::default_logger();
  LuaContext luaCtx(logger);
  auto metadata = std::make_shared<metadata::Metadata>();
  WorkloadParams wp;

  auto worker = std::make_unique<Worker>(
      "test-worker-validate",
      [&factory, &luaCtx]() {
        return factory.connect("test-worker-validate", luaCtx);
      },
      wp, metadata);

  auto connection = worker->sql_connection();
  REQUIRE(connection != nullptr);

  connection
      ->executeQuery(R"(
        CREATE TABLE IF NOT EXISTS test_validation_table (
            id SERIAL PRIMARY KEY,
            name VARCHAR(100) NOT NULL
        )
    )")
      .maybeThrow();

  worker->discover_existing_schema();
  REQUIRE(metadata->size() >= 1);

  REQUIRE(worker->validate_metadata());
}
