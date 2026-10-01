// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"
#include "test_support.hpp"

using namespace tobsv;
using namespace tobstest;

namespace {

ObservationId oid(std::uint64_t index) {
  return ObservationId::unchecked("obs-00000000000000" + std::to_string(index % 10) +
                                  std::to_string(index / 10 % 10));
}

}  // namespace

TOBSV_TEST("coupling", "a_relation_without_citations_is_not_evidence") {
  Limits limits;
  const CouplingRelation relation =
      make_coupling(entity_id("node-01"), entity_id("node-02"), CouplingKind::kSupportedThermal, 0.5,
                    {}, base_instant());
  TOBSV_ASSERT_FAILS_WITH(relation.validate(limits), ErrorCode::kUnsupported);
}

TOBSV_TEST("coupling", "one_observation_is_coincidence_not_coupling") {
  Limits limits;
  const CouplingRelation relation = make_coupling(
      entity_id("node-01"), entity_id("node-02"), CouplingKind::kSupportedThermal, 0.5,
      {cite(oid(1))}, base_instant());
  const Status status = relation.validate(limits);
  TOBSV_ASSERT_FAILS_WITH(relation.validate(limits), ErrorCode::kUnsupported);
  TOBSV_ASSERT_TRUE(status.detail().find("at least two") != std::string::npos);
}

TOBSV_TEST("coupling", "repeating_one_citation_is_still_one_observation") {
  Limits limits;
  const CouplingRelation relation = make_coupling(
      entity_id("node-01"), entity_id("node-02"), CouplingKind::kSupportedThermal, 0.5,
      {cite(oid(1)), cite(oid(1))}, base_instant());
  TOBSV_ASSERT_FAILS_WITH(relation.validate(limits), ErrorCode::kUnsupported);
}

TOBSV_TEST("coupling", "adjacency_must_cite_the_topology_that_declared_it") {
  Limits limits;
  const CouplingRelation missing = make_coupling(
      entity_id("node-01"), entity_id("node-02"), CouplingKind::kPhysicalAdjacency, 0.5,
      {cite(oid(1)), cite(oid(2))}, base_instant());
  TOBSV_ASSERT_FAILS_WITH(missing.validate(limits), ErrorCode::kUnsupported);

  const CouplingRelation declared = make_coupling(
      entity_id("node-01"), entity_id("node-02"), CouplingKind::kPhysicalAdjacency, 0.5,
      {cite_topology("topo-hall-1")}, base_instant());
  TOBSV_ASSERT_OK(declared.validate(limits));
}

TOBSV_TEST("coupling", "strength_must_be_a_fraction_in_the_open_unit_interval") {
  Limits limits;
  for (const double strength : {0.0, -0.5, 1.5, std::numeric_limits<double>::quiet_NaN()}) {
    const CouplingRelation relation = make_coupling(
        entity_id("node-01"), entity_id("node-02"), CouplingKind::kSupportedThermal, strength,
        {cite(oid(1)), cite(oid(2))}, base_instant());
    TOBSV_ASSERT_FAILS_WITH(relation.validate(limits), ErrorCode::kOutOfRange);
  }
}

TOBSV_TEST("coupling", "a_relation_that_names_itself_is_refused") {
  Limits limits;
  const CouplingRelation relation = make_coupling(
      entity_id("node-01"), entity_id("node-01"), CouplingKind::kSupportedThermal, 0.5,
      {cite(oid(1)), cite(oid(2))}, base_instant());
  TOBSV_ASSERT_FAILS_WITH(relation.validate(limits), ErrorCode::kInvalidArgument);
}

TOBSV_TEST("coupling", "a_forged_relation_identity_is_refused_by_the_graph") {
  Limits limits;
  CouplingGraph graph;
  CouplingRelation relation = make_coupling(
      entity_id("node-01"), entity_id("node-02"), CouplingKind::kSupportedThermal, 0.5,
      {cite(oid(1)), cite(oid(2))}, base_instant());
  relation.id = CouplingId::unchecked("cpl-0000000000000000");
  TOBSV_ASSERT_FAILS_WITH(graph.add(relation, limits), ErrorCode::kConflict);
}

TOBSV_TEST("coupling", "propagation_never_crosses_a_coincidental_relation") {
  Limits limits;
  CouplingGraph graph;
  TOBSV_ASSERT_OK(graph.add(make_coupling(entity_id("node-01"), entity_id("node-02"),
                                          CouplingKind::kCoincidental, 0.99,
                                          {cite(oid(1)), cite(oid(2))}, base_instant()),
                            limits));
  PropagationPolicy policy;
  const Result<PropagationResult> result = find_propagation(
      entity_id("node-01"), {entity_id("node-02")}, graph, policy, limits);
  TOBSV_ASSERT_OK(result);
  TOBSV_ASSERT_EQ(result.value().state, EvidenceState::kUnsupported);
  TOBSV_ASSERT_EQ(result.value().paths.size(), static_cast<std::size_t>(0));
  TOBSV_ASSERT_EQ(result.value().coincidental_excluded, static_cast<std::size_t>(1));
  TOBSV_ASSERT_TRUE(result.value().reason.find("never traversed") != std::string::npos);
}

TOBSV_TEST("coupling", "a_chain_reports_the_weakest_link") {
  Limits limits;
  CouplingGraph graph;
  TOBSV_ASSERT_OK(graph.add(make_coupling(entity_id("node-01"), entity_id("node-02"),
                                          CouplingKind::kSupportedThermal, 0.9,
                                          {cite(oid(1)), cite(oid(2))}, base_instant()),
                            limits));
  TOBSV_ASSERT_OK(graph.add(make_coupling(entity_id("node-02"), entity_id("node-03"),
                                          CouplingKind::kSupportedThermal, 0.4,
                                          {cite(oid(3)), cite(oid(4))}, base_instant()),
                            limits));
  PropagationPolicy policy;
  const Result<PropagationResult> result = find_propagation(
      entity_id("node-01"), {entity_id("node-03")}, graph, policy, limits);
  TOBSV_ASSERT_OK(result);
  TOBSV_ASSERT_EQ(result.value().paths.size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_NEAR(result.value().paths[0].path_strength, 0.4, 1e-9);
  TOBSV_ASSERT_EQ(result.value().paths[0].steps.size(), static_cast<std::size_t>(2));
  TOBSV_ASSERT_EQ(result.value().paths[0].weakest_kind, CouplingKind::kSupportedThermal);
}

TOBSV_TEST("coupling", "a_directed_relation_is_not_traversed_backwards") {
  Limits limits;
  CouplingGraph graph;
  TOBSV_ASSERT_OK(graph.add(make_coupling(entity_id("node-01"), entity_id("node-02"),
                                          CouplingKind::kSupportedThermal, 0.8,
                                          {cite(oid(1)), cite(oid(2))}, base_instant(),
                                          CouplingDirection::kFromTo),
                            limits));
  PropagationPolicy policy;
  const Result<PropagationResult> forward = find_propagation(
      entity_id("node-01"), {entity_id("node-02")}, graph, policy, limits);
  TOBSV_ASSERT_OK(forward);
  TOBSV_ASSERT_EQ(forward.value().paths.size(), static_cast<std::size_t>(1));

  const Result<PropagationResult> backward = find_propagation(
      entity_id("node-02"), {entity_id("node-01")}, graph, policy, limits);
  TOBSV_ASSERT_OK(backward);
  TOBSV_ASSERT_EQ(backward.value().paths.size(), static_cast<std::size_t>(0));
  TOBSV_ASSERT_EQ(backward.value().state, EvidenceState::kUnsupported);
}

TOBSV_TEST("coupling", "depth_is_bounded_and_the_bound_is_reported") {
  Limits limits;
  CouplingGraph graph;
  for (std::uint64_t index = 1; index <= 5; ++index) {
    const std::string from = "node-0" + std::to_string(index);
    const std::string to = "node-0" + std::to_string(index + 1);
    TOBSV_ASSERT_OK(graph.add(make_coupling(entity_id(from), entity_id(to),
                                            CouplingKind::kSupportedThermal, 0.8,
                                            {cite(oid(index)), cite(oid(index + 1))},
                                            base_instant()),
                              limits));
  }
  PropagationPolicy policy;
  policy.max_depth = 2;
  const Result<PropagationResult> result = find_propagation(
      entity_id("node-01"), {entity_id("node-06")}, graph, policy, limits);
  TOBSV_ASSERT_OK(result);
  TOBSV_ASSERT_EQ(result.value().paths.size(), static_cast<std::size_t>(0));
  TOBSV_ASSERT_TRUE(result.value().depth_limited);
  TOBSV_ASSERT_EQ(result.value().state, EvidenceState::kUnsupported);
}

TOBSV_TEST("coupling", "the_path_bound_is_reported_rather_than_silently_applied") {
  Limits limits;
  CouplingGraph graph;
  // A hub with many leaves produces many paths from the hub to the target set.
  for (std::uint64_t index = 1; index <= 6; ++index) {
    const std::string leaf = "leaf-0" + std::to_string(index);
    TOBSV_ASSERT_OK(graph.add(make_coupling(entity_id("hub"), entity_id(leaf),
                                            CouplingKind::kSupportedThermal, 0.8,
                                            {cite(oid(index)), cite(oid(index + 1))},
                                            base_instant()),
                              limits));
    TOBSV_ASSERT_OK(graph.add(make_coupling(entity_id(leaf), entity_id("sink"),
                                            CouplingKind::kSupportedThermal, 0.8,
                                            {cite(oid(index)), cite(oid(index + 1))},
                                            base_instant()),
                              limits));
  }
  PropagationPolicy policy;
  policy.max_depth = 3;
  policy.max_paths = 2;
  const Result<PropagationResult> result = find_propagation(
      entity_id("hub"), {entity_id("sink")}, graph, policy, limits);
  TOBSV_ASSERT_OK(result);
  TOBSV_ASSERT_EQ(result.value().paths.size(), static_cast<std::size_t>(2));
  // The traversal keeps counting dropped paths until as many have been dropped as kept, so that a
  // dense graph cannot turn a bounded query into an unbounded one. The count is therefore exactly
  // the bound rather than the total number of paths that exist.
  TOBSV_ASSERT_EQ(result.value().truncated_paths, static_cast<std::size_t>(2));
  TOBSV_ASSERT_TRUE(result.value().search_bounded);
  TOBSV_ASSERT_TRUE(result.value().reason.find("dropped at the configured bound") !=
                    std::string::npos);
}

TOBSV_TEST("coupling", "enumeration_is_identical_on_every_run") {
  Limits limits;
  CouplingGraph graph;
  for (std::uint64_t index = 1; index <= 4; ++index) {
    const std::string from = "node-0" + std::to_string(index);
    const std::string to = "node-0" + std::to_string(index + 1);
    TOBSV_ASSERT_OK(graph.add(make_coupling(entity_id(from), entity_id(to),
                                            CouplingKind::kSupportedThermal, 0.8,
                                            {cite(oid(index)), cite(oid(index + 1))},
                                            base_instant()),
                              limits));
  }
  PropagationPolicy policy;
  policy.max_depth = 4;
  const std::vector<EntityId> targets{entity_id("node-03"), entity_id("node-05")};
  const PropagationResult first =
      find_propagation(entity_id("node-01"), targets, graph, policy, limits).value();
  const PropagationResult second =
      find_propagation(entity_id("node-01"), targets, graph, policy, limits).value();
  TOBSV_ASSERT_EQ(first.paths.size(), second.paths.size());
  for (std::size_t index = 0; index < first.paths.size(); ++index) {
    TOBSV_ASSERT_EQ(first.paths[index].terminus, second.paths[index].terminus);
    TOBSV_ASSERT_EQ(first.paths[index].steps.size(), second.paths[index].steps.size());
    for (std::size_t step = 0; step < first.paths[index].steps.size(); ++step) {
      TOBSV_ASSERT_EQ(first.paths[index].steps[step].relation,
                      second.paths[index].steps[step].relation);
    }
  }
  TOBSV_ASSERT_EQ(first.reason, second.reason);
}

TOBSV_TEST("coupling", "a_cycle_does_not_produce_an_endless_walk") {
  Limits limits;
  CouplingGraph graph;
  TOBSV_ASSERT_OK(graph.add(make_coupling(entity_id("node-01"), entity_id("node-02"),
                                          CouplingKind::kSupportedThermal, 0.8,
                                          {cite(oid(1)), cite(oid(2))}, base_instant()),
                            limits));
  TOBSV_ASSERT_OK(graph.add(make_coupling(entity_id("node-02"), entity_id("node-03"),
                                          CouplingKind::kSupportedThermal, 0.8,
                                          {cite(oid(3)), cite(oid(4))}, base_instant()),
                            limits));
  TOBSV_ASSERT_OK(graph.add(make_coupling(entity_id("node-03"), entity_id("node-01"),
                                          CouplingKind::kSupportedThermal, 0.8,
                                          {cite(oid(5)), cite(oid(6))}, base_instant()),
                            limits));
  PropagationPolicy policy;
  policy.max_depth = 8;
  const Result<PropagationResult> result =
      find_propagation(entity_id("node-01"), {entity_id("node-01"), entity_id("node-02")}, graph,
                       policy, limits);
  TOBSV_ASSERT_OK(result);
  // A triangle has two simple paths from node-01 to node-02: the direct edge and the way round
  // through node-03. Both are legitimate; what matters is that no path revisits an entity.
  TOBSV_ASSERT_EQ(result.value().paths.size(), static_cast<std::size_t>(2));
  for (const PropagationPath& path : result.value().paths) {
    TOBSV_ASSERT_EQ(path.terminus.value(), std::string("node-02"));
    std::vector<EntityId> visited;
    visited.push_back(path.origin);
    for (const PropagationStep& step : path.steps) {
      for (const EntityId& seen : visited) {
        TOBSV_ASSERT_TRUE(seen != step.to);
      }
      visited.push_back(step.to);
    }
    TOBSV_ASSERT_TRUE(path.steps.size() <= 2);
  }
}

TOBSV_TEST("coupling", "the_coupling_degree_bound_is_enforced") {
  Limits limits;
  limits.max_edges_per_entity = 2;
  CouplingGraph graph;
  std::uint64_t index = 1;
  for (const char* leaf : {"leaf-1", "leaf-2"}) {
    TOBSV_ASSERT_OK(graph.add(make_coupling(entity_id("hub"), entity_id(leaf),
                                            CouplingKind::kSupportedThermal, 0.8,
                                            {cite(oid(index)), cite(oid(index + 1))},
                                            base_instant()),
                              limits));
    index += 2;
  }
  const CouplingRelation third = make_coupling(
      entity_id("hub"), entity_id("leaf-3"), CouplingKind::kSupportedThermal, 0.8,
      {cite(oid(index)), cite(oid(index + 1))}, base_instant());
  TOBSV_ASSERT_FAILS_WITH(graph.add(third, limits), ErrorCode::kLimitExceeded);
}