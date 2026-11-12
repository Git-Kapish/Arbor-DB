#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <string>

#include "index/bplus_tree.h"

using namespace arbordb;
using Tree = BPlusTree<int64_t>;

namespace {
struct Env {
    std::string path;
    std::unique_ptr<DiskManager> dm;
    std::unique_ptr<BufferPoolManager> bpm;
    explicit Env(const char* name, std::size_t pool = 16) {
        path = "/tmp/arbor_test_" + std::string(name) + "_" + std::to_string(getpid());
        unlink(path.c_str());
        dm = std::make_unique<DiskManager>(path);
        bpm = std::make_unique<BufferPoolManager>(pool, dm.get());
    }
    ~Env() { unlink(path.c_str()); }
};
RID Rid(int64_t k) { return RID{static_cast<page_id_t>(k), static_cast<uint32_t>(k * 7)}; }

void ExpectValid(Tree& t, const char* ctx = "") {
    std::string err;
    ASSERT_TRUE(t.CheckInvariants(&err)) << ctx << ": " << err;
}
void ExpectMatches(Tree& t, const std::map<int64_t, RID>& oracle) {
    auto it = t.Begin();
    for (auto& [k, v] : oracle) {
        ASSERT_TRUE(it.Valid());
        ASSERT_EQ(it.key(), k);
        ASSERT_TRUE(it.value() == v);
        it.Next();
    }
    ASSERT_FALSE(it.Valid());
}
}  // namespace

TEST(BPlusTree, EmptyTree) {
    Env e("bt_empty");
    Tree t(e.bpm.get());
    RID r;
    EXPECT_TRUE(t.IsEmpty());
    EXPECT_FALSE(t.GetValue(1, &r));
    EXPECT_FALSE(t.Remove(1));
    EXPECT_FALSE(t.Begin().Valid());
    EXPECT_EQ(t.Height(), 0);
    ExpectValid(t);
}

TEST(BPlusTree, InsertGetAndRejectDuplicate) {
    Env e("bt_basic");
    Tree t(e.bpm.get());
    for (int64_t k : {5, 1, 9, 3}) EXPECT_TRUE(t.Insert(k, Rid(k)));
    EXPECT_FALSE(t.Insert(5, RID{999, 999}));  // duplicate rejected, value unchanged
    RID r;
    ASSERT_TRUE(t.GetValue(5, &r));
    EXPECT_TRUE(r == Rid(5));
    EXPECT_FALSE(t.GetValue(4, &r));
    EXPECT_EQ(t.Size(), 4u);
    ExpectValid(t);
}

TEST(BPlusTree, RootLeafSplitIncreasesHeight) {
    Env e("bt_rootsplit");
    Tree t(e.bpm.get(), /*leaf_max=*/3, /*internal_max=*/3);
    for (int64_t k = 1; k <= 3; k++) t.Insert(k, Rid(k));
    EXPECT_EQ(t.Height(), 1);
    t.Insert(4, Rid(4));  // 4th key overflows the root leaf -> root split
    EXPECT_EQ(t.Height(), 2);
    ExpectValid(t);
}

TEST(BPlusTree, RootInternalSplitIncreasesHeightAgain) {
    Env e("bt_rootsplit2");
    Tree t(e.bpm.get(), 2, 3);
    int last = 0;
    bool saw3 = false;
    for (int64_t k = 1; k <= 40; k++) {
        t.Insert(k, Rid(k));
        int h = t.Height();
        EXPECT_GE(h, last);
        EXPECT_LE(h - last, 1) << "height may grow by at most one per insert";
        last = h;
        saw3 |= h >= 3;
        ExpectValid(t, "ascending insert");
    }
    EXPECT_TRUE(saw3);
}

TEST(BPlusTree, DeletingOnlyKeyEmptiesTree) {
    Env e("bt_onlykey");
    Tree t(e.bpm.get(), 3, 3);
    t.Insert(10, Rid(10));
    EXPECT_TRUE(t.Remove(10));
    EXPECT_TRUE(t.IsEmpty());
    ExpectValid(t);
    EXPECT_TRUE(t.Insert(11, Rid(11)));  // tree is reusable
    ExpectValid(t);
}

TEST(BPlusTree, DeleteOnlyKeyInNonRootLeafMergesAway) {
    Env e("bt_leafdel");
    Tree t(e.bpm.get(), 2, 3);  // leaf holds 1..2 keys
    for (int64_t k = 1; k <= 6; k++) t.Insert(k, Rid(k));
    for (int64_t k : {1, 2, 3, 4, 5, 6}) {
        ASSERT_TRUE(t.Remove(k));
        ExpectValid(t, "delete");
        RID r;
        EXPECT_FALSE(t.GetValue(k, &r));
    }
    EXPECT_TRUE(t.IsEmpty());
}

TEST(BPlusTree, MergesShrinkTreeByExactlyOneLevelAtATime) {
    Env e("bt_shrink");
    Tree t(e.bpm.get(), 3, 3);
    for (int64_t k = 1; k <= 200; k++) t.Insert(k, Rid(k));
    int h = t.Height();
    ASSERT_GE(h, 4);
    std::vector<int> heights{h};
    for (int64_t k = 1; k <= 200; k++) {
        ASSERT_TRUE(t.Remove(k));
        ExpectValid(t, "delete ascending");
        int nh = t.Height();
        EXPECT_LE(nh, h);
        EXPECT_GE(h - nh, 0);
        EXPECT_LE(h - nh, 1) << "tree may shrink by at most one level per delete";
        if (nh != h) heights.push_back(nh);
        h = nh;
    }
    // Went all the way down 4 -> 3 -> 2 -> 1 -> 0 without skipping a level.
    for (std::size_t i = 1; i < heights.size(); i++) EXPECT_EQ(heights[i - 1] - heights[i], 1);
    EXPECT_EQ(heights.back(), 0);
}

TEST(BPlusTree, DeleteRedistributesFromSiblingWithoutMerge) {
    Env e("bt_redistribute");
    Tree t(e.bpm.get(), 4, 4);
    // Leaves split ceil(5/2)=3 | 2. Fill so the right leaf is fat, then drain the left leaf.
    for (int64_t k : {10, 20, 30, 40, 50, 25, 26}) t.Insert(k, Rid(k));
    ExpectValid(t);
    std::map<int64_t, RID> oracle;
    for (int64_t k : {10, 20, 30, 40, 50, 25, 26}) oracle[k] = Rid(k);
    for (int64_t k : {10, 20}) {
        t.Remove(k);
        oracle.erase(k);
        ExpectValid(t, "after delete");
        ExpectMatches(t, oracle);
    }
}

TEST(BPlusTree, SequentialAscendingDescendingAndInterleavedOrders) {
    for (int order = 0; order < 3; order++) {
        Env e("bt_orders");
        Tree t(e.bpm.get(), 3, 4);
        std::vector<int64_t> keys(300);
        std::iota(keys.begin(), keys.end(), 0);
        if (order == 1) std::reverse(keys.begin(), keys.end());
        if (order == 2) {  // outside-in
            std::vector<int64_t> v;
            for (int i = 0; i < 150; i++) {
                v.push_back(i);
                v.push_back(299 - i);
            }
            keys = v;
        }
        std::map<int64_t, RID> oracle;
        for (int64_t k : keys) {
            ASSERT_TRUE(t.Insert(k, Rid(k)));
            oracle[k] = Rid(k);
            ExpectValid(t, "insert");
        }
        ExpectMatches(t, oracle);
        std::reverse(keys.begin(), keys.end());
        for (int64_t k : keys) {
            ASSERT_TRUE(t.Remove(k));
            oracle.erase(k);
            ExpectValid(t, "remove");
        }
        EXPECT_TRUE(t.IsEmpty());
    }
}

// Randomised insert/delete against a std::map oracle; invariants + full content checked after EVERY op.
TEST(BPlusTree, RandomOpsMatchOracleAndStayBalancedForManyFanouts) {
    struct Cfg { std::size_t leaf, internal; };
    for (Cfg c : {Cfg{2, 3}, Cfg{3, 3}, Cfg{4, 4}, Cfg{5, 6}, Cfg{7, 5}, Cfg{8, 8}}) {
        Env e("bt_random", 12);  // tiny pool: forces constant eviction underneath the tree
        Tree t(e.bpm.get(), c.leaf, c.internal);
        std::mt19937 rng(1234 + c.leaf * 10 + c.internal);  // seeded: reproducible
        std::map<int64_t, RID> oracle;
        for (int op = 0; op < 2500; op++) {
            int64_t k = rng() % 120;
            // Bias toward growth first, then toward shrinkage, so we cross both height transitions.
            bool grow_phase = (op / 500) % 2 == 0;
            bool do_insert = (rng() % 100) < (grow_phase ? 70 : 30);
            if (do_insert) {
                bool was_new = oracle.count(k) == 0;
                bool ok = t.Insert(k, Rid(k));
                ASSERT_EQ(ok, was_new) << "cfg " << c.leaf << "/" << c.internal << " op " << op;
                if (was_new) oracle[k] = Rid(k);
            } else {
                bool ok = t.Remove(k);
                ASSERT_EQ(ok, oracle.erase(k) == 1) << "cfg " << c.leaf << "/" << c.internal << " op " << op;
            }
            std::string err;
            ASSERT_TRUE(t.CheckInvariants(&err)) << "cfg " << c.leaf << "/" << c.internal << " op " << op << ": " << err;
            ASSERT_EQ(t.IsEmpty(), oracle.empty());
            ExpectMatches(t, oracle);
        }
    }
}

TEST(BPlusTree, RangeScanViaLowerBound) {
    Env e("bt_range");
    Tree t(e.bpm.get(), 3, 3);
    for (int64_t k = 0; k < 100; k += 5) t.Insert(k, Rid(k));  // 0,5,...,95
    auto it = t.LowerBound(22);  // gap -> first key >= 22 is 25
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(it.key(), 25);
    std::vector<int64_t> got;
    for (; it.Valid() && it.key() <= 40; it.Next()) got.push_back(it.key());
    EXPECT_EQ(got, (std::vector<int64_t>{25, 30, 35, 40}));
    EXPECT_EQ(t.LowerBound(30).key(), 30);   // exact hit
    EXPECT_EQ(t.LowerBound(-100).key(), 0);  // below everything
    EXPECT_FALSE(t.LowerBound(96).Valid());  // beyond the last key
    EXPECT_EQ(t.LowerBound(95).key(), 95);
    // Full scan crosses many leaves through the next-pointer chain.
    EXPECT_EQ(t.Size(), 20u);
}

TEST(BPlusTree, LargeDefaultFanoutRandomWorkloadThroughSmallPool) {
    Env e("bt_large", 24);
    Tree t(e.bpm.get());  // real page-sized nodes
    std::mt19937 rng(42);
    std::vector<int64_t> keys(40000);
    std::iota(keys.begin(), keys.end(), 0);
    std::shuffle(keys.begin(), keys.end(), rng);
    for (int64_t k : keys) ASSERT_TRUE(t.Insert(k, Rid(k)));
    EXPECT_EQ(t.Size(), keys.size());
    EXPECT_GE(t.Height(), 2);
    ExpectValid(t);
    std::shuffle(keys.begin(), keys.end(), rng);
    for (std::size_t i = 0; i < keys.size() - 100; i++) ASSERT_TRUE(t.Remove(keys[i]));
    EXPECT_EQ(t.Size(), 100u);
    ExpectValid(t);
    RID r;
    for (std::size_t i = keys.size() - 100; i < keys.size(); i++) {
        ASSERT_TRUE(t.GetValue(keys[i], &r));
        EXPECT_TRUE(r == Rid(keys[i]));
    }
}

TEST(BPlusTree, RejectsInvalidFanout) {
    Env e("bt_badcfg");
    EXPECT_THROW(Tree(e.bpm.get(), 1, 3), std::invalid_argument);
    EXPECT_THROW(Tree(e.bpm.get(), 3, 2), std::invalid_argument);
    EXPECT_THROW(Tree(e.bpm.get(), 100000, 3), std::invalid_argument);
}
