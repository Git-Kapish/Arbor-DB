#pragma once

#include "query/plan.h"

namespace arbordb {

// RULE-BASED optimizer. It applies fixed rewrite rules in a fixed order; it does NOT estimate
// cardinalities from statistics or compare plan costs (that would be a cost-based optimizer, which is
// explicitly out of scope -- see README Known Gaps). The only size signal is the number of heap pages of
// each base table (an exact, free number), halved per filter conjunct as a crude rule of thumb.
//
//   1. PushDownPredicates   WHERE conjuncts move as close to the scans as possible; conjuncts that span two
//                           inputs become the condition of the join that first has both.
//   2. ReorderJoins         inner-join chains are rebuilt left-deep with the smaller input first (and
//                           preferring inputs connected by a predicate over cross products).
//   3. PushDownProjections  each scan outputs only the columns something above it references.
//   4. ChooseAccessPaths    Filter+Scan on an indexed INTEGER column becomes an IndexScan (equality or range);
//                           joins with an equi-condition become HashJoin (build side = left, the smaller
//                           input), the rest NestedLoopJoin; everything else SeqScan.
class Optimizer {
public:
    // use_index_scans=false forces SeqScan everywhere. MVCC mode needs that: B+Tree entries are not versioned,
    // so an index lookup could miss a row that is still visible in an older snapshot.
    explicit Optimizer(bool use_index_scans = true) : use_index_scans_(use_index_scans) {}
    PlanPtr Optimize(PlanPtr logical);

private:
    PlanPtr PushDownPredicates(PlanPtr n);
    PlanPtr ReorderJoins(PlanPtr n);
    void PushDownProjections(PlanPtr root);
    PlanPtr ChooseAccessPaths(PlanPtr n);

    bool use_index_scans_;
};

}  // namespace arbordb
