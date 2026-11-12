#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "recovery/log_record.h"

namespace arbordb {

// Append-only write-ahead log in one file. LSNs are dense sequence numbers starting at 1 (record i is
// the i-th record in the file), so lsn -> file offset is an array lookup built when the file is opened.
//
// Durability: Append() only buffers. Flush() writes the buffer and calls fsync -- this is the ONE place
// the log is made durable, and everything the write-ahead rule and commit protocol rely on goes through
// it (BufferPoolManager before writing a page; TransactionManager::Commit).
// The destructor does NOT flush (dropping a LogManager models a crash).
//
// On open, a torn final record (partial write) is detected by its length and truncated away.
// The log is never truncated/archived in normal operation (see README Known Gaps).
class LogManager : public LogFlusher {
public:
    explicit LogManager(const std::string& path);
    ~LogManager() override;

    // Assigns r->lsn, buffers the record, returns the LSN.
    lsn_t Append(LogRecord* r);
    void Flush();
    lsn_t FlushedLsn() override;
    void FlushTo(lsn_t lsn) override;

    lsn_t LastLsn();  // 0 if empty
    bool ReadRecord(lsn_t lsn, LogRecord* out);
    // All records with lsn >= from, in order.
    std::vector<LogRecord> ScanFrom(lsn_t from);

    // The checkpoint master record: LSN of the last completed CHECKPOINT (0 if none).
    lsn_t CheckpointLsn() const { return checkpoint_lsn_; }
    void SetCheckpointLsn(lsn_t lsn);

private:
    void WriteBufferLocked();
    bool ReadAtLocked(uint64_t off, LogRecord* out);

    std::string path_;
    int fd_;
    std::mutex mu_;
    std::vector<uint64_t> offsets_;  // offsets_[lsn-1] = byte offset of that record
    uint64_t file_end_{0};           // bytes already written to the file
    std::string buf_;                // appended but not yet written
    lsn_t flushed_lsn_{0};
    lsn_t checkpoint_lsn_{0};
};

}  // namespace arbordb
