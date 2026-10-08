
#include "action/custom.hpp"
#include "action/helper.hpp"

#include <string_view>
#include <utility>

namespace action {

CustomSql::CustomSql(CustomConfig const & /*unused*/, std::string sqlStatement,
                     const inject_t &injectParameters)
    : sqlStatement(std::move(sqlStatement)),
      injectParameters(injectParameters) {
  // verify injetion parameters
  for (auto const &inject : injectParameters) {
    if (inject != "table") {
      throw std::runtime_error(
          "For now only table name can be injected to custom queries");
    }
  }
}

namespace {
void replace_all(std::string &s, std::string_view from, std::string_view to) {
  for (std::size_t pos = s.find(from); pos != std::string::npos;
       pos = s.find(from, pos + to.size())) {
    s.replace(pos, from.size(), to);
  }
}
} // namespace

void CustomSql::execute(metadata::Context &metaCtx, ps_random &rand,
                        sql_variant::LoggedSQL *connection) const {
  std::string statementCopy = sqlStatement;

  for (auto const &inject : injectParameters) {
    replace_all(statementCopy, "{" + inject + "}",
                doInject(metaCtx, rand, inject));
  }

  connection->executeQuery(statementCopy).maybeThrow();
}

std::string CustomSql::doInject(metadata::Context &metaCtx, ps_random &rand,
                                std::string const &injectionPoint) {
  if (injectionPoint == "table") {
    return find_random_table(metaCtx, rand)->name;
  }

  throw std::runtime_error(
      fmt::format("Unknown injection point: {}", injectionPoint));
}

} // namespace action
