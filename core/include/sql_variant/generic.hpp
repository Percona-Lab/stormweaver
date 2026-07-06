
#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sql_variant {

enum class flavor : std::uint8_t {
  ANY_MYSQL,
  ANY_PG,
  ps,
  pxc,
  mysql,
  postgres,
  ppg
};

struct ServerInfo {
  flavor flavor_;
  std::uint64_t version;

  [[nodiscard]] bool is_mysql_like() const {
    return flavor_ == flavor::ps || flavor_ == flavor::pxc ||
           flavor_ == flavor::mysql || flavor_ == flavor::ANY_MYSQL;
  }

  [[nodiscard]] bool is_pg_like() const {
    return flavor_ == flavor::postgres || flavor_ == flavor::ppg ||
           flavor_ == flavor::ANY_PG;
  }

  [[nodiscard]] bool matching_any(flavor flav) const {
    if (flav == flavor::ANY_MYSQL && is_mysql_like()) {
      return true;
    }
    if (flav == flavor::ANY_PG && is_pg_like()) {
      return true;
    }
    return flav == flavor_;
  }

  [[nodiscard]] bool after_or_is(flavor flav, std::uint64_t ver) const {
    if (!matching_any(flav)) {
      return false;
    }

    return version >= ver;
  }

  [[nodiscard]] bool before(flavor flav, std::uint64_t ver) const {
    if (!matching_any(flav)) {
      return false;
    }

    return version < ver;
  }

  [[nodiscard]] bool between(flavor flav, std::uint64_t verMin,
                             std::uint64_t verMax) const {
    if (!matching_any(flav)) {
      return false;
    }

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

enum class SqlStatus : std::uint8_t { success, error, serverGone };

class SqlException : public std::exception {
public:
  SqlException(std::string errorCode, std::string message,
               SqlStatus status = SqlStatus::error)
      : errorCode(std::move(errorCode)), message(std::move(message)),
        status(status) {}

  [[nodiscard]] const char *what() const noexcept override {
    return message.c_str();
  }

  [[nodiscard]] const std::string &getErrorCode() const noexcept {
    return errorCode;
  }

  [[nodiscard]] bool serverGone() const {
    return status == SqlStatus::serverGone;
  }

private:
  std::string errorCode;
  std::string message;
  SqlStatus status;
};

struct ErrorInfo {
  std::string errorCode;
  std::string errorMessage;
  SqlStatus errorStatus;

  [[nodiscard]] bool success() const {
    return errorStatus == SqlStatus::success;
  }
  [[nodiscard]] bool serverGone() const {
    return errorStatus == SqlStatus::serverGone;
  }
};

struct RowView {
  std::vector<std::optional<std::string_view>> rowData;
};

struct QuerySpecificResult {
  virtual ~QuerySpecificResult();

  [[nodiscard]] virtual std::size_t numFields() const = 0;
  [[nodiscard]] virtual std::size_t numRows() const = 0;

  [[nodiscard]] virtual RowView nextRow() const = 0;

  // random access; implementations must not depend on nextRow() cursor state
  // (mysql impl does seek the underlying cursor); throws on index >= numRows
  [[nodiscard]] virtual RowView rowAt(std::size_t index) const = 0;
};

struct QueryResult {
  std::string query;
  std::chrono::high_resolution_clock::time_point executedAt;
  std::chrono::nanoseconds executionTime;
  ErrorInfo errorInfo;
  std::uint64_t affectedRows = 0;

  std::unique_ptr<QuerySpecificResult> data;

  explicit operator bool() const { return errorInfo.success(); }

  [[nodiscard]] bool success() const { return errorInfo.success(); }

  void maybeThrow() const {
    if (!success()) {
      throw SqlException(errorInfo.errorCode,
                         fmt::format("Error while executing query: {} {}",
                                     errorInfo.errorCode,
                                     errorInfo.errorMessage),
                         errorInfo.errorStatus);
    }
  }
};

class GenericSQL {
public:
  GenericSQL() = default;
  virtual ~GenericSQL();

  GenericSQL(GenericSQL const &) = delete;
  GenericSQL &operator=(GenericSQL const &) = delete;

  GenericSQL(GenericSQL &&) noexcept = default;
  GenericSQL &operator=(GenericSQL &&) noexcept = default;

  virtual void logError(std::ostream &ostream) const = 0;

  [[nodiscard]] virtual QueryResult
  executeQuery(std::string const &query) const = 0;

  [[nodiscard]] virtual std::string serverInfoString() const = 0;

  [[nodiscard]] ServerInfo serverInfo() const;

  [[nodiscard]] virtual std::string hostInfo() const = 0;

  virtual void reconnect() = 0;

protected:
  ServerInfo serverInfo_;
};

class LoggedSQL {
public:
  ServerInfo serverInfo() const;

  LoggedSQL(std::unique_ptr<GenericSQL> sql, std::string const &logName);

  [[nodiscard]] QueryResult executeQuery(std::string const &query) const;

  // owning string: the view would dangle once the QueryResult dies
  [[nodiscard]] std::optional<std::string>
  querySingleValue(const std::string &sql) const;

  void reconnect();

  std::chrono::nanoseconds getAccumulatedSqlTime() const;
  void resetAccumulatedSqlTime();

private:
  std::unique_ptr<GenericSQL> sql;
  std::shared_ptr<spdlog::logger> logger;
  mutable std::chrono::nanoseconds accumulatedSqlTime{0};
};

} // namespace sql_variant
