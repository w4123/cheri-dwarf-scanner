
#pragma once

#include <filesystem>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include <QLoggingCategory>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace cheri {

Q_DECLARE_LOGGING_CATEGORY(storage)

class DBError : public std::runtime_error {
public:
  DBError(QSqlError err)
      : std::runtime_error(
            (err.nativeErrorCode() + ": " + err.text()).toStdString()),
        error(err) {}

  QSqlError error;
};

/**
 * Manage database interface for a scraper.
 */
class StorageManager {
public:
  StorageManager(std::filesystem::path db_path);
  StorageManager(const StorageManager &s) = delete;

  /**
   * Get the database connection instance for the current worker thread.
   * Using different connections for each thread should allow to run
   * sqlite in the multithreaded mode.
   * See https://www.sqlite.org/threadsafe.html
   */
  QSqlDatabase &getWorkerStorage();

  QSqlQuery query(const std::string &expr);
  QSqlQuery prepare(const std::string &expr);
  void transaction(std::function<void(StorageManager &sm)> fn);

private:
  std::mutex transaction_mutex_;
  std::filesystem::path db_path_;
};

} /* namespace cheri */
