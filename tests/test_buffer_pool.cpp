#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_k_replacer.h"
#include "buffer/lru_replacer.h"

using namespace arbordb;

namespace {
struct Env {
    std::string path;
    std::unique_ptr<DiskManager> dm;
    explicit Env(const char* name) {
        path = "/tmp/arbor_test_" + std::string(name) + "_" + std::to_string(getpid());
        unlink(path.c_str());
        dm = std::make_unique<DiskManager>(path);
    }
    ~Env() { unlink(path.c_str()); }
};
constexpr std::size_t kBody = kCommonHeaderSize;  // first byte after the common header

page_id_t NewUnpinned(BufferPoolManager& bpm, char stamp = 0) {
    page_id_t id;
    Page* p = bpm.NewPage(&id);
    EXPECT_NE(p, nullptr);
    p->GetData()[kBody] = stamp;
    bpm.UnpinPage(id, true);
    return id;
}
void Touch(BufferPoolManager& bpm, page_id_t id) {
    Page* p = bpm.FetchPage(id);
    ASSERT_NE(p, nullptr);
    bpm.UnpinPage(id, false);
}

class FakeLog : public LogFlusher {
public:
    lsn_t flushed = 10;
    std::vector<lsn_t> flush_requests;
    lsn_t FlushedLsn() override { return flushed; }
    void FlushTo(lsn_t l) override {
        flush_requests.push_back(l);
        flushed = l;
    }
};
}  // namespace

// ---- LRUKReplacer ----

TEST(LRUKReplacer, InfiniteDistanceFramesGoFirstOldestFirstAccessWins) {
    LRUKReplacer r(2);
    for (frame_id_t f : {0, 1, 2}) {
        r.RecordAccess(f);
        r.Unpin(f);
    }
    r.RecordAccess(0);  // frame 0 now has 2 accesses
    frame_id_t v;
    ASSERT_TRUE(r.Victim(&v));
    EXPECT_EQ(v, 1);  // frames 1,2 have <K accesses; 1 was first
    ASSERT_TRUE(r.Victim(&v));
    EXPECT_EQ(v, 2);
    ASSERT_TRUE(r.Victim(&v));
    EXPECT_EQ(v, 0);
    EXPECT_FALSE(r.Victim(&v));
}

TEST(LRUKReplacer, LargestBackwardKDistanceAmongFullHistories) {
    LRUKReplacer r(2);
    // access order: a a b b a  (ts 0..4). K-th most recent: a->ts3?? a's last two are ts{1,4}; b's {2,3}
    r.RecordAccess(0);  // a ts0
    r.RecordAccess(0);  // a ts1
    r.RecordAccess(1);  // b ts2
    r.RecordAccess(1);  // b ts3
    r.RecordAccess(0);  // a ts4
    r.Unpin(0);
    r.Unpin(1);
    frame_id_t v;
    ASSERT_TRUE(r.Victim(&v));
    EXPECT_EQ(v, 0);  // a's 2nd-most-recent access (ts1) is older than b's (ts2)
}

TEST(LRUKReplacer, PinnedFramesAreNeverVictims) {
    LRUKReplacer r(2);
    r.RecordAccess(0);
    r.Pin(0);
    frame_id_t v;
    EXPECT_FALSE(r.Victim(&v));
    r.Unpin(0);
    EXPECT_EQ(r.Size(), 1u);
    EXPECT_TRUE(r.Victim(&v));
}

// ---- BufferPoolManager ----

TEST(BufferPool, EvictsLruKVictimNotPlainLruVictim) {
    Env e("bp_lruk");
    BufferPoolManager bpm(3, e.dm.get());
    // Access timestamps: p1 {1,6}, p2 {2,3}, p3 {4,5} (K=2 keeps the last two).
    page_id_t p1 = NewUnpinned(bpm);  // ts0
    Touch(bpm, p1);                   // ts1
    page_id_t p2 = NewUnpinned(bpm);  // ts2
    Touch(bpm, p2);                   // ts3
    page_id_t p3 = NewUnpinned(bpm);  // ts4
    Touch(bpm, p3);                   // ts5
    Touch(bpm, p1);                   // ts6
    // LRU-K evicts p1 (its 2nd-most-recent access, ts1, is oldest). Plain LRU would evict p2
    // (last access ts3 is the oldest) -- so this asserts the policy really is LRU-K.
    page_id_t p4 = NewUnpinned(bpm);
    EXPECT_FALSE(bpm.IsResident(p1));
    EXPECT_TRUE(bpm.IsResident(p2));
    EXPECT_TRUE(bpm.IsResident(p3));
    EXPECT_TRUE(bpm.IsResident(p4));
}

TEST(BufferPool, LruKDiffersFromLruWhenOldPageHasHistoryAndNewOnesDont) {
    Env e("bp_lruk2");
    BufferPoolManager bpm(3, e.dm.get());
    page_id_t hot = NewUnpinned(bpm);
    Touch(bpm, hot);                        // hot: 2 accesses, oldest in recency order
    page_id_t a = NewUnpinned(bpm);         // 1 access each
    page_id_t b = NewUnpinned(bpm);
    // Plain LRU would evict `hot` (least recently used). LRU-K evicts `a`: infinite K-distance, earliest.
    NewUnpinned(bpm);
    EXPECT_TRUE(bpm.IsResident(hot));
    EXPECT_FALSE(bpm.IsResident(a));
    EXPECT_TRUE(bpm.IsResident(b));
}

TEST(BufferPool, SequentialScanDoesNotFlushHotWorkingSet) {
    Env e("bp_scan");
    BufferPoolManager bpm(4, e.dm.get());
    page_id_t h1 = NewUnpinned(bpm), h2 = NewUnpinned(bpm);
    for (int i = 0; i < 3; i++) {
        Touch(bpm, h1);
        Touch(bpm, h2);
    }
    std::vector<page_id_t> scan;
    for (int i = 0; i < 20; i++) scan.push_back(NewUnpinned(bpm));  // one-shot sequential pages
    for (page_id_t s : scan) Touch(bpm, s);
    EXPECT_TRUE(bpm.IsResident(h1));
    EXPECT_TRUE(bpm.IsResident(h2));
}

TEST(BufferPool, PinnedPageIsNeverEvicted) {
    Env e("bp_pin");
    BufferPoolManager bpm(2, e.dm.get());
    page_id_t id1, id2, id3;
    Page* p1 = bpm.NewPage(&id1);
    Page* p2 = bpm.NewPage(&id2);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(bpm.NewPage(&id3), nullptr);   // every frame pinned -> explicit failure
    EXPECT_EQ(bpm.FetchPage(999), nullptr);  // same for fetch
    bpm.UnpinPage(id2, false);
    Page* p3 = bpm.NewPage(&id3);
    ASSERT_NE(p3, nullptr);
    EXPECT_TRUE(bpm.IsResident(id1));  // pinned one stayed
    EXPECT_FALSE(bpm.IsResident(id2));
    bpm.UnpinPage(id1, false);
    bpm.UnpinPage(id3, false);
    EXPECT_EQ(bpm.PinnedCount(), 0u);
}

TEST(BufferPool, PinCountsAreCounted) {
    Env e("bp_cnt");
    BufferPoolManager bpm(2, e.dm.get());
    page_id_t id;
    Page* p = bpm.NewPage(&id);
    bpm.FetchPage(id);
    EXPECT_EQ(p->GetPinCount(), 2);
    bpm.UnpinPage(id, false);
    EXPECT_EQ(p->GetPinCount(), 1);
    bpm.UnpinPage(id, false);
    EXPECT_FALSE(bpm.UnpinPage(id, false));  // over-unpin rejected
    EXPECT_FALSE(bpm.UnpinPage(12345, false));
}

TEST(BufferPool, DirtyPageSurvivesEvictionRoundTrip) {
    Env e("bp_dirty");
    BufferPoolManager bpm(2, e.dm.get());
    page_id_t id;
    Page* p = bpm.NewPage(&id);
    std::strcpy(p->GetData() + kBody, "persist me");
    bpm.UnpinPage(id, true);
    for (int i = 0; i < 4; i++) NewUnpinned(bpm);  // force eviction
    ASSERT_FALSE(bpm.IsResident(id));
    Page* q = bpm.FetchPage(id);
    ASSERT_NE(q, nullptr);
    EXPECT_STREQ(q->GetData() + kBody, "persist me");
    bpm.UnpinPage(id, false);
}

TEST(BufferPool, DroppingPoolWithoutFlushLosesDirtyPages) {
    Env e("bp_crash");
    page_id_t id;
    {
        BufferPoolManager bpm(4, e.dm.get());
        Page* p = bpm.NewPage(&id);
        std::strcpy(p->GetData() + kBody, "never flushed");
        bpm.UnpinPage(id, true);
    }  // "crash": no FlushAll
    BufferPoolManager bpm2(4, e.dm.get());
    Page* q = bpm2.FetchPage(id);
    EXPECT_NE(std::strcmp(q->GetData() + kBody, "never flushed"), 0);
    bpm2.UnpinPage(id, false);
}

TEST(BufferPool, FlushAllMakesDataDurable) {
    Env e("bp_flush");
    page_id_t id;
    {
        BufferPoolManager bpm(4, e.dm.get());
        Page* p = bpm.NewPage(&id);
        std::strcpy(p->GetData() + kBody, "flushed");
        bpm.UnpinPage(id, true);
        bpm.FlushAll();
    }
    BufferPoolManager bpm2(4, e.dm.get());
    Page* q = bpm2.FetchPage(id);
    EXPECT_STREQ(q->GetData() + kBody, "flushed");
    bpm2.UnpinPage(id, false);
}

TEST(BufferPool, DeletePageFailsWhilePinnedAndRecyclesWhenFree) {
    Env e("bp_del");
    BufferPoolManager bpm(2, e.dm.get());
    page_id_t id;
    bpm.NewPage(&id);
    EXPECT_FALSE(bpm.DeletePage(id));
    bpm.UnpinPage(id, false);
    EXPECT_TRUE(bpm.DeletePage(id));
    page_id_t id2;
    bpm.NewPage(&id2);
    EXPECT_EQ(id2, id);  // disk free list reuse
    bpm.UnpinPage(id2, false);
}

TEST(BufferPool, WriteAheadRuleForcesLogFlushBeforePageWrite) {
    Env e("bp_wal");
    FakeLog log;
    BufferPoolManager bpm(1, e.dm.get(), &log);
    page_id_t id;
    Page* p = bpm.NewPage(&id);
    p->SetLSN(50);
    bpm.UnpinPage(id, true);
    EXPECT_TRUE(log.flush_requests.empty());
    page_id_t id2;
    ASSERT_NE(bpm.NewPage(&id2), nullptr);  // evicts page `id` -> must flush log through 50 first
    ASSERT_EQ(log.flush_requests.size(), 1u);
    EXPECT_EQ(log.flush_requests[0], 50);
    bpm.UnpinPage(id2, false);
    // A page whose LSN is already durable does not trigger a log flush.
    log.flush_requests.clear();
    Page* q = bpm.FetchPage(id);
    q->SetLSN(5);
    bpm.UnpinPage(id, true);
    bpm.FlushPage(id);
    EXPECT_TRUE(log.flush_requests.empty());
}

TEST(BufferPool, ConcurrentFetchUnpinKeepsDataIntactAndLeaksNoPins) {
    Env e("bp_mt");
    BufferPoolManager bpm(8, e.dm.get());
    constexpr int kPages = 40;
    std::vector<page_id_t> ids;
    for (int i = 0; i < kPages; i++) {
        page_id_t id;
        Page* p = bpm.NewPage(&id);
        std::memcpy(p->GetData() + kBody, &id, sizeof(id));
        bpm.UnpinPage(id, true);
        ids.push_back(id);
    }
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; t++) {
        ts.emplace_back([&, t] {
            std::mt19937 rng(t);  // seeded: deterministic access pattern per thread
            for (int i = 0; i < 3000; i++) {
                page_id_t id = ids[rng() % kPages];
                Page* p = bpm.FetchPage(id);
                if (!p) continue;  // transiently all pinned by other threads: legal
                page_id_t stamp;
                p->RLatch();
                std::memcpy(&stamp, p->GetData() + kBody, sizeof(stamp));
                p->RUnlatch();
                if (stamp != id) bad++;
                bpm.UnpinPage(id, false);
            }
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(bad.load(), 0);
    EXPECT_EQ(bpm.PinnedCount(), 0u);
}

// ---- plain LRU (the baseline policy) and pool statistics ----

TEST(LRUReplacer, EvictsLeastRecentlyAccessedAndHonoursPins) {
    LRUReplacer r;
    for (frame_id_t f : {0, 1, 2}) {
        r.RecordAccess(f);
        r.Unpin(f);
    }
    r.RecordAccess(0);  // 0 is now the most recent
    r.Pin(1);           // 1 is in use
    frame_id_t v;
    ASSERT_TRUE(r.Victim(&v));
    EXPECT_EQ(v, 2);  // oldest evictable
    ASSERT_TRUE(r.Victim(&v));
    EXPECT_EQ(v, 0);
    EXPECT_FALSE(r.Victim(&v));  // only the pinned frame is left
    r.Unpin(1);
    ASSERT_TRUE(r.Victim(&v));
    EXPECT_EQ(v, 1);
}

TEST(BufferPool, PlainLruLosesTheHotSetToASequentialScanButLruKKeepsIt) {
    for (bool lru_k : {false, true}) {
        Env e(lru_k ? "bp_scan_lruk" : "bp_scan_lru");
        std::unique_ptr<Replacer> rep;
        if (lru_k) rep = std::make_unique<LRUKReplacer>(2);
        else rep = std::make_unique<LRUReplacer>();
        BufferPoolManager bpm(4, e.dm.get(), nullptr, std::move(rep));
        page_id_t h1 = NewUnpinned(bpm), h2 = NewUnpinned(bpm);
        for (int i = 0; i < 3; i++) {
            Touch(bpm, h1);
            Touch(bpm, h2);
        }
        for (int i = 0; i < 8; i++) NewUnpinned(bpm);  // a one-shot scan of pages never seen again
        EXPECT_EQ(bpm.IsResident(h1), lru_k) << (lru_k ? "LRU-K" : "LRU");
        EXPECT_EQ(bpm.IsResident(h2), lru_k) << (lru_k ? "LRU-K" : "LRU");
    }
}

TEST(BufferPool, StatsCountHitsMissesAndEvictions) {
    Env e("bp_stats");
    BufferPoolManager bpm(2, e.dm.get());
    page_id_t a = NewUnpinned(bpm), b = NewUnpinned(bpm), c = NewUnpinned(bpm);  // c evicts one of a/b
    EXPECT_EQ(bpm.Stats().evictions, 1u);
    bpm.ResetStats();
    Touch(bpm, c);  // resident
    Touch(bpm, c);
    EXPECT_EQ(bpm.Stats().hits, 2u);
    EXPECT_EQ(bpm.Stats().misses, 0u);
    page_id_t gone = bpm.IsResident(a) ? b : a;
    Touch(bpm, gone);  // miss -> evicts something
    EXPECT_EQ(bpm.Stats().misses, 1u);
    EXPECT_EQ(bpm.Stats().evictions, 1u);
    EXPECT_NEAR(bpm.Stats().HitRate(), 2.0 / 3.0, 1e-9);
}
