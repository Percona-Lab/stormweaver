
#include "workload.hpp"

#include <chrono>
#include <spdlog/sinks/basic_file_sink.h>

#include "action/action_registry.hpp"
#include "sql_variant/generic.hpp"
#include "sql_variant/postgresql.hpp"

Worker::Worker(std::string const &name, sql_connector_t const &sql_connector,
               WorkloadParams const &config, metadata_ptr metadata)
    : name(name), sql_connector(sql_connector), sql_conn(sql_connector()),
      config(config), metadata(metadata),
      logger(spdlog::get(fmt::format("worker-{}", name)) != nullptr
                 ? spdlog::get(fmt::format("worker-{}", name))
                 : spdlog::basic_logger_st(
                       fmt::format("worker-{}", name),
                       fmt::format("logs/worker-{}.log", name))) {}

Worker::~Worker() {}

void Worker::reconnect() { sql_conn = sql_connector(); }

void Worker::create_random_tables(std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    action::CreateTable creator(config.actionConfig.ddl,
                                metadata::Table::Type::normal);
    creator.execute(*metadata.get(), rand, sql_conn.get());
  }
}

void Worker::generate_initial_data() {
  for (std::size_t idx = 0; idx < metadata->size(); ++idx) {
    auto table = (*metadata)[idx];
    if (table) {
      for (std::size_t i = 0; i < 10; ++i) {
        action::InsertData inserter(config.actionConfig.dml, table, 100);
        inserter.execute(*metadata.get(), rand, sql_conn.get());
      }
    }
  }
}

sql_variant::LoggedSQL *Worker::sql_connection() const {
  return sql_conn.get();
}

RandomWorker::RandomWorker(std::string const &name,
                           Worker::sql_connector_t const &sql_connector,
                           WorkloadParams const &config, metadata_ptr metadata,
                           action::ActionRegistry const &actions,
                           std::unique_ptr<LuaContext> luaCtx)
    : Worker(name, sql_connector, config, metadata), actions(actions),
      luaCtx(std::move(luaCtx)) {}

RandomWorker::~RandomWorker() { join(); }

void RandomWorker::run_thread(std::size_t duration_in_seconds) {
  spdlog::info("Worker {} starting, resetting statistics", name);
  successfulActions = 0;
  failedActions = 0;
  if (thread.joinable()) {
    spdlog::error("Error: thread is already running");
    return;
  }
  thread = std::thread([this, duration_in_seconds]() {
    std::size_t connectionAttempts = 0;

    std::chrono::steady_clock::time_point begin =
        std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    while (
        std::chrono::duration_cast<std::chrono::seconds>(now - begin).count() <
        static_cast<int64_t>(duration_in_seconds)) {
      const auto w = rand.random_number(std::size_t(0), actions.totalWeight());
      auto action =
          actions.lookupByWeightOffset(w).builder(config.actionConfig);
      try {
        action->execute(*metadata, rand, sql_conn.get());
        successfulActions++;
        connectionAttempts = 0;
      } catch (sql_variant::SqlException const &e) {
        failedActions++;
        logger->warn("Worker {} Action failed: {}", name, e.what());
        if (e.serverGone()) {
          connectionAttempts++;

          if (connectionAttempts <= config.max_reconnect_attempts) {

            if (connectionAttempts > 1) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }

            logger->warn("Lost connection to the server, trying to reconnect");
            reconnect();
          } else {
            logger->error("Failed to connect 5 times, stopping worker");
            break;
          }
        }
      } catch (std::exception const &e) {
        failedActions++;
        logger->warn("Worker {} Action failed: {}", name, e.what());
      }

      now = std::chrono::steady_clock::now();
    }
    spdlog::info("Worker {} exiting. Success: {}, failure: {}", name,
                 successfulActions, failedActions);
  });
}

void RandomWorker::join() {
  if (thread.joinable())
    thread.join();
  thread = std::thread();
}

action::ActionRegistry &RandomWorker::possibleActions() { return actions; }

Workload::Workload(WorkloadParams const &params, SqlFactory const &sql_factory,
                   metadata_ptr metadata, action::ActionRegistry const &actions,
                   LuaContext const &topCtx)
    : duration_in_seconds(params.duration_in_seconds),
      repeat_times(params.repeat_times), actions(actions) {

  if (repeat_times == 0)
    return;

  for (std::size_t idx = 0; idx < params.number_of_workers; ++idx) {
    auto name = fmt::format("Worker {}", idx + 1);
    auto ctx = topCtx.dup();
    auto &ref = *ctx.get();
    workers.emplace_back(
        name,
        [name, &ref, &sql_factory]() { return sql_factory.connect(name, ref); },
        params, metadata, actions, std::move(ctx));
  }
}

void Workload::run() {
  for (auto &worker : workers) {
    worker.run_thread(duration_in_seconds);
  }
}

void Workload::wait_completion() {
  for (auto &worker : workers) {
    worker.join();
  }
}

void Workload::reconnect_workers() {
  for (auto &worker : workers) {
    worker.reconnect();
  }
}

RandomWorker &Workload::worker(std::size_t idx) {
  if (idx == 0 || idx > workers.size()) {
    throw std::runtime_error(
        fmt::format("No such worker {}, maximum is {}", idx, workers.size()));
  }
  return workers[idx - 1];
}

std::size_t Workload::worker_count() const { return workers.size(); }

SqlFactory::SqlFactory(sql_variant::ServerParams const &sql_params,
                       on_connect_t connection_callback)
    : sql_params(sql_params), connection_callback(connection_callback) {}

Node::Node(SqlFactory const &sql_factory, LuaContext &topCtx)
    : sql_factory(sql_factory), metadata(new metadata::Metadata()),
      topCtx(topCtx) {}

std::unique_ptr<Worker> Node::make_worker(std::string const &name) {
  WorkloadParams wp;
  wp.actionConfig = default_config;
  return std::make_unique<Worker>(
      name, [&]() { return sql_factory.connect(name, topCtx); }, wp, metadata);
}

std::shared_ptr<Workload>
Node::init_random_workload(WorkloadParams const &params) {
  return std::make_shared<Workload>(params, sql_factory, metadata, actions,
                                    topCtx);
}

std::unique_ptr<sql_variant::LoggedSQL>
SqlFactory::connect(std::string const &connection_name,
                    LuaContext &luaCtx) const {
  auto conn = std::make_unique<sql_variant::LoggedSQL>(
      std::make_unique<sql_variant::PostgreSQL>(sql_params), connection_name);

  if (connection_callback) {
    connection_callback(luaCtx, conn.get());
  }

  return conn;
}

action::ActionRegistry &Node::possibleActions() { return actions; }

sql_variant::ServerParams const &SqlFactory::params() const {
  return sql_params;
}

sql_variant::ServerParams const &Node::sql_params() const {
  return sql_factory.params();
}
