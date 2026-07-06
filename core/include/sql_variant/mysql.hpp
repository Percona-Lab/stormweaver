
#pragma once

#include "sql_variant/generic.hpp"

struct MYSQL;

namespace sql_variant {

class MySQL : public GenericSQL {
public:
  MySQL(ServerParams const &params);
  ~MySQL() override;

  MySQL(MySQL &&) noexcept = default;
  MySQL &operator=(MySQL &&) noexcept = default;

  void logError(std::ostream &ostream) const override;

  [[nodiscard]] QueryResult
  executeQuery(std::string const &query) const override;

  [[nodiscard]] std::string serverInfoString() const override;

  [[nodiscard]] std::string hostInfo() const override;

  static void library_end();

  void reconnect() override {} // TODO

private:
  MYSQL *connection;

  [[nodiscard]] ServerInfo calculateServerInfo() const;
};
} // namespace sql_variant
