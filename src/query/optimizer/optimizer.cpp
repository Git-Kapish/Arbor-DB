#include "query/optimizer/optimizer.h"

#include <algorithm>
#include <functional>
#include <set>
#include <sstream>

namespace arbordb {

namespace {
using AliasSet = std::set<std::string>;

AliasSet ExprAliases(const Expr& e) {
    AliasSet s;
    for (auto* c : CollectColumns(e)) s.insert(c->qualifier);
    return s;
}

void NodeAliases(const PlanNode& n, AliasSet* out) {
    if (!n.alias.empty()) out->insert(n.alias);
    for (auto& c : n.children) NodeAliases(*c, out);
}
AliasSet NodeAliases(const PlanNode& n) {
    AliasSet s;
    NodeAliases(n, &s);
    return s;
}
bool Subset(const AliasSet& a, const AliasSet& b) {
    return std::all_of(a.begin(), a.end(), [&](const std::string& x) { return b.count(x) > 0; });
}

// Recompute the schema of nodes whose output is derived from their children (after pruning/reordering).
void FixSchema(PlanNode& n) {
    for (auto& c : n.children) FixSchema(*c);
    switch (n.type) {
        case PlanType::kJoin:
        case PlanType::kNestedLoopJoin:
        case PlanType::kHashJoin: n.schema = Schema::Concat(n.children[0]->schema, n.children[1]->schema); break;
        case PlanType::kFilter:
        case PlanType::kSort:
        case PlanType::kLimit: n.schema = n.children[0]->schema; break;
        default: break;
    }
}

PlanPtr NewFilter(PlanPtr child, ExprPtr pred) {
    auto f = std::make_shared<PlanNode>(PlanType::kFilter);
    f->schema = child->schema;
    f->predicate = std::move(pred);
    f->children = {std::move(child)};
    return f;
}

// Put one conjunct as deep in a (join) tree as its column references allow.
PlanPtr Place(PlanPtr node, const ExprPtr& c) {
    AliasSet need = ExprAliases(*c);
    if (node->type == PlanType::kJoin) {
        AliasSet l = NodeAliases(*node->children[0]), r = NodeAliases(*node->children[1]);
        if (Subset(need, l)) {  // also catches constant conjuncts (empty set): evaluate them at the first input
            node->children[0] = Place(node->children[0], c);
        } else if (Subset(need, r)) {
            node->children[1] = Place(node->children[1], c);
        } else {
            node->predicate = node->predicate ? Bin(BinaryOp::kAnd, node->predicate, c) : c;
        }
        return node;
    }
    if (node->type == PlanType::kFilter) {
        node->predicate = Bin(BinaryOp::kAnd, node->predicate, c);
        return node;
    }
    return NewFilter(node, c);
}

double EstimateSize(const PlanNode& leaf) {
    if (leaf.type == PlanType::kScan) return std::max<double>(1.0, static_cast<double>(leaf.table->heap->NumPages()));
    if (leaf.type == PlanType::kFilter) {
        double s = EstimateSize(*leaf.children[0]);
        for (std::size_t i = 0; i < SplitConjuncts(leaf.predicate).size(); i++) s /= 2;  // crude selectivity guess
        return std::max(0.5, s);
    }
    return leaf.children.empty() ? 1.0 : EstimateSize(*leaf.children[0]);
}

void CollectJoinInputs(const PlanPtr& n, std::vector<PlanPtr>* leaves, std::vector<ExprPtr>* preds) {
    if (n->type == PlanType::kJoin) {
        for (auto& p : SplitConjuncts(n->predicate)) preds->push_back(p);
        CollectJoinInputs(n->children[0], leaves, preds);
        CollectJoinInputs(n->children[1], leaves, preds);
    } else {
        leaves->push_back(n);
    }
}

// Visitor gathering "alias.column" for every reference.
class RefCollector : public ExprWalker {
public:
    using ExprWalker::Visit;
    std::set<std::string>* out;
    explicit RefCollector(std::set<std::string>* o) : out(o) {}
    void Visit(const ColumnRefExpr& c) override { out->insert(c.qualifier + "." + c.name); }
};

void CollectRefs(const PlanNode& n, std::set<std::string>* refs) {
    RefCollector rc(refs);
    auto add = [&](const ExprPtr& e) { if (e) e->Accept(rc); };
    add(n.predicate);
    for (auto& e : n.exprs) add(e);
    for (auto& e : n.group_by) add(e);
    for (auto& e : n.aggregates) add(e);
    for (auto& o : n.order) add(o.expr);
    for (auto& [i, e] : n.assignments) add(e);
    for (auto& c : n.children) CollectRefs(*c, refs);
}

// col <op> const  (or const <op> col) on an INTEGER literal; normalised so the column is on the left.
struct ColCmp {
    const ColumnRefExpr* col{nullptr};
    BinaryOp op{BinaryOp::kEq};
    int64_t value{0};
};
bool MatchColCmp(const Expr& e, ColCmp* out) {
    if (e.kind != ExprKind::kBinary) return false;
    auto& b = static_cast<const BinaryExpr&>(e);
    if (b.op < BinaryOp::kEq || b.op > BinaryOp::kGe || b.op == BinaryOp::kNe) return false;
    auto lit = [](const Expr& x, int64_t* v) {
        if (x.kind != ExprKind::kLiteral) return false;
        const Value& val = static_cast<const LiteralExpr&>(x).value;
        if (val.index() != 1) return false;
        *v = std::get<int64_t>(val);
        return true;
    };
    int64_t v;
    if (b.left->kind == ExprKind::kColumn && lit(*b.right, &v)) {
        *out = {static_cast<const ColumnRefExpr*>(b.left.get()), b.op, v};
        return true;
    }
    if (b.right->kind == ExprKind::kColumn && lit(*b.left, &v)) {
        BinaryOp flipped = b.op;  // 5 < x  ==  x > 5
        if (b.op == BinaryOp::kLt) flipped = BinaryOp::kGt;
        else if (b.op == BinaryOp::kLe) flipped = BinaryOp::kGe;
        else if (b.op == BinaryOp::kGt) flipped = BinaryOp::kLt;
        else if (b.op == BinaryOp::kGe) flipped = BinaryOp::kLe;
        *out = {static_cast<const ColumnRefExpr*>(b.right.get()), flipped, v};
        return true;
    }
    return false;
}
void TightenLower(IndexBound* lo, int64_t v, bool incl) {
    if (!lo->present || v > lo->value || (v == lo->value && !incl)) *lo = {true, v, incl};
}
void TightenUpper(IndexBound* hi, int64_t v, bool incl) {
    if (!hi->present || v < hi->value || (v == hi->value && !incl)) *hi = {true, v, incl};
}
}  // namespace

PlanPtr Optimizer::Optimize(PlanPtr logical) {
    PlanPtr p = PushDownPredicates(std::move(logical));
    p = ReorderJoins(p);
    FixSchema(*p);
    PushDownProjections(p);
    FixSchema(*p);
    return ChooseAccessPaths(p);
}

// ---- rule 1 ----
PlanPtr Optimizer::PushDownPredicates(PlanPtr n) {
    for (auto& c : n->children) c = PushDownPredicates(c);
    if (n->type == PlanType::kFilter) {
        PlanType below = n->children[0]->type;
        if (below == PlanType::kJoin || below == PlanType::kScan || below == PlanType::kFilter) {
            PlanPtr result = n->children[0];
            for (auto& conj : SplitConjuncts(n->predicate)) result = Place(result, conj);
            return result;
        }
    }
    if (n->type == PlanType::kJoin && n->predicate) {  // ON conjuncts that only touch one input
        std::vector<ExprPtr> keep;
        AliasSet l = NodeAliases(*n->children[0]), r = NodeAliases(*n->children[1]);
        for (auto& conj : SplitConjuncts(n->predicate)) {
            AliasSet need = ExprAliases(*conj);
            if (Subset(need, l)) n->children[0] = Place(n->children[0], conj);
            else if (Subset(need, r)) n->children[1] = Place(n->children[1], conj);
            else keep.push_back(conj);
        }
        n->predicate = AndAll(keep);
    }
    return n;
}

// ---- rule 2 ----
PlanPtr Optimizer::ReorderJoins(PlanPtr n) {
    if (n->type != PlanType::kJoin) {
        for (auto& c : n->children) c = ReorderJoins(c);
        return n;
    }
    std::vector<PlanPtr> leaves;
    std::vector<ExprPtr> preds;
    CollectJoinInputs(n, &leaves, &preds);

    std::vector<std::size_t> order(leaves.size());
    for (std::size_t i = 0; i < order.size(); i++) order[i] = i;
    std::vector<double> size;
    for (auto& l : leaves) size.push_back(EstimateSize(*l));
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return size[a] < size[b]; });

    PlanPtr cur = leaves[order[0]];
    AliasSet have = NodeAliases(*cur);
    std::vector<std::size_t> remaining(order.begin() + 1, order.end());
    while (!remaining.empty()) {
        // Prefer the smallest remaining input that some predicate connects to what we already have.
        std::size_t pick = 0;
        for (std::size_t i = 0; i < remaining.size(); i++) {
            AliasSet la = NodeAliases(*leaves[remaining[i]]);
            AliasSet both = have;
            both.insert(la.begin(), la.end());
            bool connected = std::any_of(preds.begin(), preds.end(), [&](const ExprPtr& p) {
                AliasSet need = ExprAliases(*p);
                return Subset(need, both) && !Subset(need, have) && !Subset(need, la);
            });
            if (connected) { pick = i; break; }
        }
        PlanPtr right = leaves[remaining[pick]];
        remaining.erase(remaining.begin() + pick);
        AliasSet both = have, ra = NodeAliases(*right);
        both.insert(ra.begin(), ra.end());
        std::vector<ExprPtr> here, rest;
        for (auto& p : preds) (Subset(ExprAliases(*p), both) ? here : rest).push_back(p);
        preds = rest;
        auto join = std::make_shared<PlanNode>(PlanType::kJoin);
        join->predicate = AndAll(here);
        join->children = {cur, right};
        join->schema = Schema::Concat(cur->schema, right->schema);
        cur = join;
        have = both;
    }
    if (!preds.empty()) cur->predicate = cur->predicate ? Bin(BinaryOp::kAnd, cur->predicate, AndAll(preds)) : AndAll(preds);
    return cur;
}

// ---- rule 3 ----
void Optimizer::PushDownProjections(PlanPtr root) {
    if (root->type == PlanType::kInsert || root->type == PlanType::kUpdate || root->type == PlanType::kDelete) return;  // DML needs whole rows
    std::set<std::string> refs;
    CollectRefs(*root, &refs);
    std::function<void(PlanNode&)> prune = [&](PlanNode& n) {
        if (n.type == PlanType::kScan) {
            std::vector<int> keep;
            for (int i : n.scan_columns)
                if (refs.count(n.alias + "." + n.table->schema.At(i).name)) keep.push_back(i);
            if (keep.empty()) keep.push_back(n.scan_columns.front());  // e.g. COUNT(*): still need one column
            n.scan_columns = keep;
            Schema s;
            for (int i : keep) s.Add(Column{n.table->schema.At(i).name, n.table->schema.At(i).type, n.alias});
            n.schema = s;
        }
        for (auto& c : n.children) prune(*c);
    };
    prune(*root);
}

// ---- rule 4 ----
PlanPtr Optimizer::ChooseAccessPaths(PlanPtr n) {
    if (n->type == PlanType::kFilter && n->children[0]->type == PlanType::kScan) {
        PlanPtr scan = n->children[0];
        std::vector<ExprPtr> conj = SplitConjuncts(n->predicate);
        PlanPtr best;
        std::vector<bool> best_used;
        bool best_eq = false;
        for (auto& idx : scan->table->indexes) {
            if (!use_index_scans_) break;
            auto node = std::make_shared<PlanNode>(PlanType::kIndexScan);
            *node = *scan;
            node->type = PlanType::kIndexScan;
            node->index = idx.get();
            std::vector<bool> used(conj.size(), false);
            bool eq = false, any = false;
            for (std::size_t i = 0; i < conj.size(); i++) {
                ColCmp c;
                if (!MatchColCmp(*conj[i], &c) || c.col->qualifier != scan->alias) continue;
                if (scan->table->schema.Find("", c.col->name) != idx->column) continue;
                any = true;
                used[i] = true;
                switch (c.op) {
                    case BinaryOp::kEq: TightenLower(&node->lo, c.value, true); TightenUpper(&node->hi, c.value, true); eq = true; break;
                    case BinaryOp::kGt: TightenLower(&node->lo, c.value, false); break;
                    case BinaryOp::kGe: TightenLower(&node->lo, c.value, true); break;
                    case BinaryOp::kLt: TightenUpper(&node->hi, c.value, false); break;
                    default: TightenUpper(&node->hi, c.value, true); break;
                }
            }
            if (any && (!best || (eq && !best_eq))) { best = node; best_used = used; best_eq = eq; }
        }
        if (best) {
            std::vector<ExprPtr> rest;
            for (std::size_t i = 0; i < conj.size(); i++)
                if (!best_used[i]) rest.push_back(conj[i]);
            return rest.empty() ? best : NewFilter(best, AndAll(rest));
        }
        scan->type = PlanType::kSeqScan;
        return n;
    }
    for (auto& c : n->children) c = ChooseAccessPaths(c);
    if (n->type == PlanType::kScan) n->type = PlanType::kSeqScan;
    if (n->type == PlanType::kJoin) {
        AliasSet l = NodeAliases(*n->children[0]), r = NodeAliases(*n->children[1]);
        std::vector<ExprPtr> residual;
        for (auto& conj : SplitConjuncts(n->predicate)) {
            bool matched = false;
            if (conj->kind == ExprKind::kBinary && static_cast<const BinaryExpr&>(*conj).op == BinaryOp::kEq) {
                auto& b = static_cast<const BinaryExpr&>(*conj);
                AliasSet a = ExprAliases(*b.left), c = ExprAliases(*b.right);
                if (!a.empty() && !c.empty() && Subset(a, l) && Subset(c, r)) {
                    n->left_keys.push_back(b.left);
                    n->right_keys.push_back(b.right);
                    matched = true;
                } else if (!a.empty() && !c.empty() && Subset(a, r) && Subset(c, l)) {
                    n->left_keys.push_back(b.right);
                    n->right_keys.push_back(b.left);
                    matched = true;
                }
            }
            if (!matched) residual.push_back(conj);
        }
        if (!n->left_keys.empty()) {
            n->type = PlanType::kHashJoin;
            n->predicate = AndAll(residual);
        } else {
            n->type = PlanType::kNestedLoopJoin;
        }
    }
    return n;
}

// ---- EXPLAIN ----
namespace {
std::string BoundText(const PlanNode& n) {
    std::string col = n.table->schema.At(n.index->column).name;
    std::string s;
    if (n.lo.present && n.hi.present && n.lo.value == n.hi.value && n.lo.inclusive && n.hi.inclusive)
        return col + " = " + std::to_string(n.lo.value);
    if (n.lo.present) s += col + (n.lo.inclusive ? " >= " : " > ") + std::to_string(n.lo.value);
    if (n.hi.present) s += std::string(s.empty() ? "" : " AND ") + col + (n.hi.inclusive ? " <= " : " < ") + std::to_string(n.hi.value);
    return s.empty() ? "full" : s;
}
void Explain(const PlanNode& n, int depth, std::ostringstream& o) {
    o << std::string(depth * 2, ' ');
    auto cols = [&] {
        std::string s;
        for (auto& c : n.schema.Columns()) s += (s.empty() ? "" : ", ") + c.name;
        return s;
    };
    switch (n.type) {
        case PlanType::kScan:
        case PlanType::kSeqScan: o << "SeqScan " << n.table->name << " as " << n.alias << " [" << cols() << "]"; break;
        case PlanType::kIndexScan:
            o << "IndexScan " << n.table->name << " as " << n.alias << " using " << n.index->name << " (" << BoundText(n) << ") [" << cols() << "]";
            break;
        case PlanType::kFilter: o << "Filter " << ExprToString(*n.predicate); break;
        case PlanType::kProjection: o << "Projection [" << cols() << "]"; break;
        case PlanType::kJoin: o << "Join" << (n.predicate ? " on " + ExprToString(*n.predicate) : " (cross)"); break;
        case PlanType::kNestedLoopJoin: o << "NestedLoopJoin" << (n.predicate ? " on " + ExprToString(*n.predicate) : " (cross)"); break;
        case PlanType::kHashJoin: {
            o << "HashJoin on ";
            for (std::size_t i = 0; i < n.left_keys.size(); i++)
                o << (i ? " AND " : "") << ExprToString(*n.left_keys[i]) << " = " << ExprToString(*n.right_keys[i]);
            if (n.predicate) o << " residual " << ExprToString(*n.predicate);
            break;
        }
        case PlanType::kAggregation: {
            o << "Aggregation group by [";
            for (std::size_t i = 0; i < n.group_by.size(); i++) o << (i ? ", " : "") << ExprToString(*n.group_by[i]);
            o << "] compute [";
            for (std::size_t i = 0; i < n.aggregates.size(); i++) o << (i ? ", " : "") << ExprToString(*n.aggregates[i]);
            o << "]";
            break;
        }
        case PlanType::kSort: {
            o << "Sort by ";
            for (std::size_t i = 0; i < n.order.size(); i++) o << (i ? ", " : "") << ExprToString(*n.order[i].expr) << (n.order[i].desc ? " DESC" : "");
            break;
        }
        case PlanType::kLimit: o << "Limit " << n.limit; break;
        case PlanType::kInsert: o << "Insert into " << n.table->name << " (" << n.rows.size() << " rows)"; break;
        case PlanType::kUpdate: o << "Update " << n.table->name; break;
        case PlanType::kDelete: o << "Delete from " << n.table->name; break;
    }
    o << "\n";
    for (auto& c : n.children) Explain(*c, depth + 1, o);
}
}  // namespace

std::string PlanToString(const PlanNode& n) {
    std::ostringstream o;
    Explain(n, 0, o);
    return o.str();
}

}  // namespace arbordb
