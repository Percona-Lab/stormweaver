#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <tuple>
#include <vector>

#include "logging.hpp"

namespace {

struct log_dir_guard {
  explicit log_dir_guard(std::filesystem::path dir) {
    logging::set_log_dir(std::move(dir));
  }
  ~log_dir_guard() { logging::set_log_dir("logs"); }
};

struct unified_guard {
  unified_guard(bool unified, bool splits) {
    logging::set_unified(unified, splits);
  }
  ~unified_guard() {
    logging::set_unified(false, false);
    // empty function, not empty-bodied lambda: sink must see falsy callback
    logging::set_python_sink(
        std::function<void(int, std::string const &, std::string const &)>{});
  }
};

} // namespace

TEST_CASE("log_path uses the configured directory", "[logging]") {
  log_dir_guard guard("some/dir");
  REQUIRE(logging::log_path("a.log") == "some/dir/a.log");
}

TEST_CASE("file loggers write the uniform format", "[logging]") {
  auto const dir = std::filesystem::temp_directory_path() / "sw-logging-test";
  std::filesystem::remove_all(dir);
  log_dir_guard guard(dir);

  auto logger = logging::make_file_logger("fmt-test", "fmt-test.log");
  logger->warn("hello");
  logger->flush();
  spdlog::drop("fmt-test");

  std::ifstream file(dir / "fmt-test.log");
  std::string line;
  REQUIRE(std::getline(file, line));
  // 2026-07-06 09:44:31,624 [WARNING] fmt-test: hello
  std::regex const pattern(
      R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2},\d{3} \[WARNING\] fmt-test: hello$)");
  REQUIRE(std::regex_match(line, pattern));
}

TEST_CASE("make_file_logger reuses registered loggers", "[logging]") {
  auto const dir = std::filesystem::temp_directory_path() / "sw-logging-test";
  log_dir_guard guard(dir);
  auto first = logging::make_file_logger("reuse-test", "reuse-test.log");
  auto second = logging::make_file_logger("reuse-test", "other.log");
  spdlog::drop("reuse-test");
  REQUIRE(first == second);
}

TEST_CASE("unified loggers forward to the python sink with their name",
          "[logging]") {
  auto const dir = std::filesystem::temp_directory_path() / "sw-logging-uni";
  std::filesystem::remove_all(dir);
  log_dir_guard guard(dir);
  // vector before guard: guard resets the callback during unwind, so the
  // capture target must outlive it
  std::vector<std::tuple<int, std::string, std::string>> received;
  unified_guard mode(true, false);

  logging::set_python_sink(
      [&](int level, std::string const &name, std::string const &msg) {
        received.emplace_back(level, name, msg);
      });

  auto logger = logging::make_file_logger("uni-test", "uni-test.log");
  logger->info("hello");
  spdlog::drop("uni-test");

  REQUIRE(received.size() == 1);
  REQUIRE(std::get<0>(received[0]) == static_cast<int>(spdlog::level::info));
  REQUIRE(std::get<1>(received[0]) == "uni-test");
  REQUIRE(std::get<2>(received[0]) == "hello");
  REQUIRE_FALSE(std::filesystem::exists(dir / "uni-test.log"));
}

TEST_CASE("unified with splits writes the file too", "[logging]") {
  auto const dir = std::filesystem::temp_directory_path() / "sw-logging-uni2";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  log_dir_guard guard(dir);
  std::vector<std::string> received;
  unified_guard mode(true, true);

  logging::set_python_sink(
      [&](int, std::string const &, std::string const &msg) {
        received.push_back(msg);
      });

  auto logger = logging::make_file_logger("uni-split-test", "uni-split.log");
  logger->info("hello");
  logger->flush();
  spdlog::drop("uni-split-test");

  REQUIRE(received == std::vector<std::string>{"hello"});
  REQUIRE(std::filesystem::exists(dir / "uni-split.log"));
}

TEST_CASE("swapping the callback away silences loggers still holding the sink",
          "[logging]") {
  auto const dir = std::filesystem::temp_directory_path() / "sw-logging-shut";
  std::filesystem::remove_all(dir);
  log_dir_guard guard(dir);
  std::vector<std::string> received;
  unified_guard mode(true, false);

  logging::set_python_sink(
      [&](int, std::string const &, std::string const &msg) {
        received.push_back(msg);
      });

  auto logger = logging::make_file_logger("shutdown-test", "shutdown-test.log");
  logger->info("before");
  REQUIRE(received == std::vector<std::string>{"before"});

  // same swap shutdown_core_logging does: empty function, callback goes falsy
  logging::set_python_sink(
      std::function<void(int, std::string const &, std::string const &)>{});
  logger->info("after");
  spdlog::drop("shutdown-test");

  REQUIRE(received == std::vector<std::string>{"before"});
}

TEST_CASE("default logger forwards through the shared sink as core",
          "[logging]") {
  std::vector<std::string> names;
  unified_guard mode(false, false);
  logging::set_python_sink([&](int, std::string const &name,
                               std::string const &) { names.push_back(name); });
  spdlog::info("boom");
  REQUIRE(names == std::vector<std::string>{"core"});
}
