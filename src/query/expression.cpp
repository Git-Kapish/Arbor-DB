#include "query/expression.h"

#include <set>

namespace arbordb {

namespace {
const char* OpText(BinaryOp op) {
    switch (op) {
        case BinaryOp::kAdd: return "+";
        case BinaryOp::kSub: return "-";
        case BinaryOp::kMul: return "*";
        case BinaryOp::kDiv: return "/";
        case BinaryOp::kMod: return "%";
        case BinaryOp::kEq: return "=";
        case BinaryOp::kNe: return "<>";
        case BinaryOp::kLt: return "<";
        case BinaryOp::kLe: return "<=";
        case BinaryOp::kGt: return ">";
        case BinaryOp::kGe: return ">=";
        case BinaryOp::kAnd: return "AND";
        case BinaryOp::kOr: return "OR";
    }
    return "?";
}

class Printer : public ExprVisitor {
public:
    std::string out;
    void Visit(const ColumnRefExpr& e) override { out += e.qualifier.empty() ? e.name : e.qualifier + "." + e.name; }
    void Visit(const LiteralExpr& e) override {
        if (e.value.index() == 3) out += "'" + std::get<std::string>(e.value) + "'";
        else out += ValueToString(e.value);
    }
    void Visit(const BinaryExpr& e) override {
        out += "(";
        e.left->Accept(*this);
        out += std::string(" ") + OpText(e.op) + " ";
        e.right->Accept(*this);
        out += ")";
    }
    void Visit(const UnaryExpr& e) override {
        switch (e.op) {
            case UnaryOp::kNot: out += "NOT "; e.operand->Accept(*this); break;
            case UnaryOp::kNeg: out += "-"; e.operand->Accept(*this); break;
            case UnaryOp::kIsNull: e.operand->Accept(*this); out += " IS NULL"; break;
            case UnaryOp::kIsNotNull: e.operand->Accept(*this); out += " IS NOT NULL"; break;
        }
    }
    void Visit(const AggregateExpr& e) override {
        out += std::string(AggFuncName(e.func)) + "(";
        if (e.arg) e.arg->Accept(*this);
        else out += "*";
        out += ")";
    }
};

class ColumnCollector : public ExprWalker {
public:
    using ExprWalker::Visit;
    std::vector<const ColumnRefExpr*> cols;
    void Visit(const ColumnRefExpr& e) override { cols.push_back(&e); }
};

class AggDetector : public ExprWalker {
public:
    using ExprWalker::Visit;
    bool found = false;
    void Visit(const AggregateExpr&) override { found = true; }
};

Value Arith(BinaryOp op, const Value& a, const Value& b) {
    bool numeric = (a.index() == 1 || a.index() == 2) && (b.index() == 1 || b.index() == 2);
    if (!numeric) throw ExecutionError("arithmetic requires numeric operands");
    if (a.index() == 1 && b.index() == 1) {
        int64_t x = std::get<int64_t>(a), y = std::get<int64_t>(b);
        switch (op) {
            case BinaryOp::kAdd: return Value(x + y);
            case BinaryOp::kSub: return Value(x - y);
            case BinaryOp::kMul: return Value(x * y);
            case BinaryOp::kDiv:
                if (y == 0) throw ExecutionError("division by zero");
                return Value(x / y);
            default:
                if (y == 0) throw ExecutionError("division by zero");
                return Value(x % y);
        }
    }
    double x = a.index() == 1 ? static_cast<double>(std::get<int64_t>(a)) : std::get<double>(a);
    double y = b.index() == 1 ? static_cast<double>(std::get<int64_t>(b)) : std::get<double>(b);
    switch (op) {
        case BinaryOp::kAdd: return Value(x + y);
        case BinaryOp::kSub: return Value(x - y);
        case BinaryOp::kMul: return Value(x * y);
        case BinaryOp::kDiv:
            if (y == 0) throw ExecutionError("division by zero");
            return Value(x / y);
        default: throw ExecutionError("% requires integer operands");
    }
}
}  // namespace

const char* AggFuncName(AggFunc f) {
    switch (f) {
        case AggFunc::kCount: return "COUNT";
        case AggFunc::kSum: return "SUM";
        case AggFunc::kMin: return "MIN";
        case AggFunc::kMax: return "MAX";
        case AggFunc::kAvg: return "AVG";
    }
    return "?";
}

std::string ExprToString(const Expr& e) {
    Printer p;
    e.Accept(p);
    return p.out;
}

ExprPtr RewriteExpr(const ExprPtr& e, const std::function<ExprPtr(const ExprPtr&)>& f) {
    if (ExprPtr r = f(e)) return r;
    switch (e->kind) {
        case ExprKind::kBinary: {
            auto& b = static_cast<const BinaryExpr&>(*e);
            return std::make_shared<BinaryExpr>(b.op, RewriteExpr(b.left, f), RewriteExpr(b.right, f));
        }
        case ExprKind::kUnary: {
            auto& u = static_cast<const UnaryExpr&>(*e);
            return std::make_shared<UnaryExpr>(u.op, RewriteExpr(u.operand, f));
        }
        case ExprKind::kAggregate: {
            auto& a = static_cast<const AggregateExpr&>(*e);
            return std::make_shared<AggregateExpr>(a.func, a.arg ? RewriteExpr(a.arg, f) : nullptr);
        }
        default: return e;
    }
}

std::vector<ExprPtr> SplitConjuncts(const ExprPtr& e) {
    std::vector<ExprPtr> out;
    if (!e) return out;
    if (e->kind == ExprKind::kBinary) {
        auto& b = static_cast<const BinaryExpr&>(*e);
        if (b.op == BinaryOp::kAnd) {
            auto l = SplitConjuncts(b.left), r = SplitConjuncts(b.right);
            out.insert(out.end(), l.begin(), l.end());
            out.insert(out.end(), r.begin(), r.end());
            return out;
        }
    }
    out.push_back(e);
    return out;
}

ExprPtr AndAll(const std::vector<ExprPtr>& parts) {
    ExprPtr acc;
    for (auto& p : parts) acc = acc ? Bin(BinaryOp::kAnd, acc, p) : p;
    return acc;
}

std::vector<const ColumnRefExpr*> CollectColumns(const Expr& e) {
    ColumnCollector c;
    e.Accept(c);
    return c.cols;
}

bool ContainsAggregate(const Expr& e) {
    AggDetector d;
    e.Accept(d);
    return d.found;
}

void CollectAggregates(const ExprPtr& e, std::vector<ExprPtr>* out) {
    if (e->kind == ExprKind::kAggregate) {
        std::string key = ExprToString(*e);
        for (auto& x : *out)
            if (ExprToString(*x) == key) return;
        out->push_back(e);
        return;
    }
    if (e->kind == ExprKind::kBinary) {
        auto& b = static_cast<const BinaryExpr&>(*e);
        CollectAggregates(b.left, out);
        CollectAggregates(b.right, out);
    } else if (e->kind == ExprKind::kUnary) {
        CollectAggregates(static_cast<const UnaryExpr&>(*e).operand, out);
    }
}

Value EvaluateExpr(const Expr& e, const Tuple& t, const Schema& s) {
    switch (e.kind) {
        case ExprKind::kColumn: {
            auto& c = static_cast<const ColumnRefExpr&>(e);
            int idx = s.Find(c.qualifier, c.name);
            if (idx == Schema::kAmbiguous) throw ExecutionError("ambiguous column: " + ExprToString(e));
            if (idx < 0) throw ExecutionError("unknown column: " + ExprToString(e));
            return t.values[idx];
        }
        case ExprKind::kLiteral: return static_cast<const LiteralExpr&>(e).value;
        case ExprKind::kAggregate: throw ExecutionError("aggregate used outside an aggregation: " + ExprToString(e));
        case ExprKind::kUnary: {
            auto& u = static_cast<const UnaryExpr&>(e);
            Value v = EvaluateExpr(*u.operand, t, s);
            switch (u.op) {
                case UnaryOp::kIsNull: return Value(IsNull(v));
                case UnaryOp::kIsNotNull: return Value(!IsNull(v));
                case UnaryOp::kNot:
                    if (IsNull(v)) return v;
                    if (v.index() != 4) throw ExecutionError("NOT requires a boolean");
                    return Value(!std::get<bool>(v));
                case UnaryOp::kNeg:
                    if (IsNull(v)) return v;
                    if (v.index() == 1) return Value(-std::get<int64_t>(v));
                    if (v.index() == 2) return Value(-std::get<double>(v));
                    throw ExecutionError("unary minus requires a number");
            }
            break;
        }
        case ExprKind::kBinary: {
            auto& b = static_cast<const BinaryExpr&>(e);
            if (b.op == BinaryOp::kAnd || b.op == BinaryOp::kOr) {
                bool is_and = b.op == BinaryOp::kAnd;
                Value l = EvaluateExpr(*b.left, t, s);
                auto truth = [&](const Value& v) -> int {  // 1 true, 0 false, -1 null
                    if (IsNull(v)) return -1;
                    if (v.index() != 4) throw ExecutionError("AND/OR require booleans");
                    return std::get<bool>(v);
                };
                int lt = truth(l);
                if (is_and && lt == 0) return Value(false);
                if (!is_and && lt == 1) return Value(true);
                int rt = truth(EvaluateExpr(*b.right, t, s));
                if (is_and && rt == 0) return Value(false);
                if (!is_and && rt == 1) return Value(true);
                if (lt == -1 || rt == -1) return Value();
                return Value(is_and);
            }
            Value l = EvaluateExpr(*b.left, t, s), r = EvaluateExpr(*b.right, t, s);
            if (IsNull(l) || IsNull(r)) return Value();
            switch (b.op) {
                case BinaryOp::kEq: return Value(CompareValues(l, r) == 0);
                case BinaryOp::kNe: return Value(CompareValues(l, r) != 0);
                case BinaryOp::kLt: return Value(CompareValues(l, r) < 0);
                case BinaryOp::kLe: return Value(CompareValues(l, r) <= 0);
                case BinaryOp::kGt: return Value(CompareValues(l, r) > 0);
                case BinaryOp::kGe: return Value(CompareValues(l, r) >= 0);
                default: return Arith(b.op, l, r);
            }
        }
    }
    throw ExecutionError("bad expression");
}

TypeId InferType(const Expr& e, const Schema& schema) {
    switch (e.kind) {
        case ExprKind::kColumn: {
            auto& c = static_cast<const ColumnRefExpr&>(e);
            int idx = schema.Find(c.qualifier, c.name);
            if (idx < 0) throw BindError("unknown column: " + ExprToString(e));
            return schema.At(idx).type;
        }
        case ExprKind::kLiteral: {
            switch (static_cast<const LiteralExpr&>(e).value.index()) {
                case 2: return TypeId::kDouble;
                case 3: return TypeId::kVarchar;
                case 4: return TypeId::kBoolean;
                default: return TypeId::kInteger;
            }
        }
        case ExprKind::kUnary: {
            auto& u = static_cast<const UnaryExpr&>(e);
            return u.op == UnaryOp::kNeg ? InferType(*u.operand, schema) : TypeId::kBoolean;
        }
        case ExprKind::kBinary: {
            auto& b = static_cast<const BinaryExpr&>(e);
            if (b.op >= BinaryOp::kEq) return TypeId::kBoolean;
            TypeId l = InferType(*b.left, schema), r = InferType(*b.right, schema);
            return (l == TypeId::kDouble || r == TypeId::kDouble) ? TypeId::kDouble : TypeId::kInteger;
        }
        case ExprKind::kAggregate: {
            auto& a = static_cast<const AggregateExpr&>(e);
            if (a.func == AggFunc::kCount) return TypeId::kInteger;
            if (a.func == AggFunc::kAvg) return TypeId::kDouble;
            return InferType(*a.arg, schema);
        }
    }
    return TypeId::kInteger;
}

}  // namespace arbordb
