#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "query/expression.h"

namespace arbordb {

struct ColumnDef {
    std::string name;
    TypeId type;
    bool primary_key{false};  // creates a unique index (INTEGER columns only)
};
struct CreateTableStmt {
    std::string name;
    std::vector<ColumnDef> columns;
};
struct CreateIndexStmt {
    std::string name, table, column;
};
struct DropTableStmt {
    std::string name;
};
struct InsertStmt {
    std::string table;
    std::vector<std::string> columns;  // empty: all columns in table order
    std::vector<std::vector<ExprPtr>> rows;
};
struct TableRef {
    std::string table, alias;  // alias defaults to the table name
};
struct JoinItem {
    TableRef ref;
    ExprPtr on;  // nullptr for comma / CROSS joins
};
struct SelectItem {
    ExprPtr expr;  // null when star
    std::string alias;
    bool star{false};
    std::string star_qualifier;  // for t.*
};
struct OrderItem {
    ExprPtr expr;
    bool desc{false};
};
struct SelectStmt {
    std::vector<SelectItem> items;
    TableRef from;
    std::vector<JoinItem> joins;
    ExprPtr where;
    std::vector<ExprPtr> group_by;
    ExprPtr having;
    std::vector<OrderItem> order_by;
    std::optional<int64_t> limit;
};
struct Assignment {
    std::string column;
    ExprPtr value;
};
struct UpdateStmt {
    std::string table;
    std::vector<Assignment> assignments;
    ExprPtr where;
};
struct DeleteStmt {
    std::string table;
    ExprPtr where;
};
struct TxnStmt {
    enum Kind { kBegin, kCommit, kRollback } kind;
};
struct CheckpointStmt {};
struct ExplainStmt {
    std::shared_ptr<SelectStmt> select;
};

using Statement = std::variant<CreateTableStmt, CreateIndexStmt, DropTableStmt, InsertStmt, SelectStmt, UpdateStmt,
                               DeleteStmt, TxnStmt, CheckpointStmt, ExplainStmt>;

}  // namespace arbordb
