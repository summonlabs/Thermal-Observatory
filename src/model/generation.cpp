// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/model/generation.hpp"

#include "tobsv/core/limits.hpp"

namespace tobsv {

Status Fence::validate() const {
  if (!source.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "fence has no source");
  }
  if (!epoch.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "fence epoch must be at least 1");
  }
  if (!generation.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "fence generation must be at least 1");
  }
  if (!revision.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "fence revision must be at least 1");
  }
  if (!incarnation.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "fence incarnation must be at least 1");
  }
  if (!sequence.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "fence sequence must be at least 1");
  }
  if (!attempt.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "fence attempt identity is required");
  }
  return Status::success();
}

std::string Fence::to_string() const {
  return std::string(source.is_set() ? source.value() : std::string("<unset>")) + "/e" +
         epoch.to_string() + "/g" + generation.to_string() + "/r" + revision.to_string() + "/i" +
         incarnation.to_string() + "/s" + sequence.to_string() + "/a" +
         (attempt.is_set() ? attempt.value() : std::string("<unset>"));
}

FenceOrdering order(const Fence& candidate, const Fence& reference) noexcept {
  if (candidate.source != reference.source) {
    return FenceOrdering::kIncomparable;
  }
  if (candidate.incarnation != reference.incarnation) {
    return FenceOrdering::kIncomparable;
  }
  if (candidate.epoch != reference.epoch) {
    return candidate.epoch < reference.epoch ? FenceOrdering::kOlder : FenceOrdering::kNewer;
  }
  if (candidate.generation != reference.generation) {
    return candidate.generation < reference.generation ? FenceOrdering::kOlder
                                                       : FenceOrdering::kNewer;
  }
  if (candidate.revision != reference.revision) {
    return candidate.revision < reference.revision ? FenceOrdering::kOlder : FenceOrdering::kNewer;
  }
  if (candidate.sequence != reference.sequence) {
    return candidate.sequence < reference.sequence ? FenceOrdering::kOlder : FenceOrdering::kNewer;
  }
  return FenceOrdering::kEqual;
}

std::string_view to_string(FenceVerdict verdict) noexcept {
  switch (verdict) {
    case FenceVerdict::kAdvance: return "advance";
    case FenceVerdict::kNewEpoch: return "new_epoch";
    case FenceVerdict::kDuplicate: return "duplicate";
    case FenceVerdict::kStaleEpoch: return "stale_epoch";
    case FenceVerdict::kStaleGeneration: return "stale_generation";
    case FenceVerdict::kReplayRejected: return "replay_rejected";
    case FenceVerdict::kConflict: return "conflict";
    case FenceVerdict::kIncomparable: return "incomparable";
  }
  return "incomparable";
}

FenceVerdict FenceTracker::classify(const Fence& candidate, bool same_content) const noexcept {
  if (!has_high_water_) {
    return FenceVerdict::kAdvance;
  }
  if (candidate.source != high_water_.source) {
    return FenceVerdict::kIncomparable;
  }
  if (candidate.incarnation != high_water_.incarnation) {
    // A different process incarnation is a different ordering domain. Refusing is the only honest
    // answer: there is no evidence that lets us place the two in order.
    return FenceVerdict::kIncomparable;
  }
  if (candidate.epoch > high_water_.epoch) {
    return FenceVerdict::kNewEpoch;
  }
  if (candidate.epoch < high_water_.epoch) {
    return FenceVerdict::kStaleEpoch;
  }
  if (candidate.generation > high_water_.generation) {
    return FenceVerdict::kAdvance;
  }
  if (candidate.generation < high_water_.generation) {
    return FenceVerdict::kStaleGeneration;
  }
  if (candidate.revision > high_water_.revision) {
    return FenceVerdict::kAdvance;
  }
  if (candidate.revision < high_water_.revision) {
    return FenceVerdict::kReplayRejected;
  }
  if (candidate.sequence == high_water_.sequence) {
    return same_content ? FenceVerdict::kDuplicate : FenceVerdict::kConflict;
  }
  if (candidate.sequence > high_water_.sequence) {
    return FenceVerdict::kAdvance;
  }
  // An earlier sequence within the current revision is a reordered replay of evidence that this
  // revision has already superseded.
  return FenceVerdict::kReplayRejected;
}

Status FenceTracker::observe(const Fence& candidate, bool same_content) {
  const Status status = candidate.validate();
  if (!status.ok()) {
    return status;
  }
  switch (classify(candidate, same_content)) {
    case FenceVerdict::kAdvance:
    case FenceVerdict::kNewEpoch:
      high_water_ = candidate;
      has_high_water_ = true;
      return Status::success();
    case FenceVerdict::kDuplicate:
      return Status::failure(ErrorCode::kDuplicateIdentity,
                             "evidence at " + candidate.to_string() +
                                 " is already at the high-water mark");
    case FenceVerdict::kStaleEpoch:
      return Status::failure(ErrorCode::kStaleEpoch,
                             "evidence epoch " + candidate.epoch.to_string() +
                                 " is older than epoch " + high_water_.epoch.to_string());
    case FenceVerdict::kStaleGeneration:
      return Status::failure(ErrorCode::kStaleGeneration,
                             "evidence generation " + candidate.generation.to_string() +
                                 " is older than generation " +
                                 high_water_.generation.to_string());
    case FenceVerdict::kReplayRejected:
      return Status::failure(ErrorCode::kReplayRejected,
                             "evidence at " + candidate.to_string() +
                                 " replays a position already superseded by " +
                                 high_water_.to_string());
    case FenceVerdict::kConflict:
      return Status::failure(ErrorCode::kConflict,
                             "evidence at " + candidate.to_string() +
                                 " claims the current revision with different content");
    case FenceVerdict::kIncomparable:
      return Status::failure(ErrorCode::kConflict,
                             "evidence incarnation " + candidate.incarnation.to_string() +
                                 " is not comparable with incarnation " +
                                 high_water_.incarnation.to_string() + " for source " +
                                 high_water_.source.value());
  }
  return Status::failure(ErrorCode::kInternal, "unreachable fence verdict");
}

Status FenceTracker::force_high_water(const Fence& fence) {
  const Status status = fence.validate();
  if (!status.ok()) {
    return status;
  }
  if (source_.is_set() && fence.source != source_) {
    return Status::failure(ErrorCode::kConflict,
                           "fence for source " + fence.source.value() +
                               " cannot be applied to the tracker for source " + source_.value());
  }
  if (!source_.is_set()) {
    source_ = fence.source;
  }
  high_water_ = fence;
  has_high_water_ = true;
  return Status::success();
}

void FenceTracker::reset() {
  high_water_ = Fence{};
  has_high_water_ = false;
}

}  // namespace tobsv
