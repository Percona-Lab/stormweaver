#include "schema_discovery.hpp"

#include <fmt/format.h>
#include <map>
#include <spdlog/spdlog.h>
#include <sstream>

namespace schema_discovery {

SchemaDiscovery::SchemaDiscovery(sql_variant::LoggedSQL *connection)
    : connection_(connection) {
  if (!connection_) {
    throw std::invalid_argument("Connection cannot be null");
  }
}

std::vector<DiscoveredTable> SchemaDiscovery::discoverTables() {
  std::vector<DiscoveredTable> tables;

  const std::string query = R"(
        SELECT 
          c.relname as table_name,
          c.relkind as table_type,
          COALESCE(am.amname, 'heap') as access_method,
          COALESCE(ts.spcname, 'pg_default') as tablespace,
          c.relpartbound IS NOT NULL as is_partition,
          CASE WHEN c.relkind = 'p' THEN 'RANGE' ELSE '' END as partition_type
        FROM pg_class c
        LEFT JOIN pg_am am ON c.relam = am.oid
        LEFT JOIN pg_tablespace ts ON c.reltablespace = ts.oid
        WHERE c.relkind IN ('r', 'p')
          AND c.relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'public')
          AND NOT c.relispartition
        ORDER BY c.relname
    )";

  try {
    auto result = connection_->executeQuery(query);
    result.maybeThrow();

    if (result.data) {
      for (std::size_t i = 0; i < result.data->numRows(); ++i) {
        auto row = result.data->nextRow();

        DiscoveredTable table;
        table.name = std::string(row.rowData[0].value_or(""));
        table.table_type =
            parseTableType(std::string(row.rowData[1].value_or("")));
        table.access_method =
            parseAccessMethod(std::string(row.rowData[2].value_or("heap")));
        table.tablespace =
            parseTablespace(std::string(row.rowData[3].value_or("pg_default")));
        table.is_partition = (row.rowData[4].value_or("f") == "t");
        table.partition_type =
            parsePartitionType(std::string(row.rowData[5].value_or("")));

        tables.push_back(table);
      }
    }

    spdlog::debug("Discovered {} tables", tables.size());

  } catch (const std::exception &e) {
    spdlog::error("Failed to discover tables: {}", e.what());
    throw;
  }

  return tables;
}

std::vector<DiscoveredColumn>
SchemaDiscovery::discoverColumns(const std::string &table_name) {
  std::vector<DiscoveredColumn> columns;

  const std::string query = fmt::format(R"(
        SELECT 
          a.attname as column_name,
          t.typname as data_type,
          a.attlen as length,
          a.atttypmod as type_modifier,
          a.attnotnull as not_null,
          a.attnum as ordinal_position,
          CASE WHEN pg_get_expr(ad.adbin, ad.adrelid) LIKE 'nextval%' THEN true ELSE false END as is_serial,
          CASE WHEN a.attgenerated = 's' THEN 'stored'
               WHEN a.attgenerated = 'v' THEN 'virtual'
               ELSE 'not_generated' END as generated_type,
          COALESCE(pg_get_expr(ad.adbin, ad.adrelid), '') as default_value
        FROM pg_attribute a
        JOIN pg_type t ON a.atttypid = t.oid
        LEFT JOIN pg_attrdef ad ON a.attrelid = ad.adrelid AND a.attnum = ad.adnum
        WHERE a.attrelid = (
            SELECT c.oid FROM pg_class c 
            JOIN pg_namespace n ON c.relnamespace = n.oid 
            WHERE c.relname = '{}' AND n.nspname = 'public'
        )
          AND a.attnum > 0
          AND NOT a.attisdropped
        ORDER BY a.attnum
    )",
                                        table_name);

  try {
    auto result = connection_->executeQuery(query);
    result.maybeThrow();

    if (result.data) {
      for (std::size_t i = 0; i < result.data->numRows(); ++i) {
        auto row = result.data->nextRow();

        DiscoveredColumn column;
        column.name = std::string(row.rowData[0].value_or(""));
        column.data_type =
            parseDataType(std::string(row.rowData[1].value_or("")));
        column.type_modifier =
            row.rowData[3]
                ? std::stoi(std::string(row.rowData[3].value_or("-1")))
                : -1;
        column.not_null = (row.rowData[4].value_or("f") == "t");
        column.ordinal_position =
            row.rowData[5]
                ? std::stoi(std::string(row.rowData[5].value_or("0")))
                : 0;
        column.is_serial = (row.rowData[6].value_or("f") == "t");
        column.generated_type = parseGeneratedType(
            std::string(row.rowData[7].value_or("not_generated")));
        column.default_value = std::string(row.rowData[8].value_or(""));

        if (column.data_type == metadata::ColumnType::VARCHAR ||
            column.data_type == metadata::ColumnType::CHAR) {
          column.length = parseTypeModifier(
              std::string(row.rowData[1].value_or("")), column.type_modifier);
        } else {
          column.length = 0;
        }

        columns.push_back(column);
      }
    }

    spdlog::debug("Discovered {} columns for table {}", columns.size(),
                  table_name);

  } catch (const std::exception &e) {
    spdlog::error("Failed to discover columns for table {}: {}", table_name,
                  e.what());
    throw;
  }

  return columns;
}

std::vector<DiscoveredIndex>
SchemaDiscovery::discoverIndexes(const std::string &table_name) {
  std::vector<DiscoveredIndex> indexes;

  const std::string query = fmt::format(R"(
        SELECT 
          i.relname as index_name,
          ix.indisunique as is_unique,
          a.attname as column_name,
          array_position(ix.indkey, a.attnum) as key_position,
          pg_get_indexdef(ix.indexrelid) as index_def
        FROM pg_index ix
        JOIN pg_class i ON ix.indexrelid = i.oid
        JOIN pg_class t ON ix.indrelid = t.oid
        JOIN pg_attribute a ON t.oid = a.attrelid AND a.attnum = ANY(ix.indkey)
        JOIN pg_namespace n ON t.relnamespace = n.oid
        WHERE t.relname = '{}' 
          AND n.nspname = 'public'
          AND NOT ix.indisprimary
        ORDER BY i.relname, array_position(ix.indkey, a.attnum)
    )",
                                        table_name, table_name);

  try {
    auto result = connection_->executeQuery(query);
    result.maybeThrow();

    if (result.data) {
      std::map<std::string, DiscoveredIndex> index_map;

      for (std::size_t i = 0; i < result.data->numRows(); ++i) {
        auto row = result.data->nextRow();

        std::string index_name = std::string(row.rowData[0].value_or(""));
        bool is_unique = (row.rowData[1].value_or("f") == "t");
        std::string column_name = std::string(row.rowData[2].value_or(""));
        std::string index_def = std::string(row.rowData[4].value_or(""));

        std::string ordering = "asc";
        std::string search_pattern = column_name + " DESC";
        if (index_def.find(search_pattern) != std::string::npos) {
          ordering = "desc";
        }

        auto it = index_map.find(index_name);
        if (it == index_map.end()) {
          DiscoveredIndex new_index;
          new_index.name = index_name;
          new_index.is_unique = is_unique;
          index_map[index_name] = new_index;
        }

        index_map[index_name].column_names.push_back(column_name);
        index_map[index_name].orderings.push_back(parseIndexOrdering(ordering));
      }

      for (const auto &pair : index_map) {
        indexes.push_back(pair.second);
      }
    }

    spdlog::debug("Discovered {} indexes for table {}", indexes.size(),
                  table_name);

  } catch (const std::exception &e) {
    spdlog::error("Failed to discover indexes for table {}: {}", table_name,
                  e.what());
    throw;
  }

  return indexes;
}

std::vector<DiscoveredConstraint>
SchemaDiscovery::discoverConstraints(const std::string &table_name) {
  std::vector<DiscoveredConstraint> constraints;

  const std::string query = fmt::format(R"(
        SELECT 
          c.conname as constraint_name,
          c.contype as constraint_type,
          array_to_string(array(
            SELECT a.attname
            FROM pg_attribute a
            WHERE a.attrelid = c.conrelid 
              AND a.attnum = ANY(c.conkey)
            ORDER BY array_position(c.conkey, a.attnum)
          ), ',') as column_names,
          COALESCE(
            CASE 
              WHEN ft.relispartition = true THEN parent_ft.relname
              ELSE ft.relname 
            END, 
            ''
          ) as referenced_table,
          COALESCE(array_to_string(array(
            SELECT fa.attname
            FROM pg_attribute fa
            WHERE fa.attrelid = c.confrelid 
              AND fa.attnum = ANY(c.confkey)
            ORDER BY array_position(c.confkey, fa.attnum)
          ), ','), '') as referenced_columns
        FROM pg_constraint c
        JOIN pg_class t ON c.conrelid = t.oid
        LEFT JOIN pg_class ft ON c.confrelid = ft.oid
        LEFT JOIN pg_inherits inh ON ft.oid = inh.inhrelid AND ft.relispartition = true
        LEFT JOIN pg_class parent_ft ON inh.inhparent = parent_ft.oid
        JOIN pg_namespace n ON t.relnamespace = n.oid
        WHERE t.relname = '{}' 
          AND n.nspname = 'public'
          AND c.contype IN ('p', 'u', 'c', 'f')
        ORDER BY c.conname
    )",
                                        table_name);

  try {
    auto result = connection_->executeQuery(query);
    result.maybeThrow();

    if (result.data) {
      for (std::size_t i = 0; i < result.data->numRows(); ++i) {
        auto row = result.data->nextRow();

        DiscoveredConstraint constraint;
        constraint.name = std::string(row.rowData[0].value_or(""));
        constraint.type =
            parseConstraintType(std::string(row.rowData[1].value_or("")));

        std::string column_names_str = std::string(row.rowData[2].value_or(""));
        std::stringstream ss(column_names_str);
        std::string item;

        while (std::getline(ss, item, ',')) {
          if (!item.empty()) {
            constraint.columns.push_back(item);
          }
        }

        constraint.referenced_table = std::string(row.rowData[3].value_or(""));
        std::string referenced_columns_str =
            std::string(row.rowData[4].value_or(""));
        if (!referenced_columns_str.empty()) {
          std::stringstream ref_ss(referenced_columns_str);
          std::string ref_item;

          while (std::getline(ref_ss, ref_item, ',')) {
            if (!ref_item.empty()) {
              constraint.referenced_columns.push_back(ref_item);
            }
          }
        }

        constraints.push_back(constraint);
      }
    }

    spdlog::debug("Discovered {} constraints for table {}", constraints.size(),
                  table_name);

  } catch (const std::exception &e) {
    spdlog::error("Failed to discover constraints for table {}: {}", table_name,
                  e.what());
    throw;
  }

  return constraints;
}

std::vector<DiscoveredPartition>
SchemaDiscovery::discoverPartitions(const std::string &table_name) {
  std::vector<DiscoveredPartition> partitions;

  const std::string query = fmt::format(R"(
        SELECT 
          child.relname as partition_name,
          pg_get_expr(child.relpartbound, child.oid) as partition_bound
        FROM pg_class parent
        JOIN pg_namespace parent_ns ON parent.relnamespace = parent_ns.oid
        JOIN pg_inherits inh ON parent.oid = inh.inhparent
        JOIN pg_class child ON inh.inhrelid = child.oid
        JOIN pg_namespace child_ns ON child.relnamespace = child_ns.oid
        WHERE parent.relname = '{}'
          AND parent_ns.nspname = 'public'
          AND child_ns.nspname = 'public'
          AND child.relispartition = true
        ORDER BY child.relname
    )",
                                        table_name);

  try {
    auto result = connection_->executeQuery(query);
    result.maybeThrow();

    if (result.data) {
      for (std::size_t i = 0; i < result.data->numRows(); ++i) {
        auto row = result.data->nextRow();

        DiscoveredPartition partition;
        partition.name = std::string(row.rowData[0].value_or(""));
        partition.partition_bound = std::string(row.rowData[1].value_or(""));

        partitions.push_back(partition);
      }
    }

    spdlog::debug("Discovered {} partitions for table {}", partitions.size(),
                  table_name);

  } catch (const std::exception &e) {
    spdlog::error("Failed to discover partitions for table {}: {}", table_name,
                  e.what());
    throw;
  }

  return partitions;
}

std::vector<std::string>
SchemaDiscovery::discoverPartitionKeys(const std::string &table_name) {
  std::vector<std::string> partition_keys;

  const std::string query = fmt::format(R"(
        SELECT a.attname as column_name
        FROM pg_class c
        JOIN pg_namespace n ON c.relnamespace = n.oid
        JOIN pg_partitioned_table pt ON c.oid = pt.partrelid
        JOIN pg_attribute a ON c.oid = a.attrelid
        WHERE c.relname = '{}'
          AND n.nspname = 'public'
          AND a.attnum = ANY(pt.partattrs)
        ORDER BY array_position(pt.partattrs, a.attnum)
    )",
                                        table_name);

  try {
    auto result = connection_->executeQuery(query);
    result.maybeThrow();

    if (result.data) {
      for (std::size_t i = 0; i < result.data->numRows(); ++i) {
        auto row = result.data->nextRow();

        std::string column_name = std::string(row.rowData[0].value_or(""));
        if (!column_name.empty()) {
          partition_keys.push_back(column_name);
        }
      }
    }

    spdlog::debug("Discovered {} partition key columns for table {}",
                  partition_keys.size(), table_name);

  } catch (const std::exception &e) {
    spdlog::error("Failed to discover partition keys for table {}: {}",
                  table_name, e.what());
    throw;
  }

  return partition_keys;
}

std::string SchemaDiscovery::parseAccessMethod(const std::string &am_name) {
  return am_name;
}

std::string SchemaDiscovery::parseTablespace(const std::string &ts_name) {
  return (ts_name == "pg_default") ? "" : ts_name;
}

int SchemaDiscovery::parseTypeModifier(const std::string &type_name,
                                       int type_modifier) {
  if ((type_name == "varchar" || type_name == "bpchar") && type_modifier >= 4) {
    return type_modifier - 4; // PostgreSQL stores length + 4 in type modifier
  }
  return 0;
}

metadata::Table::Type
SchemaDiscovery::parseTableType(const std::string &type_char) {
  if (type_char == "r")
    return metadata::Table::Type::normal;
  if (type_char == "p")
    return metadata::Table::Type::partitioned;
  return metadata::Table::Type::normal;
}

PartitionType
SchemaDiscovery::parsePartitionType(const std::string &partition_type_str) {
  if (partition_type_str == "RANGE")
    return PartitionType::range;
  if (partition_type_str == "HASH")
    return PartitionType::hash;
  if (partition_type_str == "LIST")
    return PartitionType::list;
  return PartitionType::none;
}

metadata::ColumnType
SchemaDiscovery::parseDataType(const std::string &type_name) {
  if (type_name == "int2" || type_name == "int4" || type_name == "int8")
    return metadata::ColumnType::INT;
  if (type_name == "varchar")
    return metadata::ColumnType::VARCHAR;
  if (type_name == "bpchar")
    return metadata::ColumnType::CHAR;
  if (type_name == "text")
    return metadata::ColumnType::TEXT;
  if (type_name == "float4" || type_name == "float8")
    return metadata::ColumnType::REAL;
  if (type_name == "bool")
    return metadata::ColumnType::BOOL;
  if (type_name == "bytea")
    return metadata::ColumnType::BYTEA;
  // For timestamp, date, and unknown types, map to TEXT as fallback
  return metadata::ColumnType::TEXT;
}

metadata::Generated
SchemaDiscovery::parseGeneratedType(const std::string &generated_str) {
  if (generated_str == "stored")
    return metadata::Generated::stored;
  if (generated_str == "virtual")
    return metadata::Generated::virt;
  return metadata::Generated::notGenerated;
}

metadata::IndexOrdering
SchemaDiscovery::parseIndexOrdering(const std::string &ordering_str) {
  if (ordering_str == "desc")
    return metadata::IndexOrdering::desc;
  return metadata::IndexOrdering::asc;
}

ConstraintType
SchemaDiscovery::parseConstraintType(const std::string &type_char) {
  if (type_char == "p")
    return ConstraintType::primary_key;
  if (type_char == "f")
    return ConstraintType::foreign_key;
  if (type_char == "u")
    return ConstraintType::unique;
  if (type_char == "c")
    return ConstraintType::check;
  return ConstraintType::unknown;
}

} // namespace schema_discovery
