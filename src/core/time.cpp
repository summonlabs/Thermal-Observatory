// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/core/time.hpp"

#include <chrono>
#include <cstdio>

#include "tobsv/core/checked.hpp"

namespace tobsv {
namespace {

constexpr UnixNanos kNanosPerMicro = 1000;
constexpr UnixNanos kNanosPerMilli = 1000000;
constexpr UnixNanos kNanosPerSecond = 1000000000;

// Howard Hinnant's civil-from-days / days-from-civil algorithms. Proleptic Gregorian, no locale, no
// timezone database, identical on every platform.
struct CivilDate {
  int year = 1970;
  unsigned month = 1;
  unsigned day = 1;
};

constexpr CivilDate civil_from_days(std::int64_t days) {
  std::int64_t z = days + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = yoe + era * 400;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t m = mp + (mp < 10 ? 3 : -9);
  CivilDate out;
  out.year = static_cast<int>(y + (m <= 2 ? 1 : 0));
  out.month = static_cast<unsigned>(m);
  out.day = static_cast<unsigned>(d);
  return out;
}

constexpr std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
  const std::int64_t y = static_cast<std::int64_t>(year) - (month <= 2 ? 1 : 0);
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const std::int64_t yoe = y - era * 400;
  const std::int64_t doy =
      (153 * static_cast<std::int64_t>(month > 2 ? month - 3 : month + 9) + 2) / 5 +
      static_cast<std::int64_t>(day) - 1;
  const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

bool is_digit(char ch) { return ch >= '0' && ch <= '9'; }

bool is_leap_year(unsigned year) {
  return (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
}

unsigned days_in_month(unsigned year, unsigned month) {
  switch (month) {
    case 1: case 3: case 5: case 7: case 8: case 10: case 12: return 31U;
    case 4: case 6: case 9: case 11: return 30U;
    case 2: return is_leap_year(year) ? 29U : 28U;
    default: return 0U;
  }
}

bool read_digits(std::string_view text, std::size_t offset, std::size_t count, unsigned& out) {
  if (offset + count > text.size()) {
    return false;
  }
  unsigned value = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const char ch = text[offset + index];
    if (!is_digit(ch)) {
      return false;
    }
    value = value * 10U + static_cast<unsigned>(ch - '0');
  }
  out = value;
  return true;
}

}  // namespace

Result<Duration> Duration::from_micros(std::int64_t value) {
  const auto scaled = checked::mul(value, kNanosPerMicro);
  if (!scaled.has_value()) {
    return Status::failure(ErrorCode::kOutOfRange, "duration in microseconds is out of range");
  }
  return Duration::from_nanos(*scaled);
}

Result<Duration> Duration::from_millis(std::int64_t value) {
  const auto scaled = checked::mul(value, kNanosPerMilli);
  if (!scaled.has_value()) {
    return Status::failure(ErrorCode::kOutOfRange, "duration in milliseconds is out of range");
  }
  return Duration::from_nanos(*scaled);
}

namespace {

// Exactly 2^63. The conversion below must reject this value, because a double equal to 2^63 is one
// past the largest representable int64 and the cast would be undefined behaviour. An earlier guard
// compared against the decimal literal 9.2233720368547758e18, which rounds to exactly 2^63, so the
// boundary itself slipped through.
constexpr double kTwoTo63 = 9223372036854775808.0;

bool in_nanosecond_range(double scaled) {
  return scaled >= -kTwoTo63 && scaled < kTwoTo63;
}

}  // namespace

Result<Duration> Duration::from_seconds(double value) {
  if (!checked::is_finite(value)) {
    return Status::failure(ErrorCode::kInvalidArgument, "duration in seconds is not finite");
  }
  const double scaled = value * 1.0e9;
  if (!checked::is_finite(scaled) || !in_nanosecond_range(scaled)) {
    return Status::failure(ErrorCode::kOutOfRange, "duration in seconds is out of range");
  }
  return Duration::from_nanos(static_cast<Nanos>(scaled));
}

Duration Duration::magnitude() const {
  if (value_ == INT64_MIN) {
    return Duration::max();
  }
  return Duration::from_nanos(value_ < 0 ? -value_ : value_);
}

Result<Duration> operator+(const Duration& a, const Duration& b) {
  const auto sum = checked::add(a.nanos(), b.nanos());
  if (!sum.has_value()) {
    return Status::failure(ErrorCode::kOutOfRange, "duration addition overflowed");
  }
  return Duration::from_nanos(*sum);
}

Result<Duration> operator-(const Duration& a, const Duration& b) {
  const auto difference = checked::sub(a.nanos(), b.nanos());
  if (!difference.has_value()) {
    return Status::failure(ErrorCode::kOutOfRange, "duration subtraction overflowed");
  }
  return Duration::from_nanos(*difference);
}

Duration saturating_sub(const Duration& a, const Duration& b) {
  const auto difference = checked::sub(a.nanos(), b.nanos());
  if (difference.has_value()) {
    return Duration::from_nanos(*difference);
  }
  return a.nanos() > 0 ? Duration::max() : Duration::min();
}

Result<Timestamp> Timestamp::from_unix_seconds(double value) {
  if (!checked::is_finite(value)) {
    return Status::failure(ErrorCode::kInvalidArgument, "timestamp in seconds is not finite");
  }
  const double scaled = value * 1.0e9;
  if (!checked::is_finite(scaled) || !in_nanosecond_range(scaled)) {
    return Status::failure(ErrorCode::kOutOfRange, "timestamp in seconds is out of range");
  }
  const Nanos nanos = static_cast<Nanos>(scaled);
  if (nanos == kUnsetUnixNanos) {
    return Status::failure(ErrorCode::kOutOfRange, "timestamp collides with the unset sentinel");
  }
  return Timestamp::from_unix_nanos(nanos);
}

Result<Timestamp> Timestamp::now_utc() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return Timestamp::from_unix_seconds(static_cast<double>(nanos) / 1.0e9);
}

std::string Timestamp::to_string() const {
  if (!is_set()) {
    return "unset";
  }
  const UnixNanos value = value_;
  std::int64_t seconds = value / kNanosPerSecond;
  std::int64_t fraction = value % kNanosPerSecond;
  if (fraction < 0) {
    fraction += kNanosPerSecond;
    seconds -= 1;
  }
  std::int64_t days = seconds / 86400;
  std::int64_t second_of_day = seconds % 86400;
  if (second_of_day < 0) {
    second_of_day += 86400;
    days -= 1;
  }
  const CivilDate date = civil_from_days(days);
  const unsigned hour = static_cast<unsigned>(second_of_day / 3600);
  const unsigned minute = static_cast<unsigned>((second_of_day % 3600) / 60);
  const unsigned second = static_cast<unsigned>(second_of_day % 60);

  char buffer[48];
  const int written = std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02uT%02u:%02u:%02u.%09lldZ",
                                    date.year, date.month, date.day, hour, minute, second,
                                    static_cast<long long>(fraction));
  if (written <= 0) {
    return "unset";
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

Result<Timestamp> Timestamp::parse(std::string_view text) {
  // Required shape: YYYY-MM-DDTHH:MM:SS[.fraction]Z, with 1..9 fractional digits.
  if (text.size() < 20) {
    return Status::failure(ErrorCode::kMalformedInput, "timestamp is shorter than the minimum form");
  }
  if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
    return Status::failure(ErrorCode::kMalformedInput, "timestamp separators are misplaced");
  }
  unsigned year = 0;
  unsigned month = 0;
  unsigned day = 0;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  if (!read_digits(text, 0, 4, year) || !read_digits(text, 5, 2, month) ||
      !read_digits(text, 8, 2, day) || !read_digits(text, 11, 2, hour) ||
      !read_digits(text, 14, 2, minute) || !read_digits(text, 17, 2, second)) {
    return Status::failure(ErrorCode::kMalformedInput, "timestamp contains a non-digit field");
  }
  if (month < 1 || month > 12 || hour > 23 || minute > 59 || second > 60) {
    return Status::failure(ErrorCode::kMalformedInput, "timestamp field is out of range");
  }
  // A day is checked against its own month. Normalising 2026-02-31 into March would accept a
  // timestamp the producer never meant to write.
  if (day < 1 || day > days_in_month(year, month)) {
    return Status::failure(ErrorCode::kMalformedInput,
                           "timestamp day does not exist in that month");
  }
  std::size_t cursor = 19;
  std::int64_t fraction = 0;
  if (cursor < text.size() && text[cursor] == '.') {
    ++cursor;
    const std::size_t fraction_start = cursor;
    std::int64_t scale = 100000000;
    while (cursor < text.size() && is_digit(text[cursor])) {
      if (cursor - fraction_start >= 9) {
        return Status::failure(ErrorCode::kMalformedInput,
                               "timestamp has more than nine fractional digits");
      }
      fraction += static_cast<std::int64_t>(text[cursor] - '0') * scale;
      scale /= 10;
      ++cursor;
    }
    if (cursor == fraction_start) {
      return Status::failure(ErrorCode::kMalformedInput, "timestamp has an empty fraction");
    }
  }
  if (cursor >= text.size() || text[cursor] != 'Z' || cursor + 1 != text.size()) {
    return Status::failure(ErrorCode::kMalformedInput,
                           "timestamp must end with a single trailing Z (UTC)");
  }
  if (second == 60) {
    return Status::failure(ErrorCode::kMalformedInput, "leap seconds are not representable");
  }
  const std::int64_t days = days_from_civil(static_cast<int>(year), month, day);
  const std::int64_t seconds_of_day =
      static_cast<std::int64_t>(hour) * 3600 + static_cast<std::int64_t>(minute) * 60 +
      static_cast<std::int64_t>(second);
  const auto total_seconds = checked::mul(days, static_cast<std::int64_t>(86400));
  if (!total_seconds.has_value()) {
    return Status::failure(ErrorCode::kOutOfRange, "timestamp day count is out of range");
  }
  const auto with_day = checked::add(*total_seconds, seconds_of_day);
  if (!with_day.has_value()) {
    return Status::failure(ErrorCode::kOutOfRange, "timestamp seconds are out of range");
  }
  const auto nanos = checked::mul(*with_day, kNanosPerSecond);
  if (!nanos.has_value()) {
    return Status::failure(ErrorCode::kOutOfRange, "timestamp nanoseconds are out of range");
  }
  const auto total = checked::add(*nanos, fraction);
  if (!total.has_value() || *total == kUnsetUnixNanos) {
    return Status::failure(ErrorCode::kOutOfRange, "timestamp nanoseconds are out of range");
  }
  return Timestamp::from_unix_nanos(*total);
}

Duration age_of(const Timestamp& observed_at, const Timestamp& evaluated_at) {
  if (!observed_at.is_set() || !evaluated_at.is_set()) {
    return Duration::zero();
  }
  return saturating_sub(Duration::from_nanos(evaluated_at.unix_nanos()),
                        Duration::from_nanos(observed_at.unix_nanos()));
}

std::string_view to_string(ClockDomain domain) noexcept {
  switch (domain) {
    case ClockDomain::kUnspecified: return "unspecified";
    case ClockDomain::kCollectorWallClock: return "collector_wall_clock";
    case ClockDomain::kSensorMonotonic: return "sensor_monotonic";
    case ClockDomain::kHostMonotonic: return "host_monotonic";
    case ClockDomain::kSynthetic: return "synthetic";
  }
  return "unspecified";
}

bool parse_clock_domain(std::string_view text, ClockDomain& out) noexcept {
  if (text == "unspecified") { out = ClockDomain::kUnspecified; return true; }
  if (text == "collector_wall_clock") { out = ClockDomain::kCollectorWallClock; return true; }
  if (text == "sensor_monotonic") { out = ClockDomain::kSensorMonotonic; return true; }
  if (text == "host_monotonic") { out = ClockDomain::kHostMonotonic; return true; }
  if (text == "synthetic") { out = ClockDomain::kSynthetic; return true; }
  return false;
}

Nanos MonoClock::now_nanos() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

}  // namespace tobsv
