#pragma once

#include <concepts>
#include <filesystem>
#include <numeric>
#include <string>

namespace cheri {

QDebug operator<<(QDebug debug, const std::filesystem::path &p);

template <typename T, char S = ','>
std::string Join(const std::vector<T> &&vec) {
  std::string acc;
  if (vec.size() == 0)
    return acc;
  acc = vec[0];

  auto JoinFn = [](std::string left, const T &right) {
    if constexpr (std::constructible_from<std::string, decltype(right)>) {
      return std::move(left) + S + right;
    } else {
      return std::move(left) + S + std::to_string(right);
    }
  };
  return std::accumulate(vec.begin() + 1, vec.end(), acc, JoinFn);
}

} /* namespace cheri */
