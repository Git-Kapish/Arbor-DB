#pragma once

#include "query/plan.h"

namespace arbordb {

// Resolves names against the Catalog, checks them, and produces a typed LOGICAL plan:
//   Limit <- Projection <- Sort <- [Filter(having)] <- [Aggregation] <- Filter(where) <- Join(...Scan)
// Every column reference in the result is qualified with its table alias. Throws BindError.
class Binder {
public:
    explicit Binder(Catalog* catalog) : catalog_(catalog) {}

    PlanPtr BindSelect(const SelectStmt& s);
    PlanPtr BindInsert(const InsertStmt& s);
    PlanPtr BindUpdate(const UpdateStmt& s);
    PlanPtr BindDelete(const DeleteStmt& s);

private:
    PlanPtr MakeScan(TableInfo* t, const std::string& alias);
    TableInfo* MustGetTable(const std::string& name);
    // Qualify every column reference against `scope` (errors on unknown / ambiguous names).
    ExprPtr Qualify(const ExprPtr& e, const Schema& scope);

    Catalog* catalog_;
};

}  // namespace arbordb
