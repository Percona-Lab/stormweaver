
#include <catch2/catch_test_macros.hpp>

#include "action/ddl.hpp"
#include "metadata_populator.hpp"
#include "schema_discovery.hpp"
#include "sql.hpp"

namespace {
struct Fixture {
  mutable metadata::TableRegistry metaCtx;
  mutable metadata::Context ctx{metaCtx};
  mutable ps_random rand;
  action::DdlConfig config;
};

void populateFromDatabase(metadata::TableRegistry &reg) {
  auto discovery = schema_discovery::make_schema_discovery(sqlConnection.get());
  metadata_populator::MetadataPopulator populator(reg);
  populator.populateFromExistingDatabase(*discovery);
}

} // namespace

TEST_CASE_PERSISTENT_FIXTURE(Fixture, "DDLs work") {

  SECTION("tables can be created") {

    for (int i = 0; i < 100; ++i) {
      action::CreateTable ct(config, metadata::Table::Type::normal);
      REQUIRE_NOTHROW(ct.execute(ctx, rand, sqlConnection.get()));
    }
  }

  SECTION("tables can be altered") {
    for (int i = 0; i < 1000; ++i) {
      // disable change access method, as this can be run againts upstream pg
      // without tde
      action::AlterTable at(config,
                            BitFlags<action::AlterSubcommand>::AllSet().Unset(
                                action::AlterSubcommand::changeAccessMethod));
      REQUIRE_NOTHROW(at.execute(ctx, rand, sqlConnection.get()));
    }
  }

  SECTION("tables can be renamed") {
    for (int i = 0; i < 100; ++i) {
      action::RenameTable rt(config);
      REQUIRE_NOTHROW(rt.execute(ctx, rand, sqlConnection.get()));
    }
  }

  SECTION("indexes can be created") {
    for (int i = 0; i < 1000; ++i) {
      action::CreateIndex rt(config);
      REQUIRE_NOTHROW(rt.execute(ctx, rand, sqlConnection.get()));
    }
  }

  SECTION("indexes can be dropped") {
    for (int i = 0; i < 100; ++i) {
      action::DropIndex rt(config);
      REQUIRE_NOTHROW(rt.execute(ctx, rand, sqlConnection.get()));
    }
  }

  SECTION("tables can be dropped") {
    for (int i = 0; i < 100; ++i) {
      action::DropTable dt(config);
      REQUIRE_NOTHROW(dt.execute(ctx, rand, sqlConnection.get()));
    }
  }
}

// regression: dropping an indexed column must keep index metadata in sync
// with the server (pg drops the whole index, mysql trims the key part)
TEST_CASE("drop column keeps dependent index metadata in sync", "[ddl]") {
  testutil::resetTestSchema();
  sqlConnection
      ->executeQuery("CREATE TABLE dropidx (id INT PRIMARY KEY, a INT, b INT, "
                     "c INT, d INT);")
      .maybeThrow();
  sqlConnection->executeQuery("CREATE INDEX idx_ab ON dropidx (a, b);")
      .maybeThrow();
  sqlConnection->executeQuery("CREATE INDEX idx_cd ON dropidx (c, d);")
      .maybeThrow();

  metadata::TableRegistry reg;
  populateFromDatabase(reg);
  metadata::Context ctx(reg);

  ps_random rand{20260707};
  action::DdlConfig config;
  config.max_alter_clauses = 1;

  // 5 columns; the 3-column floor allows exactly three drops
  for (int i = 0; i < 3; ++i) {
    action::AlterTable at(config, BitFlags<action::AlterSubcommand>{}.Set(
                                      action::AlterSubcommand::dropColumn));
    REQUIRE_NOTHROW(at.execute(ctx, rand, sqlConnection.get()));

    metadata::TableRegistry fresh;
    populateFromDatabase(fresh);
    REQUIRE(metadata::normalize(reg) == metadata::normalize(fresh));
  }
}
