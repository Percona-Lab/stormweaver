#include "statistics.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace statistics {

void RowHistogram::record(uint64_t rows) {
  if (rows == 0) {
    buckets[0]++;
  } else if (rows == 1) {
    buckets[1]++;
  } else if (rows <= 10) {
    buckets[2]++;
  } else if (rows <= 100) {
    buckets[3]++;
  } else if (rows <= 1000) {
    buckets[4]++;
  } else {
    buckets[5]++;
  }
}

uint64_t RowHistogram::total() const {
  uint64_t sum = 0;
  for (auto b : buckets) {
    sum += b;
  }
  return sum;
}

bool RowHistogram::hasData() const { return total() > 0; }

void RowHistogram::reset() { buckets.fill(0); }

void TimingHistogram::record(std::chrono::nanoseconds duration) {
  const auto ns = duration.count();
  if (ns < 100'000) { // <0.1ms
    buckets[0]++;
  } else if (ns < 1'000'000) { // <1ms
    buckets[1]++;
  } else if (ns < 10'000'000) { // <10ms
    buckets[2]++;
  } else if (ns < 100'000'000) { // <100ms
    buckets[3]++;
  } else if (ns < 1'000'000'000) { // <1s
    buckets[4]++;
  } else if (ns < 10'000'000'000) { // <10s
    buckets[5]++;
  } else {
    buckets[6]++;
  }
}

void TimingHistogram::reset() { buckets.fill(0); }

void TimingStatistics::record(std::chrono::nanoseconds duration) {
  totalTime += duration;
  minTime = std::min(minTime, duration);
  maxTime = std::max(maxTime, duration);
  count++;
  histogram.record(duration);
}

double TimingStatistics::getAverageMs() const {
  if (count == 0) {
    return 0.0;
  }
  return static_cast<double>(totalTime.count()) / static_cast<double>(count) /
         1'000'000.0;
}

double TimingStatistics::getMinMs() const {
  if (count == 0 || minTime == std::chrono::nanoseconds::max()) {
    return 0.0;
  }
  return static_cast<double>(minTime.count()) / 1'000'000.0;
}

double TimingStatistics::getMaxMs() const {
  if (count == 0) {
    return 0.0;
  }
  return static_cast<double>(maxTime.count()) / 1'000'000.0;
}

void TimingStatistics::reset() {
  totalTime = std::chrono::nanoseconds{0};
  minTime = std::chrono::nanoseconds::max();
  maxTime = std::chrono::nanoseconds{0};
  count = 0;
  histogram.reset();
}

bool TimingStatistics::hasData() const { return count > 0; }

void TransactionStatistics::record(const TransactionOutcome &outcome) {
  switch (outcome.end) {
  case TransactionOutcome::End::committed:
    committed++;
    break;
  case TransactionOutcome::End::rolledBackIntentional:
    rolledBackIntentional++;
    break;
  case TransactionOutcome::End::rolledBackError:
    rolledBackError++;
    break;
  }
  implicitCommits += outcome.implicitCommits;
  savepointRollbacks += outcome.savepointRollbacks;
  subActionsOk += outcome.subOk;
  subActionsFail += outcome.subFail;
  subActionsPerTxn.record(outcome.subOk + outcome.subFail);
}

uint64_t TransactionStatistics::total() const {
  return committed + rolledBackIntentional + rolledBackError;
}

bool TransactionStatistics::hasData() const { return total() > 0; }

void TransactionStatistics::reset() {
  committed = 0;
  rolledBackIntentional = 0;
  rolledBackError = 0;
  implicitCommits = 0;
  savepointRollbacks = 0;
  subActionsOk = 0;
  subActionsFail = 0;
  subActionsPerTxn.reset();
}

namespace {
std::chrono::nanoseconds calculateExecutionTime(
    const std::chrono::high_resolution_clock::time_point &startTime) {
  if (startTime == std::chrono::high_resolution_clock::time_point{}) {
    throw std::logic_error(
        "ActionStatistics::start() must be called before recording results");
  }
  auto endTime = std::chrono::high_resolution_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(endTime -
                                                              startTime);
}
} // namespace

void ActionStatistics::start() {
  startTime = std::chrono::high_resolution_clock::now();
}

void ActionStatistics::recordSuccess(std::chrono::nanoseconds sqlTime) {
  auto execTime = calculateExecutionTime(startTime);
  successCount++;
  executionTiming.record(execTime);
  sqlTiming.record(sqlTime);
}

void ActionStatistics::recordActionFailure(const std::string &errorName,
                                           std::chrono::nanoseconds sqlTime) {
  auto execTime = calculateExecutionTime(startTime);
  actionFailureCount++;
  actionErrorNames[errorName]++;
  executionTiming.record(execTime);
  sqlTiming.record(sqlTime);
}

void ActionStatistics::recordSqlFailure(const std::string &errorCode,
                                        std::chrono::nanoseconds sqlTime) {
  auto execTime = calculateExecutionTime(startTime);
  sqlFailureCount++;
  sqlErrorCodes[errorCode]++;
  executionTiming.record(execTime);
  sqlTiming.record(sqlTime);
}

void ActionStatistics::recordOtherFailure(std::chrono::nanoseconds sqlTime) {
  auto execTime = calculateExecutionTime(startTime);
  otherFailureCount++;
  executionTiming.record(execTime);
  sqlTiming.record(sqlTime);
}

void ActionStatistics::recordConflict(const std::string &errorCode,
                                      std::chrono::nanoseconds sqlTime) {
  auto execTime = calculateExecutionTime(startTime);
  sqlConflictCount++;
  sqlErrorCodes[errorCode]++;
  executionTiming.record(execTime);
  sqlTiming.record(sqlTime);
}

void ActionStatistics::recordRows(const std::string &kind, uint64_t rows) {
  rowHistograms[kind].record(rows);
}

uint64_t ActionStatistics::getTotalCount() const {
  return successCount + actionFailureCount + sqlFailureCount +
         otherFailureCount + sqlConflictCount;
}

uint64_t ActionStatistics::getTotalFailureCount() const {
  return actionFailureCount + sqlFailureCount + otherFailureCount +
         sqlConflictCount;
}

double ActionStatistics::getSuccessRate() const {
  uint64_t total = getTotalCount();
  if (total == 0) {
    return 0.0;
  }
  return static_cast<double>(successCount) / static_cast<double>(total) * 100.0;
}

void ActionStatistics::reset() {
  successCount = 0;
  actionFailureCount = 0;
  sqlFailureCount = 0;
  otherFailureCount = 0;
  sqlConflictCount = 0;
  actionErrorNames.clear();
  sqlErrorCodes.clear();
  rowHistograms.clear();
  executionTiming.reset();
  sqlTiming.reset();
  startTime = std::chrono::high_resolution_clock::time_point{};
}

bool ActionStatistics::hasData() const { return getTotalCount() > 0; }

void WorkerStatistics::startAction(const std::string &actionName) {
  actionStats[actionName].start();
}

void WorkerStatistics::recordSuccess(const std::string &actionName,
                                     std::chrono::nanoseconds sqlTime) {
  actionStats[actionName].recordSuccess(sqlTime);
}

void WorkerStatistics::recordActionFailure(const std::string &actionName,
                                           const std::string &errorName,
                                           std::chrono::nanoseconds sqlTime) {
  actionStats[actionName].recordActionFailure(errorName, sqlTime);
}

void WorkerStatistics::recordSqlFailure(const std::string &actionName,
                                        const std::string &errorCode,
                                        std::chrono::nanoseconds sqlTime) {
  actionStats[actionName].recordSqlFailure(errorCode, sqlTime);
}

void WorkerStatistics::recordOtherFailure(const std::string &actionName,
                                          std::chrono::nanoseconds sqlTime) {
  actionStats[actionName].recordOtherFailure(sqlTime);
}

void WorkerStatistics::recordConflict(const std::string &actionName,
                                      const std::string &errorCode,
                                      std::chrono::nanoseconds sqlTime) {
  actionStats[actionName].recordConflict(errorCode, sqlTime);
}

void WorkerStatistics::recordRows(const std::string &actionName,
                                  const std::string &kind, uint64_t rows) {
  actionStats[actionName].recordRows(kind, rows);
}

void WorkerStatistics::recordTransaction(const TransactionOutcome &outcome) {
  txnStats.record(outcome);
}

void WorkerStatistics::start() {
  startTime = std::chrono::steady_clock::now();
  endTime = startTime;
}

void WorkerStatistics::stop() { endTime = std::chrono::steady_clock::now(); }

void WorkerStatistics::reset() {
  actionStats.clear();
  txnStats.reset();
  startTime = std::chrono::steady_clock::now();
  endTime = startTime;
}

double WorkerStatistics::getTotalDurationSeconds() const {
  auto duration = endTime - startTime;
  return std::chrono::duration<double>(duration).count();
}

uint64_t WorkerStatistics::getTotalActionCount() const {
  uint64_t total = 0;
  for (const auto &[actionName, stats] : actionStats) {
    total += stats.getTotalCount();
  }
  return total;
}

uint64_t WorkerStatistics::getTotalSuccessCount() const {
  uint64_t total = 0;
  for (const auto &[actionName, stats] : actionStats) {
    total += stats.successCount;
  }
  return total;
}

uint64_t WorkerStatistics::getTotalFailureCount() const {
  uint64_t total = 0;
  for (const auto &[actionName, stats] : actionStats) {
    total += stats.getTotalFailureCount();
  }
  return total;
}

double WorkerStatistics::getOverallSuccessRate() const {
  uint64_t total = getTotalActionCount();
  if (total == 0) {
    return 0.0;
  }
  return static_cast<double>(getTotalSuccessCount()) /
         static_cast<double>(total) * 100.0;
}

double WorkerStatistics::getActionsPerSecond() const {
  double duration = getTotalDurationSeconds();
  if (duration <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(getTotalActionCount()) / duration;
}

bool WorkerStatistics::hasData() const { return getTotalActionCount() > 0; }

std::string WorkerStatistics::reportSummary() const {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2);
  oss << "Worker Summary:\n";
  oss << "  Total actions: " << getTotalActionCount() << "\n";
  oss << "  Successful: " << getTotalSuccessCount() << "\n";
  oss << "  Failed: " << getTotalFailureCount() << "\n";
  oss << "  Success rate: " << getOverallSuccessRate() << "%\n";
  oss << "  Duration: " << getTotalDurationSeconds() << "s\n";
  oss << "  Actions/sec: " << getActionsPerSecond() << "\n";
  return oss.str();
}

std::string WorkerStatistics::reportDetailed() const {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2);
  oss << "\nDetailed Action Statistics:\n";
  oss << std::string(80, '-') << "\n";

  for (const auto &[actionName, stats] : actionStats) {
    if (!stats.hasData()) {
      continue;
    }

    oss << "Action: " << actionName << "\n";
    oss << "  Total: " << stats.getTotalCount();
    oss << " (Success: " << stats.successCount;
    oss << ", Action Fail: " << stats.actionFailureCount;
    oss << ", SQL Fail: " << stats.sqlFailureCount;
    oss << ", Other Fail: " << stats.otherFailureCount;
    oss << ", Conflict: " << stats.sqlConflictCount << ")\n";
    oss << "  Success Rate: " << stats.getSuccessRate() << "%\n";

    if (stats.executionTiming.hasData()) {
      oss << "  Execution Time: avg=" << stats.executionTiming.getAverageMs()
          << "ms";
      oss << ", min=" << stats.executionTiming.getMinMs() << "ms";
      oss << ", max=" << stats.executionTiming.getMaxMs() << "ms\n";
    }

    if (stats.sqlTiming.hasData()) {
      oss << "  SQL Time: avg=" << stats.sqlTiming.getAverageMs() << "ms";
      oss << ", min=" << stats.sqlTiming.getMinMs() << "ms";
      oss << ", max=" << stats.sqlTiming.getMaxMs() << "ms\n";
    }

    if (!stats.actionErrorNames.empty()) {
      oss << "  Action Errors: ";
      bool first = true;
      for (const auto &[errorName, count] : stats.actionErrorNames) {
        if (!first) {
          oss << ", ";
        }
        oss << errorName << "=" << count;
        first = false;
      }
      oss << "\n";
    }

    if (!stats.sqlErrorCodes.empty()) {
      oss << "  SQL Errors: ";
      bool first = true;
      for (const auto &[errorCode, count] : stats.sqlErrorCodes) {
        if (!first) {
          oss << ", ";
        }
        oss << errorCode << "=" << count;
        first = false;
      }
      oss << "\n";
    }

    oss << "\n";
  }
  return oss.str();
}

std::string WorkerStatistics::report() const {
  return reportSummary() + reportDetailed();
}

} // namespace statistics
