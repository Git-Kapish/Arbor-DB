#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "common/config.h"
#include "common/errors.h"

namespace arbordb {

enum class TypeId { kInteger, kDouble, kVarchar, kBoolean };
const char* TypeName(TypeId t);

// SQL value. monostate is NULL. INTEGER columns hold int64 in memory (stored as 4 bytes, range checked).
using Value = std::variant<std::monostate, int64_t, double, std::string, bool>;

inline bool IsNull(const Value& v) { return v.index() == 0; }
std::string ValueToString(const Value& v);
// Three-way compare of two NON-null values (ints and doubles compare numerically). Throws ExecutionError
// when the types are not comparable (e.g. string vs integer).
int CompareValues(const Value& a, const Value& b);
// Canonical text for hashing / grouping: equal SQL values (1 and 1.0) give equal keys.
std::string ValueKey(const Value& v);
// Convert to a column type for storage (int -> double ok; out-of-range or mismatched -> ExecutionError).
Value CoerceToType(const Value& v, TypeId t);

struct Column {
    std::string name;
    TypeId type;
    std::string qualifier;  // table alias; empty for stored table schemas and computed columns
};

class Schema {
public:
    Schema() = default;
    explicit Schema(std::vector<Column> cols) : cols_(std::move(cols)) {}
    const std::vector<Column>& Columns() const { return cols_; }
    std::size_t Size() const { return cols_.size(); }
    const Column& At(std::size_t i) const { return cols_[i]; }
    void Add(Column c) { cols_.push_back(std::move(c)); }
    // Index of the column, kNotFound, or kAmbiguous. qualifier may be empty (match by name only).
    static constexpr int kNotFound = -1, kAmbiguous = -2;
    int Find(const std::string& qualifier, const std::string& name) const;
    static Schema Concat(const Schema& a, const Schema& b);
    Schema WithQualifier(const std::string& q) const;

private:
    std::vector<Column> cols_;
};

struct Tuple {
    std::vector<Value> values;
    RID rid;  // where it came from (scans of base tables); not meaningful for computed rows
};

// Row format: [null bitmap: ceil(n/8) bytes][non-null fields in column order]
//   INTEGER 4 bytes LE | DOUBLE 8 | BOOLEAN 1 | VARCHAR u16 length + bytes
std::string SerializeTuple(const Schema& schema, const std::vector<Value>& values);
std::vector<Value> DeserializeTuple(const Schema& schema, const std::string& bytes);

}  // namespace arbordb
