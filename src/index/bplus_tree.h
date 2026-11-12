#pragma once

#include <algorithm>
#include <cstring>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "buffer/buffer_pool_manager.h"

namespace arbordb {

// B+Tree mapping unique fixed-size keys to RIDs. Every node is a buffer-pool page.
//
// Node page layout (after the 16-byte common page header):
//   [16,20) node type (1 leaf, 2 internal)  [20,24) entry count  [24,28) next leaf page id  [32,...) entries
//   leaf entries:     (Key, RID)       internal entries: (Key, child page id); key of entry 0 is unused.
//
// Separator rule: in an internal node, child i holds keys k with keys[i-1] <= k < keys[i].
//
// Occupancy: leaves hold at most leaf_max entries and at least leaf_max/2 (root excepted); internal
// nodes hold at most internal_max children and at least ceil(internal_max/2) (root: >= 2 children).
// Underflow is repaired by borrowing from a sibling, or merging when both fit in one node; merging can
// cascade up and shrink the tree by a level. Overflow splits, cascading up and growing the tree.
//
// Concurrency: one coarse mutex serialises all tree operations (a documented simplification; latch
// crabbing is a "what I'd do at scale" item). Iterators copy a leaf at a time and are not isolated
// from concurrent modification.
//
// Durability: tree pages are not write-ahead logged. The root page id lives in memory only; the
// engine rebuilds indexes from the table heap at startup (see README Known Gaps).
template <typename Key, typename Less = std::less<Key>>
class BPlusTree {
    static_assert(std::is_trivially_copyable<Key>::value, "keys are memcpy'd into pages");

    static constexpr uint32_t kLeafType = 1, kInternalType = 2;
    static constexpr std::size_t kOffType = kCommonHeaderSize, kOffSize = kCommonHeaderSize + 4,
                                 kOffNext = kCommonHeaderSize + 8, kEntries = 32;

    struct Leaf {
        std::vector<Key> keys;
        std::vector<RID> vals;
        page_id_t next{kInvalidPageId};
    };
    struct Internal {
        std::vector<Key> keys;  // size == children.size() - 1
        std::vector<page_id_t> children;
    };
    using Path = std::vector<std::pair<page_id_t, std::size_t>>;  // (internal page, child index taken)

public:
    static constexpr std::size_t kMaxLeaf = (kPageSize - kEntries) / (sizeof(Key) + sizeof(RID));
    static constexpr std::size_t kMaxInternal = (kPageSize - kEntries) / (sizeof(Key) + sizeof(page_id_t));

    // Small max sizes are allowed so tests can build deep trees from few keys.
    explicit BPlusTree(BufferPoolManager* bpm, std::size_t leaf_max = kMaxLeaf, std::size_t internal_max = kMaxInternal,
                       Less less = Less())
        : bpm_(bpm), less_(less), leaf_max_(leaf_max), internal_max_(internal_max) {
        if (leaf_max_ < 2 || leaf_max_ > kMaxLeaf) throw std::invalid_argument("bad leaf_max");
        if (internal_max_ < 3 || internal_max_ > kMaxInternal) throw std::invalid_argument("bad internal_max");
        min_leaf_ = leaf_max_ / 2;
        min_internal_ = (internal_max_ + 1) / 2;
    }

    // Forward iterator over (key, rid) in key order.
    class Iterator {
    public:
        bool Valid() const { return i_ < keys_.size(); }
        const Key& key() const { return keys_[i_]; }
        const RID& value() const { return vals_[i_]; }
        void Next() {
            i_++;
            Normalize();
        }

    private:
        friend class BPlusTree;
        void Normalize() {
            while (i_ >= keys_.size() && next_ != kInvalidPageId) {
                Leaf l = tree_->ReadLeafLocked(next_);
                keys_ = std::move(l.keys);
                vals_ = std::move(l.vals);
                next_ = l.next;
                i_ = 0;
            }
        }
        BPlusTree* tree_{nullptr};
        std::vector<Key> keys_;
        std::vector<RID> vals_;
        page_id_t next_{kInvalidPageId};
        std::size_t i_{0};
    };

    bool IsEmpty() const { return root_ == kInvalidPageId; }

    bool GetValue(const Key& k, RID* out) {
        std::lock_guard<std::mutex> g(mu_);
        if (root_ == kInvalidPageId) return false;
        Leaf leaf = ReadLeaf(FindLeaf(k, nullptr));
        auto it = std::lower_bound(leaf.keys.begin(), leaf.keys.end(), k, less_);
        if (it == leaf.keys.end() || less_(k, *it)) return false;
        *out = leaf.vals[it - leaf.keys.begin()];
        return true;
    }

    // Returns false (and changes nothing) if the key already exists.
    bool Insert(const Key& k, const RID& v) {
        std::lock_guard<std::mutex> g(mu_);
        if (root_ == kInvalidPageId) {
            Leaf l;
            l.keys = {k};
            l.vals = {v};
            page_id_t id = NewNodePage();
            WriteLeaf(id, l);
            root_ = id;
            return true;
        }
        Path path;
        page_id_t leaf_id = FindLeaf(k, &path);
        Leaf leaf = ReadLeaf(leaf_id);
        auto it = std::lower_bound(leaf.keys.begin(), leaf.keys.end(), k, less_);
        if (it != leaf.keys.end() && !less_(k, *it)) return false;
        std::size_t pos = it - leaf.keys.begin();
        leaf.keys.insert(leaf.keys.begin() + pos, k);
        leaf.vals.insert(leaf.vals.begin() + pos, v);
        if (leaf.keys.size() <= leaf_max_) {
            WriteLeaf(leaf_id, leaf);
            return true;
        }
        // Split: left keeps ceil(n/2).
        std::size_t left_n = (leaf.keys.size() + 1) / 2;
        Leaf right;
        right.keys.assign(leaf.keys.begin() + left_n, leaf.keys.end());
        right.vals.assign(leaf.vals.begin() + left_n, leaf.vals.end());
        leaf.keys.resize(left_n);
        leaf.vals.resize(left_n);
        page_id_t right_id = NewNodePage();
        right.next = leaf.next;
        leaf.next = right_id;
        WriteLeaf(right_id, right);
        WriteLeaf(leaf_id, leaf);
        InsertIntoParent(path, leaf_id, right.keys[0], right_id);
        return true;
    }

    // Returns false if the key is absent.
    bool Remove(const Key& k) {
        std::lock_guard<std::mutex> g(mu_);
        if (root_ == kInvalidPageId) return false;
        Path path;
        page_id_t leaf_id = FindLeaf(k, &path);
        Leaf leaf = ReadLeaf(leaf_id);
        auto it = std::lower_bound(leaf.keys.begin(), leaf.keys.end(), k, less_);
        if (it == leaf.keys.end() || less_(k, *it)) return false;
        std::size_t pos = it - leaf.keys.begin();
        leaf.keys.erase(leaf.keys.begin() + pos);
        leaf.vals.erase(leaf.vals.begin() + pos);

        if (path.empty()) {  // the leaf is the root
            if (leaf.keys.empty()) {
                FreePage(leaf_id);
                root_ = kInvalidPageId;
            } else {
                WriteLeaf(leaf_id, leaf);
            }
            return true;
        }
        if (leaf.keys.size() >= min_leaf_) {
            WriteLeaf(leaf_id, leaf);
            return true;
        }
        auto [parent_id, idx] = path.back();
        path.pop_back();
        Internal parent = ReadInternal(parent_id);
        bool use_left = idx > 0;
        std::size_t sib_idx = use_left ? idx - 1 : idx + 1;
        page_id_t sib_id = parent.children[sib_idx];
        Leaf sib = ReadLeaf(sib_id);
        if (leaf.keys.size() + sib.keys.size() <= leaf_max_) {
            Leaf& L = use_left ? sib : leaf;
            Leaf& R = use_left ? leaf : sib;
            page_id_t Lid = use_left ? sib_id : leaf_id, Rid = use_left ? leaf_id : sib_id;
            L.keys.insert(L.keys.end(), R.keys.begin(), R.keys.end());
            L.vals.insert(L.vals.end(), R.vals.begin(), R.vals.end());
            L.next = R.next;
            WriteLeaf(Lid, L);
            FreePage(Rid);
            std::size_t sep = use_left ? idx - 1 : idx;
            parent.keys.erase(parent.keys.begin() + sep);
            parent.children.erase(parent.children.begin() + sep + 1);
            FixInternal(path, parent_id, std::move(parent));
        } else if (use_left) {  // borrow the left sibling's last entry
            leaf.keys.insert(leaf.keys.begin(), sib.keys.back());
            leaf.vals.insert(leaf.vals.begin(), sib.vals.back());
            sib.keys.pop_back();
            sib.vals.pop_back();
            parent.keys[idx - 1] = leaf.keys[0];
            WriteLeaf(leaf_id, leaf);
            WriteLeaf(sib_id, sib);
            WriteInternal(parent_id, parent);
        } else {  // borrow the right sibling's first entry
            leaf.keys.push_back(sib.keys.front());
            leaf.vals.push_back(sib.vals.front());
            sib.keys.erase(sib.keys.begin());
            sib.vals.erase(sib.vals.begin());
            parent.keys[idx] = sib.keys.front();
            WriteLeaf(leaf_id, leaf);
            WriteLeaf(sib_id, sib);
            WriteInternal(parent_id, parent);
        }
        return true;
    }

    Iterator Begin() {
        std::lock_guard<std::mutex> g(mu_);
        Iterator it;
        it.tree_ = this;
        if (root_ == kInvalidPageId) return it;
        page_id_t cur = root_;
        while (!IsLeafPage(cur)) cur = ReadInternal(cur).children.front();
        Leaf l = ReadLeaf(cur);
        it.keys_ = std::move(l.keys);
        it.vals_ = std::move(l.vals);
        it.next_ = l.next;
        it.Normalize();
        return it;
    }

    // First entry with key >= k.
    Iterator LowerBound(const Key& k) {
        std::lock_guard<std::mutex> g(mu_);
        Iterator it;
        it.tree_ = this;
        if (root_ == kInvalidPageId) return it;
        Leaf l = ReadLeaf(FindLeaf(k, nullptr));
        it.i_ = std::lower_bound(l.keys.begin(), l.keys.end(), k, less_) - l.keys.begin();
        it.keys_ = std::move(l.keys);
        it.vals_ = std::move(l.vals);
        it.next_ = l.next;
        it.Normalize();
        return it;
    }

    std::size_t Size() {
        std::size_t n = 0;
        for (auto it = Begin(); it.Valid(); it.Next()) n++;
        return n;
    }

    int Height() {
        std::lock_guard<std::mutex> g(mu_);
        if (root_ == kInvalidPageId) return 0;
        int h = 1;
        page_id_t cur = root_;
        while (!IsLeafPage(cur)) {
            cur = ReadInternal(cur).children.front();
            h++;
        }
        return h;
    }

    // Verifies every structural invariant; returns false and fills *err on the first violation.
    bool CheckInvariants(std::string* err) {
        std::lock_guard<std::mutex> g(mu_);
        std::ostringstream e;
        std::vector<page_id_t> leaves_in_order;
        if (root_ != kInvalidPageId) {
            int depth = -1;
            if (!Check(root_, nullptr, nullptr, 1, &depth, true, &leaves_in_order, e)) {
                *err = e.str();
                return false;
            }
            // Leaf chain must visit exactly the leaves found by the tree walk, in order.
            page_id_t cur = leaves_in_order.front();
            for (std::size_t i = 0; i < leaves_in_order.size(); i++) {
                if (cur != leaves_in_order[i]) {
                    *err = "leaf chain differs from tree order at leaf " + std::to_string(i);
                    return false;
                }
                cur = ReadLeaf(cur).next;
            }
            if (cur != kInvalidPageId) {
                *err = "leaf chain longer than tree";
                return false;
            }
        }
        if (bpm_->PinnedCount() != 0) {
            *err = "pages left pinned";
            return false;
        }
        return true;
    }

private:
    // ---- page <-> node conversion (each helper pins, copies, unpins) ----
    Page* Fetch(page_id_t id) {
        Page* p = bpm_->FetchPage(id);
        if (!p) throw std::runtime_error("buffer pool exhausted");
        return p;
    }
    page_id_t NewNodePage() {
        page_id_t id;
        if (!bpm_->NewPage(&id)) throw std::runtime_error("buffer pool exhausted");
        bpm_->UnpinPage(id, true);
        return id;
    }
    void FreePage(page_id_t id) { bpm_->DeletePage(id); }

    bool IsLeafPage(page_id_t id) {
        PageGuard g(bpm_, Fetch(id));
        uint32_t t;
        std::memcpy(&t, g->GetData() + kOffType, 4);
        return t == kLeafType;
    }

    Leaf ReadLeaf(page_id_t id) {
        PageGuard g(bpm_, Fetch(id));
        g->RLatch();
        const char* d = g->GetData();
        uint32_t n;
        std::memcpy(&n, d + kOffSize, 4);
        Leaf l;
        std::memcpy(&l.next, d + kOffNext, sizeof(page_id_t));
        l.keys.resize(n);
        l.vals.resize(n);
        for (uint32_t i = 0; i < n; i++) {
            std::memcpy(&l.keys[i], d + kEntries + i * (sizeof(Key) + sizeof(RID)), sizeof(Key));
            std::memcpy(&l.vals[i], d + kEntries + i * (sizeof(Key) + sizeof(RID)) + sizeof(Key), sizeof(RID));
        }
        g->RUnlatch();
        return l;
    }
    Leaf ReadLeafLocked(page_id_t id) {  // for iterators, outside the tree's own critical sections
        std::lock_guard<std::mutex> g(mu_);
        return ReadLeaf(id);
    }
    void WriteLeaf(page_id_t id, const Leaf& l) {
        PageGuard g(bpm_, Fetch(id));
        g->WLatch();
        char* d = g->GetData();
        uint32_t t = kLeafType, n = static_cast<uint32_t>(l.keys.size());
        std::memcpy(d + kOffType, &t, 4);
        std::memcpy(d + kOffSize, &n, 4);
        std::memcpy(d + kOffNext, &l.next, sizeof(page_id_t));
        for (uint32_t i = 0; i < n; i++) {
            std::memcpy(d + kEntries + i * (sizeof(Key) + sizeof(RID)), &l.keys[i], sizeof(Key));
            std::memcpy(d + kEntries + i * (sizeof(Key) + sizeof(RID)) + sizeof(Key), &l.vals[i], sizeof(RID));
        }
        g->WUnlatch();
        g.MarkDirty();
    }
    Internal ReadInternal(page_id_t id) {
        PageGuard g(bpm_, Fetch(id));
        g->RLatch();
        const char* d = g->GetData();
        uint32_t n;
        std::memcpy(&n, d + kOffSize, 4);
        Internal in;
        in.children.resize(n);
        in.keys.resize(n ? n - 1 : 0);
        for (uint32_t i = 0; i < n; i++) {
            const char* e = d + kEntries + i * (sizeof(Key) + sizeof(page_id_t));
            if (i > 0) std::memcpy(&in.keys[i - 1], e, sizeof(Key));
            std::memcpy(&in.children[i], e + sizeof(Key), sizeof(page_id_t));
        }
        g->RUnlatch();
        return in;
    }
    void WriteInternal(page_id_t id, const Internal& in) {
        PageGuard g(bpm_, Fetch(id));
        g->WLatch();
        char* d = g->GetData();
        uint32_t t = kInternalType, n = static_cast<uint32_t>(in.children.size());
        std::memcpy(d + kOffType, &t, 4);
        std::memcpy(d + kOffSize, &n, 4);
        for (uint32_t i = 0; i < n; i++) {
            char* e = d + kEntries + i * (sizeof(Key) + sizeof(page_id_t));
            if (i > 0) std::memcpy(e, &in.keys[i - 1], sizeof(Key));
            else std::memset(e, 0, sizeof(Key));
            std::memcpy(e + sizeof(Key), &in.children[i], sizeof(page_id_t));
        }
        g->WUnlatch();
        g.MarkDirty();
    }

    // ---- algorithms ----
    page_id_t FindLeaf(const Key& k, Path* path) {
        page_id_t cur = root_;
        while (!IsLeafPage(cur)) {
            Internal in = ReadInternal(cur);
            std::size_t idx = std::upper_bound(in.keys.begin(), in.keys.end(), k, less_) - in.keys.begin();
            if (path) path->push_back({cur, idx});
            cur = in.children[idx];
        }
        return cur;
    }

    // `left` was split; `right` is the new sibling and `sep` the smallest key under it.
    void InsertIntoParent(Path& path, page_id_t left, const Key& sep, page_id_t right) {
        if (path.empty()) {  // root split: the tree grows by one level
            Internal r;
            r.keys = {sep};
            r.children = {left, right};
            page_id_t id = NewNodePage();
            WriteInternal(id, r);
            root_ = id;
            return;
        }
        auto [pid, idx] = path.back();
        path.pop_back();
        Internal p = ReadInternal(pid);
        p.keys.insert(p.keys.begin() + idx, sep);
        p.children.insert(p.children.begin() + idx + 1, right);
        if (p.children.size() <= internal_max_) {
            WriteInternal(pid, p);
            return;
        }
        std::size_t left_c = (p.children.size() + 1) / 2;
        Internal r;
        r.children.assign(p.children.begin() + left_c, p.children.end());
        Key promoted = p.keys[left_c - 1];
        r.keys.assign(p.keys.begin() + left_c, p.keys.end());
        p.children.resize(left_c);
        p.keys.resize(left_c - 1);
        page_id_t rid = NewNodePage();
        WriteInternal(rid, r);
        WriteInternal(pid, p);
        InsertIntoParent(path, pid, promoted, rid);
    }

    // `node` (page `pid`) just lost a child. `path` leads from the root to node's parent.
    void FixInternal(Path& path, page_id_t pid, Internal node) {
        while (true) {
            if (path.empty()) {  // node is the root
                if (node.children.size() == 1) {  // root with one child: the tree shrinks a level
                    root_ = node.children[0];
                    FreePage(pid);
                } else {
                    WriteInternal(pid, node);
                }
                return;
            }
            if (node.children.size() >= min_internal_) {
                WriteInternal(pid, node);
                return;
            }
            auto [gid, idx] = path.back();
            path.pop_back();
            Internal parent = ReadInternal(gid);
            bool use_left = idx > 0;
            std::size_t sib_idx = use_left ? idx - 1 : idx + 1;
            page_id_t sib_id = parent.children[sib_idx];
            Internal sib = ReadInternal(sib_id);
            if (node.children.size() + sib.children.size() <= internal_max_) {
                Internal& L = use_left ? sib : node;
                Internal& R = use_left ? node : sib;
                page_id_t Lid = use_left ? sib_id : pid, Rid = use_left ? pid : sib_id;
                std::size_t sep = use_left ? idx - 1 : idx;
                L.keys.push_back(parent.keys[sep]);  // separator comes down
                L.keys.insert(L.keys.end(), R.keys.begin(), R.keys.end());
                L.children.insert(L.children.end(), R.children.begin(), R.children.end());
                WriteInternal(Lid, L);
                FreePage(Rid);
                parent.keys.erase(parent.keys.begin() + sep);
                parent.children.erase(parent.children.begin() + sep + 1);
                pid = gid;
                node = std::move(parent);
                continue;
            }
            if (use_left) {  // rotate right through the parent
                node.children.insert(node.children.begin(), sib.children.back());
                node.keys.insert(node.keys.begin(), parent.keys[idx - 1]);
                parent.keys[idx - 1] = sib.keys.back();
                sib.children.pop_back();
                sib.keys.pop_back();
            } else {  // rotate left through the parent
                node.children.push_back(sib.children.front());
                node.keys.push_back(parent.keys[idx]);
                parent.keys[idx] = sib.keys.front();
                sib.children.erase(sib.children.begin());
                sib.keys.erase(sib.keys.begin());
            }
            WriteInternal(pid, node);
            WriteInternal(sib_id, sib);
            WriteInternal(gid, parent);
            return;
        }
    }

    bool Check(page_id_t id, const Key* lo, const Key* hi, int depth, int* leaf_depth, bool is_root,
               std::vector<page_id_t>* leaves, std::ostringstream& e) {
        auto in_range = [&](const Key& k) { return (!lo || !less_(k, *lo)) && (!hi || less_(k, *hi)); };
        if (IsLeafPage(id)) {
            Leaf l = ReadLeaf(id);
            if (*leaf_depth == -1) *leaf_depth = depth;
            if (*leaf_depth != depth) { e << "leaves at different depths"; return false; }
            if (l.keys.size() > leaf_max_) { e << "leaf " << id << " overfull"; return false; }
            if (!is_root && l.keys.size() < min_leaf_) { e << "leaf " << id << " underfull"; return false; }
            if (is_root && l.keys.empty()) { e << "empty root leaf"; return false; }
            for (std::size_t i = 0; i < l.keys.size(); i++) {
                if (i > 0 && !less_(l.keys[i - 1], l.keys[i])) { e << "leaf " << id << " not strictly sorted"; return false; }
                if (!in_range(l.keys[i])) { e << "leaf " << id << " key outside separator range"; return false; }
            }
            leaves->push_back(id);
            return true;
        }
        Internal in = ReadInternal(id);
        if (in.children.size() > internal_max_) { e << "internal " << id << " overfull"; return false; }
        if (is_root ? in.children.size() < 2 : in.children.size() < min_internal_) {
            e << "internal " << id << " underfull (" << in.children.size() << " children)";
            return false;
        }
        for (std::size_t i = 0; i < in.keys.size(); i++) {
            if (i > 0 && !less_(in.keys[i - 1], in.keys[i])) { e << "internal " << id << " keys unsorted"; return false; }
            if (!in_range(in.keys[i])) { e << "internal " << id << " key outside range"; return false; }
        }
        for (std::size_t i = 0; i < in.children.size(); i++) {
            const Key* clo = i == 0 ? lo : &in.keys[i - 1];
            const Key* chi = i + 1 == in.children.size() ? hi : &in.keys[i];
            if (!Check(in.children[i], clo, chi, depth + 1, leaf_depth, false, leaves, e)) return false;
        }
        return true;
    }

    BufferPoolManager* bpm_;
    Less less_;
    std::size_t leaf_max_, internal_max_, min_leaf_, min_internal_;
    page_id_t root_{kInvalidPageId};
    std::mutex mu_;
};

}  // namespace arbordb
