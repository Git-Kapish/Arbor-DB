#pragma once

#include <string>
#include <vector>

#include "query/parser/ast.h"

namespace arbordb {

enum class TokenType { kKeyword, kIdent, kInt, kFloat, kString, kSymbol, kEnd };

struct Token {
    TokenType type;
    std::string text;  // keywords upper-cased, identifiers lower-cased, symbols as written
    std::size_t pos;   // byte offset in the SQL text (for error messages)
};

// Hand-written lexer + recursive-descent parser (no parser generator). One method per grammar rule.
//
//   statement  := create | drop | insert | select | update | delete | BEGIN | COMMIT | ROLLBACK | CHECKPOINT
//                 | EXPLAIN select
//   select     := SELECT items FROM table_ref {join} [WHERE expr] [GROUP BY exprs] [HAVING expr]
//                 [ORDER BY order_items] [LIMIT int]
//   join       := {, table_ref} | [INNER] JOIN table_ref ON expr | CROSS JOIN table_ref
//   expr       := or_expr ;  or := and {OR and} ; and := not {AND not} ; not := NOT not | comparison
//   comparison := additive [(= | <> | != | < | <= | > | >=) additive | IS [NOT] NULL
//                          | [NOT] BETWEEN additive AND additive | [NOT] IN (expr {, expr})]
//   additive   := term {(+|-) term} ; term := unary {(*|/|%) unary} ; unary := - unary | primary
//   primary    := literal | ( expr ) | [table .] column | agg ( * | expr )
class Parser {
public:
    explicit Parser(const std::string& sql);
    // Parses every ';'-separated statement. Throws ParseError with the position of the problem.
    std::vector<Statement> ParseScript();

private:
    Statement ParseStatement();
    Statement ParseCreate();
    Statement ParseDrop();
    InsertStmt ParseInsert();
    SelectStmt ParseSelectStatement();
    void ParseFromClause(SelectStmt* s);
    TableRef ParseTableRef();
    ExprPtr ParseWhereClause();
    std::vector<ExprPtr> ParseGroupBy();
    std::vector<OrderItem> ParseOrderBy();
    UpdateStmt ParseUpdate();
    DeleteStmt ParseDelete();

    ExprPtr ParseExpr();
    ExprPtr ParseOr();
    ExprPtr ParseAnd();
    ExprPtr ParseNot();
    ExprPtr ParseComparison();
    ExprPtr ParseAdditive();
    ExprPtr ParseTerm();
    ExprPtr ParseUnary();
    ExprPtr ParsePrimary();

    // token helpers
    const Token& Peek(std::size_t ahead = 0) const;
    Token Advance();
    bool IsKeyword(const char* kw, std::size_t ahead = 0) const;
    bool IsSymbol(const char* sym, std::size_t ahead = 0) const;
    bool AcceptKeyword(const char* kw);
    bool AcceptSymbol(const char* sym);
    void ExpectKeyword(const char* kw);
    void ExpectSymbol(const char* sym);
    std::string ExpectIdent(const char* what);
    [[noreturn]] void Fail(const std::string& msg) const;

    std::string sql_;
    std::vector<Token> toks_;
    std::size_t pos_{0};
};

std::vector<Statement> ParseSql(const std::string& sql);

}  // namespace arbordb
