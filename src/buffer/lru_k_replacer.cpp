#include "buffer/lru_k_replacer.h"

namespace arbordb {

void LRUKReplacer::RecordAccess(frame_id_t f) {
    std::lock_guard<std::mutex> g(mu_);
    Node& n = nodes_[f];
    n.history.push_back(clock_++);
    if (n.history.size() > k_) n.history.pop_front();
}

void LRUKReplacer::Pin(frame_id_t f) {
    std::lock_guard<std::mutex> g(mu_);
    nodes_[f].evictable = false;
}

void LRUKReplacer::Unpin(frame_id_t f) {
    std::lock_guard<std::mutex> g(mu_);
    nodes_[f].evictable = true;
}

void LRUKReplacer::Remove(frame_id_t f) {
    std::lock_guard<std::mutex> g(mu_);
    nodes_.erase(f);
}

std::size_t LRUKReplacer::Size() const {
    std::lock_guard<std::mutex> g(mu_);
    std::size_t n = 0;
    for (auto& [f, node] : nodes_) n += node.evictable;
    return n;
}

bool LRUKReplacer::Victim(frame_id_t* out) {
    std::lock_guard<std::mutex> g(mu_);
    bool have_inf = false, found = false;
    frame_id_t best = 0;
    uint64_t best_ts = 0;
    for (auto& [f, n] : nodes_) {
        if (!n.evictable || n.history.empty()) continue;
        bool inf = n.history.size() < k_;
        // history.front() is the first access for inf frames and the K-th most recent otherwise;
        // in both cases the smaller timestamp is the better victim.
        uint64_t ts = n.history.front();
        if (!found || (inf && !have_inf) || (inf == have_inf && ts < best_ts)) {
            found = true;
            have_inf = inf;
            best = f;
            best_ts = ts;
        }
    }
    if (!found) return false;
    nodes_.erase(best);
    *out = best;
    return true;
}

}  // namespace arbordb
