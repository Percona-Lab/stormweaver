#pragma once

#include "metadata/context.hpp"
#include "querygen/config.hpp"
#include "querygen/ir.hpp"
#include "random.hpp"
#include "sql_variant/generic.hpp"

#include <optional>
#include <string_view>

namespace querygen {

enum class Purpose : std::uint8_t { standalone, pkSelect, predicate };

/* One-shot: construct, call generate()/generatePredicate() once. Draws
   all randomness up front from the caller's stream; no server IO. */
class Generator {
public:
  Generator(metadata::Context const &ctx, ps_random &rand,
            QueryGenConfig const &cfg, sql_variant::ServerInfo const &server);

  // nullopt when the catalog has no usable tables
  std::optional<QuerySpec> generate(Purpose purpose,
                                    metadata::table_cptr target);

  struct PkSelectOpts {
    std::size_t limit = 0; // 0 = no limit
    sql_dialect::LockClause lock = sql_dialect::LockClause::none;
  };
  // pk-returning query over target, safe for WHERE pk IN (...)
  std::optional<QuerySpec> generatePkSelect(metadata::table_cptr target,
                                            PkSelectOpts const &opts);

  // boolean expr over `target` qualified with `qualifier` (table name in
  // UPDATE/DELETE, alias inside generated queries); subqueries against
  // other tables allowed
  Expr generatePredicate(metadata::table_cptr target,
                         std::string_view qualifier);

private:
  struct ScopeEntry {
    std::string alias;
    metadata::table_cptr table;
  };
  using Scope = std::vector<ScopeEntry>;

  Expr genBool(Scope const &scope, std::size_t depth, std::size_t subqDepth,
               bool allowAgg);
  Expr genScalar(metadata::ColumnType family, Scope const &scope,
                 std::size_t depth, bool allowAgg);
  Expr genLeaf(metadata::ColumnType family, Scope const &scope);
  Expr genLiteral(metadata::ColumnType type, std::size_t length);
  std::optional<Expr> pickColumn(Scope const &scope,
                                 metadata::ColumnType family);

  Expr genSubqueryBool(Scope const &scope, std::size_t subqDepth);
  Expr genAggCall(Scope const &scope);
  Expr genWindowCall(Scope const &scope);
  static void trimToSingleItem(QuerySpec &q, std::size_t keep);

  QuerySpec genQuery(Purpose purpose, metadata::table_cptr target,
                     Scope const &outer, std::size_t subqDepth,
                     bool allowExtras); // Task 7

  bool roll(std::size_t percent);
  std::string freshAlias();

  // ctx_/server_ kept for later stages (snapshot refresh, feature gates)
  [[maybe_unused]] metadata::Context const &ctx_;
  ps_random &rand_;
  QueryGenConfig const &cfg_;
  [[maybe_unused]] sql_variant::ServerInfo server_;
  std::vector<metadata::table_cptr> tables_; // snapshotAll, id-sorted
  std::size_t aliasCounter_ = 0;
};

} // namespace querygen
