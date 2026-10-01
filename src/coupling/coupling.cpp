// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/coupling/coupling.hpp"

#include <algorithm>

#include "tobsv/core/checked.hpp"
#include "tobsv/core/format.hpp"
#include "tobsv/core/hash.hpp"

namespace tobsv {
namespace {

int kind_rank(CouplingKind kind) {
  switch (kind) {
    case CouplingKind::kSupportedThermal: return 2;
    case CouplingKind::kPhysicalAdjacency: return 1;
    case CouplingKind::kCoincidental: return 0;
  }
  return 0;
}

constexpr std::size_t kMinVisits = 1024;

}  // namespace

std::string_view to_string(CouplingKind kind) noexcept {
  switch (kind) {
    case CouplingKind::kPhysicalAdjacency: return "physical_adjacency";
    case CouplingKind::kSupportedThermal: return "supported_thermal";
    case CouplingKind::kCoincidental: return "coincidental";
  }
  return "coincidental";
}

bool parse_coupling_kind(std::string_view text, CouplingKind& out) noexcept {
  if (text == "physical_adjacency") { out = CouplingKind::kPhysicalAdjacency; return true; }
  if (text == "supported_thermal") { out = CouplingKind::kSupportedThermal; return true; }
  if (text == "coincidental") { out = CouplingKind::kCoincidental; return true; }
  return false;
}

std::string_view to_string(CouplingDirection direction) noexcept {
  switch (direction) {
    case CouplingDirection::kSymmetric: return "symmetric";
    case CouplingDirection::kFromTo: return "from_to";
  }
  return "symmetric";
}

std::string CouplingCitation::key() const {
  return (is_topology ? "t:" : "o:") + (is_topology ? topology.value() : observation.value());
}

JsonValue CouplingCitation::to_json() const {
  JsonValue out = JsonValue::object();
  if (is_topology) {
    out.set("topology", JsonValue::text(topology.value()));
  } else {
    out.set("observation", JsonValue::text(observation.value()));
  }
  return out;
}

std::size_t CouplingRelation::distinct_observation_citations() const {
  std::vector<ObservationId> ids;
  for (const CouplingCitation& citation : citations) {
    if (!citation.is_topology && citation.observation.is_set()) {
      ids.push_back(citation.observation);
    }
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids.size();
}

Status CouplingRelation::validate(const Limits& limits) const {
  if (!id.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "coupling relation has no identity");
  }
  if (!from.is_set() || !to.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "coupling relation " + id.value() + " requires both endpoints");
  }
  if (from == to) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "coupling relation " + id.value() + " joins " + from.value() +
                               " to itself");
  }
  if (!asserted_by.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "coupling relation " + id.value() + " names no asserting source");
  }
  if (!asserted_at.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "coupling relation " + id.value() + " has no assertion timestamp");
  }
  const Status fence_status = fence.validate();
  if (!fence_status.ok()) {
    return fence_status;
  }
  if (fence.source != asserted_by) {
    return Status::failure(ErrorCode::kConflict,
                           "coupling relation " + id.value() + " is fenced by source " +
                               fence.source.value() + " but attributed to " +
                               asserted_by.value());
  }
  const Status method_status = validate_text(method, "coupling method", limits.max_text_length);
  if (!method_status.ok()) {
    return method_status;
  }
  if (!checked::is_finite(strength) || strength <= 0.0 || strength > 1.0) {
    return Status::failure(ErrorCode::kOutOfRange,
                           "coupling relation " + id.value() +
                               " must declare a strength in (0, 1]");
  }
  const Status capacity =
      check_capacity(citations.size(), limits.max_citations_per_relation, "coupling citations");
  if (!capacity.ok()) {
    return capacity;
  }
  if (citations.empty()) {
    return Status::failure(ErrorCode::kUnsupported,
                           "coupling relation " + id.value() +
                               " carries no citation; an uncited relation is not evidence");
  }
  std::size_t topology_citations = 0;
  for (const CouplingCitation& citation : citations) {
    if (citation.is_topology) {
      if (!citation.topology.is_set()) {
        return Status::failure(ErrorCode::kInvalidArgument,
                               "coupling relation " + id.value() +
                                   " carries an empty topology citation");
      }
      ++topology_citations;
    } else if (!citation.observation.is_set()) {
      return Status::failure(ErrorCode::kInvalidArgument,
                             "coupling relation " + id.value() +
                                 " carries an empty observation citation");
    }
  }

  if (kind == CouplingKind::kPhysicalAdjacency) {
    if (topology_citations == 0) {
      return Status::failure(ErrorCode::kUnsupported,
                             "adjacency relation " + id.value() +
                                 " must cite the topology declaration it rests on");
    }
    return Status::success();
  }

  // Both remaining kinds are claims about thermal behaviour, and a claim about co-movement needs at
  // least two distinct observations. Temporal coincidence of a single sample pair is not evidence.
  const std::size_t observations = distinct_observation_citations();
  if (observations < 2) {
    return Status::failure(
        ErrorCode::kUnsupported,
        "relation " + id.value() + " cites " + std::to_string(observations) +
            " distinct observation(s); a thermal coupling claim requires at least two");
  }
  return Status::success();
}

bool CouplingRelation::allows(const EntityId& from_entity, const EntityId& to_entity) const {
  if (direction == CouplingDirection::kSymmetric) {
    return (from == from_entity && to == to_entity) || (from == to_entity && to == from_entity);
  }
  return from == from_entity && to == to_entity;
}

CouplingId compute_coupling_id(const CouplingRelation& relation) {
  StableDigest digest;
  digest.absorb_text("tobsv.coupling.v1");
  digest.absorb_text(relation.from.value());
  digest.absorb_text(relation.to.value());
  digest.absorb_number(static_cast<std::uint64_t>(relation.kind));
  digest.absorb_number(static_cast<std::uint64_t>(relation.direction));
  digest.absorb_real(relation.strength);
  std::vector<std::string> keys;
  keys.reserve(relation.citations.size());
  for (const CouplingCitation& citation : relation.citations) {
    keys.push_back(citation.key());
  }
  std::sort(keys.begin(), keys.end());
  for (const std::string& key : keys) {
    digest.absorb_text(key);
  }
  digest.absorb_text(relation.asserted_by.value());
  return CouplingId::from_digest(digest.value());
}

Status CouplingGraph::add(const CouplingRelation& relation, const Limits& limits) {
  const Status valid = relation.validate(limits);
  if (!valid.ok()) {
    return valid;
  }
  const CouplingId derived = compute_coupling_id(relation);
  if (derived != relation.id) {
    return Status::failure(ErrorCode::kConflict,
                           "coupling identity " + relation.id.value() +
                               " does not match its content, which hashes to " + derived.value());
  }
  if (relations_.find(relation.id) != relations_.end()) {
    return Status::failure(ErrorCode::kDuplicateIdentity,
                           "coupling relation " + relation.id.value() + " is already recorded");
  }
  const Status capacity = check_capacity(relations_.size(), limits.max_couplings, "couplings");
  if (!capacity.ok()) {
    return capacity;
  }
  // A symmetric relation is indexed under both endpoints, so the degree is the number of distinct
  // incident relations rather than the sum of the two index lists.
  const auto degree = [this](const EntityId& entity) {
    std::vector<CouplingId> incident;
    const auto from_it = from_index_.find(entity);
    if (from_it != from_index_.end()) {
      incident.insert(incident.end(), from_it->second.begin(), from_it->second.end());
    }
    const auto to_it = to_index_.find(entity);
    if (to_it != to_index_.end()) {
      incident.insert(incident.end(), to_it->second.begin(), to_it->second.end());
    }
    std::sort(incident.begin(), incident.end());
    incident.erase(std::unique(incident.begin(), incident.end()), incident.end());
    return incident.size();
  };
  if (degree(relation.from) >= limits.max_edges_per_entity ||
      degree(relation.to) >= limits.max_edges_per_entity) {
    return Status::failure(ErrorCode::kLimitExceeded,
                           "coupling degree bound of " +
                               std::to_string(limits.max_edges_per_entity) + " reached at " +
                               (degree(relation.from) >= limits.max_edges_per_entity
                                    ? relation.from.value()
                                    : relation.to.value()));
  }

  relations_.emplace(relation.id, relation);
  from_index_[relation.from].push_back(relation.id);
  if (relation.direction == CouplingDirection::kSymmetric && relation.to != relation.from) {
    from_index_[relation.to].push_back(relation.id);
  }
  to_index_[relation.to].push_back(relation.id);
  if (relation.direction == CouplingDirection::kSymmetric && relation.to != relation.from) {
    to_index_[relation.from].push_back(relation.id);
  }
  return Status::success();
}

const CouplingRelation* CouplingGraph::find(const CouplingId& id) const {
  const auto it = relations_.find(id);
  return it == relations_.end() ? nullptr : &it->second;
}

std::vector<CouplingId> CouplingGraph::ids() const {
  std::vector<CouplingId> out;
  out.reserve(relations_.size());
  for (const auto& entry : relations_) {
    out.push_back(entry.first);
  }
  return out;
}

std::vector<const CouplingRelation*> CouplingGraph::outgoing(const EntityId& entity) const {
  std::vector<const CouplingRelation*> out;
  const auto from_it = from_index_.find(entity);
  if (from_it != from_index_.end()) {
    for (const CouplingId& id : from_it->second) {
      const auto relation_it = relations_.find(id);
      if (relation_it != relations_.end()) {
        out.push_back(&relation_it->second);
      }
    }
  }
  const auto to_it = to_index_.find(entity);
  if (to_it != to_index_.end()) {
    for (const CouplingId& id : to_it->second) {
      const auto relation_it = relations_.find(id);
      if (relation_it == relations_.end()) {
        continue;
      }
      if (relation_it->second.direction != CouplingDirection::kSymmetric) {
        continue;
      }
      out.push_back(&relation_it->second);
    }
  }
  std::sort(out.begin(), out.end(),
            [](const CouplingRelation* a, const CouplingRelation* b) { return a->id < b->id; });
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<const CouplingRelation*> CouplingGraph::incident(const EntityId& entity) const {
  std::vector<const CouplingRelation*> out;
  const auto from_it = from_index_.find(entity);
  if (from_it != from_index_.end()) {
    for (const CouplingId& id : from_it->second) {
      const auto relation_it = relations_.find(id);
      if (relation_it != relations_.end()) {
        out.push_back(&relation_it->second);
      }
    }
  }
  const auto to_it = to_index_.find(entity);
  if (to_it != to_index_.end()) {
    for (const CouplingId& id : to_it->second) {
      const auto relation_it = relations_.find(id);
      if (relation_it != relations_.end()) {
        out.push_back(&relation_it->second);
      }
    }
  }
  std::sort(out.begin(), out.end(),
            [](const CouplingRelation* a, const CouplingRelation* b) { return a->id < b->id; });
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::size_t CouplingGraph::coincidental_count() const {
  std::size_t count = 0;
  for (const auto& entry : relations_) {
    if (entry.second.kind == CouplingKind::kCoincidental) {
      ++count;
    }
  }
  return count;
}

Status PropagationPolicy::validate(const Limits& limits) const {
  if (max_depth == 0) {
    return Status::failure(ErrorCode::kInvalidArgument, "propagation depth must be at least one");
  }
  if (max_depth > limits.max_coupling_depth) {
    return Status::failure(ErrorCode::kOutOfRange,
                           "propagation depth " + std::to_string(max_depth) +
                               " exceeds the configured bound of " +
                               std::to_string(limits.max_coupling_depth));
  }
  if (max_paths == 0) {
    return Status::failure(ErrorCode::kInvalidArgument, "propagation path bound must be positive");
  }
  if (max_paths > limits.max_propagation_paths) {
    return Status::failure(ErrorCode::kOutOfRange,
                           "propagation path bound " + std::to_string(max_paths) +
                               " exceeds the configured bound of " +
                               std::to_string(limits.max_propagation_paths));
  }
  return Status::success();
}

JsonValue PropagationStep::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("from", JsonValue::text(from.value()));
  out.set("to", JsonValue::text(to.value()));
  out.set("relation", JsonValue::text(relation.value()));
  out.set("kind", JsonValue::text(std::string(to_string(kind))));
  out.set("strength", JsonValue::real(strength));
  return out;
}

JsonValue PropagationPath::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("origin", JsonValue::text(origin.value()));
  out.set("terminus", JsonValue::text(terminus.value()));
  out.set("path_strength", JsonValue::real(path_strength));
  out.set("weakest_kind", JsonValue::text(std::string(to_string(weakest_kind))));
  out.set("hops", JsonValue::integer(static_cast<std::int64_t>(steps.size())));
  JsonValue step_array = JsonValue::array();
  for (const PropagationStep& step : steps) {
    step_array.push(step.to_json());
  }
  out.set("steps", step_array);
  out.set("reason", JsonValue::text(reason));
  return out;
}

JsonValue PropagationResult::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("origin", JsonValue::text(origin.value()));
  out.set("state", JsonValue::text(std::string(to_string(state))));
  out.set("truncated_paths", JsonValue::integer(static_cast<std::int64_t>(truncated_paths)));
  out.set("depth_limited", JsonValue::boolean(depth_limited));
  out.set("search_bounded", JsonValue::boolean(search_bounded));
  out.set("coincidental_excluded", JsonValue::integer(static_cast<std::int64_t>(coincidental_excluded)));
  JsonValue path_array = JsonValue::array();
  for (const PropagationPath& path : paths) {
    path_array.push(path.to_json());
  }
  out.set("paths", path_array);
  out.set("reason", JsonValue::text(reason));
  return out;
}

namespace {

class Walker {
 public:
  Walker(const EntityId& origin, const std::vector<EntityId>& targets, const CouplingGraph& graph,
         const PropagationPolicy& policy, std::size_t visit_budget, PropagationResult& result)
      : origin_(origin), targets_(targets), graph_(graph), policy_(policy),
        visit_budget_(visit_budget), result_(result) {
    visited_.push_back(origin);
  }

  void run() {
    walk(origin_, 0);
    if (visits_ >= visit_budget_) {
      result_.search_bounded = true;
    }
  }

  void set_depth_limited() { result_.depth_limited = true; }

 private:
  void walk(const EntityId& node, std::size_t depth) {
    if (++visits_ > visit_budget_) {
      result_.search_bounded = true;
      return;
    }
    if (depth > 0 && std::binary_search(targets_.begin(), targets_.end(), node)) {
      emit(node);
    }
    const std::vector<const CouplingRelation*> edges = graph_.outgoing(node);
    bool has_traversable = false;
    for (const CouplingRelation* relation : edges) {
      if (relation->is_traversable()) {
        has_traversable = true;
        break;
      }
    }
    if (depth >= policy_.max_depth) {
      if (has_traversable) {
        result_.depth_limited = true;
      }
      return;
    }
    for (const CouplingRelation* relation : edges) {
      if (!relation->is_traversable()) {
        ++result_.coincidental_excluded;
        continue;
      }
      if (relation->kind == CouplingKind::kPhysicalAdjacency && !policy_.allow_adjacency) {
        continue;
      }
      const EntityId next = relation->from == node ? relation->to : relation->from;
      if (std::find(visited_.begin(), visited_.end(), next) != visited_.end()) {
        continue;
      }
      PropagationStep step;
      step.from = node;
      step.to = next;
      step.relation = relation->id;
      step.kind = relation->kind;
      step.strength = relation->strength;
      steps_.push_back(step);
      visited_.push_back(next);
      walk(next, depth + 1);
      visited_.pop_back();
      steps_.pop_back();
    }
  }

  void emit(const EntityId& terminus) {
    if (result_.paths.size() >= policy_.max_paths) {
      ++result_.truncated_paths;
      if (result_.truncated_paths >= policy_.max_paths) {
        result_.search_bounded = true;
        visits_ = visit_budget_;
      }
      return;
    }
    PropagationPath path;
    path.origin = origin_;
    path.terminus = terminus;
    path.steps = steps_;
    path.path_strength = 1.0;
    path.weakest_kind = CouplingKind::kSupportedThermal;
    for (const PropagationStep& step : steps_) {
      if (step.strength < path.path_strength ||
          (step.strength == path.path_strength &&
           kind_rank(step.kind) < kind_rank(path.weakest_kind))) {
        path.path_strength = step.strength;
        path.weakest_kind = step.kind;
      }
    }
    std::vector<std::string> hops;
    hops.reserve(path.steps.size());
    for (const PropagationStep& step : path.steps) {
      hops.push_back(step.from.value() + "->" + step.to.value() + "[" +
                     std::string(to_string(step.kind)) + " " + format_real(step.strength) + "]");
    }
    path.reason = "path from " + origin_.value() + " to " + terminus.value() + " over " +
                  std::to_string(path.steps.size()) + " hop(s) with weakest link " +
                  format_real(path.path_strength) + " (" +
                  std::string(to_string(path.weakest_kind)) + "): " + join(hops, ", ");
    result_.paths.push_back(std::move(path));
  }

  EntityId origin_;
  const std::vector<EntityId>& targets_;
  const CouplingGraph& graph_;
  const PropagationPolicy& policy_;
  std::size_t visit_budget_;
  PropagationResult& result_;
  std::vector<EntityId> visited_;
  std::vector<PropagationStep> steps_;
  std::size_t visits_ = 0;
};

}  // namespace

Result<PropagationResult> find_propagation(const EntityId& origin,
                                           const std::vector<EntityId>& targets,
                                           const CouplingGraph& graph,
                                           const PropagationPolicy& policy, const Limits& limits) {
  const Status policy_status = policy.validate(limits);
  if (!policy_status.ok()) {
    return policy_status;
  }
  if (!origin.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "propagation requires a set origin entity");
  }

  PropagationResult result;
  result.origin = origin;

  std::vector<EntityId> sorted_targets;
  sorted_targets.reserve(targets.size());
  for (const EntityId& target : targets) {
    if (target.is_set() && target != origin) {
      sorted_targets.push_back(target);
    }
  }
  std::sort(sorted_targets.begin(), sorted_targets.end());
  sorted_targets.erase(std::unique(sorted_targets.begin(), sorted_targets.end()),
                       sorted_targets.end());

  const std::vector<const CouplingRelation*> edges = graph.outgoing(origin);
  bool any_traversable = false;
  for (const CouplingRelation* relation : edges) {
    if (!relation->is_traversable()) {
      ++result.coincidental_excluded;
      continue;
    }
    if (relation->kind == CouplingKind::kPhysicalAdjacency && !policy.allow_adjacency) {
      continue;
    }
    any_traversable = true;
  }
  if (!any_traversable) {
    result.state = EvidenceState::kUnsupported;
    result.reason = "entity " + origin.value() +
                    " has no traversable coupling; coincidence is recorded but never traversed"
                    + (result.coincidental_excluded > 0
                           ? " (" + std::to_string(result.coincidental_excluded) +
                                 " coincidental relation(s) excluded)"
                           : std::string());
    return result;
  }
  if (sorted_targets.empty()) {
    result.state = EvidenceState::kUnsupported;
    result.reason = "no target entity other than the origin was requested";
    return result;
  }

  const std::size_t budget =
      std::max(kMinVisits, policy.max_paths * 64U);
  Walker walker(origin, sorted_targets, graph, policy, budget, result);
  walker.run();

  if (result.paths.empty()) {
    result.state = EvidenceState::kUnsupported;
    result.reason = "no traversable coupling path from " + origin.value() +
                    " reaches the requested targets within depth " +
                    std::to_string(policy.max_depth);
    return result;
  }
  result.state = EvidenceState::kFresh;
  result.reason = std::to_string(result.paths.size()) + " supported path(s) from " +
                  origin.value() + " within depth " + std::to_string(policy.max_depth);
  if (result.truncated_paths > 0) {
    result.reason += "; " + std::to_string(result.truncated_paths) +
                     " further path(s) were dropped at the configured bound";
  }
  if (result.depth_limited) {
    result.reason += "; the search stopped at the configured depth";
  }
  if (result.search_bounded) {
    result.reason += "; the search exhausted its exploration budget";
  }
  if (result.coincidental_excluded > 0) {
    result.reason += "; " + std::to_string(result.coincidental_excluded) +
                     " coincidental relation(s) were not traversed";
  }
  const Status text = validate_text(result.reason, "propagation reason", limits.max_text_length * 8);
  if (!text.ok()) {
    result.reason = std::to_string(result.paths.size()) + " supported path(s) from " +
                    origin.value();
  }
  return result;
}

}  // namespace tobsv
