#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include "storage/page/page.h"

namespace arbordb {

// Slotted-page view over a Page's data (non-owning).
//
// Layout: [common header 16][free_ptr u16][slot_count u16][next_page_id i32][slots...  ->   <- tuple bytes]
// The slot array grows from the front, tuple bytes grow down from the back.
//
// Design notes (these matter for recovery/undo, see recovery/):
//  * Slots are NEVER reused: Insert always appends, so a RID identifies one tuple forever.
//  * Delete only sets a tombstone flag; the bytes stay, so an aborted delete can be undone
//    (Undelete) without needing free space.
//  * Update is in place when the new length fits the slot's capacity, so undoing a shrinking
//    update always fits. Otherwise the tuple is relocated and the capacity grows.
//  * Compact() physically reclaims tombstoned bytes and capacity slack. It is explicit and the
//    table heap never calls it automatically, because it would destroy undo information.
class SlottedPage {
public:
    static constexpr std::size_t kHeaderSize = kCommonHeaderSize + 8;
    static constexpr std::size_t kSlotSize = 8;

    explicit SlottedPage(char* data) : d_(data) {}
    explicit SlottedPage(Page* page) : d_(page->GetData()) {}

    void Init(page_id_t page_id);

    uint16_t SlotCount() const { return Read16(kOffSlotCount); }
    page_id_t NextPageId() const;
    void SetNextPageId(page_id_t id);

    // Contiguous free bytes between slot array and tuple area (before accounting for a new slot).
    std::size_t FreeSpace() const;
    // Free bytes if the page were compacted.
    std::size_t TotalFreeSpace() const;
    bool CanInsert(uint16_t len) const { return FreeSpace() >= len + kSlotSize; }

    // Appends a tuple; returns its slot, or nullopt when the page is full.
    std::optional<uint16_t> Insert(const char* data, uint16_t len);
    // Pointer/length of a live tuple; nullopt if slot is out of range or deleted.
    std::optional<std::pair<const char*, uint16_t>> Get(uint16_t slot) const;
    bool IsDeleted(uint16_t slot) const;
    bool MarkDeleted(uint16_t slot);
    bool Undelete(uint16_t slot);
    bool Update(uint16_t slot, const char* data, uint16_t len);
    // Would Update(slot, _, len) succeed? (live slot, and fits in place or in contiguous free space)
    bool CanUpdate(uint16_t slot, uint16_t len) const;
    // Physically reclaim space. Tombstoned slots lose their bytes permanently.
    void Compact();

private:
    static constexpr std::size_t kOffFreePtr = kCommonHeaderSize;
    static constexpr std::size_t kOffSlotCount = kCommonHeaderSize + 2;
    static constexpr std::size_t kOffNext = kCommonHeaderSize + 4;
    static constexpr uint16_t kFlagDeleted = 1;

    struct Slot {
        uint16_t offset, size, capacity, flags;
    };

    uint16_t Read16(std::size_t off) const {
        uint16_t v;
        std::memcpy(&v, d_ + off, 2);
        return v;
    }
    void Write16(std::size_t off, uint16_t v) { std::memcpy(d_ + off, &v, 2); }
    Slot GetSlot(uint16_t i) const {
        Slot s;
        std::memcpy(&s, d_ + kHeaderSize + i * kSlotSize, sizeof(s));
        return s;
    }
    void SetSlot(uint16_t i, const Slot& s) { std::memcpy(d_ + kHeaderSize + i * kSlotSize, &s, sizeof(s)); }
    uint16_t FreePtr() const { return Read16(kOffFreePtr); }

    char* d_;
};

}  // namespace arbordb
