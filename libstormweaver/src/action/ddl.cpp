
#include "action/ddl.hpp"
#include "action/helper.hpp"

#include <boost/algorithm/string/join.hpp>
#include <fmt/format.h>
#include <rfl.hpp>

using namespace metadata;
using namespace action;

namespace {
ColumnType randomColumnType(ps_random &rand) {
  auto arr = rfl::get_enumerator_array<ColumnType>();
  return arr[rand.random_number(std::size_t(0), arr.size() - 1)].second;
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

std::string columnDefinition(Column const &col) {
  if (col.auto_increment) {
    // assert that this is an int type
    return fmt::format("{} {}", col.name, "SERIAL");
  } else {
    std::string def =
        fmt::format("{} {}", col.name, rfl::enum_to_string(col.type));
    if (col.length > 0) {
      def += fmt::format("({})", col.length);
    }
    if (!col.foreign_key_references.empty()) {
      def += fmt::format(" REFERENCES {} ON DELETE CASCADE",
                         col.foreign_key_references);
    }
    return def;
  }
}

} // namespace

CreateTable::CreateTable(DdlConfig const &config, Table::Type type)
    : config(config), type(type) {}

void CreateTable::setSuccessCallback(TableCallback const &cb) {
  successCallback = cb;
}

void CreateTable::execute(Metadata &metaCtx, ps_random &rand,
                          sql_variant::LoggedSQL *connection) const {
  if (metaCtx.size() >= config.max_table_count) {
    // log error and skip
    return;
  }

  metaCtx.createTable([&](Metadata::Reservation &res) {
    // 1: build definition

    auto table = res.table();

    table->name =
        fmt::format("foo{}", rand.random_number(1, 100000000)); // name;

    const size_t column_count =
        rand.random_number<std::size_t>(2, config.max_column_count);

    const bool partitioned = type == Table::Type::partitioned;

    const bool add_foreign_key = rand.random_number<std::size_t>(1, 100) <=
                                 config.ct_foreign_key_percentage;

    for (size_t idx = 0; idx < column_count; ++idx) {
      const bool primary_key_column = idx == 0;
      const bool foreign_key_column = add_foreign_key && idx == 1;
      table->columns.push_back(
          randomColumn(rand, primary_key_column || foreign_key_column));
    }

    table->columns[0].name = "id";

    table->columns[0].primary_key = true;
    table->columns[0].nullable = false;
    if (partitioned) {
      // with partitioned tables, the primary key won't be a serial as we want
      // to generate random numbers to evenly distribute the partitions
      table->columns[0].partition_key = true;

      table->partitioning = RangePartitioning{};
      table->partitioning->rangeSize = 10000000;
    } else {
      table->columns[0].auto_increment = true;
    }

    if (add_foreign_key) {
      // Foreign keys are always added to the second column (index 1) as a
      // simplification for now
      try {
        auto table_ref = find_random_table(metaCtx, rand);
        table->columns[1].foreign_key_references = table_ref->name;
      } catch (ActionException const &ae) {
      }
    }

    // 2: build & execute SQL statement

    std::vector<std::string> defs;
    std::vector<std::string> pk_columns;

    for (auto const &col : table->columns) {
      if (col.primary_key) {
        pk_columns.push_back(col.name);
      }
      defs.push_back(columnDefinition(col));
    }

    if (!pk_columns.empty()) {
      defs.push_back(fmt::format("PRIMARY KEY ({})",
                                 boost::algorithm::join(pk_columns, ", ")));
    }

    std::string partitionClause = "";

    if (partitioned) {
      partitionClause =
          fmt::format(" PARTITION BY RANGE ({})", table->columns[0].name);
    }

    connection
        ->executeQuery(fmt::format("CREATE TABLE {} ({}) {};", table->name,
                                   boost::algorithm::join(defs, ",\n"),
                                   partitionClause))
        .maybeThrow();

    if (partitioned) {
      auto cnt = rand.random_number(config.min_partition_count,
                                    config.max_partition_count);
      const auto partitionSize = table->partitioning->rangeSize;
      for (std::size_t i = 0; i < cnt; ++i) {
        connection
            ->executeQuery(fmt::format("CREATE TABLE {}_p{} PARTITION OF {} "
                                       "FOR VALUES FROM ({}) TO ({});",
                                       table->name, i, table->name,
                                       partitionSize * i,
                                       partitionSize * (i + 1)))
            .maybeThrow();

        table->partitioning->ranges.push_back(RangePartition{i});
      }
    }

    res.complete();

    if (successCallback) {
      successCallback(table);
    }
  });
}

DropTable::DropTable(DdlConfig const &config) : config(config) {}

void DropTable::execute(Metadata &metaCtx, ps_random &rand,
                        sql_variant::LoggedSQL *connection) const {
  if (metaCtx.size() <= config.min_table_count) {
    // log error and skip
    return;
  }

  auto idx = rand.random_number(std::size_t(0), metaCtx.size() - 1);

  std::string tableName;

  metaCtx.dropTable(idx, [&](Metadata::Reservation &res) {
    if (!res.open())
      return;
    connection
        ->executeQuery(fmt::format("DROP TABLE {} CASCADE;", res.table()->name))
        .maybeThrow();

    tableName = res.table()->name;

    res.complete();
  });

  if (tableName == "")
    return;

  // Make a best effort at trying to remove foreign key references from metadata
  for (std::size_t table_idx = 0; table_idx < metaCtx.size(); ++table_idx) {
    auto table = metaCtx[table_idx];
    if (table && table->hasReferenceTo(tableName)) {
      metaCtx.dropTable(table_idx, [&](Metadata::Reservation &res) {
        if (!res.open())
          return;
        res.table()->removeReferencesTo(tableName);
      });
    }
  }
}

AlterTable::AlterTable(DdlConfig const &config,
                       BitFlags<AlterSubcommand> const &possibleCommands)
    : config(config), possibleCommands(possibleCommands) {}

void AlterTable::execute(Metadata &metaCtx, ps_random &rand,
                         sql_variant::LoggedSQL *connection) const {
  if (metaCtx.size() == 0)
    return;

  auto idx = rand.random_number(std::size_t(0), metaCtx.size() - 1);

  metaCtx.alterTable(idx, [&](Metadata::Reservation &res) {
    if (!res.open())
      return;

    auto table = res.table();

    const auto commands = possibleCommands.All();

    const auto howManySubcommands =
        rand.random_number(std::size_t(1), config.max_alter_clauses);

    std::vector<std::string> alterSubcommands;

    std::vector<std::size_t> availableColumns(table->columns.size());
    std::vector<std::size_t> droppedColumns;
    std::iota(availableColumns.begin(), availableColumns.end(), 0);

    std::vector<Column> newColumns;

    bool changingAm = false;

    for (std::size_t idx = 0; idx < howManySubcommands; ++idx) {
      bool addedSubcommand = false;

      while (!addedSubcommand) {
        const auto cmdIndex =
            rand.random_number(std::size_t(0), commands.size() - 1);

        switch (commands[cmdIndex]) {
        case AlterSubcommand::addColumn: {
          const auto column = randomColumn(rand);
          alterSubcommands.emplace_back(
              fmt::format("ADD COLUMN {}", columnDefinition(column)));
          // we can't accidentally modify / drop new columns in the same
          // statement
          newColumns.push_back(column);
          addedSubcommand = true;
          break;
        }
        case AlterSubcommand::dropColumn: {
          if (table->columns.size() - droppedColumns.size() < 3 ||
              availableColumns.size() < 1)
            continue;
          const auto columnIndexIndex =
              rand.random_number(std::size_t(0), availableColumns.size() - 1);
          const auto columnIndex = availableColumns[columnIndexIndex];
          if (columnIndex == 0)
            break; // do not try to drop the primary key
          alterSubcommands.emplace_back(
              fmt::format("DROP COLUMN {}", table->columns[columnIndex].name));
          droppedColumns.push_back(columnIndex);
          availableColumns.erase(availableColumns.begin() + columnIndexIndex);
          addedSubcommand = true;
          break;
        }
        case AlterSubcommand::changeColumn: {
          // very simple implementation, we only do numeric -> string
          for (std::size_t idx = 0; idx < availableColumns.size(); ++idx) {
            auto &col = table->columns[availableColumns[idx]];
            const bool numericColumn = col.type == metadata::ColumnType::INT ||
                                       col.type == metadata::ColumnType::REAL;

            if (numericColumn && col.foreign_key_references.empty() &&
                !col.primary_key) {
              alterSubcommands.emplace_back(
                  fmt::format("ALTER COLUMN {} TYPE VARCHAR(32)", col.name));
              availableColumns.erase(availableColumns.begin() + idx);
              addedSubcommand = true;
              col.type = ColumnType::VARCHAR;
              col.length = 32;
              break;
            }
          }
          break;
        }
        case AlterSubcommand::changeAccessMethod: {
          if (changingAm)
            break;
          const auto amIndex = rand.random_number(
              std::size_t(0), config.access_methods.size() - 1);
          alterSubcommands.emplace_back(fmt::format(
              "SET ACCESS METHOD {}", config.access_methods[amIndex]));
          changingAm = true;
          addedSubcommand = true;
        }
        }
      }
    }

    std::sort(droppedColumns.begin(), droppedColumns.end(), std::greater<>());
    for (auto const &columnIndex : droppedColumns) {
      table->columns.erase(table->columns.begin() + columnIndex);
    }

    table->columns.insert(table->columns.end(), newColumns.begin(),
                          newColumns.end());

    connection
        ->executeQuery(
            fmt::format("ALTER TABLE {} \n {};", res.table()->name,
                        boost::algorithm::join(alterSubcommands, ",\n")))
        .maybeThrow();

    res.complete();
  });
}

RenameTable::RenameTable(DdlConfig const &config) : config(config) {}

void RenameTable::execute(Metadata &metaCtx, ps_random &rand,
                          sql_variant::LoggedSQL *connection) const {
  if (metaCtx.size() == 0) {
    return;
  }

  auto idx = rand.random_number(std::size_t(0), metaCtx.size() - 1);

  std::string oldTableName;
  std::string newTableName;

  metaCtx.alterTable(idx, [&](Metadata::Reservation &res) {
    if (!res.open())
      return;

    oldTableName = res.table()->name;
    newTableName = fmt::format("foo{}", rand.random_number(1, 1000000));
    res.table()->name = newTableName;

    connection
        ->executeQuery(fmt::format("ALTER TABLE {} RENAME TO {};", oldTableName,
                                   res.table()->name))
        .maybeThrow();

    res.complete();
  });

  if (oldTableName == "")
    return;

  // Make a best effort at trying to update foreign key references in metadata
  for (std::size_t table_idx = 0; table_idx < metaCtx.size(); ++table_idx) {
    auto table = metaCtx[table_idx];
    if (table && table->hasReferenceTo(oldTableName)) {
      metaCtx.dropTable(table_idx, [&](Metadata::Reservation &res) {
        if (!res.open())
          return;
        res.table()->updateReferencesTo(oldTableName, newTableName);
      });
    }
  }
}

CreateIndex::CreateIndex(DdlConfig const &config) : config(config) {}

void CreateIndex::execute(Metadata &metaCtx, ps_random &rand,
                          sql_variant::LoggedSQL *connection) const {
  // TODO: support partial / functional indexes, and the missing parameters,
  // like null distinct
  if (metaCtx.size() == 0) {
    return;
  }

  auto idx = rand.random_number(std::size_t(0), metaCtx.size() - 1);

  metaCtx.alterTable(idx, [&](Metadata::Reservation &res) {
    if (!res.open())
      return;

    metadata::Index newIndex;
    newIndex.name =
        fmt::format("idx{}", rand.random_number(1, 100000000)); // name;

    std::vector<std::size_t> availableColumns(res.table()->columns.size());
    rand.shuffle(availableColumns);

    const auto columnCount = rand.random_number(
        std::size_t(1), std::min<std::size_t>(availableColumns.size() - 1, 32));

    std::vector<std::string> indexColumns;
    for (std::size_t i = 0; i < columnCount; ++i) {
      const std::string columnName =
          res.table()->columns[availableColumns[i]].name;
      const bool ascending = rand.random_bool();
      indexColumns.emplace_back(
          fmt::format("{} {}", columnName, ascending ? "ASC" : "DESC"));
      newIndex.fields.emplace_back(metadata::IndexColumn{
          columnName, ascending ? metadata::IndexOrdering::asc
                                : metadata::IndexOrdering::desc});
    }

    newIndex.unique = rand.random_bool();
    const std::string_view unique = newIndex.unique ? "UNIQUE" : "";
    const std::string_view concurrently =
        rand.random_bool() ? "CONCURRENTLY" : "";
    const std::string_view only = rand.random_bool() ? "ONLY" : "";

    res.table()->indexes.push_back(newIndex);

    connection
        ->executeQuery(fmt::format("CREATE {} INDEX {} {} ON {} {} ({});",
                                   unique, concurrently, newIndex.name, only,
                                   res.table()->name,
                                   boost::algorithm::join(indexColumns, ", ")))
        .maybeThrow();

    res.complete();
  });
}

DropIndex::DropIndex(DdlConfig const &config) : config(config) {}

void DropIndex::execute(Metadata &metaCtx, ps_random &rand,
                        sql_variant::LoggedSQL *connection) const {
  if (metaCtx.size() < 1) {
    // log error and skip
    return;
  }

  for (int remaingTries = 10; remaingTries > 0; remaingTries--) {
    auto idx = rand.random_number(std::size_t(0), metaCtx.size() - 1);

    auto table = metaCtx[idx];

    if (table == nullptr || table->indexes.empty())
      continue;

    if (metaCtx.alterTable(idx, [&](Metadata::Reservation &res) {
          if (!res.open())
            return false;

          auto &indexes = res.table()->indexes;

          if (indexes.empty())
            return false;

          auto indexIdx =
              rand.random_number(std::size_t(0), indexes.size() - 1);

          connection
              ->executeQuery(
                  fmt::format("DROP INDEX {};", indexes[indexIdx].name))
              .maybeThrow();

          indexes.erase(indexes.begin() + indexIdx);

          res.complete();

          return true;
        })) {
      break;
    }
  }
}

static int findPartitionedTable(Metadata &metaCtx, ps_random &rand,
                                DdlConfig const &config) {
  const auto size = metaCtx.size();

  if (size == 0) {
    return -1;
  }

  for (std::size_t i = 0; i < 10; ++i) {
    auto idx = rand.random_number(std::size_t(0), metaCtx.size() - 1);

    auto table = metaCtx[idx];

    if (table == nullptr || !table->partitioning.has_value()) {
      continue;
    }

    auto count = table->partitioning->ranges.size();

    if (count >= config.max_partition_count ||
        count <= config.min_partition_count) {
      continue;
    }

    return idx;
  }

  return -1;
}

CreatePartition::CreatePartition(DdlConfig const &config) : config(config) {}

void CreatePartition::execute(Metadata &metaCtx, ps_random &rand,
                              sql_variant::LoggedSQL *connection) const {

  auto idx = findPartitionedTable(metaCtx, rand, config);

  if (idx == -1)
    return;

  metaCtx.alterTable(idx, [&](Metadata::Reservation &res) {
    if (!res.open())
      return;

    auto table = res.table();

    if (!table->partitioning.has_value())
      return;

    auto currentCount = table->partitioning->ranges.size();
    if (currentCount >= config.max_partition_count) {
      return;
    }

    std::size_t partIdx = rand.random_number(1, 100000000);
    const auto partitionSize = table->partitioning->rangeSize;
    connection
        ->executeQuery(fmt::format(
            "CREATE TABLE {}_p{} PARTITION OF {} FOR VALUES FROM ({}) TO ({});",
            table->name, partIdx, table->name, partitionSize * partIdx,
            partitionSize * (partIdx + 1)))
        .maybeThrow();

    table->partitioning->ranges.push_back(RangePartition{partIdx});

    res.complete();
  });
}

DropPartition::DropPartition(DdlConfig const &config) : config(config) {}

void DropPartition::execute(Metadata &metaCtx, ps_random &rand,
                            sql_variant::LoggedSQL *connection) const {

  auto idx = findPartitionedTable(metaCtx, rand, config);

  if (idx == -1)
    return;

  metaCtx.alterTable(idx, [&](Metadata::Reservation &res) {
    if (!res.open())
      return;

    auto table = res.table();

    if (!table->partitioning.has_value())
      return;

    auto currentCount = table->partitioning->ranges.size();
    if (currentCount <= config.min_partition_count) {
      return;
    }

    std::size_t partId = rand.random_number(
        std::size_t(0), table->partitioning->ranges.size() - 1);

    std::size_t partIdx = table->partitioning->ranges[partId].rangebase;

    connection
        ->executeQuery(
            fmt::format("DROP TABLE {}_p{} CASCADE;", table->name, partIdx))
        .maybeThrow();

    table->partitioning->ranges.erase(table->partitioning->ranges.begin() +
                                      partId);

    res.complete();
  });
}
