#pragma once

#include <cstddef>

namespace querygen {

struct QueryGenConfig {
  std::size_t max_joins = 3;
  std::size_t max_expr_depth = 4;
  std::size_t max_subquery_depth = 2;
  // percent probabilities
  std::size_t join_prob = 60;
  std::size_t subquery_prob = 30;
  std::size_t aggregate_prob = 25;
  std::size_t cte_prob = 10;
  std::size_t setop_prob = 10;
  std::size_t window_prob = 10;
  std::size_t order_by_prob = 30;
  std::size_t limit_prob = 40;
  std::size_t correlation_prob = 40;
  // chance existing DML actions use the generator instead of the
  // legacy pk-subquery / randomRowsSelect forms
  std::size_t dml_predicate_prob = 30;
  std::size_t dml_pk_select_prob = 30;
};

} // namespace querygen
