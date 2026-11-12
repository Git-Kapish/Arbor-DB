#pragma once

#include <mutex>
#include <string>

#include "common/config.h"

namespace arbordb {

// Raw page I/O over one file using pread/pwrite. All POSIX-specific code lives here.
//
// File layout: page 0 is a metadata page {magic, num_pages, free_list_head}; user pages start
// at id 1. Free pages form a linked list: the first 4 bytes of a free page hold the next free id.
// The metadata page is rewritten (not fsynced) on every allocate/deallocate; fsync happens in Sync().
// Page allocation is NOT write-ahead logged (see README Known Gaps).
class DiskManager {
public:
    explicit DiskManager(const std::string& path);
    ~DiskManager();
    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    // Reading a page past EOF yields zeros (a page allocated but never written).
    void ReadPage(page_id_t id, char* out);
    void WritePage(page_id_t id, const char* in);

    page_id_t AllocatePage();
    void DeallocatePage(page_id_t id);
    // Recovery: make sure id is considered allocated (file may be shorter than the log implies).
    void EnsureAllocated(page_id_t id);

    void Sync();  // fsync the data file
    page_id_t NumPages() const;  // highest allocated id + 1

private:
    void WriteMeta();

    int fd_;
    mutable std::mutex mu_;
    page_id_t num_pages_;
    page_id_t free_head_;
};

}  // namespace arbordb
