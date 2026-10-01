// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/model/topology.hpp"

#include <algorithm>

namespace tobsv {

std::string_view to_string(AdjacencyKind kind) noexcept {
  switch (kind) {
    case AdjacencyKind::kDeclaredUnknown: return "declared_unknown";
    case AdjacencyKind::kPhysicalContainment: return "physical_containment";
    case AdjacencyKind::kSharedEnclosure: return "shared_enclosure";
    case AdjacencyKind::kCoolantPathProximity: return "coolant_path_proximity";
    case AdjacencyKind::kAirflowPathProximity: return "airflow_path_proximity";
  }
  return "declared_unknown";
}

bool parse_adjacency_kind(std::string_view text, AdjacencyKind& out) noexcept {
  if (text == "declared_unknown") { out = AdjacencyKind::kDeclaredUnknown; return true; }
  if (text == "physical_containment") { out = AdjacencyKind::kPhysicalContainment; return true; }
  if (text == "shared_enclosure") { out = AdjacencyKind::kSharedEnclosure; return true; }
  if (text == "coolant_path_proximity") { out = AdjacencyKind::kCoolantPathProximity; return true; }
  if (text == "airflow_path_proximity") { out = AdjacencyKind::kAirflowPathProximity; return true; }
  return false;
}

Status TopologyEdge::validate() const {
  if (!from.is_set() || !to.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "topology edge requires both endpoints");
  }
  if (from == to) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "topology edge joins " + from.value() + " to itself");
  }
  if (kind == AdjacencyKind::kDeclaredUnknown) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "topology edge between " + from.value() + " and " + to.value() +
                               " must declare an adjacency kind");
  }
  if (!topology.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "topology edge between " + from.value() + " and " + to.value() +
                               " must cite the topology it was declared by");
  }
  if (!declared_by.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "topology edge between " + from.value() + " and " + to.value() +
                               " must name the source that declared it");
  }
  return Status::success();
}

Status ThermalTopology::add_entity(const EntityId& entity, const Limits& limits) {
  if (!entity.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "topology entity identity is empty");
  }
  if (std::binary_search(entities_.begin(), entities_.end(), entity)) {
    return Status::failure(ErrorCode::kDuplicateIdentity,
                           "entity " + entity.value() + " is already in the observation topology");
  }
  const Status capacity = check_capacity(entities_.size(), limits.max_entities, "entities");
  if (!capacity.ok()) {
    return capacity;
  }
  entities_.insert(std::lower_bound(entities_.begin(), entities_.end(), entity), entity);
  return Status::success();
}

Status ThermalTopology::add_edge(const TopologyEdge& edge, const Limits& limits) {
  const Status valid = edge.validate();
  if (!valid.ok()) {
    return valid;
  }
  EdgeKey key;
  key.low = edge.from < edge.to ? edge.from : edge.to;
  key.high = edge.from < edge.to ? edge.to : edge.from;
  key.kind = edge.kind;
  if (std::binary_search(index_.begin(), index_.end(), key)) {
    return Status::failure(ErrorCode::kDuplicateIdentity,
                           "adjacency " + key.low.value() + "-" + key.high.value() + " of kind " +
                               std::string(to_string(edge.kind)) + " is already declared");
  }
  const Status capacity = check_capacity(edges_.size(), limits.max_topology_edges, "topology edges");
  if (!capacity.ok()) {
    return capacity;
  }
  std::size_t degree_low = 0;
  std::size_t degree_high = 0;
  for (const EdgeKey& existing : index_) {
    if (existing.low == key.low || existing.high == key.low) ++degree_low;
    if (existing.low == key.high || existing.high == key.high) ++degree_high;
  }
  if (degree_low >= limits.max_edges_per_entity || degree_high >= limits.max_edges_per_entity) {
    return Status::failure(ErrorCode::kLimitExceeded,
                           "adjacency degree bound of " +
                               std::to_string(limits.max_edges_per_entity) + " reached for " +
                               (degree_low >= limits.max_edges_per_entity ? key.low.value()
                                                                          : key.high.value()));
  }
  index_.insert(std::lower_bound(index_.begin(), index_.end(), key), key);
  edges_.push_back(edge);
  return Status::success();
}

bool ThermalTopology::has_entity(const EntityId& entity) const {
  return std::binary_search(entities_.begin(), entities_.end(), entity);
}

std::vector<EntityId> ThermalTopology::entities() const { return entities_; }

std::vector<EntityId> ThermalTopology::neighbors(const EntityId& entity) const {
  std::vector<EntityId> out;
  for (const EdgeKey& key : index_) {
    if (key.low == entity) {
      out.push_back(key.high);
    } else if (key.high == entity) {
      out.push_back(key.low);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<TopologyEdge> ThermalTopology::edges() const { return edges_; }

AdjacencyKind ThermalTopology::adjacency_between(const EntityId& a, const EntityId& b) const {
  const EntityId low = a < b ? a : b;
  const EntityId high = a < b ? b : a;
  for (const EdgeKey& key : index_) {
    if (key.low == low && key.high == high) {
      return key.kind;
    }
  }
  return AdjacencyKind::kDeclaredUnknown;
}

}  // namespace tobsv
