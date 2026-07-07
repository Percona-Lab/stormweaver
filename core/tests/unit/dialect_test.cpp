#include <catch2/catch_test_macros.hpp>

#include "sql_dialect/dialect.hpp"

using namespace metadata;
using namespace sql_dialect;

namespace {

Table makeSimpleTable() {
  Table table;
  table.name = "foo1";

  Column pk;
  pk.name = "id";
  pk.type = ColumnType::INT;
  pk.primary_key = true;
  pk.nullable = false;
  pk.auto_increment = true;
  table.columns.push_back(pk);

  Column col;
  col.name = "col1";
  col.type = ColumnType::VARCHAR;
  col.length = 32;
  table.columns.push_back(col);

  return table;
}

Table makePartitionedTable() {
  Table table;
  table.name = "foo2";

  Column pk;
  pk.name = "id";
  pk.type = ColumnType::INT;
  pk.primary_key = true;
  pk.nullable = false;
  pk.partition_key = true;
  table.columns.push_back(pk);

  Column col;
  col.name = "col1";
  col.type = ColumnType::VARCHAR;
  col.length = 32;
  table.columns.push_back(col);

  table.partitioning = RangePartitioning{};
  table.partitioning->rangeSize = 10000000;

  return table;
}

Table makePartitionedTableWithRanges() {
  Table table = makePartitionedTable();
  table.partitioning->ranges.push_back(RangePartition{.rangebase = 0});
  table.partitioning->ranges.push_back(RangePartition{.rangebase = 1});
  return table;
}

} // namespace

TEST_CASE("pg createTable: serial pk + varchar column", "[dialect]") {
  auto const &dialect = pg_dialect();
  auto table = makeSimpleTable();

  auto sql = dialect.createTable(table, "");

  REQUIRE(sql == "CREATE TABLE foo1 (id SERIAL,\ncol1 VARCHAR(32),\n"
                 "PRIMARY KEY (id)) ;");
}

TEST_CASE("pg createTable: foreign key on columns[1]", "[dialect]") {
  auto const &dialect = pg_dialect();
  auto table = makeSimpleTable();

  auto sql = dialect.createTable(table, "bar1");

  REQUIRE(sql == "CREATE TABLE foo1 (id SERIAL,\n"
                 "col1 VARCHAR(32) REFERENCES bar1 ON DELETE CASCADE,\n"
                 "PRIMARY KEY (id)) ;");
}

TEST_CASE("pg createTable: partitioned table", "[dialect]") {
  auto const &dialect = pg_dialect();
  auto table = makePartitionedTable();

  auto sql = dialect.createTable(table, "");

  REQUIRE(sql == "CREATE TABLE foo2 (id INT,\ncol1 VARCHAR(32),\n"
                 "PRIMARY KEY (id))  PARTITION BY RANGE (id);");
}

TEST_CASE("pg addPartition / dropPartition", "[dialect]") {
  auto const &dialect = pg_dialect();
  auto table = makePartitionedTable();

  REQUIRE(dialect.addPartition(table, 3) ==
          "CREATE TABLE foo2_p3 PARTITION OF foo2 FOR VALUES FROM (30000000) "
          "TO (40000000);");

  REQUIRE(dialect.dropPartition(table, 3) == "DROP TABLE foo2_p3 CASCADE;");

  REQUIRE_FALSE(dialect.partitionsInlineInCreate());
  REQUIRE(dialect.supportsFkOnPartitionedTables());
  REQUIRE(dialect.canDropFkColumn());
  REQUIRE(dialect.maxIndexColumns() == 32);
}

TEST_CASE("pg createIndex: unique + concurrent", "[dialect]") {
  auto const &dialect = pg_dialect();
  Table table = makeSimpleTable();

  Index index;
  index.name = "idx1";
  index.unique = true;
  index.fields.push_back(
      IndexColumn{.column_name = "id", .ordering = IndexOrdering::asc});
  index.fields.push_back(
      IndexColumn{.column_name = "col1", .ordering = IndexOrdering::desc});

  IndexOptions opts;
  opts.concurrent = true;
  opts.only = false;

  auto sql = dialect.createIndex(table, index, opts);

  REQUIRE(sql == "CREATE UNIQUE INDEX CONCURRENTLY idx1 ON  foo1 "
                 "(id ASC, col1 DESC);");
}

TEST_CASE("pg dropIndex", "[dialect]") {
  auto const &dialect = pg_dialect();

  REQUIRE(dialect.dropIndex("foo1", "idx1") == "DROP INDEX idx1;");
}

TEST_CASE("pg alterColumnType", "[dialect]") {
  auto const &dialect = pg_dialect();

  REQUIRE(dialect.alterColumnType("col1", ColumnType::VARCHAR, 32) ==
          "ALTER COLUMN col1 TYPE VARCHAR(32)");
}

TEST_CASE("pg alterStorage", "[dialect]") {
  auto const &dialect = pg_dialect();

  REQUIRE(dialect.alterStorage("heap2") == "SET ACCESS METHOD heap2");
}

TEST_CASE("pg randomRowSubquery", "[dialect]") {
  auto const &dialect = pg_dialect();

  REQUIRE(dialect.randomRowSubquery("foo1", "id", 1) ==
          "(SELECT id FROM foo1 ORDER BY random() LIMIT 1)");
}

TEST_CASE("pg typeName", "[dialect]") {
  auto const &dialect = pg_dialect();

  REQUIRE(dialect.typeName(ColumnType::BYTEA, 0) == "BYTEA");
}

TEST_CASE("dialect_for returns pg dialect", "[dialect]") {
  sql_variant::ServerInfo info{.flavor_ = sql_variant::flavor::postgres,
                               .version = 160000};

  REQUIRE(&sql_dialect::dialect_for(info) == &pg_dialect());
}

TEST_CASE("mysql createTable: auto_increment pk + varchar column",
          "[dialect]") {
  auto const &dialect = mysql_dialect();
  auto table = makeSimpleTable();

  auto sql = dialect.createTable(table, "");

  REQUIRE(sql == "CREATE TABLE foo1 (id INT AUTO_INCREMENT,\n"
                 "col1 VARCHAR(32),\n"
                 "PRIMARY KEY (id)) ;");
}

TEST_CASE("mysql createTable: table-level foreign key", "[dialect]") {
  auto const &dialect = mysql_dialect();
  auto table = makeSimpleTable();

  auto sql = dialect.createTable(table, "bar1");

  REQUIRE(sql == "CREATE TABLE foo1 (id INT AUTO_INCREMENT,\n"
                 "col1 VARCHAR(32),\n"
                 "PRIMARY KEY (id),\n"
                 "FOREIGN KEY (col1) REFERENCES bar1 (id) ON DELETE "
                 "CASCADE) ;");
}

TEST_CASE("mysql createTable: partitioned table inline ranges", "[dialect]") {
  auto const &dialect = mysql_dialect();
  auto table = makePartitionedTableWithRanges();

  auto sql = dialect.createTable(table, "");

  REQUIRE(sql == "CREATE TABLE foo2 (id INT,\ncol1 VARCHAR(32),\n"
                 "PRIMARY KEY (id))  PARTITION BY RANGE (id) "
                 "(PARTITION p0 VALUES LESS THAN (10000000), "
                 "PARTITION p1 VALUES LESS THAN (20000000));");
}

TEST_CASE("mysql addPartition / dropPartition", "[dialect]") {
  auto const &dialect = mysql_dialect();
  auto table = makePartitionedTable();

  REQUIRE(dialect.addPartition(table, 3) ==
          "ALTER TABLE foo2 ADD PARTITION (PARTITION p3 VALUES LESS THAN "
          "(40000000));");

  REQUIRE(dialect.dropPartition(table, 3) ==
          "ALTER TABLE foo2 DROP PARTITION p3;");

  REQUIRE(dialect.partitionsInlineInCreate());
  REQUIRE_FALSE(dialect.supportsFkOnPartitionedTables());
  REQUIRE_FALSE(dialect.canDropFkColumn());
  REQUIRE(dialect.maxIndexColumns() == 16);
  REQUIRE(dialect.maxIndexesPerTable() == 60);
}

TEST_CASE("mysql createIndex: no prefix on plain columns", "[dialect]") {
  auto const &dialect = mysql_dialect();
  Table table = makeSimpleTable();

  Index index;
  index.name = "idx1";
  index.unique = true;
  index.fields.push_back(
      IndexColumn{.column_name = "id", .ordering = IndexOrdering::asc});
  index.fields.push_back(
      IndexColumn{.column_name = "col1", .ordering = IndexOrdering::desc});

  IndexOptions opts;

  auto sql = dialect.createIndex(table, index, opts);

  REQUIRE(sql == "CREATE UNIQUE INDEX idx1 ON foo1 (id ASC, col1 DESC);");
}

TEST_CASE("mysql createIndex: key-prefix on TEXT/BYTEA columns", "[dialect]") {
  auto const &dialect = mysql_dialect();

  Table table;
  table.name = "foo3";

  Column pk;
  pk.name = "id";
  pk.type = ColumnType::INT;
  pk.primary_key = true;
  table.columns.push_back(pk);

  Column blobcol;
  blobcol.name = "data";
  blobcol.type = ColumnType::BYTEA;
  table.columns.push_back(blobcol);

  Column textcol;
  textcol.name = "note";
  textcol.type = ColumnType::TEXT;
  table.columns.push_back(textcol);

  Index index;
  index.name = "idx2";
  index.fields.push_back(
      IndexColumn{.column_name = "data", .ordering = IndexOrdering::asc});
  index.fields.push_back(
      IndexColumn{.column_name = "note", .ordering = IndexOrdering::desc});

  IndexOptions opts;

  auto sql = dialect.createIndex(table, index, opts);

  REQUIRE(sql == "CREATE  INDEX idx2 ON foo3 (data(32) ASC, note(32) DESC);");
}

TEST_CASE("mysql dropIndex", "[dialect]") {
  auto const &dialect = mysql_dialect();

  REQUIRE(dialect.dropIndex("foo1", "idx1") == "DROP INDEX idx1 ON foo1;");
}

TEST_CASE("mysql alterColumnType", "[dialect]") {
  auto const &dialect = mysql_dialect();

  REQUIRE(dialect.alterColumnType("col1", ColumnType::VARCHAR, 32) ==
          "MODIFY COLUMN col1 VARCHAR(32)");
}

TEST_CASE("mysql alterStorage", "[dialect]") {
  auto const &dialect = mysql_dialect();

  REQUIRE(dialect.alterStorage("InnoDB") == "ENGINE=InnoDB");
}

TEST_CASE("mysql randomRowSubquery", "[dialect]") {
  auto const &dialect = mysql_dialect();

  REQUIRE(dialect.randomRowSubquery("foo1", "id", 1) ==
          "(SELECT id FROM (SELECT id FROM foo1 ORDER BY RAND() LIMIT 1) AS "
          "swrnd)");
}

TEST_CASE("mysql typeName", "[dialect]") {
  auto const &dialect = mysql_dialect();

  REQUIRE(dialect.typeName(ColumnType::BYTEA, 0) == "BLOB");
  REQUIRE(dialect.typeName(ColumnType::TEXT, 0) == "TEXT");
  REQUIRE(dialect.typeName(ColumnType::VARCHAR, 32) == "VARCHAR(32)");
}

TEST_CASE("transaction capabilities", "[dialect]") {
  auto const &pg = pg_dialect();
  auto const &my = mysql_dialect();

  REQUIRE(pg.transactionalDDL());
  REQUIRE_FALSE(my.transactionalDDL());

  REQUIRE(pg.beginStatements(IsolationLevel::serverDefault) ==
          std::vector<std::string>{"BEGIN;"});
  REQUIRE(pg.beginStatements(IsolationLevel::serializable) ==
          std::vector<std::string>{"BEGIN ISOLATION LEVEL SERIALIZABLE;"});
  REQUIRE(pg.beginStatements(IsolationLevel::repeatableRead) ==
          std::vector<std::string>{"BEGIN ISOLATION LEVEL REPEATABLE READ;"});
  REQUIRE(pg.beginStatements(IsolationLevel::readCommitted) ==
          std::vector<std::string>{"BEGIN ISOLATION LEVEL READ COMMITTED;"});

  REQUIRE(my.beginStatements(IsolationLevel::serverDefault) ==
          std::vector<std::string>{"START TRANSACTION;"});
  REQUIRE(
      my.beginStatements(IsolationLevel::serializable) ==
      std::vector<std::string>{"SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;",
                               "START TRANSACTION;"});
  REQUIRE(my.beginStatements(IsolationLevel::readCommitted) ==
          std::vector<std::string>{
              "SET TRANSACTION ISOLATION LEVEL READ COMMITTED;",
              "START TRANSACTION;"});
  REQUIRE(my.beginStatements(IsolationLevel::repeatableRead) ==
          std::vector<std::string>{
              "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;",
              "START TRANSACTION;"});
}

TEST_CASE("dialect_for dispatches on flavor", "[dialect]") {
  sql_variant::ServerInfo mysqlInfo{.flavor_ = sql_variant::flavor::ps,
                                    .version = 80400};
  sql_variant::ServerInfo pxcInfo{.flavor_ = sql_variant::flavor::pxc,
                                  .version = 80400};
  sql_variant::ServerInfo pgInfo{.flavor_ = sql_variant::flavor::postgres,
                                 .version = 160000};

  REQUIRE(&sql_dialect::dialect_for(mysqlInfo) == &mysql_dialect());
  REQUIRE(&sql_dialect::dialect_for(pxcInfo) == &mysql_dialect());
  REQUIRE(&sql_dialect::dialect_for(pgInfo) == &pg_dialect());
}
