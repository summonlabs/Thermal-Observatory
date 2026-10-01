// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "test_framework.hpp"
#include "test_support.hpp"

using namespace tobsv;
using namespace tobstest;

namespace {

// The framework status assertions are defined over Status; a Result carries its Status through
// error(), so this adapter lets a Result be checked with the same macros and messages.
template <class T>
Status status_of(const Result<T>& result) {
  return result.error();
}

}  // namespace

TOBSV_TEST("model", "evidence_state_vocabulary_round_trips") {
  struct Named {
    EvidenceState value;
    const char* name;
  };
  const Named states[] = {
      {EvidenceState::kFresh, "fresh"},         {EvidenceState::kStale, "stale"},
      {EvidenceState::kUnknown, "unknown"},     {EvidenceState::kConflicting, "conflicting"},
      {EvidenceState::kUnsupported, "unsupported"},
      {EvidenceState::kIndeterminate, "indeterminate"},
      {EvidenceState::kRefused, "refused"},
  };
  for (const Named& entry : states) {
    TOBSV_ASSERT_EQ(to_string(entry.value), std::string_view(entry.name));
    EvidenceState parsed = EvidenceState::kRefused;
    TOBSV_ASSERT_TRUE(parse_evidence_state(entry.name, parsed));
    TOBSV_ASSERT_TRUE(parsed == entry.value);
  }
  // An unrecognised spelling is refused rather than mapped to a default, and the output value is
  // left untouched so a caller cannot mistake the old value for a parsed one.
  EvidenceState untouched = EvidenceState::kFresh;
  TOBSV_ASSERT_FALSE(parse_evidence_state("FRESH", untouched));
  TOBSV_ASSERT_FALSE(parse_evidence_state("", untouched));
  TOBSV_ASSERT_TRUE(untouched == EvidenceState::kFresh);
  // Only fresh evidence is usable; everything else has to be reported as a limitation.
  TOBSV_ASSERT_TRUE(is_usable(EvidenceState::kFresh));
  TOBSV_ASSERT_FALSE(is_usable(EvidenceState::kStale));
  TOBSV_ASSERT_FALSE(is_usable(EvidenceState::kUnknown));
  TOBSV_ASSERT_FALSE(is_usable(EvidenceState::kRefused));
}

TOBSV_TEST("model", "measurement_site_vocabulary_round_trips") {
  struct Named {
    MeasurementSite value;
    const char* name;
  };
  const Named sites[] = {
      {MeasurementSite::kUnknown, "unknown"},
      {MeasurementSite::kDie, "die"},
      {MeasurementSite::kCase, "case"},
      {MeasurementSite::kBoard, "board"},
      {MeasurementSite::kInlet, "inlet"},
      {MeasurementSite::kOutlet, "outlet"},
      {MeasurementSite::kCoolantSupply, "coolant_supply"},
      {MeasurementSite::kCoolantReturn, "coolant_return"},
      {MeasurementSite::kAmbient, "ambient"},
      {MeasurementSite::kMemory, "memory"},
      {MeasurementSite::kPowerStage, "power_stage"},
  };
  for (const Named& entry : sites) {
    MeasurementSite parsed = MeasurementSite::kUnknown;
    TOBSV_ASSERT_TRUE(parse_measurement_site(to_string(entry.value), parsed));
    TOBSV_ASSERT_TRUE(parsed == entry.value);
    TOBSV_ASSERT_EQ(to_string(entry.value), std::string_view(entry.name));
  }
  // The names are distinct: two sites that spelled the same would make two incomparable readings
  // look like one subject.
  for (std::size_t left = 0; left < 11; ++left) {
    for (std::size_t right = left + 1; right < 11; ++right) {
      TOBSV_ASSERT_TRUE(to_string(sites[left].value) != to_string(sites[right].value));
    }
  }
  MeasurementSite untouched = MeasurementSite::kDie;
  TOBSV_ASSERT_FALSE(parse_measurement_site("Outlet", untouched));
  TOBSV_ASSERT_TRUE(untouched == MeasurementSite::kDie);
}

TOBSV_TEST("model", "entity_class_vocabulary_round_trips") {
  struct Named {
    EntityClass value;
    const char* name;
  };
  const Named classes[] = {
      {EntityClass::kUnknown, "unknown"},
      {EntityClass::kComputeNode, "compute_node"},
      {EntityClass::kAccelerator, "accelerator"},
      {EntityClass::kCpuPackage, "cpu_package"},
      {EntityClass::kMemoryModule, "memory_module"},
      {EntityClass::kStorageDevice, "storage_device"},
      {EntityClass::kPowerSupply, "power_supply"},
      {EntityClass::kNetworkSwitch, "network_switch"},
      {EntityClass::kRack, "rack"},
      {EntityClass::kCoolantLoop, "coolant_loop"},
      {EntityClass::kEnvironmentalPoint, "environmental_point"},
  };
  for (const Named& entry : classes) {
    EntityClass parsed = EntityClass::kRack;
    TOBSV_ASSERT_TRUE(parse_entity_class(to_string(entry.value), parsed));
    TOBSV_ASSERT_TRUE(parsed == entry.value);
    TOBSV_ASSERT_EQ(to_string(entry.value), std::string_view(entry.name));
  }
  EntityClass untouched = EntityClass::kRack;
  TOBSV_ASSERT_FALSE(parse_entity_class("compute node", untouched));
  TOBSV_ASSERT_TRUE(untouched == EntityClass::kRack);
}

TOBSV_TEST("model", "threshold_level_vocabulary_and_neighbours") {
  struct Named {
    ThresholdLevel value;
    const char* name;
  };
  const Named levels[] = {
      {ThresholdLevel::kNominal, "nominal"},   {ThresholdLevel::kWarn, "warn"},
      {ThresholdLevel::kHigh, "high"},         {ThresholdLevel::kCritical, "critical"},
      {ThresholdLevel::kMaximum, "maximum"},
  };
  for (const Named& entry : levels) {
    ThresholdLevel parsed = ThresholdLevel::kNominal;
    TOBSV_ASSERT_TRUE(parse_threshold_level(to_string(entry.value), parsed));
    TOBSV_ASSERT_TRUE(parsed == entry.value);
    TOBSV_ASSERT_EQ(to_string(entry.value), std::string_view(entry.name));
  }
  // The neighbours walk the declared order and clamp at both ends rather than wrapping, so a
  // caller can never step from the coldest band to the hottest by accident.
  TOBSV_ASSERT_TRUE(raise(ThresholdLevel::kNominal) == ThresholdLevel::kWarn);
  TOBSV_ASSERT_TRUE(raise(ThresholdLevel::kCritical) == ThresholdLevel::kMaximum);
  TOBSV_ASSERT_TRUE(raise(ThresholdLevel::kMaximum) == ThresholdLevel::kMaximum);
  TOBSV_ASSERT_TRUE(lower(ThresholdLevel::kWarn) == ThresholdLevel::kNominal);
  TOBSV_ASSERT_TRUE(lower(ThresholdLevel::kMaximum) == ThresholdLevel::kCritical);
  TOBSV_ASSERT_TRUE(lower(ThresholdLevel::kNominal) == ThresholdLevel::kNominal);
  // The underlying numbering must agree with the declared order, because band comparisons rely on
  // it whenever the textual names are not consulted.
  const ThresholdLevel ascending[] = {ThresholdLevel::kNominal, ThresholdLevel::kWarn,
                                      ThresholdLevel::kHigh, ThresholdLevel::kCritical,
                                      ThresholdLevel::kMaximum};
  for (std::size_t index = 1; index < 5; ++index) {
    TOBSV_ASSERT_TRUE(static_cast<unsigned>(ascending[index - 1]) <
                      static_cast<unsigned>(ascending[index]));
  }
  ThresholdLevel untouched = ThresholdLevel::kWarn;
  TOBSV_ASSERT_FALSE(parse_threshold_level("WARN", untouched));
  TOBSV_ASSERT_TRUE(untouched == ThresholdLevel::kWarn);
}

TOBSV_TEST("model", "clock_ingest_and_record_vocabulary_round_trips") {
  struct NamedClock {
    ClockDomain value;
    const char* name;
  };
  const NamedClock clocks[] = {
      {ClockDomain::kUnspecified, "unspecified"},
      {ClockDomain::kCollectorWallClock, "collector_wall_clock"},
      {ClockDomain::kSensorMonotonic, "sensor_monotonic"},
      {ClockDomain::kHostMonotonic, "host_monotonic"},
      {ClockDomain::kSynthetic, "synthetic"},
  };
  for (const NamedClock& entry : clocks) {
    ClockDomain parsed = ClockDomain::kUnspecified;
    TOBSV_ASSERT_TRUE(parse_clock_domain(to_string(entry.value), parsed));
    TOBSV_ASSERT_TRUE(parsed == entry.value);
    TOBSV_ASSERT_EQ(to_string(entry.value), std::string_view(entry.name));
  }
  ClockDomain untouched = ClockDomain::kSensorMonotonic;
  TOBSV_ASSERT_FALSE(parse_clock_domain("sensor", untouched));
  TOBSV_ASSERT_TRUE(untouched == ClockDomain::kSensorMonotonic);

  // The ingest verdicts and the durable record kinds are the vocabulary of every report a caller
  // reads, so each one has a stable spelling.
  struct NamedIngest {
    IngestKind value;
    const char* name;
  };
  const NamedIngest ingests[] = {
      {IngestKind::kRecorded, "recorded"},
      {IngestKind::kDuplicate, "duplicate"},
      {IngestKind::kRefused, "refused"},
      {IngestKind::kConflict, "conflict"},
  };
  for (const NamedIngest& entry : ingests) {
    TOBSV_ASSERT_EQ(to_string(entry.value), std::string_view(entry.name));
  }
  struct NamedKind {
    RecordKind value;
    const char* name;
  };
  const NamedKind kinds[] = {
      {RecordKind::kObservation, "observation"}, {RecordKind::kFence, "fence"},
      {RecordKind::kEnvelope, "envelope"},       {RecordKind::kEntity, "entity"},
      {RecordKind::kAdjacency, "adjacency"},     {RecordKind::kCoupling, "coupling"},
      {RecordKind::kDerating, "derating"},
  };
  for (const NamedKind& entry : kinds) {
    RecordKind parsed = RecordKind::kFence;
    TOBSV_ASSERT_TRUE(parse_record_kind(to_string(entry.value), parsed));
    TOBSV_ASSERT_TRUE(parsed == entry.value);
    TOBSV_ASSERT_EQ(to_string(entry.value), std::string_view(entry.name));
  }
  RecordKind untouched_kind = RecordKind::kFence;
  TOBSV_ASSERT_FALSE(parse_record_kind("records", untouched_kind));
  TOBSV_ASSERT_TRUE(untouched_kind == RecordKind::kFence);
}

TOBSV_TEST("model", "counter_saturates_and_orders_totally") {
  const Epoch unset;
  TOBSV_ASSERT_FALSE(unset.is_set());
  TOBSV_ASSERT_EQ(unset.value(), static_cast<std::uint64_t>(0));
  TOBSV_ASSERT_EQ(unset.to_string(), std::string("0"));

  const Epoch first = Epoch::from(1);
  TOBSV_ASSERT_TRUE(first.is_set());
  TOBSV_ASSERT_TRUE(first.next().value() == 2);
  TOBSV_ASSERT_TRUE(first < first.next());
  TOBSV_ASSERT_TRUE(first.next() > first);
  TOBSV_ASSERT_TRUE(first <= first);
  TOBSV_ASSERT_TRUE(first >= first);
  TOBSV_ASSERT_TRUE(first == Epoch::from(1));
  TOBSV_ASSERT_TRUE(first != Epoch::from(2));
  TOBSV_ASSERT_EQ(first.to_string(), std::string("1"));

  // Saturation is the conservative choice: a counter that cannot advance must not wrap onto a value
  // that would be classified as new evidence.
  const Epoch saturated = Epoch::from(std::numeric_limits<std::uint64_t>::max());
  TOBSV_ASSERT_TRUE(saturated.next() == saturated);
  TOBSV_ASSERT_TRUE(saturated.next().value() == std::numeric_limits<std::uint64_t>::max());
  TOBSV_ASSERT_TRUE(Epoch::from(0) < first);
  // Distinct counter types are not interchangeable, which is why the tag exists; the comparison
  // below is between two epochs and would not compile against a generation.
  TOBSV_ASSERT_TRUE(Epoch::from(7) > Epoch::from(6));
}

TOBSV_TEST("model", "fence_validate_rejects_each_missing_component") {
  const SourceId source = source_id("fence-source");
  const Fence complete = make_fence(source, 4);
  TOBSV_ASSERT_OK(complete.validate());
  TOBSV_ASSERT_TRUE(complete.to_string().find("fence-source") != std::string::npos);

  Fence no_source = complete;
  no_source.source = SourceId{};
  TOBSV_ASSERT_FAILS_WITH(no_source.validate(), ErrorCode::kInvalidArgument);

  Fence zero_epoch = complete;
  zero_epoch.epoch = Epoch::from(0);
  TOBSV_ASSERT_FAILS_WITH(zero_epoch.validate(), ErrorCode::kInvalidArgument);

  Fence zero_generation = complete;
  zero_generation.generation = Generation::from(0);
  TOBSV_ASSERT_FAILS_WITH(zero_generation.validate(), ErrorCode::kInvalidArgument);

  Fence zero_revision = complete;
  zero_revision.revision = Revision::from(0);
  TOBSV_ASSERT_FAILS_WITH(zero_revision.validate(), ErrorCode::kInvalidArgument);

  Fence zero_incarnation = complete;
  zero_incarnation.incarnation = Incarnation::from(0);
  TOBSV_ASSERT_FAILS_WITH(zero_incarnation.validate(), ErrorCode::kInvalidArgument);

  Fence zero_sequence = complete;
  zero_sequence.sequence = Sequence::from(0);
  TOBSV_ASSERT_FAILS_WITH(zero_sequence.validate(), ErrorCode::kInvalidArgument);

  // A retry identity is what makes a repeated mutation recognisable, so a fence without one cannot
  // be accepted even though every counter is valid.
  Fence no_attempt = complete;
  no_attempt.attempt = AttemptId{};
  TOBSV_ASSERT_FAILS_WITH(no_attempt.validate(), ErrorCode::kInvalidArgument);
}

TOBSV_TEST("model", "fence_text_names_every_component") {
  const Fence fence = make_fence(source_id("src-a"), 7, 2, 3, 9, "att-9");
  TOBSV_ASSERT_EQ(fence.to_string(), std::string("src-a/e2/g3/r7/i1/s9/aatt-9"));
  // An unset fence renders placeholders rather than zeros that could be read as real counters.
  TOBSV_ASSERT_EQ(Fence{}.to_string(), std::string("<unset>/e0/g0/r0/i0/s0/a<unset>"));
  TOBSV_ASSERT_TRUE(make_fence(source_id("src-b"), 7, 2, 3, 9, "att-9").to_string() !=
                    fence.to_string());
}

TOBSV_TEST("model", "order_is_incomparable_across_sources_and_incarnations") {
  const SourceId source = source_id("src-order");
  const Fence reference = make_fence(source, 5, 2, 3, 5, "att-5");
  TOBSV_ASSERT_TRUE(order(reference, reference) == FenceOrdering::kEqual);
  TOBSV_ASSERT_TRUE(order(make_fence(source, 6, 2, 3, 6), reference) == FenceOrdering::kNewer);
  TOBSV_ASSERT_TRUE(order(make_fence(source, 4, 2, 3, 4), reference) == FenceOrdering::kOlder);
  // The epoch dominates every counter below it: an older epoch is older whatever it claims.
  TOBSV_ASSERT_TRUE(order(make_fence(source, 1, 3, 1, 1), reference) == FenceOrdering::kNewer);
  TOBSV_ASSERT_TRUE(order(make_fence(source, 9, 1, 9, 9), reference) == FenceOrdering::kOlder);
  TOBSV_ASSERT_TRUE(order(make_fence(source, 1, 2, 4, 1), reference) == FenceOrdering::kNewer);
  TOBSV_ASSERT_TRUE(order(make_fence(source, 1, 2, 2, 1), reference) == FenceOrdering::kOlder);

  // Evidence from another source, or from another process incarnation, has no order at all; claiming
  // one would invent an ordering the evidence does not carry.
  TOBSV_ASSERT_TRUE(order(make_fence(source_id("src-other"), 5, 2, 3, 5), reference) ==
                    FenceOrdering::kIncomparable);
  Fence other_incarnation = reference;
  other_incarnation.incarnation = Incarnation::from(2);
  TOBSV_ASSERT_TRUE(order(other_incarnation, reference) == FenceOrdering::kIncomparable);
  TOBSV_ASSERT_TRUE(order(reference, other_incarnation) == FenceOrdering::kIncomparable);
}

TOBSV_TEST("model", "tracker_classifies_advances_and_opens_new_epochs") {
  const SourceId source = source_id("src-track");
  FenceTracker tracker(source);
  TOBSV_ASSERT_FALSE(tracker.has_high_water());
  TOBSV_ASSERT_TRUE(tracker.classify(make_fence(source, 1), false) == FenceVerdict::kAdvance);
  TOBSV_ASSERT_OK(tracker.observe(make_fence(source, 1), false));
  TOBSV_ASSERT_TRUE(tracker.has_high_water());
  TOBSV_ASSERT_TRUE(tracker.high_water().revision.value() == 1);

  // A higher revision, a higher generation and a higher sequence within one revision are all
  // advances; the classification is what keeps the mark monotonic.
  TOBSV_ASSERT_TRUE(tracker.classify(make_fence(source, 2), false) == FenceVerdict::kAdvance);
  TOBSV_ASSERT_TRUE(tracker.classify(make_fence(source, 1, 1, 2, 1), false) ==
                    FenceVerdict::kAdvance);
  TOBSV_ASSERT_TRUE(tracker.classify(make_fence(source, 1, 1, 1, 5), false) ==
                    FenceVerdict::kAdvance);

  TOBSV_ASSERT_OK(tracker.observe(make_fence(source, 3), false));
  TOBSV_ASSERT_TRUE(tracker.classify(make_fence(source, 1, 2, 1, 1), false) ==
                    FenceVerdict::kNewEpoch);
  TOBSV_ASSERT_OK(tracker.observe(make_fence(source, 1, 2, 1, 1), false));
  // A new epoch resets the counters below it: the revision is allowed to fall back.
  TOBSV_ASSERT_TRUE(tracker.high_water().epoch.value() == 2);
  TOBSV_ASSERT_TRUE(tracker.high_water().revision.value() == 1);
}

TOBSV_TEST("model", "tracker_classifies_duplicates_conflicts_and_stale_positions") {
  const SourceId source = source_id("src-track2");
  FenceTracker tracker(source);
  TOBSV_ASSERT_OK(tracker.observe(make_fence(source, 4, 3, 2, 4), false));

  // The identical position with the same content is an idempotent retry.
  TOBSV_ASSERT_TRUE(tracker.classify(make_fence(source, 4, 3, 2, 4), true) ==
                    FenceVerdict::kDuplicate);
  TOBSV_ASSERT_FAILS_WITH(tracker.observe(make_fence(source, 4, 3, 2, 4), true),
                          ErrorCode::kDuplicateIdentity);
  // The identical position with different content is a contradiction, not a retry.
  TOBSV_ASSERT_TRUE(tracker.classify(make_fence(source, 4, 3, 2, 4), false) ==
                    FenceVerdict::kConflict);
  TOBSV_ASSERT_FAILS_WITH(tracker.observe(make_fence(source, 4, 3, 2, 4), false),
                          ErrorCode::kConflict);
  // An earlier revision inside the same generation is a replay of a superseded position.
  TOBSV_ASSERT_TRUE(tracker.classify(make_fence(source, 3, 3, 2, 3), false) ==
                    FenceVerdict::kReplayRejected);
  TOBSV_ASSERT_FAILS_WITH(tracker.observe(make_fence(source, 3, 3, 2, 3), false),
                          ErrorCode::kReplayRejected);
  // An earlier sequence inside the current revision is a reordered replay.
  TOBSV_ASSERT_TRUE(tracker.classify(make_fence(source, 4, 3, 2, 2), false) ==
                    FenceVerdict::kReplayRejected);
  // An older epoch and an older generation are distinct failures, because an operator has to know
  // whether a writer restarted or merely fell behind.
  TOBSV_ASSERT_TRUE(tracker.classify(make_fence(source, 9, 2, 9, 9), false) ==
                    FenceVerdict::kStaleEpoch);
  TOBSV_ASSERT_FAILS_WITH(tracker.observe(make_fence(source, 9, 2, 9, 9), false),
                          ErrorCode::kStaleEpoch);
  TOBSV_ASSERT_TRUE(tracker.classify(make_fence(source, 9, 3, 1, 9), false) ==
                    FenceVerdict::kStaleGeneration);
  TOBSV_ASSERT_FAILS_WITH(tracker.observe(make_fence(source, 9, 3, 1, 9), false),
                          ErrorCode::kStaleGeneration);

  // None of the refusals may move the mark.
  TOBSV_ASSERT_TRUE(tracker.high_water().revision.value() == 4);
  TOBSV_ASSERT_TRUE(tracker.high_water().epoch.value() == 3);
  TOBSV_ASSERT_TRUE(tracker.high_water().generation.value() == 2);
}

TOBSV_TEST("model", "tracker_refuses_an_incomparable_incarnation") {
  const SourceId source = source_id("src-track3");
  FenceTracker tracker(source);
  TOBSV_ASSERT_OK(tracker.observe(make_fence(source, 1), false));
  Fence other_incarnation = make_fence(source, 5);
  other_incarnation.incarnation = Incarnation::from(7);
  TOBSV_ASSERT_TRUE(tracker.classify(other_incarnation, false) == FenceVerdict::kIncomparable);
  TOBSV_ASSERT_FAILS_WITH(tracker.observe(other_incarnation, false), ErrorCode::kConflict);
  Fence foreign_source = make_fence(source_id("src-foreign"), 5);
  TOBSV_ASSERT_TRUE(tracker.classify(foreign_source, false) == FenceVerdict::kIncomparable);
  TOBSV_ASSERT_FAILS_WITH(tracker.observe(foreign_source, false), ErrorCode::kConflict);

  // observe() validates before it classifies, so an invalid fence cannot become the mark.
  Fence invalid = make_fence(source, 2);
  invalid.attempt = AttemptId{};
  TOBSV_ASSERT_FAILS_WITH(tracker.observe(invalid, false), ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_TRUE(tracker.high_water().revision.value() == 1);
  // reset() forgets the mark so that a reopened store cannot inherit a stale position.
  tracker.reset();
  TOBSV_ASSERT_FALSE(tracker.has_high_water());
  TOBSV_ASSERT_TRUE(tracker.classify(make_fence(source, 1), false) == FenceVerdict::kAdvance);
  TOBSV_ASSERT_TRUE(to_string(FenceVerdict::kReplayRejected) == std::string_view("replay_rejected"));
  TOBSV_ASSERT_TRUE(to_string(FenceVerdict::kIncomparable) == std::string_view("incomparable"));
}

TOBSV_TEST("model", "force_high_water_restores_without_classifying") {
  FenceTracker unbound;
  TOBSV_ASSERT_OK(unbound.force_high_water(make_fence(source_id("src-a"), 2)));
  TOBSV_ASSERT_TRUE(unbound.source().value() == std::string("src-a"));
  TOBSV_ASSERT_TRUE(unbound.has_high_water());
  TOBSV_ASSERT_FAILS_WITH(unbound.force_high_water(make_fence(source_id("src-b"), 3)),
                          ErrorCode::kConflict);

  FenceTracker bound(source_id("src-c"));
  TOBSV_ASSERT_FAILS_WITH(bound.force_high_water(make_fence(source_id("src-d"), 1)),
                          ErrorCode::kConflict);
  TOBSV_ASSERT_FALSE(bound.has_high_water());

  // Restoring replays durable state, so a lower position is accepted when the caller says the
  // durable record holds it; validation still applies.
  FenceTracker restored(source_id("src-e"));
  TOBSV_ASSERT_OK(restored.force_high_water(make_fence(source_id("src-e"), 9)));
  TOBSV_ASSERT_OK(restored.force_high_water(make_fence(source_id("src-e"), 2)));
  TOBSV_ASSERT_TRUE(restored.high_water().revision.value() == 2);
  TOBSV_ASSERT_FAILS_WITH(restored.force_high_water(Fence{}), ErrorCode::kInvalidArgument);
  // After restoring, ordinary classification resumes against the restored mark.
  TOBSV_ASSERT_TRUE(restored.classify(make_fence(source_id("src-e"), 1), false) ==
                    FenceVerdict::kReplayRejected);
}

TOBSV_TEST("model", "topology_add_entity_keeps_a_canonical_set") {
  ThermalTopology topology;
  const Limits limits;
  TOBSV_ASSERT_FAILS_WITH(topology.add_entity(EntityId{}, limits), ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_OK(topology.add_entity(entity_id("rack-1"), limits));
  TOBSV_ASSERT_FAILS_WITH(topology.add_entity(entity_id("rack-1"), limits),
                          ErrorCode::kDuplicateIdentity);
  TOBSV_ASSERT_EQ(topology.entity_count(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_TRUE(topology.has_entity(entity_id("rack-1")));
  TOBSV_ASSERT_FALSE(topology.has_entity(entity_id("rack-2")));

  // Entities are retained in canonical order regardless of the order they arrived in, which is what
  // makes a report byte-stable.
  TOBSV_ASSERT_OK(topology.add_entity(entity_id("rack-0"), limits));
  const std::vector<EntityId> entities = topology.entities();
  TOBSV_ASSERT_EQ(entities.size(), static_cast<std::size_t>(2));
  TOBSV_ASSERT_EQ(entities[0].value(), std::string("rack-0"));
  TOBSV_ASSERT_EQ(entities[1].value(), std::string("rack-1"));
}

TOBSV_TEST("model", "topology_edge_validation_rejects_incomplete_statements") {
  ThermalTopology topology;
  const Limits limits;
  const EntityId a = entity_id("node-a");
  const EntityId b = entity_id("node-b");
  const TopologyEdge valid = make_adjacency(a, b, AdjacencyKind::kSharedEnclosure);
  TOBSV_ASSERT_OK(valid.validate());

  TopologyEdge self = valid;
  self.to = a;
  TOBSV_ASSERT_FAILS_WITH(self.validate(), ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_FAILS_WITH(topology.add_edge(self, limits), ErrorCode::kInvalidArgument);

  TopologyEdge missing_endpoint = valid;
  missing_endpoint.from = EntityId{};
  TOBSV_ASSERT_FAILS_WITH(missing_endpoint.validate(), ErrorCode::kInvalidArgument);

  // An adjacency of unknown kind says nothing about where heat can travel.
  TopologyEdge unknown_kind = valid;
  unknown_kind.kind = AdjacencyKind::kDeclaredUnknown;
  TOBSV_ASSERT_FAILS_WITH(unknown_kind.validate(), ErrorCode::kInvalidArgument);

  // The declaration has to name the topology it came from and the authority that declared it.
  TopologyEdge no_topology = valid;
  no_topology.topology = TopologyRef{};
  TOBSV_ASSERT_FAILS_WITH(no_topology.validate(), ErrorCode::kInvalidArgument);
  TopologyEdge no_source = valid;
  no_source.declared_by = SourceId{};
  TOBSV_ASSERT_FAILS_WITH(no_source.validate(), ErrorCode::kInvalidArgument);

  TOBSV_ASSERT_OK(topology.add_edge(valid, limits));
  TOBSV_ASSERT_EQ(topology.edge_count(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(topology.edges().size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_OK(topology.add_entity(a, limits));
}

TOBSV_TEST("model", "topology_rejects_duplicate_edges_and_enforces_the_degree_bound") {
  ThermalTopology topology;
  Limits limits;
  limits.max_edges_per_entity = 2;
  const EntityId hub = entity_id("hub");
  const EntityId leaf_c = entity_id("leaf-c");
  const EntityId leaf_d = entity_id("leaf-d");

  TOBSV_ASSERT_OK(
      topology.add_edge(make_adjacency(hub, leaf_c, AdjacencyKind::kSharedEnclosure), limits));
  // Adjacency is undirected for grouping, so the reversed pair is the same declaration.
  TOBSV_ASSERT_FAILS_WITH(
      topology.add_edge(make_adjacency(leaf_c, hub, AdjacencyKind::kSharedEnclosure), limits),
      ErrorCode::kDuplicateIdentity);
  // A different declared kind is a different statement and is kept.
  TOBSV_ASSERT_OK(
      topology.add_edge(make_adjacency(hub, leaf_c, AdjacencyKind::kAirflowPathProximity), limits));
  TOBSV_ASSERT_EQ(topology.edge_count(), static_cast<std::size_t>(2));

  // The hub has now reached its degree bound, so a further adjacency is refused rather than
  // silently dropped.
  TOBSV_ASSERT_FAILS_WITH(
      topology.add_edge(make_adjacency(hub, leaf_d, AdjacencyKind::kSharedEnclosure), limits),
      ErrorCode::kLimitExceeded);
  TOBSV_ASSERT_EQ(topology.edge_count(), static_cast<std::size_t>(2));
}

TOBSV_TEST("model", "topology_neighbors_are_sorted_unique_and_symmetric") {
  ThermalTopology topology;
  const Limits limits;
  const EntityId a = entity_id("node-a");
  const EntityId b = entity_id("node-b");
  const EntityId c = entity_id("node-c");
  TOBSV_ASSERT_OK(topology.add_edge(make_adjacency(b, a, AdjacencyKind::kPhysicalContainment),
                                    limits));
  TOBSV_ASSERT_OK(topology.add_edge(make_adjacency(a, b, AdjacencyKind::kAirflowPathProximity),
                                    limits));
  TOBSV_ASSERT_OK(topology.add_edge(make_adjacency(c, a, AdjacencyKind::kSharedEnclosure), limits));

  // Two declarations for one pair produce one neighbour, not two: the neighbour list feeds grouping,
  // where a repeated member would inflate an episode.
  const std::vector<EntityId> neighbors = topology.neighbors(a);
  TOBSV_ASSERT_EQ(neighbors.size(), static_cast<std::size_t>(2));
  TOBSV_ASSERT_EQ(neighbors[0].value(), std::string("node-b"));
  TOBSV_ASSERT_EQ(neighbors[1].value(), std::string("node-c"));
  TOBSV_ASSERT_TRUE(topology.neighbors(entity_id("node-zz")).empty());

  // The lookup is symmetric and names the first declared kind for the pair.
  TOBSV_ASSERT_TRUE(topology.adjacency_between(a, b) == AdjacencyKind::kPhysicalContainment);
  TOBSV_ASSERT_TRUE(topology.adjacency_between(b, a) == AdjacencyKind::kPhysicalContainment);
  TOBSV_ASSERT_TRUE(topology.adjacency_between(a, c) == AdjacencyKind::kSharedEnclosure);
  TOBSV_ASSERT_TRUE(topology.adjacency_between(b, c) == AdjacencyKind::kDeclaredUnknown);
  TOBSV_ASSERT_TRUE(to_string(AdjacencyKind::kSharedEnclosure) ==
                    std::string_view("shared_enclosure"));
}
