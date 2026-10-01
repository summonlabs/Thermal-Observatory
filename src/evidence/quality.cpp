// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/evidence/quality.hpp"

#include "tobsv/core/checked.hpp"

namespace tobsv {
namespace {

struct FlagName {
  QualityFlag flag;
  std::string_view name;
};

constexpr FlagName kFlagNames[] = {
    {QualityFlag::kCalibrated, "calibrated"},
    {QualityFlag::kUncalibrated, "uncalibrated"},
    {QualityFlag::kSuspect, "suspect"},
    {QualityFlag::kInterpolated, "interpolated"},
    {QualityFlag::kExtrapolated, "extrapolated"},
    {QualityFlag::kSensorFault, "sensor_fault"},
    {QualityFlag::kRateLimited, "rate_limited"},
    {QualityFlag::kDerivedFromModel, "derived_from_model"},
};

}  // namespace

std::string_view to_string(QualityFlag flag) noexcept {
  for (const FlagName& entry : kFlagNames) {
    if (entry.flag == flag) {
      return entry.name;
    }
  }
  return "none";
}

bool parse_quality_flag(std::string_view text, QualityFlag& out) noexcept {
  for (const FlagName& entry : kFlagNames) {
    if (entry.name == text) {
      out = entry.flag;
      return true;
    }
  }
  return false;
}

std::vector<std::string> QualityFlags::names() const {
  std::vector<std::string> out;
  for (const FlagName& entry : kFlagNames) {
    if (has(entry.flag)) {
      out.emplace_back(entry.name);
    }
  }
  return out;
}

std::string QualityFlags::joined() const {
  std::string out;
  for (const std::string& name : names()) {
    if (!out.empty()) {
      out.push_back('|');
    }
    out += name;
  }
  return out.empty() ? std::string("none") : out;
}

Status QualityMetadata::validate(const Limits& limits) const {
  if (!has_confidence) {
    return Status::success();
  }
  if (!checked::is_finite(confidence)) {
    return Status::failure(ErrorCode::kIndeterminate, "quality confidence is not a finite number");
  }
  if (confidence < 0.0 || confidence > 1.0) {
    return Status::failure(ErrorCode::kOutOfRange,
                           "quality confidence " + std::to_string(confidence) +
                               " is outside [0, 1]");
  }
  const Status text = validate_text(flags.joined(), "quality flags", limits.max_text_length);
  if (!text.ok()) {
    return text;
  }
  return Status::success();
}

bool QualityMetadata::is_degraded() const {
  if (!supplied) {
    return false;
  }
  if (flags.has(QualityFlag::kUncalibrated) || flags.has(QualityFlag::kSuspect) ||
      flags.has(QualityFlag::kInterpolated) || flags.has(QualityFlag::kExtrapolated) ||
      flags.has(QualityFlag::kSensorFault) || flags.has(QualityFlag::kRateLimited) ||
      flags.has(QualityFlag::kDerivedFromModel)) {
    return true;
  }
  return false;
}

}  // namespace tobsv
