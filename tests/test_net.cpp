#include <gtest/gtest.h>
#include <unistd.h>

#include <thread>

#include "net/server.h"

using namespace arbordb;

namespace {
struct Fixture {
    Database db;
    Server server;
    int port;
    explicit Fixture(const std::string& name)
        : db([&] {
              std::string d = "/tmp/arbor_net_" + std::to_string(getpid()) + "_" + name;
              EXPECT_EQ(system(("rm -rf " + d).c_str()), 0);
              return d;
          }()),
          server(&db) {
        port = server.Start(0);
    }
};
std::string Ask(Client& c, const std::string& sql) {
    std::string r;
    EXPECT_TRUE(c.Request(sql, &r));
    return r;
}
}  // namespace

TEST(Net, FieldEscapingRoundTrips) {
    for (std::string s : {"plain", "tab\there", "new\nline", "back\\slash", "\\t literal", ""})
        EXPECT_EQ(UnescapeField(EscapeField(s)), s);
}

TEST(Net, QueryRoundTripOverTcp) {
    Fixture f("rt");
    ASSERT_GT(f.port, 0);
    Client c;
    ASSERT_TRUE(c.Connect("127.0.0.1", f.port));
    EXPECT_EQ(Ask(c, "CREATE TABLE t (id INTEGER PRIMARY KEY, s VARCHAR(20));"), "OK\nMESSAGE\tCREATE TABLE\nEND\n");
    EXPECT_EQ(Ask(c, "INSERT INTO t VALUES (1, 'a\tb'), (2, NULL);"), "OK\nMESSAGE\t2 rows affected\nEND\n");
    EXPECT_EQ(Ask(c, "SELECT id, s FROM t ORDER BY id;"), "OK\nCOLUMNS\tid\ts\nROW\t1\ta\\tb\nROW\t2\tNULL\nEND\n");
    EXPECT_EQ(Ask(c, "SELECT COUNT(*) FROM t; SELECT MAX(id) FROM t;"),
              "OK\nCOLUMNS\tCOUNT(*)\nROW\t2\nCOLUMNS\tMAX(t.id)\nROW\t2\nEND\n");
}

TEST(Net, ErrorsComeBackAsErrorFramesAndTheConnectionSurvives) {
    Fixture f("err");
    Client c;
    ASSERT_TRUE(c.Connect("127.0.0.1", f.port));
    std::string r = Ask(c, "SELECT * FROM nothing;");
    EXPECT_EQ(r.rfind("ERROR\t", 0), 0u) << r;
    EXPECT_NE(r.find("no such table"), std::string::npos);
    EXPECT_EQ(Ask(c, "SELEC 1;").rfind("ERROR\t", 0), 0u);
    EXPECT_EQ(Ask(c, "CREATE TABLE ok (a INTEGER);"), "OK\nMESSAGE\tCREATE TABLE\nEND\n");
}

TEST(Net, TransactionStateIsPerConnection) {
    Fixture f("txn");
    Client a, b;
    ASSERT_TRUE(a.Connect("127.0.0.1", f.port));
    ASSERT_TRUE(b.Connect("127.0.0.1", f.port));
    Ask(a, "CREATE TABLE t (id INTEGER, v INTEGER); INSERT INTO t VALUES (1, 1);");
    Ask(a, "BEGIN; UPDATE t SET v = 2;");
    Ask(a, "ROLLBACK;");
    EXPECT_EQ(Ask(b, "SELECT v FROM t;"), "OK\nCOLUMNS\tv\nROW\t1\nEND\n");
    EXPECT_NE(Ask(b, "COMMIT;").find("no transaction"), std::string::npos);  // b never began one
    Ask(a, "BEGIN; UPDATE t SET v = 5;");
    a = Client();  // disconnect without committing (moves would be nicer, but this closes the socket)
}

TEST(Net, ManyClientsInParallel) {
    Fixture f("par");
    {
        Client c;
        ASSERT_TRUE(c.Connect("127.0.0.1", f.port));
        Ask(c, "CREATE TABLE t (id INTEGER PRIMARY KEY, who INTEGER);");
    }
    std::vector<std::thread> ts;
    for (int t = 0; t < 6; t++) {
        ts.emplace_back([&, t] {
            Client c;
            ASSERT_TRUE(c.Connect("127.0.0.1", f.port));
            for (int i = 0; i < 20; i++) {
                std::string r = Ask(c, "INSERT INTO t VALUES (" + std::to_string(t * 100 + i) + ", " + std::to_string(t) + ");");
                EXPECT_EQ(r, "OK\nMESSAGE\t1 row affected\nEND\n");
            }
        });
    }
    for (auto& t : ts) t.join();
    Client c;
    ASSERT_TRUE(c.Connect("127.0.0.1", f.port));
    EXPECT_EQ(Ask(c, "SELECT COUNT(*) FROM t;"), "OK\nCOLUMNS\tCOUNT(*)\nROW\t120\nEND\n");
}
