#pragma once

#include <concepts>

namespace cheri {

template <typename T> struct EnumTraits {
  static constexpr bool is_bitflag = false;
};

template <typename T>
concept BitFlagEnum = std::is_enum_v<T> && EnumTraits<T>::is_bitflag;

template <BitFlagEnum T> constexpr T operator|(T l, T r) {
  return static_cast<T>(static_cast<int>(l) | static_cast<int>(r));
}

template <BitFlagEnum T> constexpr T operator&(T l, T r) {
  return static_cast<T>(static_cast<int>(l) & static_cast<int>(r));
}

template <BitFlagEnum T> constexpr T &operator|=(T &l, T r) {
  l = l | r;
  return l;
}

template <BitFlagEnum T> constexpr bool operator!(T v) {
  return v == static_cast<T>(0);
}

template <BitFlagEnum T> std::ostream &operator<<(std::ostream &os, T v) {
  os << static_cast<int>(v);
  return os;
}

} /* namespace cheri */
