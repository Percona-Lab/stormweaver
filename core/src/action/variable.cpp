
#include "action/variable.hpp"

#include <fmt/format.h>

namespace action {
namespace variable_detail {

bool eligible(VariableSpec const &spec, VariableMechanism mechanism,
              sql_variant::ServerInfo const &info) {
  if ((spec.mechanisms & static_cast<std::uint8_t>(mechanism)) == 0) {
    return false;
  }
  if (!info.matching_any(spec.flavor)) {
    return false;
  }
  if (spec.min_version != 0 && info.version < spec.min_version) {
    return false;
  }
  if (spec.max_version != 0 && info.version > spec.max_version) {
    return false;
  }
  return true;
}

VariableSpec const *pick(std::vector<VariableSpec> const &specs,
                         VariableMechanism mechanism,
                         sql_variant::ServerInfo const &info, ps_random &rand) {
  std::size_t total = 0;
  for (auto const &spec : specs) {
    if (eligible(spec, mechanism, info)) {
      total += spec.weight;
    }
  }
  if (total == 0) {
    return nullptr;
  }
  auto roll = rand.random_number<std::size_t>(1, total);
  for (auto const &spec : specs) {
    if (!eligible(spec, mechanism, info)) {
      continue;
    }
    if (roll <= spec.weight) {
      return &spec;
    }
    roll -= spec.weight;
  }
  return nullptr; // unreachable: roll <= total by construction
}

// mirrored by python/stormweaver/variables.py render_value - keep in sync
std::string render_value(VariableSpec const &spec,
                         sql_variant::ServerInfo const &info, ps_random &rand) {
  return std::visit(
      [&](auto const &gen) -> std::string {
        using T = std::decay_t<decltype(gen)>;
        if constexpr (std::is_same_v<T, VariableChoices>) {
          return gen.values.at(
              rand.random_number<std::size_t>(0, gen.values.size() - 1));
        } else if constexpr (std::is_same_v<T, VariableIntRange>) {
          // ranges are trusted config: must be non-negative and k*step must
          // fit int64 (ps_random's integer path takes size_t)
          auto const k = rand.random_number<std::int64_t>(gen.min, gen.max);
          auto value = fmt::format("{}{}", k * gen.step, gen.suffix);
          if (info.is_pg_like() && !gen.suffix.empty()) {
            value = fmt::format("'{}'", value);
          }
          return value;
        } else {
          static_assert(std::is_same_v<T, VariableBool>);
          bool const on = rand.random_bool();
          if (info.is_pg_like()) {
            return on ? "on" : "off";
          }
          return on ? "ON" : "OFF";
        }
      },
      spec.generator);
}

std::vector<std::string> build_sql(VariableSpec const &spec,
                                   std::string const &value,
                                   VariableMechanism mechanism,
                                   sql_variant::ServerInfo const &info) {
  switch (mechanism) {
  case VariableMechanism::session:
    if (info.is_pg_like()) {
      return {fmt::format("SET {} = {}", spec.name, value)};
    }
    return {fmt::format("SET SESSION {} = {}", spec.name, value)};
  case VariableMechanism::global:
    return {fmt::format("SET GLOBAL {} = {}", spec.name, value)};
  case VariableMechanism::reload:
    return {fmt::format("ALTER SYSTEM SET {} = {}", spec.name, value),
            "SELECT pg_reload_conf()"};
  default:
    throw ActionException("invalid-variable-mechanism",
                          "startup is not a runtime mechanism");
  }
}

} // namespace variable_detail

SetVariable::SetVariable(VariableConfig config, VariableMechanism mechanism)
    : config(std::move(config)), mechanism(mechanism) {}

void SetVariable::execute(metadata::Context & /*metaCtx*/, ps_random &rand,
                          sql_variant::LoggedSQL *connection) const {
  auto const info = connection->serverInfo();
  auto const *spec = variable_detail::pick(config.specs, mechanism, info, rand);
  if (spec == nullptr) {
    return; // nothing eligible for this flavor/mechanism: no-op success
  }
  auto const value = variable_detail::render_value(*spec, info, rand);
  for (auto const &sql :
       variable_detail::build_sql(*spec, value, mechanism, info)) {
    connection->executeQuery(sql).maybeThrow();
  }
}

} // namespace action
