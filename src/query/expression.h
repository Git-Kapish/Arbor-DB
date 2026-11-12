#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "catalog/value.h"

namespace arbordb {

enum class ExprKind { kColumn, kLiteral, kBinary, kUnary, kAggregate };
enum class BinaryOp { kAdd, kSub, kMul, kDiv, kMod, kEq, kNe, kLt, kLe, kGt, kGe, kAnd, kOr };
enum class UnaryOp { kNot, kNeg, kIsNull, kIsNotNull };
enum class AggFunc { kCount, kSum, kMin, kMax, kAvg };

struct ColumnRefExpr;
struct LiteralExpr;
struct BinaryExpr;
struct UnaryExpr;
struct AggregateExpr;

// Visitor pattern over expression trees: tree-walking logic (name resolution, column collection,
// aggregate extraction, ...) lives in visitors instead of being baked into the node classes.
class ExprVisitor {
public:
    virtual ~ExprVisitor() = default;
    virtual void Visit(const ColumnRefExpr&) = 0;
    virtual void Visit(const LiteralExpr&) = 0;
    virtual void Visit(const BinaryExpr&) = 0;
    virtual void Visit(const UnaryExpr&) = 0;
    virtual void Visit(const AggregateExpr&) = 0;
};

struct Expr {
    explicit Expr(ExprKind k) : kind(k) {}
    virtual ~Expr() = default;
    virtual void Accept(ExprVisitor& v) const = 0;
    ExprKind kind;
};
using ExprPtr = std::shared_ptr<const Expr>;

struct ColumnRefExpr : Expr {
    ColumnRefExpr(std::string q, std::string n) : Expr(ExprKind::kColumn), qualifier(std::move(q)), name(std::move(n)) {}
    void Accept(ExprVisitor& v) const override { v.Visit(*this); }
    std::string qualifier, name;
};
struct LiteralExpr : Expr {
    explicit LiteralExpr(Value v) : Expr(ExprKind::kLiteral), value(std::move(v)) {}
    void Accept(ExprVisitor& v) const override { v.Visit(*this); }
    Value value;
};
struct BinaryExpr : Expr {
    BinaryExpr(BinaryOp o, ExprPtr l, ExprPtr r) : Expr(ExprKind::kBinary), op(o), left(std::move(l)), right(std::move(r)) {}
    void Accept(ExprVisitor& v) const override { v.Visit(*this); }
    BinaryOp op;
    ExprPtr left, right;
};
struct UnaryExpr : Expr {
    UnaryExpr(UnaryOp o, ExprPtr e) : Expr(ExprKind::kUnary), op(o), operand(std::move(e)) {}
    void Accept(ExprVisitor& v) const override { v.Visit(*this); }
    UnaryOp op;
    ExprPtr operand;
};
struct AggregateExpr : Expr {
    AggregateExpr(AggFunc f, ExprPtr a) : Expr(ExprKind::kAggregate), func(f), arg(std::move(a)) {}
    void Accept(ExprVisitor& v) const override { v.Visit(*this); }
    AggFunc func;
    ExprPtr arg;  // nullptr means COUNT(*)
};

// Base visitor that recurses into children; override Visit for the node kinds you care about.
class ExprWalker : public ExprVisitor {
public:
    void Visit(const ColumnRefExpr&) override {}
    void Visit(const LiteralExpr&) override {}
    void Visit(const BinaryExpr& e) override { e.left->Accept(*this); e.right->Accept(*this); }
    void Visit(const UnaryExpr& e) override { e.operand->Accept(*this); }
    void Visit(const AggregateExpr& e) override { if (e.arg) e.arg->Accept(*this); }
};

inline ExprPtr Col(std::string q, std::string n) { return std::make_shared<ColumnRefExpr>(std::move(q), std::move(n)); }
inline ExprPtr Lit(Value v) { return std::make_shared<LiteralExpr>(std::move(v)); }
inline ExprPtr Bin(BinaryOp op, ExprPtr l, ExprPtr r) { return std::make_shared<BinaryExpr>(op, std::move(l), std::move(r)); }

std::string ExprToString(const Expr& e);  // canonical text, also used as a structural-equality key
const char* AggFuncName(AggFunc f);

// Rebuilds the tree top-down: where f(node) returns non-null that replacement is used, otherwise the node
// is copied with rewritten children.
ExprPtr RewriteExpr(const ExprPtr& e, const std::function<ExprPtr(const ExprPtr&)>& f);

std::vector<ExprPtr> SplitConjuncts(const ExprPtr& e);       // a AND (b AND c) -> [a, b, c]
ExprPtr AndAll(const std::vector<ExprPtr>& parts);            // nullptr if empty
std::vector<const ColumnRefExpr*> CollectColumns(const Expr& e);
bool ContainsAggregate(const Expr& e);
void CollectAggregates(const ExprPtr& e, std::vector<ExprPtr>* out);  // distinct by ExprToString, in first-seen order

// SQL three-valued evaluation. Aggregates must already have been replaced by column references.
Value EvaluateExpr(const Expr& e, const Tuple& tuple, const Schema& schema);
inline bool IsTrue(const Value& v) { return v.index() == 4 && std::get<bool>(v); }

// Static result type of an expression against an input schema.
TypeId InferType(const Expr& e, const Schema& schema);

}  // namespace arbordb
