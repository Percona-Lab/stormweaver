
#include "sql_variant/generic.hpp"

#include <fmt/format.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace sql_variant {

QuerySpecificResult::~QuerySpecificResult() {}

GenericSQL::~GenericSQL() {}

ServerInfo GenericSQL::serverInfo() const { return serverInfo_; }

LoggedSQL::LoggedSQL(std::unique_ptr<GenericSQL> sql,
                     std::string const &logName)
    : sql(std::move(sql)),
      logger(spdlog::get(fmt::format("sql-conn-{}", logName))
                 ? spdlog::get(fmt::format("sql-conn-{}", logName))
                 : spdlog::basic_logger_st(
                       fmt::format("sql-conn-{}", logName),
                       fmt::format("logs/sql-conn-{}.log", logName))) {
  //
}

ServerInfo LoggedSQL::serverInfo() const { return sql->serverInfo(); }

QueryResult LoggedSQL::executeQuery(std::string const &query) const {
  //
  logger->info("Statement: {}", query);

  auto res = sql->executeQuery(query);

  if (!res.success()) {
    logger->error("Error while executing SQL statement: {} {}",
                  res.errorInfo.errorCode, res.errorInfo.errorMessage);
  }

  return res;
}

std::optional<std::string_view>
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

  return row.rowData[0];
}

void LoggedSQL::reconnect() { sql->reconnect(); }

} // namespace sql_variant
