// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <filesystem>
#include <string>

#include "tobsv/core/result.hpp"

namespace tobsv {

// A single-writer lock over a durable store.
//
// On Windows the lock is a byte-range lock taken with LockFileEx, and on POSIX it is an flock over
// the lock file. Both are enforced by the kernel, both are released automatically when the holding
// process exits, and neither can be broken by a second process that goes through this class. The
// POSIX lock is advisory: a process that deliberately bypasses flock is not stopped. That limitation
// is stated rather than hidden.
class SingleWriterLock {
 public:
  SingleWriterLock() = default;
  ~SingleWriterLock();

  SingleWriterLock(const SingleWriterLock&) = delete;
  SingleWriterLock& operator=(const SingleWriterLock&) = delete;
  SingleWriterLock(SingleWriterLock&& other) noexcept;
  SingleWriterLock& operator=(SingleWriterLock&& other) noexcept;

  // Acquires the lock, creating the lock file when it does not exist. A second holder receives
  // ErrorCode::kLocked rather than blocking.
  Status acquire(const std::filesystem::path& path);

  bool held() const noexcept;
  void release();

  const std::string& path() const noexcept { return path_; }
  // The process identity written into the lock file, for post-mortem diagnosis.
  long owner_process_id() const noexcept { return owner_pid_; }

 private:
  void* handle_ = nullptr;
  std::string path_;
  long owner_pid_ = 0;
};

}  // namespace tobsv
