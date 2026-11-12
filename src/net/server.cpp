#include "net/server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace arbordb {

int Server::Start(int port) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return -1;
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // local only: there is no authentication
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listen_fd_, 16) != 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }
    socklen_t len = sizeof(addr);
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    acceptor_ = std::thread([this] { AcceptLoop(); });
    return ntohs(addr.sin_port);
}

void Server::AcceptLoop() {
    while (!stopping_) {
        int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) break;  // listener closed by Stop()
        std::lock_guard<std::mutex> g(mu_);
        open_fds_.push_back(fd);
        workers_.emplace_back([this, fd] { Serve(fd); });
    }
}

void Server::Serve(int fd) {
    Session session(db_);
    std::string sql;
    while (RecvFrame(fd, &sql)) {
        std::string reply;
        try {
            reply = EncodeResults(session.ExecuteScript(sql));
        } catch (const std::exception& e) {
            reply = EncodeError(e.what());
        }
        if (!SendFrame(fd, reply)) break;
    }
    close(fd);  // Session's destructor rolls back any open transaction
}

void Server::Stop() {
    if (stopping_.exchange(true)) return;
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
    }
    if (acceptor_.joinable()) acceptor_.join();
    std::list<std::thread> ws;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (int fd : open_fds_) shutdown(fd, SHUT_RDWR);  // unblock workers stuck in recv
        ws.swap(workers_);
    }
    for (auto& t : ws) t.join();
}

Client::~Client() {
    if (fd_ >= 0) close(fd_);
}

bool Client::Connect(const std::string& host, int port) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) return false;
    return connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
}

bool Client::Request(const std::string& sql, std::string* response) {
    return SendFrame(fd_, sql) && RecvFrame(fd_, response);
}

}  // namespace arbordb
