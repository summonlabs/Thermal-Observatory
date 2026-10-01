// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tobsv/core/limits.hpp"
#include "tobsv/core/result.hpp"

namespace tobsv {

// Quality metadata is optional in the world this runtime observes: many facility sensors publish a
// bare number. The model therefore distinguishes "no quality statement was supplied" from "a
// quality statement was supplied and it is clean". An analysis that depends on quality has to
// report an attribution limit when the statement is missing rather than assuming the good case.
enum class QualityFlag : std::uint32_t {
  kNone = 0U,
  kCalibrated = 1U << 0U,
  kUncalibrated = 1U << 1U,
  kSuspect = 1U << 2U,
  kInterpolated = 1U << 3U,
  kExtrapolated = 1U << 4U,
  kSensorFault = 1U << 5U,
  kRateLimited = 1U << 6U,
  kDerivedFromModel = 1U << 7U,
};

std::string_view to_string(QualityFlag flag) noexcept;
bool parse_quality_flag(std::string_view text, QualityFlag& out) noexcept;

class QualityFlags {
 public:
  constexpr QualityFlags() = default;
  constexpr explicit QualityFlags(std::uint32_t bits) : bits_(bits) {}

  constexpr std::uint32_t bits() const noexcept { return bits_; }
  constexpr bool empty() const noexcept { return bits_ == 0U; }
  constexpr bool has(QualityFlag flag) const noexcept {
    return (bits_ & static_cast<std::uint32_t>(flag)) != 0U;
  }

  constexpr QualityFlags& add(QualityFlag flag) noexcept {
    bits_ |= static_cast<std::uint32_t>(flag);
    return *this;
  }

  friend constexpr bool operator==(const QualityFlags& a, const QualityFlags& b) {
    return a.bits_ == b.bits_;
  }
  friend constexpr bool operator!=(const QualityFlags& a, const QualityFlags& b) {
    return !(a == b);
  }

  // Flag names in a fixed order, so that a record's textual form is byte-stable.
  std::vector<std::string> names() const;
  std::string joined() const;

 private:
  std::uint32_t bits_ = 0U;
};

struct QualityMetadata {
  bool supplied = false;
  QualityFlags flags;
  bool has_confidence = false;
  double confidence = 0.0;

  static QualityMetadata unsupplied() { return QualityMetadata{}; }

  Status validate(const Limits& limits) const;

  // True when the source explicitly told us the value is not to be taken at face value.
  bool is_degraded() const;
  // True when nothing at all was said about quality.
  bool is_silent() const { return !supplied; }
};

}  // namespace tobsv
