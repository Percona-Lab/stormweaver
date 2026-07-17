
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "action/action_registry.hpp"
#include "action/variable.hpp"

using namespace action;
using namespace action::variable_detail;
using sql_variant::flavor;
using sql_variant::ServerInfo;

namespace {

VariableSpec spec_of(std::string name, flavor flav, std::uint8_t mechanisms,
                     variable_generator_t gen, std::size_t weight = 10,
                     std::uint64_t min_version = 0,
                     std::uint64_t max_version = 0) {
  return VariableSpec{.name = std::move(name),
                      .flavor = flav,
                      .min_version = min_version,
                      .max_version = max_version,
                      .mechanisms = mechanisms,
                      .weight = weight,
                      .generator = std::move(gen)};
}

constexpr auto SESSION = static_cast<std::uint8_t>(VariableMechanism::session);
constexpr auto GLOBAL = static_cast<std::uint8_t>(VariableMechanism::global);
constexpr auto RELOAD = static_cast<std::uint8_t>(VariableMechanism::reload);

const ServerInfo pg18{.flavor_ = flavor::postgres, .version = 180000};
const ServerInfo mysql84{.flavor_ = flavor::mysql, .version = 80400};

} // namespace

TEST_CASE("variable eligibility") {
  auto s =
      spec_of("work_mem", flavor::ANY_PG, SESSION, VariableChoices{{"'64MB'"}});
  CHECK(eligible(s, VariableMechanism::session, pg18));
  CHECK_FALSE(eligible(s, VariableMechanism::global, pg18));
  CHECK_FALSE(eligible(s, VariableMechanism::session, mysql84));

  auto gated = spec_of("wal_compression", flavor::ANY_PG, RELOAD,
                       VariableChoices{{"lz4"}}, 10, 150000);
  CHECK(eligible(gated, VariableMechanism::reload, pg18));
  const ServerInfo pg14{.flavor_ = flavor::postgres, .version = 140000};
  CHECK_FALSE(eligible(gated, VariableMechanism::reload, pg14));
  // a stubbed version (0) fails min gates: version fill is a prerequisite
  const ServerInfo pg0{.flavor_ = flavor::postgres, .version = 0};
  CHECK_FALSE(eligible(gated, VariableMechanism::reload, pg0));

  auto capped = spec_of("innodb_change_buffering", flavor::ANY_MYSQL, GLOBAL,
                        VariableChoices{{"none"}}, 10, 0, 80099);
  CHECK_FALSE(eligible(capped, VariableMechanism::global, mysql84));

  auto withinCap = spec_of("innodb_old_blocks_time", flavor::ANY_MYSQL, GLOBAL,
                           VariableChoices{{"1000"}}, 10, 0, 90000);
  CHECK(eligible(withinCap, VariableMechanism::global, mysql84));
}

TEST_CASE("variable weighted pick determinism and no-op") {
  std::vector<VariableSpec> specs;
  specs.push_back(spec_of("a", flavor::ANY_PG, SESSION, VariableBool{}, 10));
  specs.push_back(spec_of("b", flavor::ANY_PG, SESSION, VariableBool{}, 30));
  specs.push_back(spec_of("m", flavor::ANY_MYSQL, GLOBAL, VariableBool{}, 100));

  ps_random r1(42);
  ps_random r2(42);
  for (int i = 0; i < 20; ++i) {
    auto const *p1 = pick(specs, VariableMechanism::session, pg18, r1);
    auto const *p2 = pick(specs, VariableMechanism::session, pg18, r2);
    REQUIRE(p1 != nullptr);
    CHECK(p1->name == p2->name);
    CHECK(p1->name != "m");
  }

  ps_random r3(1);
  CHECK(pick(specs, VariableMechanism::reload, pg18, r3) == nullptr);
  CHECK(pick(specs, VariableMechanism::session, mysql84, r3) == nullptr);
}

TEST_CASE("variable value rendering") {
  ps_random rand(7);

  auto suffixed = spec_of("work_mem", flavor::ANY_PG, SESSION,
                          VariableIntRange{1, 4, 1024, "kB"});
  for (int i = 0; i < 10; ++i) {
    auto v = render_value(suffixed, pg18, rand);
    CHECK(v.front() == '\'');
    CHECK(v.back() == '\'');
    CHECK(v.find("kB") != std::string::npos);
  }

  auto bare =
      spec_of("x", flavor::ANY_MYSQL, GLOBAL, VariableIntRange{2, 2, 1024, ""});
  CHECK(render_value(bare, mysql84, rand) == "2048");

  auto pgBareNoSuffix = spec_of("join_collapse_limit", flavor::ANY_PG, SESSION,
                                VariableIntRange{2, 2, 1024, ""});
  CHECK(render_value(pgBareNoSuffix, pg18, rand) == "2048");

  auto mySuffixed = spec_of("wait_timeout", flavor::ANY_MYSQL, GLOBAL,
                            VariableIntRange{2, 2, 1024, "s"});
  CHECK(render_value(mySuffixed, mysql84, rand) == "2048s");

  auto pgBool = spec_of("jit", flavor::ANY_PG, SESSION, VariableBool{});
  auto pgVal = render_value(pgBool, pg18, rand);
  CHECK((pgVal == "on" || pgVal == "off"));
  auto myBool = spec_of("ahi", flavor::ANY_MYSQL, GLOBAL, VariableBool{});
  auto myVal = render_value(myBool, mysql84, rand);
  CHECK((myVal == "ON" || myVal == "OFF"));

  auto choice = spec_of("wal_compression", flavor::ANY_PG, RELOAD,
                        VariableChoices{{"lz4"}});
  CHECK(render_value(choice, pg18, rand) == "lz4");
}

TEST_CASE("variable sql building") {
  auto pgSpec =
      spec_of("work_mem", flavor::ANY_PG, SESSION, VariableChoices{{"'64MB'"}});
  auto sql = build_sql(pgSpec, "'64MB'", VariableMechanism::session, pg18);
  REQUIRE(sql.size() == 1);
  CHECK(sql[0] == "SET work_mem = '64MB'");

  auto mySpec = spec_of("sort_buffer_size", flavor::ANY_MYSQL, SESSION | GLOBAL,
                        VariableChoices{{"65536"}});
  CHECK(build_sql(mySpec, "65536", VariableMechanism::session, mysql84)[0] ==
        "SET SESSION sort_buffer_size = 65536");
  CHECK(build_sql(mySpec, "65536", VariableMechanism::global, mysql84)[0] ==
        "SET GLOBAL sort_buffer_size = 65536");

  auto reload = build_sql(pgSpec, "'64MB'", VariableMechanism::reload, pg18);
  REQUIRE(reload.size() == 2);
  CHECK(reload[0] == "ALTER SYSTEM SET work_mem = '64MB'");
  CHECK(reload[1] == "SELECT pg_reload_conf()");

  CHECK_THROWS_AS(build_sql(pgSpec, "'64MB'", VariableMechanism::startup, pg18),
                  ActionException);
}

TEST_CASE("variable actions registered dormant") {
  for (auto flav : {flavor::ANY_PG, flavor::ANY_MYSQL}) {
    auto &reg = action::default_registry(flav);
    for (auto const *name : {"set_session_variable", "set_global_variable",
                             "reload_global_variable"}) {
      REQUIRE(reg.has(name));
      CHECK(reg[name].weight == 0);
    }
    CHECK(reg["reload_global_variable"].txn_safe == false);
    CHECK(reg["set_session_variable"].txn_safe == true);
  }
}

TEST_CASE("txn_safe filtering") {
  ActionRegistry reg;
  reg.makeCustomSqlAction("safe_action", "SELECT 1", 10);
  reg.insert(ActionFactory{.name = "unsafe_action",
                           .builder = [](BuildContext const &)
                               -> std::unique_ptr<Action> { return nullptr; },
                           .weight = 10,
                           .type = ActionType::other,
                           .txn_safe = false});
  // same predicate TransactionAction uses for its sub-action pools
  auto filtered =
      reg.filtered([](ActionFactory const &f) { return f.txn_safe; });
  CHECK(filtered.size() == 1);
  CHECK(filtered.has("safe_action"));
}
