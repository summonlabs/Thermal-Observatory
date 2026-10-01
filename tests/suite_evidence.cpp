// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"
#include "test_support.hpp"

using namespace tobsv;
using namespace tobstest;

TOBSV_TEST("evidence", "fresh_subject_resolves_to_the_representative") {
  SingleNodeFixture fixture;
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     41.5, fixture.at, 1);
  const Result<SubjectTemperature> resolved = fixture.observatory.evidence().resolve(
      fixture.entity, fixture.sensor, MeasurementSite::kOutlet, fixture.at,
      FreshnessPolicy::from_limits(fixture.limits));
  TOBSV_ASSERT_OK(resolved);
  TOBSV_ASSERT_EQ(resolved.value().state, EvidenceState::kFresh);
  TOBSV_ASSERT_NEAR(resolved.value().representative_celsius, 41.5, 1e-9);
  TOBSV_ASSERT_EQ(resolved.value().supporting.size(), static_cast<std::size_t>(1));
}

TOBSV_TEST("evidence", "missing_subject_is_unknown_not_fine") {
  SingleNodeFixture fixture;
  const Result<SubjectTemperature> resolved = fixture.observatory.evidence().resolve(
      fixture.entity, sensor_id("never-installed"), MeasurementSite::kOutlet, fixture.at,
      FreshnessPolicy::from_limits(fixture.limits));
  TOBSV_ASSERT_OK(resolved);
  TOBSV_ASSERT_EQ(resolved.value().state, EvidenceState::kUnknown);
  TOBSV_ASSERT_FALSE(resolved.value().is_usable());
  TOBSV_ASSERT_TRUE(resolved.value().reason.find("no observation") != std::string::npos);
}

TOBSV_TEST("evidence", "evidence_outside_the_window_is_stale") {
  SingleNodeFixture fixture;
  const Timestamp old = Timestamp::from_unix_nanos(fixture.at.unix_nanos() -
                                                   900LL * 1000 * 1000 * 1000);
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     41.5, old, 1);
  FreshnessPolicy policy = FreshnessPolicy::from_limits(fixture.limits);
  policy.window = Duration::from_seconds(60.0).value();
  const Result<SubjectTemperature> resolved = fixture.observatory.evidence().resolve(
      fixture.entity, fixture.sensor, MeasurementSite::kOutlet, fixture.at, policy);
  TOBSV_ASSERT_OK(resolved);
  TOBSV_ASSERT_EQ(resolved.value().state, EvidenceState::kStale);
  TOBSV_ASSERT_EQ(resolved.value().stale.size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_FALSE(resolved.value().is_usable());
}

TOBSV_TEST("evidence", "disagreeing_sensors_of_one_subject_are_conflicting") {
  SingleNodeFixture fixture;
  const Timestamp now = fixture.at;
  const auto first = make_observation(fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 40.0,
                                      now, 1, source_id("source-a"));
  const auto second = make_observation(fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                                       46.0, now, 1, source_id("source-b"));
  TOBSV_ASSERT_OK(fixture.observatory.ingest(first).status);
  TOBSV_ASSERT_OK(fixture.observatory.ingest(second).status);

  FreshnessPolicy policy = FreshnessPolicy::from_limits(fixture.limits);
  policy.agreement_tolerance_c = 0.5;
  const Result<SubjectTemperature> resolved = fixture.observatory.evidence().resolve(
      fixture.entity, fixture.sensor, MeasurementSite::kOutlet, now, policy);
  TOBSV_ASSERT_OK(resolved);
  TOBSV_ASSERT_EQ(resolved.value().state, EvidenceState::kConflicting);
  TOBSV_ASSERT_EQ(resolved.value().supporting.size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(resolved.value().conflicting.size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_TRUE(resolved.value().reason.find("exceeds the agreement tolerance") !=
                    std::string::npos);
}

TOBSV_TEST("evidence", "replaying_the_same_observation_is_idempotent") {
  SingleNodeFixture fixture;
  const auto observation = make_observation(fixture.entity, fixture.sensor,
                                            MeasurementSite::kOutlet, 41.5, fixture.at, 1);
  const IngestOutcome first = fixture.observatory.ingest(observation);
  TOBSV_ASSERT_EQ(first.kind, IngestKind::kRecorded);
  const IngestOutcome second = fixture.observatory.ingest(observation);
  TOBSV_ASSERT_EQ(second.kind, IngestKind::kDuplicate);
  TOBSV_ASSERT_TRUE(second.accepted());
  TOBSV_ASSERT_EQ(fixture.observatory.evidence().observation_count(), static_cast<std::size_t>(1));
}

TOBSV_TEST("evidence", "an_older_revision_is_never_promoted_to_current") {
  SingleNodeFixture fixture;
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     41.5, fixture.at, 5);
  const TemperatureObservation older =
      make_observation(fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 30.0, fixture.at,
                       3, source_id("fixture-source"));
  const IngestOutcome refused = fixture.observatory.ingest(older);
  TOBSV_ASSERT_EQ(refused.kind, IngestKind::kRefused);
  TOBSV_ASSERT_EQ(refused.status.code(), ErrorCode::kReplayRejected);
  TOBSV_ASSERT_EQ(fixture.observatory.evidence().observation_count(), static_cast<std::size_t>(1));
}

TOBSV_TEST("evidence", "an_older_epoch_is_refused_and_a_newer_one_is_accepted") {
  SingleNodeFixture fixture;
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     41.5, fixture.at, 1);

  const TemperatureObservation previous_epoch = [&] {
    TemperatureObservation observation =
        make_observation(fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 42.0, fixture.at,
                         2);
    observation.fence.epoch = Epoch::from(0);
    observation.id = compute_observation_id(observation);
    return observation;
  }();
  TOBSV_ASSERT_EQ(fixture.observatory.ingest(previous_epoch).status.code(),
                  ErrorCode::kInvalidArgument);

  const TemperatureObservation stale_epoch = [&] {
    TemperatureObservation observation =
        make_observation(fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 42.0, fixture.at,
                         2);
    observation.fence.epoch = Epoch::from(1);
    observation.fence.generation = Generation::from(1);
    observation.id = compute_observation_id(observation);
    return observation;
  }();
  TOBSV_ASSERT_OK(fixture.observatory.ingest(stale_epoch).status);

  // A later observation that claims an older epoch must be refused even though its revision is
  // higher: the epoch is what makes two counters comparable at all.
  TemperatureObservation replayed_epoch =
      make_observation(fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 43.0, fixture.at, 9);
  replayed_epoch.fence.epoch = Epoch::from(1);
  replayed_epoch.fence.generation = Generation::from(1);
  replayed_epoch.fence.revision = Revision::from(1);
  replayed_epoch.fence.sequence = Sequence::from(1);
  replayed_epoch.id = compute_observation_id(replayed_epoch);
  TOBSV_ASSERT_EQ(fixture.observatory.ingest(replayed_epoch).status.code(),
                  ErrorCode::kReplayRejected);

  TemperatureObservation new_epoch =
      make_observation(fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 44.0, fixture.at, 1);
  new_epoch.fence.epoch = Epoch::from(2);
  new_epoch.id = compute_observation_id(new_epoch);
  const IngestOutcome accepted = fixture.observatory.ingest(new_epoch);
  TOBSV_ASSERT_OK(accepted.status);
  TOBSV_ASSERT_EQ(accepted.kind, IngestKind::kRecorded);
}

TOBSV_TEST("evidence", "two_different_observations_cannot_share_one_revision_slot") {
  SingleNodeFixture fixture;
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     41.5, fixture.at, 4);
  // Same fence position, different content: not a retry, a contradiction.
  TemperatureObservation conflicting =
      make_observation(fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 39.0, fixture.at, 4);
  conflicting.fence.sequence = Sequence::from(4);
  conflicting.id = compute_observation_id(conflicting);
  const IngestOutcome outcome = fixture.observatory.ingest(conflicting);
  TOBSV_ASSERT_EQ(outcome.kind, IngestKind::kConflict);
  TOBSV_ASSERT_EQ(outcome.status.code(), ErrorCode::kConflict);
}

TOBSV_TEST("evidence", "a_forged_observation_identity_is_refused") {
  SingleNodeFixture fixture;
  TemperatureObservation observation = make_observation(
      fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 41.5, fixture.at, 1);
  observation.id = ObservationId::unchecked("obs-0000000000000000");
  const IngestOutcome outcome = fixture.observatory.ingest(observation);
  TOBSV_ASSERT_EQ(outcome.kind, IngestKind::kConflict);
  TOBSV_ASSERT_TRUE(outcome.status.detail().find("does not match its content") != std::string::npos);
}

TOBSV_TEST("evidence", "non_finite_and_implausible_temperatures_are_refused") {
  SingleNodeFixture fixture;
  const double hostile[] = {std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity(), -273.16, 1000.1, 1.0e9};
  for (const double value : hostile) {
    TemperatureObservation observation = make_observation(
        fixture.entity, fixture.sensor, MeasurementSite::kOutlet, value, fixture.at, 1);
    const IngestOutcome outcome = fixture.observatory.ingest(observation);
    TOBSV_ASSERT_EQ(outcome.kind, IngestKind::kRefused);
    TOBSV_ASSERT_TRUE(outcome.status.code() == ErrorCode::kIndeterminate ||
                      outcome.status.code() == ErrorCode::kOutOfRange);
  }
  TOBSV_ASSERT_EQ(fixture.observatory.evidence().observation_count(), static_cast<std::size_t>(0));
}

TOBSV_TEST("evidence", "quality_silence_and_degradation_are_different_states") {
  SingleNodeFixture fixture;
  const auto silent = make_observation(fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                                       41.0, fixture.at, 1, source_id("quiet-source"),
                                       AuthorityLevel::kMeasured, SourceKind::kFacilitySensor,
                                       ClockDomain::kCollectorWallClock, false);
  TOBSV_ASSERT_OK(fixture.observatory.ingest(silent).status);
  const TemperatureObservation* stored = fixture.observatory.evidence().find(silent.id);
  TOBSV_ASSERT_TRUE(stored != nullptr);
  TOBSV_ASSERT_TRUE(stored->quality.is_silent());
  TOBSV_ASSERT_FALSE(stored->quality.is_degraded());

  TemperatureObservation degraded = make_observation(
      fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 41.0, fixture.at, 2,
      source_id("quiet-source"));
  degraded.quality.flags.add(QualityFlag::kSuspect);
  degraded.id = compute_observation_id(degraded);
  TOBSV_ASSERT_OK(fixture.observatory.ingest(degraded).status);
  const TemperatureObservation* second = fixture.observatory.evidence().find(degraded.id);
  TOBSV_ASSERT_TRUE(second != nullptr);
  TOBSV_ASSERT_TRUE(second->quality.supplied);
  TOBSV_ASSERT_TRUE(second->quality.is_degraded());
}

TOBSV_TEST("evidence", "a_timestamp_in_the_future_is_indeterminate_not_fresh") {
  SingleNodeFixture fixture;
  const Timestamp future = Timestamp::from_unix_nanos(fixture.at.unix_nanos() +
                                                      600LL * 1000 * 1000 * 1000);
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     41.5, future, 1);
  const Result<SubjectTemperature> resolved = fixture.observatory.evidence().resolve(
      fixture.entity, fixture.sensor, MeasurementSite::kOutlet, fixture.at,
      FreshnessPolicy::from_limits(fixture.limits));
  TOBSV_ASSERT_OK(resolved);
  TOBSV_ASSERT_EQ(resolved.value().state, EvidenceState::kIndeterminate);
  TOBSV_ASSERT_EQ(resolved.value().indeterminate.size(), static_cast<std::size_t>(1));
}

TOBSV_TEST("evidence", "the_per_subject_ring_retires_the_oldest_and_counts_it") {
  Limits limits;
  limits.max_observations_per_subject = 3;
  const EntityId entity = entity_id("node-ring");
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.limits = limits;
  TOBSV_ASSERT_OK(observatory.open(config));

  for (std::uint64_t revision = 1; revision <= 6; ++revision) {
    const Timestamp at = Timestamp::from_unix_nanos(base_instant().unix_nanos() +
                                                    static_cast<UnixNanos>(revision) * 1000000000LL);
    record_observation(observatory, entity, sensor_id("s"), MeasurementSite::kOutlet,
                       30.0 + static_cast<double>(revision), at, revision);
  }
  TOBSV_ASSERT_EQ(observatory.evidence().observation_count(), static_cast<std::size_t>(3));
  TOBSV_ASSERT_EQ(observatory.evidence().retired_count(), static_cast<std::size_t>(3));
}

TOBSV_TEST("evidence", "the_global_observation_bound_refuses_rather_than_evicting") {
  Limits limits;
  limits.max_observations = 2;
  limits.max_observations_per_subject = 2;
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.limits = limits;
  TOBSV_ASSERT_OK(observatory.open(config));

  for (std::uint64_t revision = 1; revision <= 2; ++revision) {
    record_observation(observatory, entity_id("node-" + std::to_string(revision)),
                       sensor_id("s"), MeasurementSite::kOutlet, 40.0, base_instant(), revision);
  }
  // A new entity still has to present a fence position that moves forward: revision 1 is already
  // claimed, so this uses revision 3 and the refusal that follows is about the bound, not the fence.
  const IngestOutcome refused = observatory.ingest(make_observation(
      entity_id("node-3"), sensor_id("s"), MeasurementSite::kOutlet, 40.0, base_instant(), 3));
  TOBSV_ASSERT_EQ(refused.kind, IngestKind::kRefused);
  TOBSV_ASSERT_EQ(refused.status.code(), ErrorCode::kLimitExceeded);
}

TOBSV_TEST("evidence", "resolution_order_is_canonical_and_stable") {
  Limits limits;
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.limits = limits;
  TOBSV_ASSERT_OK(observatory.open(config));
  std::uint64_t revision = 0;
  for (const char* name : {"node-c", "node-a", "node-b"}) {
    ++revision;
    TOBSV_ASSERT_OK(observatory.register_entity(make_entity(name, EntityClass::kComputeNode)));
    record_observation(observatory, entity_id(name), sensor_id("s"), MeasurementSite::kOutlet, 40.0,
                       base_instant(), revision);
  }
  const auto entities = observatory.evidence().entities();
  TOBSV_ASSERT_EQ(entities.size(), static_cast<std::size_t>(3));
  TOBSV_ASSERT_EQ(entities[0].value(), std::string("node-a"));
  TOBSV_ASSERT_EQ(entities[1].value(), std::string("node-b"));
  TOBSV_ASSERT_EQ(entities[2].value(), std::string("node-c"));

  const Result<std::vector<SubjectTemperature>> resolved = observatory.evidence().resolve_all(
      base_instant(), FreshnessPolicy::from_limits(limits));
  TOBSV_ASSERT_OK(resolved);
  TOBSV_ASSERT_EQ(resolved.value().size(), static_cast<std::size_t>(3));
  TOBSV_ASSERT_EQ(resolved.value()[0].entity.value(), std::string("node-a"));
}