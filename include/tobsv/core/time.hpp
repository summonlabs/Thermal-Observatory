// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "tobsv/core/result.hpp"

namespace tobsv {

using Nanos = std::int64_t;
using UnixNanos = std::int64_t;

// Sentinel for "no timestamp was ever supplied". It is deliberately not zero: the Unix epoch is a
// legitimate (if uninteresting) observation time, and conflating the two would let missing evidence
// masquerade as evidence from 1970.
inline constexpr UnixNanos kUnsetUnixNanos = INT64_MIN;

class Duration {
 public:
  constexpr Duration() = default;

  static constexpr Duration from_nanos(Nanos value) { return Duration(value); }
  static Result<Duration> from_micros(std::int64_t value);
  static Result<Duration> from_millis(std::int64_t value);
  static Result<Duration> from_seconds(double value);

  static constexpr Duration max() { return Duration(INT64_MAX); }
  static constexpr Duration min() { return Duration(INT64_MIN); }
  static constexpr Duration zero() { return Duration(0); }

  constexpr Nanos nanos() const noexcept { return value_; }
  double seconds() const noexcept { return static_cast<double>(value_) / 1.0e9; }
  constexpr bool is_zero() const noexcept { return value_ == 0; }
  constexpr bool is_negative() const noexcept { return value_ < 0; }
  constexpr bool is_positive() const noexcept { return value_ > 0; }

  // Saturating: a duration whose magnitude cannot be negated is clamped to Duration::max().
  Duration magnitude() const;

  friend constexpr bool operator==(const Duration& a, const Duration& b) {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(const Duration& a, const Duration& b) { return !(a == b); }
  friend constexpr bool operator<(const Duration& a, const Duration& b) {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator<=(const Duration& a, const Duration& b) { return !(b < a); }
  friend constexpr bool operator>(const Duration& a, const Duration& b) { return b < a; }
  friend constexpr bool operator>=(const Duration& a, const Duration& b) { return !(a < b); }

 private:
  explicit constexpr Duration(Nanos value) : value_(value) {}
  Nanos value_ = 0;
};

Result<Duration> operator+(const Duration& a, const Duration& b);
Result<Duration> operator-(const Duration& a, const Duration& b);
// Saturating difference used for age computations, where a clamped answer is safe and an overflow
// is not.
Duration saturating_sub(const Duration& a, const Duration& b);

// A point on the Unix timeline, in nanoseconds, always UTC. Nothing in this runtime is localised.
class Timestamp {
 public:
  constexpr Timestamp() = default;

  static constexpr Timestamp from_unix_nanos(UnixNanos value) { return Timestamp(value); }
  static Result<Timestamp> from_unix_seconds(double value);
  static Result<Timestamp> parse(std::string_view text);

  static Result<Timestamp> now_utc();

  constexpr UnixNanos unix_nanos() const noexcept { return value_; }
  double unix_seconds() const noexcept { return static_cast<double>(value_) / 1.0e9; }
  constexpr bool is_set() const noexcept { return value_ != kUnsetUnixNanos; }

  // RFC 3339 / ISO 8601 in UTC with a fixed nine-digit fractional part, e.g.
  // "2026-02-14T09:31:07.123456789Z". Fixed width keeps textual records byte-stable.
  std::string to_string() const;

  friend constexpr bool operator==(const Timestamp& a, const Timestamp& b) {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(const Timestamp& a, const Timestamp& b) { return !(a == b); }
  friend constexpr bool operator<(const Timestamp& a, const Timestamp& b) {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator<=(const Timestamp& a, const Timestamp& b) { return !(b < a); }
  friend constexpr bool operator>(const Timestamp& a, const Timestamp& b) { return b < a; }
  friend constexpr bool operator>=(const Timestamp& a, const Timestamp& b) { return !(a < b); }

 private:
  explicit constexpr Timestamp(UnixNanos value) : value_(value) {}
  UnixNanos value_ = kUnsetUnixNanos;
};

// Age of an observation relative to an evaluation instant. A negative age means the observation is
// timestamped in the future, which is preserved rather than clamped so that clock problems stay
// visible.
Duration age_of(const Timestamp& observed_at, const Timestamp& evaluated_at);

// Which clock produced a timestamp. Two observations from different clock domains cannot be ordered
// with confidence, and the runtime says so instead of pretending otherwise.
enum class ClockDomain : std::uint8_t {
  kUnspecified = 0,
  kCollectorWallClock,
  kSensorMonotonic,
  kHostMonotonic,
  kSynthetic,
};

std::string_view to_string(ClockDomain domain) noexcept;
bool parse_clock_domain(std::string_view text, ClockDomain& out) noexcept;

// Monotonic host clock: used for durations the runtime measures about itself. It is never mixed
// with wall-clock timestamps.
class MonoClock {
 public:
  static Nanos now_nanos() noexcept;
};

}  // namespace tobsv
