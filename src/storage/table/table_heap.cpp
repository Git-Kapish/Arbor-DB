#include "storage/table/table_heap.h"

#include <stdexcept>

#include "storage/page/slotted_page.h"

namespace arbordb {

namespace {
Page* MustFetch(BufferPoolManager* bpm, page_id_t id) {
    Page* p = bpm->FetchPage(id);
    if (!p) throw std::runtime_error("buffer pool exhausted");
    return p;
}
}  // namespace

page_id_t TableHeap::CreateFirstPage(BufferPoolManager* bpm) {
    page_id_t id;
    Page* p = bpm->NewPage(&id);
    if (!p) throw std::runtime_error("buffer pool exhausted");
    SlottedPage(p).Init(id);
    bpm->UnpinPage(id, true);
    bpm->FlushPage(id);  // DDL is not WAL-logged, so the page must be on disk before the catalog names it
    bpm->Disk()->Sync();
    return id;
}

TableHeap::TableHeap(BufferPoolManager* bpm, LogManager* log, ConcurrencyControlManager* cc, page_id_t first)
    : bpm_(bpm), log_(log), cc_(cc), first_page_id_(first), tail_hint_(first) {}

void TableHeap::Lock(Transaction* txn, const RID& rid, LockMode mode) {
    if (!cc_ || !txn) return;
    if (!cc_->AcquireLock(txn, rid, mode)) throw TransactionAbortedException(txn->Id(), txn->abort_reason);
}

// Log the operation, apply it to the page, stamp the page LSN. Caller holds the page's write latch,
// so log order == page application order.
lsn_t TableHeap::LogAndApply(Transaction* txn, Page* page, PageOp op) {
    lsn_t lsn = kInvalidLsn;
    if (log_ && txn) {
        LogRecord r;
        r.type = LogType::kUpdate;
        r.txn_id = txn->Id();
        r.prev_lsn = txn->last_lsn;
        r.op = op;
        lsn = log_->Append(&r);
        txn->last_lsn = lsn;
    }
    ApplyPageOp(page, op);
    if (lsn != kInvalidLsn) page->SetLSN(lsn);
    return lsn;
}

RID TableHeap::Insert(Transaction* txn, const std::string& tuple) {
    if (tuple.empty() || tuple.size() > kMaxTupleSize) throw std::length_error("tuple size out of range");
    page_id_t pid;
    {
        std::lock_guard<std::mutex> g(tail_mu_);
        pid = tail_hint_;
    }
    while (true) {
        Page* page = MustFetch(bpm_, pid);
        page->WLatch();
        SlottedPage sp(page);
        auto len = static_cast<uint16_t>(tuple.size());
        if (sp.CanInsert(len)) {
            RID rid{pid, sp.SlotCount()};
            try {
                Lock(txn, rid, LockMode::kExclusive);  // immediate: nobody else can know this RID yet
            } catch (...) {  // txn was already marked as a deadlock victim
                page->WUnlatch();
                bpm_->UnpinPage(pid, false);
                throw;
            }
            PageOp op;
            op.kind = PageOpKind::kInsert;
            op.page_id = pid;
            op.slot = rid.slot;
            op.after = tuple;
            if (cc_ && txn) cc_->OnWrite(txn, rid, WriteKind::kInsert, false, std::string());
            LogAndApply(txn, page, op);
            page->WUnlatch();
            bpm_->UnpinPage(pid, true);
            std::lock_guard<std::mutex> g(tail_mu_);
            tail_hint_ = pid;
            return rid;
        }
        page_id_t next = sp.NextPageId();
        if (next == kInvalidPageId) {
            // Extend the chain while holding the tail's latch, so concurrent inserters see the new link.
            page_id_t nid;
            Page* np = bpm_->NewPage(&nid);
            if (!np) {
                page->WUnlatch();
                bpm_->UnpinPage(pid, false);
                throw std::runtime_error("buffer pool exhausted");
            }
            np->WLatch();
            PageOp init;
            init.kind = PageOpKind::kInitPage;
            init.page_id = nid;
            LogAndApply(txn, np, init);
            np->WUnlatch();
            bpm_->UnpinPage(nid, true);
            PageOp link;
            link.kind = PageOpKind::kSetNext;
            link.page_id = pid;
            link.next_page = nid;
            LogAndApply(txn, page, link);
            next = nid;
            page->WUnlatch();
            bpm_->UnpinPage(pid, true);
        } else {
            page->WUnlatch();
            bpm_->UnpinPage(pid, false);
        }
        pid = next;
    }
}

bool TableHeap::Delete(Transaction* txn, const RID& rid) {
    Lock(txn, rid, LockMode::kExclusive);
    Page* page = MustFetch(bpm_, rid.page_id);
    page->WLatch();
    SlottedPage sp(page);
    auto old = rid.slot < sp.SlotCount() ? sp.Get(static_cast<uint16_t>(rid.slot)) : std::nullopt;
    bool exists = old.has_value();
    if (exists) {
        if (cc_ && txn) cc_->OnWrite(txn, rid, WriteKind::kDelete, true, std::string(old->first, old->second));
        PageOp op;
        op.kind = PageOpKind::kDelete;
        op.page_id = rid.page_id;
        op.slot = static_cast<uint16_t>(rid.slot);
        LogAndApply(txn, page, op);
    }
    page->WUnlatch();
    bpm_->UnpinPage(rid.page_id, exists);
    return exists;
}

std::optional<RID> TableHeap::Update(Transaction* txn, const RID& rid, const std::string& tuple) {
    if (tuple.empty() || tuple.size() > kMaxTupleSize) throw std::length_error("tuple size out of range");
    Lock(txn, rid, LockMode::kExclusive);
    Page* page = MustFetch(bpm_, rid.page_id);
    page->WLatch();
    SlottedPage sp(page);
    auto slot = static_cast<uint16_t>(rid.slot);
    auto old = rid.slot < sp.SlotCount() ? sp.Get(slot) : std::nullopt;
    if (!old) {
        page->WUnlatch();
        bpm_->UnpinPage(rid.page_id, false);
        return std::nullopt;
    }
    if (sp.CanUpdate(slot, static_cast<uint16_t>(tuple.size()))) {
        PageOp op;
        op.kind = PageOpKind::kUpdate;
        op.page_id = rid.page_id;
        op.slot = slot;
        op.before.assign(old->first, old->second);
        op.after = tuple;
        if (cc_ && txn) cc_->OnWrite(txn, rid, WriteKind::kUpdate, true, op.before);
        LogAndApply(txn, page, op);
        page->WUnlatch();
        bpm_->UnpinPage(rid.page_id, true);
        return rid;
    }
    page->WUnlatch();
    bpm_->UnpinPage(rid.page_id, false);
    // No room to grow in place: move the tuple (new RID).
    Delete(txn, rid);
    return Insert(txn, tuple);
}

bool TableHeap::Get(Transaction* txn, const RID& rid, std::string* out, bool for_update) {
    if (!cc_ || cc_->TakesReadLocks()) Lock(txn, rid, for_update ? LockMode::kExclusive : LockMode::kShared);
    Page* page = MustFetch(bpm_, rid.page_id);
    page->RLatch();
    SlottedPage sp(page);
    auto t = rid.slot < sp.SlotCount() ? sp.Get(static_cast<uint16_t>(rid.slot)) : std::nullopt;
    bool visible;
    if (cc_ && txn) {  // the concurrency strategy decides which version of the tuple this transaction sees
        visible = cc_->ResolveRead(txn, rid, t.has_value(), t ? std::string(t->first, t->second) : std::string(), out);
    } else {
        visible = t.has_value();
        if (t) out->assign(t->first, t->second);
    }
    page->RUnlatch();
    bpm_->UnpinPage(rid.page_id, false);
    return visible;
}

bool TableHeap::Next(Transaction* txn, Cursor* cur, RID* rid, std::string* out, bool for_update) {
    while (cur->page_id != kInvalidPageId) {
        Page* page = MustFetch(bpm_, cur->page_id);
        page->RLatch();
        SlottedPage sp(page);
        uint16_t count = sp.SlotCount();
        page_id_t next = sp.NextPageId();
        page->RUnlatch();
        bpm_->UnpinPage(cur->page_id, false);
        if (cur->slot >= count) {
            cur->page_id = next;
            cur->slot = 0;
            continue;
        }
        RID r{cur->page_id, cur->slot++};
        if (Get(txn, r, out, for_update)) {  // takes the lock, then re-checks under it
            *rid = r;
            return true;
        }
    }
    return false;
}

std::size_t TableHeap::NumPages() {
    std::size_t n = 0;
    for (page_id_t pid = first_page_id_; pid != kInvalidPageId; n++) {
        Page* page = MustFetch(bpm_, pid);
        page->RLatch();
        pid = SlottedPage(page).NextPageId();
        page->RUnlatch();
        bpm_->UnpinPage(page->GetPageId(), false);
    }
    return n;
}

}  // namespace arbordb
