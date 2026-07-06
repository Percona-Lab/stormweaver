#include "spdlog/sinks/stdout_color_sinks.h"
#include "sql.hpp"
#include <catch2/catch_session.hpp>
#include <filesystem>
#include <optional>
#include <spdlog/spdlog.h>

#include "mysql_launcher.hpp"
#include "pg_launcher.hpp"
#include "sql_variant/generic.hpp"
#include "sql_variant/mysql.hpp"
#include "sql_variant/postgresql.hpp"

std::unique_ptr<sql_variant::LoggedSQL> sqlConnection;
sql_variant::ServerParams globalConnParams;

using namespace std::literals;

namespace {

// launchers must stay alive for the whole test run, they shut the server
// down on destruction. caller keeps them alive in main's scope.

int run_pg(testutil::PgLauncher &pg, std::string const &dataDir,
           std::string const &port) {
  if (!pg.init()) {
    spdlog::error("Couldn't initialize postgres data directory, exiting");
    return 2;
  }
  std::filesystem::create_directory(dataDir + "/sock");
  pg.add_config("port", port);
  pg.add_config("unix_socket_directories", "sock");
  pg.add_config("logging_collector", "on");
  pg.add_config("log_statement", "all");
  pg.add_config("log_directory", "'logs'");
  pg.add_config("log_filename", "'server.log'");
  pg.add_config("log_min_messages", "'info'");

  if (!pg.start()) {
    spdlog::error("Couldn't start postgres server, exiting");
    return 2;
  }

  if (!pg.createdb("sql_tests") || !pg.createuser("stormweaver", "-s")) {
    spdlog::error("Couldn't create test db/user, exiting");
    return 2;
  }

  globalConnParams = sql_variant::ServerParams{
      "sql_tests",   "127.0.0.1", "",
      "stormweaver", "",          static_cast<std::uint16_t>(std::stoi(port))};
  sqlConnection = std::make_unique<sql_variant::LoggedSQL>(
      std::make_unique<sql_variant::PostgreSQL>(globalConnParams),
      "test-stormweaver-sql-sqllog");

  return 0;
}

int run_mysql(testutil::MysqlLauncher &mysql, std::string const &dataDir,
              std::string const &port) {
  std::filesystem::create_directories(dataDir);
  if (!mysql.init()) {
    spdlog::error("Couldn't initialize mysql data directory, exiting");
    return 2;
  }
  mysql.add_config("port", port);

  if (!mysql.start()) {
    spdlog::error("Couldn't start mysql server, exiting");
    return 2;
  }

  if (!mysql.createdb("sql_tests") || !mysql.createuser("stormweaver", "")) {
    spdlog::error("Couldn't create test db/user, exiting");
    return 2;
  }

  globalConnParams = sql_variant::ServerParams{
      "sql_tests",   "127.0.0.1", "",
      "stormweaver", "",          static_cast<std::uint16_t>(std::stoi(port))};
  sqlConnection = std::make_unique<sql_variant::LoggedSQL>(
      std::make_unique<sql_variant::MySQL>(globalConnParams),
      "test-stormweaver-sql-sqllog");

  return 0;
}

} // namespace

int main(int argc, char **argv) {

  spdlog::set_level(spdlog::level::debug);
  auto err_logger = spdlog::stderr_color_mt("stderr");
  spdlog::set_default_logger(err_logger);

  if (argc < 5) {
    spdlog::error("Usage: {} <pg|mysql> <installDir> <dataDir> <port> ... "
                  "catch2 args ...",
                  argv[0]);
    return 1;
  }

  const std::string flavor = argv[1];
  const std::string installDir = argv[2];
  const std::string dataDir = argv[3];
  const std::string port = argv[4];

  if (std::filesystem::is_directory(dataDir)) {
    spdlog::warn("Data directory '{}' already exist, deleting.", dataDir);
    std::filesystem::remove_all(dataDir);
  }

  // kept alive for the whole main, destructors shut the server down
  std::optional<testutil::PgLauncher> pg;
  std::optional<testutil::MysqlLauncher> mysql;

  int rc = 0;
  if (flavor == "pg") {
    pg.emplace(installDir, dataDir);
    rc = run_pg(*pg, dataDir, port);
  } else if (flavor == "mysql") {
    mysql.emplace(installDir, dataDir);
    rc = run_mysql(*mysql, dataDir, port);
  } else {
    spdlog::error("Unknown flavor '{}', expected pg or mysql", flavor);
    return 1;
  }

  if (rc != 0) {
    return rc;
  }

  std::vector<std::string> catch_args_storage;
  catch_args_storage.emplace_back(argv[0]);
  if (argc > 5) {
    catch_args_storage.insert(catch_args_storage.end(), argv + 5, argv + argc);
  }
  if (flavor == "mysql") {
    catch_args_storage.emplace_back("~[pg]");
  }

  std::vector<char *> remaining_args;
  remaining_args.reserve(catch_args_storage.size());
  for (auto &arg : catch_args_storage) {
    remaining_args.push_back(arg.data());
  }

  return Catch::Session().run(static_cast<int>(remaining_args.size()),
                              remaining_args.data());
}
