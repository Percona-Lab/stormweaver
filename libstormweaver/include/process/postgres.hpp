
#pragma once

#include "process/process.hpp"
#include "sql_variant/generic.hpp"
#include <filesystem>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace process {

class Postgres {
public:
  using params_t = std::unordered_map<std::string, std::string>;
  using args_t = std::vector<std::string>;

  Postgres(bool initDatadir, std::string const &logname,
           std::string const &installDir, std::string const &dataDir);

  Postgres(std::string const &logname, std::string const &installDir,
           std::string const &dataDir, sql_variant::ServerParams const &from,
           args_t additionalParams);

  ~Postgres();

  void add_config(params_t additionalConfig);
  void add_config(std::string_view name, std::string_view value);

  void add_hba(std::string const &host, std::string const &database,
               std::string const &user, std::string const &address,
               std::string const &method);

  bool start(std::string const &wrapper, args_t wrapperArgs);

  bool restart(std::size_t graceful_wait_period, std::string const &wrapper,
               args_t wrapperArgs);

  void stop(std::size_t graceful_wait_period);

  void kill9();

  bool createdb(std::string const &name);

  bool createuser(std::string const &name, args_t args);

  bool dropdb(std::string const &name);

  bool is_running();

  bool is_ready();

  bool wait_ready(std::size_t maxWaitTime);

  std::string serverPort() const;

private:
  std::filesystem::path installDir;
  std::filesystem::path dataDir;
  std::string port;

  std::shared_ptr<spdlog::logger> logger;

  BackgroundProcess::ptr_t postmaster = nullptr;
};

} // namespace process
