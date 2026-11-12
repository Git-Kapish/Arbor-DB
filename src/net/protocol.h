#pragma once

#include <string>

#include "db/database.h"

namespace arbordb {

// Wire protocol (custom, deliberately not Postgres-compatible): a stream of frames in each direction,
//   frame := [u32 big-endian payload length][payload bytes]
// A request frame's payload is SQL text (one or more ';'-separated statements).
// A response frame's payload is text, one line per row, fields separated by TAB with \t \n \\ escaped:
//   first line  "OK" or "ERROR\t<message>"
//   for each statement that produced a result set:  "COLUMNS\t<c1>\t<c2>..." then "ROW\t<v1>\t<v2>..." lines
//   for each statement without a result set:        "MESSAGE\t<text>"
//   "END" terminates the payload.
// The server turns each request into a Command (parsed statements) and runs it on the connection's Session.
std::string EncodeResults(const std::vector<QueryResult>& results);
std::string EncodeError(const std::string& message);
std::string EscapeField(const std::string& s);
std::string UnescapeField(const std::string& s);

bool SendFrame(int fd, const std::string& payload);
bool RecvFrame(int fd, std::string* payload);  // false on EOF / error

}  // namespace arbordb
