#include <algorithm>
#include <unordered_map>

#include "query/executor/executor.h"

namespace arbordb {

namespace {
using ExecPtr = std::unique_ptr<AbstractExecutor>;

Tuple ProjectColumns(const std::vector<Value>& full, const std::vector<int>& cols, const RID& rid) {
    Tuple t;
    t.rid = rid;
    t.values.reserve(cols.size());
    for (int c : cols) t.values.push_back(full[c]);
    return t;
}

// ------------------------------------------------------------------ scans
class SeqScanExecutor : public AbstractExecutor {
public:
    SeqScanExecutor(const PlanNode& p, ExecContext* c) : AbstractExecutor(p.schema), plan_(p), ctx_(c) {}
    void Init() override { cursor_ = plan_.table->heap->Begin(); }
    std::optional<Tuple> Next() override {
        RID rid;
        std::string bytes;
        if (!plan_.table->heap->Next(ctx_->txn, &cursor_, &rid, &bytes, plan_.for_update)) return std::nullopt;
        return ProjectColumns(DeserializeTuple(plan_.table->schema, bytes), plan_.scan_columns, rid);
    }

private:
    const PlanNode& plan_;
    ExecContext* ctx_;
    TableHeap::Cursor cursor_{kInvalidPageId, 0};
};

class IndexScanExecutor : public AbstractExecutor {
public:
    IndexScanExecutor(const PlanNode& p, ExecContext* c) : AbstractExecutor(p.schema), plan_(p), ctx_(c) {}
    void Init() override {
        IntIndex* tree = plan_.index->tree.get();
        it_ = plan_.lo.present ? tree->LowerBound(plan_.lo.value) : tree->Begin();
        if (plan_.lo.present && !plan_.lo.inclusive)
            while (it_.Valid() && it_.key() == plan_.lo.value) it_.Next();
    }
    std::optional<Tuple> Next() override {
        while (it_.Valid()) {
            int64_t key = it_.key();
            if (plan_.hi.present && (key > plan_.hi.value || (key == plan_.hi.value && !plan_.hi.inclusive))) return std::nullopt;
            RID rid = it_.value();
            it_.Next();
            std::string bytes;
            if (!plan_.table->heap->Get(ctx_->txn, rid, &bytes, plan_.for_update)) continue;  // deleted meanwhile
            std::vector<Value> full = DeserializeTuple(plan_.table->schema, bytes);
            // The index is not locked, so re-verify against the row we actually locked and read.
            const Value& v = full[plan_.index->column];
            if (IsNull(v)) continue;
            int64_t k = std::get<int64_t>(v);
            if (plan_.lo.present && (k < plan_.lo.value || (k == plan_.lo.value && !plan_.lo.inclusive))) continue;
            if (plan_.hi.present && (k > plan_.hi.value || (k == plan_.hi.value && !plan_.hi.inclusive))) continue;
            return ProjectColumns(full, plan_.scan_columns, rid);
        }
        return std::nullopt;
    }

private:
    const PlanNode& plan_;
    ExecContext* ctx_;
    IntIndex::Iterator it_;
};

// ------------------------------------------------------------------ relational operators
class FilterExecutor : public AbstractExecutor {
public:
    FilterExecutor(const PlanNode& p, ExecPtr child) : AbstractExecutor(p.schema), plan_(p), child_(std::move(child)) {}
    void Init() override { child_->Init(); }
    std::optional<Tuple> Next() override {
        while (auto t = child_->Next())
            if (IsTrue(EvaluateExpr(*plan_.predicate, *t, child_->OutputSchema()))) return t;
        return std::nullopt;
    }

private:
    const PlanNode& plan_;
    ExecPtr child_;
};

class ProjectionExecutor : public AbstractExecutor {
public:
    ProjectionExecutor(const PlanNode& p, ExecPtr child) : AbstractExecutor(p.schema), plan_(p), child_(std::move(child)) {}
    void Init() override { child_->Init(); }
    std::optional<Tuple> Next() override {
        auto t = child_->Next();
        if (!t) return std::nullopt;
        Tuple out;
        out.rid = t->rid;
        for (auto& e : plan_.exprs) out.values.push_back(EvaluateExpr(*e, *t, child_->OutputSchema()));
        return out;
    }

private:
    const PlanNode& plan_;
    ExecPtr child_;
};

class LimitExecutor : public AbstractExecutor {
public:
    LimitExecutor(const PlanNode& p, ExecPtr child) : AbstractExecutor(p.schema), plan_(p), child_(std::move(child)) {}
    void Init() override { child_->Init(); emitted_ = 0; }
    std::optional<Tuple> Next() override {
        if (emitted_ >= plan_.limit) return std::nullopt;
        auto t = child_->Next();
        if (t) emitted_++;
        return t;
    }

private:
    const PlanNode& plan_;
    ExecPtr child_;
    int64_t emitted_{0};
};

class SortExecutor : public AbstractExecutor {
public:
    SortExecutor(const PlanNode& p, ExecPtr child) : AbstractExecutor(p.schema), plan_(p), child_(std::move(child)) {}
    void Init() override {
        child_->Init();
        struct Row { Tuple t; std::vector<Value> keys; };
        std::vector<Row> rows;
        while (auto t = child_->Next()) {
            Row r;
            for (auto& o : plan_.order) r.keys.push_back(EvaluateExpr(*o.expr, *t, child_->OutputSchema()));
            r.t = std::move(*t);
            rows.push_back(std::move(r));
        }
        std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
            for (std::size_t i = 0; i < plan_.order.size(); i++) {
                bool an = IsNull(a.keys[i]), bn = IsNull(b.keys[i]);
                int c = an || bn ? (an == bn ? 0 : (an ? -1 : 1)) : CompareValues(a.keys[i], b.keys[i]);  // NULLs sort first
                if (c != 0) return plan_.order[i].desc ? c > 0 : c < 0;
            }
            return false;
        });
        out_.clear();
        for (auto& r : rows) out_.push_back(std::move(r.t));
        pos_ = 0;
    }
    std::optional<Tuple> Next() override { return pos_ < out_.size() ? std::optional<Tuple>(out_[pos_++]) : std::nullopt; }

private:
    const PlanNode& plan_;
    ExecPtr child_;
    std::vector<Tuple> out_;
    std::size_t pos_{0};
};

Tuple Concat(const Tuple& l, const Tuple& r) {
    Tuple t;
    t.values = l.values;
    t.values.insert(t.values.end(), r.values.begin(), r.values.end());
    return t;
}

class NestedLoopJoinExecutor : public AbstractExecutor {
public:
    NestedLoopJoinExecutor(const PlanNode& p, ExecPtr l, ExecPtr r)
        : AbstractExecutor(p.schema), plan_(p), left_(std::move(l)), right_(std::move(r)) {}
    void Init() override {
        left_->Init();
        right_->Init();
        right_rows_.clear();
        while (auto t = right_->Next()) right_rows_.push_back(std::move(*t));
        cur_left_.reset();
        ri_ = 0;
    }
    std::optional<Tuple> Next() override {
        while (true) {
            if (!cur_left_) {
                cur_left_ = left_->Next();
                if (!cur_left_) return std::nullopt;
                ri_ = 0;
            }
            while (ri_ < right_rows_.size()) {
                Tuple joined = Concat(*cur_left_, right_rows_[ri_++]);
                if (!plan_.predicate || IsTrue(EvaluateExpr(*plan_.predicate, joined, schema_))) return joined;
            }
            cur_left_.reset();
        }
    }

private:
    const PlanNode& plan_;
    ExecPtr left_, right_;
    std::vector<Tuple> right_rows_;
    std::optional<Tuple> cur_left_;
    std::size_t ri_{0};
};

// Builds a hash table on the LEFT input (the optimizer puts the smaller input there) and probes it with
// the right input. Equal-key matches come out in build order, so output order is deterministic.
class HashJoinExecutor : public AbstractExecutor {
public:
    HashJoinExecutor(const PlanNode& p, ExecPtr l, ExecPtr r)
        : AbstractExecutor(p.schema), plan_(p), left_(std::move(l)), right_(std::move(r)) {}
    void Init() override {
        left_->Init();
        right_->Init();
        table_.clear();
        while (auto t = left_->Next()) {
            std::string key;
            if (!Key(plan_.left_keys, *t, left_->OutputSchema(), &key)) continue;  // NULL never equals anything
            table_[key].push_back(std::move(*t));
        }
        cur_right_.reset();
        matches_ = nullptr;
        mi_ = 0;
    }
    std::optional<Tuple> Next() override {
        while (true) {
            while (matches_ && mi_ < matches_->size()) {
                Tuple joined = Concat((*matches_)[mi_++], *cur_right_);
                if (!plan_.predicate || IsTrue(EvaluateExpr(*plan_.predicate, joined, schema_))) return joined;
            }
            cur_right_ = right_->Next();
            if (!cur_right_) return std::nullopt;
            matches_ = nullptr;
            std::string key;
            if (!Key(plan_.right_keys, *cur_right_, right_->OutputSchema(), &key)) continue;
            auto it = table_.find(key);
            if (it != table_.end()) { matches_ = &it->second; mi_ = 0; }
        }
    }

private:
    static bool Key(const std::vector<ExprPtr>& exprs, const Tuple& t, const Schema& s, std::string* out) {
        out->clear();
        for (auto& e : exprs) {
            Value v = EvaluateExpr(*e, t, s);
            if (IsNull(v)) return false;
            *out += ValueKey(v);
            out->push_back('\x1f');
        }
        return true;
    }
    const PlanNode& plan_;
    ExecPtr left_, right_;
    std::unordered_map<std::string, std::vector<Tuple>> table_;  // only looked up, never iterated
    std::optional<Tuple> cur_right_;
    const std::vector<Tuple>* matches_{nullptr};
    std::size_t mi_{0};
};

class AggregationExecutor : public AbstractExecutor {
public:
    AggregationExecutor(const PlanNode& p, ExecPtr child) : AbstractExecutor(p.schema), plan_(p), child_(std::move(child)) {}

    void Init() override {
        child_->Init();
        struct Acc {
            int64_t count = 0, isum = 0;
            double dsum = 0;
            bool any_double = false;
            Value min, max;
        };
        struct Group { std::vector<Value> keys; std::vector<Acc> accs; };
        std::vector<Group> groups;
        std::unordered_map<std::string, std::size_t> index;
        const Schema& in = child_->OutputSchema();

        while (auto t = child_->Next()) {
            Group g;
            std::string key;
            for (auto& e : plan_.group_by) {
                g.keys.push_back(EvaluateExpr(*e, *t, in));
                key += ValueKey(g.keys.back()) + '\x1f';
            }
            auto [it, fresh] = index.try_emplace(key, groups.size());
            if (fresh) {
                g.accs.resize(plan_.aggregates.size());
                groups.push_back(std::move(g));
            }
            Group& grp = groups[it->second];
            for (std::size_t i = 0; i < plan_.aggregates.size(); i++) {
                auto& ae = static_cast<const AggregateExpr&>(*plan_.aggregates[i]);
                Acc& a = grp.accs[i];
                if (!ae.arg) { a.count++; continue; }  // COUNT(*)
                Value v = EvaluateExpr(*ae.arg, *t, in);
                if (IsNull(v)) continue;  // aggregates ignore NULLs
                a.count++;
                if (ae.func == AggFunc::kSum || ae.func == AggFunc::kAvg) {
                    if (v.index() == 1) { a.isum += std::get<int64_t>(v); a.dsum += static_cast<double>(std::get<int64_t>(v)); }
                    else { a.any_double = true; a.dsum += std::get<double>(v); }
                }
                if (ae.func == AggFunc::kMin && (IsNull(a.min) || CompareValues(v, a.min) < 0)) a.min = v;
                if (ae.func == AggFunc::kMax && (IsNull(a.max) || CompareValues(v, a.max) > 0)) a.max = v;
            }
        }
        if (groups.empty() && plan_.group_by.empty()) {  // aggregate over an empty input still yields one row
            Group g;
            g.accs.resize(plan_.aggregates.size());
            groups.push_back(std::move(g));
        }
        out_.clear();
        for (auto& g : groups) {
            Tuple row;
            row.values = g.keys;
            for (std::size_t i = 0; i < plan_.aggregates.size(); i++) {
                auto& ae = static_cast<const AggregateExpr&>(*plan_.aggregates[i]);
                const Acc& a = g.accs[i];
                switch (ae.func) {
                    case AggFunc::kCount: row.values.push_back(Value(a.count)); break;
                    case AggFunc::kSum:
                        if (a.count == 0) row.values.push_back(Value());
                        else row.values.push_back(a.any_double ? Value(a.dsum) : Value(a.isum));
                        break;
                    case AggFunc::kAvg:
                        row.values.push_back(a.count == 0 ? Value() : Value(a.dsum / static_cast<double>(a.count)));
                        break;
                    case AggFunc::kMin: row.values.push_back(a.min); break;
                    case AggFunc::kMax: row.values.push_back(a.max); break;
                }
            }
            out_.push_back(std::move(row));
        }
        pos_ = 0;
    }
    std::optional<Tuple> Next() override { return pos_ < out_.size() ? std::optional<Tuple>(out_[pos_++]) : std::nullopt; }

private:
    const PlanNode& plan_;
    ExecPtr child_;
    std::vector<Tuple> out_;
    std::size_t pos_{0};
};

// ------------------------------------------------------------------ data modification
// Each DML executor does all its work on the first Next() and returns one row: the affected-row count.
// Index pages are not write-ahead logged, so every index change registers an undo hook on the
// transaction; Abort() replays the hooks backwards.
class DmlBase : public AbstractExecutor {
public:
    DmlBase(const PlanNode& p, ExecContext* c) : AbstractExecutor(p.schema), plan_(p), ctx_(c) {}

protected:
    static std::optional<int64_t> KeyOf(const IndexInfo& idx, const std::vector<Value>& row) {
        const Value& v = row[idx.column];
        if (IsNull(v)) return std::nullopt;
        return std::get<int64_t>(v);
    }
    void IndexInsert(IndexInfo* idx, int64_t key, const RID& rid) {
        idx->tree->Insert(key, rid);
        IntIndex* tree = idx->tree.get();
        ctx_->txn->AddUndoHook([tree, key] { tree->Remove(key); });
    }
    void IndexRemove(IndexInfo* idx, int64_t key, const RID& rid) {
        idx->tree->Remove(key);
        IntIndex* tree = idx->tree.get();
        ctx_->txn->AddUndoHook([tree, key, rid] { tree->Insert(key, rid); });
    }
    static void CheckUnique(IndexInfo* idx, int64_t key) {
        RID existing;
        if (idx->tree->GetValue(key, &existing))
            throw ExecutionError("duplicate key value " + std::to_string(key) + " violates unique index " + idx->name);
    }
    std::optional<Tuple> Result(int64_t n) {
        if (done_) return std::nullopt;
        done_ = true;
        Tuple t;
        t.values = {Value(n)};
        return t;
    }
    const PlanNode& plan_;
    ExecContext* ctx_;
    bool done_{false};
};

class InsertExecutor : public DmlBase {
public:
    using DmlBase::DmlBase;
    void Init() override { done_ = false; }
    std::optional<Tuple> Next() override {
        if (done_) return std::nullopt;
        TableInfo* t = plan_.table;
        for (auto& row : plan_.rows) {
            for (auto& idx : t->indexes)
                if (auto k = KeyOf(*idx, row)) CheckUnique(idx.get(), *k);
            std::string bytes = SerializeTuple(t->schema, row);
            RID rid;
            try {
                rid = t->heap->Insert(ctx_->txn, bytes);
            } catch (const std::length_error&) {
                throw ExecutionError("row too large for a page");
            }
            for (auto& idx : t->indexes)
                if (auto k = KeyOf(*idx, row)) IndexInsert(idx.get(), *k, rid);
        }
        return Result(static_cast<int64_t>(plan_.rows.size()));
    }
};

class UpdateExecutor : public DmlBase {
public:
    UpdateExecutor(const PlanNode& p, ExecContext* c, ExecPtr child) : DmlBase(p, c), child_(std::move(child)) {}
    void Init() override { child_->Init(); done_ = false; }
    std::optional<Tuple> Next() override {
        if (done_) return std::nullopt;
        // Materialise first: updating while scanning could revisit relocated rows (the "Halloween problem").
        std::vector<Tuple> victims;
        while (auto t = child_->Next()) victims.push_back(std::move(*t));
        TableInfo* t = plan_.table;
        for (auto& old : victims) {
            std::vector<Value> row = old.values;
            for (auto& [col, expr] : plan_.assignments)
                row[col] = CoerceToType(EvaluateExpr(*expr, old, child_->OutputSchema()), t->schema.At(col).type);
            for (auto& idx : t->indexes) {
                auto nk = KeyOf(*idx, row), ok = KeyOf(*idx, old.values);
                if (nk && nk != ok) CheckUnique(idx.get(), *nk);
            }
            std::string bytes = SerializeTuple(t->schema, row);
            std::optional<RID> new_rid;
            try {
                new_rid = t->heap->Update(ctx_->txn, old.rid, bytes);
            } catch (const std::length_error&) {
                throw ExecutionError("row too large for a page");
            }
            if (!new_rid) throw ExecutionError("row vanished during UPDATE");
            for (auto& idx : t->indexes) {
                auto nk = KeyOf(*idx, row), ok = KeyOf(*idx, old.values);
                if (nk == ok && *new_rid == old.rid) continue;  // nothing in this index changed
                if (ok) IndexRemove(idx.get(), *ok, old.rid);
                if (nk) IndexInsert(idx.get(), *nk, *new_rid);
            }
        }
        return Result(static_cast<int64_t>(victims.size()));
    }

private:
    ExecPtr child_;
};

class DeleteExecutor : public DmlBase {
public:
    DeleteExecutor(const PlanNode& p, ExecContext* c, ExecPtr child) : DmlBase(p, c), child_(std::move(child)) {}
    void Init() override { child_->Init(); done_ = false; }
    std::optional<Tuple> Next() override {
        if (done_) return std::nullopt;
        std::vector<Tuple> victims;
        while (auto t = child_->Next()) victims.push_back(std::move(*t));
        TableInfo* t = plan_.table;
        for (auto& v : victims) {
            t->heap->Delete(ctx_->txn, v.rid);
            for (auto& idx : t->indexes)
                if (auto k = KeyOf(*idx, v.values)) IndexRemove(idx.get(), *k, v.rid);
        }
        return Result(static_cast<int64_t>(victims.size()));
    }

private:
    ExecPtr child_;
};
}  // namespace

std::unique_ptr<AbstractExecutor> ExecutorFactory::Create(const PlanNode& p, ExecContext* ctx) {
    auto child = [&](std::size_t i) { return Create(*p.children[i], ctx); };
    switch (p.type) {
        case PlanType::kSeqScan: return std::make_unique<SeqScanExecutor>(p, ctx);
        case PlanType::kIndexScan: return std::make_unique<IndexScanExecutor>(p, ctx);
        case PlanType::kFilter: return std::make_unique<FilterExecutor>(p, child(0));
        case PlanType::kProjection: return std::make_unique<ProjectionExecutor>(p, child(0));
        case PlanType::kNestedLoopJoin: return std::make_unique<NestedLoopJoinExecutor>(p, child(0), child(1));
        case PlanType::kHashJoin: return std::make_unique<HashJoinExecutor>(p, child(0), child(1));
        case PlanType::kAggregation: return std::make_unique<AggregationExecutor>(p, child(0));
        case PlanType::kSort: return std::make_unique<SortExecutor>(p, child(0));
        case PlanType::kLimit: return std::make_unique<LimitExecutor>(p, child(0));
        case PlanType::kInsert: return std::make_unique<InsertExecutor>(p, ctx);
        case PlanType::kUpdate: return std::make_unique<UpdateExecutor>(p, ctx, child(0));
        case PlanType::kDelete: return std::make_unique<DeleteExecutor>(p, ctx, child(0));
        case PlanType::kScan:
        case PlanType::kJoin: break;
    }
    throw ExecutionError("plan was not optimized: logical operator reached the executor");
}

}  // namespace arbordb
