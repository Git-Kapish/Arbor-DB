// arbordb <data_dir>            interactive shell / reads SQL from stdin
// arbordb <data_dir> -c "SQL"   run the given statements and exit
// Statements end with ';'. Meta commands: .tables  .exit
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>

#include "db/database.h"

using namespace arbordb;

namespace {
void PrintResult(const QueryResult& r) {
    if (!r.is_query) {
        std::printf("%s\n", r.message.c_str());
        return;
    }
    std::vector<std::size_t> w(r.columns.size());
    for (std::size_t i = 0; i < w.size(); i++) w[i] = r.columns[i].size();
    std::vector<std::vector<std::string>> cells;
    for (auto& row : r.rows) {
        cells.emplace_back();
        for (std::size_t i = 0; i < row.size(); i++) {
            cells.back().push_back(ValueToString(row[i]));
            w[i] = std::max(w[i], cells.back().back().size());
        }
    }
    auto line = [&](const std::vector<std::string>& v) {
        for (std::size_t i = 0; i < v.size(); i++) std::printf("%s%-*s", i ? " | " : "", static_cast<int>(w[i]), v[i].c_str());
        std::printf("\n");
    };
    line(r.columns);
    for (std::size_t i = 0; i < w.size(); i++) std::printf("%s%s", i ? "-+-" : "", std::string(w[i], '-').c_str());
    std::printf("\n");
    for (auto& c : cells) line(c);
    std::printf("(%zu row%s)\n", r.rows.size(), r.rows.size() == 1 ? "" : "s");
}

void Run(Session& s, const std::string& sql) {
    try {
        for (auto& r : s.ExecuteScript(sql)) PrintResult(r);
    } catch (const DbError& e) {
        std::printf("ERROR: %s\n", e.what());
    } catch (const std::exception& e) {
        std::printf("INTERNAL ERROR: %s\n", e.what());
    }
    std::fflush(stdout);  // scripts (and the crash test) read our output as it is produced
}

// Index just past the last ';' that is not inside a string literal, or npos.
std::size_t LastTerminator(const std::string& s) {
    bool in_str = false;
    std::size_t last = std::string::npos;
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\'') in_str = !in_str;
        else if (s[i] == ';' && !in_str) last = i + 1;
    }
    return last;
}
}  // namespace

int main(int argc, char** argv) {
    // Pull out --mvcc wherever it appears; the rest is <data_dir> [-c "SQL"].
    DatabaseOptions opts;
    std::vector<char*> args;
    for (int i = 0; i < argc; i++) {
        if (std::string(argv[i]) == "--mvcc") opts.concurrency = ConcurrencyMode::kMVCC;
        else args.push_back(argv[i]);
    }
    argc = static_cast<int>(args.size());
    argv = args.data();
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <data_dir> [--mvcc] [-c \"SQL\"]\n", argv[0]);
        return 2;
    }
    try {
        Database db(argv[1], opts);
        if (db.LastRecovery().txns_undone || db.LastRecovery().records_redone)
            std::printf("-- recovery: redid %zu page changes, rolled back %zu unfinished transaction(s)\n",
                        db.LastRecovery().records_redone, db.LastRecovery().txns_undone);
        Session session(&db);
        if (argc >= 4 && std::string(argv[2]) == "-c") {
            Run(session, argv[3]);
            return 0;
        }
        bool tty = isatty(0);
        std::string buf, line;
        if (tty) std::printf("arbordb> ");
        std::fflush(stdout);
        while (std::getline(std::cin, line)) {
            if (buf.empty() && (line == ".exit" || line == ".quit")) break;
            if (buf.empty() && line == ".tables") {
                for (auto* t : db.GetCatalog().Tables()) std::printf("%s\n", t->name.c_str());
                std::fflush(stdout);
                continue;
            }
            buf += line + "\n";
            std::size_t end = LastTerminator(buf);
            if (end != std::string::npos) {
                Run(session, buf.substr(0, end));
                buf = buf.substr(end);
            }
            if (tty) std::printf(buf.find_first_not_of(" \n") == std::string::npos ? "arbordb> " : "     ...> ");
            std::fflush(stdout);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
