// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "tobsv/core/limits.hpp"
#include "tobsv/core/result.hpp"
#include "tobsv/evidence/evidence.hpp"
#include "tobsv/model/generation.hpp"

namespace tobsv {

enum class IngestKind : std::uint8_t {
  kRecorded = 0,  // newly recorded
  kDuplicate,     // the very same observation, already recorded; idempotent no-op
  kRefused,       // rejected; the observation is not in the store
  kConflict,      // rejected because it contradicts the recorded position
};

std::string_view to_string(IngestKind kind) noexcept;

struct IngestOutcome {
  IngestKind kind = IngestKind::kRefused;
  Status status;
  ObservationId id;
  Fence high_water;

  bool accepted() const noexcept {
    return kind == IngestKind::kRecorded || kind == IngestKind::kDuplicate;
  }
  JsonValue to_json() const;
};

// Canonical ordering of a resolvable subject: one sensor of one entity at one measurement site.
struct SubjectKey {
  EntityId entity;
  SensorId sensor;
  MeasurementSite site = MeasurementSite::kUnknown;
};

bool operator<(const SubjectKey& a, const SubjectKey& b);

// The bounded, in-memory record of everything this runtime has been told. It owns no authority: it
// records, orders, refuses and explains.
class EvidenceStore {
 public:
  explicit EvidenceStore(const Limits& limits);

  const Limits& limits() const noexcept { return limits_; }
  const Status& configuration_status() const noexcept { return configuration_status_; }

  // Records an observation. Deterministic and idempotent: replaying the same observation returns
  // kDuplicate and changes nothing.
  IngestOutcome ingest(const TemperatureObservation& observation);

  const TemperatureObservation* find(const ObservationId& id) const;
  bool has_observation(const ObservationId& id) const;

  std::size_t observation_count() const noexcept { return observations_.size(); }
  std::size_t retired_count() const noexcept { return retired_; }
  std::size_t refused_count() const noexcept { return refused_; }
  std::size_t duplicate_count() const noexcept { return duplicates_; }
  std::size_t conflict_count() const noexcept { return conflicts_; }
  std::size_t entity_count() const noexcept { return sensor_index_.size(); }
  std::size_t subject_count() const noexcept { return subjects_.size(); }
  std::size_t sensor_count() const;

  std::vector<EntityId> entities() const;
  std::vector<SensorId> sensors() const;
  std::vector<ObservationId> observation_ids() const;
  // True when ingesting one more observation of this subject would retire its oldest one. The
  // runtime uses this to reserve the extra durable slot a retention checkpoint needs.
  bool would_retire(const EntityId& entity, const SensorId& sensor, MeasurementSite site) const;

  std::vector<ObservationId> observations_for(const EntityId& entity) const;
  std::vector<SensorId> sensors_for(const EntityId& entity) const;
  std::vector<SubjectKey> subjects_for(const EntityId& entity) const;

  // The mark for one source, without walking every other source. The retention path uses this so
  // that checkpointing a fence does not cost time proportional to the number of sources seen.
  const Fence* high_water_for(const SourceId& source) const;

  std::vector<Fence> high_water_marks() const;
  Status restore_high_water(const Fence& fence);

  // Resolution. An unknown subject is not an error: it resolves to EvidenceState::kUnknown with a
  // reason that names the missing subject.
  Result<SubjectTemperature> resolve(const EntityId& entity, const SensorId& sensor,
                                     MeasurementSite site, const Timestamp& evaluated_at,
                                     const FreshnessPolicy& policy) const;

  Result<std::vector<SubjectTemperature>> resolve_entity(const EntityId& entity,
                                                         const Timestamp& evaluated_at,
                                                         const FreshnessPolicy& policy) const;

  Result<std::vector<SubjectTemperature>> resolve_all(const Timestamp& evaluated_at,
                                                      const FreshnessPolicy& policy) const;

  // Every subject whose resolved state is usable, in canonical order.
  Result<std::vector<SubjectTemperature>> resolve_usable(const Timestamp& evaluated_at,
                                                         const FreshnessPolicy& policy) const;

  std::vector<TemperatureObservation> observations() const;

 private:
  struct SubjectEntry {
    std::vector<ObservationId> ordered;  // ascending by (observed_at, id)
  };

  ObservationRef make_ref(const TemperatureObservation& observation) const;
  void retire_front(const SubjectKey& key);

  Limits limits_;
  Status configuration_status_;
  std::map<ObservationId, TemperatureObservation> observations_;
  std::map<SubjectKey, SubjectEntry> subjects_;
  std::map<EntityId, std::map<SensorId, std::size_t>> sensor_index_;
  std::map<SourceId, FenceTracker> trackers_;
  std::map<SourceId, ObservationId> last_accepted_;
  std::size_t retired_ = 0;
  std::size_t refused_ = 0;
  std::size_t duplicates_ = 0;
  std::size_t conflicts_ = 0;
};

}  // namespace tobsv
