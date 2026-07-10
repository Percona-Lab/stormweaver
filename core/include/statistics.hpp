#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

namespace statistics {

struct RowHistogram {
  // buckets: 0, 1, 2-10, 11-100, 101-1000, 1000+
  std::array<uint64_t, 6> buckets{};

  void record(uint64_t rows);
  [[nodiscard]] uint64_t total() const;
  [[nodiscard]] bool hasData() const;
  void reset();
};

struct TimingHistogram {
  // buckets: <0.1ms, 0.1-1ms, 1-10ms, 10-100ms, 100ms-1s, 1-10s, >10s
  std::array<uint64_t, 7> buckets{};

  void record(std::chrono::nanoseconds duration);
  void reset();
};

struct TimingStatistics {
  std::chrono::nanoseconds totalTime{0};
  std::chrono::nanoseconds minTime{std::chrono::nanoseconds::max()};
  std::chrono::nanoseconds maxTime{0};
  uint64_t count = 0;
  TimingHistogram histogram;

  void record(std::chrono::nanoseconds duration);
  [[nodiscard]] double getAverageMs() const;
  [[nodiscard]] double getMinMs() const;
  [[nodiscard]] double getMaxMs() const;
  void reset();
  [[nodiscard]] bool hasData() const;
};

// one record per TransactionAction execution, produced by the action,
// drained by the worker
struct TransactionOutcome {
  enum class End : uint8_t {
    committed,
    rolledBackIntentional,
    rolledBackError
  };

  // RAII producer: unset means the guard rolled back
  End end = End::rolledBackError;
  uint64_t implicitCommits = 0;
  uint64_t savepointRollbacks = 0;
  uint64_t subOk = 0;
  uint64_t subFail = 0;
};

// one record per successful query, produced by LoggedSQL, drained by the
// worker into ActionStatistics::rowHistograms
struct RowObservation {
  std::string action;
  std::string kind; // "select", "insert", "update", "delete", "dml"
  uint64_t rows = 0;
};

struct TransactionStatistics {
  uint64_t committed = 0;
  uint64_t rolledBackIntentional = 0;
  uint64_t rolledBackError = 0;
  uint64_t implicitCommits = 0;
  uint64_t savepointRollbacks = 0;
  uint64_t subActionsOk = 0;
  uint64_t subActionsFail = 0;
  RowHistogram subActionsPerTxn;

  void record(const TransactionOutcome &outcome);
  [[nodiscard]] uint64_t total() const;
  [[nodiscard]] bool hasData() const;
  void reset();
};

struct ActionStatistics {
  uint64_t successCount = 0;
  uint64_t actionFailureCount = 0;
  uint64_t sqlFailureCount = 0;
  uint64_t otherFailureCount = 0;
  uint64_t sqlConflictCount = 0;

  std::map<std::string, uint64_t> actionErrorNames;
  std::map<std::string, uint64_t> sqlErrorCodes;
  std::map<std::string, RowHistogram> rowHistograms;

  TimingStatistics executionTiming;
  TimingStatistics sqlTiming;

  // explicit {}: epoch value is the "not started" sentinel checked by
  // calculateExecutionTime; keep it visible even though the default ctor
  // already zeroes it
  // NOLINTNEXTLINE(readability-redundant-member-init)
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
  void recordConflict(
      const std::string &errorCode,
      std::chrono::nanoseconds sqlTime = std::chrono::nanoseconds{0});
  void recordRows(const std::string &kind, uint64_t rows);
  [[nodiscard]] uint64_t getTotalCount() const;
  [[nodiscard]] uint64_t getTotalFailureCount() const;
  [[nodiscard]] double getSuccessRate() const;

  void reset();
  [[nodiscard]] bool hasData() const;
};

struct WorkerStatistics {
  std::unordered_map<std::string, ActionStatistics> actionStats;
  TransactionStatistics txnStats;
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
  void recordConflict(
      const std::string &actionName, const std::string &errorCode,
      std::chrono::nanoseconds sqlTime = std::chrono::nanoseconds{0});
  void recordRows(const std::string &actionName, const std::string &kind,
                  uint64_t rows);
  void recordTransaction(const TransactionOutcome &outcome);
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
