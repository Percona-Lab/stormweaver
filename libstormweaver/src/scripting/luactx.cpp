
#include "scripting/luactx.hpp"

#include "action/action_registry.hpp"
#include "process/postgres.hpp"
#include "process/process.hpp"
#include "workload.hpp"
#include <boost/algorithm/string/replace.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <spdlog/sinks/basic_file_sink.h>

inline std::unique_ptr<Node> setup_node_pg(LuaContext &parentCtx,
                                           sol::table const &table) {
  const std::string host = table.get_or("host", std::string("localhost"));
  const std::uint16_t port = table.get_or("port", 5432);
  const std::string user = table.get_or("user", std::string("postgres"));
  const std::string password = table.get_or("password", std::string(""));
  const std::string database =
      table.get_or("database", std::string("stormweaver"));
  // TODO: generalize and document proper multithreaded callback creation
  auto on_connect_lua = table.get<sol::function>("on_connect");

  SqlFactory::on_connect_t callback;

  if (on_connect_lua.valid()) {
    const auto bytecode = on_connect_lua.dump();
    callback = SqlFactory::on_connect_t(bytecode);
  }

  spdlog::info("Setting up PG node on host: '{}', port: {}", host, port);

  return std::make_unique<Node>(
      SqlFactory(
          sql_variant::ServerParams{database, host, "", user, password, port},
          callback),
      parentCtx);
}

inline void node_init(Node &self, sol::protected_function init_callback) {
  auto worker = self.make_worker("Initialization");

  sol::protected_function_result result = init_callback(worker.get());

  if (!result.valid()) {
    sol::error err = result;
    spdlog::error("Node initialization lua callback failed: {}", err.what());
    throw std::runtime_error("node_init failed");
  }
}

inline auto init_random_workload(Node &self, WorkloadParams wp) {
  return self.init_random_workload(wp);
}

extern "C" {
LUALIB_API int luaopen_toml(lua_State *L);
LUALIB_API int luaopen_lfs(lua_State *L);
}

struct Fs {};

LuaContext::LuaContext(std::shared_ptr<spdlog::logger> logger)
    : logger(logger) {
  luaState.open_libraries();
  luaState.require("toml", luaopen_toml);
  luaState.require("lfs", luaopen_lfs);

  const std::string original_package_path = luaState["package"]["path"];
  const std::string base_dir =
      boost::dll::program_location().parent_path().parent_path().string();
  luaState["package"]["path"] =
      original_package_path + (!original_package_path.empty() ? ";" : "") +
      fmt::format("{}/scripts/?.lua;{}/scripts_3p/?.lua", base_dir, base_dir);

  luaState["sleep"] = [](std::size_t milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
  };
  luaState["defaultActionRegistry"] = []() {
    return &action::default_registy();
  };

  auto rowview_usertype = luaState.new_usertype<sql_variant::RowView>(
      "RowView", sol::no_constructor);
  rowview_usertype["field"] = [](sql_variant::RowView &self, std::size_t idx) {
    return self.rowData[idx - 1];
  };
  rowview_usertype["numFields"] = [](sql_variant::RowView &self) {
    return self.rowData.size();
  };

  auto queryspecificresult_usertype =
      luaState.new_usertype<sql_variant::QuerySpecificResult>(
          "QuerySpecificResult", sol::no_constructor);

  queryspecificresult_usertype["numField"] =
      &sql_variant::QuerySpecificResult::numFields;
  queryspecificresult_usertype["numRows"] =
      &sql_variant::QuerySpecificResult::numRows;
  queryspecificresult_usertype["nextRow"] =
      &sql_variant::QuerySpecificResult::nextRow;

  auto queryresult_usertype = luaState.new_usertype<sql_variant::QueryResult>(
      "QueryResult", sol::no_constructor);

  queryresult_usertype["sucess"] = &sql_variant::QueryResult::success;
  queryresult_usertype["data"] = [](sql_variant::QueryResult &self) {
    return self.data.get();
  };
  queryresult_usertype["query"] = &sql_variant::QueryResult::query;

  auto sql_usertype =
      luaState.new_usertype<sql_variant::LoggedSQL>("SQL", sol::no_constructor);
  sql_usertype["execute_query"] = &sql_variant::LoggedSQL::executeQuery;

  auto node_usertype = luaState.new_usertype<Node>("Node", sol::no_constructor);
  node_usertype["init"] = &node_init;
  node_usertype["initRandomWorkload"] = &init_random_workload;
  node_usertype["make_worker"] = [](Node &self, std::string const &logName) {
    return self.make_worker(logName);
  };
  node_usertype["possibleActions"] = &Node::possibleActions;

  auto worker_usertype =
      luaState.new_usertype<Worker>("Worker", sol::no_constructor);
  worker_usertype["create_random_tables"] = &Worker::create_random_tables;
  worker_usertype["sql_connection"] = &Worker::sql_connection;

  luaState.new_usertype<RandomWorker>(
      "Worker", sol::no_constructor, "create_random_tables",
      &RandomWorker::create_random_tables, "possibleActions",
      &RandomWorker::possibleActions);

  auto workload_usertype =
      luaState.new_usertype<Workload>("Workload", sol::no_constructor);
  workload_usertype["run"] = &Workload::run;
  workload_usertype["wait_completion"] = &Workload::wait_completion;
  workload_usertype["worker"] = &Workload::worker;
  workload_usertype["worker_count"] = &Workload::worker_count;
  workload_usertype["reconnect_workers"] = &Workload::reconnect_workers;

  auto action_factory_usertype = luaState.new_usertype<action::ActionFactory>(
      "ActionFactory", sol::no_constructor);
  action_factory_usertype["weight"] = sol::property(
      [](action::ActionFactory &self) { return self.weight; },
      [](action::ActionFactory &self, std::size_t v) { self.weight = v; });

  auto action_registry_usertype = luaState.new_usertype<action::ActionRegistry>(
      "ActionRegistry", sol::no_constructor);
  action_registry_usertype["remove"] = &action::ActionRegistry::remove;
  action_registry_usertype["insert"] = &action::ActionRegistry::insert;
  action_registry_usertype["has"] = &action::ActionRegistry::has;
  action_registry_usertype["makeCustomSqlAction"] =
      &action::ActionRegistry::makeCustomSqlAction;
  action_registry_usertype["makeCustomTableSqlAction"] =
      &action::ActionRegistry::makeCustomTableSqlAction;
  action_registry_usertype["get"] = &action::ActionRegistry::getReference;
  action_registry_usertype["use"] = &action::ActionRegistry::use;

  auto postgres_usertype =
      luaState.new_usertype<process::Postgres>("Postgres", sol::no_constructor);
  postgres_usertype["start"] =
      sol::overload(&process::Postgres::start,
                    [](process::Postgres &self) { return self.start("", {}); });
  postgres_usertype["stop"] = &process::Postgres::stop;
  postgres_usertype["restart"] =
      sol::overload(&process::Postgres::restart,
                    [](process::Postgres &self, std::size_t wait_period) {
                      return self.restart(wait_period, "", {});
                    });
  postgres_usertype["kill9"] = &process::Postgres::kill9;
  postgres_usertype["createdb"] = &process::Postgres::createdb;
  postgres_usertype["dropdb"] = &process::Postgres::dropdb;
  postgres_usertype["createuser"] = &process::Postgres::createuser;
  postgres_usertype["is_running"] = &process::Postgres::is_running;
  postgres_usertype["serverPort"] = &process::Postgres::serverPort;
  postgres_usertype["is_ready"] = &process::Postgres::is_ready;
  postgres_usertype["wait_ready"] = &process::Postgres::wait_ready;
  postgres_usertype["add_config"] =
      static_cast<void (process::Postgres::*)(process::Postgres::params_t)>(
          &process::Postgres::add_config);
  postgres_usertype["add_hba"] = &process::Postgres::add_hba;

  // TODO: add a constructor that takes a lua hash and initializes parameters
  auto workload_params_usertype = luaState.new_usertype<WorkloadParams>(
      "WorkloadParams", sol::constructors<WorkloadParams()>());

  workload_params_usertype["duration_in_seconds"] =
      &WorkloadParams::duration_in_seconds;
  workload_params_usertype["repeat_times"] = &WorkloadParams::repeat_times;
  workload_params_usertype["number_of_workers"] =
      &WorkloadParams::number_of_workers;
  workload_params_usertype["max_reconnect_attempts"] =
      &WorkloadParams::max_reconnect_attempts;

  auto bg_t = luaState.new_usertype<BackgroundThread>("BackgroundThread",
                                                      sol::no_constructor);
  bg_t["run"] = [this](std::string const &logName, std::string const &fname) {
    return std::make_unique<BackgroundThread>(
        *this,
        spdlog::basic_logger_st(fmt::format("bg-{}", logName),
                                fmt::format("logs/bg-{}.log", logName)),
        this->dump(fname));
  };

  bg_t["join"] = &BackgroundThread::join;
  bg_t["send"] = [](BackgroundThread &self, std::string msg) {
    self.toQueue().send(msg);
  };
  bg_t["receive"] = [](BackgroundThread &self) { self.fromQueue().receive(); };
  bg_t["receiveIfAny"] = [](BackgroundThread &self) {
    self.fromQueue().receiveIfAny();
  };

  auto bgq_t =
      luaState.new_usertype<CommQueue>("CommQueue", sol::no_constructor);
  bgq_t["send"] = &CommQueue::send;
  bgq_t["receive"] = &CommQueue::receive;
  bgq_t["receiveIfAny"] = &CommQueue::receiveIfAny;

  luaState["initPostgresDatadir"] = [](std::string const &installDir,
                                       std::string const &dataDir) {
    return std::make_unique<process::Postgres>(
        true, boost::replace_all_copy(dataDir, "/", "-"), installDir, dataDir);
  };

  luaState["initBasebackupFrom"] = [](std::string const &installDir,
                                      std::string const &dataDir,
                                      Node const &node, sol::variadic_args va) {
    return std::make_unique<process::Postgres>(
        boost::replace_all_copy(dataDir, "/", "-"), installDir, dataDir,
        node.sql_params(), std::vector<std::string>(va.begin(), va.end()));
  };

  luaState["debug"] = [logger](std::string const &str) { logger->debug(str); };
  luaState["info"] = [logger](std::string const &str) { logger->info(str); };
  luaState["warning"] = [logger](std::string const &str) { logger->warn(str); };
  // TODO: handle this in a way that doesn't override the default lua error
  // handler
  // luaState["error"] = [logger](std::string const &str) { logger->error(str);
  // };

  luaState["getenv"] = [](std::string const &name,
                          std::string const &default_value) -> std::string {
    auto env = getenv(name.c_str());
    if (env != nullptr && strlen(env) > 0)
      return env;
    return default_value;
  };

  luaState["setup_node_pg"] = [this](sol::table const &table) {
    return setup_node_pg(*this, table);
  };

  auto fs_usertype = luaState.new_usertype<Fs>("fs", sol::no_constructor);
  fs_usertype["is_directory"] = [](std::string const &path) {
    return std::filesystem::is_directory(path);
  };
  fs_usertype["copy_directory"] = [](std::string const &from,
                                     std::string const &to) {
    return std::filesystem::copy(from, to,
                                 std::filesystem::copy_options::recursive);
  };
  fs_usertype["delete_directory"] = [](std::string const &dir) {
    return std::filesystem::remove_all(dir);
  };
  fs_usertype["create_directory"] = [](std::string const &path) {
    return std::filesystem::create_directories(path);
  };
}

sol::state &LuaContext::ctx() { return luaState; }

sol::bytecode LuaContext::dump(std::string const &name) const {
  sol::function target = luaState[name];
  return target.dump();
}

bool LuaContext::loadScript(std::filesystem::path file) {
  auto script = luaState.load_file(file);

  if (!script.valid()) {
    sol::error err = script;
    spdlog::error("Loading script '{}' failed: {}", file.c_str(), err.what());
    return false;
  }

  sol::protected_function_result result = script();

  if (!result.valid()) {
    sol::error err = result;
    spdlog::error("Running script '{}' failed: {}", file.c_str(), err.what());
    return false;
  }

  loadedFiles.push_back(file);

  return true;
}

std::unique_ptr<LuaContext> LuaContext::dup() const {
  std::unique_ptr<LuaContext> newCtx = std::make_unique<LuaContext>(logger);
  for (auto const &f : loadedFiles) {
    if (!newCtx->loadScript(f)) {
      throw std::runtime_error("Failed to process for dup");
    }
  }
  return newCtx;
}

std::unique_ptr<LuaContext>
LuaContext::dup(std::shared_ptr<spdlog::logger> newLogger) const {
  std::unique_ptr<LuaContext> newCtx = std::make_unique<LuaContext>(newLogger);
  for (auto const &f : loadedFiles) {
    if (!newCtx->loadScript(f)) {
      throw std::runtime_error("Failed to process for dup");
    }
  }
  return newCtx;
}

BackgroundThread::BackgroundThread(LuaContext const &originalCtx,
                                   std::shared_ptr<spdlog::logger> newLogger,
                                   sol::bytecode const &func)
    : luaCtx(originalCtx.dup(newLogger)), thd([this, func]() {
        luaCtx->addFunction("receive", [&]() { return toThread.receive(); });
        luaCtx->addFunction("receiveIfAny",
                            [&]() { return toThread.receiveIfAny(); });
        luaCtx->run(func);
      }) {}

BackgroundThread::~BackgroundThread() { join(); }

CommQueue &BackgroundThread::toQueue() { return toThread; }
CommQueue &BackgroundThread::fromQueue() { return fromThread; }

void BackgroundThread::join() {
  if (thd.joinable())
    thd.join();
}

void CommQueue::send(message_t const &message) {
  {
    std::unique_lock<std::mutex> lk(queueMtx);
    messages.push(message);
  }
  queueCv.notify_all();
}

std::optional<CommQueue::message_t> CommQueue::receiveIfAny() {
  std::unique_lock<std::mutex> lk(queueMtx);

  if (messages.empty()) {
    return std::nullopt;
  }

  const auto v = messages.front();
  messages.pop();

  return v;
}

CommQueue::message_t CommQueue::receive() {
  std::unique_lock<std::mutex> lk(queueMtx);
  queueCv.wait(lk, [&] { return !messages.empty(); });

  const auto v = messages.front();
  messages.pop();

  return v;
}
