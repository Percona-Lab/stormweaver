
#pragma once

#include <cstdint>
#include <thread>

#include "action/action_registry.hpp"
#include "checksum.hpp"
#include "metadata.hpp"
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
  // 0 = entropy seed (default, current behavior), nonzero = deterministic
  // per-worker stream derived from (seed, worker name)
  std::uint64_t seed = 0;
};

class Worker {
public:
  using sql_connector_t =
      std::function<std::unique_ptr<sql_variant::LoggedSQL>()>;

  Worker(std::string const &name, sql_connector_t const &sql_connector,
         WorkloadParams config, metadata_ptr metadata);

  Worker(Worker &&) = default;

  virtual ~Worker();

  void create_random_tables(std::size_t count);

  void discover_existing_schema();

  void reset_metadata();

  bool validate_metadata();

  [[nodiscard]] sql_variant::LoggedSQL *sql_connection() const;

  void calculate_database_checksums(const std::string &filename);

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
               action::ActionRegistry actions);

  RandomWorker(RandomWorker &&) = default;

  ~RandomWorker() override;

  void run_thread(std::size_t duration_in_seconds);

  void join();

  action::ActionRegistry &possibleActions();

  const statistics::WorkerStatistics &statistics() const;

protected:
  action::ActionRegistry actions;
  std::thread thread;
  statistics::WorkerStatistics stats;
};

class Workload {
public:
  Workload(WorkloadParams const &params,
           Worker::sql_connector_t const &sql_connector,
           const metadata_ptr &metadata, action::ActionRegistry const &actions);

  void run();

  void wait_completion();

  RandomWorker &worker(std::size_t idx);

  std::size_t worker_count() const;

  void reconnect_workers();

private:
  std::size_t duration_in_seconds;
  std::size_t repeat_times;
  std::vector<RandomWorker> workers;
  action::ActionRegistry actions;
};
