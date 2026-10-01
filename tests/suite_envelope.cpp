// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"
#include "test_support.hpp"

using namespace tobsv;
using namespace tobstest;

namespace {

SubjectTemperature resolve_subject(const SingleNodeFixture& fixture) {
  const Result<SubjectTemperature> resolved = fixture.observatory.evidence().resolve(
      fixture.entity, fixture.sensor, MeasurementSite::kOutlet, fixture.at,
      FreshnessPolicy::from_limits(fixture.limits));
  return resolved.value();
}

}  // namespace

TOBSV_TEST("envelope", "bands_must_be_ordered_and_the_ceiling_must_exceed_nominal") {
  Limits limits;
  ThermalEnvelope envelope = make_envelope(entity_id("node-01"), EntityClass::kComputeNode,
                                           MeasurementSite::kOutlet, 20.0, 35.0, 40.0, 45.0, 50.0);
  TOBSV_ASSERT_OK(envelope.validate(limits));

  ThermalEnvelope unordered = envelope;
  unordered.warn_c = 41.0;
  TOBSV_ASSERT_FAILS_WITH(unordered.validate(limits), ErrorCode::kInvalidArgument);

  ThermalEnvelope inverted = envelope;
  inverted.maximum_c = 10.0;
  TOBSV_ASSERT_FAILS_WITH(inverted.validate(limits), ErrorCode::kInvalidArgument);

  ThermalEnvelope without_ceiling = envelope;
  without_ceiling.has_maximum = false;
  TOBSV_ASSERT_FAILS_WITH(without_ceiling.validate(limits), ErrorCode::kInvalidArgument);

  ThermalEnvelope impossible = envelope;
  impossible.warn_c = -300.0;
  TOBSV_ASSERT_FAILS_WITH(impossible.validate(limits), ErrorCode::kOutOfRange);
}

TOBSV_TEST("envelope", "an_envelope_without_an_owner_is_unsupported") {
  Limits limits;
  ThermalEnvelope envelope = make_envelope(EntityId{}, EntityClass::kUnknown,
                                           MeasurementSite::kOutlet, 20.0, 35.0, 40.0, 45.0, 50.0);
  envelope.entity = EntityId{};
  TOBSV_ASSERT_FAILS_WITH(envelope.validate(limits), ErrorCode::kUnsupported);
}

TOBSV_TEST("envelope", "a_forged_envelope_identity_is_refused_by_the_registry") {
  SingleNodeFixture fixture;
  ThermalEnvelope envelope = fixture.envelope;
  envelope.id = EnvelopeId::unchecked("env-0000000000000000");
  TOBSV_ASSERT_FAILS_WITH(fixture.observatory.register_envelope(envelope), ErrorCode::kConflict);
}

TOBSV_TEST("envelope", "a_duplicate_registration_is_refused") {
  SingleNodeFixture fixture;
  TOBSV_ASSERT_FAILS_WITH(fixture.observatory.register_envelope(fixture.envelope),
                          ErrorCode::kDuplicateIdentity);
}

TOBSV_TEST("envelope", "selection_prefers_the_most_specific_envelope") {
  Limits limits;
  EnvelopeRegistry registry;
  ThermalEnvelope class_wide = make_envelope(EntityId{}, EntityClass::kComputeNode,
                                             MeasurementSite::kOutlet, 20.0, 35.0, 40.0, 45.0, 50.0);
  class_wide.entity = EntityId{};
  class_wide.id = compute_envelope_id(class_wide);
  TOBSV_ASSERT_OK(registry.add(class_wide, limits));

  ThermalEnvelope entity_specific = make_envelope(
      entity_id("node-01"), EntityClass::kUnknown, MeasurementSite::kUnknown, 20.0, 30.0, 40.0, 45.0,
      55.0);
  entity_specific.entity_class = EntityClass::kUnknown;
  entity_specific.site = MeasurementSite::kUnknown;
  entity_specific.id = compute_envelope_id(entity_specific);
  TOBSV_ASSERT_OK(registry.add(entity_specific, limits));

  const ThermalEnvelope* selected =
      registry.select(entity_id("node-01"), EntityClass::kComputeNode, MeasurementSite::kOutlet);
  TOBSV_ASSERT_TRUE(selected != nullptr);
  TOBSV_ASSERT_EQ(selected->id, entity_specific.id);
  TOBSV_ASSERT_NEAR(selected->maximum_c, 55.0, 1e-9);

  const ThermalEnvelope* other =
      registry.select(entity_id("node-02"), EntityClass::kComputeNode, MeasurementSite::kOutlet);
  TOBSV_ASSERT_TRUE(other != nullptr);
  TOBSV_ASSERT_EQ(other->id, class_wide.id);

  // An entity-bound envelope applies to that entity whatever its class; nothing applies to an
  // entity that no envelope names.
  TOBSV_ASSERT_TRUE(registry.select(entity_id("node-02"), EntityClass::kAccelerator,
                                    MeasurementSite::kOutlet) == nullptr);
  // The class-wide envelope still applies to an unknown entity of the right class, but not to an
  // entity whose class no envelope mentions.
  const ThermalEnvelope* unknown_entity =
      registry.select(entity_id("node-99"), EntityClass::kComputeNode, MeasurementSite::kOutlet);
  TOBSV_ASSERT_TRUE(unknown_entity != nullptr);
  TOBSV_ASSERT_EQ(unknown_entity->id, class_wide.id);
  TOBSV_ASSERT_TRUE(registry.select(entity_id("node-99"), EntityClass::kAccelerator,
                                    MeasurementSite::kOutlet) == nullptr);
}

TOBSV_TEST("envelope", "headroom_is_the_exact_signed_distance_to_each_band") {
  SingleNodeFixture fixture;
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     41.5, fixture.at, 1);
  const SubjectTemperature subject = resolve_subject(fixture);
  const Result<HeadroomReport> report = compute_headroom(
      subject, EntityClass::kComputeNode, fixture.observatory.envelopes(), fixture.limits);
  TOBSV_ASSERT_OK(report);
  TOBSV_ASSERT_EQ(report.value().state, EvidenceState::kFresh);
  TOBSV_ASSERT_EQ(report.value().levels.size(), static_cast<std::size_t>(5));
  TOBSV_ASSERT_NEAR(report.value().level(ThresholdLevel::kWarn)->headroom_c, -6.5, 1e-9);
  TOBSV_ASSERT_NEAR(report.value().level(ThresholdLevel::kCritical)->headroom_c, 3.5, 1e-9);
  TOBSV_ASSERT_NEAR(report.value().level(ThresholdLevel::kMaximum)->headroom_c, 8.5, 1e-9);
  TOBSV_ASSERT_TRUE(report.value().level(ThresholdLevel::kWarn)->exceeded);
  TOBSV_ASSERT_FALSE(report.value().level(ThresholdLevel::kMaximum)->exceeded);
  TOBSV_ASSERT_EQ(report.value().current_level, ThresholdLevel::kHigh);
  TOBSV_ASSERT_TRUE(report.value().current_level_known);
}

TOBSV_TEST("envelope", "consumed_fraction_is_defined_only_when_a_usable_band_exists") {
  SingleNodeFixture fixture;
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     30.0, fixture.at, 1);
  const SubjectTemperature subject = resolve_subject(fixture);
  const Result<HeadroomReport> report = compute_headroom(
      subject, EntityClass::kComputeNode, fixture.observatory.envelopes(), fixture.limits);
  TOBSV_ASSERT_OK(report);
  const HeadroomValue* warn = report.value().level(ThresholdLevel::kWarn);
  TOBSV_ASSERT_TRUE(warn != nullptr);
  TOBSV_ASSERT_TRUE(warn->consumed_fraction_defined);
  // nominal 20, warn 35, observed 30 -> (30-20)/(35-20)
  TOBSV_ASSERT_NEAR(warn->consumed_fraction, 10.0 / 15.0, 1e-9);

  const HeadroomValue* nominal = report.value().level(ThresholdLevel::kNominal);
  TOBSV_ASSERT_TRUE(nominal != nullptr);
  TOBSV_ASSERT_FALSE(nominal->consumed_fraction_defined);
}

TOBSV_TEST("envelope", "undeclared_bands_are_listed_rather_than_assumed") {
  Limits limits;
  ThermalEnvelope sparse = make_envelope(entity_id("node-01"), EntityClass::kComputeNode,
                                         MeasurementSite::kOutlet, 20.0, 0.0, 0.0, 0.0, 50.0);
  sparse.has_warn = false;
  sparse.has_high = false;
  sparse.has_critical = false;
  sparse.id = compute_envelope_id(sparse);
  TOBSV_ASSERT_OK(sparse.validate(limits));

  SubjectTemperature subject;
  subject.entity = entity_id("node-01");
  subject.sensor = sensor_id("sensor-outlet-1");
  subject.site = MeasurementSite::kOutlet;
  subject.state = EvidenceState::kFresh;
  subject.representative_celsius = 44.0;
  subject.representative_id = ObservationId::unchecked("obs-0000000000000001");
  subject.representative_at = base_instant();

  EnvelopeRegistry registry;
  TOBSV_ASSERT_OK(registry.add(sparse, limits));
  const Result<HeadroomReport> report =
      compute_headroom(subject, EntityClass::kComputeNode, registry, limits);
  TOBSV_ASSERT_OK(report);
  TOBSV_ASSERT_EQ(report.value().levels.size(), static_cast<std::size_t>(2));
  TOBSV_ASSERT_EQ(report.value().missing_levels.size(), static_cast<std::size_t>(3));
  TOBSV_ASSERT_TRUE(report.value().reason.find("bands not declared") != std::string::npos);
}

TOBSV_TEST("envelope", "without_an_envelope_headroom_is_unsupported_not_huge") {
  SingleNodeFixture fixture;
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     41.5, fixture.at, 1);
  const SubjectTemperature subject = resolve_subject(fixture);
  EnvelopeRegistry empty;
  const Result<HeadroomReport> report =
      compute_headroom(subject, EntityClass::kComputeNode, empty, fixture.limits);
  TOBSV_ASSERT_OK(report);
  TOBSV_ASSERT_EQ(report.value().state, EvidenceState::kUnsupported);
  TOBSV_ASSERT_EQ(report.value().levels.size(), static_cast<std::size_t>(0));
  TOBSV_ASSERT_EQ(report.value().missing_levels.size(), static_cast<std::size_t>(5));
  TOBSV_ASSERT_TRUE(report.value().reason.find("no thermal envelope") != std::string::npos);
}

TOBSV_TEST("envelope", "a_stale_subject_yields_no_headroom_at_all") {
  SingleNodeFixture fixture;
  const Timestamp old =
      Timestamp::from_unix_nanos(fixture.at.unix_nanos() - 5000LL * 1000 * 1000 * 1000);
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     41.5, old, 1);
  const SubjectTemperature subject = resolve_subject(fixture);
  TOBSV_ASSERT_EQ(subject.state, EvidenceState::kStale);
  const Result<HeadroomReport> report = compute_headroom(
      subject, EntityClass::kComputeNode, fixture.observatory.envelopes(), fixture.limits);
  TOBSV_ASSERT_OK(report);
  TOBSV_ASSERT_EQ(report.value().state, EvidenceState::kStale);
  TOBSV_ASSERT_EQ(report.value().levels.size(), static_cast<std::size_t>(0));
}

TOBSV_TEST("envelope", "band_of_never_decreases_as_temperature_rises") {
  const ThermalEnvelope envelope =
      make_envelope(entity_id("node-01"), EntityClass::kComputeNode, MeasurementSite::kOutlet, 20.0,
                    35.0, 40.0, 45.0, 50.0);
  int previous = -1;
  for (double celsius = -40.0; celsius <= 60.0; celsius += 0.25) {
    ThresholdLevel band = ThresholdLevel::kNominal;
    TOBSV_ASSERT_TRUE(band_of(envelope, celsius, band));
    const int rank = static_cast<int>(static_cast<unsigned>(band));
    TOBSV_ASSERT_TRUE(rank >= previous);
    previous = rank;
  }
  ThresholdLevel unused = ThresholdLevel::kNominal;
  TOBSV_ASSERT_FALSE(band_of(envelope, std::numeric_limits<double>::quiet_NaN(), unused));
}

TOBSV_TEST("envelope", "transitions_are_named_and_never_invented") {
  SingleNodeFixture fixture;
  const auto evaluate = [&](double celsius, std::uint64_t revision) {
    record_observation(fixture.observatory, fixture.entity, fixture.sensor,
                       MeasurementSite::kOutlet, celsius, fixture.at, revision);
    const SubjectTemperature subject = resolve_subject(fixture);
    return compute_headroom(subject, EntityClass::kComputeNode, fixture.observatory.envelopes(),
                            fixture.limits)
        .value();
  };

  HeadroomReport first = evaluate(30.0, 1);
  ThresholdTransitionRecord record = classify_transition(false, ThresholdLevel::kNominal, first, fixture.at);
  TOBSV_ASSERT_EQ(record.transition, ThresholdTransition::kNone);

  HeadroomReport second = evaluate(41.0, 2);
  record = classify_transition(true, ThresholdLevel::kNominal, second, fixture.at);
  TOBSV_ASSERT_EQ(record.transition, ThresholdTransition::kEntered);
  TOBSV_ASSERT_EQ(record.current, ThresholdLevel::kHigh);

  HeadroomReport third = evaluate(46.0, 3);
  record = classify_transition(true, ThresholdLevel::kHigh, third, fixture.at);
  TOBSV_ASSERT_EQ(record.transition, ThresholdTransition::kEscalated);
  TOBSV_ASSERT_EQ(record.current, ThresholdLevel::kCritical);

  HeadroomReport fourth = evaluate(42.0, 4);
  record = classify_transition(true, ThresholdLevel::kCritical, fourth, fixture.at);
  TOBSV_ASSERT_EQ(record.transition, ThresholdTransition::kDeescalated);
  TOBSV_ASSERT_EQ(record.current, ThresholdLevel::kHigh);

  HeadroomReport fifth = evaluate(22.0, 5);
  record = classify_transition(true, ThresholdLevel::kHigh, fifth, fixture.at);
  TOBSV_ASSERT_EQ(record.transition, ThresholdTransition::kExited);
  TOBSV_ASSERT_EQ(record.current, ThresholdLevel::kNominal);

  HeadroomReport sixth = evaluate(21.0, 6);
  record = classify_transition(true, ThresholdLevel::kNominal, sixth, fixture.at);
  TOBSV_ASSERT_EQ(record.transition, ThresholdTransition::kUnchanged);
}

TOBSV_TEST("envelope", "an_unresolved_subject_yields_an_unknown_transition") {
  SingleNodeFixture fixture;
  HeadroomReport report;
  report.entity = fixture.entity;
  report.sensor = fixture.sensor;
  report.site = MeasurementSite::kOutlet;
  report.state = EvidenceState::kStale;
  report.reason = "the reading is older than the freshness window";
  const ThresholdTransitionRecord record =
      classify_transition(true, ThresholdLevel::kHigh, report, fixture.at);
  TOBSV_ASSERT_EQ(record.transition, ThresholdTransition::kUnknown);
  TOBSV_ASSERT_TRUE(record.reason.find("could not be established") != std::string::npos);
}