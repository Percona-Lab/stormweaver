
#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <vector>

namespace sql_variant {

enum class flavor { ANY_MYSQL, ANY_PG, ps, pxc, mysql, postgres, ppg };

struct ServerInfo {
  flavor flavor_;
  std::uint64_t version;

  bool is_mysql_like() const {
    return flavor_ == flavor::ps || flavor_ == flavor::pxc ||
           flavor_ == flavor::mysql || flavor_ == flavor::ANY_MYSQL;
  }

  bool is_pg_like() const {
    return flavor_ == flavor::postgres || flavor_ == flavor::ppg ||
           flavor_ == flavor::ANY_PG;
  }

  bool matching_any(flavor flav) const {
    if (flav == flavor::ANY_MYSQL && is_mysql_like())
      return true;
    if (flav == flavor::ANY_PG && is_pg_like())
      return true;
    return flav == flavor_;
  }

  bool after_or_is(flavor flav, std::uint64_t ver) const {
    if (!matching_any(flav))
      return false;

    return version >= ver;
  }

  bool before(flavor flav, std::uint64_t ver) const {
    if (!matching_any(flav))
      return false;

    return version < ver;
  }

  bool between(flavor flav, std::uint64_t verMin, std::uint64_t verMax) const {
    if (!matching_any(flav))
      return false;

    return version >= verMin && version <= verMax;
  }
};

struct ServerParams {
  std::string database;
  std::string address;
  std::string socket;
  std::string username;
  std::string password;

  std::uint16_t port;
};

struct QueryResult;

class SqlException : public std::exception {
public:
  SqlException(std::string const &message) : message(message) {}

  const char *what() const noexcept override { return message.c_str(); }

private:
  std::string message;
};

enum class SqlStatus { success, error, serverGone };

struct ErrorInfo {
  std::string errorCode;
  std::string errorMessage;
  SqlStatus errorStatus;

  bool success() const { return errorStatus == SqlStatus::success; }
  bool serverGone() const { return errorStatus == SqlStatus::serverGone; }
};

struct RowView {
  std::vector<std::optional<std::string_view>> rowData;
};

struct QuerySpecificResult {
  virtual ~QuerySpecificResult();

  virtual std::size_t numFields() const = 0;
  virtual std::size_t numRows() const = 0;

  virtual RowView nextRow() const = 0;
};

struct QueryResult {
  std::string query;
  std::chrono::high_resolution_clock::time_point executedAt;
  std::chrono::nanoseconds executionTime;
  ErrorInfo errorInfo;
  std::uint64_t affectedRows = 0;

  std::unique_ptr<QuerySpecificResult> data;

  explicit operator bool() const { return errorInfo.success(); }

  bool success() const { return errorInfo.success(); }

  void maybeThrow() const {
    if (!success()) {
      throw SqlException(fmt::format("Error while executing query: {} {}",
                                     errorInfo.errorCode,
                                     errorInfo.errorMessage));
    }
  }
};

class GenericSQL {
public:
  GenericSQL() {}
  virtual ~GenericSQL();

  GenericSQL(GenericSQL const &) = delete;
  GenericSQL &operator=(GenericSQL const &) = delete;

  GenericSQL(GenericSQL &&) noexcept = default;
  GenericSQL &operator=(GenericSQL &&) noexcept = default;

  virtual void logError(std::ostream &ostream) const = 0;

  virtual QueryResult executeQuery(std::string const &query) const = 0;

  virtual std::string serverInfoString() const = 0;

  ServerInfo serverInfo() const;

  virtual std::string hostInfo() const = 0;

  virtual void reconnect() = 0;

protected:
  ServerInfo serverInfo_;
};

class LoggedSQL {
public:
  ServerInfo serverInfo() const;

  LoggedSQL(std::unique_ptr<GenericSQL> sql, std::string const &logName);

  [[nodiscard]] QueryResult executeQuery(std::string const &query) const;

  [[nodiscard]] std::optional<std::string_view>
  querySingleValue(const std::string &sql) const;

  void reconnect();

private:
  std::unique_ptr<GenericSQL> sql;
  std::shared_ptr<spdlog::logger> logger;
};

} // namespace sql_variant
