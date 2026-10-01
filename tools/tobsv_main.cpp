// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

//
// thermal-observatory - command line front end.
//
// The tool speaks the same record format the durable log uses. A line of input is a record
// document: {"codec":1,"kind":"observation","payload":{...}}. Because the durable codec validates
// every record, anything the tool accepts is also something the log can hold, and anything the log
// holds can be fed back through the tool.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "tobsv/tobsv.hpp"

namespace {

using namespace tobsv;

struct Options {
  std::string command;
  std::filesystem::path log_path;
  std::string input;
  bool have_input = false;
  std::filesystem::path output;
  bool have_output = false;
  std::string pretty = "compact";
  bool durable = true;
  bool have_at = false;
  Timestamp at;
  double freshness_seconds = 0.0;
  double agreement_tolerance_c = 0.5;
  ThresholdLevel band = ThresholdLevel::kWarn;
  bool include_propagation = true;
  std::size_t max_depth = 4;
  bool in_memory = false;
  std::uint64_t bench_iterations = 2000;
};

void usage() {
  std::printf(
      "thermal-observatory %s\n"
      "\n"
      "Usage: thermal-observatory <command> [options]\n"
      "\n"
      "Commands:\n"
      "  version                 Print the product, version, compiler and format version.\n"
      "  selfcheck               Validate the built-in limits, the codec and the record format.\n"
      "  apply                   Apply records from a file (or stdin with -) to a store.\n"
      "  analyze                 Run the full analysis and print it.\n"
      "  explain                 Print the reason steps and attribution limits only.\n"
      "  inspect                 Inspect a durable log and report its recovery state.\n"
      "  export                  Write every committed record of a log as one JSON document per line.\n"
      "  bench                   Measure completed analysis work and report it honestly.\n"
      "\n"
      "Options:\n"
      "  --log <path>            Durable log path (omit with --in-memory).\n"
      "  --in-memory             Keep everything in memory; nothing is written.\n"
      "  --in <path>             Record file, one record per line; - reads standard input.\n"
      "  --out <path>            Write to this file instead of standard output.\n"
      "  --at <rfc3339>          Evaluation instant, for example 2026-02-14T09:31:07Z.\n"
      "  --freshness <seconds>   Freshness window in seconds.\n"
      "  --agreement <celsius>   Agreement tolerance between sensors, in Celsius.\n"
      "  --band <level>          Hotspot band: warn, high, critical or maximum.\n"
      "  --no-propagation        Skip the propagation search.\n"
      "  --depth <n>             Maximum propagation depth.\n"
      "  --iterations <n>        Benchmark iterations.\n"
      "  --pretty                Indent JSON output.\n"
      "  --not-durable           Enqueue commits without waiting for the device flush.\n",
      version_string().c_str());
}

bool parse_arguments(int argc, char** argv, Options& options, std::string& error) {
  if (argc < 2) {
    error = "a command is required";
    return false;
  }
  options.command = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string flag = argv[index];
    const auto next = [&](std::string& out) {
      if (index + 1 >= argc) {
        error = "option " + flag + " needs a value";
        return false;
      }
      out = argv[++index];
      return true;
    };
    if (flag == "--log") {
      std::string value;
      if (!next(value)) return false;
      options.log_path = value;
    } else if (flag == "--in-memory") {
      options.in_memory = true;
    } else if (flag == "--in") {
      std::string value;
      if (!next(value)) return false;
      options.input = value;
      options.have_input = true;
    } else if (flag == "--out") {
      std::string value;
      if (!next(value)) return false;
      options.output = value;
      options.have_output = true;
    } else if (flag == "--at") {
      std::string value;
      if (!next(value)) return false;
      Result<Timestamp> parsed = Timestamp::parse(value);
      if (!parsed.ok()) {
        error = "cannot read the evaluation instant: " + parsed.error().describe();
        return false;
      }
      options.at = parsed.value();
      options.have_at = true;
    } else if (flag == "--freshness") {
      std::string value;
      if (!next(value)) return false;
      options.freshness_seconds = std::strtod(value.c_str(), nullptr);
    } else if (flag == "--agreement") {
      std::string value;
      if (!next(value)) return false;
      options.agreement_tolerance_c = std::strtod(value.c_str(), nullptr);
    } else if (flag == "--band") {
      std::string value;
      if (!next(value)) return false;
      if (!parse_threshold_level(value, options.band)) {
        error = "unknown threshold band '" + value + "'";
        return false;
      }
    } else if (flag == "--no-propagation") {
      options.include_propagation = false;
    } else if (flag == "--depth") {
      std::string value;
      if (!next(value)) return false;
      options.max_depth = static_cast<std::size_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (flag == "--iterations") {
      std::string value;
      if (!next(value)) return false;
      options.bench_iterations = std::strtoull(value.c_str(), nullptr, 10);
    } else if (flag == "--pretty") {
      options.pretty = "indented";
    } else if (flag == "--not-durable") {
      options.durable = false;
    } else if (flag == "--help" || flag == "-h") {
      usage();
      std::exit(0);
    } else {
      error = "unknown option '" + flag + "'";
      return false;
    }
  }
  return true;
}

// Every JSON result goes through here, so --out applies uniformly to every command that produces a
// document rather than to a privileged few.
int emit(const JsonValue& value, const Options& options) {
  Result<std::string> text =
      options.pretty == "indented" ? value.dump_indented() : value.dump_compact();
  if (!text.ok()) {
    std::fprintf(stderr, "cannot encode the result: %s\n", text.error().describe().c_str());
    return 3;
  }
  if (options.have_output) {
    const Status written = write_file_atomic(options.output, text.value() + "\n");
    if (!written.ok()) {
      std::fprintf(stderr, "%s\n", written.describe().c_str());
      return 3;
    }
    return 0;
  }
  std::printf("%s\n", text.value().c_str());
  return 0;
}

std::vector<std::string> read_lines(const Options& options, Status& status) {
  std::vector<std::string> lines;
  std::string content;
  if (!options.have_input) {
    return lines;
  }
  if (options.input == "-") {
    std::string line;
    while (std::getline(std::cin, line)) {
      lines.push_back(line);
    }
    return lines;
  }
  Result<std::string> bytes = read_file_bytes(options.input, 64U << 20);
  if (!bytes.ok()) {
    status = bytes.error();
    return lines;
  }
  content = bytes.value();
  std::size_t start = 0;
  while (start <= content.size()) {
    const std::size_t end = content.find('\n', start);
    const std::string line =
        end == std::string::npos ? content.substr(start) : content.substr(start, end - start);
    if (!line.empty() && line != "\r") {
      lines.push_back(line);
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return lines;
}

Status apply_record(ThermalObservatory& observatory, const DecodedRecord& record) {
  switch (record.kind) {
    case RecordKind::kObservation: {
      const IngestOutcome outcome = observatory.ingest(record.observation);
      if (outcome.kind == IngestKind::kDuplicate) {
        return Status::success();
      }
      return outcome.status;
    }
    case RecordKind::kFence:
      return Status::failure(ErrorCode::kUnsupported,
                             "a bare fence record is written by the runtime, not applied by hand");
    case RecordKind::kEnvelope:
      return observatory.register_envelope(record.envelope);
    case RecordKind::kEntity:
      return observatory.register_entity(record.entity);
    case RecordKind::kAdjacency:
      return observatory.declare_adjacency(record.adjacency);
    case RecordKind::kCoupling:
      return observatory.record_coupling(record.coupling);
    case RecordKind::kDerating:
      return observatory.record_derating(record.derating);
  }
  return Status::failure(ErrorCode::kInternal, "unreachable record kind");
}

ThermalQuery make_query(const Options& options, const Limits& limits) {
  ThermalQuery query;
  query.freshness = FreshnessPolicy::from_limits(limits);
  if (options.freshness_seconds > 0.0) {
    const Result<Duration> window = Duration::from_seconds(options.freshness_seconds);
    if (window.ok()) {
      query.freshness.window = window.value();
    }
  }
  query.freshness.agreement_tolerance_c = options.agreement_tolerance_c;
  query.hotspots.threshold = options.band;
  query.hotspots.episode_gap = Duration::from_nanos(limits.default_episode_gap);
  query.propagation.max_depth = options.max_depth;
  query.include_propagation = options.include_propagation;
  query.focus = options.band;
  query.evaluated_at = options.have_at ? options.at : Timestamp{};
  return query;
}

int run_apply(const Options& options, ThermalObservatory& observatory) {
  Status read_status = Status::success();
  const std::vector<std::string> lines = read_lines(options, read_status);
  if (!read_status.ok()) {
    std::fprintf(stderr, "%s\n", read_status.describe().c_str());
    return 2;
  }
  std::size_t applied = 0;
  std::size_t refused = 0;
  JsonValue rejections = JsonValue::array();
  for (const std::string& line : lines) {
    const Result<DecodedRecord> decoded = decode_record(line, observatory.limits());
    if (!decoded.ok()) {
      ++refused;
      rejections.push(JsonValue::text("record rejected: " + decoded.error().describe()));
      continue;
    }
    const Status status = apply_record(observatory, decoded.value());
    if (status.ok()) {
      ++applied;
    } else {
      ++refused;
      rejections.push(JsonValue::text(std::string(to_string(decoded.value().kind)) + ": " +
                                      status.describe()));
    }
  }
  const Status flushed = observatory.flush_durable();
  JsonValue out = JsonValue::object();
  out.set("applied", JsonValue::integer(static_cast<std::int64_t>(applied)));
  out.set("refused", JsonValue::integer(static_cast<std::int64_t>(refused)));
  out.set("rejections", rejections);
  out.set("flush", JsonValue::text(flushed.describe()));
  out.set("observations_held",
          JsonValue::integer(static_cast<std::int64_t>(observatory.evidence().observation_count())));
  out.set("durable", JsonValue::boolean(!options.in_memory));
  const int code = emit(out, options);
  if (!flushed.ok()) {
    return 4;
  }
  if (refused == 0) {
    return code;
  }
  // A run that refused everything is a failure; a run that refused some records reports the counts
  // in the document and still succeeds, so that a partial batch is visible rather than fatal.
  return applied == 0 ? 5 : code;
}

int run_analyze(const Options& options, ThermalObservatory& observatory, bool explain_only) {
  ThermalQuery query = make_query(options, observatory.limits());
  if (!options.have_at) {
    Result<Timestamp> now = Timestamp::now_utc();
    if (!now.ok()) {
      std::fprintf(stderr, "%s\n", now.error().describe().c_str());
      return 2;
    }
    query.evaluated_at = now.value();
  }
  Result<ThermalAnalysis> analysis = observatory.analyze(query);
  if (!analysis.ok()) {
    std::fprintf(stderr, "%s\n", analysis.error().describe().c_str());
    return 2;
  }
  if (!explain_only) {
    return emit(analysis.value().to_json(), options);
  }
  JsonValue out = JsonValue::object();
  out.set("digest", JsonValue::text(analysis.value().digest));
  out.set("state", JsonValue::text(std::string(to_string(analysis.value().state))));
  JsonValue steps = JsonValue::array();
  for (const std::string& step : analysis.value().reason_steps) {
    steps.push(JsonValue::text(step));
  }
  out.set("reason_steps", steps);
  out.set("attribution", analysis.value().attribution.to_json());
  out.set("hotspots", analysis.value().hotspots.to_json());
  out.set("propagation", analysis.value().propagation.to_json());
  return emit(out, options);
}

int run_inspect(const Options& options) {
  if (options.log_path.empty()) {
    std::fprintf(stderr, "inspect needs --log\n");
    return 2;
  }
  Limits limits;
  Result<ThermalLog::LoadResult> loaded = ThermalLog::inspect(options.log_path, limits);
  if (!loaded.ok()) {
    std::fprintf(stderr, "%s\n", loaded.error().describe().c_str());
    return 2;
  }
  JsonValue out = JsonValue::object();
  out.set("path", JsonValue::text(options.log_path.string()));
  out.set("records", JsonValue::integer(static_cast<std::int64_t>(loaded.value().payloads.size())));
  out.set("next_sequence",
          JsonValue::integer(static_cast<std::int64_t>(loaded.value().next_sequence)));
  out.set("file_bytes", JsonValue::integer(static_cast<std::int64_t>(loaded.value().file_bytes)));
  out.set("committed_bytes",
          JsonValue::integer(static_cast<std::int64_t>(loaded.value().committed_bytes)));
  out.set("discarded_tail_bytes",
          JsonValue::integer(static_cast<std::int64_t>(loaded.value().discarded_tail_bytes)));
  out.set("writer_epoch", JsonValue::integer(static_cast<std::int64_t>(loaded.value().writer_epoch)));
  out.set("short_header_reinitialised",
          JsonValue::boolean(loaded.value().reinitialised_short_header));
  return emit(out, options);
}

// Writes the committed records of a durable log back out in the same document format the tool
// reads. This is what makes the format shared in both directions: a log can be replayed into
// another store, or migrated, without a separate conversion step.
int run_export(const Options& options) {
  if (options.log_path.empty()) {
    std::fprintf(stderr, "export needs --log\n");
    return 2;
  }
  Limits limits;
  const Result<ThermalLog::LoadResult> loaded = ThermalLog::inspect(options.log_path, limits);
  if (!loaded.ok()) {
    std::fprintf(stderr, "%s\n", loaded.error().describe().c_str());
    return 2;
  }
  std::string document;
  for (const std::string& payload : loaded.value().payloads) {
    document += payload;
    document.push_back('\n');
  }
  if (options.have_output) {
    JsonValue out = JsonValue::object();
    out.set("path", JsonValue::text(options.log_path.string()));
    out.set("output", JsonValue::text(options.output.string()));
    out.set("records", JsonValue::integer(static_cast<std::int64_t>(loaded.value().payloads.size())));
    out.set("committed_bytes",
            JsonValue::integer(static_cast<std::int64_t>(loaded.value().committed_bytes)));
    out.set("discarded_tail_bytes",
            JsonValue::integer(static_cast<std::int64_t>(loaded.value().discarded_tail_bytes)));
    const Status written = write_file_atomic(options.output, document);
    if (!written.ok()) {
      std::fprintf(stderr, "%s\n", written.describe().c_str());
      return 2;
    }
    // The record documents own the --out path, so the summary goes to standard output rather than
    // through emit(), which would atomically replace the file that was just written.
    Result<std::string> text =
        options.pretty == "indented" ? out.dump_indented() : out.dump_compact();
    if (!text.ok()) {
      std::fprintf(stderr, "cannot encode the result: %s\n", text.error().describe().c_str());
      return 3;
    }
    std::printf("%s\n", text.value().c_str());
    return 0;
  }
  std::fwrite(document.data(), 1, document.size(), stdout);
  return 0;
}

int run_selfcheck(const Options& options) {
  Limits limits;
  std::vector<std::string> checks;
  bool ok = true;
  const Status limits_status = limits.validate();
  checks.push_back("limits: " + limits_status.describe());
  ok = ok && limits_status.ok();

  // The canonical CRC-32C check value from the specification: crc32c("123456789") == 0xE3069283.
  const std::string text = "123456789";
  const std::uint32_t digest = crc32c(text);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%08x", digest);
  const bool crc_ok = digest == 0xE3069283U;
  checks.push_back(std::string("crc32c(check vector \"123456789\"): ") + buffer +
                   (crc_ok ? " (matches the published value e3069283)" : " (MISMATCH)"));
  ok = ok && crc_ok;

  StableDigest stable;
  stable.absorb_text("tobsv");
  const bool hash_ok = stable.value() != 0;
  checks.push_back("stable digest: " + stable.hex() + (hash_ok ? " (non-trivial)" : " (MISMATCH)"));
  ok = ok && hash_ok;

  Result<Timestamp> parsed = Timestamp::parse("2026-02-14T09:31:07.123456789Z");
  const bool timestamp_ok = parsed.ok() && parsed.value().to_string() == "2026-02-14T09:31:07.123456789Z";
  checks.push_back(std::string("timestamp round trip: ") +
                   (parsed.ok() ? parsed.value().to_string() : parsed.error().describe()) +
                   (timestamp_ok ? " (stable)" : " (UNSTABLE)"));
  ok = ok && timestamp_ok;

  const std::string sample =
      "{\"codec\":1,\"kind\":\"fence\",\"payload\":{\"fence\":{\"attempt\":\"att-1\","
      "\"epoch\":1,\"generation\":1,\"incarnation\":1,\"revision\":1,\"sequence\":1,"
      "\"source\":\"selfcheck\"}}}";
  Result<DecodedRecord> decoded = decode_record(sample, limits);
  checks.push_back("codec round trip: " +
                   (decoded.ok() ? std::string("decoded ") + std::string(to_string(decoded.value().kind))
                                 : decoded.error().describe()));
  ok = ok && decoded.ok();

  Result<JsonValue> malformed = JsonValue::parse("{\"a\":1,}", limits);
  const bool malformed_rejected = !malformed.ok();
  checks.push_back(std::string("malformed json rejected: ") +
                   (malformed_rejected ? "yes" : "NO"));
  ok = ok && malformed_rejected;

  JsonValue out = JsonValue::object();
  out.set("ok", JsonValue::boolean(ok));
  out.set("version", JsonValue::text(version_string()));
  out.set("compiler", JsonValue::text(build_compiler()));
  out.set("configuration", JsonValue::text(build_configuration()));
  out.set("log_format_version", JsonValue::integer(static_cast<std::int64_t>(kLogFormatVersion)));
  JsonValue check_array = JsonValue::array();
  for (const std::string& check : checks) {
    check_array.push(JsonValue::text(check));
  }
  out.set("checks", check_array);
  const int code = emit(out, options);
  return ok ? code : 5;
}

int run_bench(const Options& options, ThermalObservatory& observatory) {
  // The benchmark measures completed work: every iteration runs a full analysis over whatever the
  // caller loaded. It reports wall time and analysis throughput, and it labels what it measured.
  const std::size_t observations = observatory.evidence().observation_count();
  const std::string label =
      observations == 0 ? "EMPTY (no evidence loaded; this measures the fixed cost of an analysis "
                          "over an empty store)"
                        : "SYNTHETIC-OR-LOADED (measures analysis over the records actually loaded)";
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(options.bench_iterations));
  const Nanos started = MonoClock::now_nanos();
  std::size_t usable = 0;
  for (std::uint64_t iteration = 0; iteration < options.bench_iterations; ++iteration) {
    ThermalQuery query = make_query(options, observatory.limits());
    Result<Timestamp> now = Timestamp::now_utc();
    if (!now.ok()) {
      return 2;
    }
    query.evaluated_at = now.value();
    const Nanos before = MonoClock::now_nanos();
    Result<ThermalAnalysis> analysis = observatory.analyze(query);
    const Nanos after = MonoClock::now_nanos();
    if (!analysis.ok()) {
      std::fprintf(stderr, "%s\n", analysis.error().describe().c_str());
      return 2;
    }
    usable += analysis.value().usable_subject_count;
    samples.push_back(static_cast<double>(after - before) / 1.0e6);
  }
  const Nanos finished = MonoClock::now_nanos();
  std::sort(samples.begin(), samples.end());
  const double total_ms = static_cast<double>(finished - started) / 1.0e6;
  const double mean = samples.empty() ? 0.0
                                      : total_ms / static_cast<double>(samples.size());
  JsonValue out = JsonValue::object();
  out.set("label", JsonValue::text(label));
  out.set("iterations", JsonValue::integer(static_cast<std::int64_t>(options.bench_iterations)));
  out.set("observations_loaded", JsonValue::integer(static_cast<std::int64_t>(observations)));
  out.set("total_ms", JsonValue::real(total_ms));
  out.set("mean_analysis_ms", JsonValue::real(mean));
  out.set("p50_analysis_ms", JsonValue::real(samples.empty() ? 0.0 : samples[samples.size() / 2]));
  out.set("p99_analysis_ms",
          JsonValue::real(samples.empty() ? 0.0 : samples[(samples.size() * 99) / 100]));
  out.set("max_analysis_ms", JsonValue::real(samples.empty() ? 0.0 : samples.back()));
  out.set("usable_subject_observations",
          JsonValue::integer(static_cast<std::int64_t>(usable)));
  out.set("compiler", JsonValue::text(build_compiler()));
  out.set("configuration", JsonValue::text(build_configuration()));
  return emit(out, options);
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!parse_arguments(argc, argv, options, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    usage();
    return 2;
  }

  if (options.command == "version") {
    JsonValue out = JsonValue::object();
    out.set("product", JsonValue::text(product_id()));
    out.set("version", JsonValue::text(version_string()));
    out.set("compiler", JsonValue::text(build_compiler()));
    out.set("configuration", JsonValue::text(build_configuration()));
    out.set("log_format_version", JsonValue::integer(static_cast<std::int64_t>(kLogFormatVersion)));
    out.set("codec_version", JsonValue::integer(static_cast<std::int64_t>(kCodecVersion)));
    return emit(out, options);
  }
  if (options.command == "help" || options.command == "--help") {
    usage();
    return 0;
  }
  if (options.command == "selfcheck") {
    return run_selfcheck(options);
  }
  if (options.command == "inspect") {
    return run_inspect(options);
  }
  if (options.command == "export") {
    return run_export(options);
  }

  if (!options.in_memory && options.log_path.empty()) {
    std::fprintf(stderr,
                 "either --log <path> or --in-memory is required for %s\n",
                 options.command.c_str());
    return 2;
  }

  ObservatoryConfig config;
  if (!options.in_memory) {
    config.log_path = options.log_path;
  }
  config.durable_commits = options.durable && !options.in_memory;

  ThermalObservatory observatory;
  const Status opened = observatory.open(config);
  if (!opened.ok()) {
    std::fprintf(stderr, "cannot open the runtime: %s\n", opened.describe().c_str());
    return 1;
  }

  int code = 0;
  if (options.command == "apply") {
    code = run_apply(options, observatory);
  } else if (options.command == "analyze") {
    code = run_analyze(options, observatory, false);
  } else if (options.command == "explain") {
    code = run_analyze(options, observatory, true);
  } else if (options.command == "bench") {
    code = run_bench(options, observatory);
  } else {
    std::fprintf(stderr, "unknown command '%s'\n", options.command.c_str());
    usage();
    code = 2;
  }

  const Status closed = observatory.close();
  if (!closed.ok() && code == 0) {
    std::fprintf(stderr, "closing the runtime reported: %s\n", closed.describe().c_str());
    code = 4;
  }
  return code;
}