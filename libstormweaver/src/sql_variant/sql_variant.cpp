
#include "sql_variant/sql_variant.hpp"

namespace sql_variant {

std::unique_ptr<GenericSQL> connect(std::string serverType,
                                    ServerParams params) {
  if (serverType == "mysql") {
    return std::make_unique<MySQL>(params);
  } else if (serverType == "postgres") {
    return std::make_unique<PostgreSQL>(params);
  } else {
    throw SqlException("unknown-database-type",
                       std::string("Unknown database type: ") + serverType);
  }
}

} // namespace sql_variant