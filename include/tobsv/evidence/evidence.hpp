// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tobsv/core/json.hpp"
#include "tobsv/core/limits.hpp"
#include "tobsv/core/result.hpp"
#include "tobsv/core/strong_id.hpp"
#include "tobsv/core/time.hpp"
#include "tobsv/evidence/provenance.hpp"
#include "tobsv/evidence/quality.hpp"
#include "tobsv/model/generation.hpp"
#include "tobsv/model/vocabulary.hpp"

namespace tobsv {

// Temperatures outside this band are refused at ingest. Below absolute zero nothing is physical;
// far above it, the value is not a data-centre measurement and treating it as one would corrupt
// every aggregate that contains it.
inline constexpr double kMinPlausibleCelsius = kAbsoluteZeroCelsius;
inline constexpr double kMaxPlausibleCelsius = kPlausibilityCeilingCelsius;

// One temperature reading, exactly as it arrived, with everything needed to explain later why it
// was or was not used.
struct TemperatureObservation {
  ObservationId id;
  EntityId entity;
  SensorId sensor;
  MeasurementSite site = MeasurementSite::kUnknown;
  double celsius = 0.0;
  Timestamp observed_at;
  Timestamp received_at;
  Provenance provenance;
  QualityMetadata quality;
  // Envelope in force when the reading was taken. Unset is legal and means "the source did not say
  // which envelope applies"; headroom analysis then reports the gap as unsupported evidence rather
  // than inventing a limit.
  EnvelopeId envelope;
  Fence fence;

  Status validate(const Limits& limits) const;

  // Schema version of the durable encoding of this record.
  static constexpr unsigned kRecordVersion = 1U;
};

// Content-addressed identity. The digest covers everything that describes the measurement and
// nothing about its delivery (the receive time is excluded on purpose), so a genuine retry of the
// same measurement reproduces the same identity and is recognised as a duplicate instead of being
// counted twice.
ObservationId compute_observation_id(const TemperatureObservation& observation);

// How long evidence stays usable, how much clock skew is tolerated, and how far apart two readings
// of the same subject may be before the runtime calls them conflicting rather than agreeing.
struct FreshnessPolicy {
  Duration window = Duration::from_nanos(30LL * 1000 * 1000 * 1000);
  Duration max_clock_skew = Duration::from_nanos(2LL * 1000 * 1000 * 1000);
  double agreement_tolerance_c = 0.5;

  static FreshnessPolicy from_limits(const Limits& limits);
  Status validate() const;
};

// A compact view of one observation, copied out of the store so that a result never holds a pointer
// into mutable storage.
struct ObservationRef {
  ObservationId id;
  SourceId source;
  double celsius = 0.0;
  Timestamp observed_at;
  // Position of this reading in its source's fencing order. Two readings of one subject can share
  // an observation instant - a source may restate or correct a value - and the fence is what decides
  // which of them is current.
  Revision fence_revision;
  AuthorityLevel authority = AuthorityLevel::kUnknown;
  MeasurementSite site = MeasurementSite::kUnknown;
  bool quality_supplied = false;
  bool quality_degraded = false;
  bool synthetic = false;

  JsonValue to_json() const;
};

// The resolved state of one (entity, sensor, site) subject at one evaluation instant.
struct SubjectTemperature {
  EntityId entity;
  SensorId sensor;
  MeasurementSite site = MeasurementSite::kUnknown;
  EvidenceState state = EvidenceState::kUnknown;
  double representative_celsius = 0.0;
  Timestamp representative_at;
  Duration age;
  AuthorityLevel representative_authority = AuthorityLevel::kUnknown;
  bool representative_synthetic = false;
  bool representative_quality_supplied = false;
  bool representative_quality_degraded = false;
  ObservationId representative_id;

  std::vector<ObservationRef> supporting;
  std::vector<ObservationRef> conflicting;
  std::vector<ObservationRef> stale;
  // Fresh observations that a newer reading from the same source superseded. A source sending a
  // second reading of one subject replaces its own earlier reading; it does not contradict itself,
  // and only different sources can disagree about the same subject.
  std::vector<ObservationRef> superseded;
  // Observations that exist but cannot be placed on the timeline: no timestamp, or a timestamp
  // further into the future than the tolerated clock skew.
  std::vector<ObservationRef> indeterminate;

  std::string reason;

  bool is_usable() const noexcept { return state == EvidenceState::kFresh; }
  JsonValue to_json() const;
};

}  // namespace tobsv
