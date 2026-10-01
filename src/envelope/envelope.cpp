// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/envelope/envelope.hpp"

#include <algorithm>

#include "tobsv/core/checked.hpp"
#include "tobsv/core/hash.hpp"

namespace tobsv {
namespace {

bool plausibility_ok(double value) {
  return checked::is_finite(value) && value >= kAbsoluteZeroCelsius &&
         value <= kPlausibilityCeilingCelsius;
}

}  // namespace

bool ThermalEnvelope::has_level(ThresholdLevel level) const {
  switch (level) {
    case ThresholdLevel::kNominal: return has_nominal;
    case ThresholdLevel::kWarn: return has_warn;
    case ThresholdLevel::kHigh: return has_high;
    case ThresholdLevel::kCritical: return has_critical;
    case ThresholdLevel::kMaximum: return has_maximum;
  }
  return false;
}

double ThermalEnvelope::limit(ThresholdLevel level) const {
  switch (level) {
    case ThresholdLevel::kNominal: return nominal_c;
    case ThresholdLevel::kWarn: return warn_c;
    case ThresholdLevel::kHigh: return high_c;
    case ThresholdLevel::kCritical: return critical_c;
    case ThresholdLevel::kMaximum: return maximum_c;
  }
  return 0.0;
}

Status ThermalEnvelope::validate(const Limits& limits) const {
  if (!id.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "envelope has no identity");
  }
  if (!entity.is_set() && entity_class == EntityClass::kUnknown) {
    return Status::failure(ErrorCode::kUnsupported,
                           "envelope " + id.value() +
                               " is bound to neither an entity nor an entity class");
  }
  if (!declared_by.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "envelope " + id.value() + " does not name the authority that declared it");
  }
  if (!declared_at.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "envelope " + id.value() + " has no declaration timestamp");
  }
  const Status basis_status = validate_text(basis, "envelope basis", limits.max_text_length);
  if (!basis_status.ok()) {
    return basis_status;
  }
  if (!has_nominal || !has_maximum) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "envelope " + id.value() +
                               " must declare both a nominal band and an envelope ceiling");
  }

  const ThresholdLevel order[5] = {ThresholdLevel::kNominal, ThresholdLevel::kWarn,
                                   ThresholdLevel::kHigh, ThresholdLevel::kCritical,
                                   ThresholdLevel::kMaximum};
  double previous = 0.0;
  bool have_previous = false;
  for (const ThresholdLevel level : order) {
    if (!has_level(level)) {
      continue;
    }
    const double value = limit(level);
    if (!plausibility_ok(value)) {
      return Status::failure(ErrorCode::kOutOfRange,
                             "envelope " + id.value() + " band " +
                                 std::string(to_string(level)) + " is not a plausible temperature");
    }
    if (have_previous && value < previous) {
      return Status::failure(ErrorCode::kInvalidArgument,
                             "envelope " + id.value() + " band " +
                                 std::string(to_string(level)) + " is colder than the band below it");
    }
    previous = value;
    have_previous = true;
  }
  if (!(nominal_c < maximum_c)) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "envelope " + id.value() +
                               " ceiling must be strictly hotter than its nominal band");
  }
  return Status::success();
}

EnvelopeId compute_envelope_id(const ThermalEnvelope& envelope) {
  StableDigest digest;
  digest.absorb_text("tobsv.envelope.v1");
  digest.absorb_text(envelope.entity.value());
  digest.absorb_number(static_cast<std::uint64_t>(envelope.entity_class));
  digest.absorb_number(static_cast<std::uint64_t>(envelope.site));
  const ThresholdLevel order[5] = {ThresholdLevel::kNominal, ThresholdLevel::kWarn,
                                   ThresholdLevel::kHigh, ThresholdLevel::kCritical,
                                   ThresholdLevel::kMaximum};
  for (const ThresholdLevel level : order) {
    digest.absorb_flag(envelope.has_level(level));
    digest.absorb_real(envelope.limit(level));
  }
  digest.absorb_text(envelope.declared_by.value());
  return EnvelopeId::from_digest(digest.value());
}

Status EnvelopeRegistry::add(const ThermalEnvelope& envelope, const Limits& limits) {
  const Status valid = envelope.validate(limits);
  if (!valid.ok()) {
    return valid;
  }
  const EnvelopeId derived = compute_envelope_id(envelope);
  if (derived != envelope.id) {
    return Status::failure(ErrorCode::kConflict,
                           "envelope identity " + envelope.id.value() +
                               " does not match its content, which hashes to " + derived.value());
  }
  if (envelopes_.find(envelope.id) != envelopes_.end()) {
    return Status::failure(ErrorCode::kDuplicateIdentity,
                           "envelope " + envelope.id.value() + " is already registered");
  }
  const Status capacity = check_capacity(envelopes_.size(), limits.max_envelopes, "envelopes");
  if (!capacity.ok()) {
    return capacity;
  }
  envelopes_.emplace(envelope.id, envelope);
  return Status::success();
}

const ThermalEnvelope* EnvelopeRegistry::find(const EnvelopeId& id) const {
  const auto it = envelopes_.find(id);
  return it == envelopes_.end() ? nullptr : &it->second;
}

const ThermalEnvelope* EnvelopeRegistry::select(const EntityId& entity, EntityClass entity_class,
                                                MeasurementSite site) const {
  const ThermalEnvelope* best = nullptr;
  int best_score = -1;
  for (const auto& entry : envelopes_) {
    const ThermalEnvelope& candidate = entry.second;
    int score = 0;
    if (candidate.entity.is_set()) {
      if (candidate.entity != entity) {
        continue;
      }
      score += 4;
    }
    if (candidate.site != MeasurementSite::kUnknown) {
      if (candidate.site != site) {
        continue;
      }
      score += 2;
    }
    if (candidate.entity_class != EntityClass::kUnknown) {
      if (candidate.entity_class != entity_class) {
        continue;
      }
      score += 1;
    }
    if (score > best_score) {
      best_score = score;
      best = &candidate;
    }
  }
  return best;
}

std::vector<EnvelopeId> EnvelopeRegistry::ids() const {
  std::vector<EnvelopeId> out;
  out.reserve(envelopes_.size());
  for (const auto& entry : envelopes_) {
    out.push_back(entry.first);
  }
  return out;
}

}  // namespace tobsv
