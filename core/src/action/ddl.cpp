
#include "action/ddl.hpp"
#include "action/helper.hpp"
#include "sql_dialect/dialect.hpp"

#include <algorithm>
#include <boost/algorithm/string/join.hpp>
#include <fmt/format.h>
#include <numeric>
#include <rfl.hpp>
#include <utility>

using namespace metadata;
using namespace action;

namespace {
ColumnType randomColumnType(ps_random &rand) {
  auto arr = rfl::get_enumerator_array<ColumnType>();
  return arr[rand.random_number(static_cast<std::size_t>(0), arr.size() - 1)]
      .second;
}

std::size_t randomColumnLength(ps_random &rand, ColumnType type) {
  switch (type) {
  case ColumnType::BYTEA:
  case ColumnType::TEXT:
    return 0;
  case ColumnType::CHAR:
  case ColumnType::VARCHAR:
    return rand.random_number(1, 100);
  case ColumnType::BOOL:
  case ColumnType::INT:
  case ColumnType::REAL:
    return 0;
  }
  return 0;
}

Column randomColumn(ps_random &rand, bool forceInt = false) {
  Column col;

  col.name = fmt::format("col{}", rand.random_number<std::size_t>());
  col.type = forceInt ? ColumnType::INT : randomColumnType(rand);
  col.length = randomColumnLength(rand, col.type);

  return col;
}

table_cptr findPartitionedTable(CatalogView<Table> const &tables,
                                ps_random &rand, DdlConfig const &config) {
  for (std::size_t i = 0; i < 10; ++i) {
    auto table = tables.randomPick(rand);

    if (table == nullptr || !table->partitioning.has_value()) {
      continue;
    }

    auto count = table->partitioning->ranges.size();

    if (count >= config.max_partition_count ||
        count <= config.min_partition_count) {
      continue;
    }

    return table;
  }

  return nullptr;
}

} // namespace

CreateTable::CreateTable(DdlConfig config, Table::Type type)
    : config(std::move(config)), type(type) {}

void CreateTable::setSuccessCallback(TableCallback const &cb) {
  successCallback = cb;
}

void CreateTable::execute(Context &metaCtx, ps_random &rand,
                          sql_variant::LoggedSQL *connection) const {
  auto const serverInfo = connection->serverInfo();
  auto const &dialect = sql_dialect::dialect_for(serverInfo);

  auto tables = metaCtx.get<Table>();

  if (tables.size() >= config.max_table_count) {
    // skip: table count limit
    return;
  }

  // 1: build the whole record locally; it stays private until insert()

  Table table;
  table.id = metaCtx.nextId();
  table.name = fmt::format("foo{}", rand.random_number(1, 100000000));

  const auto column_count =
      rand.random_number<std::size_t>(2, config.max_column_count);

  const bool partitioned = type == Table::Type::partitioned;

  const bool add_foreign_key = rand.random_number<std::size_t>(1, 100) <=
                               config.ct_foreign_key_percentage;

  for (size_t idx = 0; idx < column_count; ++idx) {
    const bool primary_key_column = idx == 0;
    const bool foreign_key_column = add_foreign_key && idx == 1;
    table.columns.push_back(
        randomColumn(rand, primary_key_column || foreign_key_column));
  }

  table.columns[0].name = "id";
  table.columns[0].primary_key = true;
  table.columns[0].nullable = false;

  if (partitioned) {
    table.columns[0].partition_key = true;
    table.partitioning = RangePartitioning{};
    table.partitioning->rangeSize = 10000000;
    // range count rolled below, after the fk pick - see comment there
  } else {
    table.columns[0].auto_increment = true;
  }

  const bool fkAllowed =
      !partitioned || dialect.supportsFkOnPartitionedTables();
  std::string fkTargetName;
  if (add_foreign_key && fkAllowed) {
    // Foreign keys are always added to the second column (index 1) as a
    // simplification for now
    try {
      auto table_ref = find_random_table(metaCtx, rand);
      table.columns[1].foreign_key_references = Ref<Table>{table_ref->id};
      fkTargetName = table_ref->name;
    } catch (ActionException const &) {
      // no table to reference yet, column just stays a plain column
      // @TODO: consider logging this skip
    }
  }

  // decide partition layout before SQL: inline-partition dialects need it
  // in the CREATE statement. rolled after the fk pick to keep the legacy
  // rand draw order
  if (partitioned) {
    const auto cnt = rand.random_number(config.min_partition_count,
                                        config.max_partition_count);
    for (std::size_t i = 0; i < cnt; ++i) {
      table.partitioning->ranges.push_back(RangePartition{i});
    }
  }

  // 2: build & execute SQL statement - no lock held anywhere here

  connection->executeQuery(dialect.createTable(table, fkTargetName))
      .maybeThrow();

  if (partitioned && !dialect.partitionsInlineInCreate()) {
    std::vector<std::size_t> failed;
    for (auto const &range : table.partitioning->ranges) {
      bool ok = false;
      for (std::size_t tries = 0; tries < 3 && !ok; ++tries) {
        ok = static_cast<bool>(connection->executeQuery(
            dialect.addPartition(table, range.rangebase)));
      }
      if (!ok) {
        failed.push_back(range.rangebase);
      }
    }
    std::erase_if(table.partitioning->ranges, [&](RangePartition const &r) {
      return std::ranges::find(failed, r.rangebase) != failed.end();
    });
  }

  // 3: publish

  const auto fkRef = table.columns.size() > 1
                         ? table.columns[1].foreign_key_references
                         : Ref<Table>{};
  const auto id = table.id;
  tables.insert(std::move(table));

  if (fkRef && tables.byId(fkRef.id) == nullptr) {
    // referenced table dropped mid-create; its sweep could not see us yet
    tables.update(id,
                  [fkRef](Table &t) { return t.removeReferencesTo(fkRef.id); });
  }

  if (successCallback) {
    successCallback(tables.byId(id));
  }
}

DropTable::DropTable(DdlConfig config) : config(std::move(config)) {}

void DropTable::execute(Context &metaCtx, ps_random &rand,
                        sql_variant::LoggedSQL *connection) const {
  auto tables = metaCtx.get<Table>();

  if (tables.size() <= config.min_table_count) {
    // skip: table count limit
    return;
  }

  auto snap = tables.randomPick(rand);
  if (snap == nullptr) {
    return;
  }

  // mysql doesn't cascade-drop through FKs like pg's CASCADE does, disable
  // the check around the drop instead
  const bool mysqlLike = connection->serverInfo().is_mysql_like();
  if (mysqlLike) {
    connection->executeQuery("SET FOREIGN_KEY_CHECKS=0;").maybeThrow();
  }

  // no dialect: valid on both pg and mysql (mysql parses and ignores CASCADE)
  try {
    connection->executeQuery(fmt::format("DROP TABLE {} CASCADE;", snap->name))
        .maybeThrow();
  } catch (...) {
    if (mysqlLike) {
      // best effort: the session survives a failed DROP and would keep
      // running with FK checks off otherwise
      std::ignore = connection->executeQuery("SET FOREIGN_KEY_CHECKS=1;");
    }
    throw;
  }

  if (mysqlLike) {
    connection->executeQuery("SET FOREIGN_KEY_CHECKS=1;").maybeThrow();
  }

  tables.erase(snap->id);

  // Best effort: remove foreign key references to the dropped table
  const auto droppedId = snap->id;
  for (auto const &other : tables.snapshotAll()) {
    if (other->hasReferenceTo(droppedId)) {
      tables.update(other->id, [droppedId](Table &t) {
        return t.removeReferencesTo(droppedId);
      });
    }
  }
}

AlterTable::AlterTable(DdlConfig config,
                       BitFlags<AlterSubcommand> const &possibleCommands)
    : config(std::move(config)), possibleCommands(possibleCommands) {}

void AlterTable::execute(Context &metaCtx, ps_random &rand,
                         sql_variant::LoggedSQL *connection) const {
  auto const serverInfo = connection->serverInfo();
  auto const &dialect = sql_dialect::dialect_for(serverInfo);

  auto tables = metaCtx.get<Table>();

  auto snap = tables.randomPick(rand);
  if (snap == nullptr) {
    return;
  }

  Table const &working = *snap; // drives SQL generation only

  const auto commands = possibleCommands.All();

  const auto howManySubcommands =
      rand.random_number(static_cast<std::size_t>(1), config.max_alter_clauses);

  std::vector<std::string> alterSubcommands;

  std::vector<std::size_t> availableColumns(working.columns.size());
  std::vector<std::size_t> droppedColumns;
  std::ranges::iota(availableColumns, 0);

  // the delta, recorded by name: concurrent ALTERs may reshuffle the
  // record before we merge
  std::vector<std::string> droppedColumnNames;
  std::vector<Column> newColumns;
  struct TypeChange {
    std::string name;
    ColumnType type;
    std::size_t length;
  };
  std::vector<TypeChange> typeChanges;

  bool changingAm = false;

  for (std::size_t idx = 0; idx < howManySubcommands; ++idx) {
    bool addedSubcommand = false;

    while (!addedSubcommand) {
      const auto cmdIndex =
          rand.random_number(static_cast<std::size_t>(0), commands.size() - 1);

      switch (commands[cmdIndex]) {
      case AlterSubcommand::addColumn: {
        const auto column = randomColumn(rand);
        alterSubcommands.emplace_back(
            fmt::format("ADD COLUMN {}", dialect.columnDefinition(column)));
        // we can't accidentally modify / drop new columns in the same
        // statement
        newColumns.push_back(column);
        addedSubcommand = true;
        break;
      }
      case AlterSubcommand::dropColumn: {
        if (working.columns.size() - droppedColumns.size() < 3 ||
            availableColumns.empty()) {
          continue;
        }
        const auto columnIndexIndex = rand.random_number(
            static_cast<std::size_t>(0), availableColumns.size() - 1);
        const auto columnIndex = availableColumns[columnIndexIndex];
        if (columnIndex == 0) {
          break; // do not try to drop the primary key
        }
        if (!dialect.canDropFkColumn() &&
            working.columns[columnIndex].foreign_key_references) {
          break;
        }
        alterSubcommands.emplace_back(
            fmt::format("DROP COLUMN {}", working.columns[columnIndex].name));
        droppedColumnNames.push_back(working.columns[columnIndex].name);
        droppedColumns.push_back(columnIndex);
        availableColumns.erase(availableColumns.begin() +
                               static_cast<std::ptrdiff_t>(columnIndexIndex));
        addedSubcommand = true;
        break;
      }
      case AlterSubcommand::changeColumn: {
        // very simple implementation, we only do numeric -> string
        for (std::size_t colIdx = 0; colIdx < availableColumns.size();
             ++colIdx) {
          auto const &col = working.columns[availableColumns[colIdx]];
          const bool numericColumn = col.type == metadata::ColumnType::INT ||
                                     col.type == metadata::ColumnType::REAL;

          if (numericColumn && !col.foreign_key_references &&
              !col.primary_key) {
            alterSubcommands.emplace_back(
                dialect.alterColumnType(col.name, ColumnType::VARCHAR, 32));
            availableColumns.erase(availableColumns.begin() +
                                   static_cast<std::ptrdiff_t>(colIdx));
            addedSubcommand = true;
            typeChanges.push_back(
                {.name = col.name, .type = ColumnType::VARCHAR, .length = 32});
            break;
          }
        }
        break;
      }
      case AlterSubcommand::changeAccessMethod: {
        if (changingAm) {
          break;
        }
        const auto amIndex = rand.random_number(
            static_cast<std::size_t>(0), config.access_methods.size() - 1);
        alterSubcommands.emplace_back(
            dialect.alterStorage(config.access_methods[amIndex]));
        changingAm = true;
        addedSubcommand = true;
      }
      }
    }
  }

  connection
      ->executeQuery(
          fmt::format("ALTER TABLE {} \n {};", snap->name,
                      boost::algorithm::join(alterSubcommands, ",\n")))
      .maybeThrow();

  const bool wholeIndexDrop = dialect.dropColumnRemovesWholeIndex();

  tables.update(snap->id, [droppedColumnNames, typeChanges, newColumns,
                           wholeIndexDrop](Table &t) {
    for (auto const &name : droppedColumnNames) {
      auto it = std::ranges::find_if(
          t.columns, [&](Column const &c) { return c.name == name; });
      if (it != t.columns.end()) {
        t.columns.erase(it);
      }
      // mirror the server's handling of indexes on the dropped column:
      // pg drops the whole index, mysql just removes the key part
      if (wholeIndexDrop) {
        t.indexes.erase(std::remove_if(t.indexes.begin(), t.indexes.end(),
                                       [&](Index const &idx) {
                                         return std::ranges::any_of(
                                             idx.fields,
                                             [&](IndexColumn const &f) {
                                               return f.column_name == name;
                                             });
                                       }),
                        t.indexes.end());
      } else {
        for (auto &idx : t.indexes) {
          idx.fields.erase(std::remove_if(idx.fields.begin(), idx.fields.end(),
                                          [&](IndexColumn const &f) {
                                            return f.column_name == name;
                                          }),
                           idx.fields.end());
        }
        t.indexes.erase(
            std::remove_if(t.indexes.begin(), t.indexes.end(),
                           [](Index const &idx) { return idx.fields.empty(); }),
            t.indexes.end());
      }
    }
    for (auto const &tc : typeChanges) {
      auto it = std::ranges::find_if(
          t.columns, [&](Column const &c) { return c.name == tc.name; });
      if (it != t.columns.end()) {
        it->type = tc.type;
        it->length = tc.length;
      }
    }
    t.columns.insert(t.columns.end(), newColumns.begin(), newColumns.end());
    return true;
  });
}

RenameTable::RenameTable(DdlConfig config) : config(std::move(config)) {}

void RenameTable::execute(Context &metaCtx, ps_random &rand,
                          sql_variant::LoggedSQL *connection) const {
  auto tables = metaCtx.get<Table>();

  auto snap = tables.randomPick(rand);
  if (snap == nullptr) {
    return;
  }

  const auto newName = fmt::format("foo{}", rand.random_number(1, 1000000));

  connection
      ->executeQuery(
          // no dialect: RENAME TO is portable across pg and mysql
          fmt::format("ALTER TABLE {} RENAME TO {};", snap->name, newName))
      .maybeThrow();

  tables.update(snap->id, [newName](Table &t) {
    t.name = newName;
    return true;
  });
}

CreateIndex::CreateIndex(DdlConfig config) : config(std::move(config)) {}

void CreateIndex::execute(Context &metaCtx, ps_random &rand,
                          sql_variant::LoggedSQL *connection) const {
  // TODO: support partial / functional indexes, and the missing parameters,
  // like null distinct
  auto const serverInfo = connection->serverInfo();
  auto const &dialect = sql_dialect::dialect_for(serverInfo);

  auto tables = metaCtx.get<Table>();

  const std::size_t maxTableIndexes = dialect.maxIndexesPerTable();

  metadata::table_cptr snap;
  for (int remainingTries = 10; remainingTries > 0; remainingTries--) {
    auto candidate = tables.randomPick(rand);
    if (candidate == nullptr) {
      return;
    }
    if (candidate->indexes.size() >= maxTableIndexes) {
      continue;
    }
    snap = candidate;
    break;
  }
  if (snap == nullptr) {
    return;
  }

  metadata::Index newIndex;
  newIndex.name = fmt::format("idx{}", rand.random_number(1, 100000000));

  std::vector<std::size_t> availableColumns(snap->columns.size());
  std::ranges::iota(availableColumns, 0);
  rand.shuffle(availableColumns);

  const auto columnCount =
      rand.random_number(static_cast<std::size_t>(1),
                         std::min<std::size_t>(availableColumns.size() - 1,
                                               dialect.maxIndexColumns()));

  for (std::size_t i = 0; i < columnCount; ++i) {
    const std::string columnName = snap->columns[availableColumns[i]].name;
    const bool ascending = rand.random_bool();
    newIndex.fields.emplace_back(metadata::IndexColumn{
        .column_name = columnName,
        .ordering = ascending ? metadata::IndexOrdering::asc
                              : metadata::IndexOrdering::desc});
  }

  newIndex.unique = rand.random_bool();
  sql_dialect::IndexOptions opts{.concurrent = rand.random_bool(),
                                 .only = rand.random_bool()};

  connection->executeQuery(dialect.createIndex(*snap, newIndex, opts))
      .maybeThrow();

  tables.update(snap->id, [newIndex](Table &t) {
    if (std::ranges::find_if(t.indexes, [&](Index const &i) {
          return i.name == newIndex.name;
        }) == t.indexes.end()) {
      t.indexes.push_back(newIndex);
    }
    return true;
  });
}

DropIndex::DropIndex(DdlConfig config) : config(std::move(config)) {}

void DropIndex::execute(Context &metaCtx, ps_random &rand,
                        sql_variant::LoggedSQL *connection) const {
  auto const serverInfo = connection->serverInfo();
  auto const &dialect = sql_dialect::dialect_for(serverInfo);

  auto tables = metaCtx.get<Table>();

  for (int remainingTries = 10; remainingTries > 0; remainingTries--) {
    auto snap = tables.randomPick(rand);

    if (snap == nullptr || snap->indexes.empty()) {
      continue;
    }

    auto indexIdx = rand.random_number(static_cast<std::size_t>(0),
                                       snap->indexes.size() - 1);
    const std::string indexName = snap->indexes[indexIdx].name;

    auto result =
        connection->executeQuery(dialect.dropIndex(snap->name, indexName));
    if (!result.success()) {
      // mysql: index backs a FK constraint, can't drop standalone, try
      // another index instead of failing the whole action
      if (serverInfo.is_mysql_like() && result.errorInfo.errorCode == "1553") {
        continue;
      }
      result.maybeThrow();
    }

    tables.update(snap->id, [indexName](Table &t) {
      auto it = std::ranges::find_if(
          t.indexes, [&](Index const &i) { return i.name == indexName; });
      if (it != t.indexes.end()) {
        t.indexes.erase(it);
      }
      return true;
    });

    break;
  }
}

CreatePartition::CreatePartition(DdlConfig config)
    : config(std::move(config)) {}

void CreatePartition::execute(Context &metaCtx, ps_random &rand,
                              sql_variant::LoggedSQL *connection) const {
  auto const serverInfo = connection->serverInfo();
  auto const &dialect = sql_dialect::dialect_for(serverInfo);

  auto tables = metaCtx.get<Table>();

  auto snap = findPartitionedTable(tables, rand, config);
  if (snap == nullptr) {
    return;
  }

  std::size_t partIdx = rand.random_number(1, 100000000);

  connection->executeQuery(dialect.addPartition(*snap, partIdx)).maybeThrow();

  tables.update(snap->id, [partIdx](Table &t) {
    if (!t.partitioning.has_value()) {
      return false; // concurrently restructured; skip
    }
    auto &ranges = t.partitioning->ranges;
    if (std::ranges::find_if(ranges, [&](RangePartition const &r) {
          return r.rangebase == partIdx;
        }) != ranges.end()) {
      return false;
    }
    ranges.push_back(RangePartition{partIdx});
    return true;
  });
}

DropPartition::DropPartition(DdlConfig config) : config(std::move(config)) {}

void DropPartition::execute(Context &metaCtx, ps_random &rand,
                            sql_variant::LoggedSQL *connection) const {
  auto const serverInfo = connection->serverInfo();
  auto const &dialect = sql_dialect::dialect_for(serverInfo);

  auto tables = metaCtx.get<Table>();

  auto snap = findPartitionedTable(tables, rand, config);
  if (snap == nullptr) {
    return;
  }

  std::size_t partId = rand.random_number(
      static_cast<std::size_t>(0), snap->partitioning->ranges.size() - 1);
  std::size_t partIdx = snap->partitioning->ranges[partId].rangebase;

  connection->executeQuery(dialect.dropPartition(*snap, partIdx)).maybeThrow();

  tables.update(snap->id, [partIdx](Table &t) {
    if (!t.partitioning.has_value()) {
      return false;
    }
    auto &ranges = t.partitioning->ranges;
    auto it = std::ranges::find_if(ranges, [&](RangePartition const &r) {
      return r.rangebase == partIdx;
    });
    if (it == ranges.end()) {
      return false;
    }
    ranges.erase(it);
    return true;
  });
}
