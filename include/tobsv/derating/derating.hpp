// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "tobsv/core/json.hpp"
#include "tobsv/core/limits.hpp"
#include "tobsv/core/result.hpp"
#include "tobsv/envelope/envelope.hpp"
#include "tobsv/evidence/evidence.hpp"
#include "tobsv/evidence/store.hpp"
#include "tobsv/model/generation.hpp"
#include "tobsv/model/inventory.hpp"

namespace tobsv {

enum class DeratingKind : std::uint8_t {
  kUnknown = 0,
  kClockThrottle,
  kFrequencyCap,
  kPowerLimit,
  kCapacityReduction,
  kFeatureDisable,
};

std::string_view to_string(DeratingKind kind) noexcept;
bool parse_derating_kind(std::string_view text, DeratingKind& out) noexcept;

// A report that an entity is being held below its capability for thermal reasons. This runtime does
// not decide derating, does not enact it and does not own the policy behind it: it records the
// claim, checks that the thermal evidence cited actually reaches the band the claim names, and
// explains the result.
struct DeratingEvidence {
  DeratingId id;
  EntityId entity;
  SensorId sensor;  // may be unset when the claim is about the entity rather than one subject
  MeasurementSite site = MeasurementSite::kUnknown;
  DeratingKind kind = DeratingKind::kUnknown;
  double magnitude = 0.0;  // fractional reduction, in (0, 1]
  Timestamp observed_at;
  Timestamp asserted_at;
  SourceId asserted_by;
  SourceKind source_kind = SourceKind::kUnknown;
  std::vector<ObservationId> citations;
  ThresholdLevel trigger_level = ThresholdLevel::kWarn;
  Fence fence;
  std::string method;

  Status validate(const Limits& limits) const;
  bool is_synthetic() const noexcept { return source_kind == SourceKind::kSyntheticGenerator; }
};

DeratingId compute_derating_id(const DeratingEvidence& evidence);

class DeratingRegistry {
 public:
  Status add(const DeratingEvidence& evidence, const Limits& limits);
  const DeratingEvidence* find(const DeratingId& id) const;
  std::vector<const DeratingEvidence*> for_entity(const EntityId& entity) const;
  std::vector<DeratingId> ids() const;
  std::size_t size() const noexcept { return entries_.size(); }

 private:
  std::map<DeratingId, DeratingEvidence> entries_;
  std::map<EntityId, std::vector<DeratingId>> by_entity_;
};

struct DeratingEntry {
  DeratingEvidence evidence;
  EvidenceState state = EvidenceState::kUnknown;
  ThresholdLevel observed_band = ThresholdLevel::kNominal;
  bool observed_band_known = false;
  double peak_citation_celsius = 0.0;
  std::vector<ObservationId> missing_citations;
  std::vector<ObservationId> stale_citations;
  std::vector<ObservationId> cold_citations;
  std::string reason;

  JsonValue to_json() const;
};

struct DeratingAppraisal {
  EntityId entity;
  EvidenceState state = EvidenceState::kUnknown;
  std::vector<DeratingEntry> entries;
  std::string reason;

  JsonValue to_json() const;
};

// Checks every derating claim recorded against an entity against the thermal evidence it cites.
Result<DeratingAppraisal> appraise_derating(const EntityId& entity,
                                            const DeratingRegistry& registry,
                                            const EvidenceStore& store,
                                            const ThermalInventory& inventory,
                                            const EnvelopeRegistry& envelopes,
                                            const Timestamp& evaluated_at,
                                            const FreshnessPolicy& freshness,
                                            const Limits& limits);

}  // namespace tobsv
