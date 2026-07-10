
#include "sql_variant/generic.hpp"

#include <cctype>
#include <fmt/format.h>

#include "logging.hpp"

namespace {
// binary values are logged as length only, never raw bytes
std::string describe_params(std::vector<sql_variant::Param> const &params) {
  std::string out = "[";
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (i != 0) {
      out += ", ";
    }
    auto const &p = params[i];
    if (!p.value) {
      out += "NULL";
    } else if (p.binary) {
      out += fmt::format("<bytes:{}>", p.value->size());
    } else {
      out += fmt::format("'{}'", *p.value);
    }
  }
  return out + "]";
}
} // namespace

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

  ++queryCount;
  auto res = sql->executeQuery(query);
  accumulatedSqlTime += res.executionTime;

  if (!res.success()) {
    logger->error("Error while executing SQL statement: {} {}",
                  res.errorInfo.errorCode, res.errorInfo.errorMessage);
  } else {
    observeResult(query, res);
  }

  return res;
}

QueryResult LoggedSQL::executeParams(std::string const &query,
                                     std::vector<Param> const &params) const {
  logger->info("Statement: {} params: {}", query, describe_params(params));

  ++queryCount;
  auto res = sql->executeParams(query, params);
  accumulatedSqlTime += res.executionTime;

  if (!res.success()) {
    logger->error("Error while executing SQL statement: {} {}",
                  res.errorInfo.errorCode, res.errorInfo.errorMessage);
  } else {
    observeResult(query, res);
  }

  return res;
}

QueryResult LoggedSQL::safeQuery(std::string const &query,
                                 std::vector<Param> const &params) const {
  auto res =
      params.empty() ? executeQuery(query) : executeParams(query, params);
  res.maybeThrow();
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

std::uint64_t LoggedSQL::getQueryCount() const { return queryCount; }

ActionNameScope::ActionNameScope(LoggedSQL &conn, std::string name)
    : conn(conn), prev(conn.currentAction()) {
  conn.setCurrentAction(std::move(name));
}

ActionNameScope::~ActionNameScope() { conn.setCurrentAction(std::move(prev)); }

void LoggedSQL::setCurrentAction(std::string name) {
  currentAction_ = std::move(name);
}

std::string const &LoggedSQL::currentAction() const { return currentAction_; }

ActionNameScope LoggedSQL::scopedActionName(std::string name) {
  return {*this, std::move(name)};
}

void LoggedSQL::recordTransactionOutcome(
    statistics::TransactionOutcome const &outcome) {
  txnOutcomes.push_back(outcome);
}

std::vector<statistics::RowObservation> LoggedSQL::drainRowObservations() {
  return std::exchange(rowObservations, {});
}

std::vector<statistics::TransactionOutcome>
LoggedSQL::drainTransactionOutcomes() {
  return std::exchange(txnOutcomes, {});
}

void LoggedSQL::clearObservations() {
  rowObservations.clear();
  txnOutcomes.clear();
}

void LoggedSQL::observeResult(std::string const &query,
                              QueryResult const &res) const {
  const auto kind = classifyStatement(query);
  // a row-shaped result has fields; DML/DDL results carry no fields even
  // though res.data itself is always wrapped by the drivers
  const bool hasData = res.data != nullptr && res.data->numFields() > 0;

  const double ms =
      static_cast<double>(res.executionTime.count()) / 1'000'000.0;
  if (hasData) {
    logger->info("Result: rows={} time={:.2f}ms", res.data->numRows(), ms);
  } else if (kind == StmtKind::insert || kind == StmtKind::update ||
             kind == StmtKind::del || kind == StmtKind::with) {
    logger->info("Result: affected={} time={:.2f}ms", res.affectedRows, ms);
  } else {
    logger->info("Result: ok time={:.2f}ms", ms);
  }

  switch (kind) {
  case StmtKind::select:
    rowObservations.push_back({.action = currentAction_,
                               .kind = "select",
                               .rows = hasData ? res.data->numRows() : 0});
    break;
  case StmtKind::insert:
    rowObservations.push_back(
        {.action = currentAction_, .kind = "insert", .rows = res.affectedRows});
    break;
  case StmtKind::update:
    rowObservations.push_back(
        {.action = currentAction_, .kind = "update", .rows = res.affectedRows});
    break;
  case StmtKind::del:
    rowObservations.push_back(
        {.action = currentAction_, .kind = "delete", .rows = res.affectedRows});
    break;
  case StmtKind::with:
    if (hasData) {
      rowObservations.push_back({.action = currentAction_,
                                 .kind = "select",
                                 .rows = res.data->numRows()});
    } else {
      rowObservations.push_back(
          {.action = currentAction_, .kind = "dml", .rows = res.affectedRows});
    }
    break;
  case StmtKind::other:
    break;
  }
}

StmtKind classifyStatement(std::string_view query) {
  std::size_t pos = 0;
  while (pos < query.size()) {
    const char c = query[pos];
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      ++pos;
      continue;
    }
    if (query.compare(pos, 2, "--") == 0) {
      const auto eol = query.find('\n', pos);
      if (eol == std::string_view::npos) {
        return StmtKind::other;
      }
      pos = eol + 1;
      continue;
    }
    if (query.compare(pos, 2, "/*") == 0) {
      const auto end = query.find("*/", pos + 2);
      if (end == std::string_view::npos) {
        return StmtKind::other;
      }
      pos = end + 2;
      continue;
    }
    break;
  }

  auto wordEnd = pos;
  while (wordEnd < query.size() &&
         (std::isalnum(static_cast<unsigned char>(query[wordEnd])) != 0 ||
          query[wordEnd] == '_')) {
    ++wordEnd;
  }
  std::string word(query.substr(pos, wordEnd - pos));
  for (auto &ch : word) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }

  if (word == "select") {
    return StmtKind::select;
  }
  if (word == "insert") {
    return StmtKind::insert;
  }
  if (word == "update") {
    return StmtKind::update;
  }
  if (word == "delete") {
    return StmtKind::del;
  }
  if (word == "with") {
    return StmtKind::with;
  }
  return StmtKind::other;
}

} // namespace sql_variant
