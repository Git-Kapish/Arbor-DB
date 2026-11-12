#include "storage/disk/disk_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace arbordb {

namespace {
constexpr uint32_t kMagic = 0x41524244;  // "ARBD"
struct Meta {
    uint32_t magic;
    page_id_t num_pages;
    page_id_t free_head;
};

void PReadFull(int fd, char* buf, size_t n, off_t off) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, buf + got, n - got, off + got);
        if (r < 0) throw std::runtime_error("pread failed");
        if (r == 0) break;  // EOF
        got += r;
    }
    if (got < n) std::memset(buf + got, 0, n - got);
}

void PWriteFull(int fd, const char* buf, size_t n, off_t off) {
    size_t done = 0;
    while (done < n) {
        ssize_t w = pwrite(fd, buf + done, n - done, off + done);
        if (w < 0) throw std::runtime_error("pwrite failed");
        done += w;
    }
}
}  // namespace

DiskManager::DiskManager(const std::string& path) {
    fd_ = open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) throw std::runtime_error("cannot open " + path);
    struct stat st;
    fstat(fd_, &st);
    Meta m{};
    if (st.st_size >= static_cast<off_t>(sizeof(Meta))) {
        PReadFull(fd_, reinterpret_cast<char*>(&m), sizeof(m), 0);
    }
    if (m.magic == kMagic) {
        num_pages_ = m.num_pages;
        free_head_ = m.free_head;
        // The file may be longer than the (non-fsynced) metadata says: trust the file.
        num_pages_ = std::max<page_id_t>(num_pages_, static_cast<page_id_t>((st.st_size + kPageSize - 1) / kPageSize));
    } else {
        num_pages_ = 1;
        free_head_ = kInvalidPageId;
    }
    WriteMeta();
}

DiskManager::~DiskManager() {
    if (fd_ >= 0) close(fd_);
}

void DiskManager::WriteMeta() {
    char buf[kPageSize] = {};
    Meta m{kMagic, num_pages_, free_head_};
    std::memcpy(buf, &m, sizeof(m));
    PWriteFull(fd_, buf, kPageSize, 0);
}

void DiskManager::ReadPage(page_id_t id, char* out) {
    PReadFull(fd_, out, kPageSize, static_cast<off_t>(id) * kPageSize);
}

void DiskManager::WritePage(page_id_t id, const char* in) {
    PWriteFull(fd_, in, kPageSize, static_cast<off_t>(id) * kPageSize);
}

page_id_t DiskManager::AllocatePage() {
    std::lock_guard<std::mutex> g(mu_);
    page_id_t id;
    if (free_head_ != kInvalidPageId) {
        id = free_head_;
        char buf[kPageSize];
        PReadFull(fd_, buf, kPageSize, static_cast<off_t>(id) * kPageSize);
        std::memcpy(&free_head_, buf, sizeof(free_head_));
    } else {
        id = num_pages_++;
    }
    WriteMeta();
    return id;
}

void DiskManager::DeallocatePage(page_id_t id) {
    std::lock_guard<std::mutex> g(mu_);
    char buf[kPageSize] = {};
    std::memcpy(buf, &free_head_, sizeof(free_head_));
    PWriteFull(fd_, buf, kPageSize, static_cast<off_t>(id) * kPageSize);
    free_head_ = id;
    WriteMeta();
}

void DiskManager::EnsureAllocated(page_id_t id) {
    std::lock_guard<std::mutex> g(mu_);
    if (id >= num_pages_) {
        num_pages_ = id + 1;
        WriteMeta();
    }
}

void DiskManager::Sync() { fsync(fd_); }

page_id_t DiskManager::NumPages() const {
    std::lock_guard<std::mutex> g(mu_);
    return num_pages_;
}

}  // namespace arbordb
