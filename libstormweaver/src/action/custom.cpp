
#include "action/custom.hpp"

#include <boost/algorithm/string.hpp>

namespace action {

CustomSql::CustomSql(CustomConfig const &, std::string const &sqlStatement,
                     inject_t injectParameters)
    : sqlStatement(sqlStatement), injectParameters(injectParameters) {
  // verify injetion parameters
  for (auto const &inject : injectParameters) {
    if (inject != "table") {
      throw std::runtime_error(
          "For now only table name can be injected to custom queries");
    }
  }
}

void CustomSql::execute(metadata::Metadata &metaCtx, ps_random &rand,
                        sql_variant::LoggedSQL *connection) const {
  std::string statementCopy = sqlStatement;

  for (auto const &inject : injectParameters) {
    // TODO: fmt::format doesn't support dynamic parameters, should switch to
    // fmt
    boost::replace_all(statementCopy, "{" + inject + "}",
                       doInject(metaCtx, rand, inject));
  }

  connection->executeQuery(statementCopy).maybeThrow();
}

std::string CustomSql::doInject(metadata::Metadata &metaCtx, ps_random &rand,
                                std::string const &injectionPoint) const {
  if (injectionPoint == "table") {
    metadata::table_cptr table;
    while (table == NULL) {
      // select a random table from metadata
      std::size_t idx = rand.random_number<std::size_t>(0, metaCtx.size() - 1);
      table = metaCtx[idx];
    }
    return table->name;
  }

  throw std::runtime_error(
      fmt::format("Unknown injection point: {}", injectionPoint));
}

} // namespace action
