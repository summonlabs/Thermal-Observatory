// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/evidence/evidence.hpp"

#include "tobsv/core/checked.hpp"
#include "tobsv/core/hash.hpp"

namespace tobsv {

Status TemperatureObservation::validate(const Limits& limits) const {
  if (!id.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "observation has no identity");
  }
  if (!entity.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "observation " + id.value() + " names no entity");
  }
  if (!sensor.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "observation " + id.value() + " names no sensor");
  }
  if (site == MeasurementSite::kUnknown) {
    return Status::failure(ErrorCode::kUnsupported,
                           "observation " + id.value() + " does not say where it was measured");
  }
  if (!checked::is_finite(celsius)) {
    return Status::failure(ErrorCode::kIndeterminate,
                           "observation " + id.value() + " carries a non-finite temperature");
  }
  if (celsius < kMinPlausibleCelsius) {
    return Status::failure(ErrorCode::kOutOfRange,
                           "observation " + id.value() + " reads " + std::to_string(celsius) +
                               " C, below absolute zero");
  }
  if (celsius > kMaxPlausibleCelsius) {
    return Status::failure(ErrorCode::kOutOfRange,
                           "observation " + id.value() + " reads " + std::to_string(celsius) +
                               " C, above the plausibility ceiling");
  }
  if (!observed_at.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "observation " + id.value() + " has no observation timestamp");
  }
  if (!received_at.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "observation " + id.value() + " has no receive timestamp");
  }
  const Status provenance_status = provenance.validate(limits);
  if (!provenance_status.ok()) {
    return provenance_status;
  }
  const Status quality_status = quality.validate(limits);
  if (!quality_status.ok()) {
    return quality_status;
  }
  const Status fence_status = fence.validate();
  if (!fence_status.ok()) {
    return fence_status;
  }
  if (fence.source != provenance.source) {
    return Status::failure(ErrorCode::kConflict,
                           "observation " + id.value() + " is fenced by source " +
                               fence.source.value() + " but attributed to source " +
                               provenance.source.value());
  }
  return Status::success();
}

ObservationId compute_observation_id(const TemperatureObservation& observation) {
  StableDigest digest;
  digest.absorb_text("tobsv.observation.v1");
  digest.absorb_text(observation.entity.value());
  digest.absorb_text(observation.sensor.value());
  digest.absorb_number(static_cast<std::uint64_t>(observation.site));
  digest.absorb_real(observation.celsius);
  digest.absorb_number(static_cast<std::uint64_t>(observation.observed_at.unix_nanos()));
  digest.absorb_text(observation.provenance.source.value());
  digest.absorb_number(static_cast<std::uint64_t>(observation.provenance.authority));
  digest.absorb_number(static_cast<std::uint64_t>(observation.provenance.kind));
  digest.absorb_number(static_cast<std::uint64_t>(observation.provenance.clock));
  digest.absorb_text(observation.provenance.method);
  digest.absorb_text(observation.envelope.value());
  digest.absorb_number(observation.fence.epoch.value());
  digest.absorb_number(observation.fence.generation.value());
  digest.absorb_number(observation.fence.revision.value());
  digest.absorb_number(observation.fence.incarnation.value());
  digest.absorb_number(observation.fence.sequence.value());
  digest.absorb_text(observation.fence.attempt.value());
  return ObservationId::from_digest(digest.value());
}

FreshnessPolicy FreshnessPolicy::from_limits(const Limits& limits) {
  FreshnessPolicy policy;
  policy.window = Duration::from_nanos(limits.default_freshness_window);
  return policy;
}

Status FreshnessPolicy::validate() const {
  if (window.nanos() <= 0) {
    return Status::failure(ErrorCode::kInvalidArgument, "freshness window must be positive");
  }
  if (max_clock_skew.nanos() < 0) {
    return Status::failure(ErrorCode::kInvalidArgument, "clock skew tolerance must not be negative");
  }
  if (!checked::is_finite_non_negative(agreement_tolerance_c)) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "agreement tolerance must be a finite, non-negative number");
  }
  return Status::success();
}

JsonValue ObservationRef::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("observation", JsonValue::text(id.value()));
  out.set("source", JsonValue::text(source.value()));
  out.set("celsius", JsonValue::real(celsius));
  out.set("observed_at", JsonValue::text(observed_at.to_string()));
  out.set("fence_revision", JsonValue::integer(static_cast<std::int64_t>(fence_revision.value())));
  out.set("authority", JsonValue::text(std::string(to_string(authority))));
  out.set("site", JsonValue::text(std::string(to_string(site))));
  out.set("quality_supplied", JsonValue::boolean(quality_supplied));
  out.set("quality_degraded", JsonValue::boolean(quality_degraded));
  out.set("synthetic", JsonValue::boolean(synthetic));
  return out;
}

namespace {

JsonValue refs_to_json(const std::vector<ObservationRef>& refs) {
  JsonValue array = JsonValue::array();
  for (const ObservationRef& ref : refs) {
    array.push(ref.to_json());
  }
  return array;
}

}  // namespace

JsonValue SubjectTemperature::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("entity", JsonValue::text(entity.value()));
  out.set("sensor", JsonValue::text(sensor.value()));
  out.set("site", JsonValue::text(std::string(to_string(site))));
  out.set("state", JsonValue::text(std::string(to_string(state))));
  if (state == EvidenceState::kFresh || state == EvidenceState::kConflicting) {
    out.set("celsius", JsonValue::real(representative_celsius));
    out.set("representative_at", JsonValue::text(representative_at.to_string()));
    out.set("representative_observation", JsonValue::text(representative_id.value()));
    out.set("representative_authority", JsonValue::text(std::string(to_string(representative_authority))));
    out.set("representative_synthetic", JsonValue::boolean(representative_synthetic));
  }
  out.set("age_seconds", JsonValue::real(age.seconds()));
  out.set("supporting", refs_to_json(supporting));
  out.set("conflicting", refs_to_json(conflicting));
  out.set("stale", refs_to_json(stale));
  out.set("superseded", refs_to_json(superseded));
  out.set("indeterminate", refs_to_json(indeterminate));
  out.set("reason", JsonValue::text(reason));
  return out;
}

}  // namespace tobsv
