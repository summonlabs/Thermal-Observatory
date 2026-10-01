// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"
#include "test_support.hpp"

using namespace tobsv;
using namespace tobstest;

namespace {

ThermalQuery make_query(const SingleNodeFixture& fixture) {
  ThermalQuery query;
  query.evaluated_at = fixture.at;
  query.freshness = FreshnessPolicy::from_limits(fixture.limits);
  query.hotspots.episode_gap = Duration::from_seconds(120.0).value();
  query.hotspots.threshold = ThresholdLevel::kWarn;
  return query;
}

}  // namespace

TOBSV_TEST("analysis", "an_empty_runtime_says_unknown_rather_than_healthy") {
  SingleNodeFixture fixture;
  const ThermalQuery query = make_query(fixture);
  const Result<ThermalAnalysis> analysis = fixture.observatory.analyze(query);
  TOBSV_ASSERT_OK(analysis);
  TOBSV_ASSERT_EQ(analysis.value().state, EvidenceState::kUnknown);
  TOBSV_ASSERT_EQ(analysis.value().subject_count, static_cast<std::size_t>(0));
  TOBSV_ASSERT_EQ(analysis.value().hotspots.episodes.size(), static_cast<std::size_t>(0));
  TOBSV_ASSERT_TRUE(analysis.value().reason_steps.size() >= 3);
}

TOBSV_TEST("analysis", "a_hot_subject_produces_headroom_a_transition_and_an_episode") {
  SingleNodeFixture fixture;
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     42.0, fixture.at, 1);
  const Result<ThermalAnalysis> analysis = fixture.observatory.analyze(make_query(fixture));
  TOBSV_ASSERT_OK(analysis);
  TOBSV_ASSERT_EQ(analysis.value().state, EvidenceState::kFresh);
  TOBSV_ASSERT_EQ(analysis.value().usable_subject_count, static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(analysis.value().headroom.size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(analysis.value().headroom[0].current_level, ThresholdLevel::kHigh);
  TOBSV_ASSERT_EQ(analysis.value().transitions.size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(analysis.value().transitions[0].transition, ThresholdTransition::kNone);
  TOBSV_ASSERT_EQ(analysis.value().hotspots.episodes.size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(analysis.value().hotspots.ungrouped.size(), static_cast<std::size_t>(1));
}

TOBSV_TEST("analysis", "the_digest_is_stable_for_identical_inputs_and_changes_with_evidence") {
  SingleNodeFixture fixture;
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     42.0, fixture.at, 1);
  const ThermalAnalysis first = fixture.observatory.analyze(make_query(fixture)).value();
  // The digest covers the whole document, including the threshold transitions, and those depend on
  // the previous evaluation of each subject. Determinism is therefore stated over the record set,
  // the query and the prior evaluation history - which is what a second, independent runtime that
  // saw the same records reproduces exactly.
  SingleNodeFixture twin;
  record_observation(twin.observatory, twin.entity, twin.sensor, MeasurementSite::kOutlet, 42.0,
                     twin.at, 1);
  const ThermalAnalysis second = twin.observatory.analyze(make_query(twin)).value();
  TOBSV_ASSERT_EQ(first.digest, second.digest);

  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     44.0, fixture.at, 2);
  const ThermalAnalysis third = fixture.observatory.analyze(make_query(fixture)).value();
  TOBSV_ASSERT_TRUE(third.digest != first.digest);
}

TOBSV_TEST("analysis", "transitions_are_carried_across_evaluations_by_the_memory") {
  SingleNodeFixture fixture;
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     25.0, fixture.at, 1);
  ThermalAnalysis first = fixture.observatory.analyze(make_query(fixture)).value();
  TOBSV_ASSERT_EQ(first.transitions[0].current, ThresholdLevel::kNominal);

  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     37.0, fixture.at, 2);
  ThermalAnalysis second = fixture.observatory.analyze(make_query(fixture)).value();
  TOBSV_ASSERT_EQ(second.transitions[0].transition, ThresholdTransition::kEntered);
  TOBSV_ASSERT_EQ(second.transitions[0].previous, ThresholdLevel::kNominal);
  TOBSV_ASSERT_EQ(second.transitions[0].current, ThresholdLevel::kWarn);
}

TOBSV_TEST("analysis", "an_unregistered_entity_and_a_missing_envelope_are_both_reported") {
  ThermalObservatory observatory;
  ObservatoryConfig config;
  TOBSV_ASSERT_OK(observatory.open(config));
  record_observation(observatory, entity_id("ghost-01"), sensor_id("s"), MeasurementSite::kOutlet,
                     40.0, base_instant(), 1);
  ThermalQuery query;
  query.evaluated_at = base_instant();
  const Result<ThermalAnalysis> analysis = observatory.analyze(query);
  TOBSV_ASSERT_OK(analysis);
  TOBSV_ASSERT_TRUE(analysis.value().attribution.contains(AttributionLimitCode::kUnregisteredEntity));
  TOBSV_ASSERT_TRUE(analysis.value().attribution.contains(AttributionLimitCode::kNoEnvelope));
  TOBSV_ASSERT_EQ(analysis.value().headroom[0].state, EvidenceState::kUnsupported);
}

TOBSV_TEST("analysis", "synthetic_and_silent_evidence_are_recorded_as_attribution_limits") {
  Limits limits;
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.limits = limits;
  TOBSV_ASSERT_OK(observatory.open(config));
  TOBSV_ASSERT_OK(observatory.register_entity(make_entity("node-01", EntityClass::kComputeNode)));
  TOBSV_ASSERT_OK(observatory.register_envelope(make_envelope(
      entity_id("node-01"), EntityClass::kComputeNode, MeasurementSite::kOutlet, 20.0, 35.0, 40.0,
      45.0, 50.0)));

  const Timestamp at = base_instant();
  TemperatureObservation synthetic = make_observation(
      entity_id("node-01"), sensor_id("s"), MeasurementSite::kOutlet, 41.0, at, 1,
      source_id("synthetic-generator"), AuthorityLevel::kSynthetic, SourceKind::kSyntheticGenerator,
      ClockDomain::kSynthetic, false);
  TOBSV_ASSERT_OK(observatory.ingest(synthetic).status);

  ThermalQuery query;
  query.evaluated_at = at;
  const Result<ThermalAnalysis> analysis = observatory.analyze(query);
  TOBSV_ASSERT_OK(analysis);
  TOBSV_ASSERT_TRUE(analysis.value().attribution.contains(AttributionLimitCode::kSyntheticSource));
  TOBSV_ASSERT_TRUE(analysis.value().attribution.contains(AttributionLimitCode::kQualityUnsupplied));
}

TOBSV_TEST("analysis", "degraded_quality_is_distinguished_from_absent_quality") {
  Limits limits;
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.limits = limits;
  TOBSV_ASSERT_OK(observatory.open(config));
  TOBSV_ASSERT_OK(observatory.register_entity(make_entity("node-01", EntityClass::kComputeNode)));
  TOBSV_ASSERT_OK(observatory.register_envelope(make_envelope(
      entity_id("node-01"), EntityClass::kComputeNode, MeasurementSite::kOutlet, 20.0, 35.0, 40.0,
      45.0, 50.0)));
  TemperatureObservation observation = make_observation(
      entity_id("node-01"), sensor_id("s"), MeasurementSite::kOutlet, 41.0, base_instant(), 1);
  observation.quality.flags.add(QualityFlag::kSuspect);
  observation.id = compute_observation_id(observation);
  TOBSV_ASSERT_OK(observatory.ingest(observation).status);

  ThermalQuery query;
  query.evaluated_at = base_instant();
  const Result<ThermalAnalysis> analysis = observatory.analyze(query);
  TOBSV_ASSERT_OK(analysis);
  TOBSV_ASSERT_TRUE(analysis.value().attribution.contains(AttributionLimitCode::kQualityDegraded));
  TOBSV_ASSERT_FALSE(analysis.value().attribution.contains(AttributionLimitCode::kQualityUnsupplied));
}

TOBSV_TEST("analysis", "clock_domain_disagreement_is_an_explicit_limit") {
  Limits limits;
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.limits = limits;
  TOBSV_ASSERT_OK(observatory.open(config));
  TOBSV_ASSERT_OK(observatory.register_entity(make_entity("node-01", EntityClass::kComputeNode)));
  TOBSV_ASSERT_OK(observatory.register_envelope(make_envelope(
      entity_id("node-01"), EntityClass::kComputeNode, MeasurementSite::kOutlet, 20.0, 35.0, 40.0,
      45.0, 50.0)));

  const Timestamp at = base_instant();
  TOBSV_ASSERT_OK(observatory
                      .ingest(make_observation(entity_id("node-01"), sensor_id("s"),
                                               MeasurementSite::kOutlet, 41.0, at, 1,
                                               source_id("source-a"), AuthorityLevel::kMeasured,
                                               SourceKind::kFacilitySensor,
                                               ClockDomain::kCollectorWallClock))
                      .status);
  TOBSV_ASSERT_OK(observatory
                      .ingest(make_observation(entity_id("node-01"), sensor_id("s"),
                                               MeasurementSite::kOutlet, 41.1, at, 1,
                                               source_id("source-b"), AuthorityLevel::kMeasured,
                                               SourceKind::kPlatformAgent,
                                               ClockDomain::kSensorMonotonic))
                      .status);
  ThermalQuery query;
  query.evaluated_at = at;
  const Result<ThermalAnalysis> analysis = observatory.analyze(query);
  TOBSV_ASSERT_OK(analysis);
  TOBSV_ASSERT_TRUE(analysis.value().attribution.contains(AttributionLimitCode::kClockDomainMismatch));
}

TOBSV_TEST("analysis", "coincidence_is_reported_and_never_traversed") {
  SingleNodeFixture fixture;
  const ObservationId first = record_observation(
      fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 42.0,
      fixture.at, 1);
  const ObservationId second = record_observation(
      fixture.observatory, entity_id("node-02"), sensor_id("s"), MeasurementSite::kOutlet, 30.0,
      fixture.at, 2);
  TOBSV_ASSERT_OK(fixture.observatory.record_coupling(make_coupling(
      fixture.entity, entity_id("node-02"), CouplingKind::kCoincidental, 0.9,
      {cite(first), cite(second)}, fixture.at)));

  const Result<ThermalAnalysis> analysis = fixture.observatory.analyze(make_query(fixture));
  TOBSV_ASSERT_OK(analysis);
  TOBSV_ASSERT_TRUE(analysis.value().attribution.contains(AttributionLimitCode::kCoincidentalCoupling));
  TOBSV_ASSERT_EQ(analysis.value().propagation.paths.size(), static_cast<std::size_t>(0));
  TOBSV_ASSERT_TRUE(analysis.value().propagation.coincidental_excluded > 0);
}

TOBSV_TEST("analysis", "an_episode_joined_only_by_adjacency_records_its_ambiguity") {
  Limits limits;
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.limits = limits;
  TOBSV_ASSERT_OK(observatory.open(config));
  std::uint64_t revision = 0;
  for (const char* name : {"node-01", "node-02"}) {
    ++revision;
    TOBSV_ASSERT_OK(observatory.register_entity(make_entity(name, EntityClass::kComputeNode)));
    TOBSV_ASSERT_OK(observatory.register_envelope(make_envelope(
        entity_id(name), EntityClass::kComputeNode, MeasurementSite::kOutlet, 20.0, 35.0, 40.0,
        45.0, 50.0)));
    record_observation(observatory, entity_id(name), sensor_id("s"), MeasurementSite::kOutlet,
                       42.0, base_instant(), revision);
  }
  TOBSV_ASSERT_OK(observatory.declare_adjacency(make_adjacency(
      entity_id("node-01"), entity_id("node-02"), AdjacencyKind::kSharedEnclosure)));

  ThermalQuery query;
  query.evaluated_at = base_instant();
  const Result<ThermalAnalysis> analysis = observatory.analyze(query);
  TOBSV_ASSERT_OK(analysis);
  TOBSV_ASSERT_EQ(analysis.value().hotspots.episodes.size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_TRUE(
      analysis.value().attribution.contains(AttributionLimitCode::kJointEnclosureAmbiguity));
}

TOBSV_TEST("analysis", "retired_evidence_is_declared_as_a_limit") {
  Limits limits;
  limits.max_observations_per_subject = 2;
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.limits = limits;
  TOBSV_ASSERT_OK(observatory.open(config));
  TOBSV_ASSERT_OK(observatory.register_entity(make_entity("node-01", EntityClass::kComputeNode)));
  for (std::uint64_t revision = 1; revision <= 4; ++revision) {
    const Timestamp at = Timestamp::from_unix_nanos(base_instant().unix_nanos() +
                                                    static_cast<UnixNanos>(revision) * 1000000000LL);
    record_observation(observatory, entity_id("node-01"), sensor_id("s"), MeasurementSite::kOutlet,
                       40.0, at, revision);
  }
  ThermalQuery query;
  query.evaluated_at = Timestamp::from_unix_nanos(base_instant().unix_nanos() + 5000000000LL);
  const Result<ThermalAnalysis> analysis = observatory.analyze(query);
  TOBSV_ASSERT_OK(analysis);
  TOBSV_ASSERT_TRUE(analysis.value().attribution.contains(AttributionLimitCode::kRetiredEvidence));
}

TOBSV_TEST("analysis", "propagation_starts_at_the_hottest_subject_and_is_explained") {
  Limits limits;
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.limits = limits;
  TOBSV_ASSERT_OK(observatory.open(config));
  for (const char* name : {"node-01", "node-02"}) {
    TOBSV_ASSERT_OK(observatory.register_entity(make_entity(name, EntityClass::kComputeNode)));
    TOBSV_ASSERT_OK(observatory.register_envelope(make_envelope(
        entity_id(name), EntityClass::kComputeNode, MeasurementSite::kOutlet, 20.0, 35.0, 40.0,
        45.0, 50.0)));
  }
  const ObservationId first = record_observation(observatory, entity_id("node-01"),
                                                 sensor_id("s"), MeasurementSite::kOutlet, 41.0,
                                                 base_instant(), 1);
  const ObservationId second = record_observation(observatory, entity_id("node-02"),
                                                  sensor_id("s"), MeasurementSite::kOutlet, 44.0,
                                                  base_instant(), 2);
  TOBSV_ASSERT_OK(observatory.record_coupling(make_coupling(
      entity_id("node-01"), entity_id("node-02"), CouplingKind::kSupportedThermal, 0.6,
      {cite(first), cite(second)}, base_instant(), CouplingDirection::kFromTo)));

  ThermalQuery query;
  query.evaluated_at = base_instant();
  query.propagation.max_depth = 3;
  const Result<ThermalAnalysis> analysis = observatory.analyze(query);
  TOBSV_ASSERT_OK(analysis);
  TOBSV_ASSERT_EQ(analysis.value().propagation.origin.value(), std::string("node-02"));
  TOBSV_ASSERT_EQ(analysis.value().propagation.state, EvidenceState::kUnsupported);
  bool explained = false;
  for (const std::string& step : analysis.value().reason_steps) {
    if (step.find("propagation was traced from node-02") != std::string::npos) {
      explained = true;
    }
  }
  TOBSV_ASSERT_TRUE(explained);
}

TOBSV_TEST("analysis", "a_conflicting_subject_makes_the_whole_analysis_conflicting") {
  SingleNodeFixture fixture;
  const Timestamp at = fixture.at;
  TOBSV_ASSERT_OK(fixture.observatory
                      .ingest(make_observation(fixture.entity, fixture.sensor,
                                               MeasurementSite::kOutlet, 40.0, at, 1,
                                               source_id("source-a")))
                      .status);
  TOBSV_ASSERT_OK(fixture.observatory
                      .ingest(make_observation(fixture.entity, fixture.sensor,
                                               MeasurementSite::kOutlet, 47.0, at, 1,
                                               source_id("source-b")))
                      .status);
  const Result<ThermalAnalysis> analysis = fixture.observatory.analyze(make_query(fixture));
  TOBSV_ASSERT_OK(analysis);
  TOBSV_ASSERT_EQ(analysis.value().state, EvidenceState::kConflicting);
  TOBSV_ASSERT_EQ(analysis.value().conflicted_subject_count, static_cast<std::size_t>(1));
  TOBSV_ASSERT_TRUE(analysis.value().attribution.contains(AttributionLimitCode::kConflictingSensors));
}

TOBSV_TEST("analysis", "the_analysis_document_is_canonical_json_with_the_expected_shape") {
  SingleNodeFixture fixture;
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     42.0, fixture.at, 1);
  const ThermalAnalysis analysis = fixture.observatory.analyze(make_query(fixture)).value();
  const Result<std::string> encoded = analysis.to_json().dump_compact();
  TOBSV_ASSERT_OK(encoded);
  Limits limits;
  const Result<JsonValue> reparsed = JsonValue::parse(encoded.value(), limits);
  TOBSV_ASSERT_OK(reparsed);
  TOBSV_ASSERT_OK(reparsed.value().require_string("digest"));
  TOBSV_ASSERT_OK(reparsed.value().require_string("state"));
  TOBSV_ASSERT_TRUE(reparsed.value().find("headroom") != nullptr);
  TOBSV_ASSERT_TRUE(reparsed.value().find("attribution") != nullptr);
  const Result<std::string> again = reparsed.value().dump_compact();
  TOBSV_ASSERT_OK(again);
  TOBSV_ASSERT_EQ(again.value(), encoded.value());
}

TOBSV_TEST("analysis", "a_nominal_focus_band_is_refused") {
  Limits limits;
  ThermalQuery query;
  query.evaluated_at = base_instant();
  query.focus = ThresholdLevel::kNominal;
  TOBSV_ASSERT_FAILS_WITH(query.validate(limits), ErrorCode::kInvalidArgument);
}

TOBSV_TEST("analysis", "an_unset_evaluation_instant_is_refused") {
  Limits limits;
  ThermalQuery query;
  TOBSV_ASSERT_FAILS_WITH(query.validate(limits), ErrorCode::kInvalidArgument);
}