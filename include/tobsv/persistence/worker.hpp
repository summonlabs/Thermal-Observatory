// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tobsv/core/limits.hpp"
#include "tobsv/core/result.hpp"
#include "tobsv/persistence/log.hpp"

namespace tobsv {

// The single background committer. It owns the only writable handle to the durable log, so there is
// exactly one writer and no interleaving to reason about.
//
// Locking contract (see docs/concurrency.md):
//   * the caller may hold the observatory state lock while calling submit or drain;
//   * this class never calls back into the observatory, so it cannot re-enter the caller's lock;
//   * the device write happens outside this class's own mutex, so submit and drain are never held
//     up by device latency;
//   * shutdown() releases its mutex before joining, so the worker can always finish.
class CommitWorker {
 public:
  CommitWorker() = default;
  ~CommitWorker();

  CommitWorker(const CommitWorker&) = delete;
  CommitWorker& operator=(const CommitWorker&) = delete;
  CommitWorker(CommitWorker&&) = delete;
  CommitWorker& operator=(CommitWorker&&) = delete;

  struct Outcome {
    std::uint64_t job_id = 0;
    std::uint64_t sequence = 0;
    Status status;
  };

  // Starts the worker over a log the caller keeps alive for the worker's lifetime.
  Status start(ThermalLog* log, const Limits& limits);

  // Enqueues a durable commit. kQueueFull means the caller must let the device catch up; the queue
  // never grows without limit.
  Result<std::uint64_t> submit(std::string payload);

  // Blocks until every submitted job has settled and hands back the outcomes recorded since the
  // previous drain.
  Status drain(std::vector<Outcome>& outcomes);

  // Drains the queue and stops the thread. Safe to call more than once.
  Status shutdown();

  // True when the queue has room for that many jobs. Every submitter takes the observatory state
  // lock first, so a successful pre-flight check makes that many following submits race free: only
  // this thread can add to the queue in between, and the worker only removes from it.
  bool has_room_for(std::size_t count) const;

  bool has_room() const { return has_room_for(1); }

  bool running() const;
  std::size_t pending() const;
  std::uint64_t submitted() const;
  std::uint64_t failed() const;
  std::uint64_t dropped_outcomes() const;

 private:
  void run();

  ThermalLog* log_ = nullptr;
  Limits limits_;
  mutable std::mutex mutex_;
  std::condition_variable idle_;
  std::deque<std::pair<std::uint64_t, std::string>> queue_;
  std::vector<Outcome> outcomes_;
  std::thread thread_;
  std::uint64_t next_job_id_ = 1;
  std::uint64_t submitted_ = 0;
  std::uint64_t failed_ = 0;
  std::uint64_t reported_failures_ = 0;
  std::uint64_t dropped_outcomes_ = 0;
  bool busy_ = false;
  bool stop_requested_ = false;
  bool running_ = false;
};

}  // namespace tobsv
