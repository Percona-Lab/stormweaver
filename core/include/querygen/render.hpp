#pragma once

#include "querygen/ir.hpp"
#include "sql_dialect/dialect.hpp"

#include <string>

namespace querygen {

[[nodiscard]] std::string render(Expr const &e,
                                 sql_dialect::Dialect const &dialect);
[[nodiscard]] std::string render(QuerySpec const &q,
                                 sql_dialect::Dialect const &dialect);

} // namespace querygen
