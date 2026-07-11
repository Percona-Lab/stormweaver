#include "logging.hpp"

#include <algorithm>
#include <array>
#include <mutex>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <vector>

namespace {

std::filesystem::path &log_dir() {
  static std::filesystem::path dir = "logs";
  return dir;
}

// python-style uppercase level names, WARNING not WARN
class upper_level_flag : public spdlog::custom_flag_formatter {
public:
  void format(spdlog::details::log_msg const &msg, std::tm const & /*tm_time*/,
              spdlog::memory_buf_t &dest) override {
    static constexpr std::array<std::string_view, 7> names = {
        "TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL", "OFF"};
    auto const name = names[static_cast<std::size_t>(msg.level)];
    dest.append(name.data(), name.data() + name.size());
  }

  [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override {
    return spdlog::details::make_unique<upper_level_flag>();
  }
};

std::unique_ptr<spdlog::pattern_formatter> file_formatter() {
  auto formatter = std::make_unique<spdlog::pattern_formatter>();
  formatter->add_flag<upper_level_flag>('*');
  // matches python's "%(asctime)s [%(levelname)s] %(name)s: %(message)s"
  formatter->set_pattern("%Y-%m-%d %H:%M:%S,%e [%*] %n: %v");
  return formatter;
}

bool &unified_flag() {
  static bool v = false;
  return v;
}

bool &splits_flag() {
  static bool v = false;
  return v;
}

// swappable callback: shutdown must be able to drop the python callable
// while named unified loggers still hold this sink in the spdlog registry
class python_sink : public spdlog::sinks::base_sink<std::mutex> {
public:
  using callback_t =
      std::function<void(int, std::string const &, std::string const &)>;

  void set_callback(callback_t cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(cb);
  }

protected:
  void sink_it_(spdlog::details::log_msg const &msg) override {
    if (!callback_) {
      return;
    }
    callback_(static_cast<int>(msg.level),
              std::string(msg.logger_name.data(), msg.logger_name.size()),
              std::string(msg.payload.data(), msg.payload.size()));
  }
  void flush_() override {}

private:
  callback_t callback_;
};

std::shared_ptr<python_sink> &python_sink_instance() {
  static auto sink = std::make_shared<python_sink>();
  return sink;
}

} // namespace

namespace logging {

void set_log_dir(std::filesystem::path dir) { log_dir() = std::move(dir); }

std::string log_path(std::string const &filename) {
  return (log_dir() / filename).string();
}

void set_unified(bool unified, bool splits) {
  unified_flag() = unified;
  splits_flag() = splits;
}

std::shared_ptr<spdlog::logger> make_file_logger(std::string const &name,
                                                 std::string const &filename) {
  if (auto existing = spdlog::get(name)) {
    return existing;
  }
  if (unified_flag()) {
    std::vector<spdlog::sink_ptr> sinks{python_sink_instance()};
    if (splits_flag()) {
      auto file = std::make_shared<spdlog::sinks::basic_file_sink_st>(
          log_path(filename));
      file->set_formatter(file_formatter());
      sinks.push_back(std::move(file));
    }
    auto logger =
        std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
    spdlog::register_logger(logger);
    return logger;
  }
  auto logger = spdlog::basic_logger_st(name, log_path(filename));
  logger->set_formatter(file_formatter());
  return logger;
}

void set_python_sink(
    std::function<void(int, std::string const &, std::string const &)> sink) {
  python_sink_instance()->set_callback(std::move(sink));
  auto logger =
      std::make_shared<spdlog::logger>("core", python_sink_instance());
  logger->set_level(spdlog::default_logger()->level());
  spdlog::set_default_logger(logger);
}

void set_level(int spdlog_level) {
  auto const clamped =
      std::clamp(spdlog_level, static_cast<int>(spdlog::level::trace),
                 static_cast<int>(spdlog::level::off));
  spdlog::default_logger()->set_level(
      static_cast<spdlog::level::level_enum>(clamped));
}

} // namespace logging
