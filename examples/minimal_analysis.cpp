// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

//
// The smallest complete story: one observed entity, one declared envelope, one analysis.
//
// Every value here is SYNTHETIC. Nothing in this example came from real hardware.

#include <cstdio>

#include "tobsv/tobsv.hpp"

using namespace tobsv;

int main() {
  ThermalObservatory observatory;
  ObservatoryConfig config;
  const Status opened = observatory.open(config);
  if (!opened.ok()) {
    std::printf("cannot open the runtime: %s\n", opened.describe().c_str());
    return 1;
  }

  const Timestamp at = Timestamp::parse("2026-02-14T09:31:07Z").value();
  const EntityId node = EntityId::unchecked("node-01");

  EntityRecord record;
  record.entity = node;
  record.entity_class = EntityClass::kComputeNode;
  record.zone = ZoneRef::unchecked("zone-a");  // a reference to another authority's zone, not ours
  record.site = SiteId::unchecked("hall-1");
  record.label = "Example node";
  if (!observatory.register_entity(record).ok()) {
    std::printf("entity registration failed\n");
    return 1;
  }

  ThermalEnvelope envelope;
  envelope.entity = node;
  envelope.entity_class = EntityClass::kComputeNode;
  envelope.site = MeasurementSite::kOutlet;
  envelope.nominal_c = 20.0;
  envelope.has_warn = true;
  envelope.warn_c = 35.0;
  envelope.has_high = true;
  envelope.high_c = 40.0;
  envelope.has_critical = true;
  envelope.critical_c = 45.0;
  envelope.maximum_c = 50.0;
  envelope.declared_by = SourceId::unchecked("facility-policy");
  envelope.declared_at = at;
  envelope.basis = "declared by the facility for this example";
  envelope.id = compute_envelope_id(envelope);
  if (!observatory.register_envelope(envelope).ok()) {
    std::printf("envelope registration failed\n");
    return 1;
  }

  TemperatureObservation observation;
  observation.entity = node;
  observation.sensor = SensorId::unchecked("sensor-outlet-4");
  observation.site = MeasurementSite::kOutlet;
  observation.celsius = 41.5;
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
  observation.fence.revision = Revision::from(1);
  observation.fence.incarnation = Incarnation::from(1);
  observation.fence.sequence = Sequence::from(1);
  observation.fence.attempt = AttemptId::unchecked("att-1");
  observation.id = compute_observation_id(observation);

  const IngestOutcome outcome = observatory.ingest(observation);
  if (!outcome.accepted()) {
    std::printf("ingest refused: %s\n", outcome.status.describe().c_str());
    return 1;
  }

  ThermalQuery query;
  query.evaluated_at = at;
  query.freshness = FreshnessPolicy::from_limits(observatory.limits());
  const Result<ThermalAnalysis> analysis = observatory.analyze(query);
  if (!analysis.ok()) {
    std::printf("analysis failed: %s\n", analysis.error().describe().c_str());
    return 1;
  }

  std::printf("%s\n", analysis.value().to_json().dump_indented().value().c_str());
  for (const std::string& step : analysis.value().reason_steps) {
    std::printf("reason: %s\n", step.c_str());
  }
  return 0;
}
