// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <clocale>
#include <fstream>
#include <thread>

using namespace tobsv;
using namespace tobstest;

TOBSV_TEST("hardening", "a_tampered_stored_observation_fails_its_content_check") {
  Limits limits;
  const TemperatureObservation observation = make_observation(
      entity_id("node-01"), sensor_id("s"), MeasurementSite::kOutlet, 41.5, base_instant(), 1);
  const Result<JsonValue> wrapped = wrap_record(RecordKind::kObservation, encode(observation));
  TOBSV_ASSERT_OK(wrapped);
  const Result<std::string> document = wrapped.value().dump_compact();
  TOBSV_ASSERT_OK(document);
  TOBSV_ASSERT_OK(decode_record(document.value(), limits));

  const std::string tampered =
      document.value().substr(0, document.value().find("41.5")) + "41.6" +
      document.value().substr(document.value().find("41.5") + 4);
  TOBSV_ASSERT_TRUE(tampered != document.value());
  TOBSV_ASSERT_FAILS_WITH(decode_record(tampered, limits), ErrorCode::kIntegrityFailure);
}

TOBSV_TEST("hardening", "a_deeply_nested_record_is_refused_by_the_depth_budget") {
  Limits limits;
  limits.max_json_depth = 8;
  std::string document = "{\"codec\":1,\"kind\":\"entity\",\"payload\":{\"entity\":";
  for (int depth = 0; depth < 20; ++depth) {
    document += "{\"a\":";
  }
  document += "1";
  for (int depth = 0; depth < 20; ++depth) {
    document += "}";
  }
  document += "}}";
  TOBSV_ASSERT_FAILS_WITH(decode_record(document, limits), ErrorCode::kLimitExceeded);
}

TOBSV_TEST("hardening", "control_characters_cannot_enter_a_durable_record") {
  Limits limits;
  EntityRecord record = make_entity("node-01", EntityClass::kComputeNode);
  record.label = "node with a" + std::string(1, static_cast<char>(0x07)) + "bell";
  TOBSV_ASSERT_FAILS_WITH(record.validate(limits), ErrorCode::kInvalidArgument);

  EntityRecord empty = make_entity("node-01", EntityClass::kComputeNode);
  empty.label.clear();
  TOBSV_ASSERT_FAILS_WITH(empty.validate(limits), ErrorCode::kInvalidArgument);

  ThermalEnvelope envelope = make_envelope(entity_id("node-01"), EntityClass::kComputeNode,
                                           MeasurementSite::kOutlet, 20.0, 35.0, 40.0, 45.0, 50.0);
  envelope.basis.clear();
  TOBSV_ASSERT_FAILS_WITH(envelope.validate(limits), ErrorCode::kInvalidArgument);
}

TOBSV_TEST("hardening", "an_over_long_identity_is_refused_before_it_reaches_storage") {
  const std::string too_long(kMaxIdentifierLength + 1, 'a');
  TOBSV_ASSERT_FAILS_WITH(EntityId::parse(too_long), ErrorCode::kLimitExceeded);
  TOBSV_ASSERT_FAILS_WITH(EntityId::parse(""), ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_FAILS_WITH(EntityId::parse("-leading-dash"), ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_FAILS_WITH(EntityId::parse("has space"), ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_FAILS_WITH(EntityId::parse(std::string("bad") + static_cast<char>(0x01)),
                          ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_OK(EntityId::parse("node-01.rack:3/a+b#c"));
}

TOBSV_TEST("hardening", "a_derived_identity_is_never_a_parsed_user_identity") {
  const ObservationId derived = ObservationId::from_digest(0x0123456789ABCDEFULL);
  TOBSV_ASSERT_EQ(derived.value(), std::string("obs-0123456789abcdef"));
  TOBSV_ASSERT_TRUE(derived.value().size() <= kMaxIdentifierLength);
  TOBSV_ASSERT_OK(ObservationId::parse(derived.value()));
  const std::string derived_entity = EntityId::from_digest(1).value();
  TOBSV_ASSERT_EQ(derived_entity, std::string("ent-0000000000000001"));
}

TOBSV_TEST("hardening", "a_log_path_that_is_a_directory_fails_cleanly") {
  ScratchDirectory scratch("log-is-directory");
  const std::filesystem::path directory = scratch.file("as-a-directory");
  TOBSV_ASSERT_OK(ensure_directory(directory));
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.log_path = directory;
  const Status opened = observatory.open(config);
  TOBSV_ASSERT_FALSE(opened.ok());
  TOBSV_ASSERT_FALSE(observatory.is_open());
  TOBSV_ASSERT_FALSE(observatory.recovery().lock_acquired);
}

TOBSV_TEST("hardening", "non_finite_envelope_bands_are_refused") {
  Limits limits;
  ThermalEnvelope envelope = make_envelope(entity_id("node-01"), EntityClass::kComputeNode,
                                           MeasurementSite::kOutlet, 20.0, 35.0, 40.0, 45.0, 50.0);
  envelope.warn_c = std::numeric_limits<double>::quiet_NaN();
  TOBSV_ASSERT_FAILS_WITH(envelope.validate(limits), ErrorCode::kOutOfRange);
  envelope.warn_c = std::numeric_limits<double>::infinity();
  TOBSV_ASSERT_FAILS_WITH(envelope.validate(limits), ErrorCode::kOutOfRange);
}

TOBSV_TEST("hardening", "a_non_finite_number_cannot_be_encoded_at_all") {
  JsonValue document = JsonValue::object();
  document.set("value", JsonValue::real(std::numeric_limits<double>::quiet_NaN()));
  TOBSV_ASSERT_FAILS_WITH(document.dump_compact(), ErrorCode::kIndeterminate);
  JsonValue finite = JsonValue::object();
  finite.set("value", JsonValue::real(1.0));
  TOBSV_ASSERT_OK(finite.dump_compact());
}

TOBSV_TEST("hardening", "the_attribution_ledger_is_bounded_and_reports_what_it_dropped") {
  Limits limits;
  limits.max_attribution_limits = 2;
  AttributionLedger ledger;
  for (int index = 0; index < 6; ++index) {
    TOBSV_ASSERT_OK(ledger.add(AttributionLimitCode::kNoEnvelope,
                               entity_id("node-0" + std::to_string(index)),
                               "no envelope applies to this subject", limits));
  }
  TOBSV_ASSERT_EQ(ledger.size(), static_cast<std::size_t>(2));
  TOBSV_ASSERT_EQ(ledger.dropped(), static_cast<std::size_t>(4));

  // Repeating the same (code, subject) is a no-op rather than a duplicate entry.
  TOBSV_ASSERT_OK(ledger.add(AttributionLimitCode::kNoEnvelope, entity_id("node-00"),
                             "a different explanation", limits));
  TOBSV_ASSERT_EQ(ledger.size(), static_cast<std::size_t>(2));
  TOBSV_ASSERT_TRUE(ledger.contains(AttributionLimitCode::kNoEnvelope, entity_id("node-00")));

  AttributionLimit empty;
  empty.code = AttributionLimitCode::kNoEvidence;
  TOBSV_ASSERT_FAILS_WITH(ledger.add(empty, limits), ErrorCode::kInvalidArgument);
}

TOBSV_TEST("hardening", "bounded_registries_refuse_rather_than_grow") {
  Limits limits;
  limits.max_derating_evidence = 1;
  limits.max_couplings = 1;
  limits.max_envelopes = 1;
  DeratingRegistry derating;
  const ObservationId citation = ObservationId::unchecked("obs-0000000000000001");
  TOBSV_ASSERT_OK(derating.add(
      make_derating(entity_id("node-01"), DeratingKind::kClockThrottle, 0.2, {citation},
                    ThresholdLevel::kHigh, base_instant()),
      limits));
  TOBSV_ASSERT_FAILS_WITH(derating.add(make_derating(entity_id("node-02"),
                                                     DeratingKind::kClockThrottle, 0.2, {citation},
                                                     ThresholdLevel::kHigh, base_instant()),
                                       limits),
                          ErrorCode::kLimitExceeded);

  EnvelopeRegistry envelopes;
  TOBSV_ASSERT_OK(envelopes.add(make_envelope(entity_id("node-01"), EntityClass::kComputeNode,
                                              MeasurementSite::kOutlet, 20.0, 35.0, 40.0, 45.0,
                                              50.0),
                                limits));
  TOBSV_ASSERT_FAILS_WITH(envelopes.add(make_envelope(entity_id("node-02"), EntityClass::kComputeNode,
                                                      MeasurementSite::kOutlet, 20.0, 35.0, 40.0, 45.0,
                                                      50.0),
                                        limits),
                          ErrorCode::kLimitExceeded);

  CouplingGraph couplings;
  const std::vector<CouplingCitation> citations{
      cite(citation), cite(ObservationId::unchecked("obs-0000000000000002"))};
  TOBSV_ASSERT_OK(couplings.add(make_coupling(entity_id("node-01"), entity_id("node-02"),
                                              CouplingKind::kSupportedThermal, 0.5, citations,
                                              base_instant()),
                                limits));
  TOBSV_ASSERT_FAILS_WITH(couplings.add(make_coupling(entity_id("node-03"), entity_id("node-04"),
                                                      CouplingKind::kSupportedThermal, 0.5,
                                                      citations, base_instant()),
                                        limits),
                          ErrorCode::kLimitExceeded);
}

TOBSV_TEST("hardening", "a_duplicate_coupling_is_refused_but_a_different_one_is_not") {
  Limits limits;
  CouplingGraph graph;
  std::vector<CouplingCitation> citations{cite(ObservationId::unchecked("obs-0000000000000001")),
                                          cite(ObservationId::unchecked("obs-0000000000000002"))};
  const CouplingRelation relation = make_coupling(entity_id("node-01"), entity_id("node-02"),
                                                  CouplingKind::kSupportedThermal, 0.5, citations,
                                                  base_instant());
  TOBSV_ASSERT_OK(graph.add(relation, limits));
  TOBSV_ASSERT_FAILS_WITH(graph.add(relation, limits), ErrorCode::kDuplicateIdentity);

  // The same endpoints with a different citation is a different claim and is admitted.
  std::vector<CouplingCitation> other{cite(ObservationId::unchecked("obs-0000000000000003")),
                                      cite(ObservationId::unchecked("obs-0000000000000004"))};
  TOBSV_ASSERT_OK(graph.add(make_coupling(entity_id("node-01"), entity_id("node-02"),
                                          CouplingKind::kCoincidental, 0.5, other,
                                          base_instant()),
                            limits));
  TOBSV_ASSERT_EQ(graph.size(), static_cast<std::size_t>(2));
  TOBSV_ASSERT_EQ(graph.coincidental_count(), static_cast<std::size_t>(1));
}

TOBSV_TEST("hardening", "limits_with_a_zero_or_absurd_bound_are_rejected") {
  Limits zero;
  zero.max_observations = 0;
  TOBSV_ASSERT_FAILS_WITH(zero.validate(), ErrorCode::kInvalidArgument);

  Limits absurd;
  absurd.max_coupling_depth = ceiling::kCouplingDepth + 1;
  TOBSV_ASSERT_FAILS_WITH(absurd.validate(), ErrorCode::kOutOfRange);

  Limits inconsistent;
  inconsistent.max_edges_per_entity = inconsistent.max_topology_edges + 1;
  TOBSV_ASSERT_FAILS_WITH(inconsistent.validate(), ErrorCode::kInvalidArgument);
}

TOBSV_TEST("hardening", "analysis_never_mutates_the_record_set") {
  SingleNodeFixture fixture;
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     42.0, fixture.at, 1);
  const std::size_t before = fixture.observatory.evidence().observation_count();
  const std::size_t ids_before = fixture.observatory.evidence().observation_ids().size();
  for (int round = 0; round < 5; ++round) {
    TOBSV_ASSERT_OK(fixture.observatory.analyze_now());
  }
  TOBSV_ASSERT_EQ(fixture.observatory.evidence().observation_count(), before);
  TOBSV_ASSERT_EQ(fixture.observatory.evidence().observation_ids().size(), ids_before);
  TOBSV_ASSERT_EQ(fixture.observatory.evidence().retired_count(), static_cast<std::size_t>(0));
}

TOBSV_TEST("hardening", "an_observation_without_an_observation_timestamp_is_refused") {
  Limits limits;
  TemperatureObservation observation = make_observation(
      entity_id("node-01"), sensor_id("s"), MeasurementSite::kOutlet, 41.0, base_instant(), 1);
  observation.observed_at = Timestamp{};
  observation.id = compute_observation_id(observation);
  TOBSV_ASSERT_FAILS_WITH(observation.validate(limits), ErrorCode::kInvalidArgument);
}

TOBSV_TEST("hardening", "an_observation_without_a_measurement_site_is_unsupported") {
  Limits limits;
  TemperatureObservation observation = make_observation(
      entity_id("node-01"), sensor_id("s"), MeasurementSite::kOutlet, 41.0, base_instant(), 1);
  observation.site = MeasurementSite::kUnknown;
  observation.id = compute_observation_id(observation);
  TOBSV_ASSERT_FAILS_WITH(observation.validate(limits), ErrorCode::kUnsupported);
}

TOBSV_TEST("hardening", "a_source_that_declares_nothing_is_unsupported") {
  Limits limits;
  TemperatureObservation observation = make_observation(
      entity_id("node-01"), sensor_id("s"), MeasurementSite::kOutlet, 41.0, base_instant(), 1);
  observation.provenance.authority = AuthorityLevel::kUnknown;
  observation.id = compute_observation_id(observation);
  TOBSV_ASSERT_FAILS_WITH(observation.validate(limits), ErrorCode::kUnsupported);
}

TOBSV_TEST("hardening", "a_fence_whose_source_disagrees_with_provenance_is_a_conflict") {
  Limits limits;
  TemperatureObservation observation = make_observation(
      entity_id("node-01"), sensor_id("s"), MeasurementSite::kOutlet, 41.0, base_instant(), 1);
  observation.fence = make_fence(source_id("a-different-source"), 1);
  observation.id = compute_observation_id(observation);
  TOBSV_ASSERT_FAILS_WITH(observation.validate(limits), ErrorCode::kConflict);
}

TOBSV_TEST("hardening", "the_extreme_legal_temperature_is_accepted_and_reported_exactly") {
  SingleNodeFixture fixture;
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     999.5, fixture.at, 1);
  const Result<SubjectTemperature> resolved = fixture.observatory.evidence().resolve(
      fixture.entity, fixture.sensor, MeasurementSite::kOutlet, fixture.at,
      FreshnessPolicy::from_limits(fixture.limits));
  TOBSV_ASSERT_OK(resolved);
  TOBSV_ASSERT_EQ(resolved.value().state, EvidenceState::kFresh);
  TOBSV_ASSERT_NEAR(resolved.value().representative_celsius, 999.5, 1e-12);
  const Result<HeadroomReport> report = compute_headroom(
      resolved.value(), EntityClass::kComputeNode, fixture.observatory.envelopes(),
      fixture.observatory.limits());
  TOBSV_ASSERT_OK(report);
  TOBSV_ASSERT_NEAR(report.value().level(ThresholdLevel::kMaximum)->headroom_c, 50.0 - 999.5, 1e-9);
}

TOBSV_TEST("hardening", "non_ascii_text_survives_the_durable_codec_byte_for_byte") {
  Limits limits;
  EntityRecord record = make_entity("node-01", EntityClass::kComputeNode);
  record.label = "NÃ¸ud â sensor";
  TOBSV_ASSERT_OK(record.validate(limits));
  const Result<JsonValue> wrapped = wrap_record(RecordKind::kEntity, encode(record));
  TOBSV_ASSERT_OK(wrapped);
  const Result<std::string> document = wrapped.value().dump_compact();
  TOBSV_ASSERT_OK(document);
  const Result<DecodedRecord> decoded = decode_record(document.value(), limits);
  TOBSV_ASSERT_OK(decoded);
  TOBSV_ASSERT_EQ(decoded.value().entity.label, record.label);
  TOBSV_ASSERT_TRUE(is_valid_utf8(decoded.value().entity.label));
}

TOBSV_TEST("hardening", "an_invalid_utf8_label_is_refused_by_the_encoder") {
  Limits limits;
  EntityRecord record = make_entity("node-01", EntityClass::kComputeNode);
  // The invalid bytes are appended explicitly: a hex escape in a narrow literal is converted into
  // the execution character set, which under /utf-8 would make the label valid UTF-8 by accident.
  record.label = "bad ";
  record.label.push_back(static_cast<char>(0xFF));
  record.label.push_back(static_cast<char>(0xFE));
  record.label += " text";
  const Result<JsonValue> wrapped = wrap_record(RecordKind::kEntity, encode(record));
  TOBSV_ASSERT_OK(wrapped);
  const Result<std::string> document = wrapped.value().dump_compact();
  TOBSV_ASSERT_OK(document);
  TOBSV_ASSERT_FAILS_WITH(decode_record(document.value(), limits), ErrorCode::kMalformedInput);
}

TOBSV_TEST("hardening", "reading_a_directory_as_a_file_fails_instead_of_looping") {
  ScratchDirectory scratch("read-directory");
  TOBSV_ASSERT_FAILS_WITH(read_file_bytes(scratch.path(), 1U << 20), ErrorCode::kNotFound);
}

TOBSV_TEST("hardening", "a_record_that_fails_to_decode_is_never_partially_applied") {
  ScratchDirectory scratch("partial-apply");
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.log_path = scratch.file("store.log");
  TOBSV_ASSERT_OK(observatory.open(config));
  const std::size_t before = observatory.evidence().observation_count();
  Limits limits;
  const Status status = decode_record("{\"codec\":1,\"kind\":\"observation\",\"payload\":{}}", limits)
                            .error();
  TOBSV_ASSERT_TRUE(status.failed());
  TOBSV_ASSERT_EQ(observatory.evidence().observation_count(), before);
  TOBSV_ASSERT_OK(observatory.close());
}
// --- Regression tests for defects found by the adversarial review pass. -------------------------

TOBSV_TEST("hardening", "a_zero_byte_log_file_is_initialised_rather_than_trusted") {
  // A file that exists but holds nothing has no header. Treating it as a log made the first commit
  // overwrite the magic at offset 0, after which every open refused the file and the committed
  // records were unreachable for ever.
  ScratchDirectory scratch("zero-byte-log");
  const std::filesystem::path path = scratch.file("store.log");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("", 0);
  }
  Limits limits;
  TOBSV_ASSERT_TRUE(std::filesystem::exists(path));
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.log_path = path;
  config.limits = limits;
  TOBSV_ASSERT_OK(observatory.open(config));
  record_observation(observatory, entity_id("node-01"), sensor_id("s"), MeasurementSite::kOutlet,
                     41.0, base_instant(), 1);
  TOBSV_ASSERT_OK(observatory.flush_durable());
  TOBSV_ASSERT_OK(observatory.close());

  // The file now begins with the magic and is readable by a fresh process.
  const Result<std::string> bytes = read_file_bytes(path, 1U << 20);
  TOBSV_ASSERT_OK(bytes);
  TOBSV_ASSERT_TRUE(bytes.value().size() > ThermalLog::kHeaderSize);
  TOBSV_ASSERT_EQ(bytes.value().compare(0, ThermalLog::kMagic.size(), ThermalLog::kMagic.data(),
                                        ThermalLog::kMagic.size()),
                  0);
  const Result<ThermalLog::LoadResult> loaded = ThermalLog::inspect(path, limits);
  TOBSV_ASSERT_OK(loaded);
  TOBSV_ASSERT_EQ(loaded.value().payloads.size(), static_cast<std::size_t>(1));
}

TOBSV_TEST("hardening", "a_directory_is_not_a_log") {
  ScratchDirectory scratch("directory-log");
  const std::filesystem::path directory = scratch.file("a-directory");
  TOBSV_ASSERT_OK(ensure_directory(directory));
  Limits limits;
  TOBSV_ASSERT_FAILS_WITH(ThermalLog::inspect(directory, limits), ErrorCode::kIoFailure);
  ThermalLog log;
  TOBSV_ASSERT_FAILS_WITH(
      log.open(directory, ThermalLog::OpenMode::kOpenOrCreate, 1, limits), ErrorCode::kIoFailure);
  // The directory is still a directory: a refusal must not delete the operator's filesystem entry.
  TOBSV_ASSERT_TRUE(std::filesystem::is_directory(directory));
}

TOBSV_TEST("hardening", "the_duration_boundary_itself_is_refused") {
  // 9.2233720368547758e9 seconds scaled by 1e9 is exactly 2^63, one past the largest int64. The
  // guard used to compare against that same decimal literal, so the boundary slipped through and
  // the cast was undefined behaviour.
  TOBSV_ASSERT_FAILS_WITH(Duration::from_seconds(9.2233720368547758e9), ErrorCode::kOutOfRange);
  TOBSV_ASSERT_FAILS_WITH(Duration::from_seconds(std::numeric_limits<double>::max()),
                          ErrorCode::kOutOfRange);
  TOBSV_ASSERT_FAILS_WITH(Timestamp::from_unix_seconds(9.2233720368547758e9),
                          ErrorCode::kOutOfRange);
  // One ulp below is representable and still accepted.
  const double below = 9223372036854774784.0;  // 2^63 - 1024, exactly representable
  const Result<Duration> accepted = Duration::from_seconds(below / 1.0e9);
  TOBSV_ASSERT_OK(accepted);
  TOBSV_ASSERT_TRUE(accepted.value().nanos() > 0);
}

TOBSV_TEST("hardening", "narrowing_a_small_unsigned_value_to_a_signed_type_succeeds") {
  // Widening both operands to their common type made the negative bound of the destination look
  // like a huge unsigned value, so every small input appeared to be out of range.
  TOBSV_ASSERT_TRUE(checked::narrow<std::int32_t>(std::uint64_t{5}).has_value());
  TOBSV_ASSERT_EQ(checked::narrow<std::int32_t>(std::uint64_t{5}).value(), 5);
  TOBSV_ASSERT_TRUE(checked::narrow<std::int32_t>(std::uint64_t{0}).has_value());
  TOBSV_ASSERT_FALSE(checked::narrow<std::int32_t>(std::uint64_t{1} << 40).has_value());
  TOBSV_ASSERT_TRUE(checked::narrow<std::int64_t>(std::uint32_t{7}).has_value());
  TOBSV_ASSERT_FALSE(checked::narrow<std::uint8_t>(std::int32_t{-1}).has_value());
  TOBSV_ASSERT_EQ(checked::narrow<std::uint8_t>(std::int32_t{200}).value(), 200);
}

TOBSV_TEST("hardening", "number_encoding_does_not_depend_on_the_global_locale") {
  // Under a comma-decimal locale, snprintf writes 41,5 - which is not JSON, and which this
  // runtime's own parser then refuses, turning every durable record into an unreadable one.
  const char* previous = std::setlocale(LC_NUMERIC, nullptr);
  const std::string saved = previous == nullptr ? "C" : previous;
  const char* applied = std::setlocale(LC_NUMERIC, "German_Germany.1252");
  if (applied == nullptr) {
    applied = std::setlocale(LC_NUMERIC, "de-DE");
  }
  if (applied == nullptr) {
    // The locale is not installed on this host; the property still holds for the default locale.
    TOBSV_ASSERT_EQ(format_real(41.5), std::string("41.5"));
    return;
  }
  TOBSV_ASSERT_EQ(format_real(41.5), std::string("41.5"));
  TOBSV_ASSERT_EQ(format_real(1.0), std::string("1.0"));
  TOBSV_ASSERT_EQ(format_real(-0.25), std::string("-0.25"));

  Limits limits;
  const Result<JsonValue> parsed = JsonValue::parse("{\"celsius\":41.5}", limits);
  TOBSV_ASSERT_OK(parsed);
  TOBSV_ASSERT_NEAR(parsed.value().require_real("celsius").value(), 41.5, 1e-12);
  const Result<std::string> encoded = parsed.value().dump_compact();
  TOBSV_ASSERT_OK(encoded);
  TOBSV_ASSERT_EQ(encoded.value(), std::string("{\"celsius\":41.5}"));
  (void)std::setlocale(LC_NUMERIC, saved.c_str());
}
