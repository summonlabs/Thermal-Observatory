// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"
#include "test_support.hpp"

using namespace tobsv;
using namespace tobstest;

TOBSV_TEST("derating", "an_uncited_claim_is_not_evidence") {
  Limits limits;
  DeratingEvidence evidence =
      make_derating(entity_id("node-01"), DeratingKind::kClockThrottle, 0.25, {},
                    ThresholdLevel::kHigh, base_instant());
  TOBSV_ASSERT_FAILS_WITH(evidence.validate(limits), ErrorCode::kUnsupported);
}

TOBSV_TEST("derating", "a_magnitude_outside_the_open_unit_interval_is_refused") {
  Limits limits;
  for (const double magnitude : {0.0, -0.1, 1.5, std::numeric_limits<double>::quiet_NaN()}) {
    DeratingEvidence evidence = make_derating(
        entity_id("node-01"), DeratingKind::kPowerLimit, magnitude,
        {ObservationId::unchecked("obs-0000000000000001")}, ThresholdLevel::kHigh, base_instant());
    TOBSV_ASSERT_FAILS_WITH(evidence.validate(limits), ErrorCode::kOutOfRange);
  }
}

TOBSV_TEST("derating", "a_nominal_trigger_band_is_refused") {
  Limits limits;
  DeratingEvidence evidence = make_derating(
      entity_id("node-01"), DeratingKind::kPowerLimit, 0.2,
      {ObservationId::unchecked("obs-0000000000000001")}, ThresholdLevel::kNominal, base_instant());
  TOBSV_ASSERT_FAILS_WITH(evidence.validate(limits), ErrorCode::kInvalidArgument);
}

TOBSV_TEST("derating", "a_claim_backed_by_evidence_in_its_band_is_supported") {
  SingleNodeFixture fixture;
  const ObservationId observation = record_observation(
      fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 46.0,
      fixture.at, 1);
  const DeratingEvidence evidence =
      make_derating(fixture.entity, DeratingKind::kClockThrottle, 0.3, {observation},
                    ThresholdLevel::kHigh, fixture.at);
  TOBSV_ASSERT_OK(fixture.observatory.record_derating(evidence));

  const Result<DeratingAppraisal> appraisal = appraise_derating(
      fixture.entity, fixture.observatory.derating(), fixture.observatory.evidence(),
      fixture.observatory.inventory(), fixture.observatory.envelopes(), fixture.at,
      FreshnessPolicy::from_limits(fixture.limits), fixture.limits);
  TOBSV_ASSERT_OK(appraisal);
  TOBSV_ASSERT_EQ(appraisal.value().state, EvidenceState::kFresh);
  TOBSV_ASSERT_EQ(appraisal.value().entries.size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(appraisal.value().entries[0].state, EvidenceState::kFresh);
  TOBSV_ASSERT_EQ(appraisal.value().entries[0].observed_band, ThresholdLevel::kCritical);
  TOBSV_ASSERT_TRUE(appraisal.value().entries[0].reason.find("is supported") != std::string::npos);
}

TOBSV_TEST("derating", "a_claim_whose_evidence_never_reaches_the_band_is_conflicting") {
  SingleNodeFixture fixture;
  const ObservationId observation = record_observation(
      fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 24.0,
      fixture.at, 1);
  DeratingEvidence evidence =
      make_derating(fixture.entity, DeratingKind::kClockThrottle, 0.3, {observation},
                    ThresholdLevel::kCritical, fixture.at);
  TOBSV_ASSERT_OK(fixture.observatory.record_derating(evidence));

  const Result<DeratingAppraisal> appraisal = appraise_derating(
      fixture.entity, fixture.observatory.derating(), fixture.observatory.evidence(),
      fixture.observatory.inventory(), fixture.observatory.envelopes(), fixture.at,
      FreshnessPolicy::from_limits(fixture.limits), fixture.limits);
  TOBSV_ASSERT_OK(appraisal);
  TOBSV_ASSERT_EQ(appraisal.value().entries[0].state, EvidenceState::kConflicting);
  TOBSV_ASSERT_TRUE(appraisal.value().entries[0].reason.find("only reach the") != std::string::npos);
}

TOBSV_TEST("derating", "a_claim_citing_an_observation_this_runtime_lacks_is_unsupported") {
  SingleNodeFixture fixture;
  const DeratingEvidence evidence = make_derating(
      fixture.entity, DeratingKind::kPowerLimit, 0.2,
      {ObservationId::unchecked("obs-00000000000000ff")}, ThresholdLevel::kHigh, fixture.at);
  TOBSV_ASSERT_OK(fixture.observatory.record_derating(evidence));

  const Result<DeratingAppraisal> appraisal = appraise_derating(
      fixture.entity, fixture.observatory.derating(), fixture.observatory.evidence(),
      fixture.observatory.inventory(), fixture.observatory.envelopes(), fixture.at,
      FreshnessPolicy::from_limits(fixture.limits), fixture.limits);
  TOBSV_ASSERT_OK(appraisal);
  TOBSV_ASSERT_EQ(appraisal.value().entries[0].state, EvidenceState::kUnsupported);
  TOBSV_ASSERT_EQ(appraisal.value().entries[0].missing_citations.size(), static_cast<std::size_t>(1));
}

TOBSV_TEST("derating", "a_claim_resting_on_stale_evidence_is_stale") {
  SingleNodeFixture fixture;
  const Timestamp old =
      Timestamp::from_unix_nanos(fixture.at.unix_nanos() - 5000LL * 1000 * 1000 * 1000);
  const ObservationId observation = record_observation(
      fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 46.0, old, 1);
  const DeratingEvidence evidence =
      make_derating(fixture.entity, DeratingKind::kClockThrottle, 0.3, {observation},
                    ThresholdLevel::kHigh, old);
  TOBSV_ASSERT_OK(fixture.observatory.record_derating(evidence));

  const Result<DeratingAppraisal> appraisal = appraise_derating(
      fixture.entity, fixture.observatory.derating(), fixture.observatory.evidence(),
      fixture.observatory.inventory(), fixture.observatory.envelopes(), fixture.at,
      FreshnessPolicy::from_limits(fixture.limits), fixture.limits);
  TOBSV_ASSERT_OK(appraisal);
  TOBSV_ASSERT_EQ(appraisal.value().entries[0].state, EvidenceState::kStale);
  TOBSV_ASSERT_EQ(appraisal.value().entries[0].stale_citations.size(), static_cast<std::size_t>(1));
}

TOBSV_TEST("derating", "a_forged_derating_identity_is_refused_by_the_registry") {
  SingleNodeFixture fixture;
  DeratingEvidence evidence = make_derating(
      fixture.entity, DeratingKind::kClockThrottle, 0.3,
      {ObservationId::unchecked("obs-0000000000000001")}, ThresholdLevel::kHigh, fixture.at);
  evidence.id = DeratingId::unchecked("dr-0000000000000000");
  TOBSV_ASSERT_FAILS_WITH(fixture.observatory.record_derating(evidence), ErrorCode::kConflict);
}

TOBSV_TEST("derating", "an_entity_without_claims_appraises_to_unknown") {
  SingleNodeFixture fixture;
  const Result<DeratingAppraisal> appraisal = appraise_derating(
      fixture.entity, fixture.observatory.derating(), fixture.observatory.evidence(),
      fixture.observatory.inventory(), fixture.observatory.envelopes(), fixture.at,
      FreshnessPolicy::from_limits(fixture.limits), fixture.limits);
  TOBSV_ASSERT_OK(appraisal);
  TOBSV_ASSERT_EQ(appraisal.value().state, EvidenceState::kUnknown);
  TOBSV_ASSERT_EQ(appraisal.value().entries.size(), static_cast<std::size_t>(0));
  TOBSV_ASSERT_TRUE(appraisal.value().reason.find("no derating claim") != std::string::npos);
}

TOBSV_TEST("derating", "several_claims_are_appraised_and_summarised") {
  SingleNodeFixture fixture;
  const ObservationId hot = record_observation(fixture.observatory, fixture.entity, fixture.sensor,
                                               MeasurementSite::kOutlet, 47.0, fixture.at, 1);
  TOBSV_ASSERT_OK(fixture.observatory.record_derating(
      make_derating(fixture.entity, DeratingKind::kClockThrottle, 0.3, {hot},
                    ThresholdLevel::kHigh, fixture.at)));
  TOBSV_ASSERT_OK(fixture.observatory.record_derating(
      make_derating(fixture.entity, DeratingKind::kPowerLimit, 0.1,
                    {ObservationId::unchecked("obs-00000000000000fe")}, ThresholdLevel::kHigh,
                    fixture.at)));

  const Result<DeratingAppraisal> appraisal = appraise_derating(
      fixture.entity, fixture.observatory.derating(), fixture.observatory.evidence(),
      fixture.observatory.inventory(), fixture.observatory.envelopes(), fixture.at,
      FreshnessPolicy::from_limits(fixture.limits), fixture.limits);
  TOBSV_ASSERT_OK(appraisal);
  TOBSV_ASSERT_EQ(appraisal.value().entries.size(), static_cast<std::size_t>(2));
  TOBSV_ASSERT_EQ(appraisal.value().state, EvidenceState::kConflicting);
  TOBSV_ASSERT_TRUE(appraisal.value().reason.find("1 supported") != std::string::npos);
  TOBSV_ASSERT_TRUE(appraisal.value().reason.find("1 unsupported") != std::string::npos);
}

TOBSV_TEST("derating", "appraisal_is_deterministic_by_claim_identity") {
  SingleNodeFixture fixture;
  const ObservationId hot = record_observation(fixture.observatory, fixture.entity, fixture.sensor,
                                               MeasurementSite::kOutlet, 47.0, fixture.at, 1);
  for (std::uint64_t index = 0; index < 4; ++index) {
    DeratingEvidence evidence = make_derating(
        fixture.entity, DeratingKind::kFrequencyCap, 0.1 + 0.05 * static_cast<double>(index),
        {hot}, ThresholdLevel::kWarn, fixture.at);
    evidence.fence = make_fence(evidence.asserted_by, index + 1);
    evidence.id = compute_derating_id(evidence);
    TOBSV_ASSERT_OK(fixture.observatory.record_derating(evidence));
  }
  const Result<DeratingAppraisal> first = appraise_derating(
      fixture.entity, fixture.observatory.derating(), fixture.observatory.evidence(),
      fixture.observatory.inventory(), fixture.observatory.envelopes(), fixture.at,
      FreshnessPolicy::from_limits(fixture.limits), fixture.limits);
  const Result<DeratingAppraisal> second = appraise_derating(
      fixture.entity, fixture.observatory.derating(), fixture.observatory.evidence(),
      fixture.observatory.inventory(), fixture.observatory.envelopes(), fixture.at,
      FreshnessPolicy::from_limits(fixture.limits), fixture.limits);
  TOBSV_ASSERT_OK(first);
  TOBSV_ASSERT_OK(second);
  TOBSV_ASSERT_EQ(first.value().entries.size(), static_cast<std::size_t>(4));
  for (std::size_t index = 0; index < first.value().entries.size(); ++index) {
    TOBSV_ASSERT_EQ(first.value().entries[index].evidence.id,
                    second.value().entries[index].evidence.id);
  }
  for (std::size_t index = 1; index < first.value().entries.size(); ++index) {
    TOBSV_ASSERT_TRUE(first.value().entries[index - 1].evidence.id <
                      first.value().entries[index].evidence.id);
  }
}
