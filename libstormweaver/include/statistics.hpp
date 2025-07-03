#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

namespace statistics {

struct TimingStatistics {
  std::chrono::nanoseconds totalTime{0};
  std::chrono::nanoseconds minTime{std::chrono::nanoseconds::max()};
  std::chrono::nanoseconds maxTime{0};
  uint64_t count = 0;

  void record(std::chrono::nanoseconds duration);
  double getAverageMs() const;
  double getMinMs() const;
  double getMaxMs() const;
  void reset();
  bool hasData() const;
};

struct ActionStatistics {
  uint64_t successCount = 0;
  uint64_t actionFailureCount = 0;
  uint64_t sqlFailureCount = 0;
  uint64_t otherFailureCount = 0;

  std::map<std::string, uint64_t> actionErrorNames;
  std::map<std::string, uint64_t> sqlErrorCodes;

  TimingStatistics executionTiming;
  TimingStatistics sqlTiming;

  std::chrono::high_resolution_clock::time_point startTime{};

  void start();
  void
  recordSuccess(std::chrono::nanoseconds sqlTime = std::chrono::nanoseconds{0});
  void recordActionFailure(
      const std::string &errorName,
      std::chrono::nanoseconds sqlTime = std::chrono::nanoseconds{0});
  void recordSqlFailure(
      const std::string &errorCode,
      std::chrono::nanoseconds sqlTime = std::chrono::nanoseconds{0});
  void recordOtherFailure(
      std::chrono::nanoseconds sqlTime = std::chrono::nanoseconds{0});
  uint64_t getTotalCount() const;
  uint64_t getTotalFailureCount() const;
  double getSuccessRate() const;

  void reset();
  bool hasData() const;
};

struct WorkerStatistics {
  std::unordered_map<std::string, ActionStatistics> actionStats;
  std::chrono::steady_clock::time_point startTime;
  std::chrono::steady_clock::time_point endTime;

  void startAction(const std::string &actionName);
  void
  recordSuccess(const std::string &actionName,
                std::chrono::nanoseconds sqlTime = std::chrono::nanoseconds{0});
  void recordActionFailure(
      const std::string &actionName, const std::string &errorName,
      std::chrono::nanoseconds sqlTime = std::chrono::nanoseconds{0});
  void recordSqlFailure(
      const std::string &actionName, const std::string &errorCode,
      std::chrono::nanoseconds sqlTime = std::chrono::nanoseconds{0});
  void recordOtherFailure(
      const std::string &actionName,
      std::chrono::nanoseconds sqlTime = std::chrono::nanoseconds{0});
  void start();
  void stop();
  void reset();
  std::string report() const;
  std::string reportSummary() const;
  std::string reportDetailed() const;
  double getTotalDurationSeconds() const;
  uint64_t getTotalActionCount() const;
  uint64_t getTotalSuccessCount() const;
  uint64_t getTotalFailureCount() const;
  double getOverallSuccessRate() const;
  double getActionsPerSecond() const;

  bool hasData() const;
};

} // namespace statistics
