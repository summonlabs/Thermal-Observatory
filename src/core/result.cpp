// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/core/result.hpp"

namespace tobsv {

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kNone: return "none";
    case ErrorCode::kInvalidArgument: return "invalid_argument";
    case ErrorCode::kUnknownIdentity: return "unknown_identity";
    case ErrorCode::kDuplicateIdentity: return "duplicate_identity";
    case ErrorCode::kOutOfRange: return "out_of_range";
    case ErrorCode::kLimitExceeded: return "limit_exceeded";
    case ErrorCode::kMalformedInput: return "malformed_input";
    case ErrorCode::kCorruptRecord: return "corrupt_record";
    case ErrorCode::kIntegrityFailure: return "integrity_failure";
    case ErrorCode::kVersionMismatch: return "version_mismatch";
    case ErrorCode::kStaleEpoch: return "stale_epoch";
    case ErrorCode::kStaleGeneration: return "stale_generation";
    case ErrorCode::kReplayRejected: return "replay_rejected";
    case ErrorCode::kLocked: return "locked";
    case ErrorCode::kNotFound: return "not_found";
    case ErrorCode::kConflict: return "conflict";
    case ErrorCode::kUnsupported: return "unsupported";
    case ErrorCode::kIndeterminate: return "indeterminate";
    case ErrorCode::kRefused: return "refused";
    case ErrorCode::kIoFailure: return "io_failure";
    case ErrorCode::kClosed: return "closed";
    case ErrorCode::kShuttingDown: return "shutting_down";
    case ErrorCode::kQueueFull: return "queue_full";
    case ErrorCode::kCancelled: return "cancelled";
    case ErrorCode::kInternal: return "internal";
  }
  return "unrecognised";
}

std::string Status::describe() const {
  if (ok()) {
    return "ok";
  }
  return std::string(to_string(code_)) + ": " + detail_;
}

}  // namespace tobsv
