#include <nanobind/nanobind.h>
#include <nanobind/stl/function.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>

#include <spdlog/spdlog.h>

#include "action/action_registry.hpp"
#include "logging.hpp"
#include "metadata.hpp"
#include "py_action.hpp"
#include "random.hpp"
#include "sql_variant/generic.hpp"
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

NB_MODULE(_stormweaver, m) {
  m.attr("__version__") = "0.1.0";

  // --- Logging ---

  m.def(
      "init_core_logging",
      [](std::string const &log_dir,
         std::function<void(int, std::string const &)> sink, int level) {
        logging::set_log_dir(log_dir);
        logging::set_python_sink(std::move(sink));
        logging::set_level(level);
      },
      nb::arg("log_dir"), nb::arg("sink"), nb::arg("level"));

  m.def("shutdown_core_logging", []() {
    // drop the python callable while the interpreter is still alive
    logging::set_python_sink([](int, std::string const &) {});
  });

  // test hook: emit through the core default logger like spdlog::info does
  m.def("_core_log", [](int level, std::string const &msg) {
    spdlog::default_logger()->log(static_cast<spdlog::level::level_enum>(level),
                                  msg);
  });

  // --- SQL Layer ---

  nb::class_<QueryResult>(m, "QueryResult")
      .def("success", &QueryResult::success)
      .def_ro("query", &QueryResult::query)
      .def_ro("affected_rows", &QueryResult::affectedRows)
      .def_prop_ro("error_code",
                   [](const QueryResult &r) { return r.errorInfo.errorCode; })
      .def_prop_ro("error_message", [](const QueryResult &r) {
        return r.errorInfo.errorMessage;
      });

  nb::class_<LoggedSQL>(m, "LoggedSQL")
      .def("execute", &LoggedSQL::executeQuery)
      .def("reconnect", &LoggedSQL::reconnect);

  m.def("connect_pg", &connect_pg, nb::arg("host") = "localhost",
        nb::arg("port") = 5432, nb::arg("dbname") = "postgres",
        nb::arg("user") = "postgres", nb::arg("password") = "",
        nb::arg("log_name") = "python");

  // --- Metadata ---

  nb::class_<metadata::Metadata>(m, "Metadata")
      .def(nb::init<>())
      .def("size", &metadata::Metadata::size)
      .def("reset", &metadata::Metadata::reset)
      .def("table_names", [](metadata::Metadata const &self) {
        std::vector<std::string> names;
        for (metadata::Metadata::index_t i = 0; i < self.size(); ++i) {
          auto table = self[i];
          if (table) {
            names.push_back(table->name);
          }
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
      .def("make_custom_sql", &action::ActionRegistry::makeCustomSqlAction)
      .def("make_custom_table_sql",
           &action::ActionRegistry::makeCustomTableSqlAction)
      .def("use", &action::ActionRegistry::use)
      .def(
          "register_python",
          [](action::ActionRegistry &registry, std::string const &name,
             std::size_t weight, nb::object fn) {
            auto shared_fn = make_py_action_fn(std::move(fn));
            registry.insert(action::ActionFactory{
                .name = name,
                .builder =
                    [shared_fn](action::AllConfig const &) {
                      return std::make_unique<PyCallableAction>(shared_fn);
                    },
                .weight = weight});
          },
          nb::arg("name"), nb::arg("weight"), nb::arg("fn"));

  m.def("default_action_registry", &action::default_registy,
        nb::rv_policy::reference);

  // --- Configs ---

  nb::class_<action::DdlConfig>(m, "DdlConfig")
      .def(nb::init<>())
      .def_rw("min_table_count", &action::DdlConfig::min_table_count)
      .def_rw("max_table_count", &action::DdlConfig::max_table_count)
      .def_rw("max_column_count", &action::DdlConfig::max_column_count)
      .def_rw("access_methods", &action::DdlConfig::access_methods);

  nb::class_<action::DmlConfig>(m, "DmlConfig")
      .def(nb::init<>())
      .def_rw("delete_min", &action::DmlConfig::deleteMin)
      .def_rw("delete_max", &action::DmlConfig::deleteMax);

  nb::class_<action::AllConfig>(m, "AllConfig")
      .def(nb::init<>())
      .def_rw("ddl", &action::AllConfig::ddl)
      .def_rw("dml", &action::AllConfig::dml);

  nb::class_<WorkloadParams>(m, "WorkloadParams")
      .def(nb::init<>())
      .def_rw("action_config", &WorkloadParams::actionConfig)
      .def_rw("duration_in_seconds", &WorkloadParams::duration_in_seconds)
      .def_rw("repeat_times", &WorkloadParams::repeat_times)
      .def_rw("number_of_workers", &WorkloadParams::number_of_workers)
      .def_rw("max_reconnect_attempts", &WorkloadParams::max_reconnect_attempts)
      .def_rw("seed", &WorkloadParams::seed);

  // --- Statistics ---

  nb::class_<statistics::TimingStatistics>(m, "TimingStatistics")
      .def("avg_ms", &statistics::TimingStatistics::getAverageMs)
      .def("min_ms", &statistics::TimingStatistics::getMinMs)
      .def("max_ms", &statistics::TimingStatistics::getMaxMs)
      .def_ro("count", &statistics::TimingStatistics::count)
      .def("has_data", &statistics::TimingStatistics::hasData);

  nb::class_<statistics::ActionStatistics>(m, "ActionStatistics")
      .def_ro("success_count", &statistics::ActionStatistics::successCount)
      .def_ro("action_failure_count",
              &statistics::ActionStatistics::actionFailureCount)
      .def_ro("sql_failure_count",
              &statistics::ActionStatistics::sqlFailureCount)
      .def_ro("other_failure_count",
              &statistics::ActionStatistics::otherFailureCount)
      .def_ro("execution_timing",
              &statistics::ActionStatistics::executionTiming)
      .def_ro("sql_timing", &statistics::ActionStatistics::sqlTiming);

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
