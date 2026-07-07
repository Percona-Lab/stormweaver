#include <catch2/catch_test_macros.hpp>

#include "sql_variant/generic.hpp"

using sql_variant::ErrorClass;

TEST_CASE("pg sqlstate classification", "[errorclass]") {
  REQUIRE(sql_variant::classify_pg_sqlstate("40001") == ErrorClass::conflict);
  REQUIRE(sql_variant::classify_pg_sqlstate("40P01") == ErrorClass::conflict);
  REQUIRE(sql_variant::classify_pg_sqlstate("25P02") == ErrorClass::failedTxn);
  REQUIRE(sql_variant::classify_pg_sqlstate("42601") == ErrorClass::other);
}

TEST_CASE("mysql errno classification", "[errorclass]") {
  REQUIRE(sql_variant::classify_mysql_errno(1213) == ErrorClass::conflict);
  REQUIRE(sql_variant::classify_mysql_errno(1205) == ErrorClass::conflict);
  REQUIRE(sql_variant::classify_mysql_errno(1062) == ErrorClass::other);
}
