#include "storage/page/slotted_page.h"

#include <vector>

namespace arbordb {

void SlottedPage::Init(page_id_t page_id) {
    std::memset(d_, 0, kPageSize);
    std::memcpy(d_ + kOffsetPageId, &page_id, sizeof(page_id));
    Write16(kOffFreePtr, static_cast<uint16_t>(kPageSize));  // 4096 fits in u16
    Write16(kOffSlotCount, 0);
    SetNextPageId(kInvalidPageId);
}

page_id_t SlottedPage::NextPageId() const {
    page_id_t id;
    std::memcpy(&id, d_ + kOffNext, sizeof(id));
    return id;
}

void SlottedPage::SetNextPageId(page_id_t id) { std::memcpy(d_ + kOffNext, &id, sizeof(id)); }

std::size_t SlottedPage::FreeSpace() const {
    std::size_t slots_end = kHeaderSize + SlotCount() * kSlotSize;
    return FreePtr() - slots_end;
}

std::size_t SlottedPage::TotalFreeSpace() const {
    std::size_t used = kHeaderSize + SlotCount() * kSlotSize;
    for (uint16_t i = 0; i < SlotCount(); i++) {
        Slot s = GetSlot(i);
        if (!(s.flags & kFlagDeleted)) used += s.size;  // Compact drops tombstone bytes and slack
    }
    return kPageSize - used;
}

std::optional<uint16_t> SlottedPage::Insert(const char* data, uint16_t len) {
    if (len == 0 || !CanInsert(len)) return std::nullopt;
    uint16_t slot = SlotCount();
    uint16_t off = static_cast<uint16_t>(FreePtr() - len);
    std::memcpy(d_ + off, data, len);
    SetSlot(slot, Slot{off, len, len, 0});
    Write16(kOffFreePtr, off);
    Write16(kOffSlotCount, slot + 1);
    return slot;
}

std::optional<std::pair<const char*, uint16_t>> SlottedPage::Get(uint16_t slot) const {
    if (slot >= SlotCount()) return std::nullopt;
    Slot s = GetSlot(slot);
    if (s.flags & kFlagDeleted) return std::nullopt;
    return std::make_pair(static_cast<const char*>(d_ + s.offset), s.size);
}

bool SlottedPage::IsDeleted(uint16_t slot) const {
    return slot < SlotCount() && (GetSlot(slot).flags & kFlagDeleted);
}

bool SlottedPage::MarkDeleted(uint16_t slot) {
    if (slot >= SlotCount()) return false;
    Slot s = GetSlot(slot);
    if (s.flags & kFlagDeleted) return false;
    s.flags |= kFlagDeleted;
    SetSlot(slot, s);
    return true;
}

bool SlottedPage::Undelete(uint16_t slot) {
    if (slot >= SlotCount()) return false;
    Slot s = GetSlot(slot);
    if (!(s.flags & kFlagDeleted) || s.capacity == 0) return false;  // capacity 0: compacted away
    s.flags &= ~kFlagDeleted;
    SetSlot(slot, s);
    return true;
}

bool SlottedPage::Update(uint16_t slot, const char* data, uint16_t len) {
    if (len == 0 || slot >= SlotCount()) return false;
    Slot s = GetSlot(slot);
    if (s.flags & kFlagDeleted) return false;
    if (len <= s.capacity) {
        std::memcpy(d_ + s.offset, data, len);
        s.size = len;
        SetSlot(slot, s);
        return true;
    }
    // Relocate: needs contiguous room for the new copy (the old bytes become slack until Compact).
    if (FreeSpace() < len) return false;
    uint16_t off = static_cast<uint16_t>(FreePtr() - len);
    std::memcpy(d_ + off, data, len);
    s.offset = off;
    s.size = len;
    s.capacity = len;
    SetSlot(slot, s);
    Write16(kOffFreePtr, off);
    return true;
}

bool SlottedPage::CanUpdate(uint16_t slot, uint16_t len) const {
    if (len == 0 || slot >= SlotCount()) return false;
    Slot s = GetSlot(slot);
    if (s.flags & kFlagDeleted) return false;
    return len <= s.capacity || FreeSpace() >= len;
}

void SlottedPage::Compact() {
    std::vector<char> copy(d_, d_ + kPageSize);
    uint16_t ptr = static_cast<uint16_t>(kPageSize);
    for (uint16_t i = 0; i < SlotCount(); i++) {
        Slot s = GetSlot(i);
        if (s.flags & kFlagDeleted) {
            s.offset = 0;
            s.size = 0;
            s.capacity = 0;
        } else {
            ptr = static_cast<uint16_t>(ptr - s.size);
            std::memcpy(d_ + ptr, copy.data() + s.offset, s.size);
            s.offset = ptr;
            s.capacity = s.size;
        }
        SetSlot(i, s);
    }
    Write16(kOffFreePtr, ptr);
}

}  // namespace arbordb
