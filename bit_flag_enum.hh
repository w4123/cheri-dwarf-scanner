#pragma once

#include <concepts>

namespace cheri {

template<typename T>
struct EnumTraits {
  static constexpr bool is_bitflag = false;
};

template<typename T>
concept BitFlagEnum = std::is_enum_v<T> && EnumTraits<T>::is_bitflag;

template<BitFlagEnum T>
T operator|(T L, T R) {
  return static_cast<T>(static_cast<int>(L) | static_cast<int>(R));
}

template<BitFlagEnum T>
T operator&(T L, T R) {
  return static_cast<T>(static_cast<int>(L) & static_cast<int>(R));
}

template<BitFlagEnum T>
T& operator|=(T &L, T R) {
  L = L | R;
  return L;
}

} /* namespace cheri */
