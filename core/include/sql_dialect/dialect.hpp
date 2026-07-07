#pragma once

#include "metadata/table.hpp"
#include "sql_variant/generic.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sql_dialect {

struct IndexOptions {
  bool concurrent = false; // pg: CONCURRENTLY; ignored on mysql
  bool only = false;       // pg: ON ONLY; ignored on mysql
};

enum class IsolationLevel : std::uint8_t {
  serverDefault,
  readCommitted,
  repeatableRead,
  serializable
};

enum class LockClause : std::uint8_t { none, forUpdate, forShare };

[[nodiscard]] constexpr std::string_view lockClauseSuffix(LockClause lock) {
  switch (lock) {
  case LockClause::forUpdate:
    return " FOR UPDATE";
  case LockClause::forShare:
    return " FOR SHARE";
  case LockClause::none:
    break;
  }
  return "";
}

/* Stateless SQL renderers. One instance per flavor family; obtained via
   dialect_for(connection->serverInfo()) at the top of Action::execute().
   Version/variant gating happens inside implementations using the
   ServerInfo predicates. */
class Dialect {
public:
  virtual ~Dialect() = default;

  [[nodiscard]] virtual std::string typeName(metadata::ColumnType type,
                                             std::size_t length) const = 0;
  // full column definition without any FK clause (FK is createTable's job)
  [[nodiscard]] virtual std::string
  columnDefinition(metadata::Column const &col) const = 0;

  // fkTargetName non-empty means columns[1] references fkTargetName(id)
  [[nodiscard]] virtual std::string
  createTable(metadata::Table const &table,
              std::string_view fkTargetName) const = 0;

  [[nodiscard]] virtual std::string
  addPartition(metadata::Table const &table, std::size_t rangebase) const = 0;
  [[nodiscard]] virtual std::string
  dropPartition(metadata::Table const &table, std::size_t rangebase) const = 0;

  [[nodiscard]] virtual std::string
  createIndex(metadata::Table const &table, metadata::Index const &index,
              IndexOptions const &opts) const = 0;
  [[nodiscard]] virtual std::string
  dropIndex(std::string_view tableName, std::string_view indexName) const = 0;

  [[nodiscard]] virtual std::string
  alterColumnType(std::string_view columnName, metadata::ColumnType type,
                  std::size_t length) const = 0;
  [[nodiscard]] virtual std::string
  alterStorage(std::string_view engine) const = 0;

  [[nodiscard]] virtual std::string
  randomRowSubquery(std::string_view tableName, std::string_view columnName,
                    std::size_t limit) const = 0;

  // standalone statement, unlike randomRowSubquery; FOR SHARE needs
  // mysql 8.0+ (the only supported mysql line)
  [[nodiscard]] virtual std::string
  randomRowsSelect(std::string_view tableName, std::string_view columnName,
                   std::size_t limit, LockClause lock) const = 0;

  // capabilities
  [[nodiscard]] virtual bool partitionsInlineInCreate() const = 0;
  [[nodiscard]] virtual bool supportsFkOnPartitionedTables() const = 0;
  // dropping a column that participates in a FK without dropping the
  // constraint first
  [[nodiscard]] virtual bool canDropFkColumn() const = 0;
  // mysql needs an index on the referencing column and silently creates one
  // when missing; emit it explicitly so the catalog knows its name
  [[nodiscard]] virtual bool fkRequiresIndex() const = 0;
  // pg drops the whole dependent index when an indexed column is dropped;
  // mysql only removes the column from the index (index stays unless empty)
  [[nodiscard]] virtual bool dropColumnRemovesWholeIndex() const = 0;
  [[nodiscard]] virtual std::size_t maxIndexColumns() const = 0;
  [[nodiscard]] virtual std::size_t maxIndexesPerTable() const = 0;

  // pg: DDL rolls back with the transaction. mysql: DDL implicitly commits.
  [[nodiscard]] virtual bool transactionalDDL() const = 0;
  // statements opening a transaction; executed in order
  [[nodiscard]] virtual std::vector<std::string>
  beginStatements(IsolationLevel level) const = 0;
};

Dialect const &pg_dialect();
Dialect const &mysql_dialect();

Dialect const &dialect_for(sql_variant::ServerInfo const &info);

} // namespace sql_dialect
