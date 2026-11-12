// arbordb_server <data_dir> [port]   (default port 5433, listens on 127.0.0.1 only)
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#include "net/server.h"

using namespace arbordb;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <data_dir> [port]\n", argv[0]);
        return 2;
    }
    Database db(argv[1]);
    Server server(&db);
    int port = server.Start(argc > 2 ? std::atoi(argv[2]) : 5433);
    if (port < 0) {
        std::fprintf(stderr, "cannot listen\n");
        return 1;
    }
    std::printf("arbordb listening on 127.0.0.1:%d\n", port);
    std::fflush(stdout);
    for (;;) pause();  // stop with SIGINT/SIGTERM; kill -9 is survivable by design
}
