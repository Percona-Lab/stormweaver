
#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "metadata.hpp"
#include "sql_variant/sql_variant.hpp"

namespace CryptoPP {
class SHA256;
}

struct ChecksumResult {
  std::string tableName;
  std::string checksum;
  size_t rowCount{0};

  ChecksumResult(std::string name) : tableName(std::move(name)) {}
};

class DatabaseChecksum {
public:
  DatabaseChecksum(sql_variant::LoggedSQL &connection,
                   const metadata::Metadata &metadata);

  void calculateAllTableChecksums();
  void writeResultsToFile(const std::string &filename);
  std::string getResultsAsString();

  [[nodiscard]] const std::vector<ChecksumResult> &getResults() const {
    return results_;
  }

private:
  sql_variant::LoggedSQL &connection_;
  const metadata::Metadata &metadata_;
  std::vector<ChecksumResult> results_;

  void processAllRows(const metadata::Table &table, CryptoPP::SHA256 &hasher);
  static std::string buildRowHash(const sql_variant::RowView &row);
  static std::string bytesToHex(const std::array<uint8_t, 32> &bytes);
};
