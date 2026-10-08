
#pragma once

#include "action/action.hpp"

#include <string>
#include <version>
#ifdef __cpp_lib_flat_set
#include <flat_set>
#else
#include <set>
#endif

namespace action {

struct CustomConfig {};

class CustomSql : public Action {
public:
  // Parameters stored as a string so we can implement dynamic dictionaries
  // later
#ifdef __cpp_lib_flat_set
  using inject_t = std::flat_set<std::string>;
#else
  using inject_t = std::set<std::string>;
#endif

  CustomSql(CustomConfig const &config, std::string sqlStatement,
            const inject_t &injectParameters);

  void execute(metadata::Context &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  // CustomConfig config;
  std::string sqlStatement;
  inject_t injectParameters;

  static std::string doInject(metadata::Context &metaCtx, ps_random &rand,
                              std::string const &injectionPoint);
};

}; // namespace action
