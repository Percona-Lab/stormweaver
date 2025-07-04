
#include "sql_variant/postgresql.hpp"

#include <mutex>
#include <pqxx/pqxx>
#include <sstream>

#include <iostream>

namespace {
struct PostgreSQLSpecificResult : sql_variant::QuerySpecificResult {

  pqxx::result result;
  mutable std::size_t rowIdx;

  PostgreSQLSpecificResult(pqxx::result result) : result(result), rowIdx(0) {}

  ~PostgreSQLSpecificResult() override {}

  std::size_t numFields() const override { return result.columns(); }

  std::size_t numRows() const override { return std::size(result); }

  sql_variant::RowView nextRow() const override {

    sql_variant::RowView rowResult;
    rowResult.rowData.resize(numFields());

    pqxx::row row = result[rowIdx];
    rowIdx++;

    for (std::size_t colnum = 0u; colnum < numFields(); ++colnum) {
      if (!row[colnum].is_null())
        rowResult.rowData[colnum] = row[colnum].view();
    }

    return rowResult;
  }
};

std::string build_connection_string(sql_variant::ServerParams const &params) {
  std::string ret;

  ret += "dbname=";
  ret += params.database;

  if (!params.username.empty()) {
    ret += " user=";
    ret += params.username;
  }

  if (!params.password.empty()) {
    ret += " password=";
    ret += params.password;
  }

  if (!params.address.empty()) {
    ret += " host=";
    ret += params.address;
  } else if (!params.socket.empty()) {
    ret += " host=";
    ret += params.socket;
  }

  if (params.port != 0 && params.port != 5432) {
    ret += " port=";
    ret += std::to_string(params.port);
  }

  return ret;
}

} // namespace

namespace sql_variant {

PostgreSQL::PostgreSQL(ServerParams const &params) try
    : params(params), connection(std::make_unique<pqxx::connection>(
                          build_connection_string(params))) {
  serverInfo_ = calculateServerInfo();
} catch (std::exception &err) {
  throw SqlException("pg-connection-failed", err.what());
}

PostgreSQL::~PostgreSQL() {}

void PostgreSQL::logError(std::ostream &) const {
  // ostream << PostgreSQL_errno(connection) << ": " <<
  // PostgreSQL_error(connection);
  //  nop
}

QueryResult PostgreSQL::executeQuery(std::string const &query) const {
  QueryResult result;

  try {
    // TODO: explicit transactions!
    pqxx::nontransaction work(*connection);

    result.executedAt = std::chrono::high_resolution_clock::now();
    const auto qres = work.exec(query);

    // work.commit(); no need with nontransaction

    const auto end = std::chrono::high_resolution_clock::now();
    result.executionTime = end - result.executedAt;
    result.errorInfo.errorStatus = SqlStatus::success;

    result.affectedRows = qres.affected_rows();
    result.data = std::make_unique<PostgreSQLSpecificResult>(qres);

  } catch (pqxx::sql_error const &e) {
    const auto end = std::chrono::high_resolution_clock::now();
    result.executionTime = end - result.executedAt;

    result.errorInfo.errorCode = e.sqlstate();
    result.errorInfo.errorMessage = e.what();
    result.errorInfo.errorStatus = SqlStatus::error;
  } catch (pqxx::broken_connection const &e) {
    const auto end = std::chrono::high_resolution_clock::now();
    result.executionTime = end - result.executedAt;

    result.errorInfo.errorCode = "0";
    result.errorInfo.errorMessage = e.what();
    result.errorInfo.errorStatus = SqlStatus::serverGone;
  }

  return result;
}

std::string PostgreSQL::serverInfoString() const {
  return ""; // TODO
}

ServerInfo PostgreSQL::calculateServerInfo() const { // TODO
  std::uint64_t server_version = 0;
  flavor flav = flavor::postgres;

  return {flav, server_version};
}

std::string PostgreSQL::hostInfo() const {
  return ""; // TODO
}

void PostgreSQL::reconnect() {
  connection =
      std::make_unique<pqxx::connection>(build_connection_string(params));
}

} // namespace sql_variant
