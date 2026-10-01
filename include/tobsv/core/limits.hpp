// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "tobsv/core/result.hpp"
#include "tobsv/core/time.hpp"

namespace tobsv {

// Hard ceilings. A caller-supplied Limits value above a ceiling is rejected rather than clamped:
// silently reducing an operator's configured bound would make the runtime's resource behaviour
// unpredictable.
namespace ceiling {
inline constexpr std::size_t kEntities = 1U << 20;
inline constexpr std::size_t kSources = 1U << 20;
inline constexpr std::size_t kSensors = 1U << 21;
inline constexpr std::size_t kObservations = 1U << 22;
inline constexpr std::size_t kRelations = 1U << 22;
inline constexpr std::size_t kMembers = 1U << 18;
inline constexpr std::size_t kEpisodes = 1U << 14;
inline constexpr std::size_t kReasonSteps = 1U << 12;
inline constexpr std::size_t kTextLength = 4096;
inline constexpr std::size_t kCitations = 1U << 12;
inline constexpr std::size_t kCouplingDepth = 32;
inline constexpr std::size_t kPropagationPaths = 1U << 12;
inline constexpr std::size_t kJsonDepth = 64;
inline constexpr std::size_t kJsonNodes = 1U << 22;
inline constexpr std::size_t kJsonBytes = 64U << 20;
inline constexpr std::size_t kRecordBytes = 16U << 20;
inline constexpr std::size_t kSegmentBytes = 8ULL << 30;
inline constexpr std::size_t kQueueDepth = 1U << 16;
inline constexpr std::size_t kLoadedRecords = 1U << 24;
}  // namespace ceiling

// All resource bounds the runtime enforces. Every collection that can be driven by external input
// has a bound here, and every insertion path checks it.
struct Limits {
  // Evidence.
  std::size_t max_entities = 4096;
  // Distinct evidence sources tracked at once. Each source keeps a fencing high-water mark and a
  // retry identity, so that set has to be bounded like every other collection.
  std::size_t max_sources = 1024;
  std::size_t max_sensors = 8192;
  std::size_t max_observations = 65536;
  std::size_t max_observations_per_subject = 512;
  std::size_t max_sensors_per_entity = 32;

  // Observation topology (declared adjacency, not ownership).
  std::size_t max_topology_edges = 16384;
  std::size_t max_edges_per_entity = 64;

  // Envelopes.
  std::size_t max_envelopes = 4096;

  // Coupling evidence.
  std::size_t max_couplings = 16384;
  std::size_t max_citations_per_relation = 64;
  std::size_t max_coupling_depth = 8;
  std::size_t max_propagation_paths = 64;

  // Hotspot episodes.
  std::size_t max_hotspot_members = 1024;
  std::size_t max_hotspot_episodes = 128;

  // Derating evidence.
  std::size_t max_derating_evidence = 8192;

  // Attribution limits carried by a single analysis.
  std::size_t max_attribution_limits = 256;

  // Explanations.
  std::size_t max_reason_steps = 256;
  std::size_t max_text_length = 512;
  std::size_t max_citations = 128;

  // Structured input.
  std::size_t max_json_depth = 32;
  std::size_t max_json_nodes = 200000;
  std::size_t max_json_bytes = 8U << 20;

  // Persistence.
  std::size_t max_record_bytes = 1U << 20;
  std::size_t max_segment_bytes = 1U << 30;
  std::size_t max_commit_queue_depth = 256;
  std::size_t max_loaded_records = 200000;

  // Freshness and episode grouping defaults, in nanoseconds.
  Nanos default_freshness_window = 30LL * 1000 * 1000 * 1000;
  Nanos default_episode_gap = 120LL * 1000 * 1000 * 1000;

  Status validate() const;
};

// Compare a prospective collection size with its bound.
Status check_capacity(std::size_t current, std::size_t bound, std::string_view what);

// Text that reaches a durable record or a report has to be bounded and control-character free.
Status validate_text(std::string_view text, std::string_view what, std::size_t max_length);

}  // namespace tobsv
