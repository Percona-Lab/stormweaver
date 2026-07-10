#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "querygen/generator.hpp"
#include "querygen/render.hpp"
#include "sql_dialect/dialect.hpp"

using namespace querygen;
using namespace metadata;

namespace {

// two tables, FK t2.ref -> t1.id, mixed column types
void fillCatalog(TableRegistry &reg) {
  Table t1;
  auto const t1Id = reg.nextId();
  t1.id = t1Id;
  t1.name = "t1_tab";
  t1.columns.push_back({.name = "id",
                        .type = ColumnType::INT,
                        .nullable = false,
                        .primary_key = true,
                        .auto_increment = true});
  t1.columns.push_back({.name = "num", .type = ColumnType::INT});
  t1.columns.push_back(
      {.name = "txt", .type = ColumnType::VARCHAR, .length = 32});
  t1.columns.push_back({.name = "flag", .type = ColumnType::BOOL});
  reg.get<Table>().insert(std::move(t1));

  Table t2;
  t2.id = reg.nextId();
  t2.name = "t2_tab";
  t2.columns.push_back({.name = "id",
                        .type = ColumnType::INT,
                        .nullable = false,
                        .primary_key = true,
                        .auto_increment = true});
  Column ref{.name = "ref", .type = ColumnType::INT};
  ref.foreign_key_references = Ref<Table>{t1Id};
  t2.columns.push_back(ref);
  t2.columns.push_back({.name = "val", .type = ColumnType::REAL});
  reg.get<Table>().insert(std::move(t2));
}

sql_variant::ServerInfo pgInfo() {
  return {sql_variant::flavor::postgres, 180000};
}

// walks the whole tree (incl. subqueries) looking for AND/OR/NOT/CASE -
// none of these may appear when the depth budget is zero
struct CombinatorScan {
  bool found = false;

  void scan(Expr const &e) {
    std::visit([&](auto const &n) { node(n); }, e.node);
  }
  void node(ColumnRef const & /*unused*/) {}
  void node(Literal const & /*unused*/) {}
  void node(UnaryExpr const &u) {
    if (u.op == UnOp::not_) {
      found = true;
    }
    scan(*u.arg);
  }
  void node(BinaryExpr const &b) {
    if (b.op == BinOp::and_ || b.op == BinOp::or_) {
      found = true;
    }
    scan(*b.lhs);
    scan(*b.rhs);
  }
  void node(BetweenExpr const &b) {
    scan(*b.arg);
    scan(*b.lo);
    scan(*b.hi);
  }
  void node(InListExpr const &i) {
    scan(*i.arg);
    for (auto const &it : i.items) {
      scan(*it);
    }
  }
  void node(InSubquery const &i) {
    scan(*i.arg);
    query(*i.sub);
  }
  void node(ExistsSubquery const &e) { query(*e.sub); }
  void node(ScalarSubquery const &s) { query(*s.sub); }
  void node(FuncCall const &f) {
    for (auto const &a : f.args) {
      scan(*a);
    }
  }
  void node(CaseExpr const & /*unused*/) { found = true; }
  void node(AggCall const &a) {
    if (a.arg) {
      scan(*a.arg);
    }
  }
  void node(WindowCall const & /*unused*/) {}
  void query(QuerySpec const &q) {
    for (auto const &si : q.selectItems) {
      scan(si.expr);
    }
    if (q.where) {
      scan(*q.where);
    }
    if (q.having) {
      scan(*q.having);
    }
  }
};

} // namespace

TEST_CASE("predicate generation is deterministic", "[querygen]") {
  TableRegistry reg;
  fillCatalog(reg);
  Context ctx(reg);
  QueryGenConfig cfg;
  auto target = ctx.get<Table>().byName("t1_tab");

  auto run = [&](std::uint64_t seed) {
    ps_random rand(seed);
    Generator gen(ctx, rand, cfg, pgInfo());
    return render(gen.generatePredicate(target, "t1_tab"),
                  sql_dialect::pg_dialect());
  };
  CHECK(run(42) == run(42));
  CHECK(run(42) != run(43)); // different seed, different query (overwhelmingly)
}

TEST_CASE("predicates render and are bool-typed", "[querygen]") {
  TableRegistry reg;
  fillCatalog(reg);
  Context ctx(reg);
  QueryGenConfig cfg;
  auto target = ctx.get<Table>().byName("t1_tab");
  ps_random rand(7);
  for (int i = 0; i < 500; ++i) {
    Generator gen(ctx, rand, cfg, pgInfo());
    auto e = gen.generatePredicate(target, "t1_tab");
    CHECK(e.type == ColumnType::BOOL);
    auto sql = render(e, sql_dialect::pg_dialect());
    REQUIRE(!sql.empty());
    CHECK(std::count(sql.begin(), sql.end(), '(') ==
          std::count(sql.begin(), sql.end(), ')'));
  }
}

TEST_CASE("depth budget zero produces only leaf comparisons", "[querygen]") {
  TableRegistry reg;
  fillCatalog(reg);
  Context ctx(reg);
  QueryGenConfig cfg;
  cfg.max_expr_depth = 0;
  auto target = ctx.get<Table>().byName("t1_tab");
  ps_random rand(3);
  for (int i = 0; i < 200; ++i) {
    Generator gen(ctx, rand, cfg, pgInfo());
    auto e = gen.generatePredicate(target, "t1_tab");
    CombinatorScan scanner;
    scanner.scan(e);
    CHECK(!scanner.found);
  }
}

TEST_CASE("subquery_prob zero disables subqueries", "[querygen]") {
  TableRegistry reg;
  fillCatalog(reg);
  Context ctx(reg);
  QueryGenConfig cfg;
  cfg.subquery_prob = 0;
  auto target = ctx.get<Table>().byName("t1_tab");
  ps_random rand(7);
  for (int i = 0; i < 200; ++i) {
    Generator gen(ctx, rand, cfg, pgInfo());
    auto sql = render(gen.generatePredicate(target, "t1_tab"),
                      sql_dialect::pg_dialect());
    CHECK(sql.find("SELECT") == std::string::npos);
  }
}

TEST_CASE("standalone generation deterministic and valid", "[querygen]") {
  TableRegistry reg;
  fillCatalog(reg);
  Context ctx(reg);
  QueryGenConfig cfg;
  auto run = [&](std::uint64_t seed) {
    ps_random rand(seed);
    Generator gen(ctx, rand, cfg, pgInfo());
    auto q = gen.generate(Purpose::standalone, nullptr);
    REQUIRE(q.has_value());
    return render(*q, sql_dialect::pg_dialect());
  };
  CHECK(run(1) == run(1));

  ps_random rand(3);
  for (int i = 0; i < 500; ++i) {
    Generator gen(ctx, rand, cfg, pgInfo());
    auto q = gen.generate(Purpose::standalone, nullptr);
    REQUIRE(q.has_value());
    auto sql = render(*q, sql_dialect::pg_dialect());
    CHECK(std::count(sql.begin(), sql.end(), '(') ==
          std::count(sql.begin(), sql.end(), ')'));
  }
}

TEST_CASE("empty catalog yields nullopt", "[querygen]") {
  TableRegistry reg;
  Context ctx(reg);
  QueryGenConfig cfg;
  ps_random rand(1);
  Generator gen(ctx, rand, cfg, pgInfo());
  CHECK(!gen.generate(Purpose::standalone, nullptr).has_value());
}

TEST_CASE("join_prob zero disables joins", "[querygen]") {
  TableRegistry reg;
  fillCatalog(reg);
  Context ctx(reg);
  QueryGenConfig cfg;
  cfg.join_prob = 0;
  ps_random rand(9);
  for (int i = 0; i < 200; ++i) {
    Generator gen(ctx, rand, cfg, pgInfo());
    auto q = gen.generate(Purpose::standalone, nullptr);
    REQUIRE(q.has_value());
    CHECK(q->joins.empty());
  }
}

TEST_CASE("aggregate select lists are group-by-consistent", "[querygen]") {
  TableRegistry reg;
  fillCatalog(reg);
  Context ctx(reg);
  QueryGenConfig cfg;
  cfg.aggregate_prob = 100;
  ps_random rand(5);
  for (int i = 0; i < 100; ++i) {
    Generator gen(ctx, rand, cfg, pgInfo());
    auto q = gen.generate(Purpose::standalone, nullptr);
    REQUIRE(q.has_value());
    if (q->groupBy.empty()) {
      continue; // pure-aggregate query, no plain refs allowed either way
    }
    for (auto const &item : q->selectItems) {
      if (auto const *cr = std::get_if<ColumnRef>(&item.expr.node)) {
        auto found = std::ranges::any_of(q->groupBy, [&](auto const &g) {
          return g.alias == cr->alias && g.column == cr->column;
        });
        CHECK(found);
      }
    }
  }
}

TEST_CASE("join conditions equate same-family columns", "[querygen]") {
  TableRegistry reg;
  fillCatalog(reg);
  Context ctx(reg);
  QueryGenConfig cfg;
  cfg.join_prob = 100;
  cfg.cte_prob = 0; // synthetic CTE tables are not resolvable by catalog name
  auto columnType = [&](std::string const &table,
                        std::string const &column) -> ColumnType {
    auto t = ctx.get<Table>().byName(table);
    REQUIRE(t != nullptr);
    for (auto const &c : t->columns) {
      if (c.name == column) {
        return c.type;
      }
    }
    FAIL("column not found: " + table + "." + column);
    return ColumnType::INT;
  };
  auto family = [](ColumnType t) {
    switch (t) {
    case ColumnType::INT:
    case ColumnType::REAL:
      return 0;
    case ColumnType::CHAR:
    case ColumnType::VARCHAR:
    case ColumnType::TEXT:
      return 1;
    case ColumnType::BOOL:
      return 2;
    case ColumnType::BYTEA:
      return 3;
    }
    return 0;
  };
  ps_random rand(13);
  for (int i = 0; i < 100; ++i) {
    Generator gen(ctx, rand, cfg, pgInfo());
    auto q = gen.generate(Purpose::standalone, nullptr);
    REQUIRE(q.has_value());
    std::vector<std::pair<std::string, std::string>> sources{
        {q->from.alias, q->from.table}};
    for (auto const &j : q->joins) {
      sources.emplace_back(j.source.alias, j.source.table);
    }
    auto tableOf = [&](std::string const &alias) {
      for (auto const &[a, t] : sources) {
        if (a == alias) {
          return t;
        }
      }
      FAIL("alias not in scope: " + alias);
      return std::string{};
    };
    for (auto const &j : q->joins) {
      if (j.kind == JoinKind::cross) {
        CHECK(!j.condition);
        continue;
      }
      REQUIRE(static_cast<bool>(j.condition));
      auto const *cmp = std::get_if<BinaryExpr>(&j.condition->node);
      REQUIRE(cmp != nullptr);
      CHECK(cmp->op == BinOp::eq);
      auto const *lhs = std::get_if<ColumnRef>(&(*cmp->lhs).node);
      auto const *rhs = std::get_if<ColumnRef>(&(*cmp->rhs).node);
      REQUIRE(lhs != nullptr);
      REQUIRE(rhs != nullptr);
      CHECK(family(columnType(tableOf(lhs->alias), lhs->column)) ==
            family(columnType(tableOf(rhs->alias), rhs->column)));
    }
  }
}

namespace {
int familyIdx(ColumnType t) {
  switch (t) {
  case ColumnType::INT:
  case ColumnType::REAL:
    return 0;
  case ColumnType::CHAR:
  case ColumnType::VARCHAR:
  case ColumnType::TEXT:
    return 1;
  case ColumnType::BOOL:
    return 2;
  case ColumnType::BYTEA:
    return 3;
  }
  return 0;
}
} // namespace

TEST_CASE("cte generation", "[querygen]") {
  TableRegistry reg;
  fillCatalog(reg);
  Context ctx(reg);
  QueryGenConfig cfg;
  cfg.cte_prob = 100;
  ps_random rand(17);
  for (int i = 0; i < 50; ++i) {
    Generator gen(ctx, rand, cfg, pgInfo());
    auto q = gen.generate(Purpose::standalone, nullptr);
    REQUIRE(q.has_value());
    REQUIRE(!q->ctes.empty());
    auto sql = render(*q, sql_dialect::pg_dialect());
    CHECK(sql.starts_with("WITH "));
    CHECK(std::count(sql.begin(), sql.end(), '(') ==
          std::count(sql.begin(), sql.end(), ')'));
    // CTE body is itself a renderable standalone query
    CHECK(!render(*q->ctes[0].query, sql_dialect::pg_dialect()).empty());
  }
}

TEST_CASE("setop select lists are family-compatible", "[querygen]") {
  TableRegistry reg;
  fillCatalog(reg);
  Context ctx(reg);
  QueryGenConfig cfg;
  cfg.setop_prob = 100;
  ps_random rand(19);
  for (int i = 0; i < 100; ++i) {
    Generator gen(ctx, rand, cfg, pgInfo());
    auto q = gen.generate(Purpose::standalone, nullptr);
    REQUIRE(q.has_value());
    REQUIRE(static_cast<bool>(q->setOpRhs));
    auto const &rhs = *q->setOpRhs;
    REQUIRE(rhs.selectItems.size() == q->selectItems.size());
    for (std::size_t p = 0; p < q->selectItems.size(); ++p) {
      CHECK(familyIdx(rhs.selectItems[p].expr.type) ==
            familyIdx(q->selectItems[p].expr.type));
    }
    CHECK(!static_cast<bool>(rhs.setOpRhs)); // operands stay flat
    auto sql = render(*q, sql_dialect::pg_dialect());
    CHECK(std::count(sql.begin(), sql.end(), '(') ==
          std::count(sql.begin(), sql.end(), ')'));
  }
}

TEST_CASE("intersect/except gated by server support", "[querygen]") {
  TableRegistry reg;
  fillCatalog(reg);
  Context ctx(reg);
  QueryGenConfig cfg;
  cfg.setop_prob = 100;
  sql_variant::ServerInfo const oldMysql{sql_variant::flavor::mysql, 80030};
  ps_random rand(23);
  for (int i = 0; i < 200; ++i) {
    Generator gen(ctx, rand, cfg, oldMysql);
    auto q = gen.generate(Purpose::standalone, nullptr);
    REQUIRE(q.has_value());
    REQUIRE(static_cast<bool>(q->setOpRhs));
    CHECK(q->setOpKind != SetOpKind::intersect);
    CHECK(q->setOpKind != SetOpKind::except);
  }
}

TEST_CASE("window generation stays out of where/having", "[querygen]") {
  TableRegistry reg;
  fillCatalog(reg);
  Context ctx(reg);
  QueryGenConfig cfg;
  cfg.window_prob = 100;
  cfg.aggregate_prob = 0;
  ps_random rand(29);
  for (int i = 0; i < 100; ++i) {
    Generator gen(ctx, rand, cfg, pgInfo());
    auto q = gen.generate(Purpose::standalone, nullptr);
    REQUIRE(q.has_value());
    CHECK(q->groupBy.empty());
    auto const windowed =
        std::ranges::any_of(q->selectItems, [](auto const &item) {
          return std::holds_alternative<WindowCall>(item.expr.node);
        });
    CHECK(windowed);
    if (q->where) {
      auto sql = render(*q->where, sql_dialect::pg_dialect());
      CHECK(sql.find(" OVER (") == std::string::npos);
    }
    if (q->having) {
      auto sql = render(*q->having, sql_dialect::pg_dialect());
      CHECK(sql.find(" OVER (") == std::string::npos);
    }
  }
}

TEST_CASE("determinism with extras at 100", "[querygen]") {
  TableRegistry reg;
  fillCatalog(reg);
  Context ctx(reg);
  QueryGenConfig cfg;
  cfg.cte_prob = 100;
  cfg.setop_prob = 100;
  cfg.window_prob = 100;
  auto run = [&](std::uint64_t seed) {
    ps_random rand(seed);
    Generator gen(ctx, rand, cfg, pgInfo());
    auto q = gen.generate(Purpose::standalone, nullptr);
    REQUIRE(q.has_value());
    return render(*q, sql_dialect::pg_dialect());
  };
  CHECK(run(31) == run(31));
}

TEST_CASE("pk select shape", "[querygen]") {
  TableRegistry reg;
  fillCatalog(reg);
  Context ctx(reg);
  QueryGenConfig cfg;
  cfg.join_prob = 100; // force joins to exercise DISTINCT + lock rules
  auto target = ctx.get<Table>().byName("t2_tab");
  ps_random rand(9);
  for (int i = 0; i < 200; ++i) {
    Generator gen(ctx, rand, cfg, pgInfo());
    auto q = gen.generatePkSelect(
        target, {.limit = 10, .lock = sql_dialect::LockClause::forUpdate});
    REQUIRE(q.has_value());
    REQUIRE(q->selectItems.size() == 1);
    auto const *cr = std::get_if<ColumnRef>(&q->selectItems[0].expr.node);
    REQUIRE(cr != nullptr);
    CHECK(cr->column == "id");
    CHECK(cr->alias == q->from.alias);
    CHECK(q->from.table == "t2_tab");
    CHECK(q->limit == 10);
    CHECK(q->lock.clause == sql_dialect::LockClause::forUpdate);
    CHECK(q->lock.ofAlias == q->from.alias);
    // pg forbids FOR UPDATE + DISTINCT; locked pk selects skip it
    CHECK(!q->distinct);
    for (auto const &j : q->joins) {
      CHECK(j.kind != JoinKind::right);
    }
    CHECK(q->groupBy.empty());
    CHECK(!static_cast<bool>(q->setOpRhs));
    CHECK(q->ctes.empty());
  }

  // without a lock, join fan-out is deduplicated
  ps_random rand2(10);
  for (int i = 0; i < 100; ++i) {
    Generator gen(ctx, rand2, cfg, pgInfo());
    auto q = gen.generatePkSelect(target, {});
    REQUIRE(q.has_value());
    if (!q->joins.empty()) {
      CHECK(q->distinct);
    }
  }
}

TEST_CASE("division always guarded", "[querygen]") {
  TableRegistry reg;
  fillCatalog(reg);
  Context ctx(reg);
  QueryGenConfig cfg;
  cfg.max_expr_depth = 6;
  auto target = ctx.get<Table>().byName("t1_tab");
  ps_random rand(11);
  for (int i = 0; i < 300; ++i) {
    Generator gen(ctx, rand, cfg, pgInfo());
    auto sql = render(gen.generatePredicate(target, "t1_tab"),
                      sql_dialect::pg_dialect());
    // every / or % is immediately followed by " NULLIF("
    for (auto pos = sql.find(" / "); pos != std::string::npos;
         pos = sql.find(" / ", pos + 1)) {
      CHECK(sql.compare(pos + 3, 7, "NULLIF(") == 0);
    }
    for (auto pos = sql.find(" % "); pos != std::string::npos;
         pos = sql.find(" % ", pos + 1)) {
      CHECK(sql.compare(pos + 3, 7, "NULLIF(") == 0);
    }
  }
}
