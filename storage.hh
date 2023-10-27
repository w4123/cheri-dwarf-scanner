#pragma once

#include <concepts>
#include <functional>
#include <filesystem>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>

#include "log.hh"

namespace cheri {

/**
 * Wrapper around storage errors.
 */
class StorageException : public std::runtime_error {
public:
  StorageException(int rc, std::string message)
      : std::runtime_error(std::format("[{:d}] {}", rc, message)), code{rc} {}

  const int code;
};

/**
 * Internal shared state of the query.
 * This is opaque to the interface.
 */
struct QueryCursor;

/**
 * Helper object that wraps access to an query result row.
 * Note that this provides readonly access with borrowed types.
 */
class SqlRowView {
 public:
  using Value = std::variant<
   int64_t,
   double,
   std::string,
   std::vector<std::byte>,
   std::monostate>;

  SqlRowView() = default;
  SqlRowView(std::weak_ptr<QueryCursor> cursor, size_t version);
  SqlRowView(const SqlRowView &other) = default;
  ~SqlRowView() = default;
  size_t Size() const;
  std::vector<std::string> Columns() const;
  Value ValueAt(std::string column) const;

  template<typename T>
  std::optional<T> At(std::string column) const {
    Value v = ValueAt(column);
    if (std::holds_alternative<T>(v) &&
        !std::holds_alternative<std::monostate>(v)) {
      return std::get<T>(v);
    }
    return std::nullopt;
  }

  template<typename T>
  std::optional<T> ExtractAt(std::string column) const {
    Value v = ValueAt(column);
    auto extract = std::visit([](auto &&arg) -> std::optional<T> {
      using A = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<A, std::monostate>) {
        return std::nullopt;
      } else if constexpr (std::is_convertible_v<A, T>) {
        return std::make_optional<T>(arg);
      } else {
        return std::nullopt;
      }
    }, v);
    return extract;
  }

 private:
  std::weak_ptr<QueryCursor> cursor_;
  size_t version_;
};

/**
 * Active SQL query object
 */
class SqlQuery {
 public:
  using SqlCallback = std::function<bool(SqlRowView view)>;

  SqlQuery(const std::string query) : query_(query) {}
  SqlQuery(std::string &&query) : query_(std::forward<std::string>(query)) {}
  SqlQuery(const SqlQuery &other) = delete;
  SqlQuery(SqlQuery &&other) = default;
  virtual ~SqlQuery() = default;

  /**
   * Execute the query and run the given callback on each row returned.
   */
  virtual void Run(SqlCallback callback) = 0;

 protected:
  std::string query_;
};

/**
 * Manage persistence of data in a tabular format.
 * This uses the pimpl idiom to decouple the sqlite dependencies.
 */
class StorageManager {
public:
  StorageManager(std::filesystem::path db_path);
  StorageManager(StorageManager &other) = delete;
  ~StorageManager();

  std::unique_ptr<SqlQuery> Sql(std::string query);
  void SqlExec(std::string query, SqlQuery::SqlCallback callback);
  void SqlExec(std::string query) {
    SqlExec(query, nullptr);
  }

private:
  class StorageManagerImpl;
  std::unique_ptr<StorageManagerImpl> pimpl_;
};

} /* namespace cheri */
