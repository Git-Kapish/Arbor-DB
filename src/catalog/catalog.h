#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "catalog/value.h"
#include "index/bplus_tree.h"
#include "storage/table/table_heap.h"

namespace arbordb {

using IntIndex = BPlusTree<int64_t>;

// A unique index on one INTEGER column. Keys are column values, values are RIDs.
struct IndexInfo {
    std::string name;
    std::string table;
    int column;  // index into the table schema
    std::unique_ptr<IntIndex> tree;
};

struct TableInfo {
    std::string name;
    Schema schema;  // stored schema: unqualified column names
    page_id_t first_page;
    std::unique_ptr<TableHeap> heap;
    std::vector<std::unique_ptr<IndexInfo>> indexes;

    IndexInfo* IndexOnColumn(int col) const {
        for (auto& i : indexes)
            if (i->column == col) return i.get();
        return nullptr;
    }
};

// Table schemas and index definitions.
//
// Persistence: a dedicated text file (catalog.meta), rewritten atomically (write temp + rename) after
// every DDL statement. Decision recorded here per ARCHITECTURE.md §5: the catalog is NOT write-ahead
// logged and DDL is not transactional (a crash during DDL can leak a page, but never corrupt data).
// DROP TABLE does not free the table's pages, because redo of the dropped table's old log records
// could otherwise scribble on pages that were reused by another table.
//
// Index pages are never trusted across restarts: indexes live in a scratch file that is truncated at
// startup, and RebuildIndexes() repopulates them from the (already recovered) table heaps.
//
// Names are case-insensitive (stored lower-case). One Catalog per Database (not a global singleton, so
// tests can open several databases side by side).
class Catalog {
public:
    Catalog(const std::string& meta_path, BufferPoolManager* heap_bpm, BufferPoolManager* index_bpm, LogManager* log,
            ConcurrencyControlManager* cc);

    void Load();  // read catalog.meta (if present) and open every table's heap
    TableInfo* GetTable(const std::string& name);
    std::vector<TableInfo*> Tables();

    TableInfo* CreateTable(const std::string& name, const Schema& schema);
    void DropTable(const std::string& name);
    // Builds the index from existing rows; fails on duplicate keys or a non-INTEGER column.
    IndexInfo* CreateIndex(const std::string& index_name, const std::string& table, const std::string& column);
    void RebuildIndexes();  // after recovery: refill all index trees from the heaps

    static std::string Lower(std::string s);

private:
    void Save();
    void Populate(TableInfo* t, IndexInfo* idx);
    std::unique_ptr<IndexInfo> MakeIndex(const std::string& name, const TableInfo& t, int col);

    std::string path_;
    BufferPoolManager* bpm_;
    BufferPoolManager* index_bpm_;
    LogManager* log_;
    ConcurrencyControlManager* cc_;
    std::mutex mu_;
    std::map<std::string, std::unique_ptr<TableInfo>> tables_;
};

}  // namespace arbordb
