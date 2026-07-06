#include "schema_discovery.hpp"

#include <stdexcept>

namespace schema_discovery {

std::unique_ptr<SchemaDiscovery>
make_schema_discovery(sql_variant::LoggedSQL *connection) {
  if (connection == nullptr) {
    throw std::invalid_argument("Connection cannot be null");
  }
  if (connection->serverInfo().is_mysql_like()) {
    return std::make_unique<MySqlSchemaDiscovery>(connection);
  }
  return std::make_unique<PgSchemaDiscovery>(connection);
}

} // namespace schema_discovery
