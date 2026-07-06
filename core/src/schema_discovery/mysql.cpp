#include "schema_discovery.hpp"

#include <fmt/format.h>
#include <map>
#include <spdlog/spdlog.h>
#include <sstream>

namespace schema_discovery {

MySqlSchemaDiscovery::MySqlSchemaDiscovery(sql_variant::LoggedSQL *connection)
    : connection_(connection) {
  if (connection_ == nullptr) {
    throw std::invalid_argument("Connection cannot be null");
  }
}

std::vector<DiscoveredTable> MySqlSchemaDiscovery::discoverTables() {
  std::vector<DiscoveredTable> tables;

  const std::string query = R"(
        SELECT t.TABLE_NAME,
               CASE WHEN MAX(p.PARTITION_NAME) IS NULL THEN 'r' ELSE 'p' END AS table_type,
               t.ENGINE AS access_method,
               '' AS tablespace,
               0 AS is_partition,
               COALESCE(MAX(p.PARTITION_METHOD), '') AS partition_type
        FROM information_schema.TABLES t
        LEFT JOIN information_schema.PARTITIONS p
          ON p.TABLE_SCHEMA = t.TABLE_SCHEMA AND p.TABLE_NAME = t.TABLE_NAME
         AND p.PARTITION_NAME IS NOT NULL
        WHERE t.TABLE_SCHEMA = DATABASE() AND t.TABLE_TYPE = 'BASE TABLE'
        GROUP BY t.TABLE_NAME, t.ENGINE
        ORDER BY t.TABLE_NAME
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
        table.access_method = std::string(row.rowData[2].value_or(""));
        table.tablespace = std::string(row.rowData[3].value_or(""));
        table.is_partition = false;
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
MySqlSchemaDiscovery::discoverColumns(const std::string &table_name) {
  std::vector<DiscoveredColumn> columns;

  const std::string query = fmt::format(R"(
        SELECT COLUMN_NAME,
               DATA_TYPE,
               COALESCE(CHARACTER_MAXIMUM_LENGTH, 0) AS length,
               -1 AS type_modifier,
               IF(IS_NULLABLE = 'NO', 1, 0) AS not_null,
               ORDINAL_POSITION,
               IF(EXTRA LIKE '%auto_increment%', 1, 0) AS is_serial,
               CASE WHEN EXTRA LIKE '%STORED GENERATED%' THEN 'stored'
                    WHEN EXTRA LIKE '%VIRTUAL GENERATED%' THEN 'virtual'
                    ELSE 'not_generated' END AS generated_type,
               COALESCE(COLUMN_DEFAULT, '') AS default_value
        FROM information_schema.COLUMNS
        WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}'
        ORDER BY ORDINAL_POSITION
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
        column.not_null = (row.rowData[4].value_or("0") == "1");
        column.ordinal_position =
            row.rowData[5]
                ? std::stoi(std::string(row.rowData[5].value_or("0")))
                : 0;
        column.is_serial = (row.rowData[6].value_or("0") == "1");
        column.generated_type = parseGeneratedType(
            std::string(row.rowData[7].value_or("not_generated")));
        column.default_value = std::string(row.rowData[8].value_or(""));

        if (column.data_type == metadata::ColumnType::VARCHAR ||
            column.data_type == metadata::ColumnType::CHAR) {
          column.length =
              row.rowData[2]
                  ? std::stoi(std::string(row.rowData[2].value_or("0")))
                  : 0;
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
MySqlSchemaDiscovery::discoverIndexes(const std::string &table_name) {
  std::vector<DiscoveredIndex> indexes;

  const std::string query = fmt::format(R"(
        SELECT INDEX_NAME,
               IF(NON_UNIQUE = 0, 1, 0) AS is_unique,
               COLUMN_NAME,
               SEQ_IN_INDEX,
               COALESCE(COLLATION, 'A') AS collation
        FROM information_schema.STATISTICS
        WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}' AND INDEX_NAME <> 'PRIMARY'
        ORDER BY INDEX_NAME, SEQ_IN_INDEX
    )",
                                        table_name);

  try {
    auto result = connection_->executeQuery(query);
    result.maybeThrow();

    if (result.data) {
      std::map<std::string, DiscoveredIndex> index_map;

      for (std::size_t i = 0; i < result.data->numRows(); ++i) {
        auto row = result.data->nextRow();

        std::string index_name = std::string(row.rowData[0].value_or(""));
        bool is_unique = (row.rowData[1].value_or("0") == "1");
        std::string column_name = std::string(row.rowData[2].value_or(""));
        std::string collation = std::string(row.rowData[4].value_or("A"));

        std::string ordering = (collation == "D") ? "desc" : "asc";

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
MySqlSchemaDiscovery::discoverConstraints(const std::string &table_name) {
  std::vector<DiscoveredConstraint> constraints;

  const std::string query = fmt::format(R"(
        SELECT tc.CONSTRAINT_NAME,
               tc.CONSTRAINT_TYPE,
               GROUP_CONCAT(kcu.COLUMN_NAME ORDER BY kcu.ORDINAL_POSITION) AS column_names,
               COALESCE(MAX(kcu.REFERENCED_TABLE_NAME), '') AS referenced_table,
               COALESCE(GROUP_CONCAT(kcu.REFERENCED_COLUMN_NAME
                                     ORDER BY kcu.ORDINAL_POSITION), '') AS referenced_columns
        FROM information_schema.TABLE_CONSTRAINTS tc
        JOIN information_schema.KEY_COLUMN_USAGE kcu
          ON kcu.CONSTRAINT_SCHEMA = tc.CONSTRAINT_SCHEMA
         AND kcu.CONSTRAINT_NAME = tc.CONSTRAINT_NAME
         AND kcu.TABLE_NAME = tc.TABLE_NAME
        WHERE tc.TABLE_SCHEMA = DATABASE() AND tc.TABLE_NAME = '{}'
          AND tc.CONSTRAINT_TYPE IN ('PRIMARY KEY', 'FOREIGN KEY', 'UNIQUE')
        GROUP BY tc.CONSTRAINT_NAME, tc.CONSTRAINT_TYPE
        ORDER BY tc.CONSTRAINT_NAME
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

    // check constraints don't show up in KEY_COLUMN_USAGE (they're not
    // key-based), pull them separately from CHECK_CONSTRAINTS
    const std::string checkQuery = fmt::format(R"(
        SELECT cc.CONSTRAINT_NAME
        FROM information_schema.CHECK_CONSTRAINTS cc
        JOIN information_schema.TABLE_CONSTRAINTS tc
          ON tc.CONSTRAINT_SCHEMA = cc.CONSTRAINT_SCHEMA
         AND tc.CONSTRAINT_NAME = cc.CONSTRAINT_NAME
        WHERE tc.TABLE_SCHEMA = DATABASE() AND tc.TABLE_NAME = '{}'
          AND tc.CONSTRAINT_TYPE = 'CHECK'
        ORDER BY cc.CONSTRAINT_NAME
    )",
                                               table_name);

    auto checkResult = connection_->executeQuery(checkQuery);
    checkResult.maybeThrow();

    if (checkResult.data) {
      for (std::size_t i = 0; i < checkResult.data->numRows(); ++i) {
        auto row = checkResult.data->nextRow();

        DiscoveredConstraint constraint;
        constraint.name = std::string(row.rowData[0].value_or(""));
        constraint.type = ConstraintType::check;

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
MySqlSchemaDiscovery::discoverPartitions(const std::string &table_name) {
  std::vector<DiscoveredPartition> partitions;

  const std::string query = fmt::format(R"(
        SELECT PARTITION_NAME,
               CONCAT('VALUES LESS THAN (', PARTITION_DESCRIPTION, ')') AS partition_bound
        FROM information_schema.PARTITIONS
        WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}' AND PARTITION_NAME IS NOT NULL
        ORDER BY PARTITION_ORDINAL_POSITION
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
MySqlSchemaDiscovery::discoverPartitionKeys(const std::string &table_name) {
  std::vector<std::string> partition_keys;

  const std::string query = fmt::format(R"(
        SELECT DISTINCT PARTITION_EXPRESSION
        FROM information_schema.PARTITIONS
        WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}'
          AND PARTITION_EXPRESSION IS NOT NULL
    )",
                                        table_name);

  try {
    auto result = connection_->executeQuery(query);
    result.maybeThrow();

    if (result.data) {
      for (std::size_t i = 0; i < result.data->numRows(); ++i) {
        auto row = result.data->nextRow();

        // multi-column keys come back as one `a`,`b` expression string
        std::string expr = std::string(row.rowData[0].value_or(""));
        std::erase(expr, '`');
        std::stringstream ss(expr);
        std::string item;
        while (std::getline(ss, item, ',')) {
          if (!item.empty()) {
            partition_keys.push_back(item);
          }
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

metadata::Table::Type
MySqlSchemaDiscovery::parseTableType(const std::string &type_char) {
  if (type_char == "r") {
    return metadata::Table::Type::normal;
  }
  if (type_char == "p") {
    return metadata::Table::Type::partitioned;
  }
  return metadata::Table::Type::normal;
}

PartitionType MySqlSchemaDiscovery::parsePartitionType(
    const std::string &partition_type_str) {
  if (partition_type_str == "RANGE") {
    return PartitionType::range;
  }
  if (partition_type_str == "HASH") {
    return PartitionType::hash;
  }
  if (partition_type_str == "LIST") {
    return PartitionType::list;
  }
  return PartitionType::none;
}

metadata::ColumnType
MySqlSchemaDiscovery::parseDataType(const std::string &type_name) {
  if (type_name == "int" || type_name == "bigint" || type_name == "smallint" ||
      type_name == "mediumint") {
    return metadata::ColumnType::INT;
  }
  if (type_name == "tinyint") {
    return metadata::ColumnType::BOOL;
  }
  if (type_name == "varchar") {
    return metadata::ColumnType::VARCHAR;
  }
  if (type_name == "char") {
    return metadata::ColumnType::CHAR;
  }
  if (type_name == "text" || type_name == "longtext" ||
      type_name == "mediumtext" || type_name == "tinytext") {
    return metadata::ColumnType::TEXT;
  }
  if (type_name == "double" || type_name == "float") {
    return metadata::ColumnType::REAL;
  }
  if (type_name == "blob" || type_name == "longblob" ||
      type_name == "mediumblob" || type_name == "tinyblob" ||
      type_name == "varbinary" || type_name == "binary") {
    return metadata::ColumnType::BYTEA;
  }
  // For timestamp, date, and unknown types, map to TEXT as fallback
  return metadata::ColumnType::TEXT;
}

metadata::Generated
MySqlSchemaDiscovery::parseGeneratedType(const std::string &generated_str) {
  if (generated_str == "stored") {
    return metadata::Generated::stored;
  }
  if (generated_str == "virtual") {
    return metadata::Generated::virt;
  }
  return metadata::Generated::notGenerated;
}

metadata::IndexOrdering
MySqlSchemaDiscovery::parseIndexOrdering(const std::string &ordering_str) {
  if (ordering_str == "desc") {
    return metadata::IndexOrdering::desc;
  }
  return metadata::IndexOrdering::asc;
}

ConstraintType
MySqlSchemaDiscovery::parseConstraintType(const std::string &type_str) {
  if (type_str == "PRIMARY KEY") {
    return ConstraintType::primary_key;
  }
  if (type_str == "FOREIGN KEY") {
    return ConstraintType::foreign_key;
  }
  if (type_str == "UNIQUE") {
    return ConstraintType::unique;
  }
  return ConstraintType::unknown;
}

} // namespace schema_discovery
