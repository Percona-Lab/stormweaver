#pragma once

#include <optional>
#include <string>
#include <vector>

#include "metadata.hpp"
#include "sql_variant/generic.hpp"

namespace schema_discovery {

enum class PartitionType { none, range, hash, list };

enum class ConstraintType { primary_key, foreign_key, unique, check, unknown };

struct DiscoveredTable {
  std::string name;
  metadata::Table::Type table_type = metadata::Table::Type::normal;
  std::string access_method; // heap, tde_heap, etc.
  std::string tablespace;
  bool is_partition = false;
  PartitionType partition_type = PartitionType::none;
};

struct DiscoveredColumn {
  std::string name;
  metadata::ColumnType data_type = metadata::ColumnType::TEXT;
  int length = 0; // For char/varchar types
  int type_modifier = -1;
  bool not_null = false;
  int ordinal_position = 0;
  bool is_serial = false;
  metadata::Generated generated_type = metadata::Generated::notGenerated;
  std::string default_value;
};

struct DiscoveredIndex {
  std::string name;
  bool is_unique = false;
  std::vector<std::string> column_names;
  std::vector<metadata::IndexOrdering> orderings;
};

struct DiscoveredConstraint {
  std::string name;
  ConstraintType type = ConstraintType::unknown;
  std::vector<std::string> columns;
  std::string referenced_table;                // For foreign key constraints
  std::vector<std::string> referenced_columns; // For foreign key constraints
};

struct DiscoveredPartition {
  std::string name;
  std::string partition_bound; // Raw PostgreSQL partition bound expression
};

class SchemaDiscovery {
public:
  explicit SchemaDiscovery(sql_variant::LoggedSQL *connection);

  std::vector<DiscoveredTable> discoverTables();
  std::vector<DiscoveredColumn> discoverColumns(const std::string &table_name);
  std::vector<DiscoveredIndex> discoverIndexes(const std::string &table_name);
  std::vector<DiscoveredConstraint>
  discoverConstraints(const std::string &table_name);
  std::vector<DiscoveredPartition>
  discoverPartitions(const std::string &table_name);
  std::vector<std::string> discoverPartitionKeys(const std::string &table_name);

private:
  sql_variant::LoggedSQL *connection_;

  std::string parseAccessMethod(const std::string &am_name);
  std::string parseTablespace(const std::string &ts_name);
  int parseTypeModifier(const std::string &type_name, int type_modifier);

  metadata::Table::Type parseTableType(const std::string &type_char);
  PartitionType parsePartitionType(const std::string &partition_type_str);
  metadata::ColumnType parseDataType(const std::string &type_name);
  metadata::Generated parseGeneratedType(const std::string &generated_str);
  metadata::IndexOrdering parseIndexOrdering(const std::string &ordering_str);
  ConstraintType parseConstraintType(const std::string &type_char);
};

} // namespace schema_discovery
