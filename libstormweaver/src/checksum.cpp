#include "checksum.hpp"
#include <cryptopp/sha.h>
#include <fmt/format.h>
#include <fstream>
#include <iomanip>
#include <sstream>

DatabaseChecksum::DatabaseChecksum(sql_variant::LoggedSQL &connection,
                                   const metadata::Metadata &metadata)
    : connection_(connection), metadata_(metadata) {}

void DatabaseChecksum::calculateAllTableChecksums() {
  results_.clear();

  for (size_t i = 0; i < metadata_.size(); ++i) {
    auto table = metadata_[i];
    if (!table)
      continue;

    ChecksumResult result(table->name);

    auto countResult = connection_.querySingleValue(
        fmt::format("SELECT COUNT(*) FROM {}", table->name));

    if (!countResult.has_value()) {
      throw std::runtime_error("Failed to get row count for table: " +
                               table->name);
    }

    result.rowCount = std::stoull(std::string(countResult.value()));

    CryptoPP::SHA256 hasher;
    processAllRows(*table, hasher);

    std::array<uint8_t, 32> hash;
    hasher.Final(hash.data());
    result.checksum = bytesToHex(hash);

    results_.push_back(result);
  }

  std::sort(results_.begin(), results_.end(), [](auto const &a, auto const &b) {
    return a.tableName < b.tableName;
  });
}

void DatabaseChecksum::writeResultsToFile(const std::string &filename) {
  std::ofstream file(filename);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open file for writing: " + filename);
  }

  file << getResultsAsString();
}

std::string DatabaseChecksum::getResultsAsString() {
  std::stringstream output;
  output << "table_name,checksum,row_count\n";

  for (const auto &result : results_) {
    output << result.tableName << "," << result.checksum << ","
           << result.rowCount << "\n";
  }

  return output.str();
}

void DatabaseChecksum::processAllRows(const metadata::Table &table,
                                      CryptoPP::SHA256 &hasher) {
  std::stringstream orderBy;

  if (!table.columns.empty()) {
    orderBy << "ORDER BY ";
    for (size_t j = 0; j < table.columns.size(); ++j) {
      if (j > 0)
        orderBy << ", ";
      orderBy << table.columns[j].name;
    }
  }

  auto queryResult = connection_.executeQuery(
      fmt::format("SELECT * FROM {} {}", table.name, orderBy.str()));

  if (!queryResult.success()) {
    throw std::runtime_error("Failed to execute query for table: " +
                             table.name);
  }

  for (size_t i = 0; i < queryResult.data->numRows(); ++i) {
    auto row = queryResult.data->nextRow();
    std::string rowHash = buildRowHash(row);
    hasher.Update(reinterpret_cast<const CryptoPP::byte *>(rowHash.c_str()),
                  rowHash.length());
  }
}

std::string DatabaseChecksum::buildRowHash(const sql_variant::RowView &row) {
  std::stringstream rowData;
  for (size_t i = 0; i < row.rowData.size(); ++i) {
    if (row.rowData[i].has_value()) {
      rowData << row.rowData[i].value();
    }
    rowData << "|";
  }
  return rowData.str();
}

std::string DatabaseChecksum::bytesToHex(const std::array<uint8_t, 32> &bytes) {
  std::stringstream ss;
  for (const auto &byte : bytes) {
    ss << std::hex << std::setfill('0') << std::setw(2)
       << static_cast<int>(byte);
  }
  return ss.str();
}
