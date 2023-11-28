
#include <cassert>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <span>
#include <thread>

#include <sqlite3.h>

#include "log.hh"
#include "storage.hh"
#include "utils.hh"

namespace fs = std::filesystem;

namespace {
using namespace cheri;

class StorageBusyException : public StorageException {
 public:
  StorageBusyException(int rc, std::string message)
      : StorageException(rc, std::move(message)) {}
};

/**
 * RAII sqlite3 connection object.
 */
class DbConn {
public:
  DbConn(fs::path db_path) : db_path_{db_path} {
    LOG(kDebug) << "Open database conn @ " << db_path;
    constexpr int flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_CREATE;
    int rc = sqlite3_open_v2(db_path.c_str(), &conn_, flags, nullptr);
    if (rc != SQLITE_OK) {
      throw StorageException(rc, sqlite3_errstr(rc));
    }
  }

  ~DbConn() {
    LOG(kDebug) << "Close database conn @ " << db_path_;
    if (conn_ != nullptr)
      sqlite3_close_v2(conn_);
  }

  sqlite3 *Get() const { return conn_; }

private:
  fs::path db_path_;
  sqlite3 *conn_;
};

} // namespace

namespace cheri {

/**
 * Shared cursor state between the query object, the cursor and the
 * row views produced by the cursor.
 */
struct QueryCursorState {
  QueryCursorState(sqlite3_stmt *stmt) : active(true), index(0), stmt(stmt) {}

  /**
   * Advance the cursor to the next element.
   */
  bool Next() {
    if (!active) {
      LOG(kError) << "Attempt to advance detached cursor";
      throw std::runtime_error("Invalid cursor state");
    }
    if (stmt == nullptr) {
      LOG(kError) << "Cursor is active but has invalidated statement";
      throw std::runtime_error("Invalid cursor state");
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      index++;
      return true;
    } else if (rc == SQLITE_DONE) {
      index++;
      return false;
    } else if (rc == SQLITE_BUSY) {
      throw StorageBusyException(rc, sqlite3_errstr(rc));
    } else {
      throw StorageException(rc, sqlite3_errstr(rc));
    }
  }

  bool active;
  size_t index;
  sqlite3_stmt *stmt;
};

/*
 * Sql query result view implementation.
 */
SqlRowView::SqlRowView(std::weak_ptr<QueryCursorState> state, size_t version)
    : state_(state), version_(version) {}

std::shared_ptr<QueryCursorState> SqlRowView::LockState() const {
  auto state = state_.lock();
  if (!state || !state->active || state->index != version_) {
    LOG(kError) << "Attempted to use stale SqlRowView";
    throw std::out_of_range("Stale row view");
  }
  return state;
}

size_t SqlRowView::Size() const {
  auto state = LockState();
  return sqlite3_data_count(state->stmt);
}

std::vector<std::string> SqlRowView::Columns() const {
  auto state = LockState();
  std::vector<std::string> cols;
  for (size_t i = 0; i < sqlite3_data_count(state->stmt); ++i) {
    cols.emplace_back(sqlite3_column_name(state->stmt, i));
  }
  return cols;
}

SqlValue SqlRowView::ValueAt(std::string column) const {
  auto state = LockState();

  for (size_t i = 0; i < sqlite3_data_count(state->stmt); ++i) {
    std::string name(sqlite3_column_name(state->stmt, i));
    if (name != column) {
      continue;
    }

    switch (sqlite3_column_type(state->stmt, i)) {
    case SQLITE_INTEGER:
      return sqlite3_column_int64(state->stmt, i);
    case SQLITE_FLOAT:
      return sqlite3_column_double(state->stmt, i);
    case SQLITE_BLOB: {
      size_t bytes = sqlite3_column_bytes(state->stmt, i);
      const std::byte *blob =
          static_cast<const std::byte *>(sqlite3_column_blob(state->stmt, i));
      return std::vector<std::byte>(blob, blob + bytes);
    }
    case SQLITE_NULL:
      return std::monostate{};
    case SQLITE_TEXT:
      return std::string(
          reinterpret_cast<const char *>(sqlite3_column_text(state->stmt, i)));
    default:
      LOG(kError) << "Unhandled sqlite data type for column "
                  << std::quoted(name);
      throw std::runtime_error("sqlite3 API error");
    }
  }
  LOG(kError) << "Column '" << column << "' does not exist in query result";
  throw std::runtime_error("Invalid column name");
}

/*
 * Sql query cursor implementation.
 */
SqlQueryCursor::SqlQueryCursor(std::weak_ptr<QueryCursorState> state)
    : state_(state) {}

SqlQueryCursor::SqlQueryCursor(SqlQueryCursor &&other)
    : state_(std::move(other.state_)) {}

SqlQueryCursor::~SqlQueryCursor() {
  auto state = state_.lock();
  if (state) {
    // Invalidate the state to signal we are done.
    state->active = false;
    state->index++;
    state->stmt = nullptr;
  }
}

std::shared_ptr<QueryCursorState> SqlQueryCursor::LockState() {
  auto state = state_.lock();
  if (!state) {
    LOG(kError) << "Cursor is disassociated from query, use after free?";
    throw std::runtime_error("Invalid cursor state");
  }
  if (!state->active) {
    LOG(kError) << "Expired cursor can not run queries";
    throw std::runtime_error("Invalid cursor state");
  }
  return state;
}

void SqlQueryCursor::Run(SqlQueryCursor::SqlCallback callback) {
  auto state = LockState();

  LOG(kDebug) << "Exec query: " << sqlite3_sql(state->stmt);

  while (state->Next()) {
    try {
      if (callback && callback(SqlRowView(state, state->index)))
        break;
    } catch (const std::exception &ex) {
      LOG(kError) << "Failed to execute query callback: " << ex.what();
      throw;
    }
  }

  // Reset the cursor state on success
  Reset();
}

void SqlQueryCursor::Reset() {
  auto state = LockState();
  sqlite3_reset(state->stmt);
  sqlite3_clear_bindings(state->stmt);
  state->index++;
}

void SqlQueryCursor::CheckBind(std::shared_ptr<QueryCursorState> state,
                               int pos) {
  if (pos < 1 || pos > sqlite3_bind_parameter_count(state->stmt)) {
    LOG(kError) << "Can not bind query value at position " << pos
                << " for query " << sqlite3_sql(state->stmt);
    throw std::invalid_argument(
        "Can not bind query argument, invalid position");
  }
}

void SqlQueryCursor::BindAt(int pos, int64_t value) {
  auto state = LockState();
  CheckBind(state, pos);
  sqlite3_bind_int64(state->stmt, pos, value);
}

void SqlQueryCursor::BindAt(int pos, double value) {
  auto state = LockState();
  CheckBind(state, pos);
  sqlite3_bind_double(state->stmt, pos, value);
}

void SqlQueryCursor::BindAt(int pos, const std::string &value) {
  auto state = LockState();
  CheckBind(state, pos);
  sqlite3_bind_text(state->stmt, pos, value.data(), value.size(),
                    SQLITE_STATIC);
}

void SqlQueryCursor::BindAt(int pos, std::string &&value) {
  auto state = LockState();
  CheckBind(state, pos);
  sqlite3_bind_text(state->stmt, pos, value.data(), value.size(),
                    SQLITE_TRANSIENT);
}

void SqlQueryCursor::BindAt(int pos, const std::vector<std::byte> &value) {
  auto state = LockState();
  CheckBind(state, pos);
  sqlite3_bind_blob(state->stmt, pos, value.data(), value.size(),
                    SQLITE_STATIC);
}

void SqlQueryCursor::BindAt(int pos, std::vector<std::byte> &&value) {
  auto state = LockState();
  CheckBind(state, pos);
  sqlite3_bind_blob(state->stmt, pos, value.data(), value.size(),
                    SQLITE_TRANSIENT);
}

void SqlQueryCursor::BindAt(int pos, std::monostate _) {
  auto state = LockState();
  CheckBind(state, pos);
  sqlite3_bind_null(state->stmt, pos);
}

int SqlQueryCursor::BindPositionFor(std::shared_ptr<QueryCursorState> state,
                                    const std::string &param) {
  int index = sqlite3_bind_parameter_index(state->stmt, param.c_str());
  if (index == 0) {
    LOG(kError) << "Can not bind query value for " << std::quoted(param)
                << ", the parameter name does not exist in query "
                << sqlite3_sql(state->stmt) << " Available parameter names are "
                << Join(GetBindNames(state));
    throw std::invalid_argument("Can not bind query argument, invalid name");
  }
  return index;
}

std::vector<std::string>
SqlQueryCursor::GetBindNames(std::shared_ptr<QueryCursorState> state) {
  std::vector<std::string> names;
  for (int i = 1; i < sqlite3_bind_parameter_count(state->stmt); i++) {
    names.emplace_back(sqlite3_bind_parameter_name(state->stmt, i));
  }
  return names;
}

/**
 * Wrapper for a database query cursor.
 *
 * This satisfies the LegacyInputIterator concept.
 */
class SqlQueryImpl : public SqlQuery {
public:
  SqlQueryImpl(std::shared_ptr<DbConn> conn, std::string query)
      : SqlQuery(std::move(query)), conn_(conn) {
    int rc = sqlite3_prepare_v2(conn->Get(), query_.c_str(), query_.size(),
                                &stmt_, nullptr);
    if (rc != SQLITE_OK) {
      LOG(kError) << "Could not compile SQL query: " << query_
                  << " error: " << sqlite3_errmsg(conn->Get());
      throw StorageException(rc, sqlite3_errstr(rc));
    }
  }

  ~SqlQueryImpl() {
    if (cursor_) {
      cursor_->active = false;
      cursor_->stmt = nullptr;
    }
    sqlite3_finalize(stmt_);
  }

  SqlQueryCursor TakeCursor() override {
    if (cursor_ && cursor_->active) {
      // Busy cursor state, fail
      LOG(kError) << "Cursor is busy for query " << this->query_;
      throw std::runtime_error("Attempted to acquire busy cursor");
    }
    cursor_ = std::make_shared<QueryCursorState>(stmt_);
    return SqlQueryCursor(cursor_);
  }

private:
  sqlite3_stmt *stmt_;
  std::shared_ptr<QueryCursorState> cursor_;
  std::shared_ptr<DbConn> conn_;
};

/**
 * Internal implementation of the storage manager.
 */
class StorageManager::StorageManagerImpl {
public:
  StorageManagerImpl(fs::path db_path) {
    db_ = std::make_shared<DbConn>(db_path);
    active_transaction_ = false;

    int rc = 0;
    /*
     * Always use WAL journaling, this has better concurrency properties.
     * Just spin here to avoid complicated locking across storage managers.
     */
    rc = sqlite3_exec(db_->Get(), "PRAGMA journal_mode=WAL;", nullptr,
                            nullptr, nullptr);
    if (rc != SQLITE_OK) {
      LOG(kError) << "Failed to set WAL journal mode";
      throw StorageException(rc, sqlite3_errstr(rc));
    }

    /* Initialize the database if necessary */
    auto make_jobs = Sql("CREATE TABLE IF NOT EXISTS jobs ("
                         "id int NOT NULL PRIMARY KEY,"
                         "name varchar(255) NOT NULL)");
    make_jobs->TakeCursor().Run();
  }

  std::unique_ptr<SqlQuery> Sql(std::string query) {
    return std::make_unique<SqlQueryImpl>(db_, std::move(query));
  }

  void Transaction(std::function<void()> fn) {
    if (active_transaction_) {
      LOG(kError) << "Attempted to begin transaction while another "
          "transaction is active. Nested transactions not supported";
      throw std::runtime_error("Cannot nest transactions");
    }
    active_transaction_ = true;
    try {
      SqlFastExec("BEGIN TRANSACTION");
      fn();
      SqlFastExec("COMMIT TRANSACTION");
      active_transaction_ = false;
    } catch (const std::exception &ex) {
      SqlFastExec("ROLLBACK TRANSACTION");
      active_transaction_ = false;
      throw;
    }
  }

private:
  /**
   * Internal helper to execute a query string.
   * This may be used inside a transaction. If the database is busy, this
   * raises a StorageBusyException, which must be handled.
   */
  void SqlFastExec(std::string query) {
    int rc = sqlite3_exec(db_->Get(), query.c_str(), nullptr, nullptr, nullptr);
    if (rc == SQLITE_BUSY) {
      throw StorageBusyException(rc, sqlite3_errstr(rc));
    } else if (rc != SQLITE_OK) {
      throw StorageException(rc, sqlite3_errstr(rc));
    }
  }

  std::shared_ptr<DbConn> db_;
  bool active_transaction_;
};

StorageManager::StorageManager(fs::path db_path)
    : pimpl_{std::make_unique<StorageManagerImpl>(db_path)} {}

StorageManager::~StorageManager() = default;

std::unique_ptr<SqlQuery> StorageManager::Sql(std::string &query) {
  return pimpl_->Sql(query);
}

std::unique_ptr<SqlQuery> StorageManager::Sql(std::string &&query) {
  return pimpl_->Sql(std::forward<std::string>(query));
}

void StorageManager::Transaction(SqlTransactionFn fn) {
  pimpl_->Transaction(std::bind(fn, this));
}

} /* namespace cheri */
