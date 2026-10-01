// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace tobsv::checked {

// Wrap-around is never acceptable in this runtime: every integer operation that could overflow is
// expressed here so that the failure is a value rather than undefined behaviour. The arithmetic is
// performed in the unsigned domain of the same width (defined to wrap) and the wrap is detected
// afterwards, which is portable and free of compiler builtins.

namespace detail {

template <std::integral T>
constexpr auto magnitude(T value) -> std::make_unsigned_t<T> {
  using U = std::make_unsigned_t<T>;
  if constexpr (std::is_signed_v<T>) {
    if (value < T{0}) {
      return static_cast<U>(U{0} - static_cast<U>(value));
    }
    return static_cast<U>(value);
  } else {
    return static_cast<U>(value);
  }
}

}  // namespace detail

template <std::integral T>
constexpr std::optional<T> add(T a, T b) {
  using U = std::make_unsigned_t<T>;
  const U wrapped = static_cast<U>(static_cast<U>(a) + static_cast<U>(b));
  if constexpr (std::is_signed_v<T>) {
    const T result = static_cast<T>(wrapped);
    if ((a >= T{0}) == (b >= T{0}) && (result >= T{0}) != (a >= T{0})) {
      return std::nullopt;
    }
    return result;
  } else {
    if (wrapped < static_cast<U>(a)) {
      return std::nullopt;
    }
    return static_cast<T>(wrapped);
  }
}

template <std::integral T>
constexpr std::optional<T> sub(T a, T b) {
  using U = std::make_unsigned_t<T>;
  const U wrapped = static_cast<U>(static_cast<U>(a) - static_cast<U>(b));
  if constexpr (std::is_signed_v<T>) {
    const T result = static_cast<T>(wrapped);
    if ((a >= T{0}) != (b >= T{0}) && (result >= T{0}) != (a >= T{0})) {
      return std::nullopt;
    }
    return result;
  } else {
    if (static_cast<U>(a) < static_cast<U>(b)) {
      return std::nullopt;
    }
    return static_cast<T>(wrapped);
  }
}

template <std::integral T>
constexpr std::optional<T> mul(T a, T b) {
  using U = std::make_unsigned_t<T>;
  if (a == T{0} || b == T{0}) {
    return T{0};
  }
  bool negative = false;
  if constexpr (std::is_signed_v<T>) {
    negative = (a < T{0}) != (b < T{0});
    // The most negative value has no positive counterpart; reject the only two cases where the
    // magnitude computation would overflow.
    if (a == std::numeric_limits<T>::min() && b == T{-1}) {
      return std::nullopt;
    }
    if (b == std::numeric_limits<T>::min() && a == T{-1}) {
      return std::nullopt;
    }
  }
  const U ua = detail::magnitude(a);
  const U ub = detail::magnitude(b);
  if (ua > std::numeric_limits<U>::max() / ub) {
    return std::nullopt;
  }
  const U product = static_cast<U>(ua * ub);
  U limit = static_cast<U>(std::numeric_limits<T>::max());
  if constexpr (std::is_signed_v<T>) {
    if (negative) {
      limit = static_cast<U>(limit + U{1});
    }
  }
  if (product > limit) {
    return std::nullopt;
  }
  if constexpr (std::is_signed_v<T>) {
    if (negative) {
      return static_cast<T>(U{0} - product);
    }
  }
  return static_cast<T>(product);
}

// Narrowing conversion with an explicit range check. Returns nullopt when the value does not fit.
template <std::integral To, std::integral From>
constexpr std::optional<To> narrow(From value) {
  // The bounds are compared with std::cmp_less / std::cmp_greater rather than by widening both
  // operands to their common type. Widening is wrong whenever the common type is unsigned and the
  // destination is signed: a negative bound converts to a huge unsigned value, and every small
  // positive input then looks out of range.
  // A negative input cannot reach an unsigned destination. This is a separate test from the bound
  // comparison below rather than a pre-widening one, because for an unsigned destination the lower
  // bound is zero and comparing against it would be a constant expression.
  if constexpr (std::is_signed_v<From> && !std::is_signed_v<To>) {
    if (value < From{0}) {
      return std::nullopt;
    }
  }
  if constexpr (std::is_signed_v<To>) {
    if (std::cmp_less(value, std::numeric_limits<To>::min())) {
      return std::nullopt;
    }
  }
  if (std::cmp_greater(value, std::numeric_limits<To>::max())) {
    return std::nullopt;
  }
  return static_cast<To>(value);
}

inline std::optional<std::size_t> size_from_u64(std::uint64_t value) {
  return narrow<std::size_t>(value);
}

inline std::optional<std::size_t> add_size(std::size_t a, std::size_t b) { return add(a, b); }

// --- Floating point ------------------------------------------------------------------------
//
// Non-finite input is a first-class outcome everywhere in this runtime: it is never silently
// propagated into an arithmetic result and never coerced to zero.

inline bool is_finite(double value) { return std::isfinite(value); }
inline bool is_finite_positive(double value) { return std::isfinite(value) && value > 0.0; }
inline bool is_finite_non_negative(double value) { return std::isfinite(value) && value >= 0.0; }

inline bool in_range(double value, double low, double high) {
  return std::isfinite(value) && value >= low && value <= high;
}

// Checked sum of two doubles. The result must itself be finite.
inline std::optional<double> add(double a, double b) {
  if (!is_finite(a) || !is_finite(b)) {
    return std::nullopt;
  }
  const double sum = a + b;
  if (!is_finite(sum)) {
    return std::nullopt;
  }
  return sum;
}

// Checked difference of two doubles. The result must itself be finite.
inline std::optional<double> sub(double a, double b) {
  if (!is_finite(a) || !is_finite(b)) {
    return std::nullopt;
  }
  const double difference = a - b;
  if (!is_finite(difference)) {
    return std::nullopt;
  }
  return difference;
}

// Checked quotient. A zero or non-finite denominator is refused rather than producing infinity.
inline std::optional<double> div(double numerator, double denominator) {
  if (!is_finite(numerator) || !is_finite(denominator) || denominator == 0.0) {
    return std::nullopt;
  }
  const double quotient = numerator / denominator;
  if (!is_finite(quotient)) {
    return std::nullopt;
  }
  return quotient;
}

inline double clamp(double value, double low, double high) {
  if (!is_finite(value)) {
    return low;
  }
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

inline std::optional<double> abs(double value) {
  if (!is_finite(value)) {
    return std::nullopt;
  }
  return value < 0.0 ? -value : value;
}

}  // namespace tobsv::checked
