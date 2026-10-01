// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "tobsv/core/checked.hpp"
#include "tobsv/core/json.hpp"
#include "tobsv/core/limits.hpp"
#include "tobsv/core/result.hpp"
#include "tobsv/core/strong_id.hpp"
#include "tobsv/core/time.hpp"
#include "tobsv/model/vocabulary.hpp"

namespace tobsv {

// A declared thermal envelope. The runtime does not author envelopes and does not own the policy
// behind them: it records what an authority declared, applies it to observations, and reports the
// distance to each band. Absent bands are absent, not defaulted.
struct ThermalEnvelope {
  EnvelopeId id;
  EntityId entity;                 // unset means the envelope applies to a whole class
  EntityClass entity_class = EntityClass::kUnknown;
  MeasurementSite site = MeasurementSite::kUnknown;  // unset means any site of the entity

  bool has_nominal = true;
  double nominal_c = 0.0;
  bool has_warn = false;
  double warn_c = 0.0;
  bool has_high = false;
  double high_c = 0.0;
  bool has_critical = false;
  double critical_c = 0.0;
  bool has_maximum = true;
  double maximum_c = 0.0;

  SourceId declared_by;
  Timestamp declared_at;
  std::string basis;

  bool has_level(ThresholdLevel level) const;
  // Only meaningful when has_level(level) is true; otherwise returns 0.
  double limit(ThresholdLevel level) const;

  Status validate(const Limits& limits) const;
};

EnvelopeId compute_envelope_id(const ThermalEnvelope& envelope);

// The band a temperature sits in for a given envelope: the hottest declared band whose limit the
// temperature has reached, or the nominal band when it has reached none. Returns false when the
// temperature is not a finite number, in which case no band can be claimed.
inline bool band_of(const ThermalEnvelope& envelope, double celsius, ThresholdLevel& out) noexcept {
  if (!checked::is_finite(celsius)) {
    return false;
  }
  ThresholdLevel band = ThresholdLevel::kNominal;
  const ThresholdLevel order[5] = {ThresholdLevel::kNominal, ThresholdLevel::kWarn,
                                   ThresholdLevel::kHigh, ThresholdLevel::kCritical,
                                   ThresholdLevel::kMaximum};
  for (const ThresholdLevel level : order) {
    if (envelope.has_level(level) && celsius >= envelope.limit(level)) {
      band = level;
    }
  }
  out = band;
  return true;
}

// Envelope lookup. Selection is by specificity and is total: for any subject there is at most one
// winner, and ties are broken by the lexicographically smallest envelope identity.
class EnvelopeRegistry {
 public:
  Status add(const ThermalEnvelope& envelope, const Limits& limits);
  const ThermalEnvelope* find(const EnvelopeId& id) const;
  // Returns nullptr when no envelope applies. The caller reports that as unsupported evidence.
  const ThermalEnvelope* select(const EntityId& entity, EntityClass entity_class,
                                MeasurementSite site) const;
  std::vector<EnvelopeId> ids() const;
  std::size_t size() const noexcept { return envelopes_.size(); }

 private:
  std::map<EnvelopeId, ThermalEnvelope> envelopes_;
};

}  // namespace tobsv
