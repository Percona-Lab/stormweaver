
#pragma once

#include "sql_variant/generic.hpp"

struct PostgreSQL;

namespace pqxx {
class connection;
}

namespace sql_variant {

class PostgreSQL : public GenericSQL {
public:
  PostgreSQL(ServerParams const &params);
  ~PostgreSQL() override;

  PostgreSQL(PostgreSQL &&) noexcept = default;
  PostgreSQL &operator=(PostgreSQL &&) noexcept = default;

  void logError(std::ostream &ostream) const override;

  QueryResult executeQuery(std::string const &query) const override;

  std::string serverInfoString() const override;

  std::string hostInfo() const override;

  static void library_end();

  void reconnect() override;

private:
  ServerParams params;
  std::unique_ptr<pqxx::connection> connection;

  ServerInfo calculateServerInfo() const;
};
} // namespace sql_variant
