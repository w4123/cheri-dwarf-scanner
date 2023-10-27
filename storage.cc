
#include <iterator>
#include <map>
#include <optional>
#include <span>

#include <sqlite3.h>

#include "log.hh"
#include "storage.hh"

namespace fs = std::filesystem;

namespace {
using namespace cheri;

/**
 * RAII sqlite3 connection object.
 */
class DbConn {
public:
  DbConn(fs::path db_path) : db_path_{db_path} {
    LOG(kDebug) << "Open database @ " << db_path;
    constexpr int flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_CREATE;
    int rc = sqlite3_open_v2(db_path.c_str(), &conn_, flags, nullptr);
    if (rc != SQLITE_OK) {
      throw StorageException(rc, sqlite3_errstr(rc));
    }
  }

  ~DbConn() {
    LOG(kDebug) << "Close database @ " << db_path_;
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
 * Internal query state.
 * This is shared between the row views and the query.
 */
struct QueryCursor {
  QueryCursor(sqlite3_stmt *stmt)
      : stmt(stmt), version(0) {}

  ~QueryCursor() {
    sqlite3_finalize(stmt);
  }

  // atomic?
  size_t version;
  sqlite3_stmt *stmt;
};

SqlRowView::SqlRowView(std::weak_ptr<QueryCursor> cursor, size_t version)
    : cursor_(cursor), version_(version) {}

size_t SqlRowView::Size() const {
  std::shared_ptr<QueryCursor> qc = cursor_.lock();
  if (!qc || qc->version != version_) {
    LOG(kError) << "Attempted to use stale SqlRowView";
    throw std::out_of_range("Stale row view");
  }
  return sqlite3_data_count(qc->stmt);
}

std::vector<std::string> SqlRowView::Columns() const {
  std::shared_ptr<QueryCursor> qc = cursor_.lock();
  if (!qc || qc->version != version_) {
    LOG(kError) << "Attempted to use stale SqlRowView";
    throw std::out_of_range("Stale row view");
  }

  std::vector<std::string> cols;
  for (size_t i = 0; i < sqlite3_data_count(qc->stmt); ++i) {
    cols.emplace_back(sqlite3_column_name(qc->stmt, i));
  }
  return cols;
}

SqlRowView::Value SqlRowView::ValueAt(std::string column) const {
  std::shared_ptr<QueryCursor> qc = cursor_.lock();
  if (!qc || qc->version != version_) {
    LOG(kError) << "Attempted to use stale SqlRowView";
    throw std::out_of_range("Stale row view");
  }

  for (size_t i = 0; i < sqlite3_data_count(qc->stmt); ++i) {
    std::string name(sqlite3_column_name(qc->stmt, i));
    switch (sqlite3_column_type(qc->stmt, i)) {
      case SQLITE_INTEGER:
        return sqlite3_column_int64(qc->stmt, i);
      case SQLITE_FLOAT:
        return sqlite3_column_double(qc->stmt, i);
      case SQLITE_BLOB: {
        size_t bytes = sqlite3_column_bytes(qc->stmt, i);
        const std::byte *blob = static_cast<const std::byte *>(
            sqlite3_column_blob(qc->stmt, i));
        return std::vector<std::byte>(blob, blob + bytes);
      }
      case SQLITE_NULL:
        return std::monostate{};
      case SQLITE_TEXT:
        return std::string(reinterpret_cast<const char *>(
            sqlite3_column_text(qc->stmt, i)));
      default:
        LOG(kError) << "Unhandled sqlite data type for column " <<
            std::quoted(name);
        throw std::runtime_error("sqlite3 API error");
    }
  }
  // pacify compiler
  return std::monostate{};
}

/**
 * Wrapper for a database query cursor.
 *
 * This satisfies the LegacyInputIterator concept.
 */
class SqlQueryImpl : public SqlQuery {
 public:

  SqlQueryImpl(std::shared_ptr<DbConn> conn, std::string query)
      : SqlQuery(query), conn_(conn) {
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(conn->Get(), query.c_str(), query.size(), &stmt, nullptr);
    if (rc != SQLITE_OK) {
      LOG(kError) << "Could not compile SQL query: " << query << " error: " <<
          sqlite3_errmsg(conn->Get());
      throw StorageException(rc, sqlite3_errstr(rc));
    }
    cursor_ = std::make_shared<QueryCursor>(stmt);
  }

  /**
   * Build a view token for the current cursor.
   */
  SqlRowView GetRow() {
    return SqlRowView(cursor_, cursor_->version);
  }

  void Run(SqlCallback callback) override {
    while (Next()) {
      if (callback && callback(GetRow()))
        break;
    }
  }

 private:
  /**
   * Advance the cursor state and implicitly invalidate all older row views.
   * Returns true if there is more data to read.
   */
  bool Next() {
    int rc = sqlite3_step(cursor_->stmt);
    if (rc == SQLITE_ROW) {
      cursor_->version++;
      return true;
    } else if (rc == SQLITE_DONE) {
      cursor_->version++;
      return false;
    } else {
      LOG(kError) << "Failed to execute sql query: " << query_;
      throw StorageException(rc, sqlite3_errstr(rc));
    }
  }

  std::shared_ptr<QueryCursor> cursor_;
  std::shared_ptr<DbConn> conn_;
};

/**
 * Internal implementation of the storage manager.
 */
class StorageManager::StorageManagerImpl {
 public:
  StorageManagerImpl(fs::path db_path) {
    db_ = std::make_shared<DbConn>(db_path);

    /* Initialize the database if necessary */
    Sql("CREATE TABLE IF NOT EXISTS Jobs ("
        "id int NOT NULL PRIMARY KEY,"
        "name varchar(255) NOT NULL)");
  }

  std::unique_ptr<SqlQuery> Sql(std::string query) {
    return std::make_unique<SqlQueryImpl>(db_, std::forward<std::string>(query));
  }

private:
  std::shared_ptr<DbConn> db_;
};

StorageManager::StorageManager(fs::path db_path)
    : pimpl_{std::make_unique<StorageManagerImpl>(db_path)} {}

StorageManager::~StorageManager() = default;

std::unique_ptr<SqlQuery> StorageManager::Sql(std::string query) {
  return pimpl_->Sql(query);
}

void StorageManager::SqlExec(std::string query,
                             SqlQuery::SqlCallback callback) {
  auto q = pimpl_->Sql(query);
  q->Run(callback);
}

} /* namespace cheri */
