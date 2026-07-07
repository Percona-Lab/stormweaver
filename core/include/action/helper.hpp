#pragma once

#include "action/action.hpp"
#include "sql_variant/generic.hpp"

#include <tuple>

namespace action {

// Throws ActionException("empty-metadata") when there are no tables.
metadata::table_cptr find_random_table(metadata::Context const &metaCtx,
                                       ps_random &rand);

/* rolls the server-side transaction back unless disarmed. workers reuse
   their connection for every subsequent action; leaking an open
   transaction would poison all of them. */
class TxGuard {
public:
  explicit TxGuard(sql_variant::LoggedSQL *conn) : conn(conn) {}
  TxGuard(TxGuard const &) = delete;
  TxGuard &operator=(TxGuard const &) = delete;
  void disarm() { armed = false; }
  ~TxGuard() {
    if (armed) {
      // best effort; must never throw during unwind
      try {
        std::ignore = conn->executeQuery("ROLLBACK;");
      } catch (...) { // NOLINT(bugprone-empty-catch)
      }
    }
  }

private:
  sql_variant::LoggedSQL *conn;
  bool armed = true;
};

} // namespace action
