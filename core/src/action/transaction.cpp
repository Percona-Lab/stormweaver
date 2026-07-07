#include "action/transaction.hpp"
#include "action/helper.hpp"
#include "metadata/context.hpp"
#include "sql_dialect/dialect.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <utility>
#include <vector>

using namespace action;

namespace {

sql_dialect::IsolationLevel pickIsolation(ps_random &rand,
                                          IsolationWeights const &w) {
  const std::size_t total =
      w.server_default + w.read_committed + w.repeatable_read + w.serializable;
  if (total == 0) {
    return sql_dialect::IsolationLevel::serverDefault;
  }
  auto roll = rand.random_number<std::size_t>(1, total);
  if (roll <= w.server_default) {
    return sql_dialect::IsolationLevel::serverDefault;
  }
  roll -= w.server_default;
  if (roll <= w.read_committed) {
    return sql_dialect::IsolationLevel::readCommitted;
  }
  roll -= w.read_committed;
  if (roll <= w.repeatable_read) {
    return sql_dialect::IsolationLevel::repeatableRead;
  }
  return sql_dialect::IsolationLevel::serializable;
}

struct Savepoint {
  std::size_t name; // spN
  std::size_t mark; // TxnBuffer position
};

} // namespace

TransactionAction::TransactionAction(AllConfig config,
                                     ActionRegistry const &pool)
    : allConfig(std::move(config)),
      poolAll(pool.filtered([](ActionFactory const &f) {
        return f.type != ActionType::transaction;
      })),
      poolNoDdl(pool.filtered([](ActionFactory const &f) {
        return f.type != ActionType::transaction && f.type != ActionType::ddl;
      })) {}

void TransactionAction::execute(metadata::Context &metaCtx, ps_random &rand,
                                sql_variant::LoggedSQL *connection) const {
  auto const &config = allConfig.transaction;
  auto const serverInfo = connection->serverInfo();
  auto const &dialect = sql_dialect::dialect_for(serverInfo);

  const bool ddlTransactional = dialect.transactionalDDL();
  const bool excludeDdl =
      !ddlTransactional &&
      config.mysql_ddl_mode == TransactionConfig::MysqlDdlMode::exclude;
  ActionRegistry const &pool = excludeDdl ? poolNoDdl : poolAll;
  if (pool.size() == 0) {
    return;
  }

  const auto isolation = pickIsolation(rand, config.isolation_weights);
  for (auto const &stmt : dialect.beginStatements(isolation)) {
    connection->executeQuery(stmt).maybeThrow();
  }

  metadata::TxnBuffer<metadata::Table> txn;
  metadata::Context trxCtx(metaCtx.registry(), &txn);
  auto &globalTables = metaCtx.registry().get<metadata::Table>();

  TxGuard guard(connection);

  const bool useSavepoints =
      config.error_mode == TransactionConfig::ErrorMode::savepoint;
  const auto subCount = rand.random_number<std::size_t>(config.min_sub_actions,
                                                        config.max_sub_actions);

  bool inTrx = true; // false after a mysql implicit commit
  std::vector<Savepoint> savepoints;
  std::size_t spCounter = 0;
  std::size_t okCount = 0;
  std::size_t failCount = 0;

  for (std::size_t i = 0; i < subCount; ++i) {
    const auto w = rand.random_number<std::size_t>(0, pool.totalWeight());
    const auto factory = pool.lookupByWeightOffset(w);
    auto sub =
        factory.builder(BuildContext{.config = allConfig, .registry = pool});

    if (inTrx && !ddlTransactional && factory.type == ActionType::ddl) {
      // mysql mirror mode: DDL implicitly commits the open transaction
      // (even when the DDL itself then fails) - but only if a statement
      // actually reaches the server. no-op actions (count limits, empty
      // picks) must leave the transaction untouched, so gate the flush on
      // the connection's query counter. the sub runs against the direct
      // context: anything it publishes is durable (DDL self-commits).
      // no savepoint wrapping (savepoints die on the implicit commit) and
      // failures are counted, not rethrown, even in abort mode: the
      // transaction either already ended or was never touched.
      const auto queriesBefore = connection->getQueryCount();
      try {
        sub->execute(metaCtx, rand, connection);
        ++okCount;
      } catch (sql_variant::SqlException const &e) {
        if (e.serverGone()) {
          guard.disarm();
          throw;
        }
        ++failCount;
      } catch (ActionException const &) {
        ++failCount;
      }
      if (connection->getQueryCount() > queriesBefore) {
        // implicit commit happened: buffered work is durable now, the
        // rest of this action runs in autocommit
        txn.publishAll(globalTables);
        inTrx = false;
        guard.disarm();
        savepoints.clear();
      }
      continue;
    }

    if (!inTrx) {
      // autocommit tail: publish directly via the caller's context
      try {
        sub->execute(metaCtx, rand, connection);
        ++okCount;
      } catch (sql_variant::SqlException const &e) {
        if (e.serverGone()) {
          throw;
        }
        ++failCount;
      } catch (ActionException const &) {
        ++failCount;
      }
      continue;
    }

    if (useSavepoints) {
      const auto sp = ++spCounter;
      connection->executeQuery(fmt::format("SAVEPOINT sp{};", sp)).maybeThrow();
      savepoints.push_back({.name = sp, .mark = txn.mark()});
      try {
        sub->execute(trxCtx, rand, connection);
        ++okCount;
        // pstress-style: sometimes rewind a chunk of the transaction
        if (rand.random_number<std::size_t>(1, 100) <=
            config.rollback_to_savepoint_probability) {
          const auto pick =
              rand.random_number<std::size_t>(0, savepoints.size() - 1);
          connection
              ->executeQuery(fmt::format("ROLLBACK TO SAVEPOINT sp{};",
                                         savepoints[pick].name))
              .maybeThrow();
          txn.rollbackTo(savepoints[pick].mark, globalTables);
          // ROLLBACK TO destroys the later savepoints, keeps the target
          savepoints.resize(pick + 1);
        }
      } catch (sql_variant::SqlException const &e) {
        if (e.serverGone()) {
          guard.disarm(); // connection is gone, nothing to roll back on it
          throw;
        }
        if (e.errorClass() == sql_variant::ErrorClass::failedTxn) {
          // 25P02 here means a statement ran inside an already-aborted
          // transaction - savepoint recovery has a hole
          spdlog::error("transaction: statement executed in aborted trx: {}",
                        e.what());
        }
        connection
            ->executeQuery(fmt::format("ROLLBACK TO SAVEPOINT sp{};",
                                       savepoints.back().name))
            .maybeThrow();
        txn.rollbackTo(savepoints.back().mark, globalTables);
        ++failCount;
      } catch (ActionException const &) {
        // no SQL failed (e.g. empty-metadata skip); rewind for uniformity
        connection
            ->executeQuery(fmt::format("ROLLBACK TO SAVEPOINT sp{};",
                                       savepoints.back().name))
            .maybeThrow();
        txn.rollbackTo(savepoints.back().mark, globalTables);
        ++failCount;
      }
    } else { // abort mode: first failure kills the whole transaction
      try {
        sub->execute(trxCtx, rand, connection);
        ++okCount;
      } catch (sql_variant::SqlException const &e) {
        if (e.serverGone()) {
          guard.disarm();
        }
        if (e.errorClass() == sql_variant::ErrorClass::failedTxn) {
          // 25P02 here means a statement ran inside an already-aborted
          // transaction - savepoint recovery has a hole
          spdlog::error("transaction: statement executed in aborted trx: {}",
                        e.what());
        }
        throw; // guard rolls back; buffer dies with txn; worker records it
      }
      // ActionException intentionally not caught: same rethrow semantics,
      // guard performs the rollback
    }
  }

  if (inTrx) {
    if (rand.random_number<std::size_t>(1, 100) <= config.commit_probability) {
      auto res = connection->executeQuery("COMMIT;");
      guard.disarm();   // success or failure, the transaction is over
      res.maybeThrow(); // commit-time conflict: buffer discarded, reported
      txn.publishAll(globalTables);
    } else {
      guard.disarm();
      connection->executeQuery("ROLLBACK;").maybeThrow();
    }
  }

  spdlog::debug("transaction: {} sub-actions, {} ok, {} failed, in_trx={}",
                subCount, okCount, failCount, inTrx);
}
