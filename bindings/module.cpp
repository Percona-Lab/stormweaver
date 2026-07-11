#include <nanobind/nanobind.h>
#include <nanobind/stl/function.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>

#include <spdlog/spdlog.h>

#include "action/action_registry.hpp"
#include "logging.hpp"
#include "metadata/context.hpp"
#include "metadata/table.hpp"
#include "py_action.hpp"
#include "querygen/config.hpp"
#include "random.hpp"
#include "sql_variant/generic.hpp"
#include "sql_variant/mysql.hpp"
#include "sql_variant/postgresql.hpp"
#include "statistics.hpp"
#include "workload.hpp"

namespace nb = nanobind;
using namespace sql_variant;

static std::unique_ptr<LoggedSQL>
connect_pg(std::string host, uint16_t port, std::string dbname,
           std::string user, std::string password, std::string log_name) {
  ServerParams params{dbname, host, "", user, password, port};
  auto sql = std::make_unique<sql_variant::PostgreSQL>(params);
  return std::make_unique<LoggedSQL>(std::move(sql), log_name);
}

static std::unique_ptr<LoggedSQL>
connect_mysql(std::string host, uint16_t port, std::string dbname,
              std::string user, std::string password, std::string socket,
              std::string log_name) {
  ServerParams params{dbname, host, socket, user, password, port};
  auto sql = std::make_unique<sql_variant::MySQL>(params);
  return std::make_unique<LoggedSQL>(std::move(sql), log_name);
}

static action::ActionType parse_action_type(std::string const &action_type) {
  if (action_type == "ddl") {
    return action::ActionType::ddl;
  }
  if (action_type == "dml") {
    return action::ActionType::dml;
  }
  return action::ActionType::other;
}

static const char *error_class_name(ErrorClass c) {
  switch (c) {
  case ErrorClass::none:
    return "none";
  case ErrorClass::conflict:
    return "conflict";
  case ErrorClass::failedTxn:
    return "failedTxn";
  case ErrorClass::serverGone:
    return "serverGone";
  case ErrorClass::other:
    return "other";
  }
  return "other";
}

static std::vector<Param> to_params(nb::handle seq) {
  // str/bytes iterate per element and would silently become many params
  if (nb::isinstance<nb::str>(seq) || nb::isinstance<nb::bytes>(seq)) {
    throw nb::type_error("params must be a sequence of values, not str/bytes");
  }
  std::vector<Param> out;
  for (nb::handle o : seq) {
    Param p;
    if (o.is_none()) {
      // NULL
    } else if (nb::isinstance<nb::bool_>(o)) {
      // bool first: python bool is an int subclass
      p.value = nb::cast<bool>(o) ? "1" : "0";
    } else if (nb::isinstance<nb::int_>(o) || nb::isinstance<nb::float_>(o)) {
      // str() round-trips arbitrary ints and shortest-repr floats
      p.value = nb::cast<std::string>(nb::str(o));
    } else if (nb::isinstance<nb::str>(o)) {
      p.value = nb::cast<std::string>(o);
    } else if (nb::isinstance<nb::bytes>(o)) {
      auto b = nb::cast<nb::bytes>(o);
      p.value = std::string(b.c_str(), b.size());
      p.binary = true;
    } else {
      throw nb::type_error("unsupported SQL parameter type");
    }
    out.push_back(std::move(p));
  }
  return out;
}

// exception type kept at file scope so the capture-less translator lambda
// (plain function pointer) can reach it
static PyObject *sql_error_type = nullptr;

NB_MODULE(_stormweaver, m) {
  m.attr("__version__") = "0.1.0";

  // --- Logging ---

  m.def(
      "init_core_logging",
      [](std::string const &log_dir,
         std::function<void(int, std::string const &, std::string const &)>
             sink,
         int level, bool unified, bool splits) {
        logging::set_log_dir(log_dir);
        logging::set_unified(unified, splits);
        logging::set_python_sink(std::move(sink));
        logging::set_level(level);
      },
      nb::arg("log_dir"), nb::arg("sink"), nb::arg("level"),
      nb::arg("unified") = false, nb::arg("splits") = false);

  m.def("shutdown_core_logging", []() {
    // drop the python callable while the interpreter is still alive.
    // empty function, not empty-bodied lambda: sink short-circuits on falsy
    logging::set_python_sink(
        std::function<void(int, std::string const &, std::string const &)>{});
  });

  // test hook: emit through the core default logger like spdlog::info does
  m.def("_core_log", [](int level, std::string const &msg) {
    spdlog::default_logger()->log(static_cast<spdlog::level::level_enum>(level),
                                  msg);
  });

  // test hook: emit through a named file logger like workers/connections do
  m.def("_file_log", [](std::string const &name, std::string const &filename,
                        int level, std::string const &msg) {
    auto logger = logging::make_file_logger(name, filename);
    logger->log(static_cast<spdlog::level::level_enum>(level), msg);
    logger->flush();
    spdlog::drop(name);
  });

  // --- SQL Layer ---

  sql_error_type = PyErr_NewException("stormweaver._stormweaver.SqlError",
                                      PyExc_RuntimeError, nullptr);
  m.attr("SqlError") = nb::borrow<nb::object>(sql_error_type);

  nb::register_exception_translator(
      [](const std::exception_ptr &p, void *) {
        try {
          std::rethrow_exception(p);
        } catch (SqlException const &e) {
          nb::object inst =
              nb::borrow<nb::object>(sql_error_type)(nb::str(e.what()));
          inst.attr("error_code") = nb::str(e.getErrorCode().c_str());
          inst.attr("error_class") = nb::str(error_class_name(e.errorClass()));
          PyErr_SetObject(sql_error_type, inst.ptr());
        }
      },
      nullptr);

  nb::class_<QueryResult>(m, "QueryResult")
      .def("success", &QueryResult::success)
      .def_ro("query", &QueryResult::query)
      .def_ro("affected_rows", &QueryResult::affectedRows)
      .def_prop_ro("error_code",
                   [](const QueryResult &r) { return r.errorInfo.errorCode; })
      .def_prop_ro(
          "error_message",
          [](const QueryResult &r) { return r.errorInfo.errorMessage; })
      .def_prop_ro("error_class",
                   [](const QueryResult &r) {
                     return error_class_name(r.errorInfo.errorClass);
                   })
      .def("rows", [](const QueryResult &r) {
        std::vector<std::vector<std::optional<std::string>>> rows;
        if (!r.data) {
          return rows;
        }
        const std::size_t count = r.data->numRows();
        rows.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
          auto row = r.data->rowAt(i);
          std::vector<std::optional<std::string>> fields;
          fields.reserve(row.rowData.size());
          for (auto const &field : row.rowData) {
            if (field) {
              fields.emplace_back(*field);
            } else {
              fields.emplace_back(std::nullopt);
            }
          }
          rows.push_back(std::move(fields));
        }
        return rows;
      });

  // marshal params while attached, then release the thread state for the
  // blocking SQL call: free-threaded cyclic GC stops the world by waiting
  // on all attached threads, so a lock-waiting query must not hold ours
  nb::class_<LoggedSQL>(m, "LoggedSQL")
      .def(
          "execute",
          [](LoggedSQL const &self, std::string const &query,
             nb::object params) {
            bool const plain = params.is_none();
            auto p = plain ? std::vector<Param>{} : to_params(params);
            nb::gil_scoped_release rel;
            return plain ? self.executeQuery(query)
                         : self.executeParams(query, p);
          },
          nb::arg("query"), nb::arg("params") = nb::none())
      .def(
          "safe_execute",
          [](LoggedSQL const &self, std::string const &query,
             nb::object params) {
            auto p =
                params.is_none() ? std::vector<Param>{} : to_params(params);
            nb::gil_scoped_release rel;
            return self.safeQuery(query, p);
          },
          nb::arg("query"), nb::arg("params") = nb::none())
      .def("reconnect", &LoggedSQL::reconnect,
           nb::call_guard<nb::gil_scoped_release>());

  m.def("connect_pg", &connect_pg, nb::arg("host") = "localhost",
        nb::arg("port") = 5432, nb::arg("dbname") = "postgres",
        nb::arg("user") = "postgres", nb::arg("password") = "",
        nb::arg("log_name") = "python");

  m.def("connect_mysql", &connect_mysql, nb::arg("host") = "localhost",
        nb::arg("port") = 3306, nb::arg("dbname") = "test",
        nb::arg("user") = "root", nb::arg("password") = "",
        nb::arg("socket") = "", nb::arg("log_name") = "python");

  // --- Metadata ---

  nb::class_<metadata::TableRegistry>(m, "Metadata")
      .def(nb::init<>())
      .def("size",
           [](metadata::TableRegistry const &self) {
             return self.get<metadata::Table>().size();
           })
      .def("reset", &metadata::TableRegistry::reset)
      .def("table_names", [](metadata::TableRegistry const &self) {
        std::vector<std::string> names;
        for (auto const &table : self.get<metadata::Table>().snapshotAll()) {
          names.push_back(table->name);
        }
        return names;
      });

  nb::class_<metadata::Context>(m, "MetadataContext")
      .def("size",
           [](metadata::Context const &self) {
             return self.get<metadata::Table>().size();
           })
      .def("reset", [](metadata::Context &self) { self.registry().reset(); })
      .def("table_names", [](metadata::Context const &self) {
        std::vector<std::string> names;
        for (auto const &table : self.get<metadata::Table>().snapshotAll()) {
          names.push_back(table->name);
        }
        return names;
      });

  // --- Random ---

  nb::class_<ps_random>(m, "Random")
      .def(nb::init<>())
      .def(nb::init<std::uint64_t>())
      .def(
          "number",
          [](ps_random &self, std::size_t min, std::size_t max) {
            return self.random_number<std::size_t>(min, max);
          },
          nb::arg("min"), nb::arg("max"))
      .def("string", &ps_random::random_string, nb::arg("min_length"),
           nb::arg("max_length"))
      .def("boolean", &ps_random::random_bool);

  // --- Action ---

  nb::class_<action::ActionFactory>(m, "ActionFactory")
      .def_ro("name", &action::ActionFactory::name)
      .def_rw("weight", &action::ActionFactory::weight);

  nb::class_<action::ActionRegistry>(m, "ActionRegistry")
      .def(nb::init<>())
      .def("insert", &action::ActionRegistry::insert)
      .def("remove", &action::ActionRegistry::remove)
      .def("has", &action::ActionRegistry::has)
      .def("size", &action::ActionRegistry::size)
      .def("total_weight", &action::ActionRegistry::totalWeight)
      // returned reference can still dangle if registry mutation reallocates
      // the factory vector after get; known limitation until action-system
      // rework
      .def("get", &action::ActionRegistry::getReference,
           nb::rv_policy::reference_internal)
      .def(
          "make_custom_sql",
          [](action::ActionRegistry &registry, std::string const &name,
             std::string const &sql, std::size_t weight,
             std::string const &action_type) {
            registry.makeCustomSqlAction(name, sql, weight,
                                         parse_action_type(action_type));
          },
          nb::arg("name"), nb::arg("sql"), nb::arg("weight"),
          nb::arg("action_type") = "other")
      .def(
          "make_custom_table_sql",
          [](action::ActionRegistry &registry, std::string const &name,
             std::string const &sql, std::size_t weight,
             std::string const &action_type) {
            registry.makeCustomTableSqlAction(name, sql, weight,
                                              parse_action_type(action_type));
          },
          nb::arg("name"), nb::arg("sql"), nb::arg("weight"),
          nb::arg("action_type") = "other")
      .def("use", &action::ActionRegistry::use)
      .def(
          "register_python",
          [](action::ActionRegistry &registry, std::string const &name,
             std::size_t weight, nb::object fn,
             std::string const &action_type) {
            auto shared_fn = make_py_action_fn(std::move(fn));
            registry.insert(action::ActionFactory{
                .name = name,
                .builder =
                    [shared_fn](action::BuildContext const &) {
                      return std::make_unique<PyCallableAction>(shared_fn);
                    },
                .weight = weight,
                .type = parse_action_type(action_type)});
          },
          nb::arg("name"), nb::arg("weight"), nb::arg("fn"),
          nb::arg("action_type") = "other");

  m.def(
      "default_action_registry",
      [](std::string const &flavor) -> action::ActionRegistry & {
        return action::default_registry(flavor == "mysql"
                                            ? sql_variant::flavor::ANY_MYSQL
                                            : sql_variant::flavor::ANY_PG);
      },
      nb::arg("flavor") = "pg", nb::rv_policy::reference);

  // --- Configs ---

  nb::class_<action::DdlConfig>(m, "DdlConfig")
      .def(nb::init<>())
      .def_rw("min_table_count", &action::DdlConfig::min_table_count)
      .def_rw("max_table_count", &action::DdlConfig::max_table_count)
      .def_rw("max_column_count", &action::DdlConfig::max_column_count)
      .def_rw("access_methods", &action::DdlConfig::access_methods);

  nb::class_<action::LockWeights>(m, "LockWeights")
      .def(nb::init<>())
      .def_rw("none", &action::LockWeights::none)
      .def_rw("for_update", &action::LockWeights::for_update)
      .def_rw("for_share", &action::LockWeights::for_share);

  nb::class_<action::DmlConfig>(m, "DmlConfig")
      .def(nb::init<>())
      .def_rw("delete_min", &action::DmlConfig::deleteMin)
      .def_rw("delete_max", &action::DmlConfig::deleteMax)
      .def_rw("update_min", &action::DmlConfig::updateMin)
      .def_rw("update_max", &action::DmlConfig::updateMax)
      .def_rw("lock_weights", &action::DmlConfig::lockWeights);

  nb::class_<action::IsolationWeights>(m, "IsolationWeights")
      .def(nb::init<>())
      .def_rw("server_default", &action::IsolationWeights::server_default)
      .def_rw("read_committed", &action::IsolationWeights::read_committed)
      .def_rw("repeatable_read", &action::IsolationWeights::repeatable_read)
      .def_rw("serializable", &action::IsolationWeights::serializable);

  nb::class_<action::TransactionConfig>(m, "TransactionConfig")
      .def(nb::init<>())
      .def_rw("min_sub_actions", &action::TransactionConfig::min_sub_actions)
      .def_rw("max_sub_actions", &action::TransactionConfig::max_sub_actions)
      .def_rw("commit_prob", &action::TransactionConfig::commit_probability)
      .def_rw("rollback_to_savepoint_prob",
              &action::TransactionConfig::rollback_to_savepoint_probability)
      .def_rw("isolation_weights",
              &action::TransactionConfig::isolation_weights)
      .def_prop_rw(
          "error_mode",
          [](action::TransactionConfig const &c) {
            return c.error_mode ==
                           action::TransactionConfig::ErrorMode::savepoint
                       ? "savepoint"
                       : "abort";
          },
          [](action::TransactionConfig &c, std::string const &v) {
            if (v == "savepoint") {
              c.error_mode = action::TransactionConfig::ErrorMode::savepoint;
            } else if (v == "abort") {
              c.error_mode = action::TransactionConfig::ErrorMode::abort;
            } else {
              throw std::invalid_argument("error_mode: savepoint|abort");
            }
          })
      .def_prop_rw(
          "mysql_ddl_mode",
          [](action::TransactionConfig const &c) {
            return c.mysql_ddl_mode ==
                           action::TransactionConfig::MysqlDdlMode::mirror
                       ? "mirror"
                       : "exclude";
          },
          [](action::TransactionConfig &c, std::string const &v) {
            if (v == "mirror") {
              c.mysql_ddl_mode =
                  action::TransactionConfig::MysqlDdlMode::mirror;
            } else if (v == "exclude") {
              c.mysql_ddl_mode =
                  action::TransactionConfig::MysqlDdlMode::exclude;
            } else {
              throw std::invalid_argument("mysql_ddl_mode: mirror|exclude");
            }
          });

  nb::class_<querygen::QueryGenConfig>(m, "QueryGenConfig")
      .def(nb::init<>())
      .def_rw("max_joins", &querygen::QueryGenConfig::max_joins)
      .def_rw("max_expr_depth", &querygen::QueryGenConfig::max_expr_depth)
      .def_rw("max_subquery_depth",
              &querygen::QueryGenConfig::max_subquery_depth)
      .def_rw("join_prob", &querygen::QueryGenConfig::join_prob)
      .def_rw("subquery_prob", &querygen::QueryGenConfig::subquery_prob)
      .def_rw("aggregate_prob", &querygen::QueryGenConfig::aggregate_prob)
      .def_rw("cte_prob", &querygen::QueryGenConfig::cte_prob)
      .def_rw("setop_prob", &querygen::QueryGenConfig::setop_prob)
      .def_rw("window_prob", &querygen::QueryGenConfig::window_prob)
      .def_rw("order_by_prob", &querygen::QueryGenConfig::order_by_prob)
      .def_rw("limit_prob", &querygen::QueryGenConfig::limit_prob)
      .def_rw("correlation_prob", &querygen::QueryGenConfig::correlation_prob)
      .def_rw("dml_predicate_prob",
              &querygen::QueryGenConfig::dml_predicate_prob)
      .def_rw("dml_pk_select_prob",
              &querygen::QueryGenConfig::dml_pk_select_prob);

  nb::class_<action::AllConfig>(m, "AllConfig")
      .def(nb::init<>())
      .def_rw("ddl", &action::AllConfig::ddl)
      .def_rw("dml", &action::AllConfig::dml)
      .def_rw("transaction", &action::AllConfig::transaction)
      .def_rw("querygen", &action::AllConfig::querygen);

  nb::class_<WorkloadParams>(m, "WorkloadParams")
      .def(nb::init<>())
      .def_rw("action_config", &WorkloadParams::actionConfig)
      .def_rw("duration_in_seconds", &WorkloadParams::duration_in_seconds)
      .def_rw("repeat_times", &WorkloadParams::repeat_times)
      .def_rw("number_of_workers", &WorkloadParams::number_of_workers)
      .def_rw("max_reconnect_attempts", &WorkloadParams::max_reconnect_attempts)
      .def_rw("seed", &WorkloadParams::seed);

  // --- Statistics ---
  // worker threads mutate their own stats while running - read only after join

  nb::class_<statistics::TimingStatistics>(m, "TimingStatistics")
      .def("avg_ms", &statistics::TimingStatistics::getAverageMs)
      .def("min_ms", &statistics::TimingStatistics::getMinMs)
      .def("max_ms", &statistics::TimingStatistics::getMaxMs)
      .def_ro("count", &statistics::TimingStatistics::count)
      .def("has_data", &statistics::TimingStatistics::hasData)
      .def("histogram", [](statistics::TimingStatistics const &self) {
        return std::vector<std::uint64_t>(self.histogram.buckets.begin(),
                                          self.histogram.buckets.end());
      });

  nb::class_<statistics::ActionStatistics>(m, "ActionStatistics")
      .def_ro("success_count", &statistics::ActionStatistics::successCount)
      .def_ro("action_failure_count",
              &statistics::ActionStatistics::actionFailureCount)
      .def_ro("sql_failure_count",
              &statistics::ActionStatistics::sqlFailureCount)
      .def_ro("other_failure_count",
              &statistics::ActionStatistics::otherFailureCount)
      .def_ro("sql_conflict_count",
              &statistics::ActionStatistics::sqlConflictCount)
      .def_ro("execution_timing",
              &statistics::ActionStatistics::executionTiming)
      .def_ro("sql_timing", &statistics::ActionStatistics::sqlTiming)
      .def_ro("action_error_names",
              &statistics::ActionStatistics::actionErrorNames)
      .def_ro("sql_error_codes", &statistics::ActionStatistics::sqlErrorCodes)
      .def("success_rate", &statistics::ActionStatistics::getSuccessRate)
      .def("row_histograms", [](statistics::ActionStatistics const &self) {
        std::map<std::string, std::vector<std::uint64_t>> out;
        for (auto const &[kind, hist] : self.rowHistograms) {
          out[kind] = std::vector<std::uint64_t>(hist.buckets.begin(),
                                                 hist.buckets.end());
        }
        return out;
      });

  nb::class_<statistics::TransactionStatistics>(m, "TransactionStatistics")
      .def_ro("committed", &statistics::TransactionStatistics::committed)
      .def_ro("rolled_back_intentional",
              &statistics::TransactionStatistics::rolledBackIntentional)
      .def_ro("rolled_back_error",
              &statistics::TransactionStatistics::rolledBackError)
      .def_ro("implicit_commits",
              &statistics::TransactionStatistics::implicitCommits)
      .def_ro("savepoint_rollbacks",
              &statistics::TransactionStatistics::savepointRollbacks)
      .def_ro("sub_actions_ok",
              &statistics::TransactionStatistics::subActionsOk)
      .def_ro("sub_actions_fail",
              &statistics::TransactionStatistics::subActionsFail)
      .def("total", &statistics::TransactionStatistics::total)
      .def("has_data", &statistics::TransactionStatistics::hasData)
      .def("sub_histogram", [](statistics::TransactionStatistics const &self) {
        return std::vector<std::uint64_t>(self.subActionsPerTxn.buckets.begin(),
                                          self.subActionsPerTxn.buckets.end());
      });

  nb::class_<statistics::WorkerStatistics>(m, "WorkerStatistics")
      .def("report", &statistics::WorkerStatistics::report)
      .def("report_summary", &statistics::WorkerStatistics::reportSummary)
      .def("report_detailed", &statistics::WorkerStatistics::reportDetailed)
      .def("total_action_count",
           &statistics::WorkerStatistics::getTotalActionCount)
      .def("total_success_count",
           &statistics::WorkerStatistics::getTotalSuccessCount)
      .def("total_failure_count",
           &statistics::WorkerStatistics::getTotalFailureCount)
      .def("has_action",
           [](statistics::WorkerStatistics const &self,
              std::string const &name) {
             return self.actionStats.contains(name);
           })
      .def("action_success_count",
           [](statistics::WorkerStatistics const &self,
              std::string const &name) -> std::uint64_t {
             auto it = self.actionStats.find(name);
             return it == self.actionStats.end() ? 0 : it->second.successCount;
           })
      .def("action_failure_count",
           [](statistics::WorkerStatistics const &self,
              std::string const &name) -> std::uint64_t {
             auto it = self.actionStats.find(name);
             return it == self.actionStats.end()
                        ? 0
                        : it->second.getTotalFailureCount();
           })
      .def("action_conflict_count",
           [](statistics::WorkerStatistics const &self,
              std::string const &name) -> std::uint64_t {
             auto it = self.actionStats.find(name);
             return it == self.actionStats.end() ? 0
                                                 : it->second.sqlConflictCount;
           })
      .def("action_names",
           [](statistics::WorkerStatistics const &self) {
             std::vector<std::string> names;
             names.reserve(self.actionStats.size());
             for (auto const &[name, stats] : self.actionStats) {
               names.push_back(name);
             }
             return names;
           })
      .def("action_stats",
           [](statistics::WorkerStatistics const &self, std::string const &name)
               -> std::optional<statistics::ActionStatistics> {
             auto it = self.actionStats.find(name);
             if (it == self.actionStats.end()) {
               return std::nullopt;
             }
             return it->second;
           })
      .def("transaction_stats", [](statistics::WorkerStatistics const &self) {
        return self.txnStats;
      });

  // --- Workers ---

  using sql_connector_t = Worker::sql_connector_t;

  nb::class_<Worker>(m, "Worker")
      .def(nb::init<std::string const &, sql_connector_t const &,
                    WorkloadParams const &, metadata_ptr>())
      .def("create_random_tables", &Worker::create_random_tables)
      .def("discover_existing_schema", &Worker::discover_existing_schema)
      .def("reset_metadata", &Worker::reset_metadata)
      .def("validate_metadata", &Worker::validate_metadata)
      .def("calculate_database_checksums",
           &Worker::calculate_database_checksums)
      .def("reconnect", &Worker::reconnect);

  nb::class_<RandomWorker, Worker>(m, "RandomWorker")
      .def(nb::init<std::string const &, sql_connector_t const &,
                    WorkloadParams const &, metadata_ptr,
                    action::ActionRegistry const &>())
      // blocking (or thread-starting) calls must release the thread state:
      // free-threaded cyclic GC stops the world by waiting on all attached
      // threads, and an attached thread parked in pthread_join would
      // deadlock against workers that need to attach for python actions
      .def("run_thread", &RandomWorker::run_thread,
           nb::call_guard<nb::gil_scoped_release>())
      .def("join", &RandomWorker::join,
           nb::call_guard<nb::gil_scoped_release>())
      .def("possible_actions", &RandomWorker::possibleActions,
           nb::rv_policy::reference_internal)
      .def("statistics", &RandomWorker::statistics,
           nb::rv_policy::reference_internal);
}
