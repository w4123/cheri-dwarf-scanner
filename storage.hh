#pragma once

#include <concepts>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include "bit_flag_enum.hh"
#include "log.hh"

namespace cheri {

namespace impl {
/**
 * Optional check helper
 */
template <typename T> struct is_optional {
  static constexpr bool value = false;
};

template <typename V> struct is_optional<std::optional<V>> {
  static constexpr bool value = true;
};

} /* namespace impl */

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
 * Opaque shared query cursor state.
 */
struct QueryCursorState;

using SqlValue = std::variant<int64_t, double, std::string,
                              std::vector<std::byte>, std::monostate>;

/**
 * Helper object that wraps access to an query result row.
 * Note that this provides readonly access with borrowed types.
 */
class SqlRowView {
public:
  SqlRowView(std::weak_ptr<QueryCursorState> state, size_t version);
  SqlRowView(const SqlRowView &other) = default;

  size_t Size() const;
  std::vector<std::string> Columns() const;
  SqlValue ValueAt(std::string column) const;

  template <typename T> std::optional<T> At(std::string column) const {
    SqlValue v = ValueAt(column);
    if (std::holds_alternative<T>(v) &&
        !std::holds_alternative<std::monostate>(v)) {
      return std::get<T>(v);
    }
    return std::nullopt;
  }

  /**
   * Attempt to coerce a column value to a given type.
   * This may fail if the column value can not be converted
   * to the type.
   */
  template <typename T> T FetchAs(std::string column) const {
    SqlValue v = ValueAt(column);

    auto extract = std::visit(
        [&column](auto &&arg) -> T {
          using A = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<A, std::monostate>) {
            if constexpr (impl::is_optional<T>::value) {
              return std::nullopt;
            } else {
              throw std::runtime_error(std::format(
                  "can not coerce NULL value for column '{}'", column));
            }
          } else if constexpr (std::convertible_to<A, T> ||
                               (BitFlagEnum<T> && std::is_integral_v<A>)) {
            return static_cast<T>(arg);
          } else {
            if constexpr (std::is_same_v<A, std::vector<std::byte>>) {
              throw std::runtime_error(std::format(
                  "can not coerce BLOB value for column '{}'", column));
            } else {
              throw std::runtime_error(std::format(
                  "can not coerce value '{}' for column '{}'", arg, column));
            }
          }
        },
        v);

    return extract;
  }

  /**
   * Extract the value of a column into the given object.
   */
  template <typename T> void Fetch(std::string column, T &value) const {
    value = FetchAs<T>(column);
  }

private:
  std::shared_ptr<QueryCursorState> LockState() const;

  std::weak_ptr<QueryCursorState> state_;
  size_t version_;
};

/**
 * Cursor that gives exclusive access to the query internal state.
 * There can only be one cursor at a time, attempting to obtain multiple
 * cursors will result in an exception.
 */
class SqlQueryCursor {
public:
  SqlQueryCursor(std::weak_ptr<QueryCursorState> state);
  SqlQueryCursor(SqlQueryCursor &&other);
  ~SqlQueryCursor();

  using SqlCallback = std::function<bool(SqlRowView view)>;

  template <typename T> void BindAt(const std::string &param, T value) {
    auto state = LockState();
    BindAt(BindPositionFor(state, param), std::forward<T>(value));
  }

  /**
   * Bind the given arguments to the query.
   */
  template <typename... Args> void Bind(Args &&...args) {
    BindImpl<0>(std::forward_as_tuple(args...));
  }

  /**
   * Execute the query and run the given callback on each row returned.
   * When the function returns successfully, the cursor is reset to a
   * state that accepts new query parameters.
   */
  void Run(SqlCallback callback);

  /**
   * Reset the cursor to a clean state.
   */
  void Reset();

  /**
   * Bind a value to a given position in the query
   */
  void BindAt(int pos, int64_t value);
  void BindAt(int pos, double value);
  void BindAt(int pos, std::string &value);
  void BindAt(int pos, std::string &&value);
  void BindAt(int pos, std::vector<std::byte> &value);
  void BindAt(int pos, std::vector<std::byte> &&value);
  void BindAt(int pos, std::monostate _);

  /**
   * Begin a new SQL transaction for multiple queries
   */
  void BeginTransaction();

  /**
   * Commit an existing SQL transaction
   */
  void CommitTransaction();

protected:
  template <typename T>
  void BindAt(int pos, T value)
    requires std::integral<T> || BitFlagEnum<T>
  {
    BindAt(pos, static_cast<int64_t>(value));
  }

  template <typename T> void BindAt(int pos, std::optional<T> &&value) {
    if (value) {
      BindAt(pos, *value);
    } else {
      BindAt(pos, std::monostate{});
    }
  }

  template <size_t I, typename... Args>
  void BindImpl(std::tuple<Args...> args) {
    using ArgT = std::tuple_element_t<I, decltype(args)>;
    BindAt(I + 1, std::forward<ArgT>(std::get<I>(args)));

    if constexpr (I < std::tuple_size_v<decltype(args)> - 1) {
      BindImpl<I + 1>(std::move(args));
    }
  }

  std::shared_ptr<QueryCursorState> LockState();
  int BindPositionFor(std::shared_ptr<QueryCursorState> state,
                      const std::string &param);
  void CheckBind(std::shared_ptr<QueryCursorState> state, int pos);
  std::vector<std::string>
  GetBindNames(std::shared_ptr<QueryCursorState> state);

  /**
   * State shared with the owning query.
   * This ensures that the query may forcibly abandon a cursor safely.
   */
  std::weak_ptr<QueryCursorState> state_;
};

/**
 * Active/prepared SQL query object
 * The query object maintains unique ownership of an sqlite statement.
 * Ownership of the statement to run queries is temporarily borrowed by
 * cursors via the TakeCursor interface.
 */
class SqlQuery {
public:
  SqlQuery(const std::string &query) : query_(query) {}
  SqlQuery(std::string &&query) : query_(std::move(query)) {}
  SqlQuery(const SqlQuery &other) = delete;
  SqlQuery(SqlQuery &&other) = default;
  virtual ~SqlQuery() = default;

  /**
   * Reset the cursor and take ownership of the internal query state.
   * Note that his returns a guard that will release the cursor when it goes
   * out of scope.
   */
  virtual SqlQueryCursor TakeCursor() = 0;

protected:
  std::string query_;
};

class SqlTransaction;

/**
 * Manage persistence of data in a tabular format.
 * This uses the pimpl idiom to decouple the sqlite dependencies.
 */
class StorageManager {
public:
  StorageManager(std::filesystem::path db_path);
  StorageManager(StorageManager &other) = delete;
  ~StorageManager();

  SqlTransaction BeginTransaction();
  void CommitTransaction();
  void RollbackTransaction();

  std::unique_ptr<SqlQuery> Sql(std::string &query);
  std::unique_ptr<SqlQuery> Sql(std::string &&query);

  template <typename T>
  void SqlExec(T &&query, SqlQueryCursor::SqlCallback callback)
    requires std::constructible_from<std::string, T>
  {
    auto q = Sql(std::forward<std::string>(query));
    auto cursor = q->TakeCursor();
    cursor.Run(callback);
  }

  template <typename T>
  void SqlExec(T &&query)
    requires std::constructible_from<std::string, T>
  {
    SqlExec(std::forward<std::string>(query), nullptr);
  }

private:
  class StorageManagerImpl;
  std::unique_ptr<StorageManagerImpl> pimpl_;
};

/**
 * Helper RAII transaction object.
 * This rolls back the transaction when it goes out of scope.
 */
class SqlTransaction {
public:
  SqlTransaction(StorageManager *sm) : sm_(sm) {}
  ~SqlTransaction() { Rollback(); }

  void Commit() {
    if (sm_) {
      sm_->CommitTransaction();
      sm_ = nullptr;
    }
  }

  void Rollback() {
    if (sm_) {
      sm_->RollbackTransaction();
      sm_ = nullptr;
    }
  }

private:
  StorageManager *sm_;
};

} /* namespace cheri */
