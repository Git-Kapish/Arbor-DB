#include <gtest/gtest.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <string>

#include "storage/disk/disk_manager.h"

using namespace arbordb;

namespace {
std::string TmpPath(const char* name) {
    std::string p = "/tmp/arbor_test_" + std::string(name) + "_" + std::to_string(getpid());
    unlink(p.c_str());
    return p;
}
void Fill(char* buf, char c) { std::memset(buf, c, kPageSize); }
}  // namespace

TEST(DiskManager, ReadBackByteIdenticalAcrossReopen) {
    auto path = TmpPath("dm_reopen");
    char out[kPageSize], in[kPageSize];
    for (std::size_t i = 0; i < kPageSize; i++) out[i] = static_cast<char>(i * 31 + 7);
    page_id_t id;
    {
        DiskManager dm(path);
        id = dm.AllocatePage();
        dm.WritePage(id, out);
        dm.Sync();
    }
    DiskManager dm2(path);  // "process restart"
    dm2.ReadPage(id, in);
    EXPECT_EQ(0, std::memcmp(out, in, kPageSize));
    EXPECT_GT(dm2.NumPages(), id);
    unlink(path.c_str());
}

TEST(DiskManager, AllocateGivesDistinctNonZeroIds) {
    auto path = TmpPath("dm_alloc");
    DiskManager dm(path);
    page_id_t a = dm.AllocatePage(), b = dm.AllocatePage();
    EXPECT_NE(a, b);
    EXPECT_GE(a, 1);  // page 0 is metadata
    EXPECT_GE(b, 1);
    unlink(path.c_str());
}

TEST(DiskManager, ReadPastEofIsZeros) {
    auto path = TmpPath("dm_eof");
    DiskManager dm(path);
    page_id_t id = dm.AllocatePage();  // allocated, never written
    char in[kPageSize];
    Fill(in, 0x55);
    dm.ReadPage(id, in);
    for (std::size_t i = 0; i < kPageSize; i++) ASSERT_EQ(in[i], 0);
    unlink(path.c_str());
}

TEST(DiskManager, FreedPagesAreReusedLifo) {
    auto path = TmpPath("dm_free");
    DiskManager dm(path);
    page_id_t a = dm.AllocatePage(), b = dm.AllocatePage(), c = dm.AllocatePage();
    dm.DeallocatePage(a);
    dm.DeallocatePage(b);
    EXPECT_EQ(dm.AllocatePage(), b);
    EXPECT_EQ(dm.AllocatePage(), a);
    page_id_t d = dm.AllocatePage();  // free list empty -> fresh page
    EXPECT_GT(d, c);
    unlink(path.c_str());
}

TEST(DiskManager, FreeListSurvivesRestart) {
    auto path = TmpPath("dm_free_restart");
    page_id_t a;
    {
        DiskManager dm(path);
        a = dm.AllocatePage();
        dm.AllocatePage();
        dm.DeallocatePage(a);
    }
    DiskManager dm(path);
    EXPECT_EQ(dm.AllocatePage(), a);
    unlink(path.c_str());
}

TEST(DiskManager, EnsureAllocatedBumpsCounter) {
    auto path = TmpPath("dm_ensure");
    DiskManager dm(path);
    dm.EnsureAllocated(10);
    EXPECT_EQ(dm.AllocatePage(), 11);
    unlink(path.c_str());
}
