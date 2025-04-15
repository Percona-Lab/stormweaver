
#pragma once

#include <boost/process.hpp>
#include <boost/process/v1/io.hpp>
#include <memory>
#include <ranges>
#include <signal.h>
#include <spdlog/spdlog.h>

namespace process {

// TODO: use ASIO!
struct BackgroundProcess
    : public std::enable_shared_from_this<BackgroundProcess> {

  using ptr_t = std::shared_ptr<BackgroundProcess>;

  boost::process::ipstream outerr;
  boost::process::child child;
  std::string commandLine;
  std::jthread loggerThd;

private:
  BackgroundProcess(std::string const &cmd,
                    std::ranges::input_range auto &&args)
      : outerr(),
        child(cmd, boost::process::args(args), boost::process::std_in.close(),
              (boost::process::std_out & boost::process::std_err) > outerr),
        commandLine(cmd + " " + boost::algorithm::join(args, " ")) {
    spdlog::info("Running {}", commandLine);
  }

  void setupLogger(std::shared_ptr<spdlog::logger> logger) {
    loggerThd = std::jthread([&, logger]() {
      auto self_ptr = shared_from_this();

      logger->info("Executing command {}", commandLine);
      std::string line;

      while (child.running() && std::getline(outerr, line) && !line.empty()) {
        logger->info(">> {}", line);
      }
    });

    loggerThd.detach();
  }

public:
  int waitUntilExits() {

    try {
      // TODO: how to avoid the no child exception if a process exists quickly
      if (child.running())
        child.wait();
    } catch (...) {
    }

    return child.exit_code();
  }

  void kill(int signal) { ::kill(child.id(), signal); }

  bool running() { return child.running(); }

  // TODO: should be string_view, but boost join doesn't support it and ranges
  // join_with is missing from libc++
  template <std::ranges::input_range Range = std::initializer_list<std::string>>
  static ptr_t run(std::shared_ptr<spdlog::logger> logger,
                   std::string const &cmd, Range &&args) {
    std::shared_ptr<BackgroundProcess> ptr(new BackgroundProcess(cmd, args));
    ptr->setupLogger(logger);

    return ptr;
  }

  // TODO: should be string_view, but boost join doesn't support it and ranges
  // join_with is missing from libc++
  template <std::ranges::input_range Range = std::initializer_list<std::string>>
  static int runAndWait(std::shared_ptr<spdlog::logger> logger,
                        std::string const &cmd, Range &&args) {
    return run(logger, cmd, args)->waitUntilExits();
  }
};

class BackgroundProcessChain {
private:
  enum class CmdStatus { pending, running, success, failure };

  struct CommandInfo {
    std::string cmd;
    std::vector<std::string> args;
    CmdStatus status = CmdStatus::pending;
    int exitCode = 0;
  };

public:
  BackgroundProcessChain(std::shared_ptr<spdlog::logger> logger);

  BackgroundProcessChain(BackgroundProcessChain &&) = default;

  void start();
  bool wait();

  ~BackgroundProcessChain() { wait(); }

  template <std::ranges::input_range Range = std::initializer_list<std::string>>
  void addCommand(std::string const &cmd, Range &&args) {
    commands.push_back(CommandInfo{cmd, {args.begin(), args.end()}});
  }

private:
  std::shared_ptr<spdlog::logger> logger;
  std::vector<CommandInfo> commands;
  std::thread coordinator;
};

class BackgroundProcessGroup {
public:
  BackgroundProcessChain &createChain(std::shared_ptr<spdlog::logger> logger) {
    chains.emplace_back(BackgroundProcessChain{logger});
    return chains.back();
  }

private:
  std::vector<BackgroundProcessChain> chains;
};

}; // namespace process
