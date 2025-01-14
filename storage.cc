#include <cassert>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <QDebug>
#include <QSqlError>
#include <QtLogging>

#include "storage.hh"
#include "utils.hh"

namespace fs = std::filesystem;

namespace {

using namespace cheri;

std::once_flag db_init;
std::condition_variable db_ready_cond;
std::mutex db_ready_mut;
bool db_ready = false;

/**
 * Helper to execute a query and fail with an exception.
 */
void execQuery(QSqlQuery &q) {
  qCDebug(storage) << "Executing" << q.lastQuery();
  if (!q.exec()) {
    qCritical() << "Failed to execute query" << q.lastQuery()
                << "reason:" << q.lastError().text();
    throw cheri::DBError(q.lastError());
  }
}

/**
 * See execQuery(QSqlQuery).
 */
QSqlQuery execQuery(const QSqlDatabase &db, const std::string &sql_expr) {
  QSqlQuery q(db);
  qCDebug(storage) << "Executing" << sql_expr;
  if (!q.exec(QString::fromStdString(sql_expr))) {
    qCritical() << "Failed to execute query" << sql_expr
                << "reason:" << q.lastError().text();
    throw cheri::DBError(db.lastError());
  }
  return q;
}

/**
 * Per-worker thread database connection initializer.
 */
class WorkerDB {
public:
  WorkerDB(fs::path db_path) {
    std::ostringstream ss;
    auto tid = std::this_thread::get_id();
    ss << tid;
    conn_uuid_ = QString::fromStdString(ss.str());
    qDebug() << "Open database conn" << conn_uuid_;
    db_ = QSqlDatabase::addDatabase("QSQLITE", conn_uuid_);
    db_.setDatabaseName(QString::fromStdString(db_path));
    if (!db_.open()) {
      qCritical() << "Failed to open database connection to" << db_path
                  << "reason:" << db_.lastError();
      auto errmsg = db_.lastError().text();
      throw std::runtime_error(errmsg.toStdString());
    }
    assert(db_.isValid() && db_.isOpen() && "Unexpected connection state");

    qDebug() << "Initialize database";
    std::call_once(db_init, [this]() {
      execQuery(db_, "PRAGMA journal_mode=WAL");
      {
        std::unique_lock lk(db_ready_mut);
        db_ready = true;
      }
      db_ready_cond.notify_all();
    });
    std::unique_lock lk(db_ready_mut);
    db_ready_cond.wait(lk, [] { return db_ready; });

    qDebug() << "Database is ready for operations on" << conn_uuid_;
  }

  ~WorkerDB() { db_.close(); }

  QSqlDatabase &getDatabase() { return db_; }

private:
  QString conn_uuid_;
  QSqlDatabase db_;
};

} // namespace

namespace cheri {

Q_LOGGING_CATEGORY(storage, "storage")

StorageManager::StorageManager(fs::path db_path) : db_path_(db_path) {}

QSqlDatabase &StorageManager::getWorkerStorage() {
  static thread_local WorkerDB worker_db(db_path_);

  return worker_db.getDatabase();
}

QSqlQuery StorageManager::query(const std::string &expr) {
  return execQuery(getWorkerStorage(), expr);
}

QSqlQuery StorageManager::prepare(const std::string &expr) {
  QSqlQuery q(getWorkerStorage());
  q.prepare(QString::fromStdString(expr));

  return q;
}

void StorageManager::transaction(std::function<void(StorageManager &sm)> fn) {
  try {
    execQuery(getWorkerStorage(), "BEGIN TRANSACTION");
    fn(*this);
    execQuery(getWorkerStorage(), "COMMIT TRANSACTION");
  } catch (const std::exception &ex) {
    execQuery(getWorkerStorage(), "ROLLBACK TRANSACTION");
    throw;
  }
}

} /* namespace cheri */
