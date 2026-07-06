#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "sql.hpp"
#include "sql_variant/mysql.hpp"
#include "sql_variant/postgresql.hpp"
#include "workload.hpp"

// Fixture to ensure clean database state for each test
class WorkerSchemaDiscoveryFixture {
public:
  WorkerSchemaDiscoveryFixture() { testutil::resetTestSchema(); }

  ~WorkerSchemaDiscoveryFixture() {
    // Clean up is automatic via schema recreation in next test
  }
};

static Worker::sql_connector_t make_connector(const std::string &conn_name) {
  sql_variant::ServerParams params = globalConnParams;
  bool mysql = testutil::isMysql();
  return
      [params, mysql, conn_name]() -> std::unique_ptr<sql_variant::LoggedSQL> {
        if (mysql) {
          return std::make_unique<sql_variant::LoggedSQL>(
              std::make_unique<sql_variant::MySQL>(params), conn_name);
        }
        return std::make_unique<sql_variant::LoggedSQL>(
            std::make_unique<sql_variant::PostgreSQL>(params), conn_name);
      };
}

TEST_CASE_METHOD(WorkerSchemaDiscoveryFixture,
                 "Worker - Schema discovery basic workflow",
                 "[worker_schema_discovery]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(fmt::format(
          R"(
        CREATE TABLE test_worker_basic (
            id {} PRIMARY KEY,
            name VARCHAR(100) NOT NULL,
            price REAL,
            active BOOLEAN DEFAULT TRUE
        )
    )",
          testutil::autoIncPk()))
      .maybeThrow();

  sqlConnection
      ->executeQuery(fmt::format(
          R"(
        CREATE TABLE test_worker_indexed (
            id {} PRIMARY KEY,
            email VARCHAR(255) UNIQUE,
            name VARCHAR(100),
            age INT
        )
    )",
          testutil::autoIncPk()))
      .maybeThrow();

  sqlConnection
      ->executeQuery(
          "CREATE INDEX idx_worker_name ON test_worker_indexed (name)")
      .maybeThrow();
  sqlConnection
      ->executeQuery("CREATE INDEX idx_worker_age_desc ON test_worker_indexed "
                     "(name, age DESC)")
      .maybeThrow();

  auto metadata = std::make_shared<metadata::TableRegistry>();
  WorkloadParams wp;

  auto worker = std::make_unique<Worker>(
      "test-worker", make_connector("test-worker"), wp, metadata);

  REQUIRE(metadata->get<metadata::Table>().size() == 0);

  worker->discover_existing_schema();

  REQUIRE(metadata->get<metadata::Table>().size() == 2);

  auto find_table =
      [&metadata](const std::string &name) -> metadata::table_cptr {
    for (auto const &table : metadata->get<metadata::Table>().snapshotAll()) {
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

  if (testutil::isMysql()) {
    sqlConnection
        ->executeQuery(R"(
        CREATE TABLE test_worker_partitioned (
            id INT,
            partition_key INT,
            data TEXT
        ) PARTITION BY RANGE (partition_key) (
            PARTITION test_worker_partitioned_p0 VALUES LESS THAN (1000),
            PARTITION test_worker_partitioned_p1 VALUES LESS THAN (2000)
        )
    )")
        .maybeThrow();
  } else {
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
  }

  auto metadata = std::make_shared<metadata::TableRegistry>();
  WorkloadParams wp;

  auto worker = std::make_unique<Worker>(
      "test-worker-partitioned", make_connector("test-worker-partitioned"), wp,
      metadata);

  worker->discover_existing_schema();

  REQUIRE(metadata->get<metadata::Table>().size() == 1);

  auto tables_snapshot = metadata->get<metadata::Table>().snapshotAll();
  REQUIRE(tables_snapshot.size() == 1);
  auto table = tables_snapshot.at(0);
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

  auto metadata = std::make_shared<metadata::TableRegistry>();
  WorkloadParams wp;

  auto worker = std::make_unique<Worker>(
      "test-worker-empty", make_connector("test-worker-empty"), wp, metadata);

  worker->discover_existing_schema();

  REQUIRE(metadata->get<metadata::Table>().size() == 0);
}

TEST_CASE_METHOD(WorkerSchemaDiscoveryFixture,
                 "Worker - Schema discovery successful workflow",
                 "[worker_schema_discovery]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(fmt::format(
          R"(
        CREATE TABLE test_worker_simple (
            id {} PRIMARY KEY,
            name VARCHAR(100)
        )
    )",
          testutil::autoIncPk()))
      .maybeThrow();

  auto metadata = std::make_shared<metadata::TableRegistry>();
  WorkloadParams wp;

  auto worker = std::make_unique<Worker>(
      "test-worker-simple", make_connector("test-worker-simple"), wp, metadata);

  REQUIRE_NOTHROW(worker->discover_existing_schema());

  REQUIRE(metadata->get<metadata::Table>().size() == 1);
  auto tables_snapshot = metadata->get<metadata::Table>().snapshotAll();
  REQUIRE(tables_snapshot.size() == 1);
  auto table = tables_snapshot.at(0);
  REQUIRE(table != nullptr);
  REQUIRE(table->name == "test_worker_simple");
}

TEST_CASE("Worker - Reset metadata functionality", "[worker_reset_metadata]") {
  auto metadata = std::make_shared<metadata::TableRegistry>();
  WorkloadParams wp;

  auto worker = std::make_unique<Worker>(
      "test-worker-reset", make_connector("test-worker-reset"), wp, metadata);

  auto connection = worker->sql_connection();
  REQUIRE(connection != nullptr);

  connection
      ->executeQuery(fmt::format(
          R"(
        CREATE TABLE IF NOT EXISTS test_reset_table (
            id {} PRIMARY KEY,
            name VARCHAR(100)
        )
    )",
          testutil::autoIncPk()))
      .maybeThrow();

  worker->discover_existing_schema();
  REQUIRE(metadata->get<metadata::Table>().size() >= 1);

  worker->reset_metadata();
  REQUIRE(metadata->get<metadata::Table>().size() == 0);

  worker->discover_existing_schema();
  REQUIRE(metadata->get<metadata::Table>().size() >= 1);
}

TEST_CASE("Worker - Metadata validation functionality",
          "[worker_validate_metadata]") {
  auto metadata = std::make_shared<metadata::TableRegistry>();
  WorkloadParams wp;

  auto worker = std::make_unique<Worker>("test-worker-validate",
                                         make_connector("test-worker-validate"),
                                         wp, metadata);

  auto connection = worker->sql_connection();
  REQUIRE(connection != nullptr);

  connection
      ->executeQuery(fmt::format(
          R"(
        CREATE TABLE IF NOT EXISTS test_validation_table (
            id {} PRIMARY KEY,
            name VARCHAR(100) NOT NULL
        )
    )",
          testutil::autoIncPk()))
      .maybeThrow();

  worker->discover_existing_schema();
  REQUIRE(metadata->get<metadata::Table>().size() >= 1);

  REQUIRE(worker->validate_metadata());
}
