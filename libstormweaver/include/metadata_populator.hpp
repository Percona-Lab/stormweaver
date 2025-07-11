#pragma once

#include "metadata.hpp"
#include "schema_discovery.hpp"

namespace metadata_populator {

class MetadataPopulator {
public:
  explicit MetadataPopulator(metadata::Metadata &metadata);

  void
  populateFromExistingDatabase(schema_discovery::SchemaDiscovery &discovery);

private:
  metadata::Metadata &metadata_;

  metadata::Table convertCompleteTable(
      schema_discovery::SchemaDiscovery &discovery,
      const schema_discovery::DiscoveredTable &discovered_table);

  metadata::Column
  convertColumn(const schema_discovery::DiscoveredColumn &discovered);
  metadata::Index
  convertIndex(const schema_discovery::DiscoveredIndex &discovered);

  void applyConstraints(
      metadata::Table &table,
      const std::vector<schema_discovery::DiscoveredConstraint> &constraints);

  void applyPartitionKeys(metadata::Table &table,
                          const std::vector<std::string> &partition_keys);

  void applyPartitioning(
      metadata::Table &table,
      const std::vector<schema_discovery::DiscoveredPartition> &partitions);
};

} // namespace metadata_populator
