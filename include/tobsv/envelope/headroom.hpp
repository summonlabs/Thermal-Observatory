// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tobsv/core/json.hpp"
#include "tobsv/core/limits.hpp"
#include "tobsv/core/result.hpp"
#include "tobsv/envelope/envelope.hpp"
#include "tobsv/evidence/evidence.hpp"

namespace tobsv {

// One distance-to-band measurement. The arithmetic is exact: headroom is the signed distance from
// the observed temperature to the band limit, so a negative headroom is an exceedance rather than a
// clamped zero.
struct HeadroomValue {
  ThresholdLevel level = ThresholdLevel::kNominal;
  double limit_c = 0.0;
  double observed_c = 0.0;
  double headroom_c = 0.0;
  bool exceeded = false;
  // Share of the declared usable band that has been consumed. Undefined when the envelope does not
  // declare a nominal band strictly below this limit, and the report says so rather than printing
  // a fabricated ratio.
  bool consumed_fraction_defined = false;
  double consumed_fraction = 0.0;

  JsonValue to_json() const;
};

struct HeadroomReport {
  EntityId entity;
  SensorId sensor;
  MeasurementSite site = MeasurementSite::kUnknown;
  EvidenceState state = EvidenceState::kUnknown;

  bool envelope_applied = false;
  EnvelopeId envelope;

  double observed_celsius = 0.0;
  Timestamp observed_at;
  ObservationId evidence;
  bool evidence_synthetic = false;

  bool current_level_known = false;
  ThresholdLevel current_level = ThresholdLevel::kNominal;

  std::vector<HeadroomValue> levels;
  // Bands that carry no headroom because the applicable envelope does not declare them, or because
  // no envelope applies at all. An empty headroom list is never silently equal to "fine".
  std::vector<ThresholdLevel> missing_levels;

  std::string reason;

  const HeadroomValue* level(ThresholdLevel wanted) const;
  JsonValue to_json() const;
};

// Computes headroom for one resolved subject against the envelope that applies to it.
Result<HeadroomReport> compute_headroom(const SubjectTemperature& subject, EntityClass entity_class,
                                        const EnvelopeRegistry& envelopes, const Limits& limits);

enum class ThresholdTransition : std::uint8_t {
  kNone = 0,        // no previous observation was available to compare against
  kUnchanged,
  kEntered,         // crossed out of the nominal band into a hotter one
  kExited,          // returned to the nominal band
  kEscalated,       // moved to a hotter band, having already been outside nominal
  kDeescalated,     // moved to a colder band but still outside nominal
  kUnknown,
};

std::string_view to_string(ThresholdTransition transition) noexcept;

struct ThresholdTransitionRecord {
  EntityId entity;
  SensorId sensor;
  MeasurementSite site = MeasurementSite::kUnknown;
  ThresholdTransition transition = ThresholdTransition::kNone;
  ThresholdLevel previous = ThresholdLevel::kNominal;
  ThresholdLevel current = ThresholdLevel::kNominal;
  Timestamp evaluated_at;
  ObservationId evidence;
  std::string reason;

  JsonValue to_json() const;
};

ThresholdTransitionRecord classify_transition(bool have_previous, ThresholdLevel previous,
                                              const HeadroomReport& report,
                                              const Timestamp& evaluated_at);

}  // namespace tobsv
