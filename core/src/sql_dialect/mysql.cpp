#include "sql_dialect/dialect.hpp"

#include <algorithm>
#include <boost/algorithm/string/join.hpp>
#include <fmt/format.h>
#include <rfl.hpp>

using namespace metadata;

namespace sql_dialect {

namespace {

// key-prefix length for BLOB/TEXT index columns; any small value works for
// the generated workload, well under InnoDB's 3072-byte key limit
constexpr std::size_t kTextIndexPrefixLength = 32;

class MySQLDialect : public Dialect {
public:
  [[nodiscard]] std::string typeName(ColumnType type,
                                     std::size_t length) const override {
    std::string name =
        type == ColumnType::BYTEA ? "BLOB" : rfl::enum_to_string(type);
    if (length > 0) {
      name += fmt::format("({})", length);
    }
    return name;
  }

  [[nodiscard]] std::string columnDefinition(Column const &col) const override {
    if (col.auto_increment) {
      return fmt::format("{} INT AUTO_INCREMENT", col.name);
    }
    return fmt::format("{} {}", col.name, typeName(col.type, col.length));
  }

  [[nodiscard]] std::string
  createTable(Table const &table,
              std::string_view fkTargetName) const override {
    std::vector<std::string> defs;
    std::vector<std::string> pk_columns;

    for (auto const &col : table.columns) {
      if (col.primary_key) {
        pk_columns.push_back(col.name);
      }
      // mysql silently ignores column-level REFERENCES; fk goes table-level
      defs.push_back(columnDefinition(col));
    }

    if (!pk_columns.empty()) {
      defs.push_back(fmt::format("PRIMARY KEY ({})",
                                 boost::algorithm::join(pk_columns, ", ")));
    }

    for (auto const &idx : table.indexes) {
      std::vector<std::string> idxColumns;
      for (auto const &field : idx.fields) {
        idxColumns.push_back(fmt::format(
            "{} {}", field.column_name,
            field.ordering == IndexOrdering::desc ? "DESC" : "ASC"));
      }
      defs.push_back(fmt::format("{}KEY {} ({})", idx.unique ? "UNIQUE " : "",
                                 idx.name,
                                 boost::algorithm::join(idxColumns, ", ")));
    }

    if (!fkTargetName.empty() && table.columns.size() > 1) {
      defs.push_back(
          fmt::format("FOREIGN KEY ({}) REFERENCES {} (id) ON DELETE CASCADE",
                      table.columns[1].name, fkTargetName));
    }

    std::string partitionClause;
    if (table.partitioning.has_value()) {
      std::vector<std::string> parts;
      for (auto const &range : table.partitioning->ranges) {
        parts.push_back(
            fmt::format("PARTITION p{} VALUES LESS THAN ({})", range.rangebase,
                        table.partitioning->rangeSize * (range.rangebase + 1)));
      }
      partitionClause =
          fmt::format(" PARTITION BY RANGE ({}) ({})", table.columns[0].name,
                      boost::algorithm::join(parts, ", "));
    }

    // same outer spacing quirks as pg, kept byte-identical on purpose
    return fmt::format("CREATE TABLE {} ({}) {};", table.name,
                       boost::algorithm::join(defs, ",\n"), partitionClause);
  }

  [[nodiscard]] std::string addPartition(Table const &table,
                                         std::size_t rangebase) const override {
    const auto rangeSize = table.partitioning->rangeSize;
    return fmt::format(
        "ALTER TABLE {} ADD PARTITION (PARTITION p{} VALUES LESS THAN "
        "({}));",
        table.name, rangebase, rangeSize * (rangebase + 1));
  }

  [[nodiscard]] std::string
  dropPartition(Table const &table, std::size_t rangebase) const override {
    return fmt::format("ALTER TABLE {} DROP PARTITION p{};", table.name,
                       rangebase);
  }

  [[nodiscard]] std::string
  createIndex(Table const &table, Index const &index,
              IndexOptions const & /*opts*/) const override {
    const std::string_view unique = index.unique ? "UNIQUE" : "";

    std::vector<std::string> indexColumns;
    for (auto const &field : index.fields) {
      // blob/text columns need an explicit key-prefix length on mysql
      std::string prefix;
      auto colIt = std::ranges::find_if(table.columns, [&](Column const &c) {
        return c.name == field.column_name;
      });
      if (colIt != table.columns.end() && (colIt->type == ColumnType::TEXT ||
                                           colIt->type == ColumnType::BYTEA)) {
        prefix = fmt::format("({})", kTextIndexPrefixLength);
      }

      indexColumns.push_back(
          fmt::format("{}{} {}", field.column_name, prefix,
                      field.ordering == IndexOrdering::desc ? "DESC" : "ASC"));
    }

    // empty unique token leaves a double space; intentional, matches pg
    return fmt::format("CREATE {} INDEX {} ON {} ({});", unique, index.name,
                       table.name, boost::algorithm::join(indexColumns, ", "));
  }

  [[nodiscard]] std::string
  dropIndex(std::string_view tableName,
            std::string_view indexName) const override {
    return fmt::format("DROP INDEX {} ON {};", indexName, tableName);
  }

  [[nodiscard]] std::string alterColumnType(std::string_view columnName,
                                            ColumnType type,
                                            std::size_t length) const override {
    return fmt::format("MODIFY COLUMN {} {}", columnName,
                       typeName(type, length));
  }

  [[nodiscard]] std::string
  alterStorage(std::string_view engine) const override {
    return fmt::format("ENGINE={}", engine);
  }

  [[nodiscard]] std::string
  randomRowSubquery(std::string_view tableName, std::string_view columnName,
                    std::size_t limit) const override {
    // mysql error 1093 forbids selecting from the table being modified in
    // the same statement; the derived table dodges that
    return fmt::format(
        "(SELECT {} FROM (SELECT {} FROM {} ORDER BY RAND() LIMIT {}) AS "
        "swrnd)",
        columnName, columnName, tableName, limit);
  }

  [[nodiscard]] std::string randomRowsSelect(std::string_view tableName,
                                             std::string_view columnName,
                                             std::size_t limit,
                                             LockClause lock) const override {
    // FOR UPDATE here locks every scanned row on innodb (whole table),
    // unlike pg which locks only the returned rows; intentional chaos
    return fmt::format("SELECT {} FROM {} ORDER BY RAND() LIMIT {}{};",
                       columnName, tableName, limit, lockClauseSuffix(lock));
  }

  [[nodiscard]] bool partitionsInlineInCreate() const override { return true; }

  [[nodiscard]] bool supportsFkOnPartitionedTables() const override {
    return false;
  }

  // mysql needs the FK constraint dropped before the column
  [[nodiscard]] bool canDropFkColumn() const override { return false; }

  [[nodiscard]] bool fkRequiresIndex() const override { return true; }

  [[nodiscard]] bool dropColumnRemovesWholeIndex() const override {
    return false;
  }

  // innodb limits: 16 key parts per index, 64 keys per table (margin kept)
  [[nodiscard]] std::size_t maxIndexColumns() const override { return 16; }

  [[nodiscard]] std::size_t maxIndexesPerTable() const override { return 60; }

  // innodb rejects keys over 3072 bytes (error 1071)
  [[nodiscard]] std::size_t maxIndexKeyBytes() const override { return 3072; }

  // utf8mb4 worst case: 4 bytes per char
  [[nodiscard]] std::size_t
  indexKeyPartBytes(Column const &col) const override {
    switch (col.type) {
    case ColumnType::CHAR:
    case ColumnType::VARCHAR:
      return col.length * 4;
    case ColumnType::TEXT:
      return kTextIndexPrefixLength * 4;
    case ColumnType::BYTEA:
      return kTextIndexPrefixLength;
    case ColumnType::BOOL:
    case ColumnType::INT:
    case ColumnType::REAL:
      break;
    }
    return 8;
  }

  [[nodiscard]] bool transactionalDDL() const override { return false; }

  [[nodiscard]] std::vector<std::string>
  beginStatements(IsolationLevel level) const override {
    std::vector<std::string> out;
    switch (level) {
    case IsolationLevel::readCommitted:
      out.emplace_back("SET TRANSACTION ISOLATION LEVEL READ COMMITTED;");
      break;
    case IsolationLevel::repeatableRead:
      out.emplace_back("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;");
      break;
    case IsolationLevel::serializable:
      out.emplace_back("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;");
      break;
    case IsolationLevel::serverDefault:
      break;
    }
    out.emplace_back("START TRANSACTION;");
    return out;
  }
};

} // namespace

Dialect const &mysql_dialect() {
  static const MySQLDialect instance;
  return instance;
}

} // namespace sql_dialect
