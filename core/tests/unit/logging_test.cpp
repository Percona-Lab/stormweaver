#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <regex>

#include "logging.hpp"

namespace {

struct log_dir_guard {
  explicit log_dir_guard(std::filesystem::path dir) {
    logging::set_log_dir(std::move(dir));
  }
  ~log_dir_guard() { logging::set_log_dir("logs"); }
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
