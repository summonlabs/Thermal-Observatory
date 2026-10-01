// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"
#include "test_support.hpp"

using namespace tobsv;
using namespace tobstest;

namespace {

// Builds headroom reports directly so that grouping can be exercised without a store.
HeadroomReport hot_report(const HeadroomReport& source, const std::string& entity, double celsius,
                          const Timestamp& at, ThresholdLevel band) {
  HeadroomReport report;
  report.entity = entity_id(entity);
  report.sensor = source.sensor.is_set() ? source.sensor : sensor_id("s");
  report.site = MeasurementSite::kOutlet;
  report.state = EvidenceState::kFresh;
  report.envelope_applied = true;
  report.envelope = EnvelopeId::unchecked("env-0000000000000001");
  report.observed_celsius = celsius;
  report.observed_at = at;
  report.evidence = ObservationId::unchecked("obs-" + std::string(16 - 1, '0') + "1");
  report.current_level_known = true;
  report.current_level = band;
  HeadroomValue warn;
  warn.level = ThresholdLevel::kWarn;
  warn.limit_c = 35.0;
  warn.observed_c = celsius;
  warn.headroom_c = 35.0 - celsius;
  warn.exceeded = celsius > 35.0;
  report.levels.push_back(warn);
  HeadroomValue maximum;
  maximum.level = ThresholdLevel::kMaximum;
  maximum.limit_c = 50.0;
  maximum.observed_c = celsius;
  maximum.headroom_c = 50.0 - celsius;
  report.levels.push_back(maximum);
  report.reason = "fixture";
  return report;
}

HeadroomReport make_hot(const std::string& entity, double celsius, const Timestamp& at,
                        ThresholdLevel band = ThresholdLevel::kHigh) {
  HeadroomReport seed;
  seed.sensor = sensor_id("s");
  return hot_report(seed, entity, celsius, at, band);
}

}  // namespace

TOBSV_TEST("hotspot", "cool_entities_produce_no_episodes_and_say_so") {
  Limits limits;
  ThermalTopology topology;
  CouplingGraph coupling;
  HotspotPolicy policy;
  const std::vector<HeadroomReport> reports{
      make_hot("node-01", 30.0, base_instant(), ThresholdLevel::kNominal)};
  const Result<HotspotResult> result =
      group_hotspots(reports, topology, coupling, policy, limits);
  TOBSV_ASSERT_OK(result);
  // The subject resolved into the nominal band, so it was never hot in the first place.
  TOBSV_ASSERT_EQ(result.value().evaluable_subjects, static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(result.value().hot_subjects, static_cast<std::size_t>(0));
  TOBSV_ASSERT_EQ(result.value().state, EvidenceState::kUnknown);
}

TOBSV_TEST("hotspot", "adjacent_hot_entities_form_one_episode") {
  Limits limits;
  ThermalTopology topology;
  TOBSV_ASSERT_OK(topology.add_entity(entity_id("rack-1"), limits));
  TOBSV_ASSERT_OK(topology.add_entity(entity_id("node-01"), limits));
  TOBSV_ASSERT_OK(topology.add_entity(entity_id("node-02"), limits));
  TOBSV_ASSERT_OK(topology.add_edge(
      make_adjacency(entity_id("rack-1"), entity_id("node-01"), AdjacencyKind::kPhysicalContainment),
      limits));
  TOBSV_ASSERT_OK(topology.add_edge(
      make_adjacency(entity_id("rack-1"), entity_id("node-02"), AdjacencyKind::kPhysicalContainment),
      limits));

  CouplingGraph coupling;
  HotspotPolicy policy;
  const std::vector<HeadroomReport> reports{make_hot("node-01", 41.0, base_instant()),
                                            make_hot("node-02", 43.0, base_instant())};
  const Result<HotspotResult> result =
      group_hotspots(reports, topology, coupling, policy, limits);
  TOBSV_ASSERT_OK(result);
  TOBSV_ASSERT_EQ(result.value().state, EvidenceState::kFresh);
  TOBSV_ASSERT_EQ(result.value().episodes.size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(result.value().episodes[0].members.size(), static_cast<std::size_t>(2));
  TOBSV_ASSERT_EQ(result.value().episodes[0].seed.value(), std::string("node-01"));
  TOBSV_ASSERT_NEAR(result.value().episodes[0].peak_c, 43.0, 1e-9);
  // The two hot nodes share a rack that is not itself hot. They are grouped transitively, and the
  // episode reports no adjacency link directly between its members, which is the honest count.
  TOBSV_ASSERT_EQ(result.value().episodes[0].adjacency_links, static_cast<std::size_t>(0));
  TOBSV_ASSERT_EQ(result.value().ungrouped.size(), static_cast<std::size_t>(0));
}

TOBSV_TEST("hotspot", "temporal_coincidence_never_joins_an_episode") {
  Limits limits;
  ThermalTopology topology;
  CouplingGraph coupling;
  const Timestamp at = base_instant();
  const CouplingRelation coincidence = make_coupling(
      entity_id("node-01"), entity_id("node-02"), CouplingKind::kCoincidental, 0.95,
      {cite(ObservationId::unchecked("obs-0000000000000001")),
       cite(ObservationId::unchecked("obs-0000000000000002"))},
      at);
  TOBSV_ASSERT_OK(coupling.add(coincidence, limits));

  HotspotPolicy policy;
  const std::vector<HeadroomReport> reports{make_hot("node-01", 41.0, at), make_hot("node-02", 43.0, at)};
  const Result<HotspotResult> result = group_hotspots(reports, topology, coupling, policy, limits);
  TOBSV_ASSERT_OK(result);
  TOBSV_ASSERT_EQ(result.value().episodes.size(), static_cast<std::size_t>(2));
  TOBSV_ASSERT_EQ(result.value().ungrouped.size(), static_cast<std::size_t>(2));
}

TOBSV_TEST("hotspot", "a_supported_thermal_coupling_joins_an_episode") {
  Limits limits;
  ThermalTopology topology;
  CouplingGraph coupling;
  const Timestamp at = base_instant();
  const CouplingRelation supported = make_coupling(
      entity_id("node-01"), entity_id("node-02"), CouplingKind::kSupportedThermal, 0.7,
      {cite(ObservationId::unchecked("obs-0000000000000001")),
       cite(ObservationId::unchecked("obs-0000000000000002"))},
      at);
  TOBSV_ASSERT_OK(coupling.add(supported, limits));

  HotspotPolicy policy;
  const std::vector<HeadroomReport> reports{make_hot("node-01", 41.0, at), make_hot("node-02", 43.0, at)};
  const Result<HotspotResult> result = group_hotspots(reports, topology, coupling, policy, limits);
  TOBSV_ASSERT_OK(result);
  TOBSV_ASSERT_EQ(result.value().episodes.size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(result.value().episodes[0].supported_links, static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(result.value().episodes[0].adjacency_links, static_cast<std::size_t>(0));
  TOBSV_ASSERT_EQ(result.value().episodes[0].support_relations.size(), static_cast<std::size_t>(1));
}

TOBSV_TEST("hotspot", "a_wide_gap_splits_one_component_into_two_episodes") {
  Limits limits;
  ThermalTopology topology;
  CouplingGraph coupling;
  const Timestamp early = base_instant();
  const Timestamp late =
      Timestamp::from_unix_nanos(early.unix_nanos() + 3600LL * 1000 * 1000 * 1000);
  const CouplingRelation supported = make_coupling(
      entity_id("node-01"), entity_id("node-02"), CouplingKind::kSupportedThermal, 0.7,
      {cite(ObservationId::unchecked("obs-0000000000000001")),
       cite(ObservationId::unchecked("obs-0000000000000002"))},
      early);
  TOBSV_ASSERT_OK(coupling.add(supported, limits));

  HotspotPolicy policy;
  policy.episode_gap = Duration::from_seconds(120.0).value();
  const std::vector<HeadroomReport> reports{make_hot("node-01", 41.0, early),
                                            make_hot("node-02", 43.0, late)};
  const Result<HotspotResult> result = group_hotspots(reports, topology, coupling, policy, limits);
  TOBSV_ASSERT_OK(result);
  TOBSV_ASSERT_EQ(result.value().episodes.size(), static_cast<std::size_t>(2));
}

TOBSV_TEST("hotspot", "episode_identity_does_not_depend_on_insertion_order") {
  Limits limits;
  ThermalTopology topology;
  CouplingGraph coupling;
  const Timestamp at = base_instant();
  const CouplingRelation supported = make_coupling(
      entity_id("node-01"), entity_id("node-02"), CouplingKind::kSupportedThermal, 0.7,
      {cite(ObservationId::unchecked("obs-0000000000000001")),
       cite(ObservationId::unchecked("obs-0000000000000002"))},
      at);
  TOBSV_ASSERT_OK(coupling.add(supported, limits));

  HotspotPolicy policy;
  const std::vector<HeadroomReport> forward{make_hot("node-01", 41.0, at), make_hot("node-02", 43.0, at)};
  const std::vector<HeadroomReport> backward{make_hot("node-02", 43.0, at),
                                             make_hot("node-01", 41.0, at)};
  const HotspotResult first = group_hotspots(forward, topology, coupling, policy, limits).value();
  const HotspotResult second = group_hotspots(backward, topology, coupling, policy, limits).value();
  TOBSV_ASSERT_EQ(first.episodes.size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(second.episodes.size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(first.episodes[0].id, second.episodes[0].id);
  TOBSV_ASSERT_EQ(first.episodes[0].members[0].entity, second.episodes[0].members[0].entity);
}

TOBSV_TEST("hotspot", "a_nominal_threshold_is_refused") {
  Limits limits;
  ThermalTopology topology;
  CouplingGraph coupling;
  HotspotPolicy policy;
  policy.threshold = ThresholdLevel::kNominal;
  TOBSV_ASSERT_FAILS_WITH(policy.validate(limits), ErrorCode::kInvalidArgument);
}

TOBSV_TEST("hotspot", "an_episode_above_the_member_bound_is_refused_not_truncated") {
  Limits limits;
  limits.max_hotspot_members = 2;
  ThermalTopology topology;
  CouplingGraph coupling;
  const Timestamp at = base_instant();
  const CouplingRelation first = make_coupling(
      entity_id("node-01"), entity_id("node-02"), CouplingKind::kSupportedThermal, 0.7,
      {cite(ObservationId::unchecked("obs-0000000000000001")),
       cite(ObservationId::unchecked("obs-0000000000000002"))},
      at);
  TOBSV_ASSERT_OK(coupling.add(first, limits));
  const CouplingRelation second = make_coupling(
      entity_id("node-02"), entity_id("node-03"), CouplingKind::kSupportedThermal, 0.7,
      {cite(ObservationId::unchecked("obs-0000000000000003")),
       cite(ObservationId::unchecked("obs-0000000000000004"))},
      at);
  TOBSV_ASSERT_OK(coupling.add(second, limits));

  HotspotPolicy policy;
  const std::vector<HeadroomReport> reports{make_hot("node-01", 41.0, at), make_hot("node-02", 43.0, at),
                                            make_hot("node-03", 44.0, at)};
  const Result<HotspotResult> result = group_hotspots(reports, topology, coupling, policy, limits);
  TOBSV_ASSERT_OK(result);
  TOBSV_ASSERT_EQ(result.value().state, EvidenceState::kRefused);
  TOBSV_ASSERT_EQ(result.value().episodes.size(), static_cast<std::size_t>(0));
  TOBSV_ASSERT_TRUE(result.value().reason.find("above the configured bound") != std::string::npos);
}

TOBSV_TEST("hotspot", "episodes_are_ordered_by_peak_then_seed") {
  Limits limits;
  ThermalTopology topology;
  CouplingGraph coupling;
  const Timestamp at = base_instant();
  HotspotPolicy policy;
  const std::vector<HeadroomReport> reports{make_hot("node-01", 41.0, at), make_hot("node-05", 44.0, at),
                                            make_hot("node-09", 42.0, at)};
  const Result<HotspotResult> result = group_hotspots(reports, topology, coupling, policy, limits);
  TOBSV_ASSERT_OK(result);
  TOBSV_ASSERT_EQ(result.value().episodes.size(), static_cast<std::size_t>(3));
  TOBSV_ASSERT_EQ(result.value().episodes[0].seed.value(), std::string("node-05"));
  TOBSV_ASSERT_EQ(result.value().episodes[1].seed.value(), std::string("node-09"));
  TOBSV_ASSERT_EQ(result.value().episodes[2].seed.value(), std::string("node-01"));
}

TOBSV_TEST("hotspot", "the_episode_bound_drops_the_least_severe_and_reports_it") {
  Limits limits;
  limits.max_hotspot_episodes = 1;
  ThermalTopology topology;
  CouplingGraph coupling;
  const Timestamp at = base_instant();
  HotspotPolicy policy;
  const std::vector<HeadroomReport> reports{make_hot("node-01", 41.0, at), make_hot("node-05", 44.0, at)};
  const Result<HotspotResult> result = group_hotspots(reports, topology, coupling, policy, limits);
  TOBSV_ASSERT_OK(result);
  TOBSV_ASSERT_EQ(result.value().episodes.size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(result.value().episodes[0].seed.value(), std::string("node-05"));
  TOBSV_ASSERT_EQ(result.value().dropped_episodes, static_cast<std::size_t>(1));
  TOBSV_ASSERT_TRUE(result.value().reason.find("dropped at the configured bound") != std::string::npos);
}