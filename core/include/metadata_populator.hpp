#pragma once

#include "metadata/table.hpp"
#include "schema_discovery.hpp"

namespace metadata_populator {

class MetadataPopulator {
public:
  explicit MetadataPopulator(metadata::TableRegistry &registry);

  void
  populateFromExistingDatabase(schema_discovery::SchemaDiscovery &discovery);

private:
  metadata::TableRegistry &registry_;

  // column name -> referenced table name, resolved to ids in a second pass
  using fk_list_t = std::vector<std::pair<std::string, std::string>>;

  static metadata::Table convertCompleteTable(
      schema_discovery::SchemaDiscovery &discovery,
      const schema_discovery::DiscoveredTable &discovered_table,
      fk_list_t &fkColumns);

  static metadata::Column
  convertColumn(const schema_discovery::DiscoveredColumn &discovered);

  static metadata::Index
  convertIndex(const schema_discovery::DiscoveredIndex &discovered);

  static void applyConstraints(
      metadata::Table &table,
      const std::vector<schema_discovery::DiscoveredConstraint> &constraints,
      fk_list_t &fkColumns);

  static void
  applyPartitionKeys(metadata::Table &table,
                     const std::vector<std::string> &partition_keys);

  static void applyPartitioning(
      metadata::Table &table,
      const std::vector<schema_discovery::DiscoveredPartition> &partitions);
};

} // namespace metadata_populator
