// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <fstream>

using namespace tobsv;
using namespace tobstest;

namespace {

struct DurableLayout {
  ScratchDirectory scratch;
  std::filesystem::path log;
  Limits limits;

  explicit DurableLayout(const std::string& tag) : scratch(tag), log(scratch.file("store.log")) {
    limits.default_freshness_window = 600LL * 1000 * 1000 * 1000;
  }
};

ThermalQuery query_at(const Timestamp& at, const Limits& limits) {
  ThermalQuery query;
  query.evaluated_at = at;
  query.freshness = FreshnessPolicy::from_limits(limits);
  query.hotspots.episode_gap = Duration::from_seconds(120.0).value();
  return query;
}

}  // namespace

TOBSV_TEST("restart", "durable_state_survives_a_real_reopen") {
  DurableLayout layout("restart-round-trip");
  const EntityId entity = entity_id("node-01");
  const Timestamp at = base_instant();
  std::string digest_before;

  {
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = layout.log;
    config.limits = layout.limits;
    TOBSV_ASSERT_OK(observatory.open(config));
    TOBSV_ASSERT_OK(observatory.register_entity(make_entity("node-01", EntityClass::kComputeNode)));
    TOBSV_ASSERT_OK(observatory.register_envelope(make_envelope(
        entity, EntityClass::kComputeNode, MeasurementSite::kOutlet, 20.0, 35.0, 40.0, 45.0, 50.0)));
    record_observation(observatory, entity, sensor_id("s"), MeasurementSite::kOutlet, 42.0, at, 1);
    TOBSV_ASSERT_OK(observatory.flush_durable());
    const ThermalObservatory::RecoveryReport& recovery = observatory.recovery();
    TOBSV_ASSERT_TRUE(recovery.durable);
    TOBSV_ASSERT_TRUE(recovery.lock_acquired);
    TOBSV_ASSERT_EQ(recovery.commit_failures, static_cast<std::uint64_t>(0));
    digest_before = observatory.analyze(query_at(at, layout.limits)).value().digest;
    TOBSV_ASSERT_OK(observatory.close());
  }

  {
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = layout.log;
    config.limits = layout.limits;
    config.writer_epoch = 2;
    TOBSV_ASSERT_OK(observatory.open(config));
    TOBSV_ASSERT_TRUE(observatory.recovery().records_loaded > 0);
    TOBSV_ASSERT_EQ(observatory.recovery().records_refused_on_replay, static_cast<std::uint64_t>(0));
    TOBSV_ASSERT_EQ(observatory.evidence().observation_count(), static_cast<std::size_t>(1));
    TOBSV_ASSERT_EQ(observatory.inventory().size(), static_cast<std::size_t>(1));
    TOBSV_ASSERT_EQ(observatory.envelopes().size(), static_cast<std::size_t>(1));
    TOBSV_ASSERT_EQ(observatory.recovery().discarded_tail_bytes, static_cast<std::uint64_t>(0));

    const Result<ThermalAnalysis> analysis = observatory.analyze(query_at(at, layout.limits));
    TOBSV_ASSERT_OK(analysis);
    TOBSV_ASSERT_EQ(analysis.value().digest, digest_before);
    TOBSV_ASSERT_EQ(analysis.value().state, EvidenceState::kFresh);
    TOBSV_ASSERT_OK(observatory.close());
  }
}

TOBSV_TEST("restart", "the_fencing_high_water_mark_survives_a_reopen") {
  DurableLayout layout("restart-fence");
  const EntityId entity = entity_id("node-01");
  const Timestamp at = base_instant();
  {
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = layout.log;
    config.limits = layout.limits;
    TOBSV_ASSERT_OK(observatory.open(config));
    record_observation(observatory, entity, sensor_id("s"), MeasurementSite::kOutlet, 42.0, at, 5);
    TOBSV_ASSERT_OK(observatory.flush_durable());
    TOBSV_ASSERT_OK(observatory.close());
  }
  {
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = layout.log;
    config.limits = layout.limits;
    config.writer_epoch = 2;
    TOBSV_ASSERT_OK(observatory.open(config));
    const TemperatureObservation stale = make_observation(
        entity, sensor_id("s"), MeasurementSite::kOutlet, 30.0, at, 2);
    const IngestOutcome refused = observatory.ingest(stale);
    TOBSV_ASSERT_EQ(refused.kind, IngestKind::kRefused);
    TOBSV_ASSERT_EQ(refused.status.code(), ErrorCode::kReplayRejected);
    TOBSV_ASSERT_OK(observatory.close());
  }
}

TOBSV_TEST("restart", "recovered_evidence_is_stale_not_current") {
  DurableLayout layout("restart-freshness");
  const EntityId entity = entity_id("node-01");
  const Timestamp at = base_instant();
  {
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = layout.log;
    config.limits = layout.limits;
    TOBSV_ASSERT_OK(observatory.open(config));
    TOBSV_ASSERT_OK(observatory.register_entity(make_entity("node-01", EntityClass::kComputeNode)));
    TOBSV_ASSERT_OK(observatory.register_envelope(make_envelope(
        entity, EntityClass::kComputeNode, MeasurementSite::kOutlet, 20.0, 35.0, 40.0, 45.0, 50.0)));
    record_observation(observatory, entity, sensor_id("s"), MeasurementSite::kOutlet, 42.0, at, 1);
    TOBSV_ASSERT_OK(observatory.flush_durable());
    TOBSV_ASSERT_OK(observatory.close());
  }
  {
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = layout.log;
    config.limits = layout.limits;
    config.writer_epoch = 2;
    TOBSV_ASSERT_OK(observatory.open(config));
    // A day later the recovered reading is evidence about the past, not a current temperature.
    const Timestamp much_later =
        Timestamp::from_unix_nanos(at.unix_nanos() + 86400LL * 1000 * 1000 * 1000);
    const Result<ThermalAnalysis> analysis =
        observatory.analyze(query_at(much_later, layout.limits));
    TOBSV_ASSERT_OK(analysis);
    TOBSV_ASSERT_EQ(analysis.value().state, EvidenceState::kStale);
    TOBSV_ASSERT_EQ(analysis.value().usable_subject_count, static_cast<std::size_t>(0));
    TOBSV_ASSERT_EQ(analysis.value().stale_subject_count, static_cast<std::size_t>(1));
    TOBSV_ASSERT_TRUE(analysis.value().attribution.contains(AttributionLimitCode::kStaleEvidence));
    TOBSV_ASSERT_OK(observatory.close());
  }
}

TOBSV_TEST("restart", "retirement_is_reapplied_deterministically_on_replay") {
  DurableLayout layout("restart-retirement");
  layout.limits.max_observations_per_subject = 2;
  const EntityId entity = entity_id("node-01");
  {
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = layout.log;
    config.limits = layout.limits;
    TOBSV_ASSERT_OK(observatory.open(config));
    for (std::uint64_t revision = 1; revision <= 5; ++revision) {
      const Timestamp at = Timestamp::from_unix_nanos(base_instant().unix_nanos() +
                                                      static_cast<UnixNanos>(revision) * 1000000000LL);
      record_observation(observatory, entity, sensor_id("s"), MeasurementSite::kOutlet, 40.0, at,
                         revision);
    }
    TOBSV_ASSERT_OK(observatory.flush_durable());
    TOBSV_ASSERT_EQ(observatory.evidence().observation_count(), static_cast<std::size_t>(2));
    TOBSV_ASSERT_EQ(observatory.evidence().retired_count(), static_cast<std::size_t>(3));
    TOBSV_ASSERT_OK(observatory.close());
  }
  {
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = layout.log;
    config.limits = layout.limits;
    config.writer_epoch = 2;
    TOBSV_ASSERT_OK(observatory.open(config));
    TOBSV_ASSERT_EQ(observatory.evidence().observation_count(), static_cast<std::size_t>(2));
    TOBSV_ASSERT_EQ(observatory.evidence().retired_count(), static_cast<std::size_t>(3));
    TOBSV_ASSERT_EQ(observatory.recovery().observations_replayed, static_cast<std::uint64_t>(5));
    TOBSV_ASSERT_OK(observatory.close());
  }
}

TOBSV_TEST("restart", "appends_after_a_reopen_continue_the_sequence") {
  DurableLayout layout("restart-sequence");
  const EntityId entity = entity_id("node-01");
  {
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = layout.log;
    config.limits = layout.limits;
    TOBSV_ASSERT_OK(observatory.open(config));
    record_observation(observatory, entity, sensor_id("s"), MeasurementSite::kOutlet, 41.0,
                       base_instant(), 1);
    TOBSV_ASSERT_OK(observatory.flush_durable());
    TOBSV_ASSERT_OK(observatory.close());
  }
  const Result<ThermalLog::LoadResult> before = ThermalLog::inspect(layout.log, layout.limits);
  TOBSV_ASSERT_OK(before);
  {
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = layout.log;
    config.limits = layout.limits;
    config.writer_epoch = 2;
    TOBSV_ASSERT_OK(observatory.open(config));
    record_observation(observatory, entity, sensor_id("s"), MeasurementSite::kOutlet, 42.0,
                       base_instant(), 2);
    TOBSV_ASSERT_OK(observatory.flush_durable());
    TOBSV_ASSERT_OK(observatory.close());
  }
  const Result<ThermalLog::LoadResult> after = ThermalLog::inspect(layout.log, layout.limits);
  TOBSV_ASSERT_OK(after);
  // One observation per session appends exactly one record: no retirement happened, so no fence
  // checkpoint was needed, and the sequence continues rather than restarting.
  TOBSV_ASSERT_EQ(after.value().payloads.size(), before.value().payloads.size() + 1);
  TOBSV_ASSERT_EQ(after.value().next_sequence, before.value().next_sequence + 1);
}

TOBSV_TEST("restart", "a_torn_tail_on_a_live_store_recovers_without_losing_committed_records") {
  DurableLayout layout("restart-torn");
  const EntityId entity = entity_id("node-01");
  {
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = layout.log;
    config.limits = layout.limits;
    TOBSV_ASSERT_OK(observatory.open(config));
    record_observation(observatory, entity, sensor_id("s"), MeasurementSite::kOutlet, 41.0,
                       base_instant(), 1);
    record_observation(observatory, entity, sensor_id("s"), MeasurementSite::kOutlet, 42.0,
                       base_instant(), 2);
    TOBSV_ASSERT_OK(observatory.flush_durable());
    TOBSV_ASSERT_OK(observatory.close());
  }
  const Result<std::string> bytes = read_file_bytes(layout.log, 1U << 20);
  TOBSV_ASSERT_OK(bytes);
  {
    // Simulate a process that died mid-write of a third record.
    std::ofstream out(layout.log, std::ios::binary | std::ios::app);
    out.write("partial-frame", 13);
  }
  {
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = layout.log;
    config.limits = layout.limits;
    config.writer_epoch = 2;
    TOBSV_ASSERT_OK(observatory.open(config));
    TOBSV_ASSERT_TRUE(observatory.recovery().discarded_tail_bytes > 0);
    TOBSV_ASSERT_EQ(observatory.evidence().observation_count(), static_cast<std::size_t>(2));
    TOBSV_ASSERT_EQ(observatory.recovery().records_refused_on_replay, static_cast<std::uint64_t>(0));

    record_observation(observatory, entity, sensor_id("s"), MeasurementSite::kOutlet, 43.0,
                       base_instant(), 3);
    TOBSV_ASSERT_OK(observatory.flush_durable());
    TOBSV_ASSERT_OK(observatory.close());
  }
  const Result<ThermalLog::LoadResult> after = ThermalLog::inspect(layout.log, layout.limits);
  TOBSV_ASSERT_OK(after);
  TOBSV_ASSERT_EQ(after.value().discarded_tail_bytes, static_cast<std::uint64_t>(0));
  TOBSV_ASSERT_TRUE(after.value().payloads.size() >= 3);

  ThermalObservatory final_check;
  ObservatoryConfig config;
  config.log_path = layout.log;
  config.limits = layout.limits;
  config.writer_epoch = 3;
  TOBSV_ASSERT_OK(final_check.open(config));
  TOBSV_ASSERT_EQ(final_check.evidence().observation_count(), static_cast<std::size_t>(3));
  TOBSV_ASSERT_OK(final_check.close());
}

TOBSV_TEST("restart", "coupling_and_derating_claims_survive_a_reopen") {
  DurableLayout layout("restart-relations");
  const EntityId first = entity_id("node-01");
  const EntityId second = entity_id("node-02");
  const Timestamp at = base_instant();
  {
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = layout.log;
    config.limits = layout.limits;
    TOBSV_ASSERT_OK(observatory.open(config));
    for (const char* name : {"node-01", "node-02"}) {
      TOBSV_ASSERT_OK(observatory.register_entity(make_entity(name, EntityClass::kComputeNode)));
      TOBSV_ASSERT_OK(observatory.register_envelope(
          make_envelope(entity_id(name), EntityClass::kComputeNode, MeasurementSite::kOutlet, 20.0,
                        35.0, 40.0, 45.0, 50.0)));
    }
    const ObservationId a = record_observation(observatory, first, sensor_id("s"),
                                               MeasurementSite::kOutlet, 42.0, at, 1);
    const ObservationId b = record_observation(observatory, second, sensor_id("s"),
                                               MeasurementSite::kOutlet, 43.0, at, 2);
    TOBSV_ASSERT_OK(observatory.declare_adjacency(
        make_adjacency(first, second, AdjacencyKind::kSharedEnclosure)));
    TOBSV_ASSERT_OK(observatory.record_coupling(make_coupling(
        first, second, CouplingKind::kSupportedThermal, 0.6, {cite(a), cite(b)}, at)));
    TOBSV_ASSERT_OK(observatory.record_derating(make_derating(
        first, DeratingKind::kClockThrottle, 0.25, {a}, ThresholdLevel::kHigh, at)));
    TOBSV_ASSERT_OK(observatory.flush_durable());
    TOBSV_ASSERT_OK(observatory.close());
  }
  {
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = layout.log;
    config.limits = layout.limits;
    config.writer_epoch = 2;
    TOBSV_ASSERT_OK(observatory.open(config));
    TOBSV_ASSERT_EQ(observatory.coupling().size(), static_cast<std::size_t>(1));
    TOBSV_ASSERT_EQ(observatory.topology().edge_count(), static_cast<std::size_t>(1));
    TOBSV_ASSERT_EQ(observatory.derating().size(), static_cast<std::size_t>(1));
    TOBSV_ASSERT_EQ(observatory.recovery().records_refused_on_replay, static_cast<std::uint64_t>(0));

    ThermalQuery query;
    query.evaluated_at = at;
    query.freshness = FreshnessPolicy::from_limits(layout.limits);
    const Result<ThermalAnalysis> analysis = observatory.analyze(query);
    TOBSV_ASSERT_OK(analysis);
    TOBSV_ASSERT_EQ(analysis.value().propagation.paths.size(), static_cast<std::size_t>(1));
    TOBSV_ASSERT_EQ(analysis.value().deratings.size(), static_cast<std::size_t>(1));
    TOBSV_ASSERT_OK(observatory.close());
  }
}

TOBSV_TEST("restart", "closing_twice_and_reopening_after_close_are_both_safe") {
  DurableLayout layout("restart-close");
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.log_path = layout.log;
  config.limits = layout.limits;
  TOBSV_ASSERT_OK(observatory.open(config));
  TOBSV_ASSERT_TRUE(observatory.is_open());
  TOBSV_ASSERT_FAILS_WITH(observatory.open(config), ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_OK(observatory.close());
  TOBSV_ASSERT_FALSE(observatory.is_open());
  TOBSV_ASSERT_OK(observatory.close());
  TOBSV_ASSERT_FAILS_WITH(observatory.analyze_now(), ErrorCode::kClosed);
  const IngestOutcome refused = observatory.ingest(make_observation(
      entity_id("node-01"), sensor_id("s"), MeasurementSite::kOutlet, 40.0, base_instant(), 1));
  TOBSV_ASSERT_EQ(refused.kind, IngestKind::kRefused);
  TOBSV_ASSERT_EQ(refused.status.code(), ErrorCode::kClosed);
}
TOBSV_TEST("restart", "reopening_the_same_object_starts_from_the_durable_state") {
  DurableLayout layout("restart-reopen-object");
  const EntityId entity = entity_id("node-01");
  const Timestamp at = base_instant();
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.log_path = layout.log;
  config.limits = layout.limits;
  TOBSV_ASSERT_OK(observatory.open(config));
  TOBSV_ASSERT_OK(observatory.register_entity(make_entity("node-01", EntityClass::kComputeNode)));
  TOBSV_ASSERT_OK(observatory.register_envelope(make_envelope(
      entity, EntityClass::kComputeNode, MeasurementSite::kOutlet, 20.0, 35.0, 40.0, 45.0, 50.0)));
  record_observation(observatory, entity, sensor_id("s"), MeasurementSite::kOutlet, 42.0, at, 1);
  TOBSV_ASSERT_OK(observatory.flush_durable());
  TOBSV_ASSERT_OK(observatory.close());

  // The same object opened again must land on exactly the durable state: one entity, one envelope,
  // one observation, and no record refused as a duplicate of something the previous session had
  // already recovered.
  TOBSV_ASSERT_OK(observatory.open(config));
  TOBSV_ASSERT_EQ(observatory.evidence().observation_count(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(observatory.inventory().size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(observatory.envelopes().size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(observatory.recovery().records_refused_on_replay, static_cast<std::uint64_t>(0));
  TOBSV_ASSERT_EQ(observatory.topology().edge_count(), static_cast<std::size_t>(0));
  TOBSV_ASSERT_EQ(observatory.coupling().size(), static_cast<std::size_t>(0));
  TOBSV_ASSERT_EQ(observatory.derating().size(), static_cast<std::size_t>(0));

  const Result<ThermalAnalysis> analysis = observatory.analyze(query_at(at, layout.limits));
  TOBSV_ASSERT_OK(analysis);
  TOBSV_ASSERT_EQ(analysis.value().usable_subject_count, static_cast<std::size_t>(1));
  TOBSV_ASSERT_OK(observatory.close());
}
