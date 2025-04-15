
#include "sql_variant/mysql.hpp"

#include <mutex>
#include <mysql.h>
#include <sstream>

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
      throw sql_variant::SqlException("Not a SELECT-like statement!");
    }

    sql_variant::RowView ret;
    const auto mdata = mysql_fetch_row(res);
    const auto lengths = mysql_fetch_lengths(res);

    if (mdata == nullptr) {
      throw sql_variant::SqlException("No more rows");
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
} // namespace

namespace sql_variant {

MySQL::MySQL(ServerParams const &params) {
  {
    // mysql_connect is not thread safe, hold a mutex

    static std::mutex mysql_init_mutex;
    std::lock_guard<std::mutex> guard(mysql_init_mutex);

    connection = mysql_init(nullptr);
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
    throw SqlException(ss.str());
  }

  if (connection == nullptr) {
    throw SqlException("mysql_init failed");
  }

  serverInfo_ = calculateServerInfo();
}

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

  result.executedAt = std::chrono::high_resolution_clock::now();
  const auto qres = mysql_real_query(connection, query.c_str(), query.size());
  const auto end = std::chrono::high_resolution_clock::now();

  result.executionTime = end - result.executedAt;

  const auto errCode = mysql_errno(connection);
  result.errorInfo.errorCode = std::to_string(errCode);
  result.errorInfo.errorMessage = mysql_error(connection);

  if (qres == 1) { // failure

    result.errorInfo.errorStatus =
        (errCode == CR_SERVER_GONE_ERROR || errCode == CR_SERVER_LOST)
            ? SqlStatus::serverGone
            : SqlStatus::error;
  } else { // success
    result.errorInfo.errorStatus = SqlStatus::success;

    result.data =
        std::make_unique<MySQLSpecificResult>(mysql_store_result(connection));
    result.affectedRows = mysql_affected_rows(connection);
  }

  return result;
}

std::string MySQL::serverInfoString() const {
  /*std::string versionInfo = mysql_get_server_info(connection);

  auto extendedInfo = querySingleValue("select @@version_comment limit 1");

  if (extendedInfo) {
    versionInfo += " ";
    versionInfo += extendedInfo.value();
  }
*/
  return "TODO";
}

ServerInfo MySQL::calculateServerInfo() const {
  std::string versionInfo = mysql_get_server_info(connection);
  unsigned long major = 0, minor = 0, version = 0;
  std::size_t major_p = versionInfo.find(".");
  if (major_p != std::string::npos)
    major = stoi(versionInfo.substr(0, major_p));

  std::size_t minor_p = versionInfo.find(".", major_p + 1);
  if (minor_p != std::string::npos)
    minor = stoi(versionInfo.substr(major_p + 1, minor_p - major_p));

  std::size_t version_p = versionInfo.find(".", minor_p + 1);
  if (version_p != std::string::npos)
    version = stoi(versionInfo.substr(minor_p + 1, version_p - minor_p));
  else
    version = stoi(versionInfo.substr(minor_p + 1));
  auto server_version = major * 10000 + minor * 100 + version;

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
