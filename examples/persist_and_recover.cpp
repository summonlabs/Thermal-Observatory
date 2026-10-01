// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

//
// Shows the durable path end to end: record, commit, close, reopen, and observe that recovered
// evidence is evidence about the past rather than a current reading.
//
// The durable behaviour here is REAL: a real file, a real device flush, a real reopen and a real
// kernel single-writer lock. The temperatures are SYNTHETIC.

#include <cstdio>
#include <filesystem>

#include "tobsv/tobsv.hpp"

using namespace tobsv;

namespace {

ThermalQuery query_at(const Timestamp& at, const Limits& limits) {
  ThermalQuery query;
  query.evaluated_at = at;
  query.freshness = FreshnessPolicy::from_limits(limits);
  return query;
}

TemperatureObservation make_reading(const EntityId& node, const Timestamp& at,
                                    std::uint64_t revision) {
  TemperatureObservation observation;
  observation.entity = node;
  observation.sensor = SensorId::unchecked("sensor-outlet-1");
  observation.site = MeasurementSite::kOutlet;
  observation.celsius = 40.0 + static_cast<double>(revision);
  observation.observed_at = at;
  observation.received_at = at;
  observation.provenance.source = SourceId::unchecked("example-facility-telemetry");
  observation.provenance.authority = AuthorityLevel::kMeasured;
  observation.provenance.kind = SourceKind::kFacilitySensor;
  observation.provenance.clock = ClockDomain::kCollectorWallClock;
  observation.provenance.method = "example synthetic reading";
  observation.quality.supplied = true;
  observation.quality.flags.add(QualityFlag::kCalibrated);
  observation.fence.source = observation.provenance.source;
  observation.fence.epoch = Epoch::from(1);
  observation.fence.generation = Generation::from(1);
  observation.fence.revision = Revision::from(revision);
  observation.fence.incarnation = Incarnation::from(1);
  observation.fence.sequence = Sequence::from(revision);
  observation.fence.attempt = AttemptId::unchecked("att-" + std::to_string(revision));
  observation.id = compute_observation_id(observation);
  return observation;
}

}  // namespace

int main() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "thermal-observatory-example";
  const std::filesystem::path log_path = directory / "store.log";
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);

  const Timestamp at = Timestamp::parse("2026-02-14T09:31:07Z").value();
  const EntityId node = EntityId::unchecked("node-11");

  {
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = log_path;
    const Status opened = observatory.open(config);
    if (!opened.ok()) {
      std::printf("cannot open a durable store at %s: %s\n", log_path.string().c_str(),
                  opened.describe().c_str());
      return 1;
    }
    EntityRecord entity;
    entity.entity = node;
    entity.entity_class = EntityClass::kComputeNode;
    entity.site = SiteId::unchecked("hall-1");
    entity.label = "Example durable node";
    if (!observatory.register_entity(entity).ok()) {
      return 1;
    }
    // Readings are a minute apart, so they are successive observations of one subject rather than
    // three contradictory readings of the same instant.
    for (std::uint64_t revision = 1; revision <= 3; ++revision) {
      const Timestamp moment =
          Timestamp::from_unix_nanos(at.unix_nanos() +
                                     static_cast<UnixNanos>(revision - 1) * 60000000000LL);
      if (!observatory.ingest(make_reading(node, moment, revision)).accepted()) {
        return 1;
      }
    }
    const Status flushed = observatory.flush_durable();
    std::printf("commit point reached: %s (commits settled %llu)\n", flushed.describe().c_str(),
                static_cast<unsigned long long>(observatory.recovery().commits_settled));
    if (!observatory.close().ok()) {
      return 1;
    }
  }

  {
    // A second process would be refused here with a lock error; this example simply reopens.
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = log_path;
    config.writer_epoch = 2;
    const Status reopened = observatory.open(config);
    if (!reopened.ok()) {
      std::printf("cannot reopen the durable store: %s\n", reopened.describe().c_str());
      return 1;
    }
    std::printf("recovered %llu record(s), %llu observation(s), %llu discarded tail byte(s)\n",
                static_cast<unsigned long long>(observatory.recovery().records_loaded),
                static_cast<unsigned long long>(observatory.recovery().observations_replayed),
                static_cast<unsigned long long>(observatory.recovery().discarded_tail_bytes));

    const Result<ThermalAnalysis> now = observatory.analyze(query_at(at, observatory.limits()));
    std::printf("at the recording instant: state=%s usable=%zu\n",
                std::string(to_string(now.value().state)).c_str(), now.value().usable_subject_count);

    const Timestamp later =
        Timestamp::from_unix_nanos(at.unix_nanos() + 86400LL * 1000 * 1000 * 1000);
    const Result<ThermalAnalysis> tomorrow =
        observatory.analyze(query_at(later, observatory.limits()));
    std::printf("a day later: state=%s usable=%zu (recovered evidence is not current)\n",
                std::string(to_string(tomorrow.value().state)).c_str(),
                tomorrow.value().usable_subject_count);
    if (!observatory.close().ok()) {
      return 1;
    }
  }

  std::filesystem::remove_all(directory, error);
  return 0;
}
