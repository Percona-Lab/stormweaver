#include "logging.hpp"

#include <algorithm>
#include <array>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/callback_sink.h>

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

} // namespace

namespace logging {

void set_log_dir(std::filesystem::path dir) { log_dir() = std::move(dir); }

std::string log_path(std::string const &filename) {
  return (log_dir() / filename).string();
}

std::shared_ptr<spdlog::logger> make_file_logger(std::string const &name,
                                                 std::string const &filename) {
  if (auto existing = spdlog::get(name)) {
    return existing;
  }
  auto logger = spdlog::basic_logger_st(name, log_path(filename));
  logger->set_formatter(file_formatter());
  return logger;
}

void set_python_sink(std::function<void(int, std::string const &)> sink) {
  auto cb = std::make_shared<spdlog::sinks::callback_sink_mt>(
      [sink = std::move(sink)](spdlog::details::log_msg const &msg) {
        sink(static_cast<int>(msg.level),
             std::string(msg.payload.data(), msg.payload.size()));
      });
  auto logger = std::make_shared<spdlog::logger>("core", std::move(cb));
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
