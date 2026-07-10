#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "logging.hpp"
#include "sql_variant/generic.hpp"

using namespace sql_variant;

namespace {

struct FakeResultData : QuerySpecificResult {
  std::size_t rows;
  std::size_t fields;
  explicit FakeResultData(std::size_t rows, std::size_t fields = 1)
      : rows(rows), fields(fields) {}
  std::size_t numFields() const override { return fields; }
  std::size_t numRows() const override { return rows; }
  RowView nextRow() const override { return {}; }
  RowView rowAt(std::size_t) const override { return {}; }
};

struct FakeSQL : GenericSQL {
  std::size_t selectRows = 0;
  std::uint64_t affected = 0;
  bool withReturnsData = false; // WITH ... SELECT: attach rows like a select

  void logError(std::ostream &) const override {}
  std::string serverInfoString() const override { return "fake"; }
  std::string hostInfo() const override { return "fake"; }
  void reconnect() override {}

  QueryResult executeQuery(std::string const &query) const override {
    QueryResult res;
    res.query = query;
    res.executedAt = std::chrono::high_resolution_clock::now();
    res.executionTime = std::chrono::nanoseconds{1000};
    res.errorInfo.errorStatus = SqlStatus::success;
    const auto kind = classifyStatement(query);
    // mirror real drivers: data is ALWAYS attached, row-shaped only for
    // select-like results (fields>0); DML/DDL get a fieldless wrapper
    if (kind == StmtKind::select ||
        (kind == StmtKind::with && withReturnsData)) {
      res.data = std::make_unique<FakeResultData>(selectRows, 1);
    } else {
      res.data = std::make_unique<FakeResultData>(0, 0);
      if (kind == StmtKind::insert || kind == StmtKind::update ||
          kind == StmtKind::del || kind == StmtKind::with) {
        res.affectedRows = affected;
      }
    }
    return res;
  }

  QueryResult executeParams(std::string const &query,
                            std::vector<Param> const &) const override {
    return executeQuery(query);
  }
};

struct log_dir_guard {
  explicit log_dir_guard(std::filesystem::path dir) {
    logging::set_log_dir(std::move(dir));
  }
  ~log_dir_guard() { logging::set_log_dir("logs"); }
};

} // namespace

TEST_CASE("LoggedSQL records row observations", "[sql][loggedsql]") {
  auto const dir = std::filesystem::temp_directory_path() / "sw-loggedsql-test";
  std::filesystem::remove_all(dir);
  log_dir_guard guard(dir);

  auto fake = std::make_unique<FakeSQL>();
  fake->selectRows = 42;
  fake->affected = 3;
  LoggedSQL conn(std::move(fake), "loggedsql-test-1");

  conn.setCurrentAction("my_select");
  (void)conn.executeQuery("SELECT * FROM t");
  conn.setCurrentAction("my_update");
  (void)conn.executeQuery("UPDATE t SET a=1");
  (void)conn.executeQuery("COMMIT;"); // no observation

  auto obs = conn.drainRowObservations();
  REQUIRE(obs.size() == 2);
  REQUIRE(obs[0].action == "my_select");
  REQUIRE(obs[0].kind == "select");
  REQUIRE(obs[0].rows == 42);
  REQUIRE(obs[1].action == "my_update");
  REQUIRE(obs[1].kind == "update");
  REQUIRE(obs[1].rows == 3);

  REQUIRE(conn.drainRowObservations().empty()); // drained
}

TEST_CASE("LoggedSQL WITH classification by result shape", "[sql][loggedsql]") {
  auto const dir = std::filesystem::temp_directory_path() / "sw-loggedsql-test";
  log_dir_guard guard(dir);

  auto fake = std::make_unique<FakeSQL>();
  fake->affected = 7;
  LoggedSQL conn(std::move(fake), "loggedsql-test-2");
  conn.setCurrentAction("act");
  // fake returns no data for WITH -> falls back to affected as kind "dml"
  (void)conn.executeQuery("WITH w AS (SELECT 1) UPDATE t SET a=1");

  auto obs = conn.drainRowObservations();
  REQUIRE(obs.size() == 1);
  REQUIRE(obs[0].kind == "dml");
  REQUIRE(obs[0].rows == 7);
}

TEST_CASE("LoggedSQL DML with present-but-fieldless data records affected",
          "[sql][loggedsql]") {
  // production shape: both drivers always attach a data wrapper, DML just
  // has fields()==0 on it. must not be mistaken for a row-shaped result.
  auto const dir = std::filesystem::temp_directory_path() / "sw-loggedsql-test";
  log_dir_guard guard(dir);

  auto fake = std::make_unique<FakeSQL>();
  fake->affected = 11;
  LoggedSQL conn(std::move(fake), "loggedsql-test-7");
  conn.setCurrentAction("act");
  (void)conn.executeQuery("UPDATE t SET a=1");

  auto obs = conn.drainRowObservations();
  REQUIRE(obs.size() == 1);
  REQUIRE(obs[0].kind == "update");
  REQUIRE(obs[0].rows == 11);
}

TEST_CASE("ActionNameScope restores previous name", "[sql][loggedsql]") {
  auto const dir = std::filesystem::temp_directory_path() / "sw-loggedsql-test";
  log_dir_guard guard(dir);

  auto fake = std::make_unique<FakeSQL>();
  fake->selectRows = 1;
  LoggedSQL conn(std::move(fake), "loggedsql-test-3");
  conn.setCurrentAction("transaction");
  {
    auto scope = conn.scopedActionName("sub_action");
    (void)conn.executeQuery("SELECT 1");
  }
  (void)conn.executeQuery("SELECT 2");

  auto obs = conn.drainRowObservations();
  REQUIRE(obs.size() == 2);
  REQUIRE(obs[0].action == "sub_action");
  REQUIRE(obs[1].action == "transaction");
}

TEST_CASE("LoggedSQL transaction outcomes drain", "[sql][loggedsql]") {
  auto const dir = std::filesystem::temp_directory_path() / "sw-loggedsql-test";
  log_dir_guard guard(dir);

  LoggedSQL conn(std::make_unique<FakeSQL>(), "loggedsql-test-4");
  statistics::TransactionOutcome out;
  out.end = statistics::TransactionOutcome::End::committed;
  conn.recordTransactionOutcome(out);

  auto drained = conn.drainTransactionOutcomes();
  REQUIRE(drained.size() == 1);
  REQUIRE(drained[0].end == statistics::TransactionOutcome::End::committed);
  REQUIRE(conn.drainTransactionOutcomes().empty());

  conn.recordTransactionOutcome(out);
  conn.clearObservations();
  REQUIRE(conn.drainTransactionOutcomes().empty());
}

TEST_CASE("LoggedSQL executeParams records an observation",
          "[sql][loggedsql]") {
  auto const dir = std::filesystem::temp_directory_path() / "sw-loggedsql-test";
  log_dir_guard guard(dir);

  auto fake = std::make_unique<FakeSQL>();
  fake->selectRows = 5;
  LoggedSQL conn(std::move(fake), "loggedsql-test-5");
  conn.setCurrentAction("act");
  (void)conn.executeParams("SELECT * FROM t", {});

  auto obs = conn.drainRowObservations();
  REQUIRE(obs.size() == 1);
  REQUIRE(obs[0].action == "act");
  REQUIRE(obs[0].kind == "select");
  REQUIRE(obs[0].rows == 5);
}

TEST_CASE("LoggedSQL WITH returning data records kind select",
          "[sql][loggedsql]") {
  auto const dir = std::filesystem::temp_directory_path() / "sw-loggedsql-test";
  log_dir_guard guard(dir);

  auto fake = std::make_unique<FakeSQL>();
  fake->selectRows = 9;
  fake->withReturnsData = true;
  LoggedSQL conn(std::move(fake), "loggedsql-test-6");
  conn.setCurrentAction("act");
  (void)conn.executeQuery("WITH w AS (SELECT 1) SELECT * FROM w");

  auto obs = conn.drainRowObservations();
  REQUIRE(obs.size() == 1);
  REQUIRE(obs[0].action == "act");
  REQUIRE(obs[0].kind == "select");
  REQUIRE(obs[0].rows == 9);
}
