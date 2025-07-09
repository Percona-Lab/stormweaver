#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sstream>

#include "action/ddl.hpp"
#include "checksum.hpp"
#include "sql.hpp"

namespace {
struct ChecksumFixture {
  mutable metadata::Metadata metaCtx;
  mutable ps_random rand;
  action::DdlConfig config;

  ChecksumFixture() {
    // Recreate public schema to ensure clean state
    sqlConnection->executeQuery("DROP SCHEMA IF EXISTS public CASCADE")
        .maybeThrow();
    sqlConnection->executeQuery("CREATE SCHEMA public").maybeThrow();
    sqlConnection->executeQuery("GRANT ALL ON SCHEMA public TO public")
        .maybeThrow();
  }

  void createTestTable(const std::string &tableName) const {
    auto createResult = sqlConnection->executeQuery(
        "CREATE TABLE " + tableName + " (id INTEGER, name TEXT, value REAL)");
    REQUIRE(createResult.success());

    metaCtx.createTable([&](metadata::Metadata::Reservation &res) {
      auto table = res.table();
      table->name = tableName;

      metadata::Column idCol;
      idCol.name = "id";
      idCol.type = metadata::ColumnType::INT;
      table->columns.push_back(idCol);

      metadata::Column nameCol;
      nameCol.name = "name";
      nameCol.type = metadata::ColumnType::TEXT;
      table->columns.push_back(nameCol);

      metadata::Column valueCol;
      valueCol.name = "value";
      valueCol.type = metadata::ColumnType::REAL;
      table->columns.push_back(valueCol);

      res.complete();
    });
  }

  void insertLargeTestData(const std::string &tableName,
                           size_t numRows = 25000) const {
    const size_t batchSize = 1000;

    for (size_t batch = 0; batch < numRows; batch += batchSize) {
      std::stringstream insertQuery;
      insertQuery << "INSERT INTO " << tableName << " VALUES ";

      size_t endRow = std::min(batch + batchSize, numRows);
      for (size_t i = batch; i < endRow; ++i) {
        if (i > batch)
          insertQuery << ", ";
        insertQuery << "(" << i + 1 << ", 'user_" << i + 1 << "', "
                    << (i + 1) * 1.5 << ")";
      }

      auto insertResult = sqlConnection->executeQuery(insertQuery.str());
      REQUIRE(insertResult.success());
    }
  }
};
} // namespace

TEST_CASE_PERSISTENT_FIXTURE(ChecksumFixture,
                             "Database checksum functionality") {

  SECTION("can calculate checksum for empty table") {
    createTestTable("empty_table");

    DatabaseChecksum checksummer(*sqlConnection, metaCtx);
    REQUIRE_NOTHROW(checksummer.calculateAllTableChecksums());

    const auto &results = checksummer.getResults();
    REQUIRE(results.size() == 1);

    auto it = std::find_if(
        results.begin(), results.end(),
        [](const ChecksumResult &r) { return r.tableName == "empty_table"; });

    REQUIRE(it != results.end());
    REQUIRE(it->rowCount == 0);
    REQUIRE(!it->checksum.empty());
  }

  SECTION("can calculate checksum for large table") {
    createTestTable("large_table");
    insertLargeTestData("large_table", 25000);

    DatabaseChecksum checksummer(*sqlConnection, metaCtx);
    REQUIRE_NOTHROW(checksummer.calculateAllTableChecksums());

    const auto &results = checksummer.getResults();
    REQUIRE(results.size() >= 1);

    auto it = std::find_if(
        results.begin(), results.end(),
        [](const ChecksumResult &r) { return r.tableName == "large_table"; });

    REQUIRE(it != results.end());
    REQUIRE(it->rowCount == 25000);
    REQUIRE(it->checksum.length() == 64); // SHA-256 hex string
    REQUIRE(!it->checksum.empty());
  }

  SECTION("checksums are deterministic with large dataset") {
    createTestTable("deterministic_table");
    insertLargeTestData("deterministic_table", 15000);

    DatabaseChecksum checksummer1(*sqlConnection, metaCtx);
    checksummer1.calculateAllTableChecksums();

    DatabaseChecksum checksummer2(*sqlConnection, metaCtx);
    checksummer2.calculateAllTableChecksums();

    const auto &results1 = checksummer1.getResults();
    const auto &results2 = checksummer2.getResults();

    REQUIRE(results1.size() == results2.size());

    for (size_t i = 0; i < results1.size(); ++i) {
      if (results1[i].tableName == "deterministic_table") {
        auto it = std::find_if(results2.begin(), results2.end(),
                               [&](const ChecksumResult &r) {
                                 return r.tableName == "deterministic_table";
                               });
        REQUIRE(it != results2.end());
        REQUIRE(results1[i].checksum == it->checksum);
        REQUIRE(results1[i].rowCount == it->rowCount);
        REQUIRE(results1[i].rowCount == 15000);
      }
    }
  }

  SECTION("can write results to file with large dataset") {
    createTestTable("file_test_table");
    insertLargeTestData("file_test_table", 12000);

    DatabaseChecksum checksummer(*sqlConnection, metaCtx);
    checksummer.calculateAllTableChecksums();

    const std::string testFile = "/tmp/test_checksums.csv";
    REQUIRE_NOTHROW(checksummer.writeResultsToFile(testFile));

    std::ifstream file(testFile);
    REQUIRE(file.is_open());

    std::string line;
    std::getline(file, line);
    REQUIRE(line == "table_name,checksum,row_count");

    bool foundTestTable = false;
    while (std::getline(file, line)) {
      if (line.find("file_test_table") != std::string::npos) {
        foundTestTable = true;
        std::stringstream ss(line);
        std::string table, checksum, rowCount;

        std::getline(ss, table, ',');
        std::getline(ss, checksum, ',');
        std::getline(ss, rowCount, ',');

        REQUIRE(table == "file_test_table");
        REQUIRE(checksum.length() == 64);
        REQUIRE(rowCount == "12000");
        break;
      }
    }
    REQUIRE(foundTestTable);

    std::remove(testFile.c_str());
  }

  SECTION("can get results as string with large dataset") {
    createTestTable("string_test_table");
    insertLargeTestData("string_test_table", 18000);

    DatabaseChecksum checksummer(*sqlConnection, metaCtx);
    checksummer.calculateAllTableChecksums();

    std::string output = checksummer.getResultsAsString();
    REQUIRE(!output.empty());
    REQUIRE(output.find("table_name,checksum,row_count") != std::string::npos);
    REQUIRE(output.find("string_test_table") != std::string::npos);

    std::stringstream ss(output);
    std::string line;
    std::getline(ss, line); // Skip header

    size_t lineCount = 0;
    bool foundStringTestTable = false;
    while (std::getline(ss, line)) {
      if (!line.empty()) {
        lineCount++;
        std::stringstream lineStream(line);
        std::string table, checksum, rowCount;

        std::getline(lineStream, table, ',');
        std::getline(lineStream, checksum, ',');
        std::getline(lineStream, rowCount, ',');

        REQUIRE(!table.empty());
        REQUIRE(checksum.length() == 64);
        REQUIRE(!rowCount.empty());

        if (table == "string_test_table") {
          foundStringTestTable = true;
          REQUIRE(rowCount == "18000");
        }
      }
    }
    REQUIRE(lineCount > 0);
    REQUIRE(foundStringTestTable);
  }

  SECTION("checksum changes when table data changes") {
    createTestTable("changing_table");
    insertLargeTestData("changing_table", 5000);

    DatabaseChecksum checksummer1(*sqlConnection, metaCtx);
    checksummer1.calculateAllTableChecksums();

    const auto &results1 = checksummer1.getResults();
    auto it1 = std::find_if(results1.begin(), results1.end(),
                            [](const ChecksumResult &r) {
                              return r.tableName == "changing_table";
                            });
    REQUIRE(it1 != results1.end());
    std::string originalChecksum = it1->checksum;

    auto updateResult = sqlConnection->executeQuery(
        "UPDATE changing_table SET value = value + 1000 WHERE id <= 100");
    REQUIRE(updateResult.success());

    DatabaseChecksum checksummer2(*sqlConnection, metaCtx);
    checksummer2.calculateAllTableChecksums();

    const auto &results2 = checksummer2.getResults();
    auto it2 = std::find_if(results2.begin(), results2.end(),
                            [](const ChecksumResult &r) {
                              return r.tableName == "changing_table";
                            });
    REQUIRE(it2 != results2.end());

    REQUIRE(it2->rowCount == it1->rowCount);
    REQUIRE(it2->checksum != originalChecksum);
    REQUIRE(it2->checksum.length() == 64);

    auto insertResult = sqlConnection->executeQuery(
        "INSERT INTO changing_table VALUES (99999, 'new_user', 42.0)");
    REQUIRE(insertResult.success());

    DatabaseChecksum checksummer3(*sqlConnection, metaCtx);
    checksummer3.calculateAllTableChecksums();

    const auto &results3 = checksummer3.getResults();
    auto it3 = std::find_if(results3.begin(), results3.end(),
                            [](const ChecksumResult &r) {
                              return r.tableName == "changing_table";
                            });
    REQUIRE(it3 != results3.end());

    REQUIRE(it3->rowCount == it1->rowCount + 1);
    REQUIRE(it3->checksum != it2->checksum);
    REQUIRE(it3->checksum != originalChecksum);
    REQUIRE(it3->checksum.length() == 64);
  }

  SECTION("throws on invalid table") {
    createTestTable("invalid_table");

    auto dropResult = sqlConnection->executeQuery("DROP TABLE invalid_table");
    REQUIRE(dropResult.success());

    DatabaseChecksum checksummer(*sqlConnection, metaCtx);
    REQUIRE_THROWS_AS(checksummer.calculateAllTableChecksums(),
                      std::runtime_error);
  }
}
