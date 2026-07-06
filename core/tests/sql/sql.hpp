
#include "sql_variant/generic.hpp"
#include <memory>
#include <string>
#include <vector>

extern std::unique_ptr<sql_variant::LoggedSQL> sqlConnection;

// params used to build sqlConnection, kept around so tests needing a second
// connection (e.g. Worker's own connector) can build one of the same flavor
extern sql_variant::ServerParams globalConnParams;

// shared test-only helpers, flavor-neutral setup so the same test bodies
// run against pg and mysql. do not add to core lib.
namespace testutil {

inline bool isMysql() { return sqlConnection->serverInfo().is_mysql_like(); }

// auto increment column definition fragment, flavor-specific keyword
inline std::string autoIncPk() {
  return isMysql() ? "INT AUTO_INCREMENT" : "SERIAL";
}

// binary blob column type name, flavor-specific
inline std::string blobType() { return isMysql() ? "BLOB" : "BYTEA"; }

// wipes the test database back to empty, pg drops/recreates the public
// schema, mysql just drops every table (no schema concept to nuke)
inline void resetTestSchema() {
  if (isMysql()) {
    sqlConnection->executeQuery("SET FOREIGN_KEY_CHECKS=0").maybeThrow();

    auto res = sqlConnection->executeQuery(
        "SELECT TABLE_NAME FROM information_schema.TABLES WHERE "
        "TABLE_SCHEMA = DATABASE()");
    res.maybeThrow();

    std::vector<std::string> tables;
    if (res.data) {
      for (std::size_t i = 0; i < res.data->numRows(); ++i) {
        auto row = res.data->nextRow();
        tables.emplace_back(row.rowData[0].value_or(""));
      }
    }

    for (auto const &table : tables) {
      sqlConnection->executeQuery("DROP TABLE IF EXISTS `" + table + "`")
          .maybeThrow();
    }

    sqlConnection->executeQuery("SET FOREIGN_KEY_CHECKS=1").maybeThrow();
  } else {
    sqlConnection->executeQuery("DROP SCHEMA IF EXISTS public CASCADE")
        .maybeThrow();
    sqlConnection->executeQuery("CREATE SCHEMA public").maybeThrow();
    sqlConnection->executeQuery("GRANT ALL ON SCHEMA public TO public")
        .maybeThrow();
  }
}

} // namespace testutil
