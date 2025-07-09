
#include "workload.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <spdlog/sinks/basic_file_sink.h>
#include <sstream>

#include "action/action_registry.hpp"
#include "metadata_populator.hpp"
#include "schema_discovery.hpp"
#include "sql_variant/generic.hpp"
#include "sql_variant/postgresql.hpp"

namespace {

std::string generate_timestamp() {
  auto now = std::chrono::system_clock::now();
  auto time_t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) %
            1000;

  std::stringstream timestamp_ss;
  timestamp_ss << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
  timestamp_ss << "_" << std::setfill('0') << std::setw(3) << ms.count();
  return timestamp_ss.str();
}

void write_metadata_file(const metadata::Metadata &metadata,
                         const std::string &timestamp,
                         const std::string &suffix) {
  std::string filename =
      fmt::format("logs/metadata_{}.{}.txt", timestamp, suffix);
  std::ofstream file(filename);
  file << metadata.debug_dump();
  file.close();
}

} // anonymous namespace

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

void Worker::discover_existing_schema() {
  logger->info("Worker {} starting schema discovery from existing database",
               name);

  try {
    schema_discovery::SchemaDiscovery discovery(sql_conn.get());
    metadata_populator::MetadataPopulator populator(*metadata);

    populator.populateFromExistingDatabase(discovery);

    logger->info("Worker {} completed schema discovery, found {} tables", name,
                 metadata->size());
  } catch (const std::exception &e) {
    logger->error("Worker {} schema discovery failed: {}", name, e.what());
    throw;
  }
}

void Worker::reset_metadata() { metadata->reset(); }

bool Worker::validate_metadata() {
  try {
    metadata::Metadata original_metadata(*metadata);

    reset_metadata();
    discover_existing_schema();

    bool is_valid = (*metadata == original_metadata);

    if (!is_valid) {
      std::string timestamp = generate_timestamp();
      write_metadata_file(original_metadata, timestamp, "orig");
      write_metadata_file(*metadata, timestamp, "new");
      logger->error("Metadata validation failed - reloaded metadata differs "
                    "from original. Debug files written with timestamp {}",
                    timestamp);
    }

    return is_valid;
  } catch (const std::exception &e) {
    logger->error("Metadata validation failed with exception: {}", e.what());
    return false;
  }
}

sql_variant::LoggedSQL *Worker::sql_connection() const {
  return sql_conn.get();
}

void Worker::calculate_database_checksums(const std::string &filename) {
  DatabaseChecksum checksummer(*sql_conn, *metadata);
  checksummer.calculateAllTableChecksums();
  checksummer.writeResultsToFile(filename);
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
  stats.reset();
  stats.start();

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
      const auto &actionFactory = actions.lookupByWeightOffset(w);
      auto action = actionFactory.builder(config.actionConfig);

      stats.startAction(actionFactory.name);
      sql_conn->resetAccumulatedSqlTime();

      try {
        action->execute(*metadata, rand, sql_conn.get());
        auto sqlTime = sql_conn->getAccumulatedSqlTime();
        stats.recordSuccess(actionFactory.name, sqlTime);

      } catch (const action::ActionException &e) {
        auto sqlTime = sql_conn->getAccumulatedSqlTime();
        stats.recordActionFailure(actionFactory.name, e.getErrorName(),
                                  sqlTime);
        logger->warn("Worker {} Action failed ({}): {}", name, e.getErrorName(),
                     e.what());

      } catch (const sql_variant::SqlException &e) {
        auto sqlTime = sql_conn->getAccumulatedSqlTime();
        stats.recordSqlFailure(actionFactory.name, e.getErrorCode(), sqlTime);
        logger->warn("Worker {} SQL failed ({}): {}", name, e.getErrorCode(),
                     e.what());
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

      } catch (const std::exception &e) {
        auto sqlTime = sql_conn->getAccumulatedSqlTime();
        stats.recordOtherFailure(actionFactory.name, sqlTime);
        logger->warn("Worker {} Action failed (other): {}", name, e.what());
      }

      now = std::chrono::steady_clock::now();
    }

    stats.stop();
    spdlog::info("Worker {} exiting", name);
    spdlog::info("\n=== Worker {} Statistics ===\n{}", name, stats.report());
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
