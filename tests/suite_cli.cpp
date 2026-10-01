// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "test_framework.hpp"
#include "test_support.hpp"

// THERMAL_CLI_PATH is provided by tests/test_support.hpp and expands to a run-time lookup of the
// installed tool next to this test executable.

using namespace tobsv;
using namespace tobstest;

namespace {

const char* kEvaluationInstant = "2026-02-14T09:31:07Z";

// Every path is wrapped in quotes: std::system hands the whole line to a command interpreter, which
// would otherwise split the command at the first space in a path. The name is deliberately not
// quoted_path: a std::string argument would otherwise find std::quoted through argument dependent
// lookup and silently select the stream manipulator instead.
std::string quoted_path(const std::filesystem::path& path) {
  return "\"" + path.string() + "\"";
}

// Runs the installed tool as a real child process with both streams captured, and mirrors the
// captured text to the caller's file so that the tests can parse it.
//
// The command construction lives in tobstest::run_cli because a command line that begins with a
// quoted program path is subject to the command interpreter's quote-removal rules: it strips the
// first and the last quote character on the line. Wrapping the whole line in one extra pair is what
// keeps a path containing a space intact.
int run_cli(const std::string& arguments, const std::filesystem::path& output) {
  const CliRun run = tobstest::run_cli(arguments);
  const Status written = write_file_atomic(output, run.output);
  (void)written;
  return run.exit_code;
}

Result<JsonValue> read_json(const std::filesystem::path& path) {
  const Result<std::string> bytes = read_file_bytes(path, 1U << 20);
  if (!bytes.ok()) {
    return bytes.error();
  }
  return JsonValue::parse(bytes.value(), Limits{});
}

std::int64_t require_integer(const JsonValue& value, const char* key) {
  const Result<std::int64_t> field = value.require_integer(key);
  TOBSV_ASSERT_TRUE(field.ok());
  return field.value();
}

std::string require_string(const JsonValue& value, const char* key) {
  const Result<std::string> field = value.require_string(key);
  TOBSV_ASSERT_TRUE(field.ok());
  return field.value();
}

bool require_bool(const JsonValue& value, const char* key) {
  const Result<bool> field = value.require_bool(key);
  TOBSV_ASSERT_TRUE(field.ok());
  return field.value();
}

double require_real(const JsonValue& value, const char* key) {
  const Result<double> field = value.require_real(key);
  TOBSV_ASSERT_TRUE(field.ok());
  return field.value();
}

std::string hex_u32(std::uint32_t value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%08x", value);
  return std::string(buffer);
}

// One canonical record per line is exactly the format the durable log holds, so the same bytes the
// library would commit are the bytes the tool is asked to apply.
std::string line_for(RecordKind kind, JsonValue payload) {
  const Result<JsonValue> wrapped = wrap_record(kind, std::move(payload));
  TOBSV_ASSERT_TRUE(wrapped.ok());
  const Result<std::string> text = wrapped.value().dump_compact();
  TOBSV_ASSERT_TRUE(text.ok());
  return text.value();
}

struct CliCase {
  std::filesystem::path records;
  std::filesystem::path log;
  std::filesystem::path output;
  EntityRecord entity;
  ThermalEnvelope envelope;
  TemperatureObservation observation;
};

// A ready-made case: one inventoried entity, one envelope with a declared ceiling and one fresh
// reading 8.5 degrees below that ceiling.
CliCase prepare_case(ScratchDirectory& scratch, const std::string& tag) {
  CliCase prepared;
  prepared.records = scratch.file(tag + "-records.jsonl");
  prepared.log = scratch.file(tag + "-log.tobsv");
  prepared.output = scratch.file(tag + "-out.json");
  const EntityId entity = entity_id("node-cli-01");
  const SensorId sensor = sensor_id("sensor-outlet-1");
  prepared.entity = make_entity("node-cli-01", EntityClass::kComputeNode, "zone-hall-1");
  prepared.envelope = make_envelope(entity, EntityClass::kComputeNode, MeasurementSite::kOutlet,
                                    20.0, 35.0, 40.0, 45.0, 50.0);
  prepared.observation = make_observation(entity, sensor, MeasurementSite::kOutlet, 41.5,
                                          instant(kEvaluationInstant), 1, source_id("cli-source"));
  std::string content;
  content += line_for(RecordKind::kEntity, encode(prepared.entity));
  content += "\n";
  content += line_for(RecordKind::kEnvelope, encode(prepared.envelope));
  content += "\n";
  content += line_for(RecordKind::kObservation, encode(prepared.observation));
  content += "\n";
  TOBSV_ASSERT_OK(write_file_atomic(prepared.records, content));
  return prepared;
}

}  // namespace

TOBSV_TEST("cli", "version_prints_the_product_identity") {
  ScratchDirectory scratch("tobsv-cli-version");
  const std::filesystem::path output = scratch.file("version.json");
  TOBSV_ASSERT_EQ(run_cli("version", output), 0);
  const Result<JsonValue> parsed = read_json(output);
  TOBSV_ASSERT_TRUE(parsed.ok());
  TOBSV_ASSERT_EQ(require_string(parsed.value(), "product"), product_id());
  TOBSV_ASSERT_EQ(require_string(parsed.value(), "product"), std::string("thermal-observatory"));
  TOBSV_ASSERT_EQ(require_string(parsed.value(), "version"), version_string());
  TOBSV_ASSERT_FALSE(require_string(parsed.value(), "compiler").empty());
  TOBSV_ASSERT_EQ(require_integer(parsed.value(), "log_format_version"),
                  static_cast<std::int64_t>(kLogFormatVersion));
  TOBSV_ASSERT_EQ(require_integer(parsed.value(), "codec_version"),
                  static_cast<std::int64_t>(kCodecVersion));
}

TOBSV_TEST("cli", "selfcheck_reports_ok_and_the_published_crc_check_value") {
  ScratchDirectory scratch("tobsv-cli-selfcheck");
  const std::filesystem::path output = scratch.file("selfcheck.json");
  const int exit_code = run_cli("selfcheck", output);
  // A failing selfcheck has to be visible to a script without parsing prose.
  TOBSV_ASSERT_EQ(exit_code, 0);
  const Result<JsonValue> parsed = read_json(output);
  TOBSV_ASSERT_TRUE(parsed.ok());
  TOBSV_ASSERT_TRUE(require_bool(parsed.value(), "ok"));
  TOBSV_ASSERT_EQ(require_string(parsed.value(), "version"), version_string());

  const JsonValue* checks = parsed.value().find("checks");
  TOBSV_ASSERT_TRUE(checks != nullptr);
  TOBSV_ASSERT_TRUE(checks->is_array());
  TOBSV_ASSERT_TRUE(checks->items().size() >= 5);
  bool limits_reported_ok = false;
  bool crc_reports_this_builds_value = false;
  // The published check value of CRC-32C for the vector string, recomputed here rather than trusted:
  // the tool and the library have to agree, and a tool that printed its own hardcoded expectation
  // would prove nothing about the code it just built.
  const std::string expected_digest = hex_u32(crc32c("123456789"));
  TOBSV_ASSERT_EQ(expected_digest, std::string("e3069283"));
  for (const JsonValue& check : checks->items()) {
    TOBSV_ASSERT_TRUE(check.is_string());
    const std::string& text = check.as_string();
    TOBSV_ASSERT_FALSE(text.empty());
    if (text == "limits: ok") {
      limits_reported_ok = true;
    }
    if (text.find("crc32c") == 0 && text.find(expected_digest) != std::string::npos) {
      crc_reports_this_builds_value = true;
    }
    // No individual check may report a bad outcome while the summary claims success.
    TOBSV_ASSERT_TRUE(text.find("MISMATCH") == std::string::npos);
    TOBSV_ASSERT_TRUE(text.find("UNSTABLE") == std::string::npos);
  }
  TOBSV_ASSERT_TRUE(limits_reported_ok);
  TOBSV_ASSERT_TRUE(crc_reports_this_builds_value);
}

TOBSV_TEST("cli", "apply_reports_counts_and_a_second_apply_is_idempotent") {
  ScratchDirectory scratch("tobsv-cli-apply");
  const CliCase prepared = prepare_case(scratch, "apply");
  const std::string command = "apply --in " + quoted_path(prepared.records) + " --log " +
                              quoted_path(prepared.log);
  TOBSV_ASSERT_EQ(run_cli(command, prepared.output), 0);
  const Result<JsonValue> first = read_json(prepared.output);
  TOBSV_ASSERT_TRUE(first.ok());
  TOBSV_ASSERT_EQ(require_integer(first.value(), "applied"), static_cast<std::int64_t>(3));
  TOBSV_ASSERT_EQ(require_integer(first.value(), "refused"), static_cast<std::int64_t>(0));
  TOBSV_ASSERT_EQ(require_integer(first.value(), "observations_held"),
                  static_cast<std::int64_t>(1));
  TOBSV_ASSERT_EQ(require_string(first.value(), "flush"), std::string("ok"));
  TOBSV_ASSERT_TRUE(require_bool(first.value(), "durable"));
  const JsonValue* rejections = first.value().find("rejections");
  TOBSV_ASSERT_TRUE(rejections != nullptr);
  TOBSV_ASSERT_TRUE(rejections->is_array());
  TOBSV_ASSERT_TRUE(rejections->items().empty());

  // Re-delivering the very same records must not create a second observation: the same evidence
  // arriving twice is still one piece of evidence, and the second run still exits zero.
  const std::filesystem::path second_output = scratch.file("apply-second.json");
  TOBSV_ASSERT_EQ(run_cli(command, second_output), 0);
  const Result<JsonValue> second = read_json(second_output);
  TOBSV_ASSERT_TRUE(second.ok());
  TOBSV_ASSERT_EQ(require_integer(second.value(), "observations_held"),
                  static_cast<std::int64_t>(1));
  TOBSV_ASSERT_EQ(require_integer(second.value(), "observations_held"),
                  require_integer(first.value(), "observations_held"));
  TOBSV_ASSERT_EQ(require_string(second.value(), "flush"), std::string("ok"));
  // Re-registering an entity or an envelope that the log already holds is reported as a duplicate
  // identity, so whatever the second run refuses is a duplicate and nothing else.
  const JsonValue* repeated = second.value().find("rejections");
  TOBSV_ASSERT_TRUE(repeated != nullptr);
  TOBSV_ASSERT_TRUE(repeated->is_array());
  for (const JsonValue& rejection : repeated->items()) {
    TOBSV_ASSERT_TRUE(rejection.is_string());
    TOBSV_ASSERT_TRUE(rejection.as_string().find("duplicate_identity") != std::string::npos);
  }
  TOBSV_ASSERT_EQ(require_integer(second.value(), "refused"),
                  static_cast<std::int64_t>(repeated->items().size()));
}

TOBSV_TEST("cli", "analyze_reports_the_usable_subject_and_its_headroom") {
  ScratchDirectory scratch("tobsv-cli-analyze");
  const CliCase prepared = prepare_case(scratch, "analyze");
  TOBSV_ASSERT_EQ(run_cli("apply --in " + quoted_path(prepared.records) + " --log " +
                              quoted_path(prepared.log),
                          prepared.output),
                  0);

  const std::filesystem::path analysis_output = scratch.file("analyze.json");
  TOBSV_ASSERT_EQ(run_cli("analyze --log " + quoted_path(prepared.log) + " --at " +
                              std::string(kEvaluationInstant) + " --pretty",
                          analysis_output),
                  0);
  const Result<std::string> raw = read_file_bytes(analysis_output, 1U << 20);
  TOBSV_ASSERT_TRUE(raw.ok());
  // The pretty form is the same document, so it has to carry the same content and really be broken
  // across lines.
  TOBSV_ASSERT_TRUE(raw.value().find('\n') != std::string::npos);
  TOBSV_ASSERT_TRUE(raw.value().find("\n  ") != std::string::npos);
  const Result<JsonValue> parsed = JsonValue::parse(raw.value(), Limits{});
  TOBSV_ASSERT_TRUE(parsed.ok());
  TOBSV_ASSERT_EQ(require_integer(parsed.value(), "subject_count"), static_cast<std::int64_t>(1));
  TOBSV_ASSERT_EQ(require_integer(parsed.value(), "usable_subject_count"),
                  static_cast<std::int64_t>(1));
  TOBSV_ASSERT_EQ(require_integer(parsed.value(), "stale_subject_count"),
                  static_cast<std::int64_t>(0));
  TOBSV_ASSERT_EQ(require_string(parsed.value(), "evaluated_at"),
                  std::string("2026-02-14T09:31:07.000000000Z"));

  const JsonValue* headroom = parsed.value().find("headroom");
  TOBSV_ASSERT_TRUE(headroom != nullptr);
  TOBSV_ASSERT_TRUE(headroom->is_array());
  TOBSV_ASSERT_EQ(headroom->items().size(), static_cast<std::size_t>(1));
  const JsonValue& report = headroom->items()[0];
  TOBSV_ASSERT_EQ(require_string(report, "entity"), std::string("node-cli-01"));
  TOBSV_ASSERT_EQ(require_string(report, "sensor"), std::string("sensor-outlet-1"));
  TOBSV_ASSERT_EQ(require_string(report, "site"), std::string("outlet"));
  TOBSV_ASSERT_EQ(require_string(report, "state"), std::string("fresh"));
  TOBSV_ASSERT_TRUE(require_bool(report, "envelope_applied"));
  TOBSV_ASSERT_NEAR(require_real(report, "observed_celsius"), 41.5, 1e-9);

  // The headroom to the declared ceiling is exactly the ceiling less the observation.
  const JsonValue* levels = report.find("levels");
  TOBSV_ASSERT_TRUE(levels != nullptr);
  TOBSV_ASSERT_TRUE(levels->is_array());
  TOBSV_ASSERT_EQ(levels->items().size(), static_cast<std::size_t>(5));
  bool found_ceiling = false;
  for (const JsonValue& level : levels->items()) {
    if (require_string(level, "level") != "maximum") {
      continue;
    }
    found_ceiling = true;
    TOBSV_ASSERT_NEAR(require_real(level, "limit_celsius"), 50.0, 1e-9);
    TOBSV_ASSERT_NEAR(require_real(level, "headroom_celsius"), 8.5, 1e-9);
    TOBSV_ASSERT_FALSE(require_bool(level, "exceeded"));
  }
  TOBSV_ASSERT_TRUE(found_ceiling);
}

TOBSV_TEST("cli", "inspect_reports_the_record_count_of_the_log") {
  ScratchDirectory scratch("tobsv-cli-inspect");
  const CliCase prepared = prepare_case(scratch, "inspect");
  TOBSV_ASSERT_EQ(run_cli("apply --in " + quoted_path(prepared.records) + " --log " +
                              quoted_path(prepared.log),
                          prepared.output),
                  0);
  const std::filesystem::path inspect_output = scratch.file("inspect.json");
  TOBSV_ASSERT_EQ(run_cli("inspect --log " + quoted_path(prepared.log), inspect_output), 0);
  const Result<JsonValue> parsed = read_json(inspect_output);
  TOBSV_ASSERT_TRUE(parsed.ok());
  TOBSV_ASSERT_EQ(require_integer(parsed.value(), "records"), static_cast<std::int64_t>(3));
  TOBSV_ASSERT_EQ(require_integer(parsed.value(), "next_sequence"),
                  static_cast<std::int64_t>(4));
  TOBSV_ASSERT_EQ(require_integer(parsed.value(), "discarded_tail_bytes"),
                  static_cast<std::int64_t>(0));
  TOBSV_ASSERT_EQ(require_integer(parsed.value(), "writer_epoch"), static_cast<std::int64_t>(1));
  // A cleanly closed log has no bytes beyond its last committed frame.
  TOBSV_ASSERT_EQ(require_integer(parsed.value(), "committed_bytes"),
                  require_integer(parsed.value(), "file_bytes"));
  TOBSV_ASSERT_FALSE(require_bool(parsed.value(), "short_header_reinitialised"));
}

TOBSV_TEST("cli", "a_malformed_record_line_is_refused_with_a_non_zero_exit") {
  ScratchDirectory scratch("tobsv-cli-malformed");
  const CliCase prepared = prepare_case(scratch, "malformed");
  const std::filesystem::path broken = scratch.file("broken-records.jsonl");
  std::string content;
  // A document that ends inside its payload, a record whose kind does not exist and a line that is
  // not a document at all: each one has to be refused without stopping the run.
  content += "{\"codec\":1,\"kind\":\"observation\",\"payload\":{\"observation\":\n";
  content += "{\"codec\":1,\"kind\":\"nonsense\",\"payload\":{}}\n";
  content += "not json at all\n";
  TOBSV_ASSERT_OK(write_file_atomic(broken, content));

  const int exit_code =
      run_cli("apply --in " + quoted_path(broken) + " --log " + quoted_path(prepared.log), prepared.output);
  // Nothing was applied, so the run must not claim success.
  TOBSV_ASSERT_TRUE(exit_code != 0);
  const Result<JsonValue> parsed = read_json(prepared.output);
  TOBSV_ASSERT_TRUE(parsed.ok());
  TOBSV_ASSERT_EQ(require_integer(parsed.value(), "applied"), static_cast<std::int64_t>(0));
  TOBSV_ASSERT_EQ(require_integer(parsed.value(), "refused"), static_cast<std::int64_t>(3));
  TOBSV_ASSERT_EQ(require_integer(parsed.value(), "observations_held"),
                  static_cast<std::int64_t>(0));
  const JsonValue* rejections = parsed.value().find("rejections");
  TOBSV_ASSERT_TRUE(rejections != nullptr);
  TOBSV_ASSERT_TRUE(rejections->is_array());
  TOBSV_ASSERT_EQ(rejections->items().size(), static_cast<std::size_t>(3));
  // The rejection names the reason, so an operator does not have to guess which line failed.
  for (const JsonValue& rejection : rejections->items()) {
    TOBSV_ASSERT_TRUE(rejection.is_string());
    TOBSV_ASSERT_TRUE(rejection.as_string().find("record rejected") == 0);
  }
}

TOBSV_TEST("cli", "a_second_process_cannot_acquire_the_writer_lock") {
  ScratchDirectory scratch("tobsv-cli-lock");
  const CliCase prepared = prepare_case(scratch, "lock");

  // This process becomes the writer, exactly as a long running instance would.
  ObservatoryConfig config;
  config.log_path = prepared.log;
  ThermalObservatory observatory;
  TOBSV_ASSERT_OK(observatory.open(config));
  TOBSV_ASSERT_TRUE(observatory.is_open());
  TOBSV_ASSERT_TRUE(observatory.recovery().lock_acquired);

  const int blocked =
      run_cli("apply --in " + quoted_path(prepared.records) + " --log " + quoted_path(prepared.log),
              prepared.output);
  // The second writer must fail rather than block or, worse, write into the same log.
  TOBSV_ASSERT_TRUE(blocked != 0);
  const Result<std::string> message = read_file_bytes(prepared.output, 1U << 20);
  TOBSV_ASSERT_TRUE(message.ok());
  TOBSV_ASSERT_TRUE(message.value().find("lock") != std::string::npos);
  TOBSV_ASSERT_TRUE(message.value().find("already held") != std::string::npos);

  // Once the writer releases the lock the same command succeeds, so what was refused was the lock
  // and not the command line.
  TOBSV_ASSERT_OK(observatory.close());
  const std::filesystem::path after_output = scratch.file("after-lock.json");
  TOBSV_ASSERT_EQ(run_cli("apply --in " + quoted_path(prepared.records) + " --log " +
                              quoted_path(prepared.log),
                          after_output),
                  0);
  const Result<JsonValue> parsed = read_json(after_output);
  TOBSV_ASSERT_TRUE(parsed.ok());
  TOBSV_ASSERT_EQ(require_integer(parsed.value(), "applied"), static_cast<std::int64_t>(3));
  TOBSV_ASSERT_EQ(require_integer(parsed.value(), "refused"), static_cast<std::int64_t>(0));
}

TOBSV_TEST("cli", "explain_prints_the_reason_steps_and_the_attribution_limits") {
  ScratchDirectory scratch("tobsv-cli-explain");
  const CliCase prepared = prepare_case(scratch, "explain");
  TOBSV_ASSERT_EQ(run_cli("apply --in " + quoted_path(prepared.records) + " --log " +
                              quoted_path(prepared.log),
                          prepared.output),
                  0);
  const std::filesystem::path explain_output = scratch.file("explain.json");
  TOBSV_ASSERT_EQ(run_cli("explain --log " + quoted_path(prepared.log) + " --at " +
                              std::string(kEvaluationInstant),
                          explain_output),
                  0);
  const Result<JsonValue> parsed = read_json(explain_output);
  TOBSV_ASSERT_TRUE(parsed.ok());
  TOBSV_ASSERT_FALSE(require_string(parsed.value(), "digest").empty());
  TOBSV_ASSERT_FALSE(require_string(parsed.value(), "state").empty());
  const JsonValue* steps = parsed.value().find("reason_steps");
  TOBSV_ASSERT_TRUE(steps != nullptr);
  TOBSV_ASSERT_TRUE(steps->is_array());
  TOBSV_ASSERT_TRUE(!steps->items().empty());
  // Every step is text a person reads, so none of them may be empty.
  for (const JsonValue& step : steps->items()) {
    TOBSV_ASSERT_TRUE(step.is_string());
    TOBSV_ASSERT_FALSE(step.as_string().empty());
  }
  // The attribution ledger and the hotspot and propagation results travel with the explanation: an
  // explanation that hid its limits would be a claim, not an explanation.
  TOBSV_ASSERT_TRUE(parsed.value().find("attribution") != nullptr);
  TOBSV_ASSERT_TRUE(parsed.value().find("hotspots") != nullptr);
  TOBSV_ASSERT_TRUE(parsed.value().find("propagation") != nullptr);
}