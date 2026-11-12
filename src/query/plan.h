#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "catalog/catalog.h"
#include "query/parser/ast.h"

namespace arbordb {

// One tree type serves as both logical plan (kScan, kJoin) and physical plan (kSeqScan, kIndexScan,
// kNestedLoopJoin, kHashJoin): the binder emits logical nodes, the optimizer rewrites them into
// physical ones. Expressions refer to columns by (alias, name), never by position, so rewrites such
// as join reordering and column pruning never have to renumber anything.
enum class PlanType {
    kScan, kSeqScan, kIndexScan, kFilter, kProjection, kJoin, kNestedLoopJoin, kHashJoin, kAggregation, kSort, kLimit,
    kInsert, kUpdate, kDelete
};

struct IndexBound {
    bool present{false};
    int64_t value{0};
    bool inclusive{true};
};

struct PlanNode;
using PlanPtr = std::shared_ptr<PlanNode>;

struct PlanNode {
    explicit PlanNode(PlanType t) : type(t) {}

    PlanType type;
    Schema schema;  // output columns
    std::vector<PlanPtr> children;

    // scans
    TableInfo* table{nullptr};
    std::string alias;
    std::vector<int> scan_columns;  // table-schema indexes this scan outputs (projection pushdown), in order
    bool for_update{false};         // X locks (scan feeding UPDATE/DELETE)
    IndexInfo* index{nullptr};      // kIndexScan
    IndexBound lo, hi;              // kIndexScan: equality is lo == hi, both inclusive

    ExprPtr predicate;                                // kFilter; kJoin/kNestedLoopJoin condition (null = cross); hash-join residual
    std::vector<ExprPtr> left_keys, right_keys;       // kHashJoin: equi-join key expressions per side
    std::vector<ExprPtr> exprs;                       // kProjection
    std::vector<ExprPtr> group_by, aggregates;        // kAggregation
    std::vector<OrderItem> order;                     // kSort
    int64_t limit{0};                                 // kLimit
    std::vector<std::vector<Value>> rows;             // kInsert (already coerced to the table schema)
    std::vector<std::pair<int, ExprPtr>> assignments; // kUpdate: (column index, new value expression)
};

std::string PlanToString(const PlanNode& n);  // EXPLAIN output, one operator per line, children indented

}  // namespace arbordb
