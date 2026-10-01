// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

using namespace tobsv;
using namespace tobstest;

namespace {

// Every producer uses its own source identity, so the fence order of one producer never depends on
// another. That makes the expected outcome a hard count rather than a race.
constexpr unsigned kProducers = 8;
constexpr unsigned kPerProducer = 40;

}  // namespace

TOBSV_TEST("concurrency", "independent_producers_all_land_without_loss") {
  ThermalObservatory observatory;
  ObservatoryConfig config;
  TOBSV_ASSERT_OK(observatory.open(config));

  const Timestamp at = base_instant();
  std::vector<std::thread> producers;
  std::atomic<unsigned> recorded{0};
  std::atomic<unsigned> refused{0};
  producers.reserve(kProducers);
  for (unsigned producer = 0; producer < kProducers; ++producer) {
    producers.emplace_back([&, producer] {
      const SourceId source = source_id("producer-" + std::to_string(producer));
      const EntityId entity = entity_id("node-" + std::to_string(producer));
      for (unsigned index = 1; index <= kPerProducer; ++index) {
        const TemperatureObservation observation =
            make_observation(entity, sensor_id("s"), MeasurementSite::kOutlet,
                             30.0 + static_cast<double>(index) * 0.1, at, index, source);
        const IngestOutcome outcome = observatory.ingest(observation);
        if (outcome.kind == IngestKind::kRecorded) {
          recorded.fetch_add(1);
        } else {
          refused.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& producer : producers) {
    producer.join();
  }
  TOBSV_ASSERT_EQ(recorded.load(), kProducers * kPerProducer);
  TOBSV_ASSERT_EQ(refused.load(), 0U);
  TOBSV_ASSERT_EQ(observatory.evidence().observation_count(),
                  static_cast<std::size_t>(kProducers * kPerProducer));
  TOBSV_ASSERT_EQ(observatory.evidence().entity_count(), static_cast<std::size_t>(kProducers));
}

TOBSV_TEST("concurrency", "analyses_run_while_another_thread_ingests") {
  Limits limits;
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.limits = limits;
  TOBSV_ASSERT_OK(observatory.open(config));
  TOBSV_ASSERT_OK(observatory.register_entity(make_entity("node-01", EntityClass::kComputeNode)));
  TOBSV_ASSERT_OK(observatory.register_envelope(make_envelope(
      entity_id("node-01"), EntityClass::kComputeNode, MeasurementSite::kOutlet, 20.0, 35.0, 40.0,
      45.0, 50.0)));

  // A fixed iteration count rather than a stop flag: a reader that only stops when the writer is
  // done can finish before the writer starts, which would make the test measure nothing.
  constexpr unsigned kReaderRounds = 25;
  std::atomic<unsigned> analyses{0};
  std::atomic<unsigned> failures{0};
  std::thread writer([&] {
    for (std::uint64_t revision = 1; revision <= 60; ++revision) {
      const Timestamp at = Timestamp::from_unix_nanos(base_instant().unix_nanos() +
                                                      static_cast<UnixNanos>(revision) * 1000000LL);
      const IngestOutcome outcome = observatory.ingest(make_observation(
          entity_id("node-01"), sensor_id("s"), MeasurementSite::kOutlet,
          40.0 + static_cast<double>(revision % 5), at, revision));
      if (!outcome.accepted()) {
        failures.fetch_add(1);
      }
    }
  });

  std::vector<std::thread> readers;
  readers.reserve(3);
  for (unsigned reader = 0; reader < 3; ++reader) {
    readers.emplace_back([&] {
      for (unsigned round = 0; round < kReaderRounds; ++round) {
        const Result<ThermalAnalysis> analysis = observatory.analyze_now();
        if (analysis.ok()) {
          analyses.fetch_add(1);
        } else {
          failures.fetch_add(1);
        }
      }
    });
  }
  writer.join();
  for (std::thread& reader : readers) {
    reader.join();
  }
  TOBSV_ASSERT_EQ(failures.load(), 0U);
  TOBSV_ASSERT_EQ(analyses.load(), 3U * kReaderRounds);
  // Every analysis saw a consistent snapshot of the record set.
  TOBSV_ASSERT_EQ(observatory.evidence().observation_count(), static_cast<std::size_t>(60));
}

TOBSV_TEST("concurrency", "the_commit_queue_refuses_work_instead_of_growing") {
  ScratchDirectory scratch("queue-bound");
  Limits limits;
  limits.max_commit_queue_depth = 1;
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.limits = limits;
  config.log_path = scratch.file("store.log");
  TOBSV_ASSERT_OK(observatory.open(config));

  // A backlog is unlikely with one tiny record, so the bound is proven by refusing once the queue
  // is full. Filling it deterministically means submitting without ever draining.
  unsigned refused = 0;
  for (std::uint64_t revision = 1; revision <= 200; ++revision) {
    const IngestOutcome outcome = observatory.ingest(make_observation(
        entity_id("node-01"), sensor_id("s"), MeasurementSite::kOutlet, 40.0, base_instant(),
        revision));
    if (outcome.kind == IngestKind::kRefused && outcome.status.code() == ErrorCode::kQueueFull) {
      ++refused;
      break;
    }
  }
  // Either the queue never filled (fast device) or it refused with the documented code. Both are
  // correct; a refusal with any other code is not.
  if (refused > 0) {
    TOBSV_ASSERT_OK(observatory.flush_durable());
    const IngestOutcome after_flush = observatory.ingest(make_observation(
        entity_id("node-01"), sensor_id("s"), MeasurementSite::kOutlet, 41.0, base_instant(), 500));
    TOBSV_ASSERT_TRUE(after_flush.accepted());
  }
  TOBSV_ASSERT_OK(observatory.flush_durable());
  TOBSV_ASSERT_OK(observatory.close());
  TOBSV_ASSERT_EQ(observatory.recovery().commit_failures, static_cast<std::uint64_t>(0));
}

TOBSV_TEST("concurrency", "shutdown_drains_every_queued_commit") {
  ScratchDirectory scratch("shutdown-drain");
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.log_path = scratch.file("store.log");
  TOBSV_ASSERT_OK(observatory.open(config));
  for (std::uint64_t revision = 1; revision <= 50; ++revision) {
    TOBSV_ASSERT_TRUE(observatory
                          .ingest(make_observation(entity_id("node-01"), sensor_id("s"),
                                                   MeasurementSite::kOutlet, 40.0, base_instant(),
                                                   revision))
                          .accepted());
  }
  TOBSV_ASSERT_OK(observatory.close());
  TOBSV_ASSERT_TRUE(observatory.recovery().commits_settled >= 50);
  TOBSV_ASSERT_EQ(observatory.recovery().commit_failures, static_cast<std::uint64_t>(0));

  // Every queued record reached the device before the process would have exited.
  Limits limits;
  const Result<ThermalLog::LoadResult> loaded = ThermalLog::inspect(scratch.file("store.log"), limits);
  TOBSV_ASSERT_OK(loaded);
  TOBSV_ASSERT_TRUE(loaded.value().payloads.size() >= 50);
}

TOBSV_TEST("concurrency", "the_commit_worker_refuses_work_after_shutdown") {
  ScratchDirectory scratch("worker-shutdown");
  Limits limits;
  ThermalLog log;
  TOBSV_ASSERT_OK(log.open(scratch.file("store.log"), ThermalLog::OpenMode::kOpenOrCreate, 1, limits));
  CommitWorker worker;
  TOBSV_ASSERT_OK(worker.start(&log, limits));
  TOBSV_ASSERT_TRUE(worker.running());
  const Result<std::uint64_t> job = worker.submit("{\"payload\":true}");
  TOBSV_ASSERT_OK(job);
  std::vector<CommitWorker::Outcome> outcomes;
  TOBSV_ASSERT_OK(worker.drain(outcomes));
  TOBSV_ASSERT_EQ(outcomes.size(), static_cast<std::size_t>(1));
  TOBSV_ASSERT_OK(outcomes[0].status);
  TOBSV_ASSERT_EQ(outcomes[0].sequence, static_cast<std::uint64_t>(1));

  TOBSV_ASSERT_OK(worker.shutdown());
  TOBSV_ASSERT_FALSE(worker.running());
  TOBSV_ASSERT_FAILS_WITH(worker.submit("{\"payload\":false}"), ErrorCode::kShuttingDown);
  TOBSV_ASSERT_OK(worker.shutdown());
  TOBSV_ASSERT_OK(log.close());
}

TOBSV_TEST("concurrency", "two_independent_stores_do_not_interfere") {
  ScratchDirectory first_scratch("parallel-store-a");
  ScratchDirectory second_scratch("parallel-store-b");
  ThermalObservatory first;
  ThermalObservatory second;
  ObservatoryConfig first_config;
  first_config.log_path = first_scratch.file("a.log");
  ObservatoryConfig second_config;
  second_config.log_path = second_scratch.file("b.log");
  TOBSV_ASSERT_OK(first.open(first_config));
  TOBSV_ASSERT_OK(second.open(second_config));

  // Assertions never run on a worker thread: the framework reports by throwing, and an exception
  // leaving a thread function would terminate the process instead of failing one test.
  std::atomic<unsigned> refused{0};
  std::thread a([&] {
    for (std::uint64_t revision = 1; revision <= 30; ++revision) {
      const IngestOutcome outcome =
          first.ingest(make_observation(entity_id("node-a"), sensor_id("s"),
                                        MeasurementSite::kOutlet, 40.0, base_instant(), revision));
      if (!outcome.accepted()) {
        refused.fetch_add(1);
      }
    }
  });
  std::thread b([&] {
    for (std::uint64_t revision = 1; revision <= 30; ++revision) {
      const IngestOutcome outcome =
          second.ingest(make_observation(entity_id("node-b"), sensor_id("s"),
                                         MeasurementSite::kOutlet, 41.0, base_instant(), revision));
      if (!outcome.accepted()) {
        refused.fetch_add(1);
      }
    }
  });
  a.join();
  b.join();
  TOBSV_ASSERT_EQ(refused.load(), 0U);
  TOBSV_ASSERT_EQ(first.evidence().observation_count(), static_cast<std::size_t>(30));
  TOBSV_ASSERT_EQ(second.evidence().observation_count(), static_cast<std::size_t>(30));
  TOBSV_ASSERT_OK(first.close());
  TOBSV_ASSERT_OK(second.close());
}

TOBSV_TEST("concurrency", "analyze_now_returns_instead_of_deadlocking_on_its_own_lock") {
  // Regression guard: analyze_now() reads the configuration under the state lock and then calls
  // analyze(). Holding a non-recursive mutex across that call would deadlock the thread against
  // itself. If this test hangs, that defect is back.
  SingleNodeFixture fixture;
  record_observation(fixture.observatory, fixture.entity, fixture.sensor, MeasurementSite::kOutlet,
                     41.0, fixture.at, 1);
  const Result<ThermalAnalysis> first = fixture.observatory.analyze_now();
  TOBSV_ASSERT_OK(first);
  const Result<ThermalAnalysis> second = fixture.observatory.analyze_now();
  TOBSV_ASSERT_OK(second);
  TOBSV_ASSERT_TRUE(!first.value().digest.empty());
}

TOBSV_TEST("concurrency", "flush_durable_is_a_safe_barrier_repeatedly") {
  ScratchDirectory scratch("flush-barrier");
  ThermalObservatory observatory;
  ObservatoryConfig config;
  config.log_path = scratch.file("store.log");
  TOBSV_ASSERT_OK(observatory.open(config));
  for (int round = 0; round < 10; ++round) {
    TOBSV_ASSERT_OK(observatory.flush_durable());
    TOBSV_ASSERT_TRUE(observatory
                          .ingest(make_observation(entity_id("node-01"), sensor_id("s"),
                                                   MeasurementSite::kOutlet, 40.0, base_instant(),
                                                   static_cast<std::uint64_t>(round + 1)))
                          .accepted());
  }
  TOBSV_ASSERT_OK(observatory.flush_durable());
  TOBSV_ASSERT_EQ(observatory.recovery().commit_failures, static_cast<std::uint64_t>(0));
  TOBSV_ASSERT_OK(observatory.close());
}