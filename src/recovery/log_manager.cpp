#include "recovery/log_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace arbordb {

namespace {
constexpr std::size_t kBufferLimit = 64 * 1024;  // write (not fsync) once this much is pending
}

LogManager::LogManager(const std::string& path) : path_(path) {
    fd_ = open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) throw std::runtime_error("cannot open log " + path);
    struct stat st;
    fstat(fd_, &st);
    std::string data(st.st_size, '\0');
    std::size_t got = 0;
    while (got < data.size()) {
        ssize_t r = pread(fd_, &data[got], data.size() - got, got);
        if (r <= 0) break;
        got += r;
    }
    // Walk the file record by record; stop at the first incomplete or malformed one.
    uint64_t off = 0;
    while (off + sizeof(uint32_t) <= got) {
        LogRecord r;
        if (!DeserializeLogRecord(data.data() + off, got - off, &r)) break;
        if (r.lsn != static_cast<lsn_t>(offsets_.size()) + 1) break;
        offsets_.push_back(off);
        uint32_t total;
        std::memcpy(&total, data.data() + off, 4);
        off += total;
    }
    if (off < got) {  // torn tail from a crash mid-write
        if (ftruncate(fd_, off) != 0) throw std::runtime_error("cannot truncate torn log tail");
    }
    file_end_ = off;
    flushed_lsn_ = static_cast<lsn_t>(offsets_.size());
    std::ifstream m(path + ".master");
    lsn_t c = 0;
    if (m >> c && c <= flushed_lsn_) checkpoint_lsn_ = c;
}

LogManager::~LogManager() {
    if (fd_ >= 0) close(fd_);
}

lsn_t LogManager::Append(LogRecord* r) {
    std::lock_guard<std::mutex> g(mu_);
    r->lsn = static_cast<lsn_t>(offsets_.size()) + 1;
    offsets_.push_back(file_end_ + buf_.size());
    SerializeLogRecord(*r, &buf_);
    if (buf_.size() >= kBufferLimit) WriteBufferLocked();
    return r->lsn;
}

void LogManager::WriteBufferLocked() {
    std::size_t done = 0;
    while (done < buf_.size()) {
        ssize_t w = pwrite(fd_, buf_.data() + done, buf_.size() - done, file_end_ + done);
        if (w < 0) throw std::runtime_error("log write failed");
        done += w;
    }
    file_end_ += buf_.size();
    buf_.clear();
}

void LogManager::Flush() {
    std::lock_guard<std::mutex> g(mu_);
    WriteBufferLocked();
    fsync(fd_);  // the log's durability point
    flushed_lsn_ = static_cast<lsn_t>(offsets_.size());
}

lsn_t LogManager::FlushedLsn() {
    std::lock_guard<std::mutex> g(mu_);
    return flushed_lsn_;
}

void LogManager::FlushTo(lsn_t lsn) {
    if (FlushedLsn() < lsn) Flush();
}

lsn_t LogManager::LastLsn() {
    std::lock_guard<std::mutex> g(mu_);
    return static_cast<lsn_t>(offsets_.size());
}

bool LogManager::ReadAtLocked(uint64_t off, LogRecord* out) {
    if (off >= file_end_) {  // still in the buffer
        uint64_t rel = off - file_end_;
        return DeserializeLogRecord(buf_.data() + rel, buf_.size() - rel, out);
    }
    uint32_t total;
    if (pread(fd_, &total, 4, off) != 4) return false;
    std::string b(total, '\0');
    if (pread(fd_, &b[0], total, off) != static_cast<ssize_t>(total)) return false;
    return DeserializeLogRecord(b.data(), b.size(), out);
}

bool LogManager::ReadRecord(lsn_t lsn, LogRecord* out) {
    std::lock_guard<std::mutex> g(mu_);
    if (lsn < 1 || lsn > static_cast<lsn_t>(offsets_.size())) return false;
    return ReadAtLocked(offsets_[lsn - 1], out);
}

std::vector<LogRecord> LogManager::ScanFrom(lsn_t from) {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<LogRecord> v;
    for (lsn_t l = std::max<lsn_t>(from, 1); l <= static_cast<lsn_t>(offsets_.size()); l++) {
        LogRecord r;
        if (!ReadAtLocked(offsets_[l - 1], &r)) throw std::runtime_error("corrupt log record " + std::to_string(l));
        v.push_back(std::move(r));
    }
    return v;
}

void LogManager::SetCheckpointLsn(lsn_t lsn) {
    std::lock_guard<std::mutex> g(mu_);
    std::string tmp = path_ + ".master.tmp";
    {
        std::ofstream o(tmp, std::ios::trunc);
        o << lsn << "\n";
    }
    std::rename(tmp.c_str(), (path_ + ".master").c_str());  // atomic replace
    checkpoint_lsn_ = lsn;
}

}  // namespace arbordb
