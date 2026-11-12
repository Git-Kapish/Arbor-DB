#pragma once

#include <stdexcept>
#include <string>

namespace arbordb {

// Everything the SQL layer reports to a user derives from DbError.
struct DbError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct ParseError : DbError {
    using DbError::DbError;
};
struct BindError : DbError {
    using DbError::DbError;
};
struct ExecutionError : DbError {
    using DbError::DbError;
};

}  // namespace arbordb
