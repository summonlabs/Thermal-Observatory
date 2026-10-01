// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "tobsv/core/limits.hpp"
#include "tobsv/core/result.hpp"
#include "tobsv/core/strong_id.hpp"

namespace tobsv {

// How two entities are adjacent for observation purposes. This is a statement about where heat can
// travel or where a sensor sits, and nothing more: it is not a thermal zone, not a cooling boundary,
// not an ownership claim and not a placement decision. Those belong to other runtimes.
enum class AdjacencyKind : std::uint8_t {
  kDeclaredUnknown = 0,
  kPhysicalContainment,   // one entity sits inside the other (node in rack, die in package)
  kSharedEnclosure,       // both sit in the same enclosure without containment
  kCoolantPathProximity,  // both sit on the same coolant path, upstream or downstream
  kAirflowPathProximity,  // both sit in the same airflow path
};

std::string_view to_string(AdjacencyKind kind) noexcept;
bool parse_adjacency_kind(std::string_view text, AdjacencyKind& out) noexcept;

struct TopologyEdge {
  EntityId from;
  EntityId to;
  AdjacencyKind kind = AdjacencyKind::kDeclaredUnknown;
  TopologyRef topology;
  SourceId declared_by;

  Status validate() const;
};

// Declared observation adjacency. Adjacency is undirected for grouping: heat does not care which end
// of a declared proximity a sensor sits on. Only coupling relations carry direction.
class ThermalTopology {
 public:
  ThermalTopology() = default;

  Status add_entity(const EntityId& entity, const Limits& limits);
  Status add_edge(const TopologyEdge& edge, const Limits& limits);

  bool has_entity(const EntityId& entity) const;
  std::size_t entity_count() const noexcept { return entities_.size(); }
  std::size_t edge_count() const noexcept { return edges_.size(); }

  std::vector<EntityId> entities() const;
  std::vector<EntityId> neighbors(const EntityId& entity) const;
  std::vector<TopologyEdge> edges() const;

  // Adjacency kind of the first declared edge joining the two entities, or kDeclaredUnknown when
  // there is none. Provided so that explanations can name the adjacency they used.
  AdjacencyKind adjacency_between(const EntityId& a, const EntityId& b) const;

 private:
  struct EdgeKey {
    EntityId low;
    EntityId high;
    AdjacencyKind kind = AdjacencyKind::kDeclaredUnknown;
    friend bool operator<(const EdgeKey& a, const EdgeKey& b) {
      if (a.low != b.low) return a.low < b.low;
      if (a.high != b.high) return a.high < b.high;
      return static_cast<unsigned>(a.kind) < static_cast<unsigned>(b.kind);
    }
  };

  std::vector<EntityId> entities_;
  std::vector<EdgeKey> index_;
  std::vector<TopologyEdge> edges_;
};

}  // namespace tobsv
