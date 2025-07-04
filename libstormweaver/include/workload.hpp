
#pragma once

#include <thread>

#include "action/action_registry.hpp"
#include "metadata.hpp"
#include "scripting/luactx.hpp"
#include "sql_variant/generic.hpp"
#include "statistics.hpp"

using logged_sql_ptr = std::unique_ptr<sql_variant::LoggedSQL>;

using metadata_ptr = std::shared_ptr<metadata::Metadata>;

struct WorkloadParams {
  action::AllConfig actionConfig;
  std::size_t duration_in_seconds = 60;
  std::size_t repeat_times = 10;
  std::size_t number_of_workers = 5;
  std::size_t max_reconnect_attempts = 5;
};

class Worker {
public:
  using sql_connector_t =
      std::function<std::unique_ptr<sql_variant::LoggedSQL>()>;

  Worker(std::string const &name, sql_connector_t const &sql_connector,
         WorkloadParams const &config, metadata_ptr metadata);

  Worker(Worker &&) = default;

  virtual ~Worker();

  void create_random_tables(std::size_t count);

  void discover_existing_schema();

  void reset_metadata();

  bool validate_metadata();

  sql_variant::LoggedSQL *sql_connection() const;

  void reconnect();

protected:
  std::string name;
  sql_connector_t sql_connector;
  logged_sql_ptr sql_conn;
  WorkloadParams config;
  metadata_ptr metadata;
  ps_random rand;
  std::shared_ptr<spdlog::logger> logger;
};

class RandomWorker : public Worker {
public:
  RandomWorker(std::string const &name,
               Worker::sql_connector_t const &sql_connector,
               WorkloadParams const &config, metadata_ptr metadata,
               action::ActionRegistry const &actions,
               std::unique_ptr<LuaContext> luaCtx);

  RandomWorker(RandomWorker &&) = default;

  ~RandomWorker() override;

  void run_thread(std::size_t duration_in_seconds);

  void join();

  action::ActionRegistry &possibleActions();

protected:
  action::ActionRegistry actions;
  std::thread thread;
  std::unique_ptr<LuaContext> luaCtx;
  statistics::WorkerStatistics stats;
};

class SqlFactory {
public:
  using on_connect_t = LuaCallback<void(sql_variant::LoggedSQL *)>;

  SqlFactory(sql_variant::ServerParams const &sql_params,
             on_connect_t connection_callback);

  std::unique_ptr<sql_variant::LoggedSQL>
  connect(std::string const &connection_name, LuaContext &luaCtx) const;

  sql_variant::ServerParams const &params() const;

private:
  // postgres / mysql selector
  sql_variant::ServerParams sql_params;
  on_connect_t connection_callback;
};

class Workload {
public:
  Workload(WorkloadParams const &params, SqlFactory const &sql_factory,
           metadata_ptr metadata, action::ActionRegistry const &actions,
           LuaContext const &topCtx);

  void run();

  void wait_completion();

  // indexes starting from 1, as that's expected from lua
  RandomWorker &worker(std::size_t idx);

  std::size_t worker_count() const;

  void reconnect_workers();

private:
  std::size_t duration_in_seconds;
  std::size_t repeat_times;
  std::vector<RandomWorker> workers;
  action::ActionRegistry actions;
};

class Node {
public:
  Node(SqlFactory const &sql_factory, LuaContext &topCtx);

  std::shared_ptr<Workload> init_random_workload(WorkloadParams const &wp);

  std::unique_ptr<Worker> make_worker(std::string const &name);

  action::ActionRegistry &possibleActions();

  sql_variant::ServerParams const &sql_params() const;

private:
  SqlFactory sql_factory;

  action::AllConfig default_config;
  metadata_ptr metadata;
  action::ActionRegistry actions = action::default_registy();
  LuaContext &topCtx;
};
