#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "buffer/replacer.h"
#include "storage/disk/disk_manager.h"
#include "storage/page/page.h"

namespace arbordb {

// What the buffer pool needs from the log to enforce the write-ahead rule (implemented by LogManager).
class LogFlusher {
public:
    virtual ~LogFlusher() = default;
    virtual lsn_t FlushedLsn() = 0;
    virtual void FlushTo(lsn_t lsn) = 0;  // make the log durable at least through lsn
};

struct BufferPoolStats {
    uint64_t hits{0};        // FetchPage served from a resident frame
    uint64_t misses{0};      // FetchPage that had to read the page from disk
    uint64_t evictions{0};   // frames reclaimed from other pages
    double HitRate() const { return hits + misses ? static_cast<double>(hits) / static_cast<double>(hits + misses) : 0.0; }
};

// Fixed-capacity pool of page frames with pin counting, dirty tracking and pluggable eviction.
// The destructor does NOT flush: dropping a pool without FlushAll() models a crash.
class BufferPoolManager {
public:
    BufferPoolManager(std::size_t pool_size, DiskManager* disk, LogFlusher* log = nullptr,
                      std::unique_ptr<Replacer> replacer = nullptr);

    // Returns a pinned page, or nullptr when every frame is pinned.
    Page* FetchPage(page_id_t id);
    // Allocates a fresh zeroed page (pinned, dirty); nullptr if every frame is pinned.
    Page* NewPage(page_id_t* id);
    bool UnpinPage(page_id_t id, bool is_dirty);
    // Write-ahead rule: before the page image goes to disk the log is flushed through the page's LSN.
    bool FlushPage(page_id_t id);
    void FlushAll();
    // Fails (false) if the page is pinned.
    bool DeletePage(page_id_t id);

    std::size_t PoolSize() const { return pool_size_; }
    std::size_t PinnedCount();           // frames with pin_count > 0 (leak detection in tests)
    bool IsResident(page_id_t id);       // test hook
    DiskManager* Disk() { return disk_; }
    BufferPoolStats Stats() const { return {hits_.load(), misses_.load(), evictions_.load()}; }
    void ResetStats() { hits_ = misses_ = evictions_ = 0; }

private:
    // Caller holds mu_. Returns a free frame (evicting if needed) or -1.
    frame_id_t AcquireFrame();
    void WriteOut(Page& p);  // WAL check + disk write; caller guarantees no concurrent modifiers

    std::size_t pool_size_;
    DiskManager* disk_;
    LogFlusher* log_;
    std::unique_ptr<Replacer> replacer_;
    std::vector<Page> frames_;
    std::unordered_map<page_id_t, frame_id_t> page_table_;
    std::deque<frame_id_t> free_list_;
    std::mutex mu_;
    std::atomic<uint64_t> hits_{0}, misses_{0}, evictions_{0};
};

// RAII pin: unpins on destruction. Use MarkDirty() if the page was modified.
class PageGuard {
public:
    PageGuard() = default;
    PageGuard(BufferPoolManager* bpm, Page* page) : bpm_(bpm), page_(page) {}
    PageGuard(PageGuard&& o) noexcept : bpm_(o.bpm_), page_(o.page_), dirty_(o.dirty_) { o.page_ = nullptr; }
    PageGuard& operator=(PageGuard&& o) noexcept {
        if (this != &o) {
            Release();
            bpm_ = o.bpm_;
            page_ = o.page_;
            dirty_ = o.dirty_;
            o.page_ = nullptr;
        }
        return *this;
    }
    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;
    ~PageGuard() { Release(); }

    Page* get() const { return page_; }
    Page* operator->() const { return page_; }
    explicit operator bool() const { return page_ != nullptr; }
    void MarkDirty() { dirty_ = true; }
    void Release() {
        if (page_) bpm_->UnpinPage(page_->GetPageId(), dirty_);
        page_ = nullptr;
    }

private:
    BufferPoolManager* bpm_{nullptr};
    Page* page_{nullptr};
    bool dirty_{false};
};

}  // namespace arbordb
