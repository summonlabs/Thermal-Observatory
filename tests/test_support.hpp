// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

#include "tobsv/tobsv.hpp"

namespace tobsv {

// Streaming operators for the vocabulary, so that an assertion failure prints a readable value
// rather than failing to compile. They live in the product namespace so that argument-dependent
// lookup finds them from any test.
inline std::ostream& operator<<(std::ostream& out, ErrorCode value) {
  return out << std::string(to_string(value));
}
inline std::ostream& operator<<(std::ostream& out, const Status& value) {
  return out << value.describe();
}
inline std::ostream& operator<<(std::ostream& out, EvidenceState value) {
  return out << std::string(to_string(value));
}
inline std::ostream& operator<<(std::ostream& out, MeasurementSite value) {
  return out << std::string(to_string(value));
}
inline std::ostream& operator<<(std::ostream& out, EntityClass value) {
  return out << std::string(to_string(value));
}
inline std::ostream& operator<<(std::ostream& out, ThresholdLevel value) {
  return out << std::string(to_string(value));
}
inline std::ostream& operator<<(std::ostream& out, IngestKind value) {
  return out << std::string(to_string(value));
}
inline std::ostream& operator<<(std::ostream& out, AuthorityLevel value) {
  return out << std::string(to_string(value));
}
inline std::ostream& operator<<(std::ostream& out, SourceKind value) {
  return out << std::string(to_string(value));
}
inline std::ostream& operator<<(std::ostream& out, ClockDomain value) {
  return out << std::string(to_string(value));
}
inline std::ostream& operator<<(std::ostream& out, CouplingKind value) {
  return out << std::string(to_string(value));
}
inline std::ostream& operator<<(std::ostream& out, DeratingKind value) {
  return out << std::string(to_string(value));
}
inline std::ostream& operator<<(std::ostream& out, AttributionLimitCode value) {
  return out << std::string(to_string(value));
}
inline std::ostream& operator<<(std::ostream& out, ThresholdTransition value) {
  return out << std::string(to_string(value));
}
inline std::ostream& operator<<(std::ostream& out, AdjacencyKind value) {
  return out << std::string(to_string(value));
}
inline std::ostream& operator<<(std::ostream& out, RecordKind value) {
  return out << std::string(to_string(value));
}
inline std::ostream& operator<<(std::ostream& out, FenceVerdict value) {
  return out << std::string(to_string(value));
}
template <class Tag>
inline std::ostream& operator<<(std::ostream& out, const StrongId<Tag>& value) {
  return out << value.value();
}

}  // namespace tobsv

namespace tobstest {

using namespace tobsv;

// A scratch directory that removes itself. Used for real filesystem and real multiprocess proofs.
class ScratchDirectory {
 public:
  explicit ScratchDirectory(const std::string& tag);
  ~ScratchDirectory();

  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;

  const std::filesystem::path& path() const noexcept { return path_; }
  std::filesystem::path file(const std::string& name) const { return path_ / name; }

 private:
  std::filesystem::path path_;
};

// Runs the installed command line tool through a real child process and captures its output.
//
// The whole command is wrapped in one extra pair of quotes because cmd.exe strips the outermost
// quotes of a command line that begins with a quoted program path, and the build tree can contain
// spaces. Getting this wrong makes the child fail for a reason that has nothing to do with what is
// under test.
struct CliRun {
  int exit_code = -1;
  std::string output;
};

// Absolute path of the thermal-observatory tool, derived from the location of the running test
// executable. Resolving it at run time rather than baking it into a compile definition is
// deliberate: a Windows path in a macro is a string literal full of backslash escapes, and a build
// tree may also contain spaces.
std::string cli_path();

CliRun run_cli(const std::string& arguments);

// Existing call sites spell the tool path as a macro; it now expands to the run-time lookup.
#ifndef THERMAL_CLI_PATH
#define THERMAL_CLI_PATH (::tobstest::cli_path())
#endif

// Deterministic identity and timing helpers for fixtures.
SourceId source_id(const std::string& name);
EntityId entity_id(const std::string& name);
SensorId sensor_id(const std::string& name);

Fence make_fence(const SourceId& source, std::uint64_t revision, std::uint64_t epoch = 1,
                 std::uint64_t generation = 1, std::uint64_t sequence = 0,
                 const std::string& attempt = "");

Timestamp instant(const std::string& rfc3339);
// Base instant used by fixtures: 2026-02-14T09:31:07Z.
Timestamp base_instant();

TemperatureObservation make_observation(const EntityId& entity, const SensorId& sensor,
                                        MeasurementSite site, double celsius,
                                        const Timestamp& observed_at, std::uint64_t revision,
                                        const SourceId& source = SourceId{},
                                        AuthorityLevel authority = AuthorityLevel::kMeasured,
                                        SourceKind kind = SourceKind::kFacilitySensor,
                                        ClockDomain clock = ClockDomain::kCollectorWallClock,
                                        bool quality_supplied = true);

EntityRecord make_entity(const std::string& name, EntityClass entity_class,
                         const std::string& zone = "", const std::string& label = "");

ThermalEnvelope make_envelope(const EntityId& entity, EntityClass entity_class,
                              MeasurementSite site, double nominal, double warn, double high,
                              double critical, double maximum);

TopologyEdge make_adjacency(const EntityId& from, const EntityId& to, AdjacencyKind kind,
                            const std::string& topology = "topo-hall-1");

CouplingRelation make_coupling(const EntityId& from, const EntityId& to, CouplingKind kind,
                               double strength, const std::vector<CouplingCitation>& citations,
                               const Timestamp& asserted_at,
                               CouplingDirection direction = CouplingDirection::kSymmetric);

CouplingCitation cite(const ObservationId& id);
CouplingCitation cite_topology(const std::string& topology);

DeratingEvidence make_derating(const EntityId& entity, DeratingKind kind, double magnitude,
                               const std::vector<ObservationId>& citations,
                               ThresholdLevel trigger, const Timestamp& observed_at);

// Adds one fresh observation of a subject at the given revision and returns its identity.
ObservationId record_observation(ThermalObservatory& observatory, const EntityId& entity,
                                 const SensorId& sensor, MeasurementSite site, double celsius,
                                 const Timestamp& observed_at, std::uint64_t revision,
                                 const SourceId& source = SourceId{});

// A ready-made single-node fixture: one entity, one envelope and one fresh observation.
struct SingleNodeFixture {
  Limits limits;
  ThermalObservatory observatory;
  Timestamp at;
  EntityId entity;
  SourceId source;
  SensorId sensor;
  ThermalEnvelope envelope;

  SingleNodeFixture();
};

}  // namespace tobstest
