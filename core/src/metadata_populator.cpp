#include "metadata_populator.hpp"

#include <algorithm>
#include <spdlog/spdlog.h>

namespace metadata_populator {

MetadataPopulator::MetadataPopulator(metadata::TableRegistry &registry)
    : registry_(registry) {}

void MetadataPopulator::populateFromExistingDatabase(
    schema_discovery::SchemaDiscovery &discovery) {
  auto tables = discovery.discoverTables();

  spdlog::info("Starting metadata population for {} discovered tables",
               tables.size());

  struct PendingForeignKey {
    metadata::ObjectId table;
    std::string column;
    std::string referencedTable;
  };
  std::vector<PendingForeignKey> pendingFks;

  auto &catalog = registry_.get<metadata::Table>();

  for (const auto &discovered_table : tables) {
    try {
      fk_list_t fkColumns;
      auto table = convertCompleteTable(discovery, discovered_table, fkColumns);
      table.id = registry_.nextId();
      const auto tableId = table.id;

      if (!catalog.insert(std::move(table))) {
        spdlog::warn("Duplicate metadata entry for table {}, skipping",
                     discovered_table.name);
        continue;
      }

      for (auto &fk : fkColumns) {
        pendingFks.push_back({.table = tableId,
                              .column = std::move(fk.first),
                              .referencedTable = std::move(fk.second)});
      }

      spdlog::debug("Successfully populated metadata for table {}",
                    discovered_table.name);

    } catch (const std::exception &e) {
      spdlog::error("Failed to populate metadata for table {}: {}",
                    discovered_table.name, e.what());
    }
  }

  // second pass: names -> ids, now that every table is present
  for (const auto &fk : pendingFks) {
    auto target = catalog.byName(fk.referencedTable);
    if (target == nullptr) {
      spdlog::warn("Foreign key target {} not found while populating",
                   fk.referencedTable);
      continue;
    }
    catalog.update(fk.table, [&](metadata::Table &t) {
      for (auto &col : t.columns) {
        if (col.name == fk.column) {
          col.foreign_key_references =
              metadata::Ref<metadata::Table>{target->id};
        }
      }
      return true;
    });
  }

  spdlog::info("Metadata population completed for {} tables", catalog.size());
}

metadata::Table MetadataPopulator::convertCompleteTable(
    schema_discovery::SchemaDiscovery &discovery,
    const schema_discovery::DiscoveredTable &discovered_table,
    fk_list_t &fkColumns) {
  metadata::Table table;
  table.name = discovered_table.name;
  table.tablespace = discovered_table.tablespace;

  // TODO: ddl operations do not set the engine field yet
  // table.engine = discovered_table.access_method;

  if (discovered_table.table_type == metadata::Table::Type::partitioned) {
    // Partitioned table - we'll set this up below when we discover partitions
  }

  auto columns = discovery.discoverColumns(discovered_table.name);
  for (const auto &discovered_col : columns) {
    table.columns.push_back(convertColumn(discovered_col));
  }

  auto indexes = discovery.discoverIndexes(discovered_table.name);
  for (const auto &discovered_idx : indexes) {
    table.indexes.push_back(convertIndex(discovered_idx));
  }

  // Apply primary key flags to columns
  auto constraints = discovery.discoverConstraints(discovered_table.name);
  applyConstraints(table, constraints, fkColumns);

  // Mark columns as partition keys
  auto partition_keys = discovery.discoverPartitionKeys(discovered_table.name);
  applyPartitionKeys(table, partition_keys);

  auto partitions = discovery.discoverPartitions(discovered_table.name);
  if (!partitions.empty()) {
    applyPartitioning(table, partitions);
  }

  spdlog::debug("Converted table {} with {} columns, {} indexes, {} "
                "constraints, {} partitions",
                table.name, table.columns.size(), table.indexes.size(),
                constraints.size(), partitions.size());

  return table;
}

metadata::Column MetadataPopulator::convertColumn(
    const schema_discovery::DiscoveredColumn &discovered) {
  metadata::Column column;

  column.name = discovered.name;
  column.type = discovered.data_type;
  column.length = static_cast<std::size_t>(std::max(0, discovered.length));
  column.nullable = !discovered.not_null;
  column.auto_increment = discovered.is_serial;

  // Skip default values for auto_increment columns (SERIAL in PostgreSQL)
  if (!discovered.default_value.empty() && !discovered.is_serial) {
    column.default_value = discovered.default_value;
  }

  column.generated = discovered.generated_type;

  return column;
}

metadata::Index MetadataPopulator::convertIndex(
    const schema_discovery::DiscoveredIndex &discovered) {
  metadata::Index index;

  index.name = discovered.name;
  index.unique = discovered.is_unique;

  for (std::size_t i = 0; i < discovered.column_names.size(); ++i) {
    metadata::IndexColumn idx_col;
    idx_col.column_name = discovered.column_names[i];

    if (i < discovered.orderings.size()) {
      idx_col.ordering = discovered.orderings[i];
    } else {
      idx_col.ordering = metadata::IndexOrdering::default_;
    }

    index.fields.push_back(idx_col);
  }

  return index;
}

void MetadataPopulator::applyConstraints(
    metadata::Table &table,
    const std::vector<schema_discovery::DiscoveredConstraint> &constraints,
    fk_list_t &fkColumns) {
  for (const auto &constraint : constraints) {
    if (constraint.type == schema_discovery::ConstraintType::primary_key) {
      for (const auto &col_name : constraint.columns) {
        auto it = std::ranges::find_if(
            table.columns, [&col_name](const metadata::Column &col) {
              return col.name == col_name;
            });
        if (it != table.columns.end()) {
          it->primary_key = true;
          spdlog::debug("Marked column {} as primary key", col_name);
        }
      }
    } else if (constraint.type ==
               schema_discovery::ConstraintType::foreign_key) {
      for (const auto &col_name : constraint.columns) {
        fkColumns.emplace_back(col_name, constraint.referenced_table);
      }
    }
    // Note: Other constraint types (UNIQUE, CHECK) are not currently
    // represented in StormWeaver's metadata structure, so we skip them
  }
}

void MetadataPopulator::applyPartitionKeys(
    metadata::Table &table, const std::vector<std::string> &partition_keys) {
  for (const auto &key_col_name : partition_keys) {
    auto it = std::ranges::find_if(
        table.columns, [&key_col_name](const metadata::Column &col) {
          return col.name == key_col_name;
        });
    if (it != table.columns.end()) {
      it->partition_key = true;
      spdlog::debug("Marked column {} as partition key", key_col_name);
    } else {
      spdlog::warn("Partition key column {} not found in table {}",
                   key_col_name, table.name);
    }
  }

  spdlog::debug("Applied {} partition keys to table {}", partition_keys.size(),
                table.name);
}

void MetadataPopulator::applyPartitioning(
    metadata::Table &table,
    const std::vector<schema_discovery::DiscoveredPartition> &partitions) {
  if (partitions.empty()) {
    return;
  }

  // Currently only supports basic range partitioning
  table.partitioning = metadata::RangePartitioning{};
  table.partitioning->rangeSize = 10000000; // Default range size

  for (const auto &partition : partitions) {
    metadata::RangePartition range_partition{0};
    // Extract range base from partition name (format: "table_name_p0",
    // "table_name_p1", etc.)
    std::size_t pos = partition.name.rfind('_');
    if (pos != std::string::npos && pos + 1 < partition.name.length()) {
      try {
        std::string range_str = partition.name.substr(pos + 1);
        if (range_str[0] == 'p') {
          range_partition.rangebase = std::stoull(range_str.substr(1));
        }
      } catch (const std::exception &e) {
        spdlog::warn("Could not parse range base from partition name {}: {}",
                     partition.name, e.what());
        range_partition.rangebase = 0;
      }
    }

    table.partitioning->ranges.push_back(range_partition);
    spdlog::debug("Added partition {} with range base {}", partition.name,
                  range_partition.rangebase);
  }

  spdlog::debug("Applied partitioning to table {} with {} partitions",
                table.name, table.partitioning->ranges.size());
}

} // namespace metadata_populator
