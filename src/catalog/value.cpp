#include "catalog/value.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace arbordb {

const char* TypeName(TypeId t) {
    switch (t) {
        case TypeId::kInteger: return "INTEGER";
        case TypeId::kDouble: return "DOUBLE";
        case TypeId::kVarchar: return "VARCHAR";
        case TypeId::kBoolean: return "BOOLEAN";
    }
    return "?";
}

std::string ValueToString(const Value& v) {
    switch (v.index()) {
        case 0: return "NULL";
        case 1: return std::to_string(std::get<int64_t>(v));
        case 2: {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.10g", std::get<double>(v));
            return buf;
        }
        case 3: return std::get<std::string>(v);
        default: return std::get<bool>(v) ? "true" : "false";
    }
}

namespace {
bool IsNumeric(const Value& v) { return v.index() == 1 || v.index() == 2; }
double AsDouble(const Value& v) { return v.index() == 1 ? static_cast<double>(std::get<int64_t>(v)) : std::get<double>(v); }
}  // namespace

int CompareValues(const Value& a, const Value& b) {
    if (IsNumeric(a) && IsNumeric(b)) {
        if (a.index() == 1 && b.index() == 1) {
            int64_t x = std::get<int64_t>(a), y = std::get<int64_t>(b);
            return x < y ? -1 : (x > y ? 1 : 0);
        }
        double x = AsDouble(a), y = AsDouble(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a.index() == 3 && b.index() == 3) {
        int c = std::get<std::string>(a).compare(std::get<std::string>(b));
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    if (a.index() == 4 && b.index() == 4) return static_cast<int>(std::get<bool>(a)) - static_cast<int>(std::get<bool>(b));
    throw ExecutionError("cannot compare values of different types");
}

std::string ValueKey(const Value& v) {
    switch (v.index()) {
        case 0: return "N";
        case 1: return "n:" + std::to_string(std::get<int64_t>(v));
        case 2: {
            double d = std::get<double>(v);
            if (std::floor(d) == d && std::fabs(d) < 9e15) return "n:" + std::to_string(static_cast<int64_t>(d));
            char buf[64];
            std::snprintf(buf, sizeof buf, "d:%.17g", d);
            return buf;
        }
        case 3: return "s:" + std::get<std::string>(v);
        default: return std::get<bool>(v) ? "b:1" : "b:0";
    }
}

Value CoerceToType(const Value& v, TypeId t) {
    if (IsNull(v)) return v;
    switch (t) {
        case TypeId::kInteger:
            if (v.index() == 1) {
                int64_t x = std::get<int64_t>(v);
                if (x < std::numeric_limits<int32_t>::min() || x > std::numeric_limits<int32_t>::max())
                    throw ExecutionError("integer out of range for INTEGER column: " + std::to_string(x));
                return v;
            }
            break;
        case TypeId::kDouble:
            if (v.index() == 2) return v;
            if (v.index() == 1) return Value(static_cast<double>(std::get<int64_t>(v)));
            break;
        case TypeId::kVarchar:
            if (v.index() == 3) return v;
            break;
        case TypeId::kBoolean:
            if (v.index() == 4) return v;
            break;
    }
    throw ExecutionError(std::string("type mismatch: cannot store ") + ValueToString(v) + " in a " + TypeName(t) + " column");
}

int Schema::Find(const std::string& qualifier, const std::string& name) const {
    int found = kNotFound;
    for (std::size_t i = 0; i < cols_.size(); i++) {
        if (cols_[i].name != name) continue;
        if (!qualifier.empty() && cols_[i].qualifier != qualifier) continue;
        if (found != kNotFound) return kAmbiguous;
        found = static_cast<int>(i);
    }
    return found;
}

Schema Schema::Concat(const Schema& a, const Schema& b) {
    std::vector<Column> c = a.cols_;
    c.insert(c.end(), b.cols_.begin(), b.cols_.end());
    return Schema(std::move(c));
}

Schema Schema::WithQualifier(const std::string& q) const {
    Schema s = *this;
    for (auto& c : s.cols_) c.qualifier = q;
    return s;
}

std::string SerializeTuple(const Schema& schema, const std::vector<Value>& values) {
    if (values.size() != schema.Size()) throw ExecutionError("wrong number of values for row");
    std::size_t n = schema.Size();
    std::string out((n + 7) / 8, '\0');
    for (std::size_t i = 0; i < n; i++) {
        const Value& v = values[i];
        if (IsNull(v)) {
            out[i / 8] |= static_cast<char>(1 << (i % 8));
            continue;
        }
        Value c = CoerceToType(v, schema.At(i).type);
        switch (schema.At(i).type) {
            case TypeId::kInteger: {
                int32_t x = static_cast<int32_t>(std::get<int64_t>(c));
                out.append(reinterpret_cast<char*>(&x), 4);
                break;
            }
            case TypeId::kDouble: {
                double d = std::get<double>(c);
                out.append(reinterpret_cast<char*>(&d), 8);
                break;
            }
            case TypeId::kBoolean: out.push_back(std::get<bool>(c) ? 1 : 0); break;
            case TypeId::kVarchar: {
                const std::string& s = std::get<std::string>(c);
                if (s.size() > 60000) throw ExecutionError("string too long");
                uint16_t len = static_cast<uint16_t>(s.size());
                out.append(reinterpret_cast<char*>(&len), 2);
                out.append(s);
                break;
            }
        }
    }
    return out;
}

std::vector<Value> DeserializeTuple(const Schema& schema, const std::string& bytes) {
    std::size_t n = schema.Size();
    std::vector<Value> out(n);
    std::size_t pos = (n + 7) / 8;
    for (std::size_t i = 0; i < n; i++) {
        if (bytes[i / 8] & (1 << (i % 8))) continue;  // NULL
        switch (schema.At(i).type) {
            case TypeId::kInteger: {
                int32_t x;
                std::memcpy(&x, bytes.data() + pos, 4);
                pos += 4;
                out[i] = static_cast<int64_t>(x);
                break;
            }
            case TypeId::kDouble: {
                double d;
                std::memcpy(&d, bytes.data() + pos, 8);
                pos += 8;
                out[i] = d;
                break;
            }
            case TypeId::kBoolean: out[i] = bytes[pos++] != 0; break;
            case TypeId::kVarchar: {
                uint16_t len;
                std::memcpy(&len, bytes.data() + pos, 2);
                pos += 2;
                out[i] = std::string(bytes.data() + pos, len);
                pos += len;
                break;
            }
        }
    }
    return out;
}

}  // namespace arbordb
