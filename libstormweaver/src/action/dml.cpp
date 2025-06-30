
#include "action/dml.hpp"
#include "action/helper.hpp"

#include <boost/algorithm/string/join.hpp>
#include <fmt/format.h>
#include <rfl.hpp>

using namespace metadata;
using namespace action;

namespace {

std::string generate_value(metadata::Column const &col, ps_random &rand,
                           std::optional<RangePartitioning> const &rp) {
  if (col.partition_key) {
    // Query will fail, but at least we don't crash
    if (rp->ranges.size() == 0)
      return "0";

    std::size_t num =
        rand.random_number(std::size_t(0), rp->rangeSize * rp->ranges.size());
    std::size_t range = num / rp->rangeSize;
    return std::to_string(rp->ranges[range].rangebase * rp->rangeSize +
                          (num % rp->rangeSize));
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
}; // namespace

InsertData::InsertData(DmlConfig const &config, std::size_t rows)
    : config(config), rows(rows) {}

InsertData::InsertData(DmlConfig const &config, std::size_t rows,
                       TableLocator const &locator)
    : config(config), locator(locator), rows(rows) {}

void InsertData::execute(Metadata &metaCtx, ps_random &rand,
                         sql_variant::LoggedSQL *connection) const {

  table_cptr table = find_random_table(metaCtx, rand);

  std::stringstream sql;
  sql << "INSERT INTO ";
  sql << table->name;
  sql << " (";

  bool first = true;
  for (auto const &f : table->columns) {
    if (!f.auto_increment) {
      if (!first)
        sql << ", ";
      sql << f.name;
      first = false;
    }
  }

  sql << " ) VALUES ";
  for (std::size_t idx = 0; idx < rows; ++idx) {
    if (idx != 0)
      sql << ", ";
    sql << "(";

    first = true;
    for (auto const &f : table->columns) {
      if (!f.auto_increment) {
        if (!first)
          sql << ", ";
        sql << generate_value(f, rand, table->partitioning);
        first = false;
      }
    }

    sql << ")";
  }

  sql << ";";

  connection->executeQuery(sql.str()).maybeThrow();
}

DeleteData::DeleteData(DmlConfig const &config) : config(config) {}

void DeleteData::execute(Metadata &metaCtx, ps_random &rand,
                         sql_variant::LoggedSQL *connection) const {

  table_cptr table = find_random_table(metaCtx, rand);

  auto const &tableName = table->name;
  // TODO: assumes we have a single column primary key as the first column.
  // Currently always true.
  auto const &pkName = table->columns[0].name;
  // TODO: add other types of deletes, e.g. not based on primary key
  auto const rows = rand.random_number(config.deleteMin, config.deleteMax);

  connection
      ->executeQuery(fmt::format("DELETE FROM {} WHERE {} IN (SELECT {} FROM "
                                 "{} ORDER BY random() LIMIT {});",
                                 tableName, pkName, pkName, tableName, rows))
      .maybeThrow();
}

UpdateOneRow::UpdateOneRow(DmlConfig const &config) : config(config) {}

void UpdateOneRow::execute(Metadata &metaCtx, ps_random &rand,
                           sql_variant::LoggedSQL *connection) const {

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
      if (!first)
        sql << ", ";
      sql << f.name;
      sql << " = ";
      sql << generate_value(f, rand, table->partitioning);
      first = false;
    }
  }

  sql << fmt::format(
      " WHERE {} IN (SELECT {} FROM {} ORDER BY random() LIMIT 1)", pkName,
      pkName, tableName);
  sql << ";";

  connection->executeQuery(sql.str()).maybeThrow();
}
