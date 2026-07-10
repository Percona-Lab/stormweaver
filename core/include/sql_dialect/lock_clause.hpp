#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sql_dialect {

enum class LockClause : std::uint8_t { none, forUpdate, forShare };

[[nodiscard]] constexpr std::string_view lockClauseSuffix(LockClause lock) {
  switch (lock) {
  case LockClause::forUpdate:
    return " FOR UPDATE";
  case LockClause::forShare:
    return " FOR SHARE";
  case LockClause::none:
    break;
  }
  return "";
}

// " FOR UPDATE OF <alias>" / " FOR SHARE OF <alias>"; "" for none
[[nodiscard]] inline std::string lockOfSuffix(LockClause lock,
                                              std::string_view alias) {
  std::string out{lockClauseSuffix(lock)};
  if (!out.empty()) {
    out += " OF ";
    out += alias;
  }
  return out;
}

} // namespace sql_dialect
