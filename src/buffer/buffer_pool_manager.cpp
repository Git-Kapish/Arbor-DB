#include "buffer/buffer_pool_manager.h"

#include <cstring>

#include "buffer/lru_k_replacer.h"

namespace arbordb {

BufferPoolManager::BufferPoolManager(std::size_t pool_size, DiskManager* disk, LogFlusher* log,
                                     std::unique_ptr<Replacer> replacer)
    : pool_size_(pool_size),
      disk_(disk),
      log_(log),
      replacer_(replacer ? std::move(replacer) : std::make_unique<LRUKReplacer>()),
      frames_(pool_size) {
    for (std::size_t i = 0; i < pool_size; i++) free_list_.push_back(static_cast<frame_id_t>(i));
}

void BufferPoolManager::WriteOut(Page& p) {
    if (log_) {
        lsn_t lsn = p.GetLSN();
        if (lsn > log_->FlushedLsn()) log_->FlushTo(lsn);  // write-ahead rule
    }
    disk_->WritePage(p.page_id_, p.data_);
}

frame_id_t BufferPoolManager::AcquireFrame() {
    if (!free_list_.empty()) {
        frame_id_t f = free_list_.front();
        free_list_.pop_front();
        return f;
    }
    frame_id_t f;
    if (!replacer_->Victim(&f)) return -1;
    Page& p = frames_[f];
    if (p.is_dirty_) WriteOut(p);  // pin_count == 0, so nobody can be modifying it
    page_table_.erase(p.page_id_);
    p.page_id_ = kInvalidPageId;
    p.is_dirty_ = false;
    return f;
}

Page* BufferPoolManager::FetchPage(page_id_t id) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = page_table_.find(id);
    if (it != page_table_.end()) {
        Page& p = frames_[it->second];
        p.pin_count_++;
        replacer_->RecordAccess(it->second);
        replacer_->Pin(it->second);
        return &p;
    }
    frame_id_t f = AcquireFrame();
    if (f < 0) return nullptr;
    Page& p = frames_[f];
    disk_->ReadPage(id, p.data_);
    p.page_id_ = id;
    p.pin_count_ = 1;
    p.is_dirty_ = false;
    page_table_[id] = f;
    replacer_->RecordAccess(f);
    replacer_->Pin(f);
    return &p;
}

Page* BufferPoolManager::NewPage(page_id_t* id) {
    std::lock_guard<std::mutex> g(mu_);
    frame_id_t f = AcquireFrame();
    if (f < 0) return nullptr;
    page_id_t pid = disk_->AllocatePage();
    Page& p = frames_[f];
    std::memset(p.data_, 0, kPageSize);
    std::memcpy(p.data_ + kOffsetPageId, &pid, sizeof(pid));
    p.page_id_ = pid;
    p.pin_count_ = 1;
    p.is_dirty_ = true;
    page_table_[pid] = f;
    replacer_->RecordAccess(f);
    replacer_->Pin(f);
    *id = pid;
    return &p;
}

bool BufferPoolManager::UnpinPage(page_id_t id, bool is_dirty) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = page_table_.find(id);
    if (it == page_table_.end()) return false;
    Page& p = frames_[it->second];
    if (p.pin_count_ <= 0) return false;
    if (is_dirty) p.is_dirty_ = true;
    if (--p.pin_count_ == 0) replacer_->Unpin(it->second);
    return true;
}

bool BufferPoolManager::FlushPage(page_id_t id) {
    Page* p;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = page_table_.find(id);
        if (it == page_table_.end()) return false;
        p = &frames_[it->second];
        p->pin_count_++;  // keep it resident while we write without holding mu_
        replacer_->Pin(it->second);
        p->is_dirty_ = false;  // cleared first: a concurrent modifier re-sets it on unpin
    }
    // Latch outside mu_: a thread holding the page latch may call into the pool (NewPage).
    p->RLatch();
    std::vector<char> copy(p->data_, p->data_ + kPageSize);
    p->RUnlatch();
    lsn_t lsn;
    std::memcpy(&lsn, copy.data() + kOffsetLsn, sizeof(lsn));
    if (log_ && lsn > log_->FlushedLsn()) log_->FlushTo(lsn);
    disk_->WritePage(id, copy.data());
    UnpinPage(id, false);
    return true;
}

void BufferPoolManager::FlushAll() {
    std::vector<page_id_t> ids;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& p : frames_)
            if (p.page_id_ != kInvalidPageId && p.is_dirty_) ids.push_back(p.page_id_);
    }
    for (page_id_t id : ids) FlushPage(id);
}

bool BufferPoolManager::DeletePage(page_id_t id) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = page_table_.find(id);
    if (it != page_table_.end()) {
        frame_id_t f = it->second;
        Page& p = frames_[f];
        if (p.pin_count_ > 0) return false;
        replacer_->Remove(f);
        page_table_.erase(it);
        p.page_id_ = kInvalidPageId;
        p.is_dirty_ = false;
        free_list_.push_back(f);
    }
    disk_->DeallocatePage(id);
    return true;
}

std::size_t BufferPoolManager::PinnedCount() {
    std::lock_guard<std::mutex> g(mu_);
    std::size_t n = 0;
    for (auto& p : frames_) n += p.pin_count_ > 0;
    return n;
}

bool BufferPoolManager::IsResident(page_id_t id) {
    std::lock_guard<std::mutex> g(mu_);
    return page_table_.count(id) > 0;
}

}  // namespace arbordb
