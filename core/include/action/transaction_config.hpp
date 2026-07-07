#pragma once

#include <cstddef>
#include <cstdint>

namespace action {

struct IsolationWeights {
  std::size_t server_default = 1;
  std::size_t read_committed = 1;
  std::size_t repeatable_read = 1;
  std::size_t serializable = 1;
};

struct TransactionConfig {
  enum class ErrorMode : std::uint8_t { savepoint, abort };
  enum class MysqlDdlMode : std::uint8_t { mirror, exclude };

  std::size_t min_sub_actions = 2;
  std::size_t max_sub_actions = 10;
  std::size_t commit_probability = 95;                // percent
  std::size_t rollback_to_savepoint_probability = 10; // percent, per success
  ErrorMode error_mode = ErrorMode::savepoint;
  MysqlDdlMode mysql_ddl_mode = MysqlDdlMode::mirror;
  IsolationWeights isolation_weights;
};

} // namespace action
