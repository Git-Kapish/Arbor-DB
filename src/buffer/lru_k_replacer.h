#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>

#include "buffer/replacer.h"

namespace arbordb {

// LRU-K: evict the evictable frame with the largest backward K-distance (time since its K-th most
// recent access). Frames with fewer than K recorded accesses have infinite distance and go first
// (oldest first access wins among them). A one-off sequential scan touches each page once, so scan
// pages stay at infinite distance and are evicted before pages with a real reuse history -- plain
// LRU would instead evict the hot pages.
class LRUKReplacer : public Replacer {
public:
    explicit LRUKReplacer(std::size_t k = kLRUKValue) : k_(k) {}

    bool Victim(frame_id_t* out) override;
    void Pin(frame_id_t f) override;
    void Unpin(frame_id_t f) override;
    void RecordAccess(frame_id_t f) override;
    void Remove(frame_id_t f) override;
    std::size_t Size() const override;

private:
    struct Node {
        std::deque<uint64_t> history;  // up to k most recent access timestamps, oldest first
        bool evictable{false};
    };
    std::size_t k_;
    uint64_t clock_{0};
    std::map<frame_id_t, Node> nodes_;  // ordered: deterministic tie-breaking
    mutable std::mutex mu_;
};

}  // namespace arbordb
