#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>

namespace logging {

// process-wide base dir for all log files, default "logs".
// not synchronized: set it once, before any workers/loggers are created
void set_log_dir(std::filesystem::path dir);
std::string log_path(std::string const &filename);

// get-or-create file logger writing to log_path(filename) with the
// uniform format shared with the python side
std::shared_ptr<spdlog::logger> make_file_logger(std::string const &name,
                                                 std::string const &filename);

// unified mode: loggers created by make_file_logger forward to the python
// sink instead of (or, with splits, in addition to) their own file.
// not synchronized: set once, before any workers/loggers are created.
// mode is fixed once loggers exist: already-registered loggers keep their
// original sinks, re-init in a different mode does not re-route them
void set_unified(bool unified, bool splits);

// replace the python sink's output with a callback receiving
// (spdlog level number, logger name, raw message). the default logger and,
// in unified mode, all named loggers share this sink
void set_python_sink(
    std::function<void(int, std::string const &, std::string const &)> sink);

// level of the default logger only, file loggers are untouched
void set_level(int spdlog_level);

} // namespace logging
