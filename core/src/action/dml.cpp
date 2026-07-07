
#include "action/dml.hpp"
#include "action/helper.hpp"
#include "sql_dialect/dialect.hpp"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <optional>
#include <rfl.hpp>
#include <utility>
#include <vector>

using namespace metadata;
using namespace action;

namespace {

std::string generate_value(metadata::Column const &col, ps_random &rand,
                           std::optional<RangePartitioning> const &rp,
                           metadata::CatalogView<metadata::Table> const &tables,
                           sql_dialect::Dialect const &dialect) {
  if (col.partition_key) {
    // Query will fail, but at least we don't crash
    if (rp->ranges.empty()) {
      return "0";
    }

    // random_number is inclusive on both ends
    std::size_t num = rand.random_number(
        static_cast<std::size_t>(0), (rp->rangeSize * rp->ranges.size()) - 1);
    std::size_t range = num / rp->rangeSize;
    return std::to_string((rp->ranges[range].rangebase * rp->rangeSize) +
                          (num % rp->rangeSize));
  }
  if (col.foreign_key_references) {
    auto target = tables.byId(col.foreign_key_references.id);
    if (target == nullptr) {
      return "NULL"; // referenced table dropped meanwhile; tolerated drift
    }
    // TODO: column name is hardcoded
    return dialect.randomRowSubquery(target->name, "id", 1);
  }

  switch (col.type) {
  case metadata::ColumnType::INT:
    return std::to_string(rand.random_number(1, 1000000));
  case metadata::ColumnType::REAL:
    return std::to_string(rand.random_number(1.0, 1000000.0));
  case metadata::ColumnType::VARCHAR:
    return std::string("'") + rand.random_string(0, col.length) + "'";
  case metadata::ColumnType::BYTEA:
  case metadata::ColumnType::TEXT:
    return std::string("'") + rand.random_string(50, 1000) + "'";
  case metadata::ColumnType::BOOL:
    return rand.random_number(0, 1) == 1 ? "true" : "false";
  case metadata::ColumnType::CHAR:
    return std::string("'") + rand.random_string(0, col.length) + "'";
  }
  return "";
}

sql_dialect::LockClause pick_lock(ps_random &rand,
                                  action::LockWeights const &w) {
  const std::size_t total = w.none + w.for_update + w.for_share;
  if (total == 0) {
    return sql_dialect::LockClause::none;
  }
  auto roll = rand.random_number<std::size_t>(1, total);
  if (roll <= w.none) {
    return sql_dialect::LockClause::none;
  }
  roll -= w.none;
  if (roll <= w.for_update) {
    return sql_dialect::LockClause::forUpdate;
  }
  return sql_dialect::LockClause::forShare;
}

std::vector<std::string> fetch_random_ids(sql_variant::LoggedSQL *connection,
                                          sql_dialect::Dialect const &dialect,
                                          std::string_view tableName,
                                          std::string_view pkName,
                                          std::size_t limit,
                                          sql_dialect::LockClause lock) {
  auto res = connection->executeQuery(
      dialect.randomRowsSelect(tableName, pkName, limit, lock));
  res.maybeThrow();
  std::vector<std::string> ids;
  if (res.data == nullptr) {
    return ids;
  }
  const auto rows = res.data->numRows();
  ids.reserve(rows);
  for (std::size_t i = 0; i < rows; ++i) {
    auto const row = res.data->nextRow();
    if (!row.rowData.empty() && row.rowData[0]) {
      ids.emplace_back(*row.rowData[0]);
    }
  }
  return ids;
}

// opens a transaction (arming the guard) only when not already in one;
// inside an enclosing transaction rollback is its owner's job
void maybe_begin_own_trx(metadata::Context const &metaCtx,
                         sql_variant::LoggedSQL *connection,
                         sql_dialect::Dialect const &dialect,
                         std::optional<action::TxGuard> &guard) {
  if (metaCtx.inTransaction()) {
    return;
  }
  for (auto const &stmt :
       dialect.beginStatements(sql_dialect::IsolationLevel::serverDefault)) {
    connection->executeQuery(stmt).maybeThrow();
  }
  guard.emplace(connection);
}

void maybe_commit_own_trx(std::optional<action::TxGuard> &guard,
                          sql_variant::LoggedSQL *connection) {
  if (!guard) {
    return;
  }
  auto res = connection->executeQuery("COMMIT;");
  guard->disarm(); // success or failure, the transaction is over
  res.maybeThrow();
}
}; // namespace

InsertData::InsertData(DmlConfig const &config, std::size_t rows)
    : config(config), rows(rows) {}

InsertData::InsertData(DmlConfig const &config, std::size_t rows,
                       TableLocator locator)
    : config(config), locator(std::move(locator)), rows(rows) {}

void InsertData::execute(Context &metaCtx, ps_random &rand,
                         sql_variant::LoggedSQL *connection) const {
  auto const serverInfo = connection->serverInfo();
  auto const &dialect = sql_dialect::dialect_for(serverInfo);

  auto const tables = metaCtx.get<Table>();

  table_cptr table = locator ? locator() : find_random_table(metaCtx, rand);
  if (table == nullptr) {
    return; // locator target vanished (e.g. created table already dropped)
  }

  std::stringstream sql;
  sql << "INSERT INTO ";
  sql << table->name;
  sql << " (";

  bool first = true;
  for (auto const &f : table->columns) {
    if (!f.auto_increment) {
      if (!first) {
        sql << ", ";
      }
      sql << f.name;
      first = false;
    }
  }

  sql << " ) VALUES ";
  for (std::size_t idx = 0; idx < rows; ++idx) {
    if (idx != 0) {
      sql << ", ";
    }
    sql << "(";

    first = true;
    for (auto const &f : table->columns) {
      if (!f.auto_increment) {
        if (!first) {
          sql << ", ";
        }
        sql << generate_value(f, rand, table->partitioning, tables, dialect);
        first = false;
      }
    }

    sql << ")";
  }

  sql << ";";

  connection->executeQuery(sql.str()).maybeThrow();
}

DeleteData::DeleteData(DmlConfig const &config) : config(config) {}

void DeleteData::execute(Context &metaCtx, ps_random &rand,
                         sql_variant::LoggedSQL *connection) const {
  auto const serverInfo = connection->serverInfo();
  auto const &dialect = sql_dialect::dialect_for(serverInfo);

  table_cptr table = find_random_table(metaCtx, rand);

  auto const &tableName = table->name;
  // TODO: assumes we have a single column primary key as the first column.
  // Currently always true.
  auto const &pkName = table->columns[0].name;
  // TODO: add other types of deletes, e.g. not based on primary key
  auto const rows = rand.random_number(config.deleteMin, config.deleteMax);

  connection
      ->executeQuery(
          fmt::format("DELETE FROM {} WHERE {} IN {};", tableName, pkName,
                      dialect.randomRowSubquery(tableName, pkName, rows)))
      .maybeThrow();
}

SelectThenDelete::SelectThenDelete(DmlConfig const &config) : config(config) {}

void SelectThenDelete::execute(Context &metaCtx, ps_random &rand,
                               sql_variant::LoggedSQL *connection) const {
  auto const serverInfo = connection->serverInfo();
  auto const &dialect = sql_dialect::dialect_for(serverInfo);

  table_cptr table = find_random_table(metaCtx, rand);

  auto const &tableName = table->name;
  // TODO: assumes we have a single column primary key as the first column.
  // Currently always true.
  auto const &pkName = table->columns[0].name;
  // all randomness up front: server state must not shift the rand stream
  auto const rows = rand.random_number(config.deleteMin, config.deleteMax);
  auto const lock = pick_lock(rand, config.lockWeights);

  std::optional<TxGuard> guard;
  maybe_begin_own_trx(metaCtx, connection, dialect, guard);

  auto const ids =
      fetch_random_ids(connection, dialect, tableName, pkName, rows, lock);
  if (!ids.empty()) {
    connection
        ->executeQuery(fmt::format("DELETE FROM {} WHERE {} IN ({});",
                                   tableName, pkName, fmt::join(ids, ", ")))
        .maybeThrow();
  }

  maybe_commit_own_trx(guard, connection);
}

UpdateOneRow::UpdateOneRow(DmlConfig const &config) : config(config) {}

void UpdateOneRow::execute(Context &metaCtx, ps_random &rand,
                           sql_variant::LoggedSQL *connection) const {
  auto const serverInfo = connection->serverInfo();
  auto const &dialect = sql_dialect::dialect_for(serverInfo);

  auto const tables = metaCtx.get<Table>();

  table_cptr table = find_random_table(metaCtx, rand);

  auto const &tableName = table->name;
  // TODO: assumes we have a single column primary key as the first column.
  // Currently always true.
  auto const &pkName = table->columns[0].name;

  std::stringstream sql;
  sql << "UPDATE ";
  sql << tableName;
  sql << " SET ";

  bool first = true;
  for (auto const &f : table->columns) {
    if (!f.auto_increment) {
      if (!first) {
        sql << ", ";
      }
      sql << f.name;
      sql << " = ";
      sql << generate_value(f, rand, table->partitioning, tables, dialect);
      first = false;
    }
  }

  sql << fmt::format(" WHERE {} IN {}", pkName,
                     dialect.randomRowSubquery(tableName, pkName, 1));
  sql << ";";

  connection->executeQuery(sql.str()).maybeThrow();
}

SelectThenUpdate::SelectThenUpdate(DmlConfig const &config) : config(config) {}

void SelectThenUpdate::execute(Context &metaCtx, ps_random &rand,
                               sql_variant::LoggedSQL *connection) const {
  auto const serverInfo = connection->serverInfo();
  auto const &dialect = sql_dialect::dialect_for(serverInfo);

  auto const tables = metaCtx.get<Table>();

  table_cptr table = find_random_table(metaCtx, rand);

  auto const &tableName = table->name;
  // TODO: assumes we have a single column primary key as the first column.
  // Currently always true.
  auto const &pkName = table->columns[0].name;
  // all randomness up front: server state must not shift the rand stream
  auto const rows = rand.random_number(config.updateMin, config.updateMax);
  auto const lock = pick_lock(rand, config.lockWeights);

  std::stringstream setClause;
  bool first = true;
  for (auto const &f : table->columns) {
    // pk/partition-key columns get a single constant value; setting them
    // on a multi-row update collapses every matched row into one key
    if (!f.auto_increment && !f.primary_key && !f.partition_key) {
      if (!first) {
        setClause << ", ";
      }
      setClause << f.name;
      setClause << " = ";
      setClause << generate_value(f, rand, table->partitioning, tables,
                                  dialect);
      first = false;
    }
  }

  std::optional<TxGuard> guard;
  maybe_begin_own_trx(metaCtx, connection, dialect, guard);

  auto const ids =
      fetch_random_ids(connection, dialect, tableName, pkName, rows, lock);
  // !first: table may have no updatable columns left (empty SET)
  if (!ids.empty() && !first) {
    connection
        ->executeQuery(fmt::format("UPDATE {} SET {} WHERE {} IN ({});",
                                   tableName, setClause.str(), pkName,
                                   fmt::join(ids, ", ")))
        .maybeThrow();
  }

  maybe_commit_own_trx(guard, connection);
}
