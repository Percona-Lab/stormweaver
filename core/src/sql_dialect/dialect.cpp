#include "sql_dialect/dialect.hpp"

namespace sql_dialect {

Dialect const &dialect_for(sql_variant::ServerInfo const &info) {
  return info.is_mysql_like() ? mysql_dialect() : pg_dialect();
}

} // namespace sql_dialect
