#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>

#include "buffer/replacer.h"

namespace arbordb {

// Plain LRU: evict the evictable frame whose most recent access is oldest. It is the baseline the LRU-K
// policy is benchmarked against (bench/): a single sequential scan touches every page once and, to LRU,
// those pages look newer than the hot working set, so the scan flushes the cache.
class LRUReplacer : public Replacer {
public:
    bool Victim(frame_id_t* out) override {
        std::lock_guard<std::mutex> g(mu_);
        if (by_tick_.empty()) return false;
        auto it = by_tick_.begin();
        *out = it->second;
        info_.erase(it->second);
        by_tick_.erase(it);
        return true;
    }
    void Pin(frame_id_t f) override {
        std::lock_guard<std::mutex> g(mu_);
        Info& i = info_[f];
        if (i.evictable) by_tick_.erase(i.tick);
        i.evictable = false;
    }
    void Unpin(frame_id_t f) override {
        std::lock_guard<std::mutex> g(mu_);
        Info& i = info_[f];
        if (!i.evictable) by_tick_[i.tick] = f;
        i.evictable = true;
    }
    void RecordAccess(frame_id_t f) override {
        std::lock_guard<std::mutex> g(mu_);
        Info& i = info_[f];
        if (i.evictable) by_tick_.erase(i.tick);
        i.tick = ++clock_;
        if (i.evictable) by_tick_[i.tick] = f;
    }
    void Remove(frame_id_t f) override {
        std::lock_guard<std::mutex> g(mu_);
        auto it = info_.find(f);
        if (it == info_.end()) return;
        if (it->second.evictable) by_tick_.erase(it->second.tick);
        info_.erase(it);
    }
    std::size_t Size() const override {
        std::lock_guard<std::mutex> g(mu_);
        return by_tick_.size();
    }

private:
    struct Info {
        uint64_t tick{0};
        bool evictable{false};
    };
    mutable std::mutex mu_;
    uint64_t clock_{0};
    std::unordered_map<frame_id_t, Info> info_;
    std::map<uint64_t, frame_id_t> by_tick_;  // evictable frames ordered by last access
};

}  // namespace arbordb
