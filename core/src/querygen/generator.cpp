#include "querygen/generator.hpp"

#include "sql_dialect/dialect.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>

namespace querygen {

namespace {

enum class Family : std::uint8_t { numeric, string, boolean, binary };

Family familyOf(metadata::ColumnType t) {
  using metadata::ColumnType;
  switch (t) {
  case ColumnType::INT:
  case ColumnType::REAL:
    return Family::numeric;
  case ColumnType::CHAR:
  case ColumnType::VARCHAR:
  case ColumnType::TEXT:
    return Family::string;
  case ColumnType::BOOL:
    return Family::boolean;
  case ColumnType::BYTEA:
    return Family::binary;
  }
  return Family::numeric;
}

metadata::ColumnType representative(Family f) {
  using metadata::ColumnType;
  switch (f) {
  case Family::numeric:
    return ColumnType::INT;
  case Family::string:
    return ColumnType::VARCHAR;
  case Family::boolean:
    return ColumnType::BOOL;
  case Family::binary:
    return ColumnType::BYTEA;
  }
  return ColumnType::INT;
}

Expr colExpr(std::string alias, metadata::Column const &col) {
  return Expr{.node = ColumnRef{.alias = std::move(alias), .column = col.name},
              .type = col.type};
}

metadata::Column const *pkColumn(metadata::Table const &table) {
  for (auto const &col : table.columns) {
    if (col.primary_key) {
      return &col;
    }
  }
  return table.columns.empty() ? nullptr : table.columns.data();
}

Expr wrapNullifZero(Expr rhs) {
  FuncCall guard{.fn = Func::nullif, .args = {}};
  auto type = rhs.type;
  guard.args.emplace_back(std::move(rhs));
  guard.args.emplace_back(Expr{.node = Literal{std::int64_t{0}},
                               .type = metadata::ColumnType::INT});
  return Expr{.node = std::move(guard), .type = type};
}

} // namespace

Generator::Generator(metadata::Context const &ctx, ps_random &rand,
                     QueryGenConfig const &cfg,
                     sql_variant::ServerInfo const &server)
    : ctx_(ctx), rand_(rand), cfg_(cfg), server_(server),
      tables_(ctx.get<metadata::Table>().snapshotAll()) {}

bool Generator::roll(std::size_t percent) {
  return rand_.random_number<std::size_t>(1, 100) <= percent;
}

std::string Generator::freshAlias() {
  return fmt::format("u{}", aliasCounter_++);
}

std::optional<Expr> Generator::pickColumn(Scope const &scope,
                                          metadata::ColumnType family) {
  struct Cand {
    std::string const *alias;
    metadata::Column const *col;
  };
  std::vector<Cand> cands;
  for (auto const &entry : scope) {
    for (auto const &col : entry.table->columns) {
      if (familyOf(col.type) == familyOf(family)) {
        cands.push_back({.alias = &entry.alias, .col = &col});
      }
    }
  }
  if (cands.empty()) {
    return std::nullopt;
  }
  auto const &pick =
      cands[rand_.random_number<std::size_t>(0, cands.size() - 1)];
  return colExpr(*pick.alias, *pick.col);
}

Expr Generator::genLiteral(metadata::ColumnType type, std::size_t length) {
  using metadata::ColumnType;
  Literal lit;
  switch (type) {
  case ColumnType::INT:
    lit.value = static_cast<std::int64_t>(rand_.random_number(1, 1000000));
    break;
  case ColumnType::REAL:
    lit.value = rand_.random_number(1.0, 1000000.0);
    break;
  case ColumnType::BOOL:
    lit.value = rand_.random_number(0, 1) == 1;
    break;
  case ColumnType::CHAR:
  case ColumnType::VARCHAR:
  case ColumnType::TEXT:
  case ColumnType::BYTEA: {
    std::size_t const maxLen =
        length == 0 ? 16 : std::min<std::size_t>(length, 16);
    lit.value = rand_.random_string(0, maxLen);
    break;
  }
  }
  return Expr{.node = std::move(lit), .type = type};
}

Expr Generator::genLeaf(metadata::ColumnType family, Scope const &scope) {
  if (roll(70)) {
    if (auto col = pickColumn(scope, family)) {
      return *std::move(col);
    }
  }
  return genLiteral(family, 0);
}

Expr Generator::genScalar(metadata::ColumnType family, Scope const &scope,
                          std::size_t depth, bool allowAgg) {
  Family const fam = familyOf(family);
  if (depth == 0 || fam == Family::boolean || fam == Family::binary) {
    return genLeaf(family, scope);
  }

  enum class Pick : std::uint8_t {
    leaf,
    literal,
    arith,
    mod,
    absFn,
    lengthFn,
    coalesceFn,
    nullifFn,
    caseFn,
    lowerUpper,
    concat
  };
  std::vector<Pick> choices{Pick::leaf,       Pick::leaf,     Pick::literal,
                            Pick::coalesceFn, Pick::nullifFn, Pick::caseFn};
  if (fam == Family::numeric) {
    choices.insert(choices.end(), {Pick::arith, Pick::arith, Pick::mod,
                                   Pick::absFn, Pick::lengthFn});
  }
  if (fam == Family::string) {
    choices.insert(choices.end(), {Pick::lowerUpper, Pick::concat});
  }
  auto const pick =
      choices[rand_.random_number<std::size_t>(0, choices.size() - 1)];

  switch (pick) {
  case Pick::leaf:
    return genLeaf(family, scope);
  case Pick::literal:
    return genLiteral(family, 0);
  case Pick::arith: {
    std::array const ops{BinOp::add, BinOp::sub, BinOp::mul, BinOp::div_};
    auto const op = ops[rand_.random_number<std::size_t>(0, ops.size() - 1)];
    if (op == BinOp::mul) {
      // int4 overflow guard: products stay bounded when one side is a
      // leaf (column/literal <= 1e6) and the other a tiny literal
      Expr lhs = genLeaf(family, scope);
      Expr rhs{.node = Literal{static_cast<std::int64_t>(
                   rand_.random_number(1, 10))},
               .type = metadata::ColumnType::INT};
      return Expr{.node = BinaryExpr{.op = op,
                                     .lhs = box<Expr>(std::move(lhs)),
                                     .rhs = box<Expr>(std::move(rhs))},
                  .type = family};
    }
    Expr lhs = genScalar(family, scope, depth - 1, allowAgg);
    Expr rhs = genScalar(family, scope, depth - 1, allowAgg);
    if (op == BinOp::div_) {
      rhs = wrapNullifZero(std::move(rhs));
    }
    return Expr{.node = BinaryExpr{.op = op,
                                   .lhs = box<Expr>(std::move(lhs)),
                                   .rhs = box<Expr>(std::move(rhs))},
                .type = family};
  }
  case Pick::mod: {
    // pg % is integer-only: both operands exact-INT columns or literals
    auto intLeaf = [&]() -> Expr {
      if (roll(70)) {
        std::vector<std::pair<std::string const *, metadata::Column const *>>
            ints;
        for (auto const &entry : scope) {
          for (auto const &col : entry.table->columns) {
            if (col.type == metadata::ColumnType::INT) {
              ints.emplace_back(&entry.alias, &col);
            }
          }
        }
        if (!ints.empty()) {
          auto const &[alias, col] =
              ints[rand_.random_number<std::size_t>(0, ints.size() - 1)];
          return colExpr(*alias, *col);
        }
      }
      return genLiteral(metadata::ColumnType::INT, 0);
    };
    Expr lhs = intLeaf();
    Expr rhs = wrapNullifZero(intLeaf());
    return Expr{.node = BinaryExpr{.op = BinOp::mod,
                                   .lhs = box<Expr>(std::move(lhs)),
                                   .rhs = box<Expr>(std::move(rhs))},
                .type = metadata::ColumnType::INT};
  }
  case Pick::absFn: {
    FuncCall fn{.fn = Func::abs, .args = {}};
    fn.args.emplace_back(genScalar(family, scope, depth - 1, allowAgg));
    return Expr{.node = std::move(fn), .type = family};
  }
  case Pick::lengthFn: {
    FuncCall fn{.fn = Func::length, .args = {}};
    fn.args.emplace_back(genLeaf(metadata::ColumnType::VARCHAR, scope));
    return Expr{.node = std::move(fn), .type = metadata::ColumnType::INT};
  }
  case Pick::coalesceFn: {
    FuncCall fn{.fn = Func::coalesce, .args = {}};
    fn.args.emplace_back(genLeaf(family, scope));
    fn.args.emplace_back(genLiteral(family, 0));
    return Expr{.node = std::move(fn), .type = family};
  }
  case Pick::nullifFn: {
    FuncCall fn{.fn = Func::nullif, .args = {}};
    fn.args.emplace_back(genLeaf(family, scope));
    fn.args.emplace_back(genLiteral(family, 0));
    return Expr{.node = std::move(fn), .type = family};
  }
  case Pick::caseFn: {
    CaseExpr ce;
    auto const whens = rand_.random_number<std::size_t>(1, 2);
    for (std::size_t i = 0; i < whens; ++i) {
      // no fresh subquery budget inside CASE conditions
      ce.whens.push_back(
          {.when = box<Expr>(
               genBool(scope, depth - 1, cfg_.max_subquery_depth, allowAgg)),
           .then = box<Expr>(genScalar(family, scope, depth - 1, allowAgg))});
    }
    ce.elseExpr = box<Expr>(genScalar(family, scope, depth - 1, allowAgg));
    return Expr{.node = std::move(ce), .type = family};
  }
  case Pick::lowerUpper: {
    FuncCall fn{.fn = rand_.random_bool() ? Func::lower : Func::upper,
                .args = {}};
    fn.args.emplace_back(genScalar(family, scope, depth - 1, allowAgg));
    return Expr{.node = std::move(fn), .type = family};
  }
  case Pick::concat: {
    return Expr{.node = BinaryExpr{.op = BinOp::concat,
                                   .lhs = box<Expr>(genScalar(
                                       family, scope, depth - 1, allowAgg)),
                                   .rhs = box<Expr>(genScalar(
                                       family, scope, depth - 1, allowAgg))},
                .type = family};
  }
  }
  return genLeaf(family, scope);
}

// keep exactly one select item; IN/scalar operands also lose ORDER BY/
// LIMIT/OFFSET - mysql rejects LIMIT inside IN subqueries
void Generator::trimToSingleItem(QuerySpec &q, std::size_t keep) {
  SelectItem item = std::move(q.selectItems[keep]);
  item.colAlias = "c0";
  q.selectItems.clear();
  q.selectItems.push_back(std::move(item));
  q.orderBy.clear();
  q.limit.reset();
  q.offset.reset();
}

Expr Generator::genSubqueryBool(Scope const &scope, std::size_t subqDepth) {
  QuerySpec sub =
      genQuery(Purpose::standalone, nullptr, scope, subqDepth + 1, false);
  bool const negated = rand_.random_bool();
  if (rand_.random_bool()) {
    return Expr{.node = ExistsSubquery{.sub = box<QuerySpec>(std::move(sub)),
                                       .negated = negated},
                .type = metadata::ColumnType::BOOL};
  }
  trimToSingleItem(
      sub, rand_.random_number<std::size_t>(0, sub.selectItems.size() - 1));
  auto const family = sub.selectItems[0].expr.type;
  Expr arg = genLeaf(family, scope);
  return Expr{.node = InSubquery{.arg = box<Expr>(std::move(arg)),
                                 .sub = box<QuerySpec>(std::move(sub)),
                                 .negated = negated},
              .type = metadata::ColumnType::BOOL};
}

Expr Generator::genBool(Scope const &scope, std::size_t depth,
                        std::size_t subqDepth, bool allowAgg) {
  using metadata::ColumnType;

  // families with at least one column in scope
  std::vector<Family> families;
  for (auto const &entry : scope) {
    for (auto const &col : entry.table->columns) {
      auto const fam = familyOf(col.type);
      if (std::ranges::find(families, fam) == families.end()) {
        families.push_back(fam);
      }
    }
  }

  enum class Pick : std::uint8_t {
    cmp,
    andOr,
    notFn,
    isNull,
    between,
    inList,
    likeFn,
    subquery,
    leaf
  };
  std::vector<Pick> choices{Pick::cmp,    Pick::cmp,    Pick::cmp, Pick::cmp,
                            Pick::isNull, Pick::inList, Pick::leaf};
  if (depth > 0) {
    choices.insert(choices.end(),
                   {Pick::andOr, Pick::andOr, Pick::andOr, Pick::notFn});
  }
  if (std::ranges::find(families, Family::numeric) != families.end()) {
    choices.push_back(Pick::between);
  }
  if (std::ranges::find(families, Family::string) != families.end()) {
    choices.push_back(Pick::likeFn);
  }
  if (!tables_.empty() && subqDepth < cfg_.max_subquery_depth &&
      roll(cfg_.subquery_prob)) {
    choices.insert(choices.end(), {Pick::subquery, Pick::subquery});
  }
  auto const pick =
      choices[rand_.random_number<std::size_t>(0, choices.size() - 1)];

  switch (pick) {
  case Pick::cmp: {
    auto const fam = families.empty()
                         ? Family::numeric
                         : families[rand_.random_number<std::size_t>(
                               0, families.size() - 1)];
    auto const rep = representative(fam);
    bool const leafOnly = fam == Family::boolean || fam == Family::binary;
    std::array const cmpOps{BinOp::eq, BinOp::ne, BinOp::lt,
                            BinOp::le, BinOp::gt, BinOp::ge};
    BinOp op{};
    if (leafOnly) {
      op = rand_.random_bool() ? BinOp::eq : BinOp::ne;
    } else {
      op = cmpOps[rand_.random_number<std::size_t>(0, cmpOps.size() - 1)];
    }
    auto const childDepth = depth > 0 ? depth - 1 : 0;
    Expr lhs = genScalar(rep, scope, childDepth, allowAgg);
    Expr rhs;
    bool haveRhs = false;
    if (!leafOnly && !tables_.empty() && subqDepth < cfg_.max_subquery_depth &&
        roll(cfg_.subquery_prob)) {
      // scalar subquery operand: one select item of the compared family,
      // LIMIT 1 caps it to a single row
      QuerySpec sub =
          genQuery(Purpose::standalone, nullptr, scope, subqDepth + 1, false);
      std::vector<std::size_t> matching;
      for (std::size_t i = 0; i < sub.selectItems.size(); ++i) {
        if (familyOf(sub.selectItems[i].expr.type) == fam) {
          matching.push_back(i);
        }
      }
      if (!matching.empty()) {
        trimToSingleItem(
            sub,
            matching[rand_.random_number<std::size_t>(0, matching.size() - 1)]);
        sub.limit = 1;
        rhs =
            Expr{.node = ScalarSubquery{.sub = box<QuerySpec>(std::move(sub))},
                 .type = rep};
        haveRhs = true;
      }
    }
    if (!haveRhs) {
      rhs = genScalar(rep, scope, childDepth, allowAgg);
    }
    return Expr{.node = BinaryExpr{.op = op,
                                   .lhs = box<Expr>(std::move(lhs)),
                                   .rhs = box<Expr>(std::move(rhs))},
                .type = ColumnType::BOOL};
  }
  case Pick::andOr: {
    return Expr{
        .node = BinaryExpr{.op = rand_.random_bool() ? BinOp::and_ : BinOp::or_,
                           .lhs = box<Expr>(
                               genBool(scope, depth - 1, subqDepth, allowAgg)),
                           .rhs = box<Expr>(
                               genBool(scope, depth - 1, subqDepth, allowAgg))},
        .type = ColumnType::BOOL};
  }
  case Pick::notFn: {
    return Expr{.node = UnaryExpr{.op = UnOp::not_,
                                  .arg = box<Expr>(genBool(
                                      scope, depth - 1, subqDepth, allowAgg))},
                .type = ColumnType::BOOL};
  }
  case Pick::isNull: {
    auto arg =
        pickColumn(scope, representative(families.empty() ? Family::numeric
                                                          : families[0]));
    if (families.size() > 1) {
      auto const fam =
          families[rand_.random_number<std::size_t>(0, families.size() - 1)];
      if (auto c = pickColumn(scope, representative(fam))) {
        arg = std::move(c);
      }
    }
    if (!arg) {
      return genLeaf(ColumnType::BOOL, scope);
    }
    return Expr{.node = UnaryExpr{.op = rand_.random_bool() ? UnOp::isNull
                                                            : UnOp::isNotNull,
                                  .arg = box<Expr>(*std::move(arg))},
                .type = ColumnType::BOOL};
  }
  case Pick::between: {
    auto col = pickColumn(scope, ColumnType::INT);
    if (!col) {
      return genLeaf(ColumnType::BOOL, scope);
    }
    auto const type = col->type;
    return Expr{.node = BetweenExpr{.arg = box<Expr>(*std::move(col)),
                                    .lo = box<Expr>(genLiteral(type, 0)),
                                    .hi = box<Expr>(genLiteral(type, 0))},
                .type = ColumnType::BOOL};
  }
  case Pick::inList: {
    auto const fam = families.empty()
                         ? Family::numeric
                         : families[rand_.random_number<std::size_t>(
                               0, families.size() - 1)];
    auto col = pickColumn(scope, representative(fam));
    if (!col) {
      return genLeaf(ColumnType::BOOL, scope);
    }
    auto const type = col->type;
    InListExpr in{.arg = box<Expr>(*std::move(col)),
                  .items = {},
                  .negated = rand_.random_bool()};
    auto const count = rand_.random_number<std::size_t>(1, 5);
    for (std::size_t i = 0; i < count; ++i) {
      in.items.emplace_back(genLiteral(type, 0));
    }
    return Expr{.node = std::move(in), .type = ColumnType::BOOL};
  }
  case Pick::likeFn: {
    auto col = pickColumn(scope, ColumnType::VARCHAR);
    if (!col) {
      return genLeaf(ColumnType::BOOL, scope);
    }
    // quote/backslash-free alphabet: no escaping headaches
    static constexpr std::string_view alphabet =
        "abcdefghijklmnopqrstuvwxyz0123456789%_";
    std::string pattern;
    auto const len = rand_.random_number<std::size_t>(1, 8);
    for (std::size_t i = 0; i < len; ++i) {
      pattern.push_back(
          alphabet[rand_.random_number<std::size_t>(0, alphabet.size() - 1)]);
    }
    return Expr{.node = BinaryExpr{.op = BinOp::like,
                                   .lhs = box<Expr>(*std::move(col)),
                                   .rhs = box<Expr>(
                                       Expr{.node = Literal{std::move(pattern)},
                                            .type = ColumnType::VARCHAR})},
                .type = ColumnType::BOOL};
  }
  case Pick::subquery:
    return genSubqueryBool(scope, subqDepth);
  case Pick::leaf:
    return genLeaf(ColumnType::BOOL, scope);
  }
  return genLeaf(ColumnType::BOOL, scope);
}

QuerySpec Generator::genQuery(Purpose purpose, metadata::table_cptr target,
                              Scope const &outer, std::size_t subqDepth,
                              bool allowExtras) {
  QuerySpec q;

  // from/join candidates; a CTE adds a synthetic table visible only here
  auto cands = tables_;
  if (allowExtras && roll(cfg_.cte_prob)) {
    QuerySpec body =
        genQuery(Purpose::standalone, nullptr, Scope{}, subqDepth + 1, false);
    auto name = fmt::format("w{}", aliasCounter_++);
    metadata::Table synth;
    synth.name = name;
    for (auto const &item : body.selectItems) {
      metadata::Column col;
      col.name = item.colAlias;
      col.type = item.expr.type;
      synth.columns.push_back(std::move(col));
    }
    cands.push_back(std::make_shared<metadata::Table const>(std::move(synth)));
    q.ctes.push_back(
        {.name = std::move(name), .query = box<QuerySpec>(std::move(body))});
  }

  auto seed =
      target != nullptr
          ? std::move(target)
          : cands[rand_.random_number<std::size_t>(0, cands.size() - 1)];
  Scope scope;
  scope.push_back({.alias = freshAlias(), .table = seed});
  q.from = TableSource{.table = seed->name, .alias = scope[0].alias};

  // chained cross joins multiply row counts without bound; one is plenty
  bool haveCross = false;
  while (q.joins.size() < cfg_.max_joins && roll(cfg_.join_prob)) {
    auto const &base =
        scope[rand_.random_number<std::size_t>(0, scope.size() - 1)];

    struct Edge {
      metadata::table_cptr tbl;
      std::string newCol;
      std::string baseCol;
      metadata::ColumnType newType;
      metadata::ColumnType baseType;
    };
    std::vector<Edge> edges;
    for (auto const &col : base.table->columns) {
      if (col.foreign_key_references) {
        auto parent = std::ranges::find_if(cands, [&](auto const &t) {
          return t->id == col.foreign_key_references.id;
        });
        if (parent != cands.end()) {
          if (auto const *pk = pkColumn(**parent)) {
            // join to parent: parent.pk = base.fkcol
            edges.push_back({.tbl = *parent,
                             .newCol = pk->name,
                             .baseCol = col.name,
                             .newType = pk->type,
                             .baseType = col.type});
          }
        }
      }
    }
    for (auto const &t : cands) {
      for (auto const &col : t->columns) {
        if (col.foreign_key_references &&
            col.foreign_key_references.id == base.table->id) {
          if (auto const *pk = pkColumn(*base.table)) {
            // join to child: child.fkcol = base.pk
            edges.push_back({.tbl = t,
                             .newCol = col.name,
                             .baseCol = pk->name,
                             .newType = col.type,
                             .baseType = pk->type});
          }
        }
      }
    }

    metadata::table_cptr joined;
    box<Expr> cond;
    std::string alias = freshAlias();
    if (!edges.empty() && roll(70)) {
      auto const &e =
          edges[rand_.random_number<std::size_t>(0, edges.size() - 1)];
      joined = e.tbl;
      cond = box<Expr>(Expr{
          .node =
              BinaryExpr{
                  .op = BinOp::eq,
                  .lhs = box<Expr>(Expr{
                      .node = ColumnRef{.alias = alias, .column = e.newCol},
                      .type = e.newType}),
                  .rhs = box<Expr>(Expr{.node = ColumnRef{.alias = base.alias,
                                                          .column = e.baseCol},
                                        .type = e.baseType})},
          .type = metadata::ColumnType::BOOL});
    } else if (!haveCross && roll(10)) {
      joined = cands[rand_.random_number<std::size_t>(0, cands.size() - 1)];
      // cross join: no condition
    } else {
      joined = cands[rand_.random_number<std::size_t>(0, cands.size() - 1)];
      struct Pair {
        metadata::Column const *baseCol;
        metadata::Column const *newCol;
      };
      std::vector<Pair> pairs;
      for (auto const &bc : base.table->columns) {
        auto const bf = familyOf(bc.type);
        if (bf != Family::numeric && bf != Family::string) {
          continue;
        }
        for (auto const &nc : joined->columns) {
          if (familyOf(nc.type) == bf) {
            pairs.push_back({.baseCol = &bc, .newCol = &nc});
          }
        }
      }
      if (!pairs.empty()) {
        auto const &p =
            pairs[rand_.random_number<std::size_t>(0, pairs.size() - 1)];
        cond = box<Expr>(Expr{
            .node =
                BinaryExpr{.op = BinOp::eq,
                           .lhs = box<Expr>(
                               Expr{.node = ColumnRef{.alias = alias,
                                                      .column = p.newCol->name},
                                    .type = p.newCol->type}),
                           .rhs = box<Expr>(Expr{
                               .node = ColumnRef{.alias = base.alias,
                                                 .column = p.baseCol->name},
                               .type =
                                   p.baseCol->type})},
            .type = metadata::ColumnType::BOOL});
      }
    }

    if (!cond && haveCross) {
      break; // would be a second cross join; stop extending
    }
    JoinKind kind = JoinKind::cross;
    if (cond) {
      auto const r = rand_.random_number<std::size_t>(1, 100);
      if (r <= 50) {
        kind = JoinKind::inner;
      } else if (r <= 85) {
        kind = JoinKind::left;
      } else {
        // a RIGHT join makes the seed table nullable - pg refuses to lock
        // the nullable side, so pk selects never use it
        kind = purpose == Purpose::pkSelect ? JoinKind::left : JoinKind::right;
      }
    } else {
      haveCross = true;
    }
    q.joins.push_back({.kind = kind,
                       .source = {.table = joined->name, .alias = alias},
                       .condition = std::move(cond)});
    scope.push_back({.alias = std::move(alias), .table = joined});
  }

  // flat (alias, column) view of the scope for select-list picks
  struct ScopeCol {
    std::string const *alias;
    metadata::Column const *col;
  };
  std::vector<ScopeCol> scopeCols;
  for (auto const &entry : scope) {
    for (auto const &col : entry.table->columns) {
      scopeCols.push_back({.alias = &entry.alias, .col = &col});
    }
  }
  auto randomScopeCol = [&]() -> ScopeCol const & {
    return scopeCols[rand_.random_number<std::size_t>(0, scopeCols.size() - 1)];
  };

  bool const isAgg = purpose != Purpose::pkSelect && roll(cfg_.aggregate_prob);
  if (purpose == Purpose::pkSelect) {
    // exactly the seed pk (columns[0], repo-wide single-pk assumption);
    // joins may duplicate pk values, DISTINCT keeps the id set clean
    q.selectItems.push_back(
        {.expr = colExpr(scope[0].alias, seed->columns[0]), .colAlias = ""});
    q.distinct = !q.joins.empty();
  } else if (isAgg) {
    auto const nGroup = rand_.random_number<std::size_t>(0, 2);
    for (std::size_t i = 0; i < nGroup; ++i) {
      auto const &pick = randomScopeCol();
      ColumnRef ref{.alias = *pick.alias, .column = pick.col->name};
      auto const dup = std::ranges::any_of(q.groupBy, [&](auto const &g) {
        return g.alias == ref.alias && g.column == ref.column;
      });
      if (dup) {
        continue;
      }
      q.selectItems.push_back(
          {.expr = Expr{.node = ref, .type = pick.col->type}, .colAlias = ""});
      q.groupBy.push_back(std::move(ref));
    }
    auto const nAggs = rand_.random_number<std::size_t>(1, 2);
    for (std::size_t i = 0; i < nAggs; ++i) {
      q.selectItems.push_back({.expr = genAggCall(scope), .colAlias = ""});
    }
    if (roll(50)) {
      Expr agg = genAggCall(scope);
      Expr lit = genLiteral(representative(familyOf(agg.type)), 0);
      q.having = box<Expr>(Expr{
          .node = BinaryExpr{.op = rand_.random_bool() ? BinOp::gt : BinOp::le,
                             .lhs = box<Expr>(std::move(agg)),
                             .rhs = box<Expr>(std::move(lit))},
          .type = metadata::ColumnType::BOOL});
    }
  } else {
    auto const nCols = rand_.random_number<std::size_t>(1, 4);
    for (std::size_t i = 0; i < nCols; ++i) {
      auto const &pick = randomScopeCol();
      q.selectItems.push_back(
          {.expr = colExpr(*pick.alias, *pick.col), .colAlias = ""});
    }
    if (roll(25)) {
      auto const &pick = randomScopeCol();
      q.selectItems.push_back(
          {.expr = genScalar(pick.col->type, scope, cfg_.max_expr_depth, false),
           .colAlias = ""});
    }
  }
  if (allowExtras && !isAgg && roll(cfg_.window_prob)) {
    q.selectItems.push_back({.expr = genWindowCall(scope), .colAlias = ""});
  }
  for (std::size_t i = 0; i < q.selectItems.size(); ++i) {
    q.selectItems[i].colAlias = fmt::format("c{}", i);
  }

  if (roll(75)) {
    Scope whereScope = scope;
    if (!outer.empty() && roll(cfg_.correlation_prob)) {
      whereScope.insert(whereScope.end(), outer.begin(), outer.end());
    }
    q.where =
        box<Expr>(genBool(whereScope, cfg_.max_expr_depth, subqDepth, false));
  }

  if (roll(cfg_.order_by_prob)) {
    std::vector<Expr const *> refs;
    for (auto const &item : q.selectItems) {
      if (std::holds_alternative<ColumnRef>(item.expr.node)) {
        refs.push_back(&item.expr);
      }
    }
    if (!refs.empty()) {
      auto const n = rand_.random_number<std::size_t>(
          1, std::min<std::size_t>(2, refs.size()));
      for (std::size_t i = 0; i < n; ++i) {
        auto const *pick =
            refs[rand_.random_number<std::size_t>(0, refs.size() - 1)];
        q.orderBy.push_back({.expr = *pick, .desc = rand_.random_bool()});
      }
    }
  }

  if (purpose != Purpose::pkSelect && roll(cfg_.limit_prob)) {
    q.limit = rand_.random_number<std::size_t>(1, 1000);
    if (roll(25)) {
      q.offset = rand_.random_number<std::size_t>(0, 100);
    }
  }
  if (purpose != Purpose::pkSelect && !isAgg && roll(15)) {
    q.distinct = true;
  }

  if (allowExtras && roll(cfg_.setop_prob)) {
    QuerySpec rhs =
        genQuery(Purpose::standalone, nullptr, Scope{}, subqDepth + 1, false);
    // force rhs select list into lhs shape: same count, same family per
    // position; mismatches become literals (always valid, no scope needed).
    // rhs ORDER BY may reference replaced items - drop it
    rhs.orderBy.clear();
    rhs.offset.reset();
    if (rhs.selectItems.size() > q.selectItems.size()) {
      rhs.selectItems.resize(q.selectItems.size());
    }
    for (std::size_t i = 0; i < q.selectItems.size(); ++i) {
      auto const fam = familyOf(q.selectItems[i].expr.type);
      if (i >= rhs.selectItems.size()) {
        rhs.selectItems.push_back(
            {.expr = genLiteral(representative(fam), 0), .colAlias = ""});
      } else if (familyOf(rhs.selectItems[i].expr.type) != fam) {
        rhs.selectItems[i].expr = genLiteral(representative(fam), 0);
      }
      rhs.selectItems[i].colAlias = fmt::format("c{}", i);
    }

    bool const supportsIntersect =
        sql_dialect::dialect_for(server_).supportsIntersectExcept(server_);
    auto const r = rand_.random_number<std::size_t>(1, 100);
    if (r <= 40) {
      q.setOpKind = SetOpKind::unionAll;
    } else if (r <= 70 || !supportsIntersect) {
      q.setOpKind = r % 2 == 0 ? SetOpKind::unionAll : SetOpKind::unionDistinct;
    } else if (r <= 85) {
      q.setOpKind = SetOpKind::intersect;
    } else {
      q.setOpKind = SetOpKind::except;
    }
    q.setOpRhs = box<QuerySpec>(std::move(rhs));

    // outer ORDER BY on a set op must use output column names
    q.orderBy.clear();
    if (roll(cfg_.order_by_prob)) {
      auto const idx =
          rand_.random_number<std::size_t>(0, q.selectItems.size() - 1);
      q.orderBy.push_back(
          {.expr = Expr{.node = ColumnRef{.alias = "",
                                          .column = fmt::format("c{}", idx)},
                        .type = q.selectItems[idx].expr.type},
           .desc = rand_.random_bool()});
    }
  }

  return q;
}

Expr Generator::genWindowCall(Scope const &scope) {
  using metadata::ColumnType;
  std::vector<std::pair<std::string const *, metadata::Column const *>> numeric;
  std::vector<std::pair<std::string const *, metadata::Column const *>>
      comparable;
  std::vector<std::pair<std::string const *, metadata::Column const *>> all;
  for (auto const &entry : scope) {
    for (auto const &col : entry.table->columns) {
      auto const fam = familyOf(col.type);
      if (fam == Family::numeric) {
        numeric.emplace_back(&entry.alias, &col);
      }
      if (fam == Family::numeric || fam == Family::string) {
        comparable.emplace_back(&entry.alias, &col);
      }
      all.emplace_back(&entry.alias, &col);
    }
  }

  std::vector<WinFunc> fns{WinFunc::rowNumber, WinFunc::rank,
                           WinFunc::denseRank, WinFunc::count};
  if (!numeric.empty()) {
    fns.insert(fns.end(), {WinFunc::sum, WinFunc::avg});
  }
  if (!comparable.empty()) {
    fns.insert(fns.end(), {WinFunc::min, WinFunc::max});
  }
  auto const fn = fns[rand_.random_number<std::size_t>(0, fns.size() - 1)];

  WindowCall w{.fn = fn, .arg = {}, .partitionBy = {}, .orderBy = {}};
  ColumnType type = ColumnType::INT;
  switch (fn) {
  case WinFunc::rowNumber:
  case WinFunc::rank:
  case WinFunc::denseRank:
    break;
  case WinFunc::count:
    if (!all.empty() && roll(50)) {
      auto const &[alias, col] =
          all[rand_.random_number<std::size_t>(0, all.size() - 1)];
      w.arg = box<Expr>(colExpr(*alias, *col));
    }
    break;
  case WinFunc::sum:
  case WinFunc::avg: {
    auto const &[alias, col] =
        numeric[rand_.random_number<std::size_t>(0, numeric.size() - 1)];
    w.arg = box<Expr>(colExpr(*alias, *col));
    type = fn == WinFunc::avg ? ColumnType::REAL : col->type;
    break;
  }
  case WinFunc::min:
  case WinFunc::max: {
    auto const &[alias, col] =
        comparable[rand_.random_number<std::size_t>(0, comparable.size() - 1)];
    w.arg = box<Expr>(colExpr(*alias, *col));
    type = col->type;
    break;
  }
  }

  if (!all.empty() && roll(50)) {
    auto const &[alias, col] =
        all[rand_.random_number<std::size_t>(0, all.size() - 1)];
    w.partitionBy.push_back({.alias = *alias, .column = col->name});
  }
  if (!all.empty() && roll(70)) {
    auto const &[alias, col] =
        all[rand_.random_number<std::size_t>(0, all.size() - 1)];
    w.orderBy.push_back({.alias = *alias, .column = col->name});
  }
  return Expr{.node = std::move(w), .type = type};
}

Expr Generator::genAggCall(Scope const &scope) {
  using metadata::ColumnType;
  std::vector<std::pair<std::string const *, metadata::Column const *>> numeric;
  std::vector<std::pair<std::string const *, metadata::Column const *>>
      comparable;
  for (auto const &entry : scope) {
    for (auto const &col : entry.table->columns) {
      auto const fam = familyOf(col.type);
      if (fam == Family::numeric) {
        numeric.emplace_back(&entry.alias, &col);
      }
      if (fam == Family::numeric || fam == Family::string) {
        comparable.emplace_back(&entry.alias, &col);
      }
    }
  }

  std::vector<AggFunc> fns{AggFunc::count};
  if (!numeric.empty()) {
    fns.insert(fns.end(), {AggFunc::sum, AggFunc::avg});
  }
  if (!comparable.empty()) {
    fns.insert(fns.end(), {AggFunc::min, AggFunc::max});
  }
  auto const fn = fns[rand_.random_number<std::size_t>(0, fns.size() - 1)];

  if (fn == AggFunc::count && (comparable.empty() || roll(50))) {
    return Expr{.node =
                    AggCall{.fn = AggFunc::count, .arg = {}, .distinct = false},
                .type = ColumnType::INT};
  }
  auto const &pool =
      (fn == AggFunc::sum || fn == AggFunc::avg) ? numeric : comparable;
  auto const &[alias, col] =
      pool[rand_.random_number<std::size_t>(0, pool.size() - 1)];
  ColumnType type{};
  switch (fn) {
  case AggFunc::count:
    type = ColumnType::INT;
    break;
  case AggFunc::avg:
    type = ColumnType::REAL;
    break;
  case AggFunc::sum:
  case AggFunc::min:
  case AggFunc::max:
    type = col->type;
    break;
  }
  return Expr{.node = AggCall{.fn = fn,
                              .arg = box<Expr>(colExpr(*alias, *col)),
                              .distinct = roll(25)},
              .type = type};
}

std::optional<QuerySpec> Generator::generate(Purpose purpose,
                                             metadata::table_cptr target) {
  if (purpose == Purpose::pkSelect) {
    return generatePkSelect(std::move(target), {});
  }
  if (tables_.empty()) {
    return std::nullopt;
  }
  return genQuery(purpose, std::move(target), Scope{}, 0,
                  purpose == Purpose::standalone);
}

std::optional<QuerySpec>
Generator::generatePkSelect(metadata::table_cptr target,
                            PkSelectOpts const &opts) {
  if (tables_.empty() || target == nullptr || target->columns.empty()) {
    return std::nullopt;
  }
  QuerySpec q =
      genQuery(Purpose::pkSelect, std::move(target), Scope{}, 0, false);
  if (opts.limit > 0) {
    q.limit = opts.limit;
  }
  if (opts.lock != sql_dialect::LockClause::none) {
    q.lock = LockSpec{.clause = opts.lock, .ofAlias = q.from.alias};
    // pg rejects FOR UPDATE with DISTINCT; duplicate pks from join fan-out
    // are harmless in a WHERE pk IN (...) list
    q.distinct = false;
  }
  return q;
}

Expr Generator::generatePredicate(metadata::table_cptr target,
                                  std::string_view qualifier) {
  // mysql rejects subqueries against the UPDATE/DELETE target table
  // (ER_UPDATE_TABLE_USED) - keep it out of the subquery candidates
  std::erase_if(tables_, [&](auto const &t) { return t->id == target->id; });
  Scope const scope{
      {.alias = std::string(qualifier), .table = std::move(target)}};
  return genBool(scope, cfg_.max_expr_depth, 0, false);
}

} // namespace querygen
