#pragma once

#include "metadata/table.hpp"
#include "sql_dialect/lock_clause.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace querygen {

// deep-copying single-owner pointer; IR trees stay value types so
// oracle transforms can copy-and-mutate freely
template <typename T> class box {
public:
  box() = default;
  box(T v) : p_(std::make_unique<T>(std::move(v))) {}
  box(box const &o) : p_(o.p_ ? std::make_unique<T>(*o.p_) : nullptr) {}
  box &operator=(box const &o) {
    if (this == &o) {
      return *this;
    }
    p_ = o.p_ ? std::make_unique<T>(*o.p_) : nullptr;
    return *this;
  }
  box(box &&) noexcept = default;
  box &operator=(box &&) noexcept = default;

  explicit operator bool() const { return p_ != nullptr; }
  T &operator*() { return *p_; }
  T const &operator*() const { return *p_; }
  T *operator->() { return p_.get(); }
  T const *operator->() const { return p_.get(); }

private:
  std::unique_ptr<T> p_;
};

enum class BinOp : std::uint8_t {
  eq,
  ne,
  lt,
  le,
  gt,
  ge, // comparison -> BOOL
  add,
  sub,
  mul,
  div_,
  mod, // numeric
  and_,
  or_,   // BOOL x BOOL
  like,  // string -> BOOL
  concat // string
};

enum class UnOp : std::uint8_t { not_, isNull, isNotNull, neg };

enum class Func : std::uint8_t { lower, upper, length, abs, coalesce, nullif };

enum class AggFunc : std::uint8_t { count, sum, min, max, avg };

enum class WinFunc : std::uint8_t {
  rowNumber,
  rank,
  denseRank,
  count,
  sum,
  min,
  max,
  avg
};

enum class SetOpKind : std::uint8_t {
  unionAll,
  unionDistinct,
  intersect,
  except
};

enum class JoinKind : std::uint8_t { inner, left, right, cross };

struct Expr;
struct QuerySpec;

struct ColumnRef {
  std::string alias; // table alias or table name qualifier
  std::string column;
};

// monostate = NULL literal
using LiteralValue =
    std::variant<std::monostate, std::int64_t, double, std::string, bool>;

struct Literal {
  LiteralValue value;
};

struct UnaryExpr {
  UnOp op;
  box<Expr> arg;
};

struct BinaryExpr {
  BinOp op;
  box<Expr> lhs;
  box<Expr> rhs;
};

struct BetweenExpr {
  box<Expr> arg;
  box<Expr> lo;
  box<Expr> hi;
};

struct InListExpr {
  box<Expr> arg;
  std::vector<box<Expr>> items;
  bool negated = false;
};

struct InSubquery {
  box<Expr> arg;
  box<QuerySpec> sub; // must have exactly one select item
  bool negated = false;
};

struct ExistsSubquery {
  box<QuerySpec> sub;
  bool negated = false;
};

struct ScalarSubquery {
  box<QuerySpec> sub; // one select item, LIMIT 1
};

struct FuncCall {
  Func fn;
  std::vector<box<Expr>> args;
};

struct CaseWhen {
  box<Expr> when; // BOOL
  box<Expr> then;
};

struct CaseExpr {
  std::vector<CaseWhen> whens;
  box<Expr> elseExpr; // may be empty
};

struct AggCall {
  AggFunc fn;
  box<Expr> arg; // count with empty arg = COUNT(*)
  bool distinct = false;
};

struct WindowCall {
  WinFunc fn;
  box<Expr> arg; // empty for rowNumber/rank/denseRank; count with empty
                 // arg = COUNT(*)
  std::vector<ColumnRef> partitionBy;
  std::vector<ColumnRef> orderBy;
};

struct Expr {
  std::variant<ColumnRef, Literal, UnaryExpr, BinaryExpr, BetweenExpr,
               InListExpr, InSubquery, ExistsSubquery, ScalarSubquery, FuncCall,
               CaseExpr, AggCall, WindowCall>
      node;
  // result type; generator-assigned ground truth, transforms must keep it
  // consistent with node
  metadata::ColumnType type = metadata::ColumnType::INT;
};

struct TableSource {
  std::string table; // table or CTE name
  std::string alias;
};

struct Join {
  JoinKind kind;
  TableSource source;
  box<Expr> condition; // empty for cross
};

struct SelectItem {
  Expr expr;
  std::string colAlias; // c0, c1, ... stable output names
};

struct OrderItem {
  Expr expr;
  bool desc = false;
};

struct Cte {
  std::string name;
  box<QuerySpec> query;
};

struct LockSpec {
  sql_dialect::LockClause clause = sql_dialect::LockClause::none;
  std::string ofAlias; // empty = lock everything
};

struct QuerySpec {
  std::vector<Cte> ctes;
  TableSource from;
  std::vector<Join> joins;
  bool distinct = false;
  std::vector<SelectItem> selectItems;
  box<Expr> where; // BOOL, empty = no WHERE
  std::vector<ColumnRef> groupBy;
  box<Expr> having; // BOOL, empty = none
  std::vector<OrderItem> orderBy;
  std::optional<std::size_t> limit;
  std::optional<std::size_t> offset;
  LockSpec lock;
  // set operation tail: (this) <setOpKind> (setOpRhs)
  box<QuerySpec> setOpRhs;
  SetOpKind setOpKind = SetOpKind::unionAll;
};

} // namespace querygen
