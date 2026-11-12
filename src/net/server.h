#pragma once

#include <atomic>
#include <list>
#include <mutex>
#include <thread>

#include "net/protocol.h"

namespace arbordb {

// Thread-per-connection TCP server over raw POSIX sockets. Each connection owns one Session, so
// BEGIN/COMMIT work across requests on the same connection and different connections are different
// transactions (isolated by the lock manager).
class Server {
public:
    explicit Server(Database* db) : db_(db) {}
    ~Server() { Stop(); }
    // port 0 picks a free port. Returns the bound port, or -1 on failure.
    int Start(int port);
    void Stop();

private:
    void AcceptLoop();
    void Serve(int fd);

    Database* db_;
    int listen_fd_{-1};
    std::atomic<bool> stopping_{false};
    std::thread acceptor_;
    std::mutex mu_;
    std::list<std::thread> workers_;
    std::list<int> open_fds_;
};

// Minimal blocking client used by the CLI and the tests.
class Client {
public:
    ~Client();
    bool Connect(const std::string& host, int port);
    // Sends the SQL, returns the raw response payload (see protocol.h); false on I/O failure.
    bool Request(const std::string& sql, std::string* response);

private:
    int fd_{-1};
};

}  // namespace arbordb
