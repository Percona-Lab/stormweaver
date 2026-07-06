#pragma once

#include <algorithm>
#include <boost/container/small_vector.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "metadata/catalog.hpp"

namespace metadata {

namespace limits {
const constexpr std::size_t optimized_column_count = 32;
const constexpr std::size_t optimized_index_column_count = 10;
const constexpr std::size_t optimized_index_count = 16;
} // namespace limits

enum class ColumnType : std::uint8_t {
  INT,
  CHAR,
  VARCHAR,
  REAL,
  BOOL,
  BYTEA,
  TEXT
};

enum class Generated : std::uint8_t { notGenerated, stored, virt };

struct Table;

struct Column {
  std::string name;
  ColumnType type = ColumnType::INT;

  std::size_t length = 0;

  std::string default_value;

  Generated generated = Generated::notGenerated;

  bool nullable = true;
  bool primary_key = false;
  bool partition_key = false;
  Ref<Table> foreign_key_references;
  bool auto_increment = false;

  auto operator<=>(const Column &other) const = default;
};

enum class IndexOrdering : std::uint8_t { default_, asc, desc };

struct IndexColumn {
  std::string column_name;
  IndexOrdering ordering = IndexOrdering::default_;

  auto operator<=>(const IndexColumn &other) const = default;
};

struct Index {
  std::string name;

  bool unique = false;

  boost::container::small_vector<IndexColumn,
                                 limits::optimized_index_column_count>
      fields;

  friend bool operator==(const Index &lhs, const Index &rhs) {
    return lhs.name == rhs.name && lhs.unique == rhs.unique &&
           std::ranges::equal(lhs.fields, rhs.fields);
  }
};

struct RangePartition {
  // [ rangebase * rangeSize, (rangebase+1) * rangeSize)
  std::size_t rangebase = 0;

  auto operator<=>(const RangePartition &other) const = default;
};

struct RangePartitioning {
  std::size_t rangeSize = 0;
  std::vector<RangePartition> ranges;

  auto operator<=>(const RangePartitioning &other) const = default;
};

struct Table : ObjectBase {
  enum class Type : std::uint8_t { normal, partitioned, temporary };

  std::string engine; // or access method
  std::string tablespace;

  std::optional<RangePartitioning> partitioning;

  boost::container::small_vector<Column, limits::optimized_column_count>
      columns;
  boost::container::small_vector<Index, limits::optimized_index_count> indexes;

  [[nodiscard]] bool hasReferenceTo(ObjectId target) const;
  // true if any reference was removed
  bool removeReferencesTo(ObjectId target);
};

using table_cptr = object_cptr<Table>;
using TableRegistry = Registry<Table>;

/* Id-independent view for validation and dumps: references resolved to
   names (dangling -> "#dangling"), id/version dropped, tables and their
   sub-objects name-sorted. Two registries with the same structure compare
   equal regardless of id assignment or insertion order. */
struct NormalizedColumn {
  Column column;                  // foreign_key_references cleared
  std::string foreign_key_target; // "" = none, "#dangling" = unresolvable

  auto operator<=>(const NormalizedColumn &other) const = default;
};

struct NormalizedTable {
  std::string name;
  std::string engine;
  std::string tablespace;
  std::optional<RangePartitioning> partitioning;
  std::vector<NormalizedColumn> columns; // name-sorted
  std::vector<Index> indexes;            // name-sorted

  bool operator==(const NormalizedTable &other) const {
    return name == other.name && engine == other.engine &&
           tablespace == other.tablespace &&
           partitioning == other.partitioning && columns == other.columns &&
           std::ranges::equal(indexes, other.indexes);
  }
};

/* normalize() is only self-consistent on a quiescent registry: reference
   resolution reads current state after the snapshot. Duplicate table names
   (tolerated by the catalog) make the sort order among the duplicates
   unspecified; the validation pairing never produces duplicates on the
   rediscovered side, so a false-equal cannot occur. */
std::vector<NormalizedTable> normalize(TableRegistry const &reg);

std::string debug_dump(Column const &column,
                       std::string const &foreignKeyTarget);
std::string debug_dump(Index const &index);
std::string debug_dump(Table const &table, TableRegistry const &reg);
std::string debug_dump(TableRegistry const &reg);

} // namespace metadata
