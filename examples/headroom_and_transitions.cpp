// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

//
// Shows how headroom is reported band by band and how a threshold transition is derived from two
// successive evaluations. All values are SYNTHETIC.
//
// The key point: a missing band is reported as missing, never as unlimited headroom.

#include <cstdio>
#include <vector>

#include "tobsv/tobsv.hpp"

using namespace tobsv;

namespace {

double celsius_at(double base, int step) { return base + static_cast<double>(step) * 1.5; }

}  // namespace

int main() {
  ThermalObservatory observatory;
  ObservatoryConfig config;
  if (!observatory.open(config).ok()) {
    return 1;
  }

  const Timestamp start = Timestamp::parse("2026-02-14T09:00:00Z").value();
  const EntityId node = EntityId::unchecked("node-07");

  EntityRecord record;
  record.entity = node;
  record.entity_class = EntityClass::kComputeNode;
  record.site = SiteId::unchecked("hall-2");
  record.label = "Example node with a sparse envelope";
  if (!observatory.register_entity(record).ok()) {
    return 1;
  }

  // An envelope that declares only a nominal band and a ceiling: the intermediate bands are absent
  // on purpose, so the report has to say so.
  ThermalEnvelope envelope;
  envelope.entity = node;
  envelope.entity_class = EntityClass::kComputeNode;
  envelope.site = MeasurementSite::kOutlet;
  envelope.nominal_c = 22.0;
  envelope.maximum_c = 48.0;
  envelope.declared_by = SourceId::unchecked("facility-policy");
  envelope.declared_at = start;
  envelope.basis = "sparse example envelope";
  envelope.id = compute_envelope_id(envelope);
  if (!observatory.register_envelope(envelope).ok()) {
    return 1;
  }

  for (int step = 0; step < 8; ++step) {
    const Timestamp at = Timestamp::from_unix_nanos(start.unix_nanos() +
                                                    static_cast<UnixNanos>(step) * 60000000000LL);
    const double celsius = celsius_at(22.0, step);
    TemperatureObservation observation;
    observation.entity = node;
    observation.sensor = SensorId::unchecked("sensor-outlet-7");
    observation.site = MeasurementSite::kOutlet;
    observation.celsius = celsius;
    observation.observed_at = at;
    observation.received_at = at;
    observation.provenance.source = SourceId::unchecked("example-facility-telemetry");
    observation.provenance.authority = AuthorityLevel::kMeasured;
    observation.provenance.kind = SourceKind::kFacilitySensor;
    observation.provenance.clock = ClockDomain::kCollectorWallClock;
    observation.provenance.method = "example synthetic ramp";
    observation.quality.supplied = true;
    observation.quality.flags.add(QualityFlag::kCalibrated);
    observation.fence.source = observation.provenance.source;
    observation.fence.epoch = Epoch::from(1);
    observation.fence.generation = Generation::from(1);
    observation.fence.revision = Revision::from(static_cast<std::uint64_t>(step + 1));
    observation.fence.incarnation = Incarnation::from(1);
    observation.fence.sequence = Sequence::from(static_cast<std::uint64_t>(step + 1));
    observation.fence.attempt = AttemptId::unchecked("att-" + std::to_string(step + 1));
    observation.id = compute_observation_id(observation);
    if (!observatory.ingest(observation).accepted()) {
      return 1;
    }

    ThermalQuery query;
    query.evaluated_at = at;
    query.freshness = FreshnessPolicy::from_limits(observatory.limits());
    query.hotspots.threshold = ThresholdLevel::kWarn;
    const Result<ThermalAnalysis> analysis = observatory.analyze(query);
    if (!analysis.ok()) {
      return 1;
    }
    const HeadroomReport& report = analysis.value().headroom.front();
    const HeadroomValue* ceiling = report.level(ThresholdLevel::kMaximum);
    const ThresholdTransitionRecord& transition = analysis.value().transitions.front();
    std::printf("step %d: %.1f C -> band %s (transition %s), headroom to the ceiling is %s\n", step,
                celsius, std::string(to_string(report.current_level)).c_str(),
                std::string(to_string(transition.transition)).c_str(),
                ceiling == nullptr ? "not declared" : std::to_string(ceiling->headroom_c).c_str());
    for (const ThresholdLevel missing : report.missing_levels) {
      std::printf("    band %s is not declared by the envelope\n",
                  std::string(to_string(missing)).c_str());
    }
  }
  return 0;
}
