#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common/config.h"
#include "storage/page/page.h"

namespace arbordb {

enum class LogType : uint8_t { kBegin = 1, kCommit, kAbort, kEnd, kUpdate, kClr, kCheckpoint };

// A physiological page operation: addressed to one page, expressed in slotted-page terms, applied
// with LSN gating on redo. "Before/after images" are the tuple bytes (empty where not applicable).
enum class PageOpKind : uint8_t {
    kInsert = 1,  // append `after` as slot `slot`                  (undo: kDelete)
    kDelete,      // tombstone slot                                 (undo: kUndelete)
    kUndelete,    // clear tombstone (only ever appears in a CLR)
    kUpdate,      // slot's tuple: before -> after                  (undo: kUpdate with images swapped)
    kInitPage,    // format page as an empty slotted page           (redo-only, never undone)
    kSetNext,     // set page's next-page pointer to `next_page`    (redo-only, never undone)
};

struct PageOp {
    PageOpKind kind{PageOpKind::kInsert};
    page_id_t page_id{kInvalidPageId};
    uint16_t slot{0};
    page_id_t next_page{kInvalidPageId};
    std::string before, after;

    bool Undoable() const {
        return kind == PageOpKind::kInsert || kind == PageOpKind::kDelete || kind == PageOpKind::kUpdate;
    }
    // The operation that cancels this one (what a CLR carries).
    PageOp Inverse() const;
};

// Apply op to a page (already pinned and write-latched by the caller). Throws on an op that cannot
// apply, which would mean the page state diverged from the log.
void ApplyPageOp(Page* page, const PageOp& op);

struct LogRecord {
    lsn_t lsn{kInvalidLsn};
    lsn_t prev_lsn{kInvalidLsn};  // previous record of the same transaction
    txn_id_t txn_id{kInvalidTxnId};
    LogType type{LogType::kBegin};

    PageOp op;                       // kUpdate, kClr
    lsn_t undo_next_lsn{kInvalidLsn};  // kClr: next record of this txn still to be undone

    lsn_t redo_start_lsn{kInvalidLsn};                    // kCheckpoint
    std::vector<std::pair<txn_id_t, lsn_t>> active_txns;  // kCheckpoint: (txn, its last lsn)
    // The dirty page table is empty by construction: checkpoints flush all dirty pages first (sharp
    // checkpoint), so redo_start_lsn bounds what analysis/redo must scan.

    bool ModifiesPage() const { return type == LogType::kUpdate || type == LogType::kClr; }
};

void SerializeLogRecord(const LogRecord& r, std::string* out);
// Returns false if buf does not hold a complete, well-formed record.
bool DeserializeLogRecord(const char* buf, std::size_t len, LogRecord* out);

}  // namespace arbordb
