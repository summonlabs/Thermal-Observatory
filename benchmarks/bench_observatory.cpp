// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

//
// Benchmarks for Thermal Observatory.
//
// Honesty rules applied here:
//   * every measurement is of completed work - a whole analysis that returned;
//   * the workload is labelled SYNTHETIC, because it is generated in this process and says nothing
//     about real facility hardware;
//   * no result is reported for a configuration that was not run;
//   * the durable numbers are REAL: they are device flushes on the machine running the benchmark.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "tobsv/tobsv.hpp"

using namespace tobsv;

namespace {

struct Sample {
  double milliseconds = 0.0;
};

struct Summary {
  double total_ms = 0.0;
  double mean_ms = 0.0;
  double p50_ms = 0.0;
  double p99_ms = 0.0;
  double max_ms = 0.0;
};

Summary summarise(const std::vector<Sample>& samples, double total_ms) {
  Summary summary;
  if (samples.empty()) {
    return summary;
  }
  std::vector<double> values;
  values.reserve(samples.size());
  for (const Sample& sample : samples) {
    values.push_back(sample.milliseconds);
  }
  std::sort(values.begin(), values.end());
  summary.total_ms = total_ms;
  summary.mean_ms = total_ms / static_cast<double>(values.size());
  summary.p50_ms = values[values.size() / 2];
  summary.p99_ms = values[(values.size() * 99) / 100];
  summary.max_ms = values.back();
  return summary;
}

void report(const std::string& name, const std::string& label, std::size_t operations,
            const Summary& summary) {
  std::printf("%-38s %-34s n=%-7zu total=%9.3f ms mean=%8.4f ms p50=%8.4f ms p99=%8.4f ms "
              "max=%8.4f ms\n",
              name.c_str(), label.c_str(), operations, summary.total_ms, summary.mean_ms,
              summary.p50_ms, summary.p99_ms, summary.max_ms);
}

EntityRecord make_entity(const std::string& name) {
  EntityRecord record;
  record.entity = EntityId::unchecked(name);
  record.entity_class = EntityClass::kComputeNode;
  record.site = SiteId::unchecked("hall-1");
  record.label = "benchmark node " + name;
  return record;
}

ThermalEnvelope make_envelope(const EntityId& entity) {
  ThermalEnvelope envelope;
  envelope.entity = entity;
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
  envelope.declared_by = SourceId::unchecked("benchmark-policy");
  envelope.declared_at = Timestamp::from_unix_nanos(1770000000000000000LL);
  envelope.basis = "benchmark envelope";
  envelope.id = compute_envelope_id(envelope);
  return envelope;
}

// A deterministic generator: the benchmark must produce the same workload on every run.
class Generator {
 public:
  explicit Generator(std::uint64_t seed) : state_(seed) {}

  double next_celsius() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    const double unit = static_cast<double>((state_ >> 11) & 0x1FFFFFFFFFFFFFULL) /
                        9007199254740992.0;
    return 22.0 + unit * 26.0;
  }

 private:
  std::uint64_t state_;
};

}  // namespace

int main(int argc, char** argv) {
  std::size_t nodes = 32;
  std::size_t revisions = 8;
  std::size_t analyses = 200;
  std::size_t durable_commits = 200;
  if (argc > 1) nodes = static_cast<std::size_t>(std::strtoul(argv[1], nullptr, 10));
  if (argc > 2) revisions = static_cast<std::size_t>(std::strtoul(argv[2], nullptr, 10));
  if (argc > 3) analyses = static_cast<std::size_t>(std::strtoul(argv[3], nullptr, 10));
  if (argc > 4) durable_commits = static_cast<std::size_t>(std::strtoul(argv[4], nullptr, 10));

  Limits limits;
  limits.max_observations = 1U << 20;
  if (!limits.validate().ok()) {
    std::printf("benchmark limits are invalid\n");
    return 1;
  }

  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.limits = limits;
  if (!observatory.open(config).ok()) {
    std::printf("cannot open the benchmark runtime\n");
    return 1;
  }

  const Timestamp base = Timestamp::from_unix_nanos(1770000000000000000LL);
  Generator generator(0x5EED1234ULL);

  std::vector<Sample> ingest_samples;
  ingest_samples.reserve(nodes * revisions);
  const Nanos ingest_started = MonoClock::now_nanos();
  std::size_t ingested = 0;
  for (std::size_t revision = 1; revision <= revisions; ++revision) {
    for (std::size_t node = 0; node < nodes; ++node) {
      const std::string name = "node-" + std::to_string(node);
      if (revision == 1) {
        if (!observatory.register_entity(make_entity(name)).ok()) {
          return 1;
        }
        if (!observatory.register_envelope(make_envelope(EntityId::unchecked(name))).ok()) {
          return 1;
        }
      }
      TemperatureObservation observation;
      observation.entity = EntityId::unchecked(name);
      observation.sensor = SensorId::unchecked("sensor-outlet-1");
      observation.site = MeasurementSite::kOutlet;
      observation.celsius = generator.next_celsius();
      observation.observed_at = Timestamp::from_unix_nanos(
          base.unix_nanos() + static_cast<UnixNanos>(revision) * 1000000000LL);
      observation.received_at = observation.observed_at;
      observation.provenance.source = SourceId::unchecked("benchmark-source");
      observation.provenance.authority = AuthorityLevel::kMeasured;
      observation.provenance.kind = SourceKind::kFacilitySensor;
      observation.provenance.clock = ClockDomain::kCollectorWallClock;
      observation.provenance.method = "benchmark generator";
      observation.quality.supplied = true;
      observation.quality.flags.add(QualityFlag::kCalibrated);
      observation.fence.source = observation.provenance.source;
      observation.fence.epoch = Epoch::from(1);
      observation.fence.generation = Generation::from(1);
      observation.fence.revision = Revision::from(
          static_cast<std::uint64_t>(revision) * static_cast<std::uint64_t>(nodes) +
          static_cast<std::uint64_t>(node) + 1);
      observation.fence.incarnation = Incarnation::from(1);
      observation.fence.sequence = Sequence::from(observation.fence.revision.value());
      observation.fence.attempt =
          AttemptId::unchecked("att-bench-" + std::to_string(observation.fence.revision.value()));
      observation.id = compute_observation_id(observation);
      const Nanos before = MonoClock::now_nanos();
      const IngestOutcome outcome = observatory.ingest(observation);
      const Nanos after = MonoClock::now_nanos();
      ingest_samples.push_back(Sample{static_cast<double>(after - before) / 1.0e6});
      if (!outcome.accepted()) {
        std::printf("benchmark ingest refused: %s\n", outcome.status.describe().c_str());
        return 1;
      }
      ++ingested;
    }
  }
  const Nanos ingest_finished = MonoClock::now_nanos();
  report("ingest (in memory, single thread)",
         "SYNTHETIC workload, REAL code path", ingested,
         summarise(ingest_samples, static_cast<double>(ingest_finished - ingest_started) / 1.0e6));

  std::vector<Sample> analysis_samples;
  analysis_samples.reserve(analyses);
  std::size_t last_episodes = 0;
  std::size_t last_subjects = 0;
  std::string last_digest;
  const Nanos analysis_started = MonoClock::now_nanos();
  for (std::size_t index = 0; index < analyses; ++index) {
    ThermalQuery query;
    query.evaluated_at = Timestamp::from_unix_nanos(
        base.unix_nanos() + static_cast<UnixNanos>(revisions) * 1000000000LL + 1);
    query.freshness = FreshnessPolicy::from_limits(limits);
    query.hotspots.threshold = ThresholdLevel::kWarn;
    const Nanos before = MonoClock::now_nanos();
    const Result<ThermalAnalysis> analysis = observatory.analyze(query);
    const Nanos after = MonoClock::now_nanos();
    if (!analysis.ok()) {
      std::printf("benchmark analysis failed: %s\n", analysis.error().describe().c_str());
      return 1;
    }
    last_episodes = analysis.value().hotspots.episodes.size();
    last_subjects = analysis.value().subject_count;
    last_digest = analysis.value().digest;
    analysis_samples.push_back(Sample{static_cast<double>(after - before) / 1.0e6});
  }
  const Nanos analysis_finished = MonoClock::now_nanos();
  report("full analysis (single thread)", "SYNTHETIC workload, REAL code path", analyses,
         summarise(analysis_samples,
                   static_cast<double>(analysis_finished - analysis_started) / 1.0e6));
  std::printf("    workload: %zu subjects, %zu hotspot episodes, digest %s\n", last_subjects,
              last_episodes, last_digest.c_str());

  // Durable commits are measured on the real device of this machine.
  std::error_code error;
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "thermal-observatory-benchmark";
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  {
    ThermalObservatory durable;
    ObservatoryConfig durable_config;
    durable_config.limits = limits;
    durable_config.log_path = directory / "bench.log";
    const Status durable_opened = durable.open(durable_config);
    if (!durable_opened.ok()) {
      std::printf("cannot open a durable benchmark store: %s\n",
                  durable_opened.describe().c_str());
      return 1;
    }
    std::vector<Sample> commit_samples;
    commit_samples.reserve(durable_commits);
    std::size_t committed = 0;
    const Nanos commit_started = MonoClock::now_nanos();
    for (std::size_t index = 0; index < durable_commits; ++index) {
      TemperatureObservation observation;
      observation.entity = EntityId::unchecked("node-" + std::to_string(index % nodes));
      observation.sensor = SensorId::unchecked("sensor-outlet-1");
      observation.site = MeasurementSite::kOutlet;
      observation.celsius = generator.next_celsius();
      observation.observed_at = Timestamp::from_unix_nanos(
          base.unix_nanos() + static_cast<UnixNanos>(revisions + 1) * 1000000000LL +
          static_cast<UnixNanos>(index) * 1000LL);
      observation.received_at = observation.observed_at;
      observation.provenance.source = SourceId::unchecked("benchmark-durable-source");
      observation.provenance.authority = AuthorityLevel::kMeasured;
      observation.provenance.kind = SourceKind::kFacilitySensor;
      observation.provenance.clock = ClockDomain::kCollectorWallClock;
      observation.provenance.method = "benchmark generator";
      observation.quality.supplied = true;
      observation.quality.flags.add(QualityFlag::kCalibrated);
      observation.fence.source = observation.provenance.source;
      observation.fence.epoch = Epoch::from(1);
      observation.fence.generation = Generation::from(1);
      observation.fence.revision = Revision::from(static_cast<std::uint64_t>(index) + 1);
      observation.fence.incarnation = Incarnation::from(1);
      observation.fence.sequence = Sequence::from(static_cast<std::uint64_t>(index) + 1);
      observation.fence.attempt =
          AttemptId::unchecked("att-durable-" + std::to_string(index + 1));
      observation.id = compute_observation_id(observation);

      // The measured unit is one complete durable mutation: accept the record and wait for the
      // device to confirm it. An empty barrier would measure nothing.
      const Nanos before = MonoClock::now_nanos();
      if (!durable.ingest(observation).accepted()) {
        std::printf("durable ingest refused\n");
        return 1;
      }
      const Status flushed = durable.flush_durable();
      const Nanos after = MonoClock::now_nanos();
      if (!flushed.ok()) {
        std::printf("durable flush failed: %s\n", flushed.describe().c_str());
        return 1;
      }
      commit_samples.push_back(Sample{static_cast<double>(after - before) / 1.0e6});
      ++committed;
    }
    const Nanos commit_finished = MonoClock::now_nanos();
    report("durable commit (ingest + device flush)",
           "REAL device flush on this machine", committed,
           summarise(commit_samples, static_cast<double>(commit_finished - commit_started) / 1.0e6));
    if (!durable.close().ok()) {
      return 1;
    }
  }
  std::filesystem::remove_all(directory, error);

  std::printf("%s\n", "UNSUPPORTED: no benchmark here measures real facility hardware, a real "
                       "cooling plant, a real BMS or a multi-node deployment, because none of "
                       "those were available. No such number is reported.");
  return 0;
}