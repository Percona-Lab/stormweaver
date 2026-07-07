#include <catch2/catch_test_macros.hpp>

#include "sql.hpp"

namespace {
std::string ph(int n) {
  return testutil::isMysql() ? std::string("?") : "$" + std::to_string(n);
}
std::string byteLen() {
  return testutil::isMysql() ? "LENGTH" : "octet_length";
}
std::string hexOf(std::string const &col) {
  return testutil::isMysql() ? "LOWER(HEX(" + col + "))"
                             : "encode(" + col + ", 'hex')";
}
} // namespace

TEST_CASE("executeParams round-trips basic types", "[sql][params]") {
  testutil::resetTestSchema();
  sqlConnection
      ->executeQuery("CREATE TABLE pt (a INT, b TEXT, c " +
                     testutil::blobType() + ")")
      .maybeThrow();

  const std::string blob("\x00\x01\xff", 3);
  sqlConnection
      ->executeParams("INSERT INTO pt VALUES (" + ph(1) + ", " + ph(2) + ", " +
                          ph(3) + ")",
                      {{.value = "42"},
                       {.value = "it's a test"},
                       {.value = blob, .binary = true}})
      .maybeThrow();

  auto res = sqlConnection->executeParams("SELECT b, " + byteLen() + "(c), " +
                                              hexOf("c") +
                                              " FROM pt WHERE a = " + ph(1),
                                          {{.value = "42"}});
  res.maybeThrow();
  REQUIRE(res.data->numRows() == 1);
  auto row = res.data->nextRow();
  REQUIRE(row.rowData[0] == "it's a test");
  REQUIRE(row.rowData[1] == "3");
  REQUIRE(row.rowData[2] == "0001ff");
}

TEST_CASE("executeParams NULL and hostile values", "[sql][params]") {
  testutil::resetTestSchema();
  sqlConnection->executeQuery("CREATE TABLE pt (a INT, b TEXT)").maybeThrow();

  sqlConnection
      ->executeParams("INSERT INTO pt VALUES (" + ph(1) + ", " + ph(2) + ")",
                      {{.value = "1"}, {.value = std::nullopt}})
      .maybeThrow();
  auto nulls =
      sqlConnection->executeQuery("SELECT count(*) FROM pt WHERE b IS NULL");
  nulls.maybeThrow();
  REQUIRE(nulls.data->nextRow().rowData[0] == "1");

  const std::string hostile = "'; DROP TABLE pt; --";
  sqlConnection
      ->executeParams("INSERT INTO pt VALUES (" + ph(1) + ", " + ph(2) + ")",
                      {{.value = "2"}, {.value = hostile}})
      .maybeThrow();
  auto back = sqlConnection->executeParams(
      "SELECT b FROM pt WHERE a = " + ph(1), {{.value = "2"}});
  back.maybeThrow();
  REQUIRE(back.data->nextRow().rowData[0] == hostile);
  sqlConnection->executeQuery("SELECT count(*) FROM pt").maybeThrow();

  const std::string tricky = "\\' OR '1'='1";
  sqlConnection
      ->executeParams("INSERT INTO pt VALUES (" + ph(1) + ", " + ph(2) + ")",
                      {{.value = "3"}, {.value = tricky}})
      .maybeThrow();
  auto trickyBack = sqlConnection->executeParams(
      "SELECT b FROM pt WHERE a = " + ph(1), {{.value = "3"}});
  trickyBack.maybeThrow();
  REQUIRE(trickyBack.data->nextRow().rowData[0] == tricky);
}

TEST_CASE("executeParams param count mismatch", "[sql][params]") {
  testutil::resetTestSchema();
  sqlConnection->executeQuery("CREATE TABLE pt (a INT)").maybeThrow();

  auto tooFew =
      sqlConnection->executeParams("INSERT INTO pt VALUES (" + ph(1) + ")", {});
  REQUIRE_FALSE(tooFew.success());

  auto tooMany =
      sqlConnection->executeParams("INSERT INTO pt VALUES (" + ph(1) + ")",
                                   {{.value = "1"}, {.value = "2"}});
  REQUIRE_FALSE(tooMany.success());

  if (testutil::isMysql()) {
    REQUIRE(tooFew.errorInfo.errorCode == "params-mismatch");
    REQUIRE(tooMany.errorInfo.errorCode == "params-mismatch");
  }

  auto count = sqlConnection->executeQuery("SELECT count(*) FROM pt");
  count.maybeThrow();
  REQUIRE(count.data->nextRow().rowData[0] == "0");
}

TEST_CASE("executeParams empty string vs NULL", "[sql][params]") {
  testutil::resetTestSchema();
  sqlConnection
      ->executeQuery("CREATE TABLE pt (a INT, b TEXT, c " +
                     testutil::blobType() + ")")
      .maybeThrow();
  sqlConnection
      ->executeParams(
          "INSERT INTO pt VALUES (" + ph(1) + ", " + ph(2) + ", " + ph(3) + ")",
          {{.value = "1"}, {.value = ""}, {.value = "", .binary = true}})
      .maybeThrow();
  auto res = sqlConnection->executeQuery(
      "SELECT count(*) FROM pt WHERE b = '' AND b IS NOT NULL");
  res.maybeThrow();
  REQUIRE(res.data->nextRow().rowData[0] == "1");

  auto blobLen =
      sqlConnection->executeQuery("SELECT " + byteLen() + "(c) FROM pt");
  blobLen.maybeThrow();
  REQUIRE(blobLen.data->nextRow().rowData[0] == "0");
}

TEST_CASE("executeParams without placeholders or params", "[sql][params]") {
  auto res = sqlConnection->executeParams("SELECT 1", {});
  res.maybeThrow();
  REQUIRE(res.data->nextRow().rowData[0] == "1");
}

TEST_CASE("executeParams leaves quoted question marks alone", "[sql][params]") {
  testutil::resetTestSchema();
  sqlConnection->executeQuery("CREATE TABLE pt (a INT, b TEXT)").maybeThrow();
  sqlConnection
      ->executeParams("INSERT INTO pt VALUES (" + ph(1) + ", 'q?m')",
                      {{.value = "1"}})
      .maybeThrow();
  auto res = sqlConnection->executeQuery("SELECT b FROM pt WHERE a = 1");
  res.maybeThrow();
  REQUIRE(res.data->nextRow().rowData[0] == "q?m");

  if (testutil::isMysql()) {
    // default sql_mode: double quotes are strings, backticks are identifiers
    auto dq = sqlConnection->executeParams("SELECT \"d?q\"", {});
    dq.maybeThrow();
    REQUIRE(dq.data->nextRow().rowData[0] == "d?q");

    auto bs = sqlConnection->executeParams("SELECT 'it\\'s ?', " + ph(1),
                                           {{.value = "x"}});
    bs.maybeThrow();
    auto row = bs.data->nextRow();
    REQUIRE(row.rowData[0] == "it's ?");
    REQUIRE(row.rowData[1] == "x");

    sqlConnection->executeQuery("CREATE TABLE `t?q` (`c?` INT)").maybeThrow();
    sqlConnection
        ->executeParams("INSERT INTO `t?q` (`c?`) VALUES (" + ph(1) + ")",
                        {{.value = "7"}})
        .maybeThrow();
    auto bt = sqlConnection->executeQuery("SELECT `c?` FROM `t?q`");
    bt.maybeThrow();
    REQUIRE(bt.data->nextRow().rowData[0] == "7");
  }
}

TEST_CASE("LoggedSQL safeQuery throws on error", "[sql][params]") {
  testutil::resetTestSchema();
  REQUIRE_THROWS_AS(sqlConnection->safeQuery("SELECT * FROM nope_missing"),
                    sql_variant::SqlException);

  try {
    sqlConnection->safeQuery("SELECT * FROM nope_missing");
    FAIL("expected SqlException");
  } catch (sql_variant::SqlException const &e) {
    REQUIRE(e.errorClass() == sql_variant::ErrorClass::other);
  }

  auto res = sqlConnection->safeQuery("SELECT 1");
  REQUIRE(res.success());
}

TEST_CASE("safeQuery empty params allows multi-statement", "[sql][params]") {
  if (testutil::isMysql()) {
    SKIP("pg-specific: mysql connection lacks CLIENT_MULTI_STATEMENTS");
  }
  testutil::resetTestSchema();
  auto res = sqlConnection->safeQuery("SELECT 1; SELECT 2");
  REQUIRE(res.success());
}

TEST_CASE("LoggedSQL safeQuery with params", "[sql][params]") {
  testutil::resetTestSchema();
  sqlConnection->safeQuery("CREATE TABLE lp (a INT, b TEXT)");
  const auto before = sqlConnection->getQueryCount();
  sqlConnection->safeQuery("INSERT INTO lp VALUES (" + ph(1) + ", " + ph(2) +
                               ")",
                           {{.value = "7"}, {.value = std::nullopt}});
  REQUIRE(sqlConnection->getQueryCount() == before + 1);
  auto res =
      sqlConnection->safeQuery("SELECT count(*) FROM lp WHERE b IS NULL");
  REQUIRE(res.data->nextRow().rowData[0] == "1");
}
