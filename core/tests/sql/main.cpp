#include "spdlog/sinks/stdout_color_sinks.h"
#include "sql.hpp"
#include <catch2/catch_session.hpp>
#include <filesystem>
#include <spdlog/spdlog.h>

#include "pg_launcher.hpp"
#include "sql_variant/generic.hpp"
#include "sql_variant/postgresql.hpp"

std::unique_ptr<sql_variant::LoggedSQL> sqlConnection;

using namespace std::literals;

int main(int argc, char **argv) {

  spdlog::set_level(spdlog::level::debug);
  auto err_logger = spdlog::stderr_color_mt("stderr");
  spdlog::set_default_logger(err_logger);

  if (argc < 4) {
    spdlog::error(
        "Usage: {} <postgresInstallDir> <dataDir> <port> ... catch2 args ...",
        argv[0]);
    return 1;
  }

  const std::string postgresDir = argv[1];
  const std::string dataDir = argv[2];
  const std::string postgresPort = argv[3];

  if (std::filesystem::is_directory(dataDir)) {
    spdlog::warn("Data directory '{}' already exist, deleting.", dataDir);
    std::filesystem::remove_all(dataDir);
  }

  testutil::PgLauncher pg(postgresDir, dataDir);
  if (!pg.init()) {
    spdlog::error("Couldn't initialize postgres data directory, exiting");
    return 2;
  }
  std::filesystem::create_directory(dataDir + "/sock");
  pg.add_config("port", postgresPort);
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

  sqlConnection = std::make_unique<sql_variant::LoggedSQL>(
      std::make_unique<sql_variant::PostgreSQL>(sql_variant::ServerParams{
          "sql_tests", "127.0.0.1", "", "stormweaver", "",
          static_cast<std::uint16_t>(std::stoi(postgresPort))}),
      "test-stormweaver-sql-sqllog");

  std::vector<char *> remaining_args;

  remaining_args.push_back(argv[0]);
  if (argc > 4) {
    remaining_args.insert(remaining_args.end(), argv + 4, argv + argc);
  }

  return Catch::Session().run(remaining_args.size(), remaining_args.data());
}
