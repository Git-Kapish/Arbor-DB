#pragma once

#include "concurrency/transaction.h"

namespace arbordb {

enum class LockMode { kShared, kExclusive };

// Strategy pattern: the data layer talks to concurrency control only through this interface.
// TwoPLManager (strict 2PL, implemented) and MVCCManager (stretch, not implemented) are the strategies.
class ConcurrencyControlManager {
public:
    virtual ~ConcurrencyControlManager() = default;
    // Blocks until granted. Returns false if the transaction must abort (deadlock victim).
    virtual bool AcquireLock(Transaction* txn, const RID& rid, LockMode mode) = 0;
    virtual void ReleaseLock(Transaction* txn, const RID& rid) = 0;
    // Called at commit/abort: strict 2PL releases everything here and nowhere else.
    virtual void ReleaseAllLocks(Transaction* txn) = 0;
};

}  // namespace arbordb
