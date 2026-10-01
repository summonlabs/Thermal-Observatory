// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tobsv {

// Every non-trivial operation in this runtime either produces a value or a Status. A Status always
// carries a machine-readable code and a human-readable reason; nothing fails silently and nothing
// fails with a bare boolean.
enum class ErrorCode : std::uint8_t {
  kNone = 0,
  kInvalidArgument,
  kUnknownIdentity,
  kDuplicateIdentity,
  kOutOfRange,
  kLimitExceeded,
  kMalformedInput,
  kCorruptRecord,
  kIntegrityFailure,
  kVersionMismatch,
  kStaleEpoch,
  kStaleGeneration,
  kReplayRejected,
  kLocked,
  kNotFound,
  kConflict,
  kUnsupported,
  kIndeterminate,
  kRefused,
  kIoFailure,
  kClosed,
  kShuttingDown,
  kQueueFull,
  kCancelled,
  kInternal,
};

std::string_view to_string(ErrorCode code) noexcept;

class Status {
 public:
  Status() = default;
  Status(ErrorCode code, std::string detail) : code_(code), detail_(std::move(detail)) {}

  static Status success() { return Status{}; }
  static Status failure(ErrorCode code, std::string detail) {
    return Status{code, std::move(detail)};
  }

  bool ok() const noexcept { return code_ == ErrorCode::kNone; }
  bool failed() const noexcept { return code_ != ErrorCode::kNone; }
  ErrorCode code() const noexcept { return code_; }
  const std::string& detail() const noexcept { return detail_; }

  // A single deterministic line: "ok", or "<code>: <detail>" for a failure.
  std::string describe() const;

  friend bool operator==(const Status& a, const Status& b) {
    return a.code_ == b.code_ && a.detail_ == b.detail_;
  }
  friend bool operator!=(const Status& a, const Status& b) { return !(a == b); }

 private:
  ErrorCode code_ = ErrorCode::kNone;
  std::string detail_;
};

// Value-or-error carrier. T must be default constructible so that an unchecked value() accessor
// can exist without throwing; callers are expected to test ok() first.
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  bool ok() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return ok(); }

  const Status& error() const noexcept { return status_; }
  const std::string& failure_detail() const noexcept { return status_.detail(); }

  const T& value() const& { return value_ ? *value_ : placeholder(); }
  T& value() & { return value_ ? *value_ : placeholder(); }
  const T& operator*() const& { return value(); }
  const T* operator->() const { return &value(); }

 private:
  static T& placeholder() {
    static T fallback{};
    return fallback;
  }
  std::optional<T> value_;
  Status status_;
};

// Void specialisation: success carries no payload.
template <>
class Result<void> {
 public:
  Result() = default;
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  bool ok() const noexcept { return status_.ok(); }
  explicit operator bool() const noexcept { return ok(); }
  const Status& error() const noexcept { return status_; }
  const std::string& failure_detail() const noexcept { return status_.detail(); }

 private:
  Status status_;
};

using VoidResult = Result<void>;

}  // namespace tobsv
