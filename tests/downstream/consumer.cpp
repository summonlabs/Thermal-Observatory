// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

//
// Independent downstream consumer.
//
// This program is written the way an external project would write it: it includes the installed
// public headers, links ThermalObservatory::thermal_observatory, and exercises the documented
// surface. It is built twice - once inside this repository and once as a standalone project that
// resolves the installed package with find_package(ThermalObservatory CONFIG).

#include <cstdio>
#include <string>
#include <vector>

#include "tobsv/tobsv.hpp"

using namespace tobsv;

namespace {

Fence make_fence(const SourceId& source, std::uint64_t revision) {
  Fence fence;
  fence.source = source;
  fence.epoch = Epoch::from(1);
  fence.generation = Generation::from(1);
  fence.revision = Revision::from(revision);
  fence.incarnation = Incarnation::from(1);
  fence.sequence = Sequence::from(revision);
  fence.attempt = AttemptId::unchecked("att-downstream-" + std::to_string(revision));
  return fence;
}

TemperatureObservation make_observation(const EntityId& entity, double celsius,
                                        const Timestamp& at, std::uint64_t revision) {
  TemperatureObservation observation;
  observation.entity = entity;
  observation.sensor = SensorId::unchecked("sensor-outlet-1");
  observation.site = MeasurementSite::kOutlet;
  observation.celsius = celsius;
  observation.observed_at = at;
  observation.received_at = at;
  observation.provenance.source = SourceId::unchecked("downstream-facility");
  observation.provenance.authority = AuthorityLevel::kMeasured;
  observation.provenance.kind = SourceKind::kFacilitySensor;
  observation.provenance.clock = ClockDomain::kCollectorWallClock;
  observation.provenance.method = "downstream fixture thermometer";
  observation.quality.supplied = true;
  observation.quality.flags.add(QualityFlag::kCalibrated);
  observation.fence = make_fence(observation.provenance.source, revision);
  observation.id = compute_observation_id(observation);
  return observation;
}

}  // namespace

int main() {
  Limits limits;
  if (!limits.validate().ok()) {
    std::printf("downstream: the default limits are invalid\n");
    return 1;
  }

  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.limits = limits;
  const Status opened = observatory.open(config);
  if (!opened.ok()) {
    std::printf("downstream: cannot open the runtime: %s\n", opened.describe().c_str());
    return 1;
  }

  Result<Timestamp> at = Timestamp::parse("2026-02-14T09:31:07Z");
  if (!at.ok()) {
    std::printf("downstream: timestamp rejected\n");
    return 1;
  }

  EntityRecord record;
  record.entity = EntityId::unchecked("node-01");
  record.entity_class = EntityClass::kComputeNode;
  record.zone = ZoneRef::unchecked("zone-a");
  record.site = SiteId::unchecked("hall-1");
  record.label = "Downstream node";
  if (!observatory.register_entity(record).ok()) {
    std::printf("downstream: entity registration failed\n");
    return 1;
  }

  ThermalEnvelope envelope;
  envelope.entity = record.entity;
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
  envelope.declared_by = SourceId::unchecked("downstream-policy");
  envelope.declared_at = at.value();
  envelope.basis = "downstream declared envelope";
  envelope.id = compute_envelope_id(envelope);
  if (!observatory.register_envelope(envelope).ok()) {
    std::printf("downstream: envelope registration failed\n");
    return 1;
  }

  const IngestOutcome ingested =
      observatory.ingest(make_observation(record.entity, 41.5, at.value(), 1));
  if (ingested.kind != IngestKind::kRecorded) {
    std::printf("downstream: ingestion failed: %s\n", ingested.status.describe().c_str());
    return 1;
  }
  const IngestOutcome replayed =
      observatory.ingest(make_observation(record.entity, 41.5, at.value(), 1));
  if (replayed.kind != IngestKind::kDuplicate) {
    std::printf("downstream: replaying an observation was not idempotent\n");
    return 1;
  }

  ThermalQuery query;
  query.evaluated_at = at.value();
  query.freshness = FreshnessPolicy::from_limits(limits);
  query.freshness.window = Duration::from_seconds(600.0).value();
  Result<ThermalAnalysis> analysis = observatory.analyze(query);
  if (!analysis.ok()) {
    std::printf("downstream: analysis failed: %s\n", analysis.error().describe().c_str());
    return 1;
  }
  if (analysis.value().usable_subject_count != 1) {
    std::printf("downstream: expected one usable subject, saw %zu\n",
                analysis.value().usable_subject_count);
    return 1;
  }
  const HeadroomReport& report = analysis.value().headroom.front();
  const HeadroomValue* critical = report.level(ThresholdLevel::kCritical);
  if (critical == nullptr || critical->headroom_c != 3.5) {
    std::printf("downstream: headroom to the critical band is wrong\n");
    return 1;
  }
  if (report.current_level != ThresholdLevel::kHigh) {
    std::printf("downstream: the reported band is not the high band\n");
    return 1;
  }

  // An envelope that declares no ceiling band must be refused rather than defaulted.
  ThermalEnvelope broken = envelope;
  broken.has_maximum = false;
  broken.id = compute_envelope_id(broken);
  if (observatory.register_envelope(broken).ok()) {
    std::printf("downstream: an envelope without a ceiling was accepted\n");
    return 1;
  }

  // Coincidence must never be traversable as propagation.
  CouplingRelation coincidence;
  coincidence.from = record.entity;
  coincidence.to = EntityId::unchecked("node-02");
  coincidence.kind = CouplingKind::kCoincidental;
  coincidence.strength = 0.9;
  coincidence.asserted_by = SourceId::unchecked("downstream-collector");
  coincidence.asserted_at = at.value();
  coincidence.method = "downstream correlation";
  coincidence.fence = make_fence(coincidence.asserted_by, 2);
  CouplingCitation first;
  first.observation = ingested.id;
  CouplingCitation second;
  second.observation = ObservationId::unchecked("obs-0000000000000001");
  coincidence.citations = {first, second};
  coincidence.id = compute_coupling_id(coincidence);
  if (!observatory.record_coupling(coincidence).ok()) {
    std::printf("downstream: a cited coincidence record was refused\n");
    return 1;
  }
  PropagationPolicy policy;
  PropagationResult propagation =
      find_propagation(record.entity, {coincidence.to}, observatory.coupling(), policy, limits).value();
  if (propagation.state == EvidenceState::kFresh) {
    std::printf("downstream: coincidence was traversed as propagation\n");
    return 1;
  }

  Result<std::string> json = analysis.value().to_json().dump_compact();
  if (!json.ok() || json.value().empty()) {
    std::printf("downstream: the analysis could not be encoded\n");
    return 1;
  }

  std::printf("downstream: %s verdict=%s subjects=%zu hotspots=%zu digest=%s\n",
              version_string().c_str(), std::string(to_string(analysis.value().state)).c_str(),
              analysis.value().subject_count, analysis.value().hotspots.episodes.size(),
              analysis.value().digest.c_str());
  return 0;
}
