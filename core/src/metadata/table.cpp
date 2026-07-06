#include "metadata/table.hpp"

#include <algorithm>
#include <fmt/format.h>

namespace metadata {

bool Table::hasReferenceTo(ObjectId target) const {
  return std::ranges::any_of(columns, [&](const Column &column) {
    return column.foreign_key_references.id == target;
  });
}

bool Table::removeReferencesTo(ObjectId target) {
  bool changed = false;
  for (auto &column : columns) {
    if (column.foreign_key_references.id == target) {
      column.foreign_key_references = {};
      changed = true;
    }
  }
  return changed;
}

namespace {

std::string resolveTargetName(TableRegistry const &reg, Ref<Table> ref) {
  if (!ref) {
    return "";
  }
  auto target = reg.get<Table>().byId(ref.id);
  return target == nullptr ? "#dangling" : target->name;
}

} // namespace

std::vector<NormalizedTable> normalize(TableRegistry const &reg) {
  std::vector<NormalizedTable> result;

  for (auto const &table : reg.get<Table>().snapshotAll()) {
    NormalizedTable normalized;
    normalized.name = table->name;
    normalized.engine = table->engine;
    normalized.tablespace = table->tablespace;
    normalized.partitioning = table->partitioning;

    for (auto const &column : table->columns) {
      NormalizedColumn nc;
      nc.column = column;
      nc.column.foreign_key_references = {};
      nc.foreign_key_target =
          resolveTargetName(reg, column.foreign_key_references);
      normalized.columns.push_back(std::move(nc));
    }
    std::ranges::sort(normalized.columns,
                      [](NormalizedColumn const &a, NormalizedColumn const &b) {
                        return a.column.name < b.column.name;
                      });

    normalized.indexes.assign(table->indexes.begin(), table->indexes.end());
    std::ranges::sort(normalized.indexes, [](Index const &a, Index const &b) {
      return a.name < b.name;
    });

    if (normalized.partitioning.has_value()) {
      std::ranges::sort(normalized.partitioning->ranges,
                        [](RangePartition const &a, RangePartition const &b) {
                          return a.rangebase < b.rangebase;
                        });
    }

    result.push_back(std::move(normalized));
  }

  std::ranges::sort(result,
                    [](NormalizedTable const &a, NormalizedTable const &b) {
                      return a.name < b.name;
                    });
  return result;
}

std::string debug_dump(Column const &column,
                       std::string const &foreignKeyTarget) {
  std::string type_str;
  switch (column.type) {
  case ColumnType::INT:
    type_str = "INT";
    break;
  case ColumnType::CHAR:
    type_str = "CHAR";
    break;
  case ColumnType::VARCHAR:
    type_str = "VARCHAR";
    break;
  case ColumnType::REAL:
    type_str = "REAL";
    break;
  case ColumnType::BOOL:
    type_str = "BOOL";
    break;
  case ColumnType::BYTEA:
    type_str = "BYTEA";
    break;
  case ColumnType::TEXT:
    type_str = "TEXT";
    break;
  }

  if (column.length > 0) {
    type_str = fmt::format("{}({})", type_str, column.length);
  }

  std::vector<std::string> attributes;
  if (column.primary_key) {
    attributes.emplace_back("PRIMARY KEY");
  }
  if (column.auto_increment) {
    attributes.emplace_back("AUTO_INCREMENT");
  }
  if (!column.nullable) {
    attributes.emplace_back("NOT NULL");
  }
  if (column.partition_key) {
    attributes.emplace_back("PARTITION KEY");
  }
  if (!foreignKeyTarget.empty()) {
    attributes.push_back(fmt::format("REFERENCES {}", foreignKeyTarget));
  }
  if (!column.default_value.empty()) {
    attributes.push_back(fmt::format("DEFAULT '{}'", column.default_value));
  }
  if (column.generated != Generated::notGenerated) {
    attributes.push_back(fmt::format(
        "GENERATED {}",
        column.generated == Generated::stored ? "STORED" : "VIRTUAL"));
  }

  std::string result = fmt::format("{} {}", column.name, type_str);
  for (const auto &attr : attributes) {
    result += fmt::format(" {}", attr);
  }
  return result;
}

std::string debug_dump(Index const &index) {
  std::string fields_str;
  for (std::size_t i = 0; i < index.fields.size(); ++i) {
    if (i > 0) {
      fields_str += ", ";
    }
    fields_str += index.fields[i].column_name;
    if (index.fields[i].ordering != IndexOrdering::default_) {
      fields_str +=
          index.fields[i].ordering == IndexOrdering::asc ? " ASC" : " DESC";
    }
  }

  return fmt::format("{}{} ({})", index.name, index.unique ? " UNIQUE" : "",
                     fields_str);
}

std::string debug_dump(Table const &table, TableRegistry const &reg) {
  std::vector<std::string> lines;

  lines.push_back(fmt::format("Table: {}", table.name));
  lines.push_back(fmt::format("  Engine: {}", table.engine));
  if (!table.tablespace.empty()) {
    lines.push_back(fmt::format("  Tablespace: {}", table.tablespace));
  }

  if (table.partitioning.has_value()) {
    lines.push_back(fmt::format("  Partitioning: range (size={}, {} ranges)",
                                table.partitioning->rangeSize,
                                table.partitioning->ranges.size()));
    for (const auto &range : table.partitioning->ranges) {
      lines.push_back(fmt::format("    Range: base={}", range.rangebase));
    }
  }

  lines.push_back(fmt::format("  Columns ({}):", table.columns.size()));
  for (const auto &col : table.columns) {
    lines.push_back(fmt::format(
        "    {}",
        debug_dump(col, resolveTargetName(reg, col.foreign_key_references))));
  }

  if (!table.indexes.empty()) {
    lines.push_back(fmt::format("  Indexes ({}):", table.indexes.size()));
    for (const auto &idx : table.indexes) {
      lines.push_back(fmt::format("    {}", debug_dump(idx)));
    }
  }

  std::string result;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      result += "\n";
    }
    result += lines[i];
  }
  return result;
}

std::string debug_dump(TableRegistry const &reg) {
  auto tables = reg.get<Table>().snapshotAll();
  std::ranges::sort(tables, [](table_cptr const &a, table_cptr const &b) {
    return a->name < b->name;
  });

  std::string result = fmt::format("Metadata dump (size={}):", tables.size());
  for (const auto &table : tables) {
    result += "\n";
    result += debug_dump(*table, reg);
    result += "\n";
  }
  return result;
}

} // namespace metadata
