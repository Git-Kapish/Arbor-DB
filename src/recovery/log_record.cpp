#include "recovery/log_record.h"

#include <cstring>
#include <stdexcept>

#include "storage/page/slotted_page.h"

namespace arbordb {

PageOp PageOp::Inverse() const {
    PageOp inv = *this;
    switch (kind) {
        case PageOpKind::kInsert: inv.kind = PageOpKind::kDelete; break;
        case PageOpKind::kDelete: inv.kind = PageOpKind::kUndelete; break;
        case PageOpKind::kUndelete: inv.kind = PageOpKind::kDelete; break;
        case PageOpKind::kUpdate: std::swap(inv.before, inv.after); break;
        default: throw std::logic_error("operation has no inverse");
    }
    return inv;
}

void ApplyPageOp(Page* page, const PageOp& op) {
    SlottedPage sp(page);
    bool ok = true;
    switch (op.kind) {
        case PageOpKind::kInitPage: sp.Init(op.page_id); break;
        case PageOpKind::kSetNext: sp.SetNextPageId(op.next_page); break;
        case PageOpKind::kInsert: {
            auto slot = sp.Insert(op.after.data(), static_cast<uint16_t>(op.after.size()));
            ok = slot.has_value() && *slot == op.slot;
            break;
        }
        case PageOpKind::kDelete: ok = sp.MarkDeleted(op.slot); break;
        case PageOpKind::kUndelete: ok = sp.Undelete(op.slot); break;
        case PageOpKind::kUpdate: ok = sp.Update(op.slot, op.after.data(), static_cast<uint16_t>(op.after.size())); break;
    }
    if (!ok) throw std::runtime_error("page op does not apply: page " + std::to_string(op.page_id) + " slot " + std::to_string(op.slot));
}

namespace {
template <typename T>
void Put(std::string* s, T v) {
    s->append(reinterpret_cast<const char*>(&v), sizeof(T));
}
void PutBytes(std::string* s, const std::string& b) {
    Put<uint32_t>(s, static_cast<uint32_t>(b.size()));
    s->append(b);
}
struct Reader {
    const char* p;
    const char* end;
    bool ok{true};
    template <typename T>
    T Get() {
        T v{};
        if (static_cast<std::size_t>(end - p) < sizeof(T)) { ok = false; return v; }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    std::string GetBytes() {
        uint32_t n = Get<uint32_t>();
        if (!ok || static_cast<std::size_t>(end - p) < n) { ok = false; return {}; }
        std::string s(p, n);
        p += n;
        return s;
    }
};
void PutOp(std::string* s, const PageOp& op) {
    Put<uint8_t>(s, static_cast<uint8_t>(op.kind));
    Put<page_id_t>(s, op.page_id);
    Put<uint16_t>(s, op.slot);
    Put<page_id_t>(s, op.next_page);
    PutBytes(s, op.before);
    PutBytes(s, op.after);
}
PageOp GetOp(Reader& r) {
    PageOp op;
    op.kind = static_cast<PageOpKind>(r.Get<uint8_t>());
    op.page_id = r.Get<page_id_t>();
    op.slot = r.Get<uint16_t>();
    op.next_page = r.Get<page_id_t>();
    op.before = r.GetBytes();
    op.after = r.GetBytes();
    return op;
}
}  // namespace

// Wire format: [u32 total_size][u8 type][i64 lsn][i64 prev_lsn][i32 txn_id][type-specific payload]
void SerializeLogRecord(const LogRecord& r, std::string* out) {
    std::string body;
    Put<uint8_t>(&body, static_cast<uint8_t>(r.type));
    Put<lsn_t>(&body, r.lsn);
    Put<lsn_t>(&body, r.prev_lsn);
    Put<txn_id_t>(&body, r.txn_id);
    switch (r.type) {
        case LogType::kUpdate: PutOp(&body, r.op); break;
        case LogType::kClr:
            PutOp(&body, r.op);
            Put<lsn_t>(&body, r.undo_next_lsn);
            break;
        case LogType::kCheckpoint:
            Put<lsn_t>(&body, r.redo_start_lsn);
            Put<uint32_t>(&body, static_cast<uint32_t>(r.active_txns.size()));
            for (auto& [t, l] : r.active_txns) {
                Put<txn_id_t>(&body, t);
                Put<lsn_t>(&body, l);
            }
            break;
        default: break;
    }
    Put<uint32_t>(out, static_cast<uint32_t>(body.size() + sizeof(uint32_t)));
    out->append(body);
}

bool DeserializeLogRecord(const char* buf, std::size_t len, LogRecord* out) {
    Reader r{buf, buf + len};
    uint32_t total = r.Get<uint32_t>();
    if (!r.ok || total > len || total < sizeof(uint32_t) + 21) return false;
    r.end = buf + total;
    LogRecord rec;
    rec.type = static_cast<LogType>(r.Get<uint8_t>());
    rec.lsn = r.Get<lsn_t>();
    rec.prev_lsn = r.Get<lsn_t>();
    rec.txn_id = r.Get<txn_id_t>();
    switch (rec.type) {
        case LogType::kBegin: case LogType::kCommit: case LogType::kAbort: case LogType::kEnd: break;
        case LogType::kUpdate: rec.op = GetOp(r); break;
        case LogType::kClr:
            rec.op = GetOp(r);
            rec.undo_next_lsn = r.Get<lsn_t>();
            break;
        case LogType::kCheckpoint: {
            rec.redo_start_lsn = r.Get<lsn_t>();
            uint32_t n = r.Get<uint32_t>();
            for (uint32_t i = 0; i < n && r.ok; i++) {
                txn_id_t t = r.Get<txn_id_t>();
                lsn_t l = r.Get<lsn_t>();
                rec.active_txns.push_back({t, l});
            }
            break;
        }
        default: return false;
    }
    if (!r.ok) return false;
    *out = std::move(rec);
    return true;
}

}  // namespace arbordb
