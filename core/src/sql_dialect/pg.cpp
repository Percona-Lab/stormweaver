#include "sql_dialect/dialect.hpp"

#include <boost/algorithm/string/join.hpp>
#include <fmt/format.h>
#include <limits>
#include <rfl.hpp>

using namespace metadata;

namespace sql_dialect {

namespace {

class PgDialect : public Dialect {
public:
  [[nodiscard]] std::string typeName(ColumnType type,
                                     std::size_t length) const override {
    std::string name = rfl::enum_to_string(type);
    if (length > 0) {
      name += fmt::format("({})", length);
    }
    return name;
  }

  [[nodiscard]] std::string columnDefinition(Column const &col) const override {
    if (col.auto_increment) {
      return fmt::format("{} {}", col.name, "SERIAL");
    }
    return fmt::format("{} {}", col.name, typeName(col.type, col.length));
  }

  [[nodiscard]] std::string
  createTable(Table const &table,
              std::string_view fkTargetName) const override {
    std::vector<std::string> defs;
    std::vector<std::string> pk_columns;

    for (std::size_t idx = 0; idx < table.columns.size(); ++idx) {
      auto const &col = table.columns[idx];
      if (col.primary_key) {
        pk_columns.push_back(col.name);
      }
      std::string def = columnDefinition(col);
      if (idx == 1 && !fkTargetName.empty()) {
        def += fmt::format(" REFERENCES {} ON DELETE CASCADE", fkTargetName);
      }
      defs.push_back(def);
    }

    if (!pk_columns.empty()) {
      defs.push_back(fmt::format("PRIMARY KEY ({})",
                                 boost::algorithm::join(pk_columns, ", ")));
    }

    std::string partitionClause;
    if (table.partitioning.has_value()) {
      partitionClause =
          fmt::format(" PARTITION BY RANGE ({})", table.columns[0].name);
    }

    // spacing quirks (") ;" / "))  PARTITION") are intentional: output must
    // stay byte-identical to the legacy action-layer format strings
    return fmt::format("CREATE TABLE {} ({}) {};", table.name,
                       boost::algorithm::join(defs, ",\n"), partitionClause);
  }

  [[nodiscard]] std::string addPartition(Table const &table,
                                         std::size_t rangebase) const override {
    const auto rangeSize = table.partitioning->rangeSize;
    return fmt::format(
        "CREATE TABLE {}_p{} PARTITION OF {} FOR VALUES FROM ({}) TO ({});",
        table.name, rangebase, table.name, rangeSize * rangebase,
        rangeSize * (rangebase + 1));
  }

  [[nodiscard]] std::string
  dropPartition(Table const &table, std::size_t rangebase) const override {
    return fmt::format("DROP TABLE {}_p{} CASCADE;", table.name, rangebase);
  }

  [[nodiscard]] std::string
  createIndex(Table const &table, Index const &index,
              IndexOptions const &opts) const override {
    const std::string_view unique = index.unique ? "UNIQUE" : "";
    const std::string_view concurrently = opts.concurrent ? "CONCURRENTLY" : "";
    const std::string_view only = opts.only ? "ONLY" : "";

    std::vector<std::string> indexColumns;
    for (auto const &field : index.fields) {
      // default_ ordering renders as ASC, same as the legacy action code
      indexColumns.emplace_back(
          fmt::format("{} {}", field.column_name,
                      field.ordering == IndexOrdering::desc ? "DESC" : "ASC"));
    }

    // empty optional keywords leave double spaces; intentional, see above
    return fmt::format("CREATE {} INDEX {} {} ON {} {} ({});", unique,
                       concurrently, index.name, only, table.name,
                       boost::algorithm::join(indexColumns, ", "));
  }

  [[nodiscard]] std::string
  dropIndex(std::string_view /*tableName*/,
            std::string_view indexName) const override {
    return fmt::format("DROP INDEX {};", indexName);
  }

  [[nodiscard]] std::string alterColumnType(std::string_view columnName,
                                            ColumnType type,
                                            std::size_t length) const override {
    return fmt::format("ALTER COLUMN {} TYPE {}", columnName,
                       typeName(type, length));
  }

  [[nodiscard]] std::string
  alterStorage(std::string_view engine) const override {
    return fmt::format("SET ACCESS METHOD {}", engine);
  }

  [[nodiscard]] std::string
  randomRowSubquery(std::string_view tableName, std::string_view columnName,
                    std::size_t limit) const override {
    return fmt::format("(SELECT {} FROM {} ORDER BY random() LIMIT {})",
                       columnName, tableName, limit);
  }

  [[nodiscard]] std::string randomRowsSelect(std::string_view tableName,
                                             std::string_view columnName,
                                             std::size_t limit,
                                             LockClause lock) const override {
    return fmt::format("SELECT {} FROM {} ORDER BY random() LIMIT {}{};",
                       columnName, tableName, limit, lockClauseSuffix(lock));
  }

  [[nodiscard]] bool partitionsInlineInCreate() const override { return false; }

  [[nodiscard]] bool supportsFkOnPartitionedTables() const override {
    return true;
  }

  [[nodiscard]] bool canDropFkColumn() const override { return true; }

  [[nodiscard]] bool fkRequiresIndex() const override { return false; }

  [[nodiscard]] bool dropColumnRemovesWholeIndex() const override {
    return true;
  }

  [[nodiscard]] std::size_t maxIndexColumns() const override { return 32; }

  [[nodiscard]] std::size_t maxIndexesPerTable() const override {
    return std::numeric_limits<std::size_t>::max();
  }

  [[nodiscard]] bool transactionalDDL() const override { return true; }

  [[nodiscard]] std::vector<std::string>
  beginStatements(IsolationLevel level) const override {
    switch (level) {
    case IsolationLevel::readCommitted:
      return {"BEGIN ISOLATION LEVEL READ COMMITTED;"};
    case IsolationLevel::repeatableRead:
      return {"BEGIN ISOLATION LEVEL REPEATABLE READ;"};
    case IsolationLevel::serializable:
      return {"BEGIN ISOLATION LEVEL SERIALIZABLE;"};
    case IsolationLevel::serverDefault:
      break;
    }
    return {"BEGIN;"};
  }
};

} // namespace

Dialect const &pg_dialect() {
  static const PgDialect instance;
  return instance;
}

} // namespace sql_dialect
