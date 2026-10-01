// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_support.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace tobstest {

namespace {

std::filesystem::path scratch_root() {
  std::error_code error;
  const std::filesystem::path temp = std::filesystem::temp_directory_path(error);
  if (error) {
    return std::filesystem::path("tobsv-tests");
  }
  return temp / "tobsv-tests";
}

std::atomic<unsigned> g_scratch_counter{0};

}  // namespace

std::string cli_path() {
  static const std::string resolved = [] {
#if defined(_WIN32)
    wchar_t buffer[32768];
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, 32768);
    const std::filesystem::path executable =
        length == 0 ? std::filesystem::path("tobsv_tests.exe")
                    : std::filesystem::path(std::wstring(buffer, buffer + length));
    std::filesystem::path tool = executable.parent_path().parent_path() / "tools" /
                                 "thermal-observatory.exe";
    return tool.string();
#else
    std::error_code error;
    const std::filesystem::path executable = std::filesystem::read_symlink("/proc/self/exe", error);
    const std::filesystem::path base =
        error ? std::filesystem::current_path() : executable.parent_path().parent_path();
    return (base / "tools" / "thermal-observatory").string();
#endif
  }();
  return resolved;
}

CliRun run_cli(const std::string& arguments) {
  static std::atomic<unsigned> run_counter{0};
  CliRun run;
  std::error_code error;
  const std::filesystem::path directory = scratch_root() / "cli-runs";
  std::filesystem::create_directories(directory, error);
  const std::filesystem::path capture =
      directory / ("output-" + std::to_string(run_counter.fetch_add(1)) + ".txt");
  std::filesystem::remove(capture, error);
  const std::string command = "\"\"" + cli_path() + "\" " + arguments +
                              " > \"" + capture.string() + "\" 2>&1\"";
  run.exit_code = std::system(command.c_str());
  const Result<std::string> bytes = read_file_bytes(capture, 8U << 20);
  if (bytes.ok()) {
    run.output = bytes.value();
  }
  std::filesystem::remove(capture, error);
  // Removes the directory too when this was the last capture in it, so a test run leaves nothing
  // behind in the temporary directory.
  std::filesystem::remove(directory, error);
  return run;
}

ScratchDirectory::ScratchDirectory(const std::string& tag) {
  const unsigned index = g_scratch_counter.fetch_add(1);
  path_ = scratch_root() / (tag + "-" + std::to_string(index));
  std::error_code error;
  std::filesystem::remove_all(path_, error);
  std::filesystem::create_directories(path_, error);
}

ScratchDirectory::~ScratchDirectory() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

SourceId source_id(const std::string& name) { return SourceId::unchecked(name); }
EntityId entity_id(const std::string& name) { return EntityId::unchecked(name); }
SensorId sensor_id(const std::string& name) { return SensorId::unchecked(name); }

Fence make_fence(const SourceId& source, std::uint64_t revision, std::uint64_t epoch,
                 std::uint64_t generation, std::uint64_t sequence, const std::string& attempt) {
  Fence fence;
  fence.source = source;
  fence.epoch = Epoch::from(epoch);
  fence.generation = Generation::from(generation);
  fence.revision = Revision::from(revision);
  fence.incarnation = Incarnation::from(1);
  fence.sequence = Sequence::from(sequence == 0 ? revision : sequence);
  fence.attempt =
      AttemptId::unchecked(attempt.empty() ? "att-" + std::to_string(revision) : attempt);
  return fence;
}

Timestamp instant(const std::string& rfc3339) {
  const Result<Timestamp> parsed = Timestamp::parse(rfc3339);
  if (!parsed.ok()) {
    std::fprintf(stderr, "fixture timestamp rejected: %s\n", parsed.error().describe().c_str());
    return Timestamp{};
  }
  return parsed.value();
}

Timestamp base_instant() { return instant("2026-02-14T09:31:07Z"); }

TemperatureObservation make_observation(const EntityId& entity, const SensorId& sensor,
                                        MeasurementSite site, double celsius,
                                        const Timestamp& observed_at, std::uint64_t revision,
                                        const SourceId& source, AuthorityLevel authority,
                                        SourceKind kind, ClockDomain clock,
                                        bool quality_supplied) {
  TemperatureObservation observation;
  observation.entity = entity;
  observation.sensor = sensor;
  observation.site = site;
  observation.celsius = celsius;
  observation.observed_at = observed_at;
  observation.received_at = observed_at;
  observation.provenance.source = source.is_set() ? source : source_id("fixture-source");
  observation.provenance.authority = authority;
  observation.provenance.kind = kind;
  observation.provenance.clock = clock;
  observation.provenance.method = "test fixture reading";
  observation.quality.supplied = quality_supplied;
  if (quality_supplied) {
    observation.quality.flags.add(QualityFlag::kCalibrated);
  }
  observation.fence = make_fence(observation.provenance.source, revision);
  observation.id = compute_observation_id(observation);
  return observation;
}

EntityRecord make_entity(const std::string& name, EntityClass entity_class, const std::string& zone,
                         const std::string& label) {
  EntityRecord record;
  record.entity = entity_id(name);
  record.entity_class = entity_class;
  if (!zone.empty()) {
    record.zone = ZoneRef::unchecked(zone);
  }
  record.site = SiteId::unchecked("hall-1");
  record.label = label.empty() ? ("fixture entity " + name) : label;
  return record;
}

ThermalEnvelope make_envelope(const EntityId& entity, EntityClass entity_class,
                              MeasurementSite site, double nominal, double warn, double high,
                              double critical, double maximum) {
  ThermalEnvelope envelope;
  envelope.entity = entity;
  envelope.entity_class = entity_class;
  envelope.site = site;
  envelope.nominal_c = nominal;
  envelope.has_warn = true;
  envelope.warn_c = warn;
  envelope.has_high = true;
  envelope.high_c = high;
  envelope.has_critical = true;
  envelope.critical_c = critical;
  envelope.maximum_c = maximum;
  envelope.declared_by = source_id("fixture-policy");
  envelope.declared_at = base_instant();
  envelope.basis = "test fixture envelope";
  envelope.id = compute_envelope_id(envelope);
  return envelope;
}

TopologyEdge make_adjacency(const EntityId& from, const EntityId& to, AdjacencyKind kind,
                            const std::string& topology) {
  TopologyEdge edge;
  edge.from = from;
  edge.to = to;
  edge.kind = kind;
  edge.topology = TopologyRef::unchecked(topology);
  edge.declared_by = source_id("fixture-topology");
  return edge;
}

CouplingCitation cite(const ObservationId& id) {
  CouplingCitation citation;
  citation.is_topology = false;
  citation.observation = id;
  return citation;
}

CouplingCitation cite_topology(const std::string& topology) {
  CouplingCitation citation;
  citation.is_topology = true;
  citation.topology = TopologyRef::unchecked(topology);
  return citation;
}

CouplingRelation make_coupling(const EntityId& from, const EntityId& to, CouplingKind kind,
                               double strength, const std::vector<CouplingCitation>& citations,
                               const Timestamp& asserted_at, CouplingDirection direction) {
  CouplingRelation relation;
  relation.from = from;
  relation.to = to;
  relation.kind = kind;
  relation.direction = direction;
  relation.strength = strength;
  relation.citations = citations;
  relation.asserted_by = source_id("fixture-coupling");
  relation.asserted_at = asserted_at;
  relation.method = "test fixture coupling";
  relation.fence = make_fence(relation.asserted_by, 1);
  relation.id = compute_coupling_id(relation);
  return relation;
}

DeratingEvidence make_derating(const EntityId& entity, DeratingKind kind, double magnitude,
                               const std::vector<ObservationId>& citations,
                               ThresholdLevel trigger, const Timestamp& observed_at) {
  DeratingEvidence evidence;
  evidence.entity = entity;
  evidence.kind = kind;
  evidence.magnitude = magnitude;
  evidence.citations = citations;
  evidence.trigger_level = trigger;
  evidence.observed_at = observed_at;
  evidence.asserted_at = observed_at;
  evidence.asserted_by = source_id("fixture-derating");
  evidence.source_kind = SourceKind::kPlatformAgent;
  evidence.method = "test fixture derating claim";
  evidence.fence = make_fence(evidence.asserted_by, 1);
  evidence.id = compute_derating_id(evidence);
  return evidence;
}

ObservationId record_observation(ThermalObservatory& observatory, const EntityId& entity,
                                 const SensorId& sensor, MeasurementSite site, double celsius,
                                 const Timestamp& observed_at, std::uint64_t revision,
                                 const SourceId& source) {
  const SourceId actual = source.is_set() ? source : source_id("fixture-source");
  const TemperatureObservation observation = make_observation(
      entity, sensor, site, celsius, observed_at, revision, actual);
  const Status status = observatory.ingest(observation).status;
  if (status.failed()) {
    std::fprintf(stderr, "fixture ingest failed: %s\n", status.describe().c_str());
  }
  return observation.id;
}

SingleNodeFixture::SingleNodeFixture() {
  limits.default_freshness_window = 600LL * 1000 * 1000 * 1000;
  at = base_instant();
  entity = entity_id("node-01");
  source = source_id("fixture-source");
  sensor = sensor_id("sensor-outlet-1");

  ObservatoryConfig config;
  config.limits = limits;
  const Status opened = observatory.open(config);
  if (!opened.ok()) {
    std::fprintf(stderr, "fixture open failed: %s\n", opened.describe().c_str());
  }
  const Status entity_status = observatory.register_entity(make_entity("node-01", EntityClass::kComputeNode));
  if (entity_status.failed()) {
    std::fprintf(stderr, "fixture entity failed: %s\n", entity_status.describe().c_str());
  }
  envelope = make_envelope(entity, EntityClass::kComputeNode, MeasurementSite::kOutlet, 20.0, 35.0,
                           40.0, 45.0, 50.0);
  const Status envelope_status = observatory.register_envelope(envelope);
  if (envelope_status.failed()) {
    std::fprintf(stderr, "fixture envelope failed: %s\n", envelope_status.describe().c_str());
  }
}

}  // namespace tobstest