#pragma once

#include <cstddef>
#include <cstdint>

namespace arbordb {

using page_id_t = int32_t;
using frame_id_t = int32_t;
using txn_id_t = int32_t;
using lsn_t = int64_t;

constexpr page_id_t kInvalidPageId = -1;
constexpr txn_id_t kInvalidTxnId = -1;
constexpr lsn_t kInvalidLsn = -1;

constexpr std::size_t kPageSize = 4096;  // configurable at build time later
constexpr std::size_t kLRUKValue = 2;

// Record ID: page + slot within that page.
struct RID {
    page_id_t page_id{kInvalidPageId};
    uint32_t slot{0};

    bool operator==(const RID& o) const { return page_id == o.page_id && slot == o.slot; }
    bool operator<(const RID& o) const { return page_id != o.page_id ? page_id < o.page_id : slot < o.slot; }
};

}  // namespace arbordb
