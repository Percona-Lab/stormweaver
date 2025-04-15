
#pragma once

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <queue>
#include <sol/sol.hpp>
#include <spdlog/spdlog.h>
#include <thread>
#include <vector>

class LuaContext {
public:
  LuaContext(std::shared_ptr<spdlog::logger> logger);

  sol::state &ctx();

  sol::bytecode dump(std::string const &name) const;

  bool run(sol::bytecode const &bytecode);

  bool loadScript(std::filesystem::path file);

  LuaContext dup(std::shared_ptr<spdlog::logger> newLogger) const;

  template <typename T> void addFunction(std::string const &name, T const &t) {
    luaState[name] = t;
  }

private:
  std::shared_ptr<spdlog::logger> logger;
  sol::state luaState;
  std::vector<std::filesystem::path> loadedFiles;
};

class CommQueue {
public:
  using message_t = std::string; // TODO: what instead?

  void send(message_t const &message);

  std::optional<message_t> receiveIfAny();
  message_t receive(); // TODO: timeout?
private:
  std::mutex queueMtx;
  std::condition_variable queueCv;
  std::queue<message_t> messages;
};

class BackgroundThread {
public:
  BackgroundThread(LuaContext const &originalCtx,
                   std::shared_ptr<spdlog::logger> newLogger,
                   sol::bytecode const &func);
  ~BackgroundThread();

  void join();

  CommQueue &toQueue();
  CommQueue &fromQueue();

private:
  LuaContext luaCtx;
  std::thread thd;
  CommQueue toThread;
  CommQueue fromThread;
};