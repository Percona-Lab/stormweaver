
#include "sql_variant/postgresql.hpp"

#include <mutex>
#include <pqxx/pqxx>
#include <sstream>
#include <stdexcept>

#include <iostream>
#include <utility>

namespace {
struct PostgreSQLSpecificResult : sql_variant::QuerySpecificResult {

  pqxx::result result;
  mutable std::size_t rowIdx{0};

  PostgreSQLSpecificResult(pqxx::result result) : result(std::move(result)) {}

  ~PostgreSQLSpecificResult() override = default;

  std::size_t numFields() const override { return result.columns(); }

  std::size_t numRows() const override { return std::size(result); }

  sql_variant::RowView nextRow() const override {

    sql_variant::RowView rowResult;
    rowResult.rowData.resize(numFields());

    pqxx::row row = result[static_cast<int>(rowIdx)];
    rowIdx++;

    for (std::size_t colnum = 0U; colnum < numFields(); ++colnum) {
      auto col = static_cast<int>(colnum);
      if (!row[col].is_null()) {
        rowResult.rowData[colnum] = row[col].view();
      }
    }

    return rowResult;
  }

  sql_variant::RowView rowAt(std::size_t index) const override {
    if (index >= numRows()) {
      throw std::out_of_range("row index out of range");
    }

    sql_variant::RowView rowResult;
    rowResult.rowData.resize(numFields());

    pqxx::row row = result[static_cast<int>(index)];

    for (std::size_t colnum = 0U; colnum < numFields(); ++colnum) {
      auto col = static_cast<int>(colnum);
      if (!row[col].is_null()) {
        rowResult.rowData[colnum] = row[col].view();
      }
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

template <typename Fn>
sql_variant::QueryResult run_pg_query(std::string const &query, Fn &&fn) {
  sql_variant::QueryResult result;
  result.query = query;

  try {
    result.executedAt = std::chrono::high_resolution_clock::now();
    const auto qres = fn();

    const auto end = std::chrono::high_resolution_clock::now();
    result.executionTime = end - result.executedAt;
    result.errorInfo.errorStatus = sql_variant::SqlStatus::success;

    result.affectedRows = qres.affected_rows();
    result.data = std::make_unique<PostgreSQLSpecificResult>(qres);

  } catch (pqxx::sql_error const &e) {
    const auto end = std::chrono::high_resolution_clock::now();
    result.executionTime = end - result.executedAt;

    result.errorInfo.errorCode = e.sqlstate();
    result.errorInfo.errorMessage = e.what();
    result.errorInfo.errorStatus = sql_variant::SqlStatus::error;
    result.errorInfo.errorClass =
        sql_variant::classify_pg_sqlstate(e.sqlstate());
  } catch (pqxx::broken_connection const &e) {
    const auto end = std::chrono::high_resolution_clock::now();
    result.executionTime = end - result.executedAt;

    result.errorInfo.errorCode = "0";
    result.errorInfo.errorMessage = e.what();
    result.errorInfo.errorStatus = sql_variant::SqlStatus::serverGone;
    result.errorInfo.errorClass = sql_variant::ErrorClass::serverGone;
  }

  return result;
}

} // namespace

namespace sql_variant {

ErrorClass classify_pg_sqlstate(std::string_view sqlstate) {
  if (sqlstate == "40001" || sqlstate == "40P01") {
    return ErrorClass::conflict;
  }
  if (sqlstate == "25P02") {
    return ErrorClass::failedTxn;
  }
  return ErrorClass::other;
}

PostgreSQL::PostgreSQL(ServerParams const &params) try
    : params(params), connection(std::make_unique<pqxx::connection>(
                          build_connection_string(params))) {
  serverInfo_ = calculateServerInfo();
} catch (std::exception &err) {
  throw SqlException("pg-connection-failed", err.what());
}

PostgreSQL::~PostgreSQL() = default;

void PostgreSQL::logError(std::ostream & /*ostream*/) const {
  // ostream << PostgreSQL_errno(connection) << ": " <<
  // PostgreSQL_error(connection);
  //  nop
}

QueryResult PostgreSQL::executeQuery(std::string const &query) const {
  return run_pg_query(query, [&] {
    // TODO: explicit transactions!
    pqxx::nontransaction work(*connection);
    return work.exec(query);
  });
}

// PQexecParams under the hood: single statement only, no multi-statement
// scripts
QueryResult PostgreSQL::executeParams(std::string const &query,
                                      std::vector<Param> const &params) const {
  return run_pg_query(query, [&] {
    pqxx::nontransaction work(*connection);
    pqxx::params pq_params;
    for (auto const &param : params) {
      if (!param.value) {
        pq_params.append();
      } else if (param.binary) {
        pq_params.append(
            pqxx::binary_cast(param.value->data(), param.value->size()));
      } else {
        pq_params.append(*param.value);
      }
    }
    return work.exec_params(query, pq_params);
  });
}

std::string PostgreSQL::serverInfoString() const {
  return ""; // TODO
}

ServerInfo PostgreSQL::calculateServerInfo() const {
  std::uint64_t server_version = 0;
  if (connection != nullptr) {
    server_version = static_cast<std::uint64_t>(connection->server_version());
  }

  return {.flavor_ = flavor::postgres, .version = server_version};
}

std::string PostgreSQL::hostInfo() const {
  return ""; // TODO
}

void PostgreSQL::reconnect() {
  connection =
      std::make_unique<pqxx::connection>(build_connection_string(params));
}

} // namespace sql_variant
