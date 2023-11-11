#pragma once

#include <concepts>
#include <functional>
#include <filesystem>
#include <format>
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
template<typename T>
struct is_optional {
  static constexpr bool value = false;
};

template<typename V>
struct is_optional<std::optional<V>> {
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
 * Internal shared state of the query.
 * This is opaque to the interface.
 */
struct QueryCursor;

using SqlValue = std::variant<
  int64_t,
  double,
  std::string,
  std::vector<std::byte>,
  std::monostate>;

/**
 * Helper object that wraps access to an query result row.
 * Note that this provides readonly access with borrowed types.
 */
class SqlRowView {
 public:
  SqlRowView() = default;
  SqlRowView(std::weak_ptr<QueryCursor> cursor, size_t version);
  SqlRowView(const SqlRowView &other) = default;
  ~SqlRowView() = default;
  size_t Size() const;
  std::vector<std::string> Columns() const;
  SqlValue ValueAt(std::string column) const;

  template<typename T>
  std::optional<T> At(std::string column) const {
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
  template<typename T>
  T FetchAs(std::string column) const {
    SqlValue v = ValueAt(column);

    auto extract = std::visit([&column](auto &&arg) -> T {
      using A = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<A, std::monostate>) {
        if constexpr (impl::is_optional<T>::value) {
          return std::nullopt;
        } else {
          throw std::runtime_error(std::format(
              "can not coerce NULL value for column '{}'", column));
        }
      } else if constexpr (std::convertible_to<A, T> || (BitFlagEnum<T> && std::is_integral_v<A>)) {
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
    }, v);

    return extract;
  }

  /**
   * Extract the value of a column into the given object.
   */
  template<typename T>
  void Fetch(std::string column, T &value) const {
    value = FetchAs<T>(column);
  }

 private:
  std::weak_ptr<QueryCursor> cursor_;
  size_t version_;
};

/**
 * Active/prepared SQL query object
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

  /**
   * Bind a value to a given position in the query
   */
  virtual void BindAt(int pos, int64_t value) = 0;
  virtual void BindAt(int pos, double value) = 0;
  virtual void BindAt(int pos, std::string &value) = 0;
  virtual void BindAt(int pos, std::string &&value) = 0;
  virtual void BindAt(int pos, std::vector<std::byte> &value) = 0;
  virtual void BindAt(int pos, std::vector<std::byte> &&value) = 0;
  virtual void BindAt(int pos, std::monostate _) = 0;

  /**
   * Reset the cursor and take ownership of the internal query state.
   * XXX this should return a guard to release the cursor once we are done.
   */
  virtual void TakeCursor() = 0;

  template<typename T>
  void BindAt(const std::string &param, T value) {
    BindAt(BindPositionFor(param), std::forward<T>(value));
  }

  /**
   * Bind the given arguments to the query.
   */
  template<typename... Args>
  void Bind(Args&& ...args) {
    BindImpl<0>(std::forward_as_tuple(args...));
  }

 protected:
  virtual int BindPositionFor(const std::string &param) = 0;

  template<typename T>
  void BindAt(int pos, T value) requires std::integral<T> || BitFlagEnum<T> {
    BindAt(pos, static_cast<int64_t>(value));
  }

  template<typename T>
  void BindAt(int pos, std::optional<T> &&value) {
    if (value) {
      BindAt(pos, *value);
    } else {
      BindAt(pos, std::monostate{});
    }
  }

  template<size_t I, typename... Args>
  void BindImpl(std::tuple<Args...> args) {
    using ArgT = std::tuple_element_t<I, decltype(args)>;
    BindAt(I + 1, std::forward<ArgT>(std::get<I>(args)));

    if constexpr (I < std::tuple_size_v<decltype(args)> - 1) {
      BindImpl<I + 1>(std::move(args));
    }
  }

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
