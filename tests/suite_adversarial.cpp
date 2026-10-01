// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdint>
#include <filesystem>
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

// Every payload is the same length so that frame offsets can be reasoned about as well as read.
std::string payload_for(std::size_t index) { return "record-" + std::to_string(1000 + index); }

std::string build_log(const std::filesystem::path& path, std::size_t records) {
  const Limits limits;
  ThermalLog log;
  TOBSV_ASSERT_OK(log.open(path, ThermalLog::OpenMode::kCreateNew, 1, limits));
  for (std::size_t index = 0; index < records; ++index) {
    TOBSV_ASSERT_OK(status_of(log.append(payload_for(index), true)));
  }
  TOBSV_ASSERT_OK(log.close());
  const Result<std::string> bytes = read_file_bytes(path, 1U << 20);
  TOBSV_ASSERT_TRUE(bytes.ok());
  return bytes.value();
}

std::size_t frame_payload_length(const std::string& bytes, std::size_t offset) {
  const unsigned char* raw =
      reinterpret_cast<const unsigned char*>(bytes.data()) + offset;
  return static_cast<std::size_t>(raw[0]) | (static_cast<std::size_t>(raw[1]) << 8U) |
         (static_cast<std::size_t>(raw[2]) << 16U) | (static_cast<std::size_t>(raw[3]) << 24U);
}

// Flips one bit inside the file, which is the smallest damage a storage layer can do.
void flip_byte(std::string& bytes, std::size_t offset, unsigned mask) {
  bytes[offset] = static_cast<char>(static_cast<unsigned char>(bytes[offset]) ^ mask);
}

std::string fence_record_text() {
  const Result<JsonValue> wrapped =
      wrap_record(RecordKind::kFence, encode(make_fence(source_id("codec-source"), 1)));
  TOBSV_ASSERT_TRUE(wrapped.ok());
  const Result<std::string> text = wrapped.value().dump_compact();
  TOBSV_ASSERT_TRUE(text.ok());
  return text.value();
}

}  // namespace

TOBSV_TEST("adversarial", "a_torn_tail_recovers_while_interior_corruption_is_refused") {
  ScratchDirectory scratch("tobsv-adversarial-torn");
  Limits limits;
  const std::filesystem::path source = scratch.file("source.log");
  const std::string bytes = build_log(source, 4);
  TOBSV_ASSERT_EQ(bytes.size(), ThermalLog::kHeaderSize +
                                    4 * (ThermalLog::kFrameOverhead + payload_for(0).size()));

  std::size_t offset = ThermalLog::kHeaderSize;
  for (std::size_t index = 0; index < 4; ++index) {
    const std::size_t payload_length = frame_payload_length(bytes, offset);
    const std::size_t cut = offset + ThermalLog::kFrameOverhead + payload_length / 2;
    const std::filesystem::path torn = scratch.file("torn-" + std::to_string(index) + ".log");
    TOBSV_ASSERT_OK(write_file_atomic(torn, std::string_view(bytes).substr(0, cut)));

    // A frame that runs past the end of the file can never have been committed, so everything before
    // it has to survive and the tear has to be reported rather than hidden.
    const Result<ThermalLog::LoadResult> loaded = ThermalLog::inspect(torn, limits);
    TOBSV_ASSERT_TRUE(loaded.ok());
    TOBSV_ASSERT_EQ(loaded.value().payloads.size(), index);
    TOBSV_ASSERT_TRUE(loaded.value().discarded_tail_bytes > 0);
    TOBSV_ASSERT_EQ(loaded.value().committed_bytes, static_cast<std::uint64_t>(offset));

    // Recovery: the tear is truncated away and the next committed record follows the surviving ones
    // with no gap in the sequence.
    ThermalLog reopened;
    TOBSV_ASSERT_OK(reopened.open(torn, ThermalLog::OpenMode::kOpenOrCreate, 2, limits));
    TOBSV_ASSERT_OK(status_of(reopened.append("recovered-record", true)));
    TOBSV_ASSERT_OK(reopened.close());
    const Result<ThermalLog::LoadResult> after = ThermalLog::inspect(torn, limits);
    TOBSV_ASSERT_TRUE(after.ok());
    TOBSV_ASSERT_EQ(after.value().payloads.size(), index + 1);
    TOBSV_ASSERT_EQ(after.value().payloads[index], std::string("recovered-record"));
    TOBSV_ASSERT_EQ(after.value().discarded_tail_bytes, static_cast<std::uint64_t>(0));
    for (std::size_t kept = 0; kept < index; ++kept) {
      TOBSV_ASSERT_EQ(after.value().payloads[kept], payload_for(kept));
    }
    offset += ThermalLog::kFrameOverhead + payload_length;
  }

  // A frame that fits, fails its checksum and has more bytes after it is interior damage: silently
  // discarding the middle of a log would hide exactly what an operator needs to see.
  std::string interior_bytes = bytes;
  flip_byte(interior_bytes, ThermalLog::kHeaderSize + ThermalLog::kFrameOverhead + 2, 0x20U);
  const std::filesystem::path interior = scratch.file("interior.log");
  TOBSV_ASSERT_OK(write_file_atomic(interior, interior_bytes));
  const Result<ThermalLog::LoadResult> refused = ThermalLog::inspect(interior, limits);
  TOBSV_ASSERT_FALSE(refused.ok());
  TOBSV_ASSERT_TRUE(refused.error().code() == ErrorCode::kIntegrityFailure);

  // The very same damage in the final frame is a torn tail, because a partial device write can leave
  // a complete length with a partial body.
  std::string tail_bytes = bytes;
  flip_byte(tail_bytes, bytes.size() - 3, 0x20U);
  const std::filesystem::path tail = scratch.file("tail.log");
  TOBSV_ASSERT_OK(write_file_atomic(tail, tail_bytes));
  const Result<ThermalLog::LoadResult> tail_loaded = ThermalLog::inspect(tail, limits);
  TOBSV_ASSERT_TRUE(tail_loaded.ok());
  TOBSV_ASSERT_EQ(tail_loaded.value().payloads.size(), static_cast<std::size_t>(3));
  TOBSV_ASSERT_TRUE(tail_loaded.value().discarded_tail_bytes > 0);
}

TOBSV_TEST("adversarial", "an_incomplete_header_is_reinitialised_and_a_missing_log_is_not_found") {
  ScratchDirectory scratch("tobsv-adversarial-header");
  Limits limits;
  const std::filesystem::path source = scratch.file("source.log");
  const std::string bytes = build_log(source, 2);

  // A partial header means creation never committed, and no record can hide behind it.
  const std::filesystem::path short_file = scratch.file("short.log");
  TOBSV_ASSERT_OK(write_file_atomic(short_file, std::string_view(bytes).substr(0, 40)));
  const Result<ThermalLog::LoadResult> loaded = ThermalLog::inspect(short_file, limits);
  TOBSV_ASSERT_TRUE(loaded.ok());
  TOBSV_ASSERT_TRUE(loaded.value().reinitialised_short_header);
  TOBSV_ASSERT_TRUE(loaded.value().payloads.empty());
  TOBSV_ASSERT_EQ(loaded.value().file_bytes, static_cast<std::uint64_t>(40));

  ThermalLog log;
  TOBSV_ASSERT_OK(log.open(short_file, ThermalLog::OpenMode::kOpenOrCreate, 5, limits));
  TOBSV_ASSERT_OK(status_of(log.append("after-reinit", true)));
  TOBSV_ASSERT_OK(log.close());
  const Result<ThermalLog::LoadResult> after = ThermalLog::inspect(short_file, limits);
  TOBSV_ASSERT_TRUE(after.ok());
  TOBSV_ASSERT_EQ(after.value().payloads.size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(after.value().payloads[0], std::string("after-reinit"));
  TOBSV_ASSERT_EQ(after.value().writer_epoch, static_cast<std::uint64_t>(5));

  // An empty file was never initialised at all, which is a different fact from a damaged header.
  const std::filesystem::path empty = scratch.file("empty.log");
  TOBSV_ASSERT_OK(write_file_atomic(empty, std::string_view()));
  const Result<ThermalLog::LoadResult> empty_loaded = ThermalLog::inspect(empty, limits);
  TOBSV_ASSERT_TRUE(empty_loaded.ok());
  TOBSV_ASSERT_TRUE(empty_loaded.value().created);
  TOBSV_ASSERT_FALSE(empty_loaded.value().reinitialised_short_header);

  // A missing log is a failure, not an empty log: treating absence as emptiness would let a typo in
  // a path silently start a second history.
  const Result<ThermalLog::LoadResult> missing =
      ThermalLog::inspect(scratch.file("absent.log"), limits);
  TOBSV_ASSERT_FALSE(missing.ok());
  TOBSV_ASSERT_TRUE(missing.error().code() == ErrorCode::kNotFound);
}

TOBSV_TEST("adversarial", "a_wrong_magic_version_or_header_size_is_refused") {
  ScratchDirectory scratch("tobsv-adversarial-magic");
  Limits limits;
  const std::string bytes = build_log(scratch.file("source.log"), 2);

  std::string wrong_magic = bytes;
  wrong_magic[0] = 'X';
  const std::filesystem::path magic_path = scratch.file("magic.log");
  TOBSV_ASSERT_OK(write_file_atomic(magic_path, wrong_magic));
  const Result<ThermalLog::LoadResult> magic = ThermalLog::inspect(magic_path, limits);
  TOBSV_ASSERT_FALSE(magic.ok());
  TOBSV_ASSERT_TRUE(magic.error().code() == ErrorCode::kCorruptRecord);

  // A format version this build does not read is reported as a version mismatch rather than being
  // guessed at, even though the header checksum also no longer matches.
  std::string wrong_version = bytes;
  wrong_version[8] = static_cast<char>(2);
  const std::filesystem::path version_path = scratch.file("version.log");
  TOBSV_ASSERT_OK(write_file_atomic(version_path, wrong_version));
  const Result<ThermalLog::LoadResult> version = ThermalLog::inspect(version_path, limits);
  TOBSV_ASSERT_FALSE(version.ok());
  TOBSV_ASSERT_TRUE(version.error().code() == ErrorCode::kVersionMismatch);

  std::string wrong_size = bytes;
  wrong_size[12] = static_cast<char>(32);
  const std::filesystem::path size_path = scratch.file("size.log");
  TOBSV_ASSERT_OK(write_file_atomic(size_path, wrong_size));
  const Result<ThermalLog::LoadResult> size = ThermalLog::inspect(size_path, limits);
  TOBSV_ASSERT_FALSE(size.ok());
  TOBSV_ASSERT_TRUE(size.error().code() == ErrorCode::kCorruptRecord);

  // The undamaged original still loads, so the refusals above are about the damage.
  const Result<ThermalLog::LoadResult> intact = ThermalLog::inspect(scratch.file("source.log"), limits);
  TOBSV_ASSERT_TRUE(intact.ok());
  TOBSV_ASSERT_EQ(intact.value().payloads.size(), static_cast<std::size_t>(2));
}

TOBSV_TEST("adversarial", "a_header_with_a_bad_checksum_is_refused") {
  ScratchDirectory scratch("tobsv-adversarial-crc");
  Limits limits;
  const std::string bytes = build_log(scratch.file("source.log"), 2);

  // The creation timestamp lives inside the checksummed prefix, so any edit there must be detected.
  std::string bad_prefix = bytes;
  flip_byte(bad_prefix, 24, 0x01U);
  const std::filesystem::path prefix_path = scratch.file("prefix.log");
  TOBSV_ASSERT_OK(write_file_atomic(prefix_path, bad_prefix));
  const Result<ThermalLog::LoadResult> prefix = ThermalLog::inspect(prefix_path, limits);
  TOBSV_ASSERT_FALSE(prefix.ok());
  TOBSV_ASSERT_TRUE(prefix.error().code() == ErrorCode::kIntegrityFailure);

  // Corrupting the stored checksum itself is the same class of failure: consistency cannot be
  // established either way, so the header is not trusted.
  std::string bad_field = bytes;
  flip_byte(bad_field, 48, 0x01U);
  const std::filesystem::path field_path = scratch.file("field.log");
  TOBSV_ASSERT_OK(write_file_atomic(field_path, bad_field));
  const Result<ThermalLog::LoadResult> field = ThermalLog::inspect(field_path, limits);
  TOBSV_ASSERT_FALSE(field.ok());
  TOBSV_ASSERT_TRUE(field.error().code() == ErrorCode::kIntegrityFailure);

  // Opening for writing must refuse the same damage instead of repairing it.
  ThermalLog log;
  const Status opened = log.open(field_path, ThermalLog::OpenMode::kOpenExisting, 1, limits);
  TOBSV_ASSERT_FAILS_WITH(opened, ErrorCode::kIntegrityFailure);
  TOBSV_ASSERT_FALSE(log.is_open());

  // An open that demands an existing log refuses a path that has none.
  ThermalLog fresh;
  TOBSV_ASSERT_FAILS_WITH(fresh.open(scratch.file("absent.log"), ThermalLog::OpenMode::kOpenExisting,
                                     1, limits),
                          ErrorCode::kNotFound);
}

TOBSV_TEST("adversarial", "malformed_record_documents_are_refused_and_never_applied") {
  const Limits limits;
  const std::string valid = fence_record_text();

  const std::string malformed[] = {
      std::string(),
      "not json at all",
      "[1,2,3]",
      "{\"kind\":\"fence\"}",
      "{\"codec\":2,\"kind\":\"fence\",\"payload\":{}}",
      "{\"codec\":1,\"kind\":\"nonsense\",\"payload\":{}}",
      "{\"codec\":1,\"kind\":\"fence\",\"payload\":[]}",
      "{\"codec\":1,\"kind\":\"fence\",\"payload\":{}}",
      "{\"codec\":1,\"kind\":\"observation\",\"payload\":{}}",
      "{\"codec\":1,\"kind\":\"fence\",\"payload\":{\"fence\":{\"source\":\"\","
      "\"epoch\":1,\"generation\":1,\"revision\":1,\"incarnation\":1,\"sequence\":1,"
      "\"attempt\":\"att-1\"}}}",
      "{\"codec\":1,\"kind\":\"fence\",\"payload\":{\"fence\":{\"source\":\"codec-source\","
      "\"epoch\":0,\"generation\":1,\"revision\":1,\"incarnation\":1,\"sequence\":1,"
      "\"attempt\":\"att-1\"}}}",
      "{\"codec\":1,\"kind\":\"fence\",\"payload\":{\"fence\":{\"source\":\"codec-source\","
      "\"epoch\":1,\"generation\":1,\"revision\":1,\"incarnation\":1,\"sequence\":1,"
      "\"attempt\":\"att-1\"}} trailing}",
  };
  for (const std::string& document : malformed) {
    const Result<DecodedRecord> decoded = decode_record(document, limits);
    TOBSV_ASSERT_FALSE(decoded.ok());
  }

  // The well formed record still decodes, so the rejections above are about the defects rather than
  // about a decoder that refuses everything.
  const Result<DecodedRecord> good = decode_record(valid, limits);
  TOBSV_ASSERT_TRUE(good.ok());
  TOBSV_ASSERT_TRUE(good.value().kind == RecordKind::kFence);
  TOBSV_ASSERT_TRUE(good.value().fence.revision.value() == 1);

  // An edit inside a record that was already addressed by content is an integrity failure, and an
  // integrity failure must leave the store untouched rather than applying the edited value.
  SingleNodeFixture fixture;
  JsonValue payload = encode(make_observation(fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                                              41.5, fixture.at, 1));
  payload.set("celsius", JsonValue::real(99.0));
  const Result<JsonValue> wrapped = wrap_record(RecordKind::kObservation, payload);
  TOBSV_ASSERT_TRUE(wrapped.ok());
  const Result<std::string> document = wrapped.value().dump_compact();
  TOBSV_ASSERT_TRUE(document.ok());
  const Result<DecodedRecord> tampered = decode_record(document.value(), fixture.limits);
  TOBSV_ASSERT_FALSE(tampered.ok());
  TOBSV_ASSERT_TRUE(tampered.error().code() == ErrorCode::kIntegrityFailure);
  TOBSV_ASSERT_EQ(fixture.observatory.evidence().observation_count(), static_cast<std::size_t>(0));
}

TOBSV_TEST("adversarial", "hostile_observation_values_are_refused_at_ingest") {
  SingleNodeFixture fixture;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  const double hostile[] = {nan, infinity, -infinity, -273.16, 1000.1, 1.0e9, -1.0e9};
  std::uint64_t revision = 1;
  for (const double value : hostile) {
    const IngestOutcome outcome = fixture.observatory.ingest(make_observation(
        fixture.entity, fixture.sensor, MeasurementSite::kOutlet, value, fixture.at, revision++));
    TOBSV_ASSERT_TRUE(outcome.kind == IngestKind::kRefused);
    TOBSV_ASSERT_FALSE(outcome.accepted());
    TOBSV_ASSERT_TRUE(outcome.status.code() == ErrorCode::kIndeterminate ||
                      outcome.status.code() == ErrorCode::kOutOfRange);
  }
  TOBSV_ASSERT_EQ(fixture.observatory.evidence().observation_count(), static_cast<std::size_t>(0));

  // The two ends of the plausible band are inside it; a hundredth of a degree beyond either is not.
  const double accepted[] = {-273.15, 1000.0};
  for (const double value : accepted) {
    const IngestOutcome outcome = fixture.observatory.ingest(make_observation(
        fixture.entity, fixture.sensor, MeasurementSite::kOutlet, value, fixture.at, revision++));
    TOBSV_ASSERT_TRUE(outcome.kind == IngestKind::kRecorded);
  }

  // An observation that cannot name its subject, its sensor, its site or its time cannot be placed
  // on the timeline, and a value with no provenance is not evidence about anyone.
  // Validation happens before any fence classification, so a record that cannot describe a subject
  // is refused on its own terms whatever slot it claims.
  const auto refuse = [&fixture](const TemperatureObservation& observation, ErrorCode expected) {
    const IngestOutcome outcome = fixture.observatory.ingest(observation);
    TOBSV_ASSERT_TRUE(outcome.kind == IngestKind::kRefused);
    TOBSV_ASSERT_TRUE(outcome.status.code() == expected);
  };
  const TemperatureObservation base = make_observation(
      fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 40.0, fixture.at, 20);
  TemperatureObservation no_entity = base;
  no_entity.entity = EntityId{};
  refuse(no_entity, ErrorCode::kInvalidArgument);
  TemperatureObservation no_sensor = base;
  no_sensor.sensor = SensorId{};
  refuse(no_sensor, ErrorCode::kInvalidArgument);
  TemperatureObservation unknown_site = base;
  unknown_site.site = MeasurementSite::kUnknown;
  refuse(unknown_site, ErrorCode::kUnsupported);
  TemperatureObservation no_time = base;
  no_time.observed_at = Timestamp{};
  refuse(no_time, ErrorCode::kInvalidArgument);
  TemperatureObservation no_source = base;
  no_source.provenance.source = SourceId{};
  no_source.fence.source = SourceId{};
  refuse(no_source, ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_EQ(fixture.observatory.evidence().observation_count(), static_cast<std::size_t>(2));
}

TOBSV_TEST("adversarial", "an_observation_fenced_by_another_source_is_refused") {
  SingleNodeFixture fixture;
  TemperatureObservation mismatched = make_observation(
      fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 41.5, fixture.at, 1,
      source_id("source-a"));
  mismatched.fence.source = source_id("source-b");
  // The identity is recomputed so that the refusal below is about the mismatch and not about a stale
  // content address.
  mismatched.id = compute_observation_id(mismatched);
  const IngestOutcome outcome = fixture.observatory.ingest(mismatched);
  TOBSV_ASSERT_TRUE(outcome.kind == IngestKind::kRefused);
  TOBSV_ASSERT_TRUE(outcome.status.code() == ErrorCode::kConflict);
  TOBSV_ASSERT_FALSE(outcome.accepted());
  TOBSV_ASSERT_TRUE(outcome.status.detail().find("fenced by source") != std::string::npos);
  TOBSV_ASSERT_EQ(fixture.observatory.evidence().observation_count(), static_cast<std::size_t>(0));

  // The same reading with a consistent fence is accepted, so the refusal is about the mismatch.
  const IngestOutcome accepted = fixture.observatory.ingest(make_observation(
      fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 41.5, fixture.at, 1,
      source_id("source-a")));
  TOBSV_ASSERT_TRUE(accepted.kind == IngestKind::kRecorded);
}

TOBSV_TEST("adversarial", "a_forged_observation_identity_is_refused") {
  SingleNodeFixture fixture;
  const TemperatureObservation genuine = make_observation(
      fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 41.5, fixture.at, 1);
  TemperatureObservation forged = genuine;
  forged.id = ObservationId::unchecked("obs-0000000000000000");
  const IngestOutcome first = fixture.observatory.ingest(forged);
  TOBSV_ASSERT_TRUE(first.kind == IngestKind::kConflict);
  TOBSV_ASSERT_TRUE(first.status.code() == ErrorCode::kConflict);
  TOBSV_ASSERT_TRUE(first.status.detail().find("does not match its content") != std::string::npos);

  // A second forgery with a different invented identity is judged the same way: the identity a
  // record claims has to be derivable from the record.
  TemperatureObservation other = genuine;
  other.celsius = 42.5;
  other.id = ObservationId::unchecked("obs-1111111111111111");
  const IngestOutcome second = fixture.observatory.ingest(other);
  TOBSV_ASSERT_TRUE(second.kind == IngestKind::kConflict);
  TOBSV_ASSERT_EQ(fixture.observatory.evidence().conflict_count(), static_cast<std::size_t>(2));
  TOBSV_ASSERT_EQ(fixture.observatory.evidence().observation_count(), static_cast<std::size_t>(0));
}

TOBSV_TEST("adversarial", "coupling_relations_need_citations_and_a_bounded_strength") {
  SingleNodeFixture fixture;
  const Limits limits = fixture.limits;
  const EntityId from = fixture.entity;
  const EntityId to = entity_id("node-02");
  const ObservationId first = record_observation(fixture.observatory, from, fixture.sensor,
                                                 MeasurementSite::kOutlet, 41.0, fixture.at, 1);
  const ObservationId second = record_observation(fixture.observatory, from, fixture.sensor,
                                                  MeasurementSite::kOutlet, 42.0, fixture.at, 2);
  CouplingGraph graph;

  // An uncited relation is not evidence at all.
  const CouplingRelation uncited =
      make_coupling(from, to, CouplingKind::kSupportedThermal, 0.5, {}, fixture.at);
  TOBSV_ASSERT_FAILS_WITH(uncited.validate(limits), ErrorCode::kUnsupported);
  TOBSV_ASSERT_FAILS_WITH(graph.add(uncited, limits), ErrorCode::kUnsupported);

  // One observation cited twice is still one observation: the co-movement of a single sample pair is
  // a coincidence, not a coupling.
  const CouplingRelation single = make_coupling(
      from, to, CouplingKind::kSupportedThermal, 0.5, {cite(first), cite(first)}, fixture.at);
  TOBSV_ASSERT_FAILS_WITH(single.validate(limits), ErrorCode::kUnsupported);

  // A self-citation is a relation from an entity to itself, which no propagation may traverse.
  const CouplingRelation self = make_coupling(
      from, from, CouplingKind::kSupportedThermal, 0.5, {cite(first), cite(second)}, fixture.at);
  TOBSV_ASSERT_FAILS_WITH(self.validate(limits), ErrorCode::kInvalidArgument);

  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  const double strengths[] = {0.0, -0.25, 1.0001, nan, infinity};
  for (const double strength : strengths) {
    const CouplingRelation relation = make_coupling(
        from, to, CouplingKind::kSupportedThermal, strength, {cite(first), cite(second)},
        fixture.at);
    TOBSV_ASSERT_FAILS_WITH(relation.validate(limits), ErrorCode::kOutOfRange);
  }

  // A relation resting on two distinct observations is accepted, and so is one resting on a declared
  // topology when the claim is only adjacency.
  const CouplingRelation supported = make_coupling(
      from, to, CouplingKind::kSupportedThermal, 0.5, {cite(first), cite(second)}, fixture.at);
  TOBSV_ASSERT_OK(supported.validate(limits));
  TOBSV_ASSERT_OK(graph.add(supported, limits));
  TOBSV_ASSERT_EQ(graph.size(), static_cast<std::size_t>(1));

  // Adjacency must cite the topology declaration; observations cannot stand in for it.
  const CouplingRelation adjacency_without_topology = make_coupling(
      from, to, CouplingKind::kPhysicalAdjacency, 0.5, {cite(first), cite(second)}, fixture.at);
  TOBSV_ASSERT_FAILS_WITH(adjacency_without_topology.validate(limits), ErrorCode::kUnsupported);
  const CouplingRelation adjacency = make_coupling(from, to, CouplingKind::kPhysicalAdjacency, 0.5,
                                                   {cite_topology("topo-hall-1")}, fixture.at);
  TOBSV_ASSERT_OK(adjacency.validate(limits));
}

TOBSV_TEST("adversarial", "a_coincidental_relation_is_never_traversed") {
  SingleNodeFixture fixture;
  const EntityId origin = fixture.entity;
  const EntityId middle = entity_id("node-02");
  const EntityId target = entity_id("node-03");
  const ObservationId first = record_observation(fixture.observatory, origin, fixture.sensor,
                                                 MeasurementSite::kOutlet, 41.0, fixture.at, 1);
  const ObservationId second = record_observation(fixture.observatory, middle, fixture.sensor,
                                                  MeasurementSite::kOutlet, 42.0, fixture.at, 2);
  CouplingGraph graph;
  const CouplingRelation coincidental =
      make_coupling(origin, middle, CouplingKind::kCoincidental, 0.9, {cite(first), cite(second)},
                    fixture.at, CouplingDirection::kFromTo);
  TOBSV_ASSERT_OK(graph.add(coincidental, fixture.limits));
  TOBSV_ASSERT_EQ(graph.coincidental_count(), static_cast<std::size_t>(1));
  // The relation is recorded, and it is explicitly not propagation evidence.
  TOBSV_ASSERT_FALSE(coincidental.is_traversable());
  TOBSV_ASSERT_FALSE(coincidental.allows(target, middle));
  TOBSV_ASSERT_TRUE(coincidental.allows(origin, middle));

  PropagationPolicy policy;
  policy.max_depth = 4;
  policy.max_paths = 8;
  const Result<PropagationResult> result =
      find_propagation(origin, {middle, target}, graph, policy, fixture.limits);
  TOBSV_ASSERT_TRUE(result.ok());
  TOBSV_ASSERT_TRUE(result.value().paths.empty());
  TOBSV_ASSERT_TRUE(result.value().coincidental_excluded > 0);
  // With nothing traversable the answer is unsupported evidence, never "fine".
  TOBSV_ASSERT_TRUE(result.value().state == EvidenceState::kUnsupported);
  TOBSV_ASSERT_TRUE(result.value().reason.find("coincidence") != std::string::npos);
}

TOBSV_TEST("adversarial", "envelopes_with_broken_bands_are_refused") {
  const Limits limits;
  EnvelopeRegistry registry;
  const EntityId entity = entity_id("node-env");
  const ThermalEnvelope good = make_envelope(entity, EntityClass::kComputeNode,
                                             MeasurementSite::kOutlet, 20.0, 35.0, 40.0, 45.0, 50.0);
  TOBSV_ASSERT_OK(good.validate(limits));
  TOBSV_ASSERT_OK(registry.add(good, limits));
  TOBSV_ASSERT_FAILS_WITH(registry.add(good, limits), ErrorCode::kDuplicateIdentity);

  // A band colder than the band below it would make the band order meaningless.
  ThermalEnvelope out_of_order = good;
  out_of_order.high_c = 30.0;
  TOBSV_ASSERT_FAILS_WITH(out_of_order.validate(limits), ErrorCode::kInvalidArgument);

  // A ceiling below the nominal band is not an envelope at all.
  ThermalEnvelope inverted = good;
  inverted.maximum_c = 10.0;
  TOBSV_ASSERT_FAILS_WITH(inverted.validate(limits), ErrorCode::kInvalidArgument);

  // Both ends of the band list are required, because they are what a headroom is measured against.
  ThermalEnvelope headless = good;
  headless.has_nominal = false;
  TOBSV_ASSERT_FAILS_WITH(headless.validate(limits), ErrorCode::kInvalidArgument);
  ThermalEnvelope topless = good;
  topless.has_maximum = false;
  TOBSV_ASSERT_FAILS_WITH(topless.validate(limits), ErrorCode::kInvalidArgument);

  // A band outside physical plausibility is not a temperature and would corrupt every aggregate
  // that compared against it.
  ThermalEnvelope implausible = good;
  implausible.maximum_c = 2000.0;
  TOBSV_ASSERT_FAILS_WITH(implausible.validate(limits), ErrorCode::kOutOfRange);

  // An envelope bound to neither an entity nor a class could be applied to anything at all.
  ThermalEnvelope unbound = good;
  unbound.entity = EntityId{};
  unbound.entity_class = EntityClass::kUnknown;
  TOBSV_ASSERT_FAILS_WITH(unbound.validate(limits), ErrorCode::kUnsupported);

  // An identity that does not match the content is refused rather than trusted, so an envelope
  // cannot be swapped underneath a record that cites it.
  ThermalEnvelope forged = good;
  forged.id = EnvelopeId::unchecked("env-0000000000000000");
  TOBSV_ASSERT_FAILS_WITH(registry.add(forged, limits), ErrorCode::kConflict);

  // A class-wide envelope is legal, and the entity-bound envelope still wins the selection.
  const ThermalEnvelope class_wide =
      make_envelope(EntityId{}, EntityClass::kComputeNode, MeasurementSite::kUnknown, 25.0, 30.0,
                    35.0, 40.0, 45.0);
  TOBSV_ASSERT_OK(registry.add(class_wide, limits));
  const ThermalEnvelope* selected =
      registry.select(entity, EntityClass::kComputeNode, MeasurementSite::kOutlet);
  TOBSV_ASSERT_TRUE(selected != nullptr);
  TOBSV_ASSERT_TRUE(selected->id == good.id);
}

TOBSV_TEST("adversarial", "duplicate_and_conflicting_fence_slots_are_refused") {
  SingleNodeFixture fixture;
  const SourceId source = fixture.source;
  FenceTracker tracker(source);
  const Fence first = make_fence(source, 7);
  TOBSV_ASSERT_OK(tracker.observe(first, false));
  // The same slot with the same content is an idempotent retry...
  TOBSV_ASSERT_FAILS_WITH(tracker.observe(first, true), ErrorCode::kDuplicateIdentity);
  // ...and the same slot with different content is a contradiction that must not overwrite the
  // recorded position.
  TOBSV_ASSERT_FAILS_WITH(tracker.observe(first, false), ErrorCode::kConflict);
  TOBSV_ASSERT_TRUE(tracker.high_water().revision.value() == 7);
  TOBSV_ASSERT_OK(tracker.observe(make_fence(source, 9), false));
  TOBSV_ASSERT_FAILS_WITH(tracker.observe(make_fence(source, 8), false), ErrorCode::kReplayRejected);
  TOBSV_ASSERT_TRUE(tracker.high_water().revision.value() == 9);

  // The same rules hold through the store: a re-delivered observation is a duplicate, and a
  // different reading that claims the recorded slot is a conflict.
  const TemperatureObservation recorded = make_observation(
      fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 41.0, fixture.at, 7);
  TOBSV_ASSERT_TRUE(fixture.observatory.ingest(recorded).kind == IngestKind::kRecorded);
  const IngestOutcome duplicate = fixture.observatory.ingest(recorded);
  TOBSV_ASSERT_TRUE(duplicate.kind == IngestKind::kDuplicate);
  TOBSV_ASSERT_FALSE(duplicate.status.failed());
  const IngestOutcome conflicting = fixture.observatory.ingest(make_observation(
      fixture.entity, fixture.sensor, MeasurementSite::kOutlet, 39.0, fixture.at, 7));
  TOBSV_ASSERT_TRUE(conflicting.kind == IngestKind::kConflict);
  TOBSV_ASSERT_TRUE(conflicting.status.code() == ErrorCode::kConflict);
  TOBSV_ASSERT_EQ(fixture.observatory.evidence().observation_count(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(fixture.observatory.evidence().conflict_count(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_EQ(fixture.observatory.evidence().duplicate_count(), static_cast<std::size_t>(1));
}

TOBSV_TEST("adversarial", "oversized_text_and_citation_lists_hit_the_limit_codes") {
  const Limits limits;
  // Text that exceeds the bound is refused rather than truncated, because a truncated reason would
  // silently change what was claimed.
  TOBSV_ASSERT_FAILS_WITH(
      validate_text(std::string(limits.max_text_length + 1, 'x'), "basis", limits.max_text_length),
      ErrorCode::kLimitExceeded);
  TOBSV_ASSERT_OK(validate_text(std::string(limits.max_text_length, 'x'), "basis",
                                limits.max_text_length));

  ThermalInventory inventory;
  EntityRecord record = make_entity("node-long", EntityClass::kComputeNode);
  record.label = std::string(limits.max_text_length + 1, 'x');
  TOBSV_ASSERT_FAILS_WITH(inventory.add(record, limits), ErrorCode::kLimitExceeded);

  // A coupling that cites more observations than a relation may carry fails on the bound, not on
  // the citation contents.
  Limits tight = limits;
  tight.max_citations_per_relation = 2;
  std::vector<CouplingCitation> citations;
  for (std::uint64_t index = 0; index < 3; ++index) {
    citations.push_back(cite(ObservationId::from_digest(index)));
  }
  const CouplingRelation relation = make_coupling(entity_id("a"), entity_id("b"),
                                                  CouplingKind::kSupportedThermal, 0.5, citations,
                                                  base_instant());
  TOBSV_ASSERT_FAILS_WITH(relation.validate(tight), ErrorCode::kLimitExceeded);
  TOBSV_ASSERT_OK(relation.validate(limits));

  // The same bound applies to a derating claim's citation list.
  Limits small = limits;
  small.max_citations = 2;
  std::vector<ObservationId> ids;
  for (std::uint64_t index = 0; index < 3; ++index) {
    ids.push_back(ObservationId::from_digest(index));
  }
  const DeratingEvidence derating = make_derating(entity_id("a"), DeratingKind::kClockThrottle, 0.25,
                                                  ids, ThresholdLevel::kWarn, base_instant());
  TOBSV_ASSERT_FAILS_WITH(derating.validate(small), ErrorCode::kLimitExceeded);
  TOBSV_ASSERT_OK(derating.validate(limits));

  // A derating claim with no citation is refused as unsupported rather than counted as evidence.
  const DeratingEvidence uncited = make_derating(entity_id("a"), DeratingKind::kClockThrottle, 0.25,
                                                 {}, ThresholdLevel::kWarn, base_instant());
  TOBSV_ASSERT_FAILS_WITH(uncited.validate(limits), ErrorCode::kUnsupported);
}
