#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "storage/page/slotted_page.h"

using namespace arbordb;

namespace {
struct Fixture {
    Page page;
    SlottedPage sp{&page};
    Fixture() { sp.Init(7); }
};
std::string Str(const std::pair<const char*, uint16_t>& p) { return std::string(p.first, p.second); }
uint16_t Ins(SlottedPage& sp, const std::string& s) {
    auto r = sp.Insert(s.data(), static_cast<uint16_t>(s.size()));
    EXPECT_TRUE(r.has_value());
    return *r;
}
}  // namespace

TEST(SlottedPage, InsertAndGetVariableLength) {
    Fixture f;
    uint16_t a = Ins(f.sp, "x"), b = Ins(f.sp, "hello world"), c = Ins(f.sp, std::string(300, 'z'));
    EXPECT_EQ(Str(*f.sp.Get(a)), "x");
    EXPECT_EQ(Str(*f.sp.Get(b)), "hello world");
    EXPECT_EQ(Str(*f.sp.Get(c)), std::string(300, 'z'));
    EXPECT_EQ(f.sp.SlotCount(), 3);
}

TEST(SlottedPage, DeleteMakesTupleInvisibleAndSlotsAreNotReused) {
    Fixture f;
    uint16_t a = Ins(f.sp, "aaa"), b = Ins(f.sp, "bbb");
    EXPECT_TRUE(f.sp.MarkDeleted(a));
    EXPECT_FALSE(f.sp.Get(a).has_value());
    EXPECT_TRUE(f.sp.IsDeleted(a));
    EXPECT_FALSE(f.sp.MarkDeleted(a));  // double delete rejected
    EXPECT_EQ(Str(*f.sp.Get(b)), "bbb");
    uint16_t c = Ins(f.sp, "ccc");
    EXPECT_EQ(c, 2);  // fresh slot, not a's
}

TEST(SlottedPage, UndeleteRestoresTuple) {
    Fixture f;
    uint16_t a = Ins(f.sp, "keepme");
    f.sp.MarkDeleted(a);
    EXPECT_TRUE(f.sp.Undelete(a));
    EXPECT_EQ(Str(*f.sp.Get(a)), "keepme");
}

TEST(SlottedPage, PageFullEdgeCase) {
    Fixture f;
    std::string tuple(100, 'q');
    int n = 0;
    while (f.sp.CanInsert(100)) {
        ASSERT_TRUE(f.sp.Insert(tuple.data(), 100).has_value());
        n++;
    }
    EXPECT_GT(n, 30);
    EXPECT_FALSE(f.sp.Insert(tuple.data(), 100).has_value());  // full: refuses, no corruption
    // A smaller tuple may still fit in the leftover bytes; fill them exactly.
    while (f.sp.FreeSpace() >= 1 + SlottedPage::kSlotSize) {
        ASSERT_TRUE(f.sp.Insert("k", 1).has_value());
    }
    EXPECT_FALSE(f.sp.Insert("k", 1).has_value());
    for (uint16_t i = 0; i < static_cast<uint16_t>(n); i++) ASSERT_EQ(Str(*f.sp.Get(i)), tuple);
}

TEST(SlottedPage, ZeroLengthInsertRejected) {
    Fixture f;
    EXPECT_FALSE(f.sp.Insert("", 0).has_value());
}

TEST(SlottedPage, CompactReclaimsDeletedSpaceAndKeepsLiveTuples) {
    Fixture f;
    std::string big(1000, 'b');
    uint16_t a = Ins(f.sp, big), b = Ins(f.sp, "live-one"), c = Ins(f.sp, big), d = Ins(f.sp, "live-two");
    f.sp.MarkDeleted(a);
    f.sp.MarkDeleted(c);
    std::size_t before = f.sp.FreeSpace();
    EXPECT_EQ(f.sp.TotalFreeSpace(), before + 2000);
    f.sp.Compact();
    EXPECT_EQ(f.sp.FreeSpace(), before + 2000);
    EXPECT_EQ(Str(*f.sp.Get(b)), "live-one");
    EXPECT_EQ(Str(*f.sp.Get(d)), "live-two");
    EXPECT_FALSE(f.sp.Get(a).has_value());
    EXPECT_FALSE(f.sp.Undelete(a));  // bytes are gone after compaction
    EXPECT_TRUE(f.sp.Insert(big.data(), 1000).has_value());  // space is usable again
}

TEST(SlottedPage, CompactMakesRoomWhenPageWasFull) {
    Fixture f;
    std::string t(500, 'x');
    std::vector<uint16_t> slots;
    while (f.sp.CanInsert(500)) slots.push_back(Ins(f.sp, t));
    EXPECT_FALSE(f.sp.Insert(t.data(), 500).has_value());
    f.sp.MarkDeleted(slots[0]);
    EXPECT_FALSE(f.sp.Insert(t.data(), 500).has_value());  // delete alone doesn't free contiguous space
    f.sp.Compact();
    EXPECT_TRUE(f.sp.Insert(t.data(), 500).has_value());
}

TEST(SlottedPage, UpdateInPlaceShrinkThenGrowBackWithinCapacity) {
    Fixture f;
    uint16_t a = Ins(f.sp, "0123456789");
    EXPECT_TRUE(f.sp.Update(a, "abc", 3));
    EXPECT_EQ(Str(*f.sp.Get(a)), "abc");
    EXPECT_TRUE(f.sp.Update(a, "0123456789", 10));  // undo of the shrink always fits
    EXPECT_EQ(Str(*f.sp.Get(a)), "0123456789");
}

TEST(SlottedPage, UpdateGrowthRelocates) {
    Fixture f;
    uint16_t a = Ins(f.sp, "ab"), b = Ins(f.sp, "neighbour");
    std::string longer(50, 'L');
    EXPECT_TRUE(f.sp.Update(a, longer.data(), 50));
    EXPECT_EQ(Str(*f.sp.Get(a)), longer);
    EXPECT_EQ(Str(*f.sp.Get(b)), "neighbour");
}

TEST(SlottedPage, UpdateGrowthFailsWhenNoRoom) {
    Fixture f;
    uint16_t a = Ins(f.sp, "ab");
    while (f.sp.CanInsert(200)) Ins(f.sp, std::string(200, 'f'));
    std::string longer(500, 'L');
    EXPECT_FALSE(f.sp.Update(a, longer.data(), 500));
    EXPECT_EQ(Str(*f.sp.Get(a)), "ab");  // untouched
}

TEST(SlottedPage, NextPagePointerAndHeader) {
    Fixture f;
    EXPECT_EQ(f.sp.NextPageId(), kInvalidPageId);
    f.sp.SetNextPageId(42);
    EXPECT_EQ(f.sp.NextPageId(), 42);
    f.page.SetLSN(123);
    EXPECT_EQ(f.page.GetLSN(), 123);
}
