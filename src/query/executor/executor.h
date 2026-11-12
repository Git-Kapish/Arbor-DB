#pragma once

#include <memory>
#include <optional>

#include "concurrency/transaction.h"
#include "query/plan.h"

namespace arbordb {

struct ExecContext {
    Transaction* txn;  // every data access goes through this transaction (locks + logging)
};

// Volcano / iterator model: an operator is pulled one tuple at a time and pulls from its children, so
// operators compose without knowing each other's concrete types. Init() may be called once before Next().
class AbstractExecutor {
public:
    virtual ~AbstractExecutor() = default;
    virtual void Init() = 0;
    // Next output tuple, or nullopt when exhausted.
    virtual std::optional<Tuple> Next() = 0;
    const Schema& OutputSchema() const { return schema_; }

protected:
    explicit AbstractExecutor(Schema s) : schema_(std::move(s)) {}
    Schema schema_;
};

// Factory pattern: the single place that maps a physical plan node to its executor class.
class ExecutorFactory {
public:
    static std::unique_ptr<AbstractExecutor> Create(const PlanNode& plan, ExecContext* ctx);
};

}  // namespace arbordb
