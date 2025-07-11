#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "metadata_populator.hpp"
#include "schema_discovery.hpp"
#include "sql.hpp"

using namespace metadata_populator;
using namespace schema_discovery;

// Fixture to ensure clean database state for each test
class MetadataPopulatorFixture {
public:
  MetadataPopulatorFixture() {
    // Recreate public schema to ensure clean state
    sqlConnection->executeQuery("DROP SCHEMA IF EXISTS public CASCADE")
        .maybeThrow();
    sqlConnection->executeQuery("CREATE SCHEMA public").maybeThrow();
    sqlConnection->executeQuery("GRANT ALL ON SCHEMA public TO public")
        .maybeThrow();
  }
};

TEST_CASE_METHOD(MetadataPopulatorFixture,
                 "MetadataPopulator - Basic table conversion",
                 "[metadata_populator]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_populator_basic (
            id SERIAL PRIMARY KEY,
            name VARCHAR(100) NOT NULL,
            price REAL,
            active BOOLEAN DEFAULT TRUE
        )
    )")
      .maybeThrow();

  metadata::Metadata metadata;
  SchemaDiscovery discovery(sqlConnection.get());
  MetadataPopulator populator(metadata);

  populator.populateFromExistingDatabase(discovery);

  REQUIRE(metadata.size() == 1);

  auto table = metadata[0];
  REQUIRE(table != nullptr);
  REQUIRE(table->name == "test_populator_basic");
  REQUIRE(table->engine == "");
  REQUIRE(table->columns.size() == 4);

  auto find_column =
      [&table](const std::string &name) -> const metadata::Column * {
    auto it = std::find_if(
        table->columns.begin(), table->columns.end(),
        [&name](const metadata::Column &col) { return col.name == name; });
    return it != table->columns.end() ? &(*it) : nullptr;
  };

  auto id_col = find_column("id");
  REQUIRE(id_col != nullptr);
  REQUIRE(id_col->type == metadata::ColumnType::INT);
  REQUIRE(id_col->auto_increment == true);
  REQUIRE(id_col->primary_key == true);
  REQUIRE(id_col->nullable == false);
  // SERIAL columns should have empty default values (not the complex nextval
  // expression)
  REQUIRE(id_col->default_value.empty());

  auto name_col = find_column("name");
  REQUIRE(name_col != nullptr);
  REQUIRE(name_col->type == metadata::ColumnType::VARCHAR);
  REQUIRE(name_col->length == 100);
  REQUIRE(name_col->nullable == false);

  auto price_col = find_column("price");
  REQUIRE(price_col != nullptr);
  REQUIRE(price_col->type == metadata::ColumnType::REAL);
  REQUIRE(price_col->nullable == true);

  auto active_col = find_column("active");
  REQUIRE(active_col != nullptr);
  REQUIRE(active_col->type == metadata::ColumnType::BOOL);
  REQUIRE(active_col->nullable == true);
  REQUIRE_FALSE(active_col->default_value.empty());
}

TEST_CASE_METHOD(MetadataPopulatorFixture,
                 "MetadataPopulator - Index conversion",
                 "[metadata_populator]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_populator_indexes (
            id SERIAL PRIMARY KEY,
            email VARCHAR(255) UNIQUE,
            name VARCHAR(100),
            age INT
        )
    )")
      .maybeThrow();

  sqlConnection
      ->executeQuery("CREATE INDEX idx_name ON test_populator_indexes (name)")
      .maybeThrow();
  sqlConnection
      ->executeQuery("CREATE INDEX idx_name_age ON test_populator_indexes "
                     "(name, age DESC)")
      .maybeThrow();

  metadata::Metadata metadata;
  SchemaDiscovery discovery(sqlConnection.get());
  MetadataPopulator populator(metadata);

  populator.populateFromExistingDatabase(discovery);

  REQUIRE(metadata.size() == 1);

  auto table = metadata[0];
  REQUIRE(table != nullptr);
  REQUIRE(table->indexes.size() == 3); // unique constraint + 2 explicit indexes

  auto find_index =
      [&table](const std::string &name) -> const metadata::Index * {
    auto it = std::find_if(
        table->indexes.begin(), table->indexes.end(),
        [&name](const metadata::Index &idx) { return idx.name == name; });
    return it != table->indexes.end() ? &(*it) : nullptr;
  };

  auto unique_idx = find_index("test_populator_indexes_email_key");
  REQUIRE(unique_idx != nullptr);
  REQUIRE(unique_idx->unique == true);
  REQUIRE(unique_idx->fields.size() == 1);
  REQUIRE(unique_idx->fields[0].column_name == "email");

  auto name_idx = find_index("idx_name");
  REQUIRE(name_idx != nullptr);
  REQUIRE(name_idx->unique == false);
  REQUIRE(name_idx->fields.size() == 1);
  REQUIRE(name_idx->fields[0].column_name == "name");

  auto composite_idx = find_index("idx_name_age");
  REQUIRE(composite_idx != nullptr);
  REQUIRE(composite_idx->fields.size() == 2);
  REQUIRE(composite_idx->fields[0].column_name == "name");
  REQUIRE(composite_idx->fields[1].column_name == "age");
  REQUIRE(composite_idx->fields[0].ordering == metadata::IndexOrdering::asc);
  REQUIRE(composite_idx->fields[1].ordering == metadata::IndexOrdering::desc);
}

TEST_CASE_METHOD(MetadataPopulatorFixture, "MetadataPopulator - Type mapping",
                 "[metadata_populator]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_populator_types (
            int_col INT,
            bigint_col BIGINT,
            varchar_col VARCHAR(50),
            char_col CHAR(10),
            text_col TEXT,
            real_col REAL,
            double_col DOUBLE PRECISION,
            bool_col BOOLEAN,
            bytea_col BYTEA
        )
    )")
      .maybeThrow();

  metadata::Metadata metadata;
  SchemaDiscovery discovery(sqlConnection.get());
  MetadataPopulator populator(metadata);

  populator.populateFromExistingDatabase(discovery);

  REQUIRE(metadata.size() == 1);

  auto table = metadata[0];
  REQUIRE(table != nullptr);
  REQUIRE(table->columns.size() == 9);

  auto find_column =
      [&table](const std::string &name) -> const metadata::Column * {
    auto it = std::find_if(
        table->columns.begin(), table->columns.end(),
        [&name](const metadata::Column &col) { return col.name == name; });
    return it != table->columns.end() ? &(*it) : nullptr;
  };

  REQUIRE(find_column("int_col")->type == metadata::ColumnType::INT);
  REQUIRE(find_column("bigint_col")->type == metadata::ColumnType::INT);
  REQUIRE(find_column("varchar_col")->type == metadata::ColumnType::VARCHAR);
  REQUIRE(find_column("char_col")->type == metadata::ColumnType::CHAR);
  REQUIRE(find_column("text_col")->type == metadata::ColumnType::TEXT);
  REQUIRE(find_column("real_col")->type == metadata::ColumnType::REAL);
  REQUIRE(find_column("double_col")->type == metadata::ColumnType::REAL);
  REQUIRE(find_column("bool_col")->type == metadata::ColumnType::BOOL);
  REQUIRE(find_column("bytea_col")->type == metadata::ColumnType::BYTEA);

  REQUIRE(find_column("varchar_col")->length == 50);
  REQUIRE(find_column("char_col")->length == 10);
}

TEST_CASE_METHOD(MetadataPopulatorFixture,
                 "MetadataPopulator - Partitioned table conversion",
                 "[metadata_populator]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_populator_partitioned (
            id SERIAL,
            partition_key INT,
            data TEXT
        ) PARTITION BY RANGE (partition_key)
    )")
      .maybeThrow();

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_populator_partitioned_p0 PARTITION OF test_populator_partitioned 
        FOR VALUES FROM (0) TO (1000)
    )")
      .maybeThrow();

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_populator_partitioned_p1 PARTITION OF test_populator_partitioned 
        FOR VALUES FROM (1000) TO (2000)
    )")
      .maybeThrow();

  metadata::Metadata metadata;
  SchemaDiscovery discovery(sqlConnection.get());
  MetadataPopulator populator(metadata);

  populator.populateFromExistingDatabase(discovery);

  REQUIRE(metadata.size() == 1);

  auto table = metadata[0];
  REQUIRE(table != nullptr);
  REQUIRE(table->name == "test_populator_partitioned");

  REQUIRE(table->partitioning.has_value());
  REQUIRE(table->partitioning->ranges.size() == 2);

  // Verify partition range bases were parsed correctly
  bool found_p0 = false, found_p1 = false;
  for (const auto &range : table->partitioning->ranges) {
    if (range.rangebase == 0)
      found_p0 = true;
    if (range.rangebase == 1)
      found_p1 = true;
  }
  REQUIRE(found_p0);
  REQUIRE(found_p1);

  auto find_column =
      [&table](const std::string &name) -> const metadata::Column * {
    auto it = std::find_if(
        table->columns.begin(), table->columns.end(),
        [&name](const metadata::Column &col) { return col.name == name; });
    return it != table->columns.end() ? &(*it) : nullptr;
  };

  auto partition_key_col = find_column("partition_key");
  REQUIRE(partition_key_col != nullptr);
  REQUIRE(partition_key_col->partition_key == true);

  auto id_col = find_column("id");
  REQUIRE(id_col != nullptr);
  REQUIRE(id_col->partition_key == false);

  auto data_col = find_column("data");
  REQUIRE(data_col != nullptr);
  REQUIRE(data_col->partition_key == false);
}

TEST_CASE_METHOD(MetadataPopulatorFixture,
                 "MetadataPopulator - Multiple tables",
                 "[metadata_populator]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_multi_1 (
            id SERIAL PRIMARY KEY,
            name VARCHAR(50)
        )
    )")
      .maybeThrow();

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_multi_2 (
            id SERIAL PRIMARY KEY,
            description TEXT,
            price REAL
        )
    )")
      .maybeThrow();

  metadata::Metadata metadata;
  SchemaDiscovery discovery(sqlConnection.get());
  MetadataPopulator populator(metadata);

  populator.populateFromExistingDatabase(discovery);

  REQUIRE(metadata.size() == 2);
  bool found_table1 = false, found_table2 = false;
  for (std::size_t i = 0; i < metadata.size(); ++i) {
    auto table = metadata[i];
    if (table && table->name == "test_multi_1") {
      found_table1 = true;
      REQUIRE(table->columns.size() == 2);
    } else if (table && table->name == "test_multi_2") {
      found_table2 = true;
      REQUIRE(table->columns.size() == 3);
    }
  }

  REQUIRE(found_table1);
  REQUIRE(found_table2);
}

TEST_CASE_METHOD(MetadataPopulatorFixture,
                 "MetadataPopulator - Foreign key population",
                 "[metadata_populator]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE orders (
            id SERIAL PRIMARY KEY,
            customer_name VARCHAR(100) NOT NULL,
            total REAL
        )
    )")
      .maybeThrow();

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE order_items (
            id SERIAL PRIMARY KEY,
            order_id INT NOT NULL,
            product_name VARCHAR(200),
            quantity INT DEFAULT 1,
            FOREIGN KEY (order_id) REFERENCES orders(id)
        )
    )")
      .maybeThrow();

  metadata::Metadata metadata;
  MetadataPopulator populator(metadata);
  SchemaDiscovery discovery(sqlConnection.get());

  populator.populateFromExistingDatabase(discovery);

  REQUIRE(metadata.size() == 2);
  const metadata::Table *order_items_table = nullptr;
  for (std::size_t i = 0; i < metadata.size(); ++i) {
    auto table = metadata[i];
    if (table && table->name == "order_items") {
      order_items_table = table.get();
      break;
    }
  }

  REQUIRE(order_items_table != nullptr);
  REQUIRE(order_items_table->columns.size() == 4);

  auto order_id_col = std::find_if(
      order_items_table->columns.begin(), order_items_table->columns.end(),
      [](const metadata::Column &col) { return col.name == "order_id"; });
  REQUIRE(order_id_col != order_items_table->columns.end());
  REQUIRE(order_id_col->foreign_key_references == "orders");
  REQUIRE_FALSE(order_id_col->nullable); // NOT NULL
  REQUIRE(order_id_col->type == metadata::ColumnType::INT);
  auto id_col = std::find_if(
      order_items_table->columns.begin(), order_items_table->columns.end(),
      [](const metadata::Column &col) { return col.name == "id"; });
  REQUIRE(id_col != order_items_table->columns.end());
  REQUIRE(id_col->foreign_key_references.empty());
  REQUIRE(id_col->primary_key == true);
}

TEST_CASE_METHOD(MetadataPopulatorFixture,
                 "MetadataPopulator - Foreign key to partitioned table",
                 "[metadata_populator]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE partitioned_orders (
            id INT PRIMARY KEY,
            customer_id INT
        ) PARTITION BY RANGE (id)
    )")
      .maybeThrow();

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE partitioned_orders_2023 PARTITION OF partitioned_orders 
        FOR VALUES FROM (1000) TO (2000)
    )")
      .maybeThrow();

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE order_details (
            id SERIAL PRIMARY KEY,
            order_id INT REFERENCES partitioned_orders(id),
            product_name VARCHAR(100)
        )
    )")
      .maybeThrow();

  metadata::Metadata metadata;
  SchemaDiscovery discovery(sqlConnection.get());
  MetadataPopulator populator(metadata);

  populator.populateFromExistingDatabase(discovery);

  REQUIRE(metadata.size() == 2);
  const metadata::Table *order_details_table = nullptr;
  for (std::size_t i = 0; i < metadata.size(); ++i) {
    auto table = metadata[i];
    if (table && table->name == "order_details") {
      order_details_table = table.get();
      break;
    }
  }

  REQUIRE(order_details_table != nullptr);

  auto order_id_col = std::find_if(
      order_details_table->columns.begin(), order_details_table->columns.end(),
      [](const metadata::Column &col) { return col.name == "order_id"; });
  REQUIRE(order_id_col != order_details_table->columns.end());

  // Foreign key should reference the parent table, not the partition
  REQUIRE(order_id_col->foreign_key_references == "partitioned_orders");
  REQUIRE(order_id_col->foreign_key_references != "partitioned_orders_2023");
}

TEST_CASE_METHOD(MetadataPopulatorFixture, "MetadataPopulator - Empty database",
                 "[metadata_populator]") {
  REQUIRE(sqlConnection != nullptr);

  metadata::Metadata metadata;
  SchemaDiscovery discovery(sqlConnection.get());
  MetadataPopulator populator(metadata);

  populator.populateFromExistingDatabase(discovery);

  REQUIRE(metadata.size() == 0);
}
