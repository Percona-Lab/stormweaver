
#include <catch2/catch_test_macros.hpp>

#include "action/ddl.hpp"
#include "sql.hpp"

namespace {
struct Fixture {
  mutable metadata::Metadata metaCtx;
  mutable ps_random rand;
  action::DdlConfig config;
};

} // namespace

TEST_CASE_PERSISTENT_FIXTURE(Fixture, "DDLs work") {

  SECTION("tables can be created") {

    for (int i = 0; i < 100; ++i) {
      action::CreateTable ct(config, metadata::Table::Type::normal);
      REQUIRE_NOTHROW(ct.execute(metaCtx, rand, sqlConnection.get()));
    }
  }

  SECTION("tables can be altered") {
    for (int i = 0; i < 1000; ++i) {
      // disable change access method, as this can be run againts upstream pg
      // without tde
      action::AlterTable at(config,
                            BitFlags<action::AlterSubcommand>::AllSet().Unset(
                                action::AlterSubcommand::changeAccessMethod));
      REQUIRE_NOTHROW(at.execute(metaCtx, rand, sqlConnection.get()));
    }
  }

  SECTION("tables can be dropped") {
    for (int i = 0; i < 100; ++i) {
      action::DropTable dt(config);
      REQUIRE_NOTHROW(dt.execute(metaCtx, rand, sqlConnection.get()));
    }
  }
}
