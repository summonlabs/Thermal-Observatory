// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <thread>

using namespace tobsv;
using namespace tobstest;

namespace {

std::string read_whole(const std::filesystem::path& path) {
  const Result<std::string> bytes = read_file_bytes(path, 64U << 20);
  return bytes.ok() ? bytes.value() : std::string();
}

void write_whole(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

constexpr const char* kRecordA =
    "{\"codec\":1,\"kind\":\"fence\",\"payload\":{\"fence\":{\"attempt\":\"att-a\","
    "\"epoch\":1,\"generation\":1,\"incarnation\":1,\"revision\":1,\"sequence\":1,"
    "\"source\":\"source-a\"}}}";
constexpr const char* kRecordB =
    "{\"codec\":1,\"kind\":\"fence\",\"payload\":{\"fence\":{\"attempt\":\"att-b\","
    "\"epoch\":1,\"generation\":1,\"incarnation\":1,\"revision\":2,\"sequence\":2,"
    "\"source\":\"source-a\"}}}";

}  // namespace

TOBSV_TEST("persistence", "a_created_log_round_trips_its_records") {
  ScratchDirectory scratch("log-round-trip");
  const std::filesystem::path path = scratch.file("observatory.log");
  ThermalLog log;
  Limits limits;
  TOBSV_ASSERT_OK(log.open(path, ThermalLog::OpenMode::kOpenOrCreate, 7, limits));
  const Result<std::uint64_t> first = log.append(kRecordA, true);
  TOBSV_ASSERT_OK(first);
  TOBSV_ASSERT_EQ(first.value(), static_cast<std::uint64_t>(1));
  const Result<std::uint64_t> second = log.append(kRecordB, true);
  TOBSV_ASSERT_OK(second);
  TOBSV_ASSERT_EQ(second.value(), static_cast<std::uint64_t>(2));
  TOBSV_ASSERT_OK(log.close());

  const Result<ThermalLog::LoadResult> loaded = ThermalLog::inspect(path, limits);
  TOBSV_ASSERT_OK(loaded);
  TOBSV_ASSERT_EQ(loaded.value().payloads.size(), static_cast<std::size_t>(2));
  TOBSV_ASSERT_EQ(loaded.value().payloads[0], std::string(kRecordA));
  TOBSV_ASSERT_EQ(loaded.value().next_sequence, static_cast<std::uint64_t>(3));
  TOBSV_ASSERT_EQ(loaded.value().discarded_tail_bytes, static_cast<std::uint64_t>(0));
  TOBSV_ASSERT_EQ(loaded.value().writer_epoch, static_cast<std::uint64_t>(7));
  TOBSV_ASSERT_TRUE(loaded.value().payloads[0] != loaded.value().payloads[1]);
}

TOBSV_TEST("persistence", "a_torn_tail_is_recovered_and_trimmed") {
  ScratchDirectory scratch("log-torn-tail");
  const std::filesystem::path path = scratch.file("observatory.log");
  Limits limits;
  {
    ThermalLog log;
    TOBSV_ASSERT_OK(log.open(path, ThermalLog::OpenMode::kOpenOrCreate, 1, limits));
    TOBSV_ASSERT_OK(log.append(kRecordA, true));
    TOBSV_ASSERT_OK(log.append(kRecordB, true));
    TOBSV_ASSERT_OK(log.close());
  }
  const std::string complete = read_whole(path);
  for (std::size_t keep = 1; keep <= 9; ++keep) {
    const std::string truncated = complete.substr(0, complete.size() - keep);
    write_whole(path, truncated);
    const Result<ThermalLog::LoadResult> loaded = ThermalLog::inspect(path, limits);
    TOBSV_ASSERT_OK(loaded);
    TOBSV_ASSERT_EQ(loaded.value().payloads.size(), static_cast<std::size_t>(1));
    TOBSV_ASSERT_EQ(loaded.value().payloads[0], std::string(kRecordA));
    TOBSV_ASSERT_TRUE(loaded.value().discarded_tail_bytes > 0);
    TOBSV_ASSERT_EQ(loaded.value().next_sequence, static_cast<std::uint64_t>(2));

    // Opening for writing trims the torn tail and appending continues from the commit point.
    ThermalLog reopened;
    TOBSV_ASSERT_OK(reopened.open(path, ThermalLog::OpenMode::kOpenOrCreate, 1, limits));
    TOBSV_ASSERT_TRUE(reopened.discarded_tail_bytes() > 0);
    const Result<std::uint64_t> appended = reopened.append(kRecordB, true);
    TOBSV_ASSERT_OK(appended);
    TOBSV_ASSERT_EQ(appended.value(), static_cast<std::uint64_t>(2));
    TOBSV_ASSERT_OK(reopened.close());
    const Result<ThermalLog::LoadResult> again = ThermalLog::inspect(path, limits);
    TOBSV_ASSERT_OK(again);
    TOBSV_ASSERT_EQ(again.value().payloads.size(), static_cast<std::size_t>(2));
    TOBSV_ASSERT_EQ(again.value().discarded_tail_bytes, static_cast<std::uint64_t>(0));
  }
}

TOBSV_TEST("persistence", "interior_corruption_is_refused_rather_than_trimmed") {
  ScratchDirectory scratch("log-interior");
  const std::filesystem::path path = scratch.file("observatory.log");
  Limits limits;
  {
    ThermalLog log;
    TOBSV_ASSERT_OK(log.open(path, ThermalLog::OpenMode::kOpenOrCreate, 1, limits));
    TOBSV_ASSERT_OK(log.append(kRecordA, true));
    TOBSV_ASSERT_OK(log.append(kRecordB, true));
    TOBSV_ASSERT_OK(log.append(kRecordB, true));
    TOBSV_ASSERT_OK(log.close());
  }
  std::string bytes = read_whole(path);
  // Flip a byte in the payload of the first record, leaving valid frames after it.
  bytes[ThermalLog::kHeaderSize + ThermalLog::kFrameOverhead + 5] ^= 0x5A;
  write_whole(path, bytes);
  const Result<ThermalLog::LoadResult> loaded = ThermalLog::inspect(path, limits);
  TOBSV_ASSERT_FALSE(loaded.ok());
  TOBSV_ASSERT_EQ(loaded.error().code(), ErrorCode::kIntegrityFailure);
  TOBSV_ASSERT_TRUE(loaded.error().detail().find("interior damage") != std::string::npos);

  ThermalLog log;
  TOBSV_ASSERT_FAILS_WITH(log.open(path, ThermalLog::OpenMode::kOpenOrCreate, 1, limits),
                          ErrorCode::kIntegrityFailure);
}

TOBSV_TEST("persistence", "a_header_that_does_not_verify_is_refused") {
  ScratchDirectory scratch("log-header");
  const std::filesystem::path path = scratch.file("observatory.log");
  Limits limits;
  {
    ThermalLog log;
    TOBSV_ASSERT_OK(log.open(path, ThermalLog::OpenMode::kOpenOrCreate, 1, limits));
    TOBSV_ASSERT_OK(log.append(kRecordA, true));
    TOBSV_ASSERT_OK(log.close());
  }
  const std::string good = read_whole(path);
  TOBSV_ASSERT_TRUE(good.size() > ThermalLog::kHeaderSize);

  std::string bad_magic = good;
  bad_magic[0] = 'X';
  write_whole(path, bad_magic);
  TOBSV_ASSERT_EQ(ThermalLog::inspect(path, limits).error().code(), ErrorCode::kCorruptRecord);

  std::string bad_version = good;
  bad_version[8] = static_cast<char>(9);
  write_whole(path, bad_version);
  TOBSV_ASSERT_EQ(ThermalLog::inspect(path, limits).error().code(), ErrorCode::kVersionMismatch);

  std::string bad_crc = good;
  bad_crc[48] = static_cast<char>(bad_crc[48] ^ 0xFF);
  write_whole(path, bad_crc);
  TOBSV_ASSERT_EQ(ThermalLog::inspect(path, limits).error().code(), ErrorCode::kIntegrityFailure);
}

TOBSV_TEST("persistence", "a_partial_header_is_treated_as_never_created") {
  ScratchDirectory scratch("log-short-header");
  const std::filesystem::path path = scratch.file("observatory.log");
  Limits limits;
  write_whole(path, std::string(17, 'Z'));
  const Result<ThermalLog::LoadResult> loaded = ThermalLog::inspect(path, limits);
  TOBSV_ASSERT_OK(loaded);
  TOBSV_ASSERT_TRUE(loaded.value().reinitialised_short_header);
  TOBSV_ASSERT_EQ(loaded.value().payloads.size(), static_cast<std::size_t>(0));

  ThermalLog log;
  TOBSV_ASSERT_OK(log.open(path, ThermalLog::OpenMode::kOpenOrCreate, 1, limits));
  TOBSV_ASSERT_OK(log.append(kRecordA, true));
  TOBSV_ASSERT_OK(log.close());
  const Result<ThermalLog::LoadResult> again = ThermalLog::inspect(path, limits);
  TOBSV_ASSERT_OK(again);
  TOBSV_ASSERT_EQ(again.value().payloads.size(), static_cast<std::size_t>(1));
}

TOBSV_TEST("persistence", "record_and_segment_bounds_are_enforced_before_writing") {
  ScratchDirectory scratch("log-bounds");
  const std::filesystem::path path = scratch.file("observatory.log");
  Limits limits;
  limits.max_record_bytes = 1024;
  ThermalLog log;
  TOBSV_ASSERT_OK(log.open(path, ThermalLog::OpenMode::kOpenOrCreate, 1, limits));
  TOBSV_ASSERT_FAILS_WITH(log.append(std::string(2048, 'x'), true), ErrorCode::kLimitExceeded);
  TOBSV_ASSERT_FAILS_WITH(log.append("", true), ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_OK(log.append(kRecordA, true));

  ThermalLog segment;
  Limits tiny = limits;
  tiny.max_segment_bytes = ThermalLog::kHeaderSize + ThermalLog::kFrameOverhead + 8;
  const std::filesystem::path second = scratch.file("segment.log");
  TOBSV_ASSERT_OK(segment.open(second, ThermalLog::OpenMode::kOpenOrCreate, 1, tiny));
  TOBSV_ASSERT_FAILS_WITH(segment.append(kRecordA, true), ErrorCode::kLimitExceeded);
}

TOBSV_TEST("persistence", "opening_a_log_that_does_not_exist_is_explicit") {
  ScratchDirectory scratch("log-missing");
  Limits limits;
  ThermalLog log;
  TOBSV_ASSERT_FAILS_WITH(
      log.open(scratch.file("absent.log"), ThermalLog::OpenMode::kOpenExisting, 1, limits),
      ErrorCode::kNotFound);
  TOBSV_ASSERT_FAILS_WITH(ThermalLog::inspect(scratch.file("absent.log"), limits),
                          ErrorCode::kNotFound);
}

TOBSV_TEST("persistence", "create_new_refuses_to_overwrite_a_live_log") {
  ScratchDirectory scratch("log-create-new");
  const std::filesystem::path path = scratch.file("observatory.log");
  Limits limits;
  {
    ThermalLog log;
    TOBSV_ASSERT_OK(log.open(path, ThermalLog::OpenMode::kOpenOrCreate, 1, limits));
    TOBSV_ASSERT_OK(log.append(kRecordA, true));
    TOBSV_ASSERT_OK(log.close());
  }
  ThermalLog second;
  TOBSV_ASSERT_FAILS_WITH(second.open(path, ThermalLog::OpenMode::kCreateNew, 1, limits),
                          ErrorCode::kDuplicateIdentity);
}

TOBSV_TEST("persistence", "the_single_writer_lock_excludes_a_second_holder") {
  ScratchDirectory scratch("log-lock");
  const std::filesystem::path lock_path = scratch.file("observatory.log.lock");
  SingleWriterLock first;
  TOBSV_ASSERT_OK(first.acquire(lock_path));
  TOBSV_ASSERT_TRUE(first.held());

  SingleWriterLock second;
  TOBSV_ASSERT_FAILS_WITH(second.acquire(lock_path), ErrorCode::kLocked);
  TOBSV_ASSERT_FALSE(second.held());

  // Re-acquiring through the same object is a programming error, not a silent no-op.
  TOBSV_ASSERT_FAILS_WITH(first.acquire(lock_path), ErrorCode::kLocked);

  first.release();
  TOBSV_ASSERT_FALSE(first.held());
  TOBSV_ASSERT_OK(second.acquire(lock_path));
  second.release();
}

TOBSV_TEST("persistence", "a_second_process_cannot_open_the_same_durable_store") {
  ScratchDirectory scratch("log-multiprocess");
  const std::filesystem::path path = scratch.file("store.log");
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.log_path = path;
  TOBSV_ASSERT_OK(observatory.open(config));
  TOBSV_ASSERT_OK(observatory.register_entity(make_entity("node-01", EntityClass::kComputeNode)));
  TOBSV_ASSERT_OK(observatory.flush_durable());

  // Reading a live store from another process must keep working: exclusion is the lock's job.
  const CliRun inspected = run_cli("inspect --log \"" + path.string() + "\"");
  TOBSV_ASSERT_EQ(inspected.exit_code, 0);
  TOBSV_ASSERT_TRUE(inspected.output.find("\"records\"") != std::string::npos);

  // Writing it must not.
  const CliRun applied = run_cli("apply --log \"" + path.string() + "\"");
  TOBSV_ASSERT_TRUE(applied.exit_code != 0);
  TOBSV_ASSERT_TRUE(applied.output.find("lock") != std::string::npos);
  TOBSV_ASSERT_OK(observatory.close());
}

TOBSV_TEST("persistence", "atomic_publication_never_leaves_a_partial_file") {
  ScratchDirectory scratch("atomic-publish");
  const std::filesystem::path path = scratch.file("export.json");
  TOBSV_ASSERT_OK(write_file_atomic(path, "first"));
  TOBSV_ASSERT_EQ(read_whole(path), std::string("first"));
  TOBSV_ASSERT_OK(write_file_atomic(path, "second-and-longer"));
  TOBSV_ASSERT_EQ(read_whole(path), std::string("second-and-longer"));
  TOBSV_ASSERT_FALSE(std::filesystem::exists(scratch.file("export.json.tmp")));
  TOBSV_ASSERT_FAILS_WITH(read_file_bytes(path, 4), ErrorCode::kLimitExceeded);
  TOBSV_ASSERT_OK(remove_file_if_present(path));
  TOBSV_ASSERT_FALSE(std::filesystem::exists(path));
  TOBSV_ASSERT_OK(remove_file_if_present(path));
}

TOBSV_TEST("persistence", "a_log_can_be_exported_and_replayed_into_another_log") {
  ScratchDirectory scratch("export-replay");
  const std::filesystem::path first = scratch.file("first.log");
  const std::filesystem::path exported = scratch.file("records.jsonl");
  const std::filesystem::path second = scratch.file("second.log");

  {
    ThermalObservatory observatory;
    ObservatoryConfig config;
    config.log_path = first;
    TOBSV_ASSERT_OK(observatory.open(config));
    TOBSV_ASSERT_OK(observatory.register_entity(make_entity("node-01", EntityClass::kComputeNode)));
    TOBSV_ASSERT_OK(observatory.register_envelope(make_envelope(
        entity_id("node-01"), EntityClass::kComputeNode, MeasurementSite::kOutlet, 20.0, 35.0, 40.0,
        45.0, 50.0)));
    record_observation(observatory, entity_id("node-01"), sensor_id("s"), MeasurementSite::kOutlet,
                       41.5, base_instant(), 1);
    TOBSV_ASSERT_OK(observatory.flush_durable());
    TOBSV_ASSERT_OK(observatory.close());
  }

  // The tool reads the same record format the log writes, so a store can be replayed into another
  // store without a conversion step.
  const CliRun exported_run =
      run_cli("export --log \"" + first.string() + "\" --out \"" + exported.string() + "\"");
  TOBSV_ASSERT_EQ(exported_run.exit_code, 0);
  const Result<std::string> records = read_file_bytes(exported, 1U << 20);
  TOBSV_ASSERT_OK(records);
  TOBSV_ASSERT_TRUE(records.value().find("\"kind\":\"observation\"") != std::string::npos);
  TOBSV_ASSERT_TRUE(records.value().find("\"kind\":\"entity\"") != std::string::npos);

  const CliRun applied_run =
      run_cli("apply --log \"" + second.string() + "\" --in \"" + exported.string() + "\"");
  TOBSV_ASSERT_EQ(applied_run.exit_code, 0);
  TOBSV_ASSERT_TRUE(applied_run.output.find("\"refused\":0") != std::string::npos);

  Limits limits;
  const Result<ThermalLog::LoadResult> first_load = ThermalLog::inspect(first, limits);
  const Result<ThermalLog::LoadResult> second_load = ThermalLog::inspect(second, limits);
  TOBSV_ASSERT_OK(first_load);
  TOBSV_ASSERT_OK(second_load);
  TOBSV_ASSERT_EQ(first_load.value().payloads.size(), second_load.value().payloads.size());
  for (std::size_t index = 0; index < first_load.value().payloads.size(); ++index) {
    TOBSV_ASSERT_EQ(first_load.value().payloads[index], second_load.value().payloads[index]);
  }

  // The replayed store answers the core question identically.
  ThermalObservatory replay;
  ObservatoryConfig config;
  config.log_path = second;
  config.writer_epoch = 2;
  TOBSV_ASSERT_OK(replay.open(config));
  ThermalQuery query;
  query.evaluated_at = base_instant();
  query.freshness = FreshnessPolicy::from_limits(limits);
  const Result<ThermalAnalysis> analysis = replay.analyze(query);
  TOBSV_ASSERT_OK(analysis);
  TOBSV_ASSERT_EQ(analysis.value().usable_subject_count, static_cast<std::size_t>(1));
  TOBSV_ASSERT_NEAR(analysis.value().headroom[0].level(ThresholdLevel::kCritical)->headroom_c, 3.5,
                    1e-9);
  TOBSV_ASSERT_OK(replay.close());
}

TOBSV_TEST("persistence", "the_codec_is_strict_about_unknown_versions_and_shapes") {
  Limits limits;
  TOBSV_ASSERT_EQ(decode_record("{\"codec\":2,\"kind\":\"fence\",\"payload\":{}}", limits)
                      .error()
                      .code(),
                  ErrorCode::kVersionMismatch);
  TOBSV_ASSERT_EQ(decode_record("{\"codec\":1,\"kind\":\"nonsense\",\"payload\":{}}", limits)
                      .error()
                      .code(),
                  ErrorCode::kMalformedInput);
  TOBSV_ASSERT_EQ(decode_record("{\"codec\":1,\"kind\":\"fence\"}", limits).error().code(),
                  ErrorCode::kMalformedInput);
  TOBSV_ASSERT_EQ(decode_record("[]", limits).error().code(), ErrorCode::kMalformedInput);
  TOBSV_ASSERT_EQ(decode_record("{\"codec\":1,\"kind\":\"fence\",\"payload\":{},}", limits)
                      .error()
                      .code(),
                  ErrorCode::kMalformedInput);
  TOBSV_ASSERT_OK(decode_record(kRecordA, limits));
}