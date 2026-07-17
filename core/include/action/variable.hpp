
#pragma once

#include "action/action.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace action {

// which apply path may set a variable; startup is python-side only, kept
// here so one bitmask describes the whole spec
enum class VariableMechanism : std::uint8_t {
  session = 1,
  global = 2,
  reload = 4,
  startup = 8,
};

struct VariableChoices {
  std::vector<std::string> values;
};

struct VariableIntRange {
  std::int64_t min = 0;
  std::int64_t max = 0;
  std::int64_t step = 1;
  std::string suffix;
};

struct VariableBool {};

using variable_generator_t =
    std::variant<VariableChoices, VariableIntRange, VariableBool>;

struct VariableSpec {
  std::string name;
  sql_variant::flavor flavor = sql_variant::flavor::ANY_PG;
  std::uint64_t min_version = 0; // 0 = no gate
  std::uint64_t max_version = 0; // 0 = no gate
  std::uint8_t mechanisms = 0;   // VariableMechanism bits
  std::size_t weight = 10;
  variable_generator_t generator;
};

struct VariableConfig {
  std::vector<VariableSpec> specs;
};

namespace variable_detail {

bool eligible(VariableSpec const &spec, VariableMechanism mechanism,
              sql_variant::ServerInfo const &info);

// nullptr when no spec is eligible
VariableSpec const *pick(std::vector<VariableSpec> const &specs,
                         VariableMechanism mechanism,
                         sql_variant::ServerInfo const &info, ps_random &rand);

std::string render_value(VariableSpec const &spec,
                         sql_variant::ServerInfo const &info, ps_random &rand);

std::vector<std::string> build_sql(VariableSpec const &spec,
                                   std::string const &value,
                                   VariableMechanism mechanism,
                                   sql_variant::ServerInfo const &info);

} // namespace variable_detail

class SetVariable : public Action {
public:
  SetVariable(VariableConfig config, VariableMechanism mechanism);

  void execute(metadata::Context &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  VariableConfig config;
  VariableMechanism mechanism;
};

} // namespace action
