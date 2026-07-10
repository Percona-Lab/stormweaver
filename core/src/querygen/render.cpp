#include "querygen/render.hpp"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <stdexcept>
#include <type_traits>

namespace querygen {

namespace {

std::string escapeStringLiteral(std::string const &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

std::string binOpToken(BinOp op) {
  switch (op) {
  case BinOp::eq:
    return "=";
  case BinOp::ne:
    return "<>";
  case BinOp::lt:
    return "<";
  case BinOp::le:
    return "<=";
  case BinOp::gt:
    return ">";
  case BinOp::ge:
    return ">=";
  case BinOp::add:
    return "+";
  case BinOp::sub:
    return "-";
  case BinOp::mul:
    return "*";
  case BinOp::div_:
    return "/";
  case BinOp::mod:
    return "%";
  case BinOp::and_:
    return "AND";
  case BinOp::or_:
    return "OR";
  case BinOp::like:
    return "LIKE";
  case BinOp::concat:
    break; // dialect renders concat directly, never turned into a token
  }
  throw std::logic_error("querygen render: concat has no token");
}

std::string funcName(Func fn) {
  switch (fn) {
  case Func::lower:
    return "LOWER";
  case Func::upper:
    return "UPPER";
  case Func::length:
    return "LENGTH";
  case Func::abs:
    return "ABS";
  case Func::coalesce:
    return "COALESCE";
  case Func::nullif:
    return "NULLIF";
  }
  throw std::logic_error("querygen render: unknown func");
}

std::string aggName(AggFunc fn) {
  switch (fn) {
  case AggFunc::count:
    return "COUNT";
  case AggFunc::sum:
    return "SUM";
  case AggFunc::min:
    return "MIN";
  case AggFunc::max:
    return "MAX";
  case AggFunc::avg:
    return "AVG";
  }
  throw std::logic_error("querygen render: unknown agg");
}

std::string winName(WinFunc fn) {
  switch (fn) {
  case WinFunc::rowNumber:
    return "ROW_NUMBER";
  case WinFunc::rank:
    return "RANK";
  case WinFunc::denseRank:
    return "DENSE_RANK";
  case WinFunc::count:
    return "COUNT";
  case WinFunc::sum:
    return "SUM";
  case WinFunc::min:
    return "MIN";
  case WinFunc::max:
    return "MAX";
  case WinFunc::avg:
    return "AVG";
  }
  throw std::logic_error("querygen render: unknown window func");
}

std::string joinToken(JoinKind kind) {
  switch (kind) {
  case JoinKind::inner:
    return "JOIN";
  case JoinKind::left:
    return "LEFT JOIN";
  case JoinKind::right:
    return "RIGHT JOIN";
  case JoinKind::cross:
    return "CROSS JOIN";
  }
  throw std::logic_error("querygen render: unknown join kind");
}

std::string setOpToken(SetOpKind kind) {
  switch (kind) {
  case SetOpKind::unionAll:
    return "UNION ALL";
  case SetOpKind::unionDistinct:
    return "UNION";
  case SetOpKind::intersect:
    return "INTERSECT";
  case SetOpKind::except:
    return "EXCEPT";
  }
  throw std::logic_error("querygen render: unknown set op");
}

// render each item through f and join with ", "
template <typename T, typename F>
std::string renderList(std::vector<T> const &items, F &&f) {
  std::vector<std::string> parts;
  parts.reserve(items.size());
  for (auto const &it : items) {
    parts.push_back(f(it));
  }
  return fmt::format("{}", fmt::join(parts, ", "));
}

// dialect is threaded through explicitly (rather than a Renderer class with a
// dialect member) so plain node functions stay free functions - keeps
// static/const-member clang-tidy noise out of a visitor with mixed
// dialect-dependent and dialect-independent cases
std::string exprStr(Expr const &e, sql_dialect::Dialect const &dialect);
std::string queryStr(QuerySpec const &q, sql_dialect::Dialect const &dialect);

std::string node(ColumnRef const &c, sql_dialect::Dialect const & /*dialect*/) {
  if (c.alias.empty()) {
    return c.column;
  }
  return fmt::format("{}.{}", c.alias, c.column);
}

std::string node(Literal const &l, sql_dialect::Dialect const & /*dialect*/) {
  return std::visit(
      [](auto const &v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return "NULL";
        } else if constexpr (std::is_same_v<T, std::string>) {
          return escapeStringLiteral(v);
        } else if constexpr (std::is_same_v<T, bool>) {
          return v ? "true" : "false";
        } else {
          return fmt::format("{}", v);
        }
      },
      l.value);
}

std::string node(UnaryExpr const &u, sql_dialect::Dialect const &dialect) {
  switch (u.op) {
  case UnOp::not_:
    return fmt::format("(NOT {})", exprStr(*u.arg, dialect));
  case UnOp::isNull:
    return fmt::format("({} IS NULL)", exprStr(*u.arg, dialect));
  case UnOp::isNotNull:
    return fmt::format("({} IS NOT NULL)", exprStr(*u.arg, dialect));
  case UnOp::neg:
    return fmt::format("(-{})", exprStr(*u.arg, dialect));
  }
  throw std::logic_error("querygen render: unknown unop");
}

std::string node(BinaryExpr const &b, sql_dialect::Dialect const &dialect) {
  std::string lhs = exprStr(*b.lhs, dialect);
  std::string rhs = exprStr(*b.rhs, dialect);
  if (b.op == BinOp::concat) {
    return dialect.concatExpr(lhs, rhs);
  }
  return fmt::format("({} {} {})", lhs, binOpToken(b.op), rhs);
}

std::string node(BetweenExpr const &b, sql_dialect::Dialect const &dialect) {
  return fmt::format("({} BETWEEN {} AND {})", exprStr(*b.arg, dialect),
                     exprStr(*b.lo, dialect), exprStr(*b.hi, dialect));
}

std::string node(InListExpr const &in, sql_dialect::Dialect const &dialect) {
  std::string items = renderList(
      in.items, [&](box<Expr> const &e) { return exprStr(*e, dialect); });
  return fmt::format("({} {}IN ({}))", exprStr(*in.arg, dialect),
                     in.negated ? "NOT " : "", items);
}

std::string node(InSubquery const &in, sql_dialect::Dialect const &dialect) {
  return fmt::format("({} {}IN ({}))", exprStr(*in.arg, dialect),
                     in.negated ? "NOT " : "", queryStr(*in.sub, dialect));
}

std::string node(ExistsSubquery const &ex,
                 sql_dialect::Dialect const &dialect) {
  return fmt::format("({}EXISTS ({}))", ex.negated ? "NOT " : "",
                     queryStr(*ex.sub, dialect));
}

std::string node(ScalarSubquery const &sc,
                 sql_dialect::Dialect const &dialect) {
  return fmt::format("({})", queryStr(*sc.sub, dialect));
}

std::string node(FuncCall const &f, sql_dialect::Dialect const &dialect) {
  std::string args = renderList(
      f.args, [&](box<Expr> const &e) { return exprStr(*e, dialect); });
  return fmt::format("{}({})", funcName(f.fn), args);
}

std::string node(CaseExpr const &c, sql_dialect::Dialect const &dialect) {
  std::string out = "CASE";
  for (auto const &w : c.whens) {
    out += fmt::format(" WHEN {} THEN {}", exprStr(*w.when, dialect),
                       exprStr(*w.then, dialect));
  }
  if (c.elseExpr) {
    out += fmt::format(" ELSE {}", exprStr(*c.elseExpr, dialect));
  }
  out += " END";
  return out;
}

std::string node(AggCall const &a, sql_dialect::Dialect const &dialect) {
  if (!a.arg) {
    return "COUNT(*)";
  }
  return fmt::format("{}({}{})", aggName(a.fn), a.distinct ? "DISTINCT " : "",
                     exprStr(*a.arg, dialect));
}

std::string node(WindowCall const &w, sql_dialect::Dialect const &dialect) {
  std::string arg;
  switch (w.fn) {
  case WinFunc::rowNumber:
  case WinFunc::rank:
  case WinFunc::denseRank:
    break;
  default:
    arg = w.arg ? exprStr(*w.arg, dialect) : "*";
  }

  std::vector<std::string> over;
  if (!w.partitionBy.empty()) {
    over.push_back(fmt::format(
        "PARTITION BY {}", renderList(w.partitionBy, [&](ColumnRef const &c) {
          return node(c, dialect);
        })));
  }
  if (!w.orderBy.empty()) {
    over.push_back(fmt::format("ORDER BY {}",
                               renderList(w.orderBy, [&](ColumnRef const &c) {
                                 return node(c, dialect);
                               })));
  }
  return fmt::format("{}({}) OVER ({})", winName(w.fn), arg,
                     fmt::join(over, " "));
}

std::string exprStr(Expr const &e, sql_dialect::Dialect const &dialect) {
  return std::visit([&](auto const &n) { return node(n, dialect); }, e.node);
}

// SELECT ... FROM ... joins ... WHERE ... GROUP BY ... HAVING; the tail
// (ORDER BY/LIMIT/OFFSET/lock) is separate so set ops can wrap the body only
std::string queryBody(QuerySpec const &q, sql_dialect::Dialect const &dialect) {
  std::string out = q.distinct ? "SELECT DISTINCT " : "SELECT ";
  out += renderList(q.selectItems, [&](SelectItem const &si) {
    return fmt::format("{} AS {}", exprStr(si.expr, dialect), si.colAlias);
  });
  out += fmt::format(" FROM {} {}", q.from.table, q.from.alias);

  for (auto const &j : q.joins) {
    out += fmt::format(" {} {} {}", joinToken(j.kind), j.source.table,
                       j.source.alias);
    if (j.kind != JoinKind::cross) {
      out += fmt::format(" ON {}", exprStr(*j.condition, dialect));
    }
  }

  if (q.where) {
    out += fmt::format(" WHERE {}", exprStr(*q.where, dialect));
  }
  if (!q.groupBy.empty()) {
    out += fmt::format(" GROUP BY {}",
                       renderList(q.groupBy, [&](ColumnRef const &c) {
                         return node(c, dialect);
                       }));
  }
  if (q.having) {
    out += fmt::format(" HAVING {}", exprStr(*q.having, dialect));
  }
  return out;
}

std::string queryStr(QuerySpec const &q, sql_dialect::Dialect const &dialect) {
  std::string out;
  if (!q.ctes.empty()) {
    out += fmt::format("WITH {} ", renderList(q.ctes, [&](Cte const &c) {
                         return fmt::format("{} AS ({})", c.name,
                                            queryStr(*c.query, dialect));
                       }));
  }

  if (q.setOpRhs) {
    out += fmt::format("({}) {} ({})", queryBody(q, dialect),
                       setOpToken(q.setOpKind), queryStr(*q.setOpRhs, dialect));
  } else {
    out += queryBody(q, dialect);
  }

  if (!q.orderBy.empty()) {
    out += fmt::format(
        " ORDER BY {}", renderList(q.orderBy, [&](OrderItem const &o) {
          std::string rendered = exprStr(o.expr, dialect);
          return o.desc ? fmt::format("{} DESC", rendered) : rendered;
        }));
  }
  if (q.limit) {
    out += fmt::format(" LIMIT {}", *q.limit);
  }
  if (q.offset) {
    out += fmt::format(" OFFSET {}", *q.offset);
  }
  out += q.lock.ofAlias.empty()
             ? std::string(sql_dialect::lockClauseSuffix(q.lock.clause))
             : sql_dialect::lockOfSuffix(q.lock.clause, q.lock.ofAlias);

  return out;
}

} // namespace

std::string render(Expr const &e, sql_dialect::Dialect const &dialect) {
  return exprStr(e, dialect);
}

std::string render(QuerySpec const &q, sql_dialect::Dialect const &dialect) {
  return queryStr(q, dialect);
}

} // namespace querygen
