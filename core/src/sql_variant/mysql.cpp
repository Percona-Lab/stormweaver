
#include "sql_variant/mysql.hpp"

#include <limits>
#include <mutex>
#include <mysql.h>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>

// #include "common.hpp"
#ifndef MAX_PACKET_DEFAULT
#define MAX_PACKET_DEFAULT 67108864
#endif

namespace {
struct MySQLSpecificResult : sql_variant::QuerySpecificResult {
  MYSQL_RES *res;
  std::size_t num_fields;

  MySQLSpecificResult(MYSQL_RES *res)
      : res(res), num_fields(res == nullptr ? 0 : mysql_num_fields(res)) {}

  ~MySQLSpecificResult() override {
    if (res != nullptr)
      mysql_free_result(res);
  }

  std::size_t numFields() const override { return num_fields; }

  std::size_t numRows() const override {
    return res == nullptr ? 0 : mysql_num_rows(res);
  }

  sql_variant::RowView nextRow() const override {
    if (res == nullptr) {
      throw sql_variant::SqlException("mysql-no-select",
                                      "Not a SELECT-like statement!");
    }

    sql_variant::RowView ret;
    const auto mdata = mysql_fetch_row(res);
    const auto lengths = mysql_fetch_lengths(res);

    if (mdata == nullptr) {
      throw sql_variant::SqlException("mysql-no-more-rows", "No more rows");
    }

    ret.rowData.resize(num_fields);

    for (std::size_t i = 0; i < num_fields; ++i) {
      if (mdata[i] != nullptr) {
        ret.rowData[i] = std::string_view(mdata[i], lengths[i]);
      }
    }

    return ret;
  }

  sql_variant::RowView rowAt(std::size_t index) const override {
    if (res == nullptr) {
      throw sql_variant::SqlException("mysql-no-select",
                                      "Not a SELECT-like statement!");
    }

    if (index >= numRows()) {
      throw std::out_of_range("row index out of range");
    }

    mysql_data_seek(res, index);

    sql_variant::RowView ret;
    const auto mdata = mysql_fetch_row(res);
    const auto lengths = mysql_fetch_lengths(res);

    if (mdata == nullptr) {
      throw sql_variant::SqlException("mysql-no-more-rows", "No more rows");
    }

    ret.rowData.resize(num_fields);

    for (std::size_t i = 0; i < num_fields; ++i) {
      if (mdata[i] != nullptr) {
        ret.rowData[i] = std::string_view(mdata[i], lengths[i]);
      }
    }

    return ret;
  }
};

unsigned long parseVersionComponent(std::string const &s) {
  try {
    return std::stoul(s);
  } catch (...) {
    return 0;
  }
}

sql_variant::SqlStatus statusForError(unsigned int errCode) {
  return (errCode == CR_SERVER_GONE_ERROR || errCode == CR_SERVER_LOST)
             ? sql_variant::SqlStatus::serverGone
             : sql_variant::SqlStatus::error;
}

// nullopt when mysql_real_escape_string rejects the value (invalid
// multibyte data, or NO_BACKSLASH_ESCAPES in effect)
std::optional<std::string> escape_param(MYSQL *conn,
                                        sql_variant::Param const &param) {
  if (!param.value) {
    return "NULL";
  }
  if (param.binary) {
    static constexpr std::string_view hexd = "0123456789ABCDEF";
    std::string out = "X'";
    for (unsigned char c : *param.value) {
      out += hexd[c >> 4];
      out += hexd[c & 0xF];
    }
    out += '\'';
    return out;
  }
  std::string buf((param.value->size() * 2) + 1, '\0');
  const auto len = mysql_real_escape_string(
      conn, buf.data(), param.value->data(), param.value->size());
  if (len == std::numeric_limits<unsigned long>::max()) {
    return std::nullopt;
  }
  buf.resize(len);
  return "'" + buf + "'";
}
} // namespace

namespace sql_variant {

ErrorClass classify_mysql_errno(unsigned int errcode) {
  return (errcode == 1213 || errcode == 1205) ? ErrorClass::conflict
                                              : ErrorClass::other;
}

MySQL::MySQL(ServerParams const &params) {
  {
    // mysql_connect is not thread safe, hold a mutex

    static std::mutex mysql_init_mutex;
    std::lock_guard<std::mutex> guard(mysql_init_mutex);

    connection = mysql_init(nullptr);
  }

  if (connection == nullptr) {
    throw SqlException("mysql-init-failed", "mysql_init failed");
  }

  if (params.maxpacket != MAX_PACKET_DEFAULT) {
    mysql_options(connection, MYSQL_OPT_MAX_ALLOWED_PACKET, &params.maxpacket);
  }
  if (mysql_real_connect(connection, params.address.c_str(),
                         params.username.c_str(), params.password.c_str(),
                         params.database.c_str(), params.port,
                         params.socket.c_str(), 0) == NULL) {

    std::stringstream ss;
    logError(ss);

    mysql_close(connection);
    mysql_thread_end();
    throw SqlException("mysql-connection-failed", ss.str());
  }

  // anything past this point must not leak the live handle on throw
  try {
    serverInfo_ = calculateServerInfo();
  } catch (...) {
    mysql_close(connection);
    mysql_thread_end();
    throw;
  }
}

// mysql_thread_end cleans up TLS for the thread calling close, not the
// thread that ran the queries; Worker currently connects and closes on
// the owning thread while queries run on the worker thread.
MySQL::~MySQL() {
  if (connection != nullptr) {
    mysql_close(connection);
    mysql_thread_end();
    connection = nullptr;
  }
}

void MySQL::logError(std::ostream &ostream) const {
  ostream << mysql_errno(connection) << ": " << mysql_error(connection);
}

QueryResult MySQL::executeQuery(std::string const &query) const {
  QueryResult result;
  result.query = query;

  result.executedAt = std::chrono::high_resolution_clock::now();
  const auto qres = mysql_real_query(connection, query.c_str(), query.size());
  const auto end = std::chrono::high_resolution_clock::now();

  result.executionTime = end - result.executedAt;

  auto errCode = mysql_errno(connection);
  result.errorInfo.errorCode = std::to_string(errCode);
  result.errorInfo.errorMessage = mysql_error(connection);

  if (qres == 1) { // failure

    result.errorInfo.errorStatus = statusForError(errCode);
    result.errorInfo.errorClass =
        result.errorInfo.errorStatus == SqlStatus::serverGone
            ? ErrorClass::serverGone
            : classify_mysql_errno(errCode);
  } else {
    auto *res = mysql_store_result(connection);
    errCode = mysql_errno(connection);

    if (res == nullptr && errCode != 0) {
      // server may have died between mysql_real_query and mysql_store_result
      result.errorInfo.errorCode = std::to_string(errCode);
      result.errorInfo.errorMessage = mysql_error(connection);
      result.errorInfo.errorStatus = statusForError(errCode);
      result.errorInfo.errorClass =
          result.errorInfo.errorStatus == SqlStatus::serverGone
              ? ErrorClass::serverGone
              : classify_mysql_errno(errCode);
    } else { // success, res == nullptr here is normal for DML
      result.errorInfo.errorStatus = SqlStatus::success;
      result.data = std::make_unique<MySQLSpecificResult>(res);
      result.affectedRows = mysql_affected_rows(connection);
    }
  }

  return result;
}

QueryResult MySQL::executeParams(std::string const &query,
                                 std::vector<Param> const &params) const {
  // splice escaped literals over ? placeholders; ? inside quoted strings
  // and backtick identifiers is left alone. assumes default sql_mode
  // backslash escaping (NO_BACKSLASH_ESCAPES would break the scanner).
  // also not comment-aware: ? inside --, # or /* */ comments is treated
  // as a placeholder.
  const auto fail = [&](std::string code, std::string message) {
    QueryResult result;
    result.query = query;
    result.executionTime = std::chrono::nanoseconds{0};
    result.errorInfo.errorCode = std::move(code);
    result.errorInfo.errorMessage = std::move(message);
    result.errorInfo.errorStatus = SqlStatus::error;
    result.errorInfo.errorClass = ErrorClass::other;
    return result;
  };

  std::string expanded;
  expanded.reserve(query.size() + (32 * params.size()));
  std::size_t idx = 0;
  char quote = 0;

  for (std::size_t i = 0; i < query.size(); ++i) {
    const char c = query[i];
    if (quote != 0) {
      expanded += c;
      if (c == '\\' && quote != '`' && i + 1 < query.size()) {
        expanded += query[++i];
      } else if (c == quote) {
        quote = 0;
      }
      continue;
    }
    if (c == '\'' || c == '"' || c == '`') {
      quote = c;
      expanded += c;
      continue;
    }
    if (c == '?') {
      if (idx >= params.size()) {
        return fail("params-mismatch", "placeholder/parameter count mismatch");
      }
      auto escaped = escape_param(connection, params[idx++]);
      if (!escaped) {
        return fail("params-escape-failed",
                    "mysql_real_escape_string rejected parameter");
      }
      expanded += *escaped;
      continue;
    }
    expanded += c;
  }

  if (idx != params.size()) {
    return fail("params-mismatch", "placeholder/parameter count mismatch");
  }

  return executeQuery(expanded);
}

std::string MySQL::serverInfoString() const { return "TODO"; }

ServerInfo MySQL::calculateServerInfo() const {
  std::string versionInfo = mysql_get_server_info(connection);
  unsigned long major = 0;
  unsigned long minor = 0;
  unsigned long version = 0;

  std::size_t major_p = versionInfo.find('.');
  if (major_p != std::string::npos) {
    major = parseVersionComponent(versionInfo.substr(0, major_p));

    std::size_t minor_p = versionInfo.find('.', major_p + 1);
    if (minor_p != std::string::npos) {
      minor = parseVersionComponent(
          versionInfo.substr(major_p + 1, minor_p - major_p - 1));

      std::size_t version_p = versionInfo.find('.', minor_p + 1);
      version = parseVersionComponent(
          version_p != std::string::npos
              ? versionInfo.substr(minor_p + 1, version_p - minor_p - 1)
              : versionInfo.substr(minor_p + 1));
    }
  }
  auto server_version = (major * 10000) + (minor * 100) + version;

  flavor flav = flavor::mysql;

  const auto res = executeQuery("SHOW VARIABLES LIKE '%wsrep%';");

  if (res.success() && res.data != nullptr && res.data->numRows() > 0) {
    flav = flavor::pxc;
  } else if (versionInfo.find("-") != std::string::npos) {
    flav = flavor::ps; // PS is like X.Y.Z-U, upstream is just X.Y.Z
  }

  return {flav, server_version};
}

std::string MySQL::hostInfo() const { return mysql_get_host_info(connection); }

void MySQL::library_end() { mysql_library_end(); }

} // namespace sql_variant
