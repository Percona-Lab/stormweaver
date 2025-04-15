
#include "process/process.hpp"

namespace process {
BackgroundProcessChain::BackgroundProcessChain(
    std::shared_ptr<spdlog::logger> logger)
    : logger(logger) {}

void BackgroundProcessChain::start() {
  if (coordinator.joinable() ||
      (!commands.empty() && commands[0].status != CmdStatus::pending)) {
    throw std::runtime_error("Command chain already started");
  }

  coordinator = std::thread([this] {
    logger->debug("Starting process chain");
    for (auto &cmd : commands) {
      cmd.status = CmdStatus::running;
      if ((cmd.exitCode =
               BackgroundProcess::runAndWait(logger, cmd.cmd, cmd.args)) != 0) {
        logger->warn("Process chain failed");
        cmd.status = CmdStatus::failure;
        break;
      }
      cmd.status = CmdStatus::success;
    }
    logger->debug("Process chain ending");
    //
  });
}

bool BackgroundProcessChain::wait() {
  if (coordinator.joinable()) {
    coordinator.join();
  }

  if (commands.empty() || commands[0].status == CmdStatus::pending) {
    throw std::runtime_error("Command chain wasn't started");
  }

  for (auto const &cmd : commands) {
    if (cmd.status != CmdStatus::success) {
      return false;
    }
  }

  return true;
}
} // namespace process
