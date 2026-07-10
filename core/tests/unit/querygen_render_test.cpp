#include <catch2/catch_test_macros.hpp>

#include "querygen/ir.hpp"
#include "querygen/oracle.hpp"
#include "querygen/render.hpp"
#include "sql_dialect/dialect.hpp"

using namespace querygen;

TEST_CASE("IR deep copy is independent", "[querygen]") {
  Expr lhs{ColumnRef{"t0", "id"}, metadata::ColumnType::INT};
  Expr rhs{Literal{std::int64_t{42}}, metadata::ColumnType::INT};
  Expr cmp{BinaryExpr{BinOp::eq, box<Expr>(lhs), box<Expr>(rhs)},
           metadata::ColumnType::BOOL};

  QuerySpec q;
  q.from = {"t1_table", "t0"};
  q.selectItems.push_back({lhs, "c0"});
  q.where = box<Expr>(cmp);

  QuerySpec copy = q;
  std::get<BinaryExpr>(copy.where->node).op = BinOp::ne;

  CHECK(std::get<BinaryExpr>(q.where->node).op == BinOp::eq);
  CHECK(std::get<BinaryExpr>(copy.where->node).op == BinOp::ne);

  QuerySpec q2;
  q2.from = {"other", "t9"};
  q2 = q; // copy-assign
  std::get<BinaryExpr>(q2.where->node).op = BinOp::lt;
  REQUIRE(std::get<BinaryExpr>(q.where->node).op == BinOp::eq);
  QuerySpec &alias = q;
  q = alias; // self-assign must not corrupt
  REQUIRE(std::get<BinaryExpr>(q.where->node).op == BinOp::eq);
}

namespace {
querygen::Expr col(std::string alias, std::string name,
                   metadata::ColumnType t = metadata::ColumnType::INT) {
  return {querygen::ColumnRef{std::move(alias), std::move(name)}, t};
}
querygen::Expr lit(std::int64_t v) {
  return {querygen::Literal{v}, metadata::ColumnType::INT};
}
querygen::Expr lit(std::string v) {
  return {querygen::Literal{std::move(v)}, metadata::ColumnType::VARCHAR};
}
} // namespace

TEST_CASE("render scalar expressions", "[querygen]") {
  auto const &pg = sql_dialect::pg_dialect();
  using namespace querygen;

  Expr cmp{BinaryExpr{BinOp::le, box<Expr>(col("t0", "a")), box<Expr>(lit(5))},
           metadata::ColumnType::BOOL};
  REQUIRE(render(cmp, pg) == "(t0.a <= 5)");

  Expr quote{Literal{std::string("it's")}, metadata::ColumnType::VARCHAR};
  REQUIRE(render(quote, pg) == "'it''s'");

  Expr null_{Literal{}, metadata::ColumnType::INT};
  REQUIRE(render(null_, pg) == "NULL");

  Expr bt{Literal{true}, metadata::ColumnType::BOOL};
  REQUIRE(render(bt, pg) == "true");

  Expr isn{UnaryExpr{UnOp::isNotNull, box<Expr>(col("t0", "a"))},
           metadata::ColumnType::BOOL};
  REQUIRE(render(isn, pg) == "(t0.a IS NOT NULL)");

  Expr neg{UnaryExpr{UnOp::neg, box<Expr>(col("t0", "a"))},
           metadata::ColumnType::INT};
  REQUIRE(render(neg, pg) == "(-t0.a)");

  Expr betw{BetweenExpr{box<Expr>(col("t0", "a")), box<Expr>(lit(1)),
                        box<Expr>(lit(9))},
            metadata::ColumnType::BOOL};
  REQUIRE(render(betw, pg) == "(t0.a BETWEEN 1 AND 9)");

  InListExpr inl{box<Expr>(col("t0", "a")), {}, true};
  inl.items.push_back(box<Expr>(lit(1)));
  inl.items.push_back(box<Expr>(lit(2)));
  REQUIRE(render(Expr{std::move(inl), metadata::ColumnType::BOOL}, pg) ==
          "(t0.a NOT IN (1, 2))");

  FuncCall nf{Func::nullif, {}};
  nf.args.push_back(box<Expr>(col("t0", "a")));
  nf.args.push_back(box<Expr>(lit(0)));
  REQUIRE(render(Expr{std::move(nf), metadata::ColumnType::INT}, pg) ==
          "NULLIF(t0.a, 0)");

  Expr cc{BinaryExpr{BinOp::concat,
                     box<Expr>(col("t0", "s", metadata::ColumnType::TEXT)),
                     box<Expr>(lit(std::string("x")))},
          metadata::ColumnType::TEXT};
  REQUIRE(render(cc, pg) == "(t0.s || 'x')");
  REQUIRE(render(cc, sql_dialect::mysql_dialect()) == "CONCAT(t0.s, 'x')");
}

TEST_CASE("render case expression", "[querygen]") {
  using namespace querygen;
  CaseExpr ce;
  ce.whens.push_back(
      {box<Expr>(Expr{
           BinaryExpr{BinOp::gt, box<Expr>(col("t0", "a")), box<Expr>(lit(0))},
           metadata::ColumnType::BOOL}),
       box<Expr>(lit(1))});
  ce.elseExpr = box<Expr>(lit(2));
  REQUIRE(render(Expr{std::move(ce), metadata::ColumnType::INT},
                 sql_dialect::pg_dialect()) ==
          "CASE WHEN (t0.a > 0) THEN 1 ELSE 2 END");
}

TEST_CASE("render single-table query", "[querygen]") {
  using namespace querygen;
  QuerySpec q;
  q.from = {"t1_table", "t0"};
  q.selectItems.push_back({col("t0", "id"), "c0"});
  q.selectItems.push_back({col("t0", "a"), "c1"});
  q.where = box<Expr>(
      Expr{BinaryExpr{BinOp::eq, box<Expr>(col("t0", "a")), box<Expr>(lit(7))},
           metadata::ColumnType::BOOL});
  q.orderBy.push_back({col("t0", "id"), true});
  q.limit = 10;
  q.lock = {sql_dialect::LockClause::forUpdate, "t0"};

  REQUIRE(render(q, sql_dialect::pg_dialect()) ==
          "SELECT t0.id AS c0, t0.a AS c1 FROM t1_table t0 "
          "WHERE (t0.a = 7) ORDER BY t0.id DESC LIMIT 10 FOR UPDATE OF t0");
}

TEST_CASE("render distinct offset plain lock", "[querygen]") {
  using namespace querygen;
  QuerySpec q;
  q.from = {"t1_table", "t0"};
  q.distinct = true;
  q.selectItems.push_back({col("t0", "id"), "c0"});
  q.limit = 5;
  q.offset = 2;
  q.lock = {sql_dialect::LockClause::forShare, ""};
  REQUIRE(render(q, sql_dialect::pg_dialect()) ==
          "SELECT DISTINCT t0.id AS c0 FROM t1_table t0 "
          "LIMIT 5 OFFSET 2 FOR SHARE");
}

TEST_CASE("render joins", "[querygen]") {
  using namespace querygen;
  QuerySpec q;
  q.from = {"t1_table", "t0"};
  q.joins.push_back(
      {JoinKind::left,
       {"t2_table", "t1"},
       box<Expr>(Expr{BinaryExpr{BinOp::eq, box<Expr>(col("t1", "t1_id")),
                                 box<Expr>(col("t0", "id"))},
                      metadata::ColumnType::BOOL})});
  q.joins.push_back({JoinKind::cross, {"t3_table", "t2"}, {}});
  q.selectItems.push_back({col("t0", "id"), "c0"});
  REQUIRE(render(q, sql_dialect::pg_dialect()) ==
          "SELECT t0.id AS c0 FROM t1_table t0 "
          "LEFT JOIN t2_table t1 ON (t1.t1_id = t0.id) "
          "CROSS JOIN t3_table t2");

  QuerySpec q2;
  q2.from = {"t1_table", "t0"};
  q2.joins.push_back(
      {JoinKind::inner,
       {"t2_table", "t1"},
       box<Expr>(Expr{BinaryExpr{BinOp::eq, box<Expr>(col("t1", "t1_id")),
                                 box<Expr>(col("t0", "id"))},
                      metadata::ColumnType::BOOL})});
  q2.joins.push_back(
      {JoinKind::right,
       {"t3_table", "t2"},
       box<Expr>(Expr{BinaryExpr{BinOp::eq, box<Expr>(col("t2", "t2_id")),
                                 box<Expr>(col("t1", "id"))},
                      metadata::ColumnType::BOOL})});
  q2.selectItems.push_back({col("t0", "id"), "c0"});
  REQUIRE(render(q2, sql_dialect::pg_dialect()) ==
          "SELECT t0.id AS c0 FROM t1_table t0 "
          "JOIN t2_table t1 ON (t1.t1_id = t0.id) "
          "RIGHT JOIN t3_table t2 ON (t2.t2_id = t1.id)");
}

TEST_CASE("render subqueries", "[querygen]") {
  using namespace querygen;
  QuerySpec inner;
  inner.from = {"t2_table", "t1"};
  inner.selectItems.push_back({col("t1", "ref"), "c0"});

  Expr in{InSubquery{box<Expr>(col("t0", "id")), box<QuerySpec>(inner), false},
          metadata::ColumnType::BOOL};
  REQUIRE(render(in, sql_dialect::pg_dialect()) ==
          "(t0.id IN (SELECT t1.ref AS c0 FROM t2_table t1))");

  Expr ex{ExistsSubquery{box<QuerySpec>(inner), true},
          metadata::ColumnType::BOOL};
  REQUIRE(render(ex, sql_dialect::pg_dialect()) ==
          "(NOT EXISTS (SELECT t1.ref AS c0 FROM t2_table t1))");

  QuerySpec one = inner;
  one.limit = 1;
  Expr sc{ScalarSubquery{box<QuerySpec>(one)}, metadata::ColumnType::INT};
  REQUIRE(render(sc, sql_dialect::pg_dialect()) ==
          "(SELECT t1.ref AS c0 FROM t2_table t1 LIMIT 1)");
}

TEST_CASE("render aggregates and having", "[querygen]") {
  using namespace querygen;
  QuerySpec q;
  q.from = {"t1_table", "t0"};
  q.selectItems.push_back({col("t0", "a"), "c0"});
  AggCall cnt{AggFunc::count, {}, false};
  q.selectItems.push_back({Expr{cnt, metadata::ColumnType::INT}, "c1"});
  q.groupBy.push_back({"t0", "a"});
  AggCall sum{AggFunc::sum, box<Expr>(col("t0", "b")), true};
  q.having = box<Expr>(Expr{
      BinaryExpr{BinOp::gt, box<Expr>(Expr{sum, metadata::ColumnType::INT}),
                 box<Expr>(lit(0))},
      metadata::ColumnType::BOOL});
  REQUIRE(render(q, sql_dialect::pg_dialect()) ==
          "SELECT t0.a AS c0, COUNT(*) AS c1 FROM t1_table t0 "
          "GROUP BY t0.a HAVING (SUM(DISTINCT t0.b) > 0)");
}

TEST_CASE("render cte and set op", "[querygen]") {
  using namespace querygen;
  QuerySpec base;
  base.from = {"t1_table", "t0"};
  base.selectItems.push_back({col("t0", "id"), "c0"});

  QuerySpec q;
  q.ctes.push_back({"w0", box<QuerySpec>(base)});
  q.from = {"w0", "t1"};
  q.selectItems.push_back({col("t1", "c0"), "c0"});
  q.setOpRhs = box<QuerySpec>(base);
  q.setOpKind = SetOpKind::unionDistinct;
  q.limit = 5;
  REQUIRE(render(q, sql_dialect::pg_dialect()) ==
          "WITH w0 AS (SELECT t0.id AS c0 FROM t1_table t0) "
          "(SELECT t1.c0 AS c0 FROM w0 t1) UNION "
          "(SELECT t0.id AS c0 FROM t1_table t0) LIMIT 5");

  QuerySpec q2;
  q2.from = {"t1_table", "t0"};
  q2.selectItems.push_back({col("t0", "id"), "c0"});
  q2.setOpRhs = box<QuerySpec>(base);
  q2.setOpKind = SetOpKind::unionAll;
  REQUIRE(render(q2, sql_dialect::pg_dialect()) ==
          "(SELECT t0.id AS c0 FROM t1_table t0) UNION ALL "
          "(SELECT t0.id AS c0 FROM t1_table t0)");
  q2.setOpKind = SetOpKind::intersect;
  REQUIRE(render(q2, sql_dialect::pg_dialect()) ==
          "(SELECT t0.id AS c0 FROM t1_table t0) INTERSECT "
          "(SELECT t0.id AS c0 FROM t1_table t0)");
  q2.setOpKind = SetOpKind::except;
  REQUIRE(render(q2, sql_dialect::pg_dialect()) ==
          "(SELECT t0.id AS c0 FROM t1_table t0) EXCEPT "
          "(SELECT t0.id AS c0 FROM t1_table t0)");
}

TEST_CASE("render window call", "[querygen]") {
  using namespace querygen;
  WindowCall w{WinFunc::rowNumber, {}, {{"t0", "a"}}, {{"t0", "id"}}};
  REQUIRE(
      render(Expr{w, metadata::ColumnType::INT}, sql_dialect::pg_dialect()) ==
      "ROW_NUMBER() OVER (PARTITION BY t0.a ORDER BY t0.id)");

  WindowCall cs{WinFunc::count, {}, {}, {{"t0", "id"}}};
  REQUIRE(
      render(Expr{cs, metadata::ColumnType::INT}, sql_dialect::pg_dialect()) ==
      "COUNT(*) OVER (ORDER BY t0.id)");

  WindowCall sm{WinFunc::sum, box<Expr>(col("t0", "b")), {{"t0", "a"}}, {}};
  REQUIRE(
      render(Expr{sm, metadata::ColumnType::INT}, sql_dialect::pg_dialect()) ==
      "SUM(t0.b) OVER (PARTITION BY t0.a)");
}
