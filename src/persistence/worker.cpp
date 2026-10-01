// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/persistence/worker.hpp"

#include <utility>

namespace tobsv {

CommitWorker::~CommitWorker() { (void)shutdown(); }

Status CommitWorker::start(ThermalLog* log, const Limits& limits) {
  if (log == nullptr) {
    return Status::failure(ErrorCode::kInvalidArgument, "the commit worker needs a durable log");
  }
  const Status limits_status = limits.validate();
  if (!limits_status.ok()) {
    return limits_status;
  }
  std::unique_lock lock(mutex_);
  if (running_) {
    return Status::failure(ErrorCode::kInvalidArgument, "the commit worker is already running");
  }
  log_ = log;
  limits_ = limits;
  stop_requested_ = false;
  busy_ = false;
  queue_.clear();
  outcomes_.clear();
  running_ = true;
  lock.unlock();
  thread_ = std::thread([this] { run(); });
  return Status::success();
}

Result<std::uint64_t> CommitWorker::submit(std::string payload) {
  std::unique_lock lock(mutex_);
  if (!running_ || stop_requested_) {
    return Status::failure(ErrorCode::kShuttingDown,
                           "the commit worker is not accepting work");
  }
  if (queue_.size() >= limits_.max_commit_queue_depth) {
    return Status::failure(ErrorCode::kQueueFull,
                           "the durable commit queue holds its bound of " +
                               std::to_string(limits_.max_commit_queue_depth) + " jobs");
  }
  const std::uint64_t job_id = next_job_id_++;
  queue_.emplace_back(job_id, std::move(payload));
  ++submitted_;
  idle_.notify_all();
  return job_id;
}

Status CommitWorker::drain(std::vector<Outcome>& outcomes) {
  std::unique_lock lock(mutex_);
  idle_.wait(lock, [this] { return queue_.empty() && !busy_; });
  const std::size_t first = outcomes.size();
  outcomes.insert(outcomes.end(), outcomes_.begin(), outcomes_.end());
  outcomes_.clear();
  const std::uint64_t failures_before = reported_failures_;
  reported_failures_ = failed_;
  for (std::size_t index = first; index < outcomes.size(); ++index) {
    if (!outcomes[index].status.ok()) {
      return Status::failure(outcomes[index].status.code(),
                             "a durable commit failed: " + outcomes[index].status.describe());
    }
  }
  // The outcome list is bounded, so a failure can outlive the outcome that described it. The
  // monotonic failure counter is what makes that impossible to report as success.
  if (failed_ > failures_before) {
    return Status::failure(ErrorCode::kIoFailure,
                           "a durable commit failed since the previous drain; " +
                               std::to_string(failed_ - failures_before) +
                               " failure(s) were recorded, and the outcome describing the oldest "
                               "may have been dropped by the outcome bound");
  }
  return Status::success();
}

Status CommitWorker::shutdown() {
  {
    std::unique_lock lock(mutex_);
    if (!thread_.joinable()) {
      running_ = false;
      return Status::success();
    }
    stop_requested_ = true;
    idle_.notify_all();
  }
  // Joining without holding the mutex is what lets the worker observe the request, finish the
  // queued work and store its last outcome.
  thread_.join();
  std::unique_lock lock(mutex_);
  running_ = false;
  log_ = nullptr;
  return Status::success();
}

bool CommitWorker::has_room_for(std::size_t count) const {
  std::unique_lock lock(mutex_);
  if (!running_ || stop_requested_) {
    return false;
  }
  return limits_.max_commit_queue_depth - queue_.size() >= count;
}

bool CommitWorker::running() const {
  std::unique_lock lock(mutex_);
  return running_;
}

std::size_t CommitWorker::pending() const {
  std::unique_lock lock(mutex_);
  return queue_.size() + (busy_ ? 1U : 0U);
}

std::uint64_t CommitWorker::submitted() const {
  std::unique_lock lock(mutex_);
  return submitted_;
}

std::uint64_t CommitWorker::failed() const {
  std::unique_lock lock(mutex_);
  return failed_;
}

std::uint64_t CommitWorker::dropped_outcomes() const {
  std::unique_lock lock(mutex_);
  return dropped_outcomes_;
}

void CommitWorker::run() {
  for (;;) {
    std::pair<std::uint64_t, std::string> job;
    {
      std::unique_lock lock(mutex_);
      idle_.wait(lock, [this] { return stop_requested_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stop_requested_) {
          break;
        }
        continue;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
      busy_ = true;
    }

    // The device write happens outside the mutex. Only this thread ever touches the log, so there
    // is no second writer to serialise against.
    Outcome outcome;
    outcome.job_id = job.first;
    Result<std::uint64_t> sequence = log_->append(job.second, true);
    if (sequence.ok()) {
      outcome.sequence = sequence.value();
      outcome.status = Status::success();
    } else {
      outcome.status = sequence.error();
    }

    {
      std::unique_lock lock(mutex_);
      busy_ = false;
      if (!outcome.status.ok()) {
        ++failed_;
      }
      const std::size_t bound = limits_.max_commit_queue_depth * 4U;
      if (outcomes_.size() >= bound) {
        outcomes_.erase(outcomes_.begin());
        ++dropped_outcomes_;
      }
      outcomes_.push_back(std::move(outcome));
      idle_.notify_all();
    }
  }
  std::unique_lock lock(mutex_);
  busy_ = false;
  idle_.notify_all();
}

}  // namespace tobsv
