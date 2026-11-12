#include "query/binder/binder.h"

#include <algorithm>

namespace arbordb {

namespace {
// Visitor: every column reference in a tree must resolve in `scope`.
class ScopeChecker : public ExprWalker {
public:
    using ExprWalker::Visit;
    ScopeChecker(const Schema& s, bool aggregated) : scope(s), aggregated_(aggregated) {}
    void Visit(const ColumnRefExpr& c) override {
        if (scope.Find(c.qualifier, c.name) < 0) {
            std::string col = c.qualifier.empty() ? c.name : c.qualifier + "." + c.name;
            if (aggregated_)
                throw BindError("column \"" + col + "\" must appear in the GROUP BY clause or be used in an aggregate function");
            throw BindError("unknown column: " + col);
        }
    }
    const Schema& scope;

private:
    bool aggregated_;
};
}  // namespace

TableInfo* Binder::MustGetTable(const std::string& name) {
    TableInfo* t = catalog_->GetTable(name);
    if (!t) throw BindError("no such table: " + name);
    return t;
}

PlanPtr Binder::MakeScan(TableInfo* t, const std::string& alias) {
    auto n = std::make_shared<PlanNode>(PlanType::kScan);
    n->table = t;
    n->alias = alias;
    n->schema = t->schema.WithQualifier(alias);
    for (std::size_t i = 0; i < t->schema.Size(); i++) n->scan_columns.push_back(static_cast<int>(i));
    return n;
}

ExprPtr Binder::Qualify(const ExprPtr& e, const Schema& scope) {
    return RewriteExpr(e, [&](const ExprPtr& x) -> ExprPtr {
        if (x->kind != ExprKind::kColumn) return nullptr;
        auto& c = static_cast<const ColumnRefExpr&>(*x);
        int idx = scope.Find(c.qualifier, c.name);
        std::string shown = c.qualifier.empty() ? c.name : c.qualifier + "." + c.name;
        if (idx == Schema::kAmbiguous) throw BindError("ambiguous column reference: " + shown);
        if (idx < 0) throw BindError("unknown column: " + shown);
        return Col(scope.At(idx).qualifier, c.name);
    });
}

PlanPtr Binder::BindSelect(const SelectStmt& s) {
    // ---- FROM / JOIN ----
    std::vector<std::string> aliases;
    auto add_alias = [&](const std::string& a) {
        if (std::find(aliases.begin(), aliases.end(), a) != aliases.end()) throw BindError("duplicate table alias: " + a);
        aliases.push_back(a);
    };
    add_alias(s.from.alias);
    PlanPtr plan = MakeScan(MustGetTable(s.from.table), s.from.alias);
    for (auto& j : s.joins) {
        add_alias(j.ref.alias);
        PlanPtr right = MakeScan(MustGetTable(j.ref.table), j.ref.alias);
        auto join = std::make_shared<PlanNode>(PlanType::kJoin);
        join->schema = Schema::Concat(plan->schema, right->schema);
        if (j.on) {
            if (ContainsAggregate(*j.on)) throw BindError("aggregates are not allowed in JOIN ... ON");
            join->predicate = Qualify(j.on, join->schema);
        }
        join->children = {plan, right};
        plan = join;
    }
    const Schema scope = plan->schema;

    // ---- WHERE ----
    if (s.where) {
        if (ContainsAggregate(*s.where)) throw BindError("aggregates are not allowed in WHERE (use HAVING)");
        auto f = std::make_shared<PlanNode>(PlanType::kFilter);
        f->predicate = Qualify(s.where, scope);
        f->schema = plan->schema;
        f->children = {plan};
        plan = f;
    }

    // ---- select list ----
    struct Item { ExprPtr expr; std::string name; bool explicit_alias; };
    std::vector<Item> items;
    for (auto& it : s.items) {
        if (it.star) {
            bool any = false;
            for (auto& c : scope.Columns()) {
                if (!it.star_qualifier.empty() && c.qualifier != it.star_qualifier) continue;
                items.push_back({Col(c.qualifier, c.name), c.name, false});
                any = true;
            }
            if (!any) throw BindError("no such table in FROM: " + it.star_qualifier);
        } else {
            ExprPtr e = Qualify(it.expr, scope);
            std::string name = !it.alias.empty() ? it.alias
                               : (e->kind == ExprKind::kColumn ? static_cast<const ColumnRefExpr&>(*e).name : ExprToString(*e));
            items.push_back({e, name, !it.alias.empty()});
        }
    }

    // ---- GROUP BY / aggregates ----
    std::vector<ExprPtr> group;
    for (auto& g : s.group_by) {
        if (ContainsAggregate(*g)) throw BindError("aggregates are not allowed in GROUP BY");
        group.push_back(Qualify(g, scope));
    }
    ExprPtr having = s.having ? Qualify(s.having, scope) : nullptr;

    // ORDER BY: ordinal position, select-list alias, or an expression over the input.
    std::vector<OrderItem> order;
    for (auto& o : s.order_by) {
        ExprPtr e;
        if (o.expr->kind == ExprKind::kLiteral && static_cast<const LiteralExpr&>(*o.expr).value.index() == 1) {
            int64_t n = std::get<int64_t>(static_cast<const LiteralExpr&>(*o.expr).value);
            if (n < 1 || n > static_cast<int64_t>(items.size())) throw BindError("ORDER BY position out of range: " + std::to_string(n));
            e = items[n - 1].expr;
        } else if (o.expr->kind == ExprKind::kColumn && static_cast<const ColumnRefExpr&>(*o.expr).qualifier.empty()) {
            const std::string& nm = static_cast<const ColumnRefExpr&>(*o.expr).name;
            for (auto& it : items)
                if (it.explicit_alias && it.name == nm) { e = it.expr; break; }
        }
        if (!e) e = Qualify(o.expr, scope);
        order.push_back({e, o.desc});
    }

    bool aggregated = !group.empty() || having;
    for (auto& it : items) aggregated |= ContainsAggregate(*it.expr);
    for (auto& o : order) aggregated |= ContainsAggregate(*o.expr);

    if (aggregated) {
        std::vector<ExprPtr> aggs;
        for (auto& it : items) CollectAggregates(it.expr, &aggs);
        if (having) CollectAggregates(having, &aggs);
        for (auto& o : order) CollectAggregates(o.expr, &aggs);

        auto agg = std::make_shared<PlanNode>(PlanType::kAggregation);
        agg->group_by = group;
        agg->aggregates = aggs;
        for (auto& g : group) {
            if (g->kind == ExprKind::kColumn) {
                auto& c = static_cast<const ColumnRefExpr&>(*g);
                agg->schema.Add(Column{c.name, InferType(*g, scope), c.qualifier});  // keeps its identity
            } else {
                agg->schema.Add(Column{ExprToString(*g), InferType(*g, scope), ""});
            }
        }
        for (auto& a : aggs) {
            auto& ae = static_cast<const AggregateExpr&>(*a);
            if (ae.arg) {
                TypeId t = InferType(*ae.arg, scope);
                if ((ae.func == AggFunc::kSum || ae.func == AggFunc::kAvg) && t != TypeId::kInteger && t != TypeId::kDouble)
                    throw BindError(std::string(AggFuncName(ae.func)) + " requires a numeric argument");
            }
            agg->schema.Add(Column{ExprToString(*a), InferType(*a, scope), ""});
        }
        agg->children = {plan};
        plan = agg;

        // Replace group-by expressions and aggregates by references to the aggregation's output columns.
        auto rewrite = [&](const ExprPtr& e) {
            ExprPtr r = RewriteExpr(e, [&](const ExprPtr& x) -> ExprPtr {
                std::string key = ExprToString(*x);
                for (auto& g : group)
                    if (ExprToString(*g) == key) return g->kind == ExprKind::kColumn ? g : Col("", key);
                if (x->kind == ExprKind::kAggregate) return Col("", key);
                return nullptr;
            });
            ScopeChecker chk(agg->schema, true);
            r->Accept(chk);
            return r;
        };
        for (auto& it : items) it.expr = rewrite(it.expr);
        for (auto& o : order) o.expr = rewrite(o.expr);
        if (having) {
            auto f = std::make_shared<PlanNode>(PlanType::kFilter);
            f->predicate = rewrite(having);
            f->schema = plan->schema;
            f->children = {plan};
            plan = f;
        }
    }

    if (!order.empty()) {
        auto sort = std::make_shared<PlanNode>(PlanType::kSort);
        sort->order = order;
        sort->schema = plan->schema;
        sort->children = {plan};
        plan = sort;
    }

    auto proj = std::make_shared<PlanNode>(PlanType::kProjection);
    for (auto& it : items) {
        proj->exprs.push_back(it.expr);
        proj->schema.Add(Column{it.name, InferType(*it.expr, plan->schema), ""});
    }
    proj->children = {plan};
    plan = proj;

    if (s.limit) {
        auto lim = std::make_shared<PlanNode>(PlanType::kLimit);
        lim->limit = *s.limit;
        lim->schema = plan->schema;
        lim->children = {plan};
        plan = lim;
    }
    return plan;
}

PlanPtr Binder::BindInsert(const InsertStmt& s) {
    TableInfo* t = MustGetTable(s.table);
    std::vector<int> targets;  // table column index for each position in a VALUES row
    if (s.columns.empty()) {
        for (std::size_t i = 0; i < t->schema.Size(); i++) targets.push_back(static_cast<int>(i));
    } else {
        for (auto& c : s.columns) {
            int idx = t->schema.Find("", c);
            if (idx < 0) throw BindError("no such column: " + c);
            if (std::find(targets.begin(), targets.end(), idx) != targets.end()) throw BindError("column listed twice: " + c);
            targets.push_back(idx);
        }
    }
    auto n = std::make_shared<PlanNode>(PlanType::kInsert);
    n->table = t;
    for (auto& row : s.rows) {
        if (row.size() != targets.size())
            throw BindError("INSERT has " + std::to_string(row.size()) + " values but " + std::to_string(targets.size()) + " columns");
        std::vector<Value> out(t->schema.Size());
        for (std::size_t i = 0; i < row.size(); i++) {
            Value v;
            try {
                v = EvaluateExpr(*row[i], Tuple{}, Schema{});
            } catch (const ExecutionError& e) {
                throw BindError(std::string("INSERT values must be constant expressions: ") + e.what());
            }
            try {
                out[targets[i]] = CoerceToType(v, t->schema.At(targets[i]).type);
            } catch (const ExecutionError& e) {
                throw BindError(e.what());
            }
        }
        n->rows.push_back(std::move(out));
    }
    n->schema = Schema({Column{"rows_affected", TypeId::kInteger, ""}});
    return n;
}

PlanPtr Binder::BindUpdate(const UpdateStmt& s) {
    TableInfo* t = MustGetTable(s.table);
    PlanPtr child = MakeScan(t, t->name);
    child->for_update = true;
    const Schema scope = child->schema;
    if (s.where) {
        if (ContainsAggregate(*s.where)) throw BindError("aggregates are not allowed in WHERE");
        auto f = std::make_shared<PlanNode>(PlanType::kFilter);
        f->predicate = Qualify(s.where, scope);
        f->schema = scope;
        f->children = {child};
        child = f;
    }
    auto n = std::make_shared<PlanNode>(PlanType::kUpdate);
    n->table = t;
    for (auto& a : s.assignments) {
        int idx = t->schema.Find("", a.column);
        if (idx < 0) throw BindError("no such column: " + a.column);
        if (ContainsAggregate(*a.value)) throw BindError("aggregates are not allowed in SET");
        for (auto& [i, e] : n->assignments)
            if (i == idx) throw BindError("column assigned twice: " + a.column);
        n->assignments.push_back({idx, Qualify(a.value, scope)});
    }
    n->children = {child};
    n->schema = Schema({Column{"rows_affected", TypeId::kInteger, ""}});
    return n;
}

PlanPtr Binder::BindDelete(const DeleteStmt& s) {
    TableInfo* t = MustGetTable(s.table);
    PlanPtr child = MakeScan(t, t->name);
    child->for_update = true;
    if (s.where) {
        if (ContainsAggregate(*s.where)) throw BindError("aggregates are not allowed in WHERE");
        auto f = std::make_shared<PlanNode>(PlanType::kFilter);
        f->predicate = Qualify(s.where, child->schema);
        f->schema = child->schema;
        f->children = {child};
        child = f;
    }
    auto n = std::make_shared<PlanNode>(PlanType::kDelete);
    n->table = t;
    n->children = {child};
    n->schema = Schema({Column{"rows_affected", TypeId::kInteger, ""}});
    return n;
}

}  // namespace arbordb
