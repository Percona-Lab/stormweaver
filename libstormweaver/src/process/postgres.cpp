
#include "process/postgres.hpp"

#include <boost/process.hpp>
#include <boost/process/v1/io.hpp>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

namespace process {

Postgres::Postgres(bool initDatadir, std::string const &logname,
                   std::string const &installDir, std::string const &dataDir)
    : installDir(installDir), dataDir(dataDir),
      logger(spdlog::basic_logger_st(fmt::format("pg-{}", logname),
                                     fmt::format("logs/pg-{}.log", logname))) {

  spdlog::info("Using PG install directory '{}' with datadir '{}'", installDir,
               dataDir);

  if (!std::filesystem::is_directory(this->installDir)) {
    throw std::runtime_error(fmt::format(
        "Specified install directory '{}' is not a directory.", installDir));
  }
  if (initDatadir) {
    spdlog::info("Initializing data directory '{}'", dataDir);
    if (std::filesystem::exists(this->dataDir)) {
      throw std::runtime_error(fmt::format(
          "Data directory '{}' already exists, can't initialize.", dataDir));
    }

    int result = BackgroundProcess::runAndWait(
        logger, fmt::format("{}/bin/initdb", installDir), {"-D", dataDir});

    if (result != 0) {
      throw std::runtime_error(fmt::format(
          "Initdb failed with data directory '{}' and install dir {}.", dataDir,
          installDir));
    }
  } else {
    if (!std::filesystem::is_directory(dataDir)) {
      throw std::runtime_error(fmt::format(
          "Specified data directory '{}' is not a directory.", installDir));
    }
  }
}

bool Postgres::start(std::string const &wrapper, args_t wrapperArgs) {
  spdlog::info("Starting postmaster for datadir {}", dataDir.string());
  if (postmaster != nullptr) {
    if (postmaster->running()) {
      spdlog::error("Can't start postgres with datadir {}: previous postmaster "
                    "is still running",
                    dataDir.string());
      return false;
    }
    spdlog::warn("Previous postmaster reference still exist, but the process "
                 "doesn't exists. Resetting.");
    postmaster = nullptr;
  }

  if (wrapper.empty()) {
    postmaster = BackgroundProcess::run(
        logger, fmt::format("{}/bin/postgres", installDir.string()),
        {"-D", dataDir.string()});
  } else {
    wrapperArgs.push_back(fmt::format("{}/bin/postgres", installDir.string()));
    wrapperArgs.push_back("-D");
    wrapperArgs.push_back(dataDir.string());

    postmaster = BackgroundProcess::run(logger, wrapper, wrapperArgs);
  }

  wait_ready(5);

  return postmaster->running();
}

bool Postgres::restart(std::size_t graceful_wait_period,
                       std::string const &wrapper, args_t wrapperArgs) {
  stop(graceful_wait_period);
  return start(wrapper, wrapperArgs);
}

void Postgres::stop(std::size_t graceful_wait_period) {
  spdlog::info("Stopping postmaster for datadir {}", dataDir.string());
  if (postmaster == nullptr || !postmaster->running()) {
    spdlog::error("Postmaster isn't running, nothing to stop.");
    postmaster = nullptr;
    return;
  }
  postmaster->kill(SIGINT);
  std::size_t seconds = 0;
  spdlog::debug(
      "Waiting {} seconds for the postmaster process to stop gracefully",
      graceful_wait_period);
  while (seconds < graceful_wait_period && postmaster->running()) {
    std::this_thread::sleep_for(
        std::chrono::seconds(1)); // TODO: proper wait for startup handler
    seconds++;
  }

  if (postmaster->running()) {
    spdlog::warn("Postmaster didn't stop gracefully within wait period, "
                 "sending SIGKILL");
    postmaster->kill(SIGKILL);
  }
  postmaster->waitUntilExits();
  postmaster = nullptr;
}

void Postgres::kill9() {
  spdlog::info("Killing postmaster with datadir {}", dataDir.string());
  postmaster->kill(SIGKILL);
  postmaster->waitUntilExits();
  postmaster = nullptr;
}

Postgres::Postgres(std::string const &logname, std::string const &installDir,
                   std::string const &dataDir,
                   sql_variant::ServerParams const &from,
                   args_t additionalParams)
    : installDir(installDir), dataDir(dataDir),
      logger(spdlog::basic_logger_st(fmt::format("pg-{}", logname),
                                     fmt::format("logs/pg-{}.log", logname))) {

  if (!std::filesystem::is_directory(this->installDir)) {
    throw std::runtime_error(fmt::format(
        "Specified install directory '{}' is not a directory.", installDir));
  }

  if (std::filesystem::is_directory(this->dataDir)) {
    throw std::runtime_error(fmt::format(
        "Specified data directory '{}' already exists.", installDir));
  }

  std::vector<std::string> allParams = {"-h",     from.address,
                                        "-U",     from.username,
                                        "--port", std::to_string(from.port),
                                        "-D",     dataDir};
  allParams.insert(allParams.end(), additionalParams.begin(),
                   additionalParams.end());
  int result = BackgroundProcess::runAndWait(
      logger, fmt::format("{}/bin/pg_basebackup", installDir), allParams);

  if (result != 0) {
    throw std::runtime_error("pg_basebackup failed");
  }
}

void Postgres::add_config(params_t additionalConfig) {
  std::ofstream stream(fmt::format("{}/postgresql.conf", dataDir.string()),
                       std::ios_base::app);
  for (auto const &conf : additionalConfig) {
    if (conf.first == "port") {
      port = conf.second;
    }
    stream << fmt::format("{} = {}", conf.first, conf.second) << std::endl;
  }
}

bool Postgres::is_running() {
  return postmaster != nullptr && postmaster->running();
}

bool Postgres::is_ready() {
  return BackgroundProcess::runAndWait(
             logger, fmt::format("{}/bin/pg_isready", installDir.string()),
             {"-p", port}) == 0;
}

bool Postgres::wait_ready(std::size_t maxWaitTime) {
  spdlog::info("Waiting to be ready up to {}", maxWaitTime);
  for (std::size_t idx = 0; idx < maxWaitTime; ++idx) {
    if (is_ready())
      return true;

    std::this_thread::sleep_for(
        std::chrono::seconds(1)); // TODO: proper wait for startup handler
  }
  return false;
}

void Postgres::add_hba(std::string const &host, std::string const &database,
                       std::string const &user, std::string const &address,
                       std::string const &method) {
  std::ofstream stream(fmt::format("{}/pg_hba.conf", dataDir.string()),
                       std::ios_base::app);
  stream << fmt::format("{} {} {} {} {}", host, database, user, address, method)
         << std::endl;
}

bool Postgres::createdb(std::string const &name) {
  return BackgroundProcess::runAndWait(
             logger, fmt::format("{}/bin/createdb", installDir.string()),
             {"-p", port, name}) == 0;
}

bool Postgres::createuser(std::string const &name, args_t args) {
  args.push_back("-p");
  args.push_back(port);
  args.push_back(name);
  return BackgroundProcess::runAndWait(
             logger, fmt::format("{}/bin/createuser", installDir.string()),
             args) == 0;
}

bool Postgres::dropdb(std::string const &name) {
  return BackgroundProcess::runAndWait(
             logger, fmt::format("{}/bin/dropdb", installDir.string()),
             {"-p", port, name}) == 0;
}

Postgres::~Postgres() { stop(10); }

std::string Postgres::serverPort() const { return port; }

} // namespace process
