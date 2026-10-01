// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "tobsv/attribution/attribution.hpp"
#include "tobsv/core/json.hpp"
#include "tobsv/core/limits.hpp"
#include "tobsv/coupling/coupling.hpp"
#include "tobsv/derating/derating.hpp"
#include "tobsv/envelope/headroom.hpp"
#include "tobsv/evidence/store.hpp"
#include "tobsv/hotspot/hotspot.hpp"
#include "tobsv/model/inventory.hpp"
#include "tobsv/model/topology.hpp"

namespace tobsv {

// Remembers the band each subject was last reported in, so that a threshold transition is a real
// transition between two evaluations rather than a guess.
class TransitionMemory {
 public:
  bool lookup(const SubjectKey& key, ThresholdLevel& out) const;
  void record(const SubjectKey& key, ThresholdLevel level);
  std::size_t size() const noexcept { return levels_.size(); }
  void clear() { levels_.clear(); }

  // Drops the memory of subjects that no longer appear in an analysis once the map is over its
  // bound. Without this a runtime that keeps meeting new subjects would remember every one of them
  // for the lifetime of the process, which is an unbounded resource.
  void retain(const std::vector<SubjectKey>& live, std::size_t bound);

 private:
  std::map<SubjectKey, ThresholdLevel> levels_;
};

struct ThermalQuery {
  Timestamp evaluated_at;
  FreshnessPolicy freshness;
  HotspotPolicy hotspots;
  PropagationPolicy propagation;
  ThresholdLevel focus = ThresholdLevel::kWarn;
  bool include_propagation = true;
  // Explicit targets for the propagation search. When empty, the other hot entities are used.
  std::vector<EntityId> propagation_targets;

  Status validate(const Limits& limits) const;
};

struct ThermalAnalysis {
  std::string digest;
  Timestamp evaluated_at;
  EvidenceState state = EvidenceState::kUnknown;

  std::size_t subject_count = 0;
  std::size_t usable_subject_count = 0;
  std::size_t conflicted_subject_count = 0;
  std::size_t stale_subject_count = 0;
  std::size_t unknown_subject_count = 0;

  std::vector<HeadroomReport> headroom;
  std::vector<ThresholdTransitionRecord> transitions;
  HotspotResult hotspots;
  PropagationResult propagation;
  std::vector<DeratingAppraisal> deratings;
  AttributionLedger attribution;
  std::vector<std::string> reason_steps;

  JsonValue to_json() const;
};

// Composes the answer to the core question: where heat is accumulating, how much headroom remains,
// which hotspots and propagation paths the evidence supports, what is derated, and where the
// attribution is uncertain.
Result<ThermalAnalysis> analyze(const ThermalQuery& query, const EvidenceStore& store,
                                const ThermalInventory& inventory,
                                const EnvelopeRegistry& envelopes, const ThermalTopology& topology,
                                const CouplingGraph& coupling, const DeratingRegistry& derating,
                                TransitionMemory& memory, const Limits& limits);

}  // namespace tobsv
