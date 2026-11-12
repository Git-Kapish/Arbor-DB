#pragma once

#include <cstring>
#include <shared_mutex>

#include "common/config.h"

namespace arbordb {

// Every page (heap or index) starts with the same 16-byte header:
//   [0,4)  page_id   [4,8) reserved   [8,16) lsn of last mutation
constexpr std::size_t kOffsetPageId = 0;
constexpr std::size_t kOffsetLsn = 8;
constexpr std::size_t kCommonHeaderSize = 16;

// A buffer-pool frame. data_ is the on-disk image; the rest is in-memory metadata
// owned by the BufferPoolManager.
class Page {
public:
    char* GetData() { return data_; }
    const char* GetData() const { return data_; }
    page_id_t GetPageId() const { return page_id_; }
    int GetPinCount() const { return pin_count_; }
    bool IsDirty() const { return is_dirty_; }

    lsn_t GetLSN() const {
        lsn_t l;
        std::memcpy(&l, data_ + kOffsetLsn, sizeof(l));
        return l;
    }
    void SetLSN(lsn_t l) { std::memcpy(data_ + kOffsetLsn, &l, sizeof(l)); }

    // Page content latch: callers hold it while reading/modifying data_.
    void WLatch() { latch_.lock(); }
    void WUnlatch() { latch_.unlock(); }
    void RLatch() { latch_.lock_shared(); }
    void RUnlatch() { latch_.unlock_shared(); }

private:
    friend class BufferPoolManager;
    char data_[kPageSize]{};
    page_id_t page_id_{kInvalidPageId};
    int pin_count_{0};
    bool is_dirty_{false};
    std::shared_mutex latch_;
};

}  // namespace arbordb
