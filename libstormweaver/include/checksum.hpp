
#pragma once

#include <array>
#include <string>
#include <vector>

#include "metadata.hpp"
#include "sql_variant/sql_variant.hpp"

namespace CryptoPP {
class SHA256;
}

struct ChecksumResult {
  std::string tableName;
  std::string checksum;
  size_t rowCount;

  ChecksumResult(const std::string &name) : tableName(name), rowCount(0) {}
};

class DatabaseChecksum {
public:
  DatabaseChecksum(sql_variant::LoggedSQL &connection,
                   const metadata::Metadata &metadata);

  void calculateAllTableChecksums();
  void writeResultsToFile(const std::string &filename);
  std::string getResultsAsString();

  const std::vector<ChecksumResult> &getResults() const { return results_; }

private:
  sql_variant::LoggedSQL &connection_;
  const metadata::Metadata &metadata_;
  std::vector<ChecksumResult> results_;

  void processAllRows(const metadata::Table &table, CryptoPP::SHA256 &hasher);
  std::string buildRowHash(const sql_variant::RowView &row);
  std::string bytesToHex(const std::array<uint8_t, 32> &bytes);
};
