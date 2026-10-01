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
#include "tobsv/core/strong_id.hpp"
#include "tobsv/core/time.hpp"
#include "tobsv/evidence/evidence.hpp"
#include "tobsv/model/generation.hpp"

namespace tobsv {

// The central distinction of this runtime. Two entities whose temperatures move together are
// correlated. That is not the same claim as "heat travels from one to the other", and this type
// system refuses to let the first be used as the second.
enum class CouplingKind : std::uint8_t {
  kPhysicalAdjacency = 0,  // declared by a topology: the entities are adjacent, no thermal claim
  kSupportedThermal,       // asserted by an authority with citations that support propagation
  kCoincidental,           // observed to move together, explicitly NOT supported for propagation
};

std::string_view to_string(CouplingKind kind) noexcept;
bool parse_coupling_kind(std::string_view text, CouplingKind& out) noexcept;

enum class CouplingDirection : std::uint8_t {
  kSymmetric = 0,
  kFromTo,
};

std::string_view to_string(CouplingDirection direction) noexcept;

// What a relation rests on. A physical adjacency rests on a topology declaration; a thermal
// coupling rests on temperature observations. Both are citations, and neither is optional.
struct CouplingCitation {
  bool is_topology = false;
  ObservationId observation;
  TopologyRef topology;

  // Canonical ordering key for deterministic output.
  std::string key() const;
  friend bool operator<(const CouplingCitation& a, const CouplingCitation& b) {
    return a.key() < b.key();
  }
  friend bool operator==(const CouplingCitation& a, const CouplingCitation& b) {
    return a.is_topology == b.is_topology && a.observation == b.observation &&
           a.topology == b.topology;
  }
  JsonValue to_json() const;
};

struct CouplingRelation {
  CouplingId id;
  EntityId from;
  EntityId to;
  CouplingKind kind = CouplingKind::kCoincidental;
  CouplingDirection direction = CouplingDirection::kSymmetric;
  // Strength in (0, 1]; the weakest link of a path is the minimum of its edge strengths.
  double strength = 0.0;
  std::vector<CouplingCitation> citations;
  SourceId asserted_by;
  Timestamp asserted_at;
  Fence fence;
  std::string method;

  Status validate(const Limits& limits) const;

  // Coincidence is recorded and reported, but it is never traversed as if it were propagation.
  bool is_traversable() const noexcept { return kind != CouplingKind::kCoincidental; }
  bool allows(const EntityId& from_entity, const EntityId& to_entity) const;
  std::size_t distinct_observation_citations() const;
};

CouplingId compute_coupling_id(const CouplingRelation& relation);

class CouplingGraph {
 public:
  Status add(const CouplingRelation& relation, const Limits& limits);
  const CouplingRelation* find(const CouplingId& id) const;
  std::vector<CouplingId> ids() const;
  std::size_t size() const noexcept { return relations_.size(); }
  // Outgoing relations from an entity, ordered by relation identity.
  std::vector<const CouplingRelation*> outgoing(const EntityId& entity) const;
  std::vector<const CouplingRelation*> incident(const EntityId& entity) const;
  std::size_t coincidental_count() const;

 private:
  std::map<CouplingId, CouplingRelation> relations_;
  std::map<EntityId, std::vector<CouplingId>> from_index_;
  std::map<EntityId, std::vector<CouplingId>> to_index_;
};

struct PropagationPolicy {
  std::size_t max_depth = 4;
  std::size_t max_paths = 16;
  // Declared physical adjacency may be used as a path segment. It never becomes a thermal claim:
  // every path records the weakest edge kind it used.
  bool allow_adjacency = true;

  Status validate(const Limits& limits) const;
};

struct PropagationStep {
  EntityId from;
  EntityId to;
  CouplingId relation;
  CouplingKind kind = CouplingKind::kCoincidental;
  double strength = 0.0;
  JsonValue to_json() const;
};

struct PropagationPath {
  EntityId origin;
  EntityId terminus;
  std::vector<PropagationStep> steps;
  double path_strength = 0.0;
  CouplingKind weakest_kind = CouplingKind::kCoincidental;
  std::string reason;
  JsonValue to_json() const;
};

struct PropagationResult {
  EntityId origin;
  EvidenceState state = EvidenceState::kUnknown;
  std::vector<PropagationPath> paths;
  // Paths dropped because the configured bound was reached. Reported, never silent.
  std::size_t truncated_paths = 0;
  bool depth_limited = false;
  // True when the traversal stopped because its exploration budget ran out rather than because the
  // graph was exhausted. A bounded search is reported as bounded.
  bool search_bounded = false;
  std::size_t coincidental_excluded = 0;
  std::string reason;

  JsonValue to_json() const;
};

// Enumerates simple, bounded paths from an origin to any of the requested targets. Neighbour order
// is by coupling identity, so the enumeration is identical on every run.
Result<PropagationResult> find_propagation(const EntityId& origin,
                                           const std::vector<EntityId>& targets,
                                           const CouplingGraph& graph,
                                           const PropagationPolicy& policy, const Limits& limits);

}  // namespace tobsv
