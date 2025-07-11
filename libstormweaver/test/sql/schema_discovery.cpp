#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "schema_discovery.hpp"
#include "sql.hpp"

using namespace schema_discovery;

// Fixture to ensure clean database state for each test
class SchemaDiscoveryFixture {
public:
  SchemaDiscoveryFixture() {
    // Recreate public schema to ensure clean state
    sqlConnection->executeQuery("DROP SCHEMA IF EXISTS public CASCADE")
        .maybeThrow();
    sqlConnection->executeQuery("CREATE SCHEMA public").maybeThrow();
    sqlConnection->executeQuery("GRANT ALL ON SCHEMA public TO public")
        .maybeThrow();
  }
};

TEST_CASE_METHOD(SchemaDiscoveryFixture,
                 "SchemaDiscovery - Basic table discovery",
                 "[schema_discovery]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_basic_table (
            id SERIAL PRIMARY KEY,
            name VARCHAR(50) NOT NULL,
            age INT,
            active BOOLEAN DEFAULT TRUE
        )
    )")
      .maybeThrow();

  SchemaDiscovery discovery(sqlConnection.get());
  auto tables = discovery.discoverTables();

  bool found_test_table = false;
  for (const auto &table : tables) {
    if (table.name == "test_basic_table") {
      found_test_table = true;
      REQUIRE(table.table_type == metadata::Table::Type::normal);
      REQUIRE(table.access_method == "heap");
      REQUIRE_FALSE(table.is_partition);
      break;
    }
  }

  REQUIRE(found_test_table);
}

TEST_CASE_METHOD(SchemaDiscoveryFixture, "SchemaDiscovery - Column discovery",
                 "[schema_discovery]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_columns (
            id SERIAL PRIMARY KEY,
            name VARCHAR(100) NOT NULL,
            description TEXT,
            price REAL,
            active BOOLEAN DEFAULT TRUE,
            data BYTEA,
            fixed_char CHAR(10)
        )
    )")
      .maybeThrow();

  SchemaDiscovery discovery(sqlConnection.get());
  auto columns = discovery.discoverColumns("test_columns");

  REQUIRE(columns.size() == 7);

  auto find_column =
      [&columns](const std::string &name) -> const DiscoveredColumn * {
    auto it = std::find_if(
        columns.begin(), columns.end(),
        [&name](const DiscoveredColumn &col) { return col.name == name; });
    return it != columns.end() ? &(*it) : nullptr;
  };

  auto id_col = find_column("id");
  REQUIRE(id_col != nullptr);
  REQUIRE(id_col->data_type == metadata::ColumnType::INT);
  REQUIRE(id_col->is_serial == true);
  REQUIRE(id_col->not_null == true);

  auto name_col = find_column("name");
  REQUIRE(name_col != nullptr);
  REQUIRE(name_col->data_type == metadata::ColumnType::VARCHAR);
  REQUIRE(name_col->length == 100);
  REQUIRE(name_col->not_null == true);

  auto desc_col = find_column("description");
  REQUIRE(desc_col != nullptr);
  REQUIRE(desc_col->data_type == metadata::ColumnType::TEXT);
  REQUIRE(desc_col->not_null == false);

  auto price_col = find_column("price");
  REQUIRE(price_col != nullptr);
  REQUIRE(price_col->data_type == metadata::ColumnType::REAL);

  auto active_col = find_column("active");
  REQUIRE(active_col != nullptr);
  REQUIRE(active_col->data_type == metadata::ColumnType::BOOL);
  REQUIRE_THAT(active_col->default_value,
               Catch::Matchers::ContainsSubstring("true"));

  auto data_col = find_column("data");
  REQUIRE(data_col != nullptr);
  REQUIRE(data_col->data_type == metadata::ColumnType::BYTEA);

  auto char_col = find_column("fixed_char");
  REQUIRE(char_col != nullptr);
  REQUIRE(char_col->data_type == metadata::ColumnType::CHAR);
  REQUIRE(char_col->length == 10);
}

TEST_CASE_METHOD(SchemaDiscoveryFixture, "SchemaDiscovery - Index discovery",
                 "[schema_discovery]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_indexes (
            id SERIAL PRIMARY KEY,
            email VARCHAR(255) UNIQUE,
            name VARCHAR(100),
            age INT
        )
    )")
      .maybeThrow();

  sqlConnection->executeQuery("CREATE INDEX idx_name ON test_indexes (name)")
      .maybeThrow();
  sqlConnection
      ->executeQuery(
          "CREATE INDEX idx_name_age_desc ON test_indexes (name, age DESC)")
      .maybeThrow();

  SchemaDiscovery discovery(sqlConnection.get());
  auto indexes = discovery.discoverIndexes("test_indexes");

  // Should have 3 indexes (unique constraint creates an index, plus our 2)
  REQUIRE(indexes.size() == 3);

  auto find_index =
      [&indexes](const std::string &name) -> const DiscoveredIndex * {
    auto it = std::find_if(
        indexes.begin(), indexes.end(),
        [&name](const DiscoveredIndex &idx) { return idx.name == name; });
    return it != indexes.end() ? &(*it) : nullptr;
  };

  auto unique_idx = find_index("test_indexes_email_key");
  REQUIRE(unique_idx != nullptr);
  REQUIRE(unique_idx->is_unique == true);
  REQUIRE(unique_idx->column_names.size() == 1);
  REQUIRE(unique_idx->column_names[0] == "email");

  auto name_idx = find_index("idx_name");
  REQUIRE(name_idx != nullptr);
  REQUIRE(name_idx->is_unique == false);
  REQUIRE(name_idx->column_names.size() == 1);
  REQUIRE(name_idx->column_names[0] == "name");

  auto composite_idx = find_index("idx_name_age_desc");
  REQUIRE(composite_idx != nullptr);
  REQUIRE(composite_idx->column_names.size() == 2);
  REQUIRE(composite_idx->column_names[0] == "name");
  REQUIRE(composite_idx->column_names[1] == "age");
  REQUIRE(composite_idx->orderings.size() == 2);
  REQUIRE(composite_idx->orderings[0] == metadata::IndexOrdering::asc);
  REQUIRE(composite_idx->orderings[1] == metadata::IndexOrdering::desc);
}

TEST_CASE_METHOD(SchemaDiscoveryFixture,
                 "SchemaDiscovery - Constraint discovery",
                 "[schema_discovery]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_constraints (
            id SERIAL PRIMARY KEY,
            email VARCHAR(255) UNIQUE,
            age INT CHECK (age >= 0 AND age <= 150),
            status VARCHAR(20) CHECK (status IN ('active', 'inactive'))
        )
    )")
      .maybeThrow();

  SchemaDiscovery discovery(sqlConnection.get());
  auto constraints = discovery.discoverConstraints("test_constraints");

  // Should have primary key, unique, and check constraints
  REQUIRE(constraints.size() >= 3);

  auto find_constraint = [&constraints](schema_discovery::ConstraintType type)
      -> const DiscoveredConstraint * {
    auto it = std::find_if(
        constraints.begin(), constraints.end(),
        [&type](const DiscoveredConstraint &c) { return c.type == type; });
    return it != constraints.end() ? &(*it) : nullptr;
  };

  auto pk_constraint =
      find_constraint(schema_discovery::ConstraintType::primary_key);
  REQUIRE(pk_constraint != nullptr);
  REQUIRE(pk_constraint->columns.size() == 1);
  REQUIRE(pk_constraint->columns[0] == "id");

  auto unique_constraint =
      find_constraint(schema_discovery::ConstraintType::unique);
  REQUIRE(unique_constraint != nullptr);
  REQUIRE(unique_constraint->columns.size() == 1);
  REQUIRE(unique_constraint->columns[0] == "email");

  auto check_constraints =
      std::count_if(constraints.begin(), constraints.end(),
                    [](const DiscoveredConstraint &c) {
                      return c.type == schema_discovery::ConstraintType::check;
                    });
  REQUIRE(check_constraints >= 2);
}

TEST_CASE_METHOD(SchemaDiscoveryFixture,
                 "SchemaDiscovery - Partitioned table discovery",
                 "[schema_discovery]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_partitioned (
            id SERIAL,
            partition_key INT,
            data TEXT
        ) PARTITION BY RANGE (partition_key)
    )")
      .maybeThrow();

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_partitioned_p0 PARTITION OF test_partitioned 
        FOR VALUES FROM (0) TO (1000)
    )")
      .maybeThrow();

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE test_partitioned_p1 PARTITION OF test_partitioned 
        FOR VALUES FROM (1000) TO (2000)
    )")
      .maybeThrow();

  SchemaDiscovery discovery(sqlConnection.get());

  auto tables = discovery.discoverTables();
  bool found_partitioned = false;
  for (const auto &table : tables) {
    if (table.name == "test_partitioned") {
      found_partitioned = true;
      REQUIRE(table.table_type == metadata::Table::Type::partitioned);
      REQUIRE(table.partition_type == schema_discovery::PartitionType::range);
      break;
    }
  }
  REQUIRE(found_partitioned);

  auto partitions = discovery.discoverPartitions("test_partitioned");
  REQUIRE(partitions.size() == 2);

  auto find_partition =
      [&partitions](const std::string &name) -> const DiscoveredPartition * {
    auto it = std::find_if(
        partitions.begin(), partitions.end(),
        [&name](const DiscoveredPartition &p) { return p.name == name; });
    return it != partitions.end() ? &(*it) : nullptr;
  };

  auto p0 = find_partition("test_partitioned_p0");
  REQUIRE(p0 != nullptr);
  REQUIRE_THAT(p0->partition_bound, Catch::Matchers::ContainsSubstring("0"));
  REQUIRE_THAT(p0->partition_bound, Catch::Matchers::ContainsSubstring("1000"));

  auto p1 = find_partition("test_partitioned_p1");
  REQUIRE(p1 != nullptr);
  REQUIRE_THAT(p1->partition_bound, Catch::Matchers::ContainsSubstring("1000"));
  REQUIRE_THAT(p1->partition_bound, Catch::Matchers::ContainsSubstring("2000"));
}

TEST_CASE_METHOD(SchemaDiscoveryFixture,
                 "SchemaDiscovery - Foreign key discovery",
                 "[schema_discovery]") {
  REQUIRE(sqlConnection != nullptr);

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE parent_table (
            id SERIAL PRIMARY KEY,
            name VARCHAR(100) NOT NULL
        )
    )")
      .maybeThrow();

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE child_table (
            id SERIAL PRIMARY KEY,
            parent_id INT NOT NULL,
            description TEXT,
            FOREIGN KEY (parent_id) REFERENCES parent_table(id)
        )
    )")
      .maybeThrow();

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE parent_composite (
            tenant_id INT,
            entity_id INT,
            name VARCHAR(100),
            PRIMARY KEY (tenant_id, entity_id)
        )
    )")
      .maybeThrow();

  sqlConnection
      ->executeQuery(R"(
        CREATE TABLE child_composite (
            id SERIAL PRIMARY KEY,
            parent_tenant_id INT NOT NULL,
            parent_entity_id INT NOT NULL,
            description TEXT,
            FOREIGN KEY (parent_tenant_id, parent_entity_id) REFERENCES parent_composite(tenant_id, entity_id)
        )
    )")
      .maybeThrow();

  SchemaDiscovery discovery(sqlConnection.get());

  auto child_constraints = discovery.discoverConstraints("child_table");
  REQUIRE(child_constraints.size() >= 2); // at least PK and FK

  auto find_constraint =
      [&child_constraints](schema_discovery::ConstraintType type)
      -> const DiscoveredConstraint * {
    auto it = std::find_if(
        child_constraints.begin(), child_constraints.end(),
        [&type](const DiscoveredConstraint &c) { return c.type == type; });
    return it != child_constraints.end() ? &(*it) : nullptr;
  };

  auto fk_constraint =
      find_constraint(schema_discovery::ConstraintType::foreign_key);
  REQUIRE(fk_constraint != nullptr);
  REQUIRE(fk_constraint->columns.size() == 1);
  REQUIRE(fk_constraint->columns[0] == "parent_id");
  REQUIRE(fk_constraint->referenced_table == "parent_table");
  REQUIRE(fk_constraint->referenced_columns.size() == 1);
  REQUIRE(fk_constraint->referenced_columns[0] == "id");

  auto composite_constraints = discovery.discoverConstraints("child_composite");
  REQUIRE(composite_constraints.size() >= 2); // at least PK and FK

  auto composite_fk = std::find_if(
      composite_constraints.begin(), composite_constraints.end(),
      [](const DiscoveredConstraint &c) {
        return c.type == schema_discovery::ConstraintType::foreign_key;
      });
  REQUIRE(composite_fk != composite_constraints.end());
  REQUIRE(composite_fk->columns.size() == 2);
  REQUIRE(std::find(composite_fk->columns.begin(), composite_fk->columns.end(),
                    "parent_tenant_id") != composite_fk->columns.end());
  REQUIRE(std::find(composite_fk->columns.begin(), composite_fk->columns.end(),
                    "parent_entity_id") != composite_fk->columns.end());
  REQUIRE(composite_fk->referenced_table == "parent_composite");
  REQUIRE(composite_fk->referenced_columns.size() == 2);
  REQUIRE(std::find(composite_fk->referenced_columns.begin(),
                    composite_fk->referenced_columns.end(),
                    "tenant_id") != composite_fk->referenced_columns.end());
  REQUIRE(std::find(composite_fk->referenced_columns.begin(),
                    composite_fk->referenced_columns.end(),
                    "entity_id") != composite_fk->referenced_columns.end());
}

TEST_CASE_METHOD(SchemaDiscoveryFixture, "SchemaDiscovery - Empty database",
                 "[schema_discovery]") {
  REQUIRE(sqlConnection != nullptr);

  SchemaDiscovery discovery(sqlConnection.get());
  auto tables = discovery.discoverTables();

  REQUIRE(tables.empty());
}