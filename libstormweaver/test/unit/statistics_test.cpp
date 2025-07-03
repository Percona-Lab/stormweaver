#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <thread>

#include "statistics.hpp"

using namespace statistics;
using namespace std::chrono_literals;

TEST_CASE("TimingStatistics basic functionality", "[statistics][timing]") {
  TimingStatistics timing;

  SECTION("Initial state") {
    REQUIRE_FALSE(timing.hasData());
    REQUIRE(timing.count == 0);
    REQUIRE(timing.getAverageMs() == 0.0);
    REQUIRE(timing.getMinMs() == 0.0);
    REQUIRE(timing.getMaxMs() == 0.0);
  }

  SECTION("Recording single timing") {
    timing.record(1000000ns); // 1ms

    REQUIRE(timing.hasData());
    REQUIRE(timing.count == 1);
    REQUIRE_THAT(timing.getAverageMs(), Catch::Matchers::WithinAbs(1.0, 0.001));
    REQUIRE_THAT(timing.getMinMs(), Catch::Matchers::WithinAbs(1.0, 0.001));
    REQUIRE_THAT(timing.getMaxMs(), Catch::Matchers::WithinAbs(1.0, 0.001));
  }

  SECTION("Recording multiple timings") {
    timing.record(1000000ns); // 1ms
    timing.record(2000000ns); // 2ms
    timing.record(3000000ns); // 3ms

    REQUIRE(timing.count == 3);
    REQUIRE_THAT(timing.getAverageMs(), Catch::Matchers::WithinAbs(2.0, 0.001));
    REQUIRE_THAT(timing.getMinMs(), Catch::Matchers::WithinAbs(1.0, 0.001));
    REQUIRE_THAT(timing.getMaxMs(), Catch::Matchers::WithinAbs(3.0, 0.001));
  }

  SECTION("Reset functionality") {
    timing.record(5000000ns); // 5ms
    timing.reset();

    REQUIRE_FALSE(timing.hasData());
    REQUIRE(timing.count == 0);
    REQUIRE(timing.getAverageMs() == 0.0);
  }
}

TEST_CASE("ActionStatistics basic functionality", "[statistics][action]") {
  ActionStatistics stats;

  SECTION("Initial state") {
    REQUIRE_FALSE(stats.hasData());
    REQUIRE(stats.getTotalCount() == 0);
    REQUIRE(stats.getTotalFailureCount() == 0);
    REQUIRE(stats.getSuccessRate() == 0.0);
    REQUIRE(stats.successCount == 0);
    REQUIRE(stats.actionFailureCount == 0);
    REQUIRE(stats.sqlFailureCount == 0);
    REQUIRE(stats.otherFailureCount == 0);
  }

  SECTION("Recording success") {
    stats.start();
    std::this_thread::sleep_for(1ms); // Ensure some execution time
    stats.recordSuccess(500000ns);    // 0.5ms SQL time

    REQUIRE(stats.hasData());
    REQUIRE(stats.getTotalCount() == 1);
    REQUIRE(stats.getTotalFailureCount() == 0);
    REQUIRE(stats.successCount == 1);
    REQUIRE_THAT(stats.getSuccessRate(),
                 Catch::Matchers::WithinAbs(100.0, 0.001));

    // Check timing was recorded
    REQUIRE(stats.executionTiming.hasData());
    REQUIRE(stats.sqlTiming.hasData());
    REQUIRE_THAT(stats.sqlTiming.getAverageMs(),
                 Catch::Matchers::WithinAbs(0.5, 0.001));
  }

  SECTION("Recording action failure") {
    stats.start();
    std::this_thread::sleep_for(1ms);
    stats.recordActionFailure("test-error", 300000ns); // 0.3ms SQL time

    REQUIRE(stats.hasData());
    REQUIRE(stats.getTotalCount() == 1);
    REQUIRE(stats.getTotalFailureCount() == 1);
    REQUIRE(stats.actionFailureCount == 1);
    REQUIRE(stats.getSuccessRate() == 0.0);

    // Check error tracking
    REQUIRE(stats.actionErrorNames.count("test-error") == 1);
    REQUIRE(stats.actionErrorNames.at("test-error") == 1);

    // Check timing was recorded
    REQUIRE(stats.executionTiming.hasData());
    REQUIRE(stats.sqlTiming.hasData());
  }

  SECTION("Recording SQL failure") {
    stats.start();
    std::this_thread::sleep_for(1ms);
    stats.recordSqlFailure("sql-error-code", 700000ns); // 0.7ms SQL time

    REQUIRE(stats.sqlFailureCount == 1);
    REQUIRE(stats.sqlErrorCodes.count("sql-error-code") == 1);
    REQUIRE(stats.sqlErrorCodes.at("sql-error-code") == 1);
    REQUIRE_THAT(stats.sqlTiming.getAverageMs(),
                 Catch::Matchers::WithinAbs(0.7, 0.001));
  }

  SECTION("Recording other failure") {
    stats.start();
    std::this_thread::sleep_for(1ms);
    stats.recordOtherFailure(100000ns); // 0.1ms SQL time

    REQUIRE(stats.otherFailureCount == 1);
    REQUIRE_THAT(stats.sqlTiming.getAverageMs(),
                 Catch::Matchers::WithinAbs(0.1, 0.001));
  }

  SECTION("Mixed statistics") {
    // Record multiple different types
    stats.start();
    std::this_thread::sleep_for(1ms);
    stats.recordSuccess(1000000ns); // 1ms SQL

    stats.start();
    std::this_thread::sleep_for(1ms);
    stats.recordActionFailure("error1", 2000000ns); // 2ms SQL

    stats.start();
    std::this_thread::sleep_for(1ms);
    stats.recordActionFailure("error1",
                              3000000ns); // 3ms SQL (same error again)

    stats.start();
    std::this_thread::sleep_for(1ms);
    stats.recordSqlFailure("sql-err", 4000000ns); // 4ms SQL

    REQUIRE(stats.getTotalCount() == 4);
    REQUIRE(stats.getTotalFailureCount() == 3);
    REQUIRE(stats.successCount == 1);
    REQUIRE(stats.actionFailureCount == 2);
    REQUIRE(stats.sqlFailureCount == 1);
    REQUIRE_THAT(stats.getSuccessRate(),
                 Catch::Matchers::WithinAbs(25.0, 0.001));

    // Check error aggregation
    REQUIRE(stats.actionErrorNames.at("error1") == 2);
    REQUIRE(stats.sqlErrorCodes.at("sql-err") == 1);

    // Check SQL timing aggregation
    REQUIRE(stats.sqlTiming.count == 4);
    REQUIRE_THAT(stats.sqlTiming.getAverageMs(),
                 Catch::Matchers::WithinAbs(2.5, 0.001)); // (1+2+3+4)/4 = 2.5
    REQUIRE_THAT(stats.sqlTiming.getMinMs(),
                 Catch::Matchers::WithinAbs(1.0, 0.001));
    REQUIRE_THAT(stats.sqlTiming.getMaxMs(),
                 Catch::Matchers::WithinAbs(4.0, 0.001));
  }

  SECTION("Reset functionality") {
    stats.start();
    std::this_thread::sleep_for(1ms);
    stats.recordSuccess(1000000ns);
    stats.recordActionFailure("error", 2000000ns);

    REQUIRE(stats.hasData());

    stats.reset();

    REQUIRE_FALSE(stats.hasData());
    REQUIRE(stats.getTotalCount() == 0);
    REQUIRE(stats.actionErrorNames.empty());
    REQUIRE(stats.sqlErrorCodes.empty());
    REQUIRE_FALSE(stats.executionTiming.hasData());
    REQUIRE_FALSE(stats.sqlTiming.hasData());
  }
}

TEST_CASE("WorkerStatistics functionality", "[statistics][worker]") {
  WorkerStatistics worker;

  SECTION("Initial state") {
    REQUIRE_FALSE(worker.hasData());
    REQUIRE(worker.getTotalActionCount() == 0);
    REQUIRE(worker.getTotalSuccessCount() == 0);
    REQUIRE(worker.getTotalFailureCount() == 0);
    REQUIRE(worker.getOverallSuccessRate() == 0.0);
    REQUIRE(worker.getActionsPerSecond() == 0.0);
  }

  SECTION("Single action type") {
    worker.start();

    worker.startAction("test-action");
    std::this_thread::sleep_for(1ms);
    worker.recordSuccess("test-action", 500000ns);

    worker.startAction("test-action");
    std::this_thread::sleep_for(1ms);
    worker.recordActionFailure("test-action", "failure-reason", 300000ns);

    worker.stop();

    REQUIRE(worker.hasData());
    REQUIRE(worker.getTotalActionCount() == 2);
    REQUIRE(worker.getTotalSuccessCount() == 1);
    REQUIRE(worker.getTotalFailureCount() == 1);
    REQUIRE_THAT(worker.getOverallSuccessRate(),
                 Catch::Matchers::WithinAbs(50.0, 0.001));

    // Check action-specific stats
    REQUIRE(worker.actionStats.count("test-action") == 1);
    const auto &actionStats = worker.actionStats.at("test-action");
    REQUIRE(actionStats.getTotalCount() == 2);
    REQUIRE(actionStats.successCount == 1);
    REQUIRE(actionStats.actionFailureCount == 1);

    // Check actions per second (should be > 0 since we had a duration)
    REQUIRE(worker.getActionsPerSecond() > 0.0);
  }

  SECTION("Multiple action types") {
    worker.start();

    // Action type 1
    worker.startAction("create-table");
    std::this_thread::sleep_for(1ms);
    worker.recordSuccess("create-table", 1000000ns);

    worker.startAction("create-table");
    std::this_thread::sleep_for(1ms);
    worker.recordSuccess("create-table", 1200000ns);

    // Action type 2
    worker.startAction("insert-data");
    std::this_thread::sleep_for(1ms);
    worker.recordSqlFailure("insert-data", "constraint-violation", 800000ns);

    worker.stop();

    REQUIRE(worker.actionStats.size() == 2);
    REQUIRE(worker.getTotalActionCount() == 3);
    REQUIRE(worker.getTotalSuccessCount() == 2);
    REQUIRE(worker.getTotalFailureCount() == 1);

    // Check per-action stats
    const auto &createStats = worker.actionStats.at("create-table");
    REQUIRE(createStats.successCount == 2);
    REQUIRE(createStats.getTotalFailureCount() == 0);

    const auto &insertStats = worker.actionStats.at("insert-data");
    REQUIRE(insertStats.successCount == 0);
    REQUIRE(insertStats.sqlFailureCount == 1);
    REQUIRE(insertStats.sqlErrorCodes.at("constraint-violation") == 1);
  }

  SECTION("Duration calculation") {
    worker.start();

    // Wait a bit to ensure measurable duration
    std::this_thread::sleep_for(10ms);

    worker.stop();

    double duration = worker.getTotalDurationSeconds();
    REQUIRE(duration >= 0.01); // At least 10ms
    REQUIRE(duration < 1.0);   // But less than 1 second
  }

  SECTION("Reset functionality") {
    worker.start();
    worker.startAction("test");
    std::this_thread::sleep_for(1ms);
    worker.recordSuccess("test", 1000000ns);
    worker.stop();

    REQUIRE(worker.hasData());

    worker.reset();

    REQUIRE_FALSE(worker.hasData());
    REQUIRE(worker.actionStats.empty());
    REQUIRE(worker.getTotalActionCount() == 0);
  }
}

TEST_CASE("Statistics edge cases", "[statistics][edge]") {
  SECTION("Zero duration timings") {
    TimingStatistics timing;
    timing.record(0ns);

    REQUIRE(timing.hasData());
    REQUIRE(timing.count == 1);
    REQUIRE(timing.getAverageMs() == 0.0);
    REQUIRE(timing.getMinMs() == 0.0);
    REQUIRE(timing.getMaxMs() == 0.0);
  }

  SECTION("Large duration values") {
    TimingStatistics timing;
    timing.record(std::chrono::seconds(1)); // 1 second = 1000ms

    REQUIRE_THAT(timing.getAverageMs(),
                 Catch::Matchers::WithinAbs(1000.0, 0.001));
  }

  SECTION("ActionStatistics without timing start throws logic error") {
    ActionStatistics stats;

    // Recording without start() should throw logic error
    REQUIRE_THROWS_AS(stats.recordSuccess(1000000ns), std::logic_error);
    REQUIRE_THROWS_AS(stats.recordActionFailure("error", 1000000ns),
                      std::logic_error);
    REQUIRE_THROWS_AS(stats.recordSqlFailure("sql-error", 1000000ns),
                      std::logic_error);
    REQUIRE_THROWS_AS(stats.recordOtherFailure(1000000ns), std::logic_error);

    // No data should be recorded due to exceptions
    REQUIRE_FALSE(stats.hasData());
    REQUIRE(stats.getTotalCount() == 0);
  }

  SECTION("Empty error strings") {
    ActionStatistics stats;
    stats.start();
    std::this_thread::sleep_for(1ms);

    stats.recordActionFailure("", 0ns);
    stats.recordSqlFailure("", 0ns);

    REQUIRE(stats.actionErrorNames.count("") == 1);
    REQUIRE(stats.sqlErrorCodes.count("") == 1);
  }
}