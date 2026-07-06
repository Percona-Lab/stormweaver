
#pragma once

#include "sql_variant/mysql.hpp"
#include "sql_variant/postgresql.hpp"

namespace sql_variant {

std::unique_ptr<GenericSQL> connect(const std::string &serverType,
                                    const ServerParams &params);

} // namespace sql_variant
