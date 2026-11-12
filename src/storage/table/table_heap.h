#pragma once

#include <mutex>
#include <optional>
#include <string>

#include "buffer/buffer_pool_manager.h"
#include "concurrency/concurrency_control_manager.h"
#include "recovery/log_manager.h"

namespace arbordb {

// A table's tuples: a singly linked chain of slotted pages. Every mutation is write-ahead logged
// (when a LogManager is supplied) and, when a ConcurrencyControlManager is supplied, protected by
// row locks taken here -- so callers cannot forget them.
//
// Locking protocol (strict 2PL, locks held until the transaction ends):
//   read   -> S lock on the RID (X if for_update)
//   insert -> X lock on the new RID, taken while the page latch is held. Slots are never reused, so no
//             other transaction can know the RID yet and the grant is immediate; this closes the window
//             in which a scanner could read an uncommitted insert.
//   update/delete -> X lock first, then the change.
// Page latches are never held while waiting for a row lock.
//
// Passing txn == nullptr (only legal without a log) skips logging and locking: used by unit tests.
class TableHeap {
public:
    struct Cursor {  // scan position; start with Begin()
        page_id_t page_id;
        uint16_t slot{0};
    };

    TableHeap(BufferPoolManager* bpm, LogManager* log, ConcurrencyControlManager* cc, page_id_t first_page_id);

    // Allocates and durably formats the first page of a new table; returns its id.
    static page_id_t CreateFirstPage(BufferPoolManager* bpm);

    page_id_t FirstPageId() const { return first_page_id_; }
    static constexpr std::size_t kMaxTupleSize = kPageSize - 24 /*header*/ - 8 /*slot*/;

    // Throws std::length_error if the tuple can never fit in a page.
    RID Insert(Transaction* txn, const std::string& tuple);
    // false if the tuple does not exist or is already deleted.
    bool Delete(Transaction* txn, const RID& rid);
    // Updates in place when possible; otherwise deletes and re-inserts, so the RID may change.
    // nullopt if the tuple does not exist. Throws std::length_error if the new tuple is too big.
    std::optional<RID> Update(Transaction* txn, const RID& rid, const std::string& tuple);
    // false if the tuple does not exist or is deleted.
    bool Get(Transaction* txn, const RID& rid, std::string* out, bool for_update = false);

    Cursor Begin() const { return Cursor{first_page_id_, 0}; }
    // Advances to the next live tuple (taking an S lock on every slot it examines, deleted or not,
    // so it never reads another transaction's uncommitted work). Returns false at the end.
    bool Next(Transaction* txn, Cursor* cur, RID* rid, std::string* out);

    // Number of pages in the chain (used by the optimizer as a size estimate).
    std::size_t NumPages();

private:
    lsn_t LogAndApply(Transaction* txn, Page* page, PageOp op);
    void Lock(Transaction* txn, const RID& rid, LockMode mode);

    BufferPoolManager* bpm_;
    LogManager* log_;
    ConcurrencyControlManager* cc_;
    page_id_t first_page_id_;
    page_id_t tail_hint_;
    std::mutex tail_mu_;
};

}  // namespace arbordb
