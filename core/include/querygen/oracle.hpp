#pragma once

#include "querygen/ir.hpp"

#include <vector>

namespace querygen {

// result rows of one variant execution; exact shape decided when the
// first oracle lands (phase 2)
struct QueryResult;

/* Metamorphic oracle: derive variant queries from a base query, then
   judge the combined results. Implementations (TLP, NoREC) are pure
   IR transforms - QuerySpec is a value type precisely for this. */
struct Oracle {
  virtual ~Oracle() = default;
  [[nodiscard]] virtual std::vector<QuerySpec>
  variants(QuerySpec const &base) const = 0;
  [[nodiscard]] virtual bool
  verdict(std::vector<QueryResult> const &results) const = 0;
};

} // namespace querygen
