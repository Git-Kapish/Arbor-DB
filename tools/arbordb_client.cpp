// arbordb_client <host> <port>    reads statements (terminated by ';') from stdin, prints results
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>

#include "net/server.h"

using namespace arbordb;

static void Print(const std::string& payload) {
    std::istringstream in(payload);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::size_t start = 0, tab;
        while ((tab = line.find('\t', start)) != std::string::npos) {
            f.push_back(UnescapeField(line.substr(start, tab - start)));
            start = tab + 1;
        }
        f.push_back(UnescapeField(line.substr(start)));
        if (f[0] == "ERROR") std::printf("ERROR: %s\n", f.size() > 1 ? f[1].c_str() : "");
        else if (f[0] == "MESSAGE") std::printf("%s\n", f.size() > 1 ? f[1].c_str() : "");
        else if (f[0] == "COLUMNS" || f[0] == "ROW")
            for (std::size_t i = 1; i < f.size(); i++) std::printf("%s%s", f[i].c_str(), i + 1 < f.size() ? " | " : "\n");
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
        return 2;
    }
    Client c;
    if (!c.Connect(argv[1], std::atoi(argv[2]))) {
        std::fprintf(stderr, "cannot connect\n");
        return 1;
    }
    std::string buf, line;
    while (std::getline(std::cin, line)) {
        buf += line + "\n";
        if (line.find(';') == std::string::npos) continue;
        std::string reply;
        if (!c.Request(buf, &reply)) {
            std::fprintf(stderr, "connection lost\n");
            return 1;
        }
        Print(reply);
        buf.clear();
    }
    return 0;
}
