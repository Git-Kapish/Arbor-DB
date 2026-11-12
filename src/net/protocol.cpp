#include "net/protocol.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace arbordb {

std::string EscapeField(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\t') o += "\\t";
        else if (c == '\n') o += "\\n";
        else if (c == '\\') o += "\\\\";
        else o += c;
    }
    return o;
}

std::string UnescapeField(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            o += n == 't' ? '\t' : (n == 'n' ? '\n' : n);
        } else {
            o += s[i];
        }
    }
    return o;
}

std::string EncodeResults(const std::vector<QueryResult>& results) {
    std::string out = "OK\n";
    for (auto& r : results) {
        if (r.is_query) {
            out += "COLUMNS";
            for (auto& c : r.columns) out += "\t" + EscapeField(c);
            out += "\n";
            for (auto& row : r.rows) {
                out += "ROW";
                for (auto& v : row) out += "\t" + EscapeField(ValueToString(v));
                out += "\n";
            }
        } else {
            out += "MESSAGE\t" + EscapeField(r.message) + "\n";
        }
    }
    return out + "END\n";
}

std::string EncodeError(const std::string& message) { return "ERROR\t" + EscapeField(message) + "\nEND\n"; }

bool SendFrame(int fd, const std::string& payload) {
    uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
    std::string buf(reinterpret_cast<char*>(&len), 4);
    buf += payload;
    std::size_t done = 0;
    while (done < buf.size()) {
        ssize_t w = send(fd, buf.data() + done, buf.size() - done, MSG_NOSIGNAL);
        if (w <= 0) return false;
        done += w;
    }
    return true;
}

static bool ReadFull(int fd, char* p, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

bool RecvFrame(int fd, std::string* payload) {
    uint32_t len;
    if (!ReadFull(fd, reinterpret_cast<char*>(&len), 4)) return false;
    len = ntohl(len);
    if (len > (64u << 20)) return false;  // refuse absurd frames
    payload->assign(len, '\0');
    return len == 0 || ReadFull(fd, &(*payload)[0], len);
}

}  // namespace arbordb
