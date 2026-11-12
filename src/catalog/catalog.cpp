#include "catalog/catalog.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace arbordb {

Catalog::Catalog(const std::string& meta_path, BufferPoolManager* heap_bpm, BufferPoolManager* index_bpm,
                 LogManager* log, ConcurrencyControlManager* cc)
    : path_(meta_path), bpm_(heap_bpm), index_bpm_(index_bpm), log_(log), cc_(cc) {}

std::string Catalog::Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// File format (one record per line):
//   TABLE <name> <first_page_id> <column_count>
//   COL <name> <INTEGER|DOUBLE|VARCHAR|BOOLEAN>      (column_count lines follow a TABLE line)
//   INDEX <index_name> <table> <column_index>
void Catalog::Save() {
    std::string tmp = path_ + ".tmp";
    {
        std::ofstream o(tmp, std::ios::trunc);
        for (auto& [name, t] : tables_) {
            o << "TABLE " << name << " " << t->first_page << " " << t->schema.Size() << "\n";
            for (auto& c : t->schema.Columns()) o << "COL " << c.name << " " << TypeName(c.type) << "\n";
        }
        for (auto& [name, t] : tables_)
            for (auto& i : t->indexes) o << "INDEX " << i->name << " " << name << " " << i->column << "\n";
    }
    std::rename(tmp.c_str(), path_.c_str());  // atomic replace
}

static TypeId ParseType(const std::string& s) {
    if (s == "INTEGER") return TypeId::kInteger;
    if (s == "DOUBLE") return TypeId::kDouble;
    if (s == "VARCHAR") return TypeId::kVarchar;
    return TypeId::kBoolean;
}

std::unique_ptr<IndexInfo> Catalog::MakeIndex(const std::string& name, const TableInfo& t, int col) {
    auto idx = std::make_unique<IndexInfo>();
    idx->name = name;
    idx->table = t.name;
    idx->column = col;
    idx->tree = std::make_unique<IntIndex>(index_bpm_);
    return idx;
}

void Catalog::Load() {
    std::lock_guard<std::mutex> g(mu_);
    std::ifstream in(path_);
    std::string line;
    TableInfo* cur = nullptr;
    std::size_t remaining = 0;
    std::vector<Column> cols;
    std::string cur_name;
    page_id_t cur_first = kInvalidPageId;
    auto finish = [&] {
        auto t = std::make_unique<TableInfo>();
        t->name = cur_name;
        t->schema = Schema(cols);
        t->first_page = cur_first;
        t->heap = std::make_unique<TableHeap>(bpm_, log_, cc_, cur_first);
        tables_[cur_name] = std::move(t);
    };
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;
        if (kind == "TABLE") {
            ss >> cur_name >> cur_first >> remaining;
            cols.clear();
            if (remaining == 0) finish();
        } else if (kind == "COL") {
            std::string name, type;
            ss >> name >> type;
            cols.push_back(Column{name, ParseType(type), ""});
            if (--remaining == 0) finish();
        } else if (kind == "INDEX") {
            std::string iname, table;
            int col;
            ss >> iname >> table >> col;
            auto it = tables_.find(table);
            if (it != tables_.end()) it->second->indexes.push_back(MakeIndex(iname, *it->second, col));
        }
    }
    (void)cur;
}

TableInfo* Catalog::GetTable(const std::string& name) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = tables_.find(Lower(name));
    return it == tables_.end() ? nullptr : it->second.get();
}

std::vector<TableInfo*> Catalog::Tables() {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<TableInfo*> v;
    for (auto& [n, t] : tables_) v.push_back(t.get());
    return v;
}

TableInfo* Catalog::CreateTable(const std::string& name, const Schema& schema) {
    std::lock_guard<std::mutex> g(mu_);
    std::string n = Lower(name);
    if (tables_.count(n)) throw BindError("table already exists: " + n);
    if (schema.Size() == 0) throw BindError("a table needs at least one column");
    for (std::size_t i = 0; i < schema.Size(); i++)
        for (std::size_t j = i + 1; j < schema.Size(); j++)
            if (schema.At(i).name == schema.At(j).name) throw BindError("duplicate column name: " + schema.At(i).name);
    auto t = std::make_unique<TableInfo>();
    t->name = n;
    t->schema = schema;
    t->first_page = TableHeap::CreateFirstPage(bpm_);  // durable before the catalog mentions it
    t->heap = std::make_unique<TableHeap>(bpm_, log_, cc_, t->first_page);
    TableInfo* raw = t.get();
    tables_[n] = std::move(t);
    Save();
    return raw;
}

void Catalog::DropTable(const std::string& name) {
    std::lock_guard<std::mutex> g(mu_);
    if (!tables_.erase(Lower(name))) throw BindError("no such table: " + name);
    Save();  // pages are deliberately leaked, see class comment
}

void Catalog::Populate(TableInfo* t, IndexInfo* idx) {
    TableHeap::Cursor c = t->heap->Begin();
    RID rid;
    std::string bytes;
    while (t->heap->Next(nullptr, &c, &rid, &bytes)) {  // no txn: recovery / DDL time, nobody else is running
        Value v = DeserializeTuple(t->schema, bytes)[idx->column];
        if (IsNull(v)) continue;  // NULLs are not indexed
        if (!idx->tree->Insert(std::get<int64_t>(v), rid))
            throw ExecutionError("duplicate key " + ValueToString(v) + " while building index " + idx->name);
    }
}

IndexInfo* Catalog::CreateIndex(const std::string& index_name, const std::string& table, const std::string& column) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = tables_.find(Lower(table));
    if (it == tables_.end()) throw BindError("no such table: " + table);
    TableInfo* t = it->second.get();
    int col = t->schema.Find("", Lower(column));
    if (col < 0) throw BindError("no such column: " + column);
    if (t->schema.At(col).type != TypeId::kInteger) throw BindError("indexes are supported on INTEGER columns only");
    for (auto& [n, tt] : tables_)
        for (auto& i : tt->indexes)
            if (i->name == Lower(index_name)) throw BindError("index already exists: " + index_name);
    if (t->IndexOnColumn(col)) throw BindError("column already has an index");
    auto idx = MakeIndex(Lower(index_name), *t, col);
    Populate(t, idx.get());
    IndexInfo* raw = idx.get();
    t->indexes.push_back(std::move(idx));
    Save();
    return raw;
}

void Catalog::RebuildIndexes() {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& [n, t] : tables_)
        for (auto& i : t->indexes) Populate(t.get(), i.get());
}

}  // namespace arbordb
