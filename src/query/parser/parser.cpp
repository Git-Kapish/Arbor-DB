#include "query/parser/parser.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <set>

namespace arbordb {

namespace {
const std::set<std::string>& Keywords() {
    static const std::set<std::string> k = {
        "SELECT", "FROM", "WHERE", "GROUP", "BY", "HAVING", "ORDER", "ASC", "DESC", "LIMIT", "INSERT", "INTO",
        "VALUES", "UPDATE", "SET", "DELETE", "CREATE", "TABLE", "INDEX", "UNIQUE", "DROP", "JOIN", "INNER", "CROSS",
        "ON", "AS", "AND", "OR", "NOT", "NULL", "IS", "TRUE", "FALSE", "BETWEEN", "IN", "PRIMARY", "KEY", "COUNT",
        "SUM", "MIN", "MAX", "AVG", "BEGIN", "COMMIT", "ROLLBACK", "TRANSACTION", "CHECKPOINT", "EXPLAIN", "INT",
        "INTEGER", "DOUBLE", "FLOAT", "VARCHAR", "TEXT", "BOOLEAN"};
    return k;
}

std::vector<Token> Lex(const std::string& s) {
    std::vector<Token> out;
    std::size_t i = 0;
    auto fail = [&](const std::string& m, std::size_t at) { throw ParseError(m + " at position " + std::to_string(at)); };
    while (i < s.size()) {
        unsigned char c = s[i];
        if (std::isspace(c)) { i++; continue; }
        if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {  // comment to end of line
            while (i < s.size() && s[i] != '\n') i++;
            continue;
        }
        std::size_t start = i;
        if (std::isalpha(c) || c == '_') {
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) i++;
            std::string w = s.substr(start, i - start), up = w;
            std::transform(up.begin(), up.end(), up.begin(), [](unsigned char ch) { return std::toupper(ch); });
            if (Keywords().count(up)) out.push_back({TokenType::kKeyword, up, start});
            else {
                std::transform(w.begin(), w.end(), w.begin(), [](unsigned char ch) { return std::tolower(ch); });
                out.push_back({TokenType::kIdent, w, start});
            }
        } else if (std::isdigit(c)) {
            bool is_float = false;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++;
            if (i < s.size() && s[i] == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
                is_float = true;
                i++;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++;
            }
            out.push_back({is_float ? TokenType::kFloat : TokenType::kInt, s.substr(start, i - start), start});
        } else if (c == '\'') {
            std::string text;
            i++;
            bool closed = false;
            while (i < s.size()) {
                if (s[i] == '\'') {
                    if (i + 1 < s.size() && s[i + 1] == '\'') { text += '\''; i += 2; continue; }  // '' escape
                    closed = true;
                    i++;
                    break;
                }
                text += s[i++];
            }
            if (!closed) fail("unterminated string literal", start);
            out.push_back({TokenType::kString, text, start});
        } else {
            std::string two = i + 1 < s.size() ? s.substr(i, 2) : "";
            if (two == "<=" || two == ">=" || two == "<>" || two == "!=") {
                out.push_back({TokenType::kSymbol, two == "!=" ? "<>" : two, start});
                i += 2;
            } else if (std::string("(),;.*=<>+-/%").find(static_cast<char>(c)) != std::string::npos) {
                out.push_back({TokenType::kSymbol, std::string(1, static_cast<char>(c)), start});
                i++;
            } else {
                fail(std::string("unexpected character '") + static_cast<char>(c) + "'", start);
            }
        }
    }
    out.push_back({TokenType::kEnd, "", s.size()});
    return out;
}
}  // namespace

Parser::Parser(const std::string& sql) : sql_(sql), toks_(Lex(sql)) {}

const Token& Parser::Peek(std::size_t ahead) const { return toks_[std::min(pos_ + ahead, toks_.size() - 1)]; }
Token Parser::Advance() { return toks_[std::min(pos_++, toks_.size() - 1)]; }
bool Parser::IsKeyword(const char* kw, std::size_t a) const { return Peek(a).type == TokenType::kKeyword && Peek(a).text == kw; }
bool Parser::IsSymbol(const char* sym, std::size_t a) const { return Peek(a).type == TokenType::kSymbol && Peek(a).text == sym; }
bool Parser::AcceptKeyword(const char* kw) {
    if (!IsKeyword(kw)) return false;
    pos_++;
    return true;
}
bool Parser::AcceptSymbol(const char* sym) {
    if (!IsSymbol(sym)) return false;
    pos_++;
    return true;
}
void Parser::Fail(const std::string& msg) const {
    const Token& t = Peek();
    std::string near = t.type == TokenType::kEnd ? "end of input" : "'" + t.text + "'";
    throw ParseError(msg + " (near " + near + ", position " + std::to_string(t.pos) + ")");
}
void Parser::ExpectKeyword(const char* kw) {
    if (!AcceptKeyword(kw)) Fail(std::string("expected ") + kw);
}
void Parser::ExpectSymbol(const char* sym) {
    if (!AcceptSymbol(sym)) Fail(std::string("expected '") + sym + "'");
}
std::string Parser::ExpectIdent(const char* what) {
    if (Peek().type != TokenType::kIdent) Fail(std::string("expected ") + what);
    return Advance().text;
}

std::vector<Statement> Parser::ParseScript() {
    std::vector<Statement> out;
    while (true) {
        while (AcceptSymbol(";")) {}
        if (Peek().type == TokenType::kEnd) break;
        out.push_back(ParseStatement());
        if (Peek().type != TokenType::kEnd && !IsSymbol(";")) Fail("unexpected token after statement");
    }
    return out;
}

Statement Parser::ParseStatement() {
    if (IsKeyword("SELECT")) return ParseSelectStatement();
    if (AcceptKeyword("EXPLAIN")) {
        if (!IsKeyword("SELECT")) Fail("EXPLAIN supports SELECT only");
        return ExplainStmt{std::make_shared<SelectStmt>(ParseSelectStatement())};
    }
    if (AcceptKeyword("INSERT")) return ParseInsert();
    if (AcceptKeyword("UPDATE")) return ParseUpdate();
    if (AcceptKeyword("DELETE")) return ParseDelete();
    if (AcceptKeyword("CREATE")) return ParseCreate();
    if (AcceptKeyword("DROP")) return ParseDrop();
    if (AcceptKeyword("BEGIN")) { AcceptKeyword("TRANSACTION"); return TxnStmt{TxnStmt::kBegin}; }
    if (AcceptKeyword("COMMIT")) return TxnStmt{TxnStmt::kCommit};
    if (AcceptKeyword("ROLLBACK")) return TxnStmt{TxnStmt::kRollback};
    if (AcceptKeyword("CHECKPOINT")) return CheckpointStmt{};
    Fail("expected a statement");
}

Statement Parser::ParseCreate() {
    if (AcceptKeyword("TABLE")) {
        CreateTableStmt s;
        s.name = ExpectIdent("table name");
        ExpectSymbol("(");
        do {
            ColumnDef c;
            c.name = ExpectIdent("column name");
            if (AcceptKeyword("INT") || AcceptKeyword("INTEGER")) c.type = TypeId::kInteger;
            else if (AcceptKeyword("DOUBLE") || AcceptKeyword("FLOAT")) c.type = TypeId::kDouble;
            else if (AcceptKeyword("VARCHAR") || AcceptKeyword("TEXT")) {
                c.type = TypeId::kVarchar;
                if (AcceptSymbol("(")) {  // length is accepted and ignored
                    if (Peek().type != TokenType::kInt) Fail("expected a length");
                    Advance();
                    ExpectSymbol(")");
                }
            } else if (AcceptKeyword("BOOLEAN")) c.type = TypeId::kBoolean;
            else Fail("expected a column type (INTEGER, DOUBLE, VARCHAR, BOOLEAN)");
            if (AcceptKeyword("PRIMARY")) {
                ExpectKeyword("KEY");
                c.primary_key = true;
            }
            s.columns.push_back(c);
        } while (AcceptSymbol(","));
        ExpectSymbol(")");
        return s;
    }
    AcceptKeyword("UNIQUE");
    if (AcceptKeyword("INDEX")) {
        CreateIndexStmt s;
        s.name = ExpectIdent("index name");
        ExpectKeyword("ON");
        s.table = ExpectIdent("table name");
        ExpectSymbol("(");
        s.column = ExpectIdent("column name");
        ExpectSymbol(")");
        return s;
    }
    Fail("expected TABLE or INDEX after CREATE");
}

Statement Parser::ParseDrop() {
    ExpectKeyword("TABLE");
    return DropTableStmt{ExpectIdent("table name")};
}

InsertStmt Parser::ParseInsert() {
    InsertStmt s;
    ExpectKeyword("INTO");
    s.table = ExpectIdent("table name");
    if (AcceptSymbol("(")) {
        do s.columns.push_back(ExpectIdent("column name"));
        while (AcceptSymbol(","));
        ExpectSymbol(")");
    }
    ExpectKeyword("VALUES");
    do {
        ExpectSymbol("(");
        std::vector<ExprPtr> row;
        do row.push_back(ParseExpr());
        while (AcceptSymbol(","));
        ExpectSymbol(")");
        s.rows.push_back(std::move(row));
    } while (AcceptSymbol(","));
    return s;
}

TableRef Parser::ParseTableRef() {
    TableRef r;
    r.table = ExpectIdent("table name");
    r.alias = r.table;
    if (AcceptKeyword("AS")) r.alias = ExpectIdent("alias");
    else if (Peek().type == TokenType::kIdent) r.alias = Advance().text;
    return r;
}

void Parser::ParseFromClause(SelectStmt* s) {
    s->from = ParseTableRef();
    while (true) {
        if (AcceptSymbol(",")) {
            s->joins.push_back({ParseTableRef(), nullptr});
        } else if (AcceptKeyword("CROSS")) {
            ExpectKeyword("JOIN");
            s->joins.push_back({ParseTableRef(), nullptr});
        } else if (IsKeyword("JOIN") || IsKeyword("INNER")) {
            AcceptKeyword("INNER");
            ExpectKeyword("JOIN");
            TableRef r = ParseTableRef();
            ExpectKeyword("ON");
            s->joins.push_back({r, ParseExpr()});
        } else {
            break;
        }
    }
}

ExprPtr Parser::ParseWhereClause() { return ParseExpr(); }

std::vector<ExprPtr> Parser::ParseGroupBy() {
    std::vector<ExprPtr> v;
    do v.push_back(ParseExpr());
    while (AcceptSymbol(","));
    return v;
}

std::vector<OrderItem> Parser::ParseOrderBy() {
    std::vector<OrderItem> v;
    do {
        OrderItem o;
        o.expr = ParseExpr();
        if (AcceptKeyword("DESC")) o.desc = true;
        else AcceptKeyword("ASC");
        v.push_back(o);
    } while (AcceptSymbol(","));
    return v;
}

SelectStmt Parser::ParseSelectStatement() {
    ExpectKeyword("SELECT");
    SelectStmt s;
    do {
        SelectItem it;
        if (AcceptSymbol("*")) {
            it.star = true;
        } else if (Peek().type == TokenType::kIdent && IsSymbol(".", 1) && IsSymbol("*", 2)) {
            it.star = true;
            it.star_qualifier = Advance().text;
            Advance();
            Advance();
        } else {
            it.expr = ParseExpr();
            if (AcceptKeyword("AS")) it.alias = ExpectIdent("alias");
            else if (Peek().type == TokenType::kIdent) it.alias = Advance().text;
        }
        s.items.push_back(std::move(it));
    } while (AcceptSymbol(","));
    ExpectKeyword("FROM");
    ParseFromClause(&s);
    if (AcceptKeyword("WHERE")) s.where = ParseWhereClause();
    if (AcceptKeyword("GROUP")) {
        ExpectKeyword("BY");
        s.group_by = ParseGroupBy();
    }
    if (AcceptKeyword("HAVING")) s.having = ParseExpr();
    if (AcceptKeyword("ORDER")) {
        ExpectKeyword("BY");
        s.order_by = ParseOrderBy();
    }
    if (AcceptKeyword("LIMIT")) {
        if (Peek().type != TokenType::kInt) Fail("expected an integer after LIMIT");
        s.limit = std::atoll(Advance().text.c_str());
    }
    return s;
}

UpdateStmt Parser::ParseUpdate() {
    UpdateStmt s;
    s.table = ExpectIdent("table name");
    ExpectKeyword("SET");
    do {
        Assignment a;
        a.column = ExpectIdent("column name");
        ExpectSymbol("=");
        a.value = ParseExpr();
        s.assignments.push_back(std::move(a));
    } while (AcceptSymbol(","));
    if (AcceptKeyword("WHERE")) s.where = ParseWhereClause();
    return s;
}

DeleteStmt Parser::ParseDelete() {
    DeleteStmt s;
    ExpectKeyword("FROM");
    s.table = ExpectIdent("table name");
    if (AcceptKeyword("WHERE")) s.where = ParseWhereClause();
    return s;
}

// ---- expressions: one function per precedence level ----

ExprPtr Parser::ParseExpr() { return ParseOr(); }

ExprPtr Parser::ParseOr() {
    ExprPtr l = ParseAnd();
    while (AcceptKeyword("OR")) l = Bin(BinaryOp::kOr, l, ParseAnd());
    return l;
}

ExprPtr Parser::ParseAnd() {
    ExprPtr l = ParseNot();
    while (AcceptKeyword("AND")) l = Bin(BinaryOp::kAnd, l, ParseNot());
    return l;
}

ExprPtr Parser::ParseNot() {
    if (AcceptKeyword("NOT")) return std::make_shared<UnaryExpr>(UnaryOp::kNot, ParseNot());
    return ParseComparison();
}

ExprPtr Parser::ParseComparison() {
    ExprPtr l = ParseAdditive();
    struct { const char* sym; BinaryOp op; } ops[] = {{"=", BinaryOp::kEq}, {"<>", BinaryOp::kNe}, {"<=", BinaryOp::kLe},
                                                      {">=", BinaryOp::kGe}, {"<", BinaryOp::kLt}, {">", BinaryOp::kGt}};
    for (auto& o : ops)
        if (AcceptSymbol(o.sym)) return Bin(o.op, l, ParseAdditive());
    if (AcceptKeyword("IS")) {
        bool neg = AcceptKeyword("NOT");
        ExpectKeyword("NULL");
        return std::make_shared<UnaryExpr>(neg ? UnaryOp::kIsNotNull : UnaryOp::kIsNull, l);
    }
    bool negated = false;
    if (IsKeyword("NOT") && (IsKeyword("BETWEEN", 1) || IsKeyword("IN", 1))) {
        Advance();
        negated = true;
    }
    ExprPtr result;
    if (AcceptKeyword("BETWEEN")) {
        ExprPtr lo = ParseAdditive();
        ExpectKeyword("AND");
        ExprPtr hi = ParseAdditive();
        result = Bin(BinaryOp::kAnd, Bin(BinaryOp::kGe, l, lo), Bin(BinaryOp::kLe, l, hi));
    } else if (AcceptKeyword("IN")) {
        ExpectSymbol("(");
        do {
            ExprPtr eq = Bin(BinaryOp::kEq, l, ParseExpr());
            result = result ? Bin(BinaryOp::kOr, result, eq) : eq;
        } while (AcceptSymbol(","));
        ExpectSymbol(")");
    } else {
        return l;
    }
    return negated ? std::make_shared<UnaryExpr>(UnaryOp::kNot, result) : result;
}

ExprPtr Parser::ParseAdditive() {
    ExprPtr l = ParseTerm();
    while (true) {
        if (AcceptSymbol("+")) l = Bin(BinaryOp::kAdd, l, ParseTerm());
        else if (AcceptSymbol("-")) l = Bin(BinaryOp::kSub, l, ParseTerm());
        else return l;
    }
}

ExprPtr Parser::ParseTerm() {
    ExprPtr l = ParseUnary();
    while (true) {
        if (AcceptSymbol("*")) l = Bin(BinaryOp::kMul, l, ParseUnary());
        else if (AcceptSymbol("/")) l = Bin(BinaryOp::kDiv, l, ParseUnary());
        else if (AcceptSymbol("%")) l = Bin(BinaryOp::kMod, l, ParseUnary());
        else return l;
    }
}

ExprPtr Parser::ParseUnary() {
    if (AcceptSymbol("-")) {
        ExprPtr e = ParseUnary();
        if (e->kind == ExprKind::kLiteral) {  // fold -5 into a literal so it is a constant for index lookups
            const Value& v = static_cast<const LiteralExpr&>(*e).value;
            if (v.index() == 1) return Lit(Value(-std::get<int64_t>(v)));
            if (v.index() == 2) return Lit(Value(-std::get<double>(v)));
        }
        return std::make_shared<UnaryExpr>(UnaryOp::kNeg, e);
    }
    return ParsePrimary();
}

ExprPtr Parser::ParsePrimary() {
    const Token& t = Peek();
    switch (t.type) {
        case TokenType::kInt: {
            Token tok = Advance();
            errno = 0;
            long long v = std::strtoll(tok.text.c_str(), nullptr, 10);
            if (errno == ERANGE) Fail("integer literal out of range");
            return Lit(Value(static_cast<int64_t>(v)));
        }
        case TokenType::kFloat: return Lit(Value(std::strtod(Advance().text.c_str(), nullptr)));
        case TokenType::kString: return Lit(Value(Advance().text));
        case TokenType::kIdent: {
            std::string first = Advance().text;
            if (AcceptSymbol(".")) return Col(first, ExpectIdent("column name"));
            return Col("", first);
        }
        case TokenType::kKeyword: {
            if (t.text == "NULL") { Advance(); return Lit(Value()); }
            if (t.text == "TRUE") { Advance(); return Lit(Value(true)); }
            if (t.text == "FALSE") { Advance(); return Lit(Value(false)); }
            struct { const char* name; AggFunc f; } aggs[] = {{"COUNT", AggFunc::kCount}, {"SUM", AggFunc::kSum},
                                                              {"MIN", AggFunc::kMin}, {"MAX", AggFunc::kMax},
                                                              {"AVG", AggFunc::kAvg}};
            for (auto& a : aggs) {
                if (t.text == a.name) {
                    Advance();
                    ExpectSymbol("(");
                    ExprPtr arg;
                    if (AcceptSymbol("*")) {
                        if (a.f != AggFunc::kCount) Fail("only COUNT accepts *");
                    } else {
                        arg = ParseExpr();
                        if (ContainsAggregate(*arg)) Fail("aggregates cannot be nested");
                    }
                    ExpectSymbol(")");
                    return std::make_shared<AggregateExpr>(a.f, arg);
                }
            }
            break;
        }
        case TokenType::kSymbol:
            if (t.text == "(") {
                Advance();
                ExprPtr e = ParseExpr();
                ExpectSymbol(")");
                return e;
            }
            break;
        default: break;
    }
    Fail("expected an expression");
}

std::vector<Statement> ParseSql(const std::string& sql) { return Parser(sql).ParseScript(); }

}  // namespace arbordb
