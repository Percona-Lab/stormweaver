
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
  explicit LuaContext(std::shared_ptr<spdlog::logger> logger);

  sol::state &ctx();

  sol::bytecode dump(std::string const &name) const;

  template<typename... Args>
  auto run(sol::bytecode const &bytecode, Args&&... args) {
    sol::load_result l = luaState.load(bytecode.as_string_view());
    return l(std::forward<Args>(args)...);
  }

  bool loadScript(std::filesystem::path file);

  std::unique_ptr<LuaContext> dup() const;
  std::unique_ptr<LuaContext> dup(std::shared_ptr<spdlog::logger> newLogger) const;

  template <typename T> void addFunction(std::string const &name, T const &t) {
    luaState[name] = t;
  }

private:
  std::shared_ptr<spdlog::logger> logger;
  sol::state luaState;
  std::vector<std::filesystem::path> loadedFiles;
};

template <typename T>
class LuaCallback;
template< class R, class... Args >
class LuaCallback<R(Args...)> {
public:
  LuaCallback() : code(std::nullopt) {}
  explicit LuaCallback(sol::bytecode code) : code(code) {}

  auto operator()(LuaContext& ctx, Args&&... args) const {
    return ctx.run(*code, std::forward<Args>(args)...);
  }

  explicit operator bool() const {
    return code.has_value();
  }
  
private:
  std::optional<sol::bytecode> code;
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
  std::unique_ptr<LuaContext> luaCtx;
  std::thread thd;
  CommQueue toThread;
  CommQueue fromThread;
};