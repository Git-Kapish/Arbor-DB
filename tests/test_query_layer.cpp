#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <future>
#include <map>
#include <random>
#include <sstream>
#include <thread>

#include "db/database.h"
#include "query/parser/parser.h"

using namespace arbordb;

namespace {
std::string NewDir(const std::string& name) {
    std::string d = "/tmp/arbor_q_" + std::to_string(getpid()) + "_" + name;
    std::string cmd = "rm -rf " + d;
    EXPECT_EQ(system(cmd.c_str()), 0);
    return d;
}

std::vector<std::string> Rows(Session& s, const std::string& sql) { return s.Execute(sql).RowStrings(); }

std::string ErrorOf(Session& s, const std::string& sql) {
    try {
        s.Execute(sql);
    } catch (const DbError& e) {
        return e.what();
    }
    return "";
}

std::string Plan(Session& s, const std::string& select) {
    std::string out;
    for (auto& r : s.Execute("EXPLAIN " + select).rows) out += std::get<std::string>(r[0]) + "\n";
    return out;
}

int Indent(const std::string& plan, const std::string& needle) {
    std::istringstream in(plan);
    std::string line;
    while (std::getline(in, line)) {
        auto p = line.find(needle);
        if (p != std::string::npos) return static_cast<int>(line.find_first_not_of(' '));
    }
    return -1;
}

// ---------- sqllogictest-style fixture runner ----------
struct Block {
    int line;
    std::string directive;
    std::vector<std::string> sql, expected;
};

std::vector<Block> ReadBlocks(const std::string& path) {
    std::ifstream in(path);
    std::vector<Block> blocks;
    std::string line;
    int n = 0;
    Block cur;
    bool have = false, in_expected = false;
    auto flush = [&] {
        if (have) blocks.push_back(cur);
        cur = Block();
        have = in_expected = false;
    };
    while (std::getline(in, line)) {
        n++;
        if (!have && (line.empty() || line[0] == '#')) continue;
        if (line.empty()) { flush(); continue; }
        if (!have) {
            cur.line = n;
            cur.directive = line;
            have = true;
        } else if (line == "----") {
            in_expected = true;
        } else if (in_expected) {
            cur.expected.push_back(line);
        } else {
            cur.sql.push_back(line);
        }
    }
    flush();
    return blocks;
}

void RunFixture(const std::string& file) {
    std::string path = std::string(ARBOR_SQL_DIR) + "/" + file;
    Database db(NewDir("fx_" + file));
    Session s(&db);
    for (auto& b : ReadBlocks(path)) {
        std::string sql;
        for (auto& l : b.sql) sql += l + "\n";
        std::string where = file + ":" + std::to_string(b.line) + " [" + b.directive + "]\n" + sql;
        if (b.directive == "statement ok") {
            try {
                s.Execute(sql);
            } catch (const std::exception& e) {
                ADD_FAILURE() << where << "unexpected error: " << e.what();
            }
        } else if (b.directive.rfind("statement error", 0) == 0) {
            std::string want = b.directive.size() > 16 ? b.directive.substr(16) : "";
            std::string got = ErrorOf(s, sql);
            if (got.empty()) ADD_FAILURE() << where << "expected an error but the statement succeeded";
            else if (got.find(want) == std::string::npos) ADD_FAILURE() << where << "error was: " << got << "\nexpected it to contain: " << want;
        } else if (b.directive == "query" || b.directive == "query rowsort") {
            try {
                auto got = Rows(s, sql);
                auto want = b.expected;
                if (b.directive == "query rowsort") { std::sort(got.begin(), got.end()); std::sort(want.begin(), want.end()); }
                if (got != want) {
                    std::string g, w;
                    for (auto& x : got) g += "  " + x + "\n";
                    for (auto& x : want) w += "  " + x + "\n";
                    ADD_FAILURE() << where << "got:\n" << g << "expected:\n" << w;
                }
            } catch (const std::exception& e) {
                ADD_FAILURE() << where << "unexpected error: " << e.what();
            }
        } else if (b.directive == "plan") {
            std::string plan = Plan(s, sql);
            for (auto& e : b.expected) {
                bool negate = !e.empty() && e[0] == '!';
                std::string needle = negate ? e.substr(1) : e;
                bool present = plan.find(needle) != std::string::npos;
                if (present == negate) ADD_FAILURE() << where << "plan " << (negate ? "must not contain" : "must contain") << ": " << needle << "\nplan:\n" << plan;
            }
        } else {
            ADD_FAILURE() << where << "unknown directive";
        }
    }
}
}  // namespace

// ---------------------------------------------------------------- fixtures
TEST(SqlFixtures, CompanySchemaJoinsAggregatesAndDml) { RunFixture("company.test"); }
TEST(SqlFixtures, ErrorsAreReportedAndHaveNoSideEffects) { RunFixture("errors.test"); }
TEST(SqlFixtures, TypesNullsAndThreeValuedLogic) { RunFixture("types.test"); }
TEST(SqlFixtures, OptimizerPlanShapes) { RunFixture("plans.test"); }

// ---------------------------------------------------------------- parser
namespace {
const SelectStmt& AsSelect(const std::vector<Statement>& v, std::size_t i = 0) { return std::get<SelectStmt>(v.at(i)); }
std::string Expr0(const std::string& sql) {
    auto stmts = ParseSql(sql);
    return ExprToString(*AsSelect(stmts).items[0].expr);
}
}  // namespace

TEST(Parser, OperatorPrecedenceAndAssociativity) {
    EXPECT_EQ(Expr0("SELECT 1 + 2 * 3 FROM t"), "(1 + (2 * 3))");
    EXPECT_EQ(Expr0("SELECT (1 + 2) * 3 FROM t"), "((1 + 2) * 3)");
    EXPECT_EQ(Expr0("SELECT 10 - 4 - 3 FROM t"), "((10 - 4) - 3)");
    EXPECT_EQ(Expr0("SELECT 8 / 2 % 3 FROM t"), "((8 / 2) % 3)");
    EXPECT_EQ(Expr0("SELECT a OR b AND c FROM t"), "(a OR (b AND c))");
    EXPECT_EQ(Expr0("SELECT NOT a AND b FROM t"), "(NOT a AND b)");
    EXPECT_EQ(Expr0("SELECT a = 1 AND b < 2 OR c >= 3 FROM t"), "(((a = 1) AND (b < 2)) OR (c >= 3))");
    EXPECT_EQ(Expr0("SELECT a + 1 > b * 2 FROM t"), "((a + 1) > (b * 2))");
}

TEST(Parser, LiteralsUnaryMinusAndSugar) {
    EXPECT_EQ(Expr0("SELECT -5 FROM t"), "-5");  // folded into a literal
    EXPECT_EQ(Expr0("SELECT - a FROM t"), "-a");
    EXPECT_EQ(Expr0("SELECT 1 - -2 FROM t"), "(1 - -2)");
    EXPECT_EQ(Expr0("SELECT 'it''s' FROM t"), "'it's'");
    EXPECT_EQ(Expr0("SELECT 2.5 FROM t"), "2.5");
    EXPECT_EQ(Expr0("SELECT x BETWEEN 1 AND 5 FROM t"), "((x >= 1) AND (x <= 5))");
    EXPECT_EQ(Expr0("SELECT x NOT BETWEEN 1 AND 5 FROM t"), "NOT ((x >= 1) AND (x <= 5))");
    EXPECT_EQ(Expr0("SELECT x IN (1, 2, 3) FROM t"), "(((x = 1) OR (x = 2)) OR (x = 3))");
    EXPECT_EQ(Expr0("SELECT x IS NOT NULL FROM t"), "x IS NOT NULL");
    EXPECT_EQ(Expr0("SELECT x != 3 FROM t"), "(x <> 3)");
    EXPECT_EQ(Expr0("SELECT COUNT(*), SUM(a + 1) FROM t"), "COUNT(*)");
}

TEST(Parser, KeywordsAreCaseInsensitiveIdentifiersFoldToLower) {
    auto s = AsSelect(ParseSql("select Name FROM Emp E where E.ID = 1"));
    EXPECT_EQ(s.from.table, "emp");
    EXPECT_EQ(s.from.alias, "e");
    EXPECT_EQ(ExprToString(*s.items[0].expr), "name");
    EXPECT_EQ(ExprToString(*s.where), "(e.id = 1)");
}

TEST(Parser, FullSelectClauseStructure) {
    auto stmts = ParseSql(
        "SELECT a.x AS ax, b.* , COUNT(*) c FROM t1 a JOIN t2 AS b ON a.id = b.id, t3 CROSS JOIN t4 "
        "WHERE a.x > 1 GROUP BY a.x, b.y HAVING COUNT(*) > 2 ORDER BY ax DESC, 2 LIMIT 10 -- trailing comment");
    const SelectStmt& s = AsSelect(stmts);
    ASSERT_EQ(s.items.size(), 3u);
    EXPECT_EQ(s.items[0].alias, "ax");
    EXPECT_TRUE(s.items[1].star);
    EXPECT_EQ(s.items[1].star_qualifier, "b");
    EXPECT_EQ(s.items[2].alias, "c");
    EXPECT_EQ(s.from.alias, "a");
    ASSERT_EQ(s.joins.size(), 3u);
    EXPECT_NE(s.joins[0].on, nullptr);
    EXPECT_EQ(s.joins[1].on, nullptr);  // comma
    EXPECT_EQ(s.joins[2].on, nullptr);  // CROSS JOIN
    EXPECT_EQ(s.group_by.size(), 2u);
    ASSERT_NE(s.having, nullptr);
    ASSERT_EQ(s.order_by.size(), 2u);
    EXPECT_TRUE(s.order_by[0].desc);
    EXPECT_FALSE(s.order_by[1].desc);
    ASSERT_TRUE(s.limit.has_value());
    EXPECT_EQ(*s.limit, 10);
}

TEST(Parser, DdlDmlAndTransactionStatements) {
    auto v = ParseSql(
        "CREATE TABLE t (id INT PRIMARY KEY, n VARCHAR(10), d DOUBLE, b BOOLEAN);"
        "CREATE INDEX i ON t (n); DROP TABLE t;"
        "INSERT INTO t (id, n) VALUES (1, 'a'), (2, 'b');"
        "UPDATE t SET n = 'z', d = d * 2 WHERE id = 1; DELETE FROM t WHERE id = 2;"
        "BEGIN; COMMIT; ROLLBACK; CHECKPOINT; EXPLAIN SELECT * FROM t;");
    ASSERT_EQ(v.size(), 11u);
    auto& ct = std::get<CreateTableStmt>(v[0]);
    ASSERT_EQ(ct.columns.size(), 4u);
    EXPECT_TRUE(ct.columns[0].primary_key);
    EXPECT_EQ(ct.columns[1].type, TypeId::kVarchar);
    EXPECT_EQ(std::get<InsertStmt>(v[3]).rows.size(), 2u);
    EXPECT_EQ(std::get<InsertStmt>(v[3]).columns.size(), 2u);
    EXPECT_EQ(std::get<UpdateStmt>(v[4]).assignments.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<DeleteStmt>(v[5]));
    EXPECT_EQ(std::get<TxnStmt>(v[6]).kind, TxnStmt::kBegin);
    EXPECT_TRUE(std::holds_alternative<CheckpointStmt>(v[9]));
    EXPECT_TRUE(std::holds_alternative<ExplainStmt>(v[10]));
}

TEST(Parser, SyntaxErrorsCarryAPosition) {
    for (const char* bad : {"SELECT", "SELECT FROM t", "SELECT * FROM", "SELECT * FROM t WHERE", "SELECT * FROM t LIMIT",
                            "INSERT INTO t VALUES", "INSERT INTO t VALUES (1,", "UPDATE t SET", "CREATE TABLE t ()",
                            "CREATE TABLE t (a BLOB)", "SELECT 'unterminated", "SELECT * FROM t WHERE a = $",
                            "SELECT (1 + 2 FROM t", "SELECT COUNT(SUM(a)) FROM t", "SELECT SUM(*) FROM t",
                            "DELETE t", "FOO BAR", "SELECT * FROM t t2 t3", "SELECT 99999999999999999999 FROM t"}) {
        try {
            ParseSql(bad);
            FAIL() << "should not parse: " << bad;
        } catch (const ParseError& e) {
            EXPECT_NE(std::string(e.what()).find("position"), std::string::npos) << bad << " -> " << e.what();
        }
    }
}

TEST(Parser, EmptyInputAndStraySemicolons) {
    EXPECT_TRUE(ParseSql("").empty());
    EXPECT_TRUE(ParseSql(" ;; -- nothing\n ; ").empty());
    EXPECT_EQ(ParseSql(";SELECT 1 FROM t;;SELECT 2 FROM t;").size(), 2u);
}

// ---------------------------------------------------------------- optimizer decisions
TEST(Optimizer, PredicatePushedBelowTheJoin) {
    Database db(NewDir("opt_push"));
    Session s(&db);
    s.ExecuteScript("CREATE TABLE a (id INTEGER, v INTEGER); CREATE TABLE b (id INTEGER, w INTEGER);");
    std::string plan = Plan(s, "SELECT a.v, b.w FROM a JOIN b ON a.id = b.id WHERE a.v > 5 AND b.w < 9");
    EXPECT_GT(Indent(plan, "Filter (a.v > 5)"), Indent(plan, "HashJoin")) << plan;
    EXPECT_GT(Indent(plan, "Filter (b.w < 9)"), Indent(plan, "HashJoin")) << plan;
}

TEST(Optimizer, SmallerTableBecomesTheHashBuildSide) {
    Database db(NewDir("opt_order"));
    Session s(&db);
    s.ExecuteScript("CREATE TABLE big (id INTEGER, pad VARCHAR(200)); CREATE TABLE tiny (id INTEGER);");
    std::string pad(150, 'x');
    for (int i = 0; i < 400; i++) s.Execute("INSERT INTO big VALUES (" + std::to_string(i) + ", '" + pad + "')");  // many pages
    s.Execute("INSERT INTO tiny VALUES (1), (2), (3)");
    // Written with the big table first; the optimizer must flip the join so `tiny` is the left/build input.
    for (std::string q : {"SELECT * FROM big JOIN tiny ON big.id = tiny.id", "SELECT * FROM tiny JOIN big ON big.id = tiny.id",
                          "SELECT * FROM big, tiny WHERE big.id = tiny.id"}) {
        std::string plan = Plan(s, q);
        EXPECT_LT(plan.find("SeqScan tiny"), plan.find("SeqScan big")) << q << "\n" << plan;
        EXPECT_EQ(s.Execute(q.replace(0, 8, "SELECT COUNT(*)")).RowStrings(), (std::vector<std::string>{"3"})) << q;
    }
}

TEST(Optimizer, ThreeWayJoinAvoidsCrossProductWhenPredicatesConnect) {
    Database db(NewDir("opt_3way"));
    Session s(&db);
    s.ExecuteScript("CREATE TABLE a (id INTEGER); CREATE TABLE b (id INTEGER, a_id INTEGER); CREATE TABLE c (id INTEGER, b_id INTEGER);");
    std::string plan = Plan(s, "SELECT * FROM a, c, b WHERE b.a_id = a.id AND c.b_id = b.id");
    EXPECT_EQ(plan.find("cross"), std::string::npos) << plan;
    EXPECT_EQ(plan.find("NestedLoopJoin"), std::string::npos) << plan;
}

TEST(Optimizer, IndexScanChosenForUpdateAndDeleteToo) {
    Database db(NewDir("opt_dml"));
    Session s(&db);
    s.ExecuteScript("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER); INSERT INTO t VALUES (1, 1), (2, 2), (3, 3);");
    EXPECT_EQ(s.Execute("UPDATE t SET v = v + 100 WHERE id = 2").rows_affected, 1);
    EXPECT_EQ(Rows(s, "SELECT v FROM t WHERE id = 2"), (std::vector<std::string>{"102"}));
    EXPECT_EQ(s.Execute("DELETE FROM t WHERE id >= 2").rows_affected, 2);
    EXPECT_EQ(Rows(s, "SELECT id FROM t"), (std::vector<std::string>{"1"}));
}

// ---------------------------------------------------------------- index correctness
TEST(Indexes, IndexScanAndSeqScanAgreeOnRandomRangeQueries) {
    Database db(NewDir("idx_equiv"));
    Session s(&db);
    s.ExecuteScript("CREATE TABLE indexed (id INTEGER PRIMARY KEY, v INTEGER); CREATE TABLE plain (id INTEGER, v INTEGER);");
    std::mt19937 rng(7);
    std::vector<int> ids(300);
    for (int i = 0; i < 300; i++) ids[i] = i * 3 - 100;  // gaps and negatives
    std::shuffle(ids.begin(), ids.end(), rng);
    for (int id : ids) {
        std::string vals = " VALUES (" + std::to_string(id) + ", " + std::to_string(id % 17) + ")";
        s.Execute("INSERT INTO indexed" + vals);
        s.Execute("INSERT INTO plain" + vals);
    }
    // mutate both identically: deletes and key-changing updates
    for (int i = 0; i < 60; i++) {
        int id = ids[rng() % ids.size()];
        std::string w = " WHERE id = " + std::to_string(id);
        s.Execute("DELETE FROM indexed" + w);
        s.Execute("DELETE FROM plain" + w);
    }
    s.Execute("UPDATE indexed SET id = id + 1000 WHERE id > 500");
    s.Execute("UPDATE plain SET id = id + 1000 WHERE id > 500");
    const char* ops[] = {"=", "<", "<=", ">", ">="};
    for (int q = 0; q < 120; q++) {
        int a = static_cast<int>(rng() % 1600) - 200, b = a + static_cast<int>(rng() % 400);
        std::string pred = "id " + std::string(ops[rng() % 5]) + " " + std::to_string(a);
        if (rng() % 2) pred += " AND id " + std::string(ops[1 + rng() % 4]) + " " + std::to_string(b);
        if (rng() % 3 == 0) pred += " AND v < 9";
        auto with = Rows(s, "SELECT id, v FROM indexed WHERE " + pred + " ORDER BY id");
        auto without = Rows(s, "SELECT id, v FROM plain WHERE " + pred + " ORDER BY id");
        ASSERT_EQ(with, without) << pred;
    }
    EXPECT_NE(Plan(s, "SELECT * FROM indexed WHERE id > 5").find("IndexScan"), std::string::npos);
}

TEST(Indexes, UniqueIndexRejectsDuplicatesAndAllowsMultipleNulls) {
    Database db(NewDir("idx_unique"));
    Session s(&db);
    s.ExecuteScript("CREATE TABLE t (k INTEGER, v INTEGER); CREATE INDEX ik ON t (k); INSERT INTO t VALUES (1, 1), (NULL, 2), (NULL, 3);");
    EXPECT_NE(ErrorOf(s, "INSERT INTO t VALUES (1, 9)").find("duplicate key"), std::string::npos);
    EXPECT_NE(ErrorOf(s, "UPDATE t SET k = 1 WHERE v = 2").find("duplicate key"), std::string::npos);
    EXPECT_EQ(Rows(s, "SELECT COUNT(*) FROM t"), (std::vector<std::string>{"3"}));
    EXPECT_EQ(Rows(s, "SELECT v FROM t WHERE k = 1"), (std::vector<std::string>{"1"}));
    EXPECT_NE(ErrorOf(s, "CREATE INDEX iz ON t (nope)").find("no such column"), std::string::npos);
}

TEST(Indexes, CreateIndexOnExistingDataBuildsFromRowsAndRejectsDuplicates) {
    Database db(NewDir("idx_build"));
    Session s(&db);
    s.ExecuteScript("CREATE TABLE t (k INTEGER, v INTEGER); INSERT INTO t VALUES (5, 1), (6, 2), (5, 3);");
    EXPECT_NE(ErrorOf(s, "CREATE INDEX ik ON t (k)").find("duplicate key"), std::string::npos);
    s.Execute("DELETE FROM t WHERE v = 3");
    s.Execute("CREATE INDEX ik ON t (k)");
    EXPECT_NE(Plan(s, "SELECT * FROM t WHERE k = 6").find("IndexScan"), std::string::npos);
    EXPECT_EQ(Rows(s, "SELECT v FROM t WHERE k = 6"), (std::vector<std::string>{"2"}));
}

TEST(Indexes, MultiPageTableWithManyRowsStaysCorrect) {
    Database db(NewDir("idx_big"));
    Session s(&db);
    s.Execute("CREATE TABLE t (id INTEGER PRIMARY KEY, s VARCHAR(100))");
    for (int base = 0; base < 3000; base += 100) {
        std::string sql = "INSERT INTO t VALUES ";
        for (int i = base; i < base + 100; i++) sql += (i > base ? ", (" : "(") + std::to_string(i) + ", 'row-" + std::to_string(i) + "-" + std::string(40, 'p') + "')";
        s.Execute(sql);
    }
    EXPECT_EQ(Rows(s, "SELECT COUNT(*) FROM t"), (std::vector<std::string>{"3000"}));
    EXPECT_EQ(Rows(s, "SELECT COUNT(*) FROM t WHERE id >= 1000 AND id < 2000"), (std::vector<std::string>{"1000"}));
    EXPECT_EQ(Rows(s, "SELECT id FROM t WHERE id = 2999"), (std::vector<std::string>{"2999"}));
}

// ---------------------------------------------------------------- persistence and recovery
TEST(Persistence, CleanRestartKeepsSchemaDataAndRebuildsIndexes) {
    std::string dir = NewDir("persist");
    {
        Database db(dir);
        Session s(&db);
        s.ExecuteScript(
            "CREATE TABLE t (id INTEGER PRIMARY KEY, name VARCHAR(10), score DOUBLE);"
            "INSERT INTO t VALUES (1, 'a', 1.5), (2, 'b', NULL), (3, 'c', 3.25);"
            "UPDATE t SET score = 9 WHERE id = 1; DELETE FROM t WHERE id = 2;");
    }
    Database db(dir);
    Session s(&db);
    EXPECT_EQ(Rows(s, "SELECT id, name, score FROM t ORDER BY id"), (std::vector<std::string>{"1|a|9", "3|c|3.25"}));
    EXPECT_NE(Plan(s, "SELECT * FROM t WHERE id = 3").find("IndexScan"), std::string::npos);
    EXPECT_EQ(Rows(s, "SELECT name FROM t WHERE id = 3"), (std::vector<std::string>{"c"}));
    EXPECT_NE(ErrorOf(s, "INSERT INTO t VALUES (3, 'dup', 0)").find("duplicate key"), std::string::npos);
    s.Execute("INSERT INTO t VALUES (2, 'again', 0)");  // the deleted key is free again
    EXPECT_EQ(Rows(s, "SELECT COUNT(*) FROM t"), (std::vector<std::string>{"3"}));
}

TEST(Recovery, CommittedWorkSurvivesACrashAndInFlightTransactionVanishes) {
    std::string dir = NewDir("sql_crash");
    {
        Database db(dir);
        Session s(&db);
        s.ExecuteScript(
            "CREATE TABLE acct (id INTEGER PRIMARY KEY, bal INTEGER);"
            "INSERT INTO acct VALUES (1, 100), (2, 200), (3, 300);"
            "UPDATE acct SET bal = 150 WHERE id = 1;");          // autocommit: durable
        s.Execute("BEGIN");
        s.Execute("UPDATE acct SET bal = 0 WHERE id = 2");
        s.Execute("DELETE FROM acct WHERE id = 3");
        s.Execute("INSERT INTO acct VALUES (4, 400)");
        Session other(&db);
        other.Execute("CHECKPOINT");                              // flushes the uncommitted changes to disk
        s.Execute("INSERT INTO acct VALUES (5, 500)");            // after the checkpoint: log only
        db.Crash();                                               // kill -9 equivalent
        s.Abandon();
    }
    Database db(dir);
    EXPECT_GT(db.LastRecovery().txns_undone, 0u);
    Session s(&db);
    EXPECT_EQ(Rows(s, "SELECT id, bal FROM acct ORDER BY id"), (std::vector<std::string>{"1|150", "2|200", "3|300"}));
    // The index was rebuilt from the recovered heap and agrees with it.
    EXPECT_EQ(Rows(s, "SELECT bal FROM acct WHERE id = 3"), (std::vector<std::string>{"300"}));
    EXPECT_TRUE(Rows(s, "SELECT bal FROM acct WHERE id = 4").empty());
    s.Execute("INSERT INTO acct VALUES (4, 41)");  // key 4 is free: the loser's insert is gone from the index too
}

TEST(Recovery, RepeatedCrashesWithoutCleanShutdownConverge) {
    std::string dir = NewDir("sql_crash2");
    {
        Database db(dir);
        Session s(&db);
        s.ExecuteScript("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER); INSERT INTO t VALUES (1, 1), (2, 2);");
        s.Execute("BEGIN");
        s.Execute("UPDATE t SET v = v + 100");
        db.Crash();
        s.Abandon();
    }
    for (int round = 0; round < 3; round++) {
        Database db(dir);
        Session s(&db);
        EXPECT_EQ(Rows(s, "SELECT v FROM t WHERE id < 10 ORDER BY id"), (std::vector<std::string>{"1", "2"})) << "round " << round;
        s.Execute("INSERT INTO t VALUES (" + std::to_string(10 + round) + ", 0)");
        db.Crash();
    }
    Database db(dir);
    Session s(&db);
    EXPECT_EQ(Rows(s, "SELECT COUNT(*) FROM t"), (std::vector<std::string>{"5"}));
}

// ---------------------------------------------------------------- transactions through SQL
TEST(Transactions, RollbackRestoresRowsAndIndex) {
    Database db(NewDir("txn_rb"));
    Session s(&db);
    s.ExecuteScript("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER); INSERT INTO t VALUES (1, 10), (2, 20);");
    s.Execute("BEGIN");
    s.Execute("INSERT INTO t VALUES (3, 30)");
    s.Execute("UPDATE t SET v = 99, id = 7 WHERE id = 1");   // changes the indexed key too
    s.Execute("DELETE FROM t WHERE id = 2");
    EXPECT_EQ(Rows(s, "SELECT id, v FROM t ORDER BY id"), (std::vector<std::string>{"3|30", "7|99"}));  // own changes visible
    s.Execute("ROLLBACK");
    EXPECT_FALSE(s.InTransaction());
    EXPECT_EQ(Rows(s, "SELECT id, v FROM t ORDER BY id"), (std::vector<std::string>{"1|10", "2|20"}));
    EXPECT_EQ(Rows(s, "SELECT v FROM t WHERE id = 1"), (std::vector<std::string>{"10"}));  // via the (restored) index
    EXPECT_TRUE(Rows(s, "SELECT v FROM t WHERE id = 7").empty());
    EXPECT_TRUE(Rows(s, "SELECT v FROM t WHERE id = 3").empty());
    s.Execute("INSERT INTO t VALUES (3, 33)");  // key 3 is free again
}

TEST(Transactions, CommitMakesChangesVisibleToOtherSessions) {
    Database db(NewDir("txn_commit"));
    Session a(&db), b(&db);
    a.Execute("CREATE TABLE t (id INTEGER, v INTEGER)");
    a.Execute("INSERT INTO t VALUES (1, 1)");
    a.Execute("BEGIN");
    a.Execute("UPDATE t SET v = 2");
    a.Execute("COMMIT");
    EXPECT_EQ(Rows(b, "SELECT v FROM t"), (std::vector<std::string>{"2"}));
    EXPECT_NE(ErrorOf(b, "COMMIT").find("no transaction"), std::string::npos);
    b.Execute("BEGIN");
    EXPECT_NE(ErrorOf(b, "BEGIN").find("already"), std::string::npos);
}

TEST(Transactions, ExecutionErrorRollsBackTheWholeTransaction) {
    Database db(NewDir("txn_err"));
    Session s(&db);
    s.ExecuteScript("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER); INSERT INTO t VALUES (1, 1);");
    s.Execute("BEGIN");
    s.Execute("INSERT INTO t VALUES (2, 2)");
    EXPECT_NE(ErrorOf(s, "INSERT INTO t VALUES (1, 9)").find("duplicate key"), std::string::npos);
    EXPECT_FALSE(s.InTransaction());  // aborted: earlier statements were undone as well
    EXPECT_EQ(Rows(s, "SELECT id FROM t ORDER BY id"), (std::vector<std::string>{"1"}));
    // A bind error has no side effects and leaves the transaction open.
    s.Execute("BEGIN");
    EXPECT_NE(ErrorOf(s, "SELECT nope FROM t").find("unknown column"), std::string::npos);
    EXPECT_TRUE(s.InTransaction());
}

TEST(Transactions, UncommittedWriteBlocksAnotherSessionsReadUntilCommit) {
    Database db(NewDir("txn_iso"));
    Session writer(&db);
    writer.ExecuteScript("CREATE TABLE t (id INTEGER, v INTEGER); INSERT INTO t VALUES (1, 100);");
    writer.Execute("BEGIN");
    writer.Execute("UPDATE t SET v = 200 WHERE id = 1");
    std::atomic<bool> done{false};
    std::vector<std::string> seen;
    std::thread t([&] {
        Session reader(&db);
        seen = Rows(reader, "SELECT v FROM t WHERE id = 1");  // seq scan: must wait for the writer's X lock
        done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(done) << "reader saw (or skipped) an uncommitted row instead of waiting";
    writer.Execute("COMMIT");
    t.join();
    EXPECT_EQ(seen, (std::vector<std::string>{"200"}));
}

TEST(Transactions, SqlLevelDeadlockAbortsTheYoungestAndTheOtherFinishes) {
    Database db(NewDir("txn_dead"));
    Session a(&db), b(&db);
    a.ExecuteScript("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER); INSERT INTO t VALUES (1, 0), (2, 0);");
    a.Execute("BEGIN");
    b.Execute("BEGIN");  // b is younger
    a.Execute("UPDATE t SET v = 1 WHERE id = 1");
    b.Execute("UPDATE t SET v = 2 WHERE id = 2");
    std::string a_err = "unset", b_err = "unset";
    auto fa = std::async(std::launch::async, [&] { a_err = ErrorOf(a, "UPDATE t SET v = 1 WHERE id = 2"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    auto fb = std::async(std::launch::async, [&] { b_err = ErrorOf(b, "UPDATE t SET v = 2 WHERE id = 1"); });
    ASSERT_EQ(fa.wait_for(std::chrono::seconds(10)), std::future_status::ready) << "deadlock was not resolved";
    ASSERT_EQ(fb.wait_for(std::chrono::seconds(10)), std::future_status::ready) << "deadlock was not resolved";
    EXPECT_EQ(a_err, "");
    EXPECT_NE(b_err.find("deadlock"), std::string::npos) << b_err;
    EXPECT_FALSE(b.InTransaction());
    a.Execute("COMMIT");
    EXPECT_EQ(Rows(a, "SELECT id, v FROM t ORDER BY id"), (std::vector<std::string>{"1|1", "2|1"}));
}

TEST(Transactions, ConcurrentSqlTransfersConserveTheTotal) {
    Database db(NewDir("txn_bank"));
    {
        Session s(&db);
        s.Execute("CREATE TABLE acct (id INTEGER PRIMARY KEY, bal INTEGER)");
        for (int i = 0; i < 8; i++) s.Execute("INSERT INTO acct VALUES (" + std::to_string(i) + ", 1000)");
    }
    std::atomic<int> bad_audits{0}, committed{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 6; t++) {
        ts.emplace_back([&, t] {
            Session s(&db);
            std::mt19937 rng(t);
            for (int i = 0; i < 25; i++) {
                int from = rng() % 8, to = (from + 1 + rng() % 7) % 8, amt = 1 + rng() % 20;
                while (true) {
                    try {
                        s.Execute("BEGIN");
                        s.Execute("UPDATE acct SET bal = bal - " + std::to_string(amt) + " WHERE id = " + std::to_string(from));
                        s.Execute("UPDATE acct SET bal = bal + " + std::to_string(amt) + " WHERE id = " + std::to_string(to));
                        s.Execute("COMMIT");
                        committed++;
                        break;
                    } catch (const DbError& e) {
                        if (std::string(e.what()).find("deadlock") == std::string::npos) throw;
                    }
                }
                if (i % 5 == 0) {  // concurrent auditor: the sum must never be seen mid-transfer
                    try {
                        if (Rows(s, "SELECT SUM(bal) FROM acct") != std::vector<std::string>{"8000"}) bad_audits++;
                    } catch (const DbError&) {  // deadlock victim: fine, just skip this audit
                    }
                }
            }
        });
    }
    for (auto& t : ts) t.join();
    Session s(&db);
    EXPECT_EQ(bad_audits.load(), 0);
    EXPECT_EQ(committed.load(), 150);
    EXPECT_EQ(Rows(s, "SELECT SUM(bal), COUNT(*) FROM acct"), (std::vector<std::string>{"8000|8"}));
}
