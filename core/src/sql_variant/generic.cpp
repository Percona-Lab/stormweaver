
#include "sql_variant/generic.hpp"

#include <fmt/format.h>

#include "logging.hpp"

namespace sql_variant {

QuerySpecificResult::~QuerySpecificResult() = default;

GenericSQL::~GenericSQL() = default;

ServerInfo GenericSQL::serverInfo() const { return serverInfo_; }

LoggedSQL::LoggedSQL(std::unique_ptr<GenericSQL> sql,
                     std::string const &logName)
    : sql(std::move(sql)), logger(logging::make_file_logger(
                               fmt::format("sql-conn-{}", logName),
                               fmt::format("sql-conn-{}.log", logName))) {
  //
}

ServerInfo LoggedSQL::serverInfo() const { return sql->serverInfo(); }

QueryResult LoggedSQL::executeQuery(std::string const &query) const {
  logger->info("Statement: {}", query);

  auto res = sql->executeQuery(query);
  accumulatedSqlTime += res.executionTime;

  if (!res.success()) {
    logger->error("Error while executing SQL statement: {} {}",
                  res.errorInfo.errorCode, res.errorInfo.errorMessage);
  }

  return res;
}

std::optional<std::string>
LoggedSQL::querySingleValue(const std::string &sql) const {

  const auto res = executeQuery(sql);

  if (!res.success()) {
    return std::nullopt;
  }

  if (res.data == nullptr || res.data->numFields() < 1 ||
      res.data->numRows() < 1) {
    logger->error("Received no data from the server");
    return std::nullopt;
  }

  const auto row = res.data->nextRow();

  if (!row.rowData[0].has_value()) {
    return std::nullopt;
  }

  return std::string(*row.rowData[0]);
}

void LoggedSQL::reconnect() { sql->reconnect(); }

std::chrono::nanoseconds LoggedSQL::getAccumulatedSqlTime() const {
  return accumulatedSqlTime;
}

void LoggedSQL::resetAccumulatedSqlTime() {
  accumulatedSqlTime = std::chrono::nanoseconds{0};
}

} // namespace sql_variant
