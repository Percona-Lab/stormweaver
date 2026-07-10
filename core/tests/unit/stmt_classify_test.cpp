#include <catch2/catch_test_macros.hpp>

#include "sql_variant/generic.hpp"

using sql_variant::classifyStatement;
using sql_variant::StmtKind;

TEST_CASE("classifyStatement keywords", "[sql][classify]") {
  REQUIRE(classifyStatement("SELECT 1") == StmtKind::select);
  REQUIRE(classifyStatement("select * from t") == StmtKind::select);
  REQUIRE(classifyStatement("INSERT INTO t VALUES (1)") == StmtKind::insert);
  REQUIRE(classifyStatement("Update t SET a=1") == StmtKind::update);
  REQUIRE(classifyStatement("DELETE FROM t") == StmtKind::del);
  REQUIRE(classifyStatement("WITH w AS (SELECT 1) SELECT * FROM w") ==
          StmtKind::with);
  REQUIRE(classifyStatement("BEGIN;") == StmtKind::other);
  REQUIRE(classifyStatement("COMMIT;") == StmtKind::other);
  REQUIRE(classifyStatement("CREATE TABLE t (a int)") == StmtKind::other);
  REQUIRE(classifyStatement("SAVEPOINT sp1;") == StmtKind::other);
  REQUIRE(classifyStatement("") == StmtKind::other);
}

TEST_CASE("classifyStatement skips whitespace and comments",
          "[sql][classify]") {
  REQUIRE(classifyStatement("   \n\t SELECT 1") == StmtKind::select);
  REQUIRE(classifyStatement("-- comment\nSELECT 1") == StmtKind::select);
  REQUIRE(classifyStatement("/* block */ UPDATE t SET a=1") ==
          StmtKind::update);
  REQUIRE(classifyStatement("/* multi\nline */\n-- more\nDELETE FROM t") ==
          StmtKind::del);
  REQUIRE(classifyStatement("-- only a comment") == StmtKind::other);
  REQUIRE(classifyStatement("/* unterminated") == StmtKind::other);
  // keyword must be a whole word
  REQUIRE(classifyStatement("SELECTX") == StmtKind::other);
  REQUIRE(classifyStatement("SELECT2 * FROM t") == StmtKind::other);
  REQUIRE(classifyStatement("-") == StmtKind::other);
  REQUIRE(classifyStatement("/") == StmtKind::other);
  REQUIRE(classifyStatement("SELECT") == StmtKind::select);
  REQUIRE(classifyStatement("--\nSELECT 1") == StmtKind::select);
  // /*/ does not self-close, comment never terminates
  REQUIRE(classifyStatement("/*/ SELECT 1") == StmtKind::other);
}
