// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tobsv/core/json.hpp"
#include "tobsv/core/limits.hpp"
#include "tobsv/core/result.hpp"
#include "tobsv/coupling/coupling.hpp"
#include "tobsv/envelope/headroom.hpp"
#include "tobsv/model/topology.hpp"

namespace tobsv {

struct HotspotPolicy {
  // Coldest band that counts as hot. The band is read from the envelope; it is not a magic number.
  ThresholdLevel threshold = ThresholdLevel::kWarn;
  // Members of one episode must have been observed within this window of each other. Two entities
  // that were hot hours apart are not one episode.
  Duration episode_gap = Duration::from_nanos(120LL * 1000 * 1000 * 1000);
  // Declared physical adjacency may join two hot entities. Coincidental coupling never may.
  bool allow_adjacency = true;

  Status validate(const Limits& limits) const;
};

struct HotspotMember {
  EntityId entity;
  SensorId sensor;
  MeasurementSite site = MeasurementSite::kUnknown;
  double celsius = 0.0;
  ThresholdLevel level = ThresholdLevel::kNominal;
  double excursion_c = 0.0;
  ObservationId evidence;
  Timestamp observed_at;
  bool synthetic = false;
  bool quality_degraded = false;

  JsonValue to_json() const;
};

struct HotspotEpisode {
  EpisodeId id;
  ThresholdLevel threshold = ThresholdLevel::kWarn;
  std::vector<HotspotMember> members;  // ascending by entity identity
  double peak_c = 0.0;
  EntityId seed;                       // lexicographically smallest member
  Timestamp observed_at;               // latest member observation
  Timestamp earliest_at;
  // How many member pairs are joined by a supported relation rather than only by adjacency.
  std::size_t supported_links = 0;
  std::size_t adjacency_links = 0;
  std::vector<CouplingId> support_relations;
  std::string reason;

  JsonValue to_json() const;
};

struct HotspotResult {
  EvidenceState state = EvidenceState::kUnknown;
  std::vector<HotspotEpisode> episodes;   // descending peak, then ascending seed
  std::vector<EntityId> ungrouped;        // hot entities that joined nothing
  std::size_t evaluable_subjects = 0;
  std::size_t hot_subjects = 0;
  std::size_t dropped_episodes = 0;
  std::string reason;

  JsonValue to_json() const;
};

// Groups hot subjects into episodes. Members of an episode are connected through traversable
// coupling or declared adjacency, and their observations fall inside the episode gap.
Result<HotspotResult> group_hotspots(const std::vector<HeadroomReport>& headroom,
                                     const ThermalTopology& topology, const CouplingGraph& coupling,
                                     const HotspotPolicy& policy, const Limits& limits);

EpisodeId compute_episode_id(const std::vector<HotspotMember>& members, ThresholdLevel threshold);

}  // namespace tobsv
