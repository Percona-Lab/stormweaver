
#pragma once

#include "sql_variant/generic.hpp"

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

  [[nodiscard]] QueryResult
  executeQuery(std::string const &query) const override;

  [[nodiscard]] QueryResult
  executeParams(std::string const &query,
                std::vector<Param> const &params) const override;

  [[nodiscard]] std::string serverInfoString() const override;

  [[nodiscard]] std::string hostInfo() const override;

  static void library_end();

  void reconnect() override;

private:
  ServerParams params;
  std::unique_ptr<pqxx::connection> connection;

  [[nodiscard]] static ServerInfo calculateServerInfo();
};
} // namespace sql_variant
