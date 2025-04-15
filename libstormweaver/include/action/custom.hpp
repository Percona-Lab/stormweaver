
#pragma once

#include "action/action.hpp"

#include <boost/container/flat_set.hpp>

namespace action {

struct CustomConfig {};

class CustomSql : public Action {
public:
  // Parameters stored as a string so we can implement dynamic dictionaries
  // later
  using inject_t = boost::container::flat_set<std::string>;

  CustomSql(CustomConfig const &config, std::string const &sqlStatement,
            inject_t injectParameters);

  void execute(metadata::Metadata &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  // CustomConfig config;
  std::string sqlStatement;
  inject_t injectParameters;

  std::string doInject(metadata::Metadata &metaCtx, ps_random &rand,
                       std::string const &injectionPoint) const;
};

}; // namespace action
