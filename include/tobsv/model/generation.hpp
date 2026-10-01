// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "tobsv/core/hash.hpp"
#include "tobsv/core/result.hpp"
#include "tobsv/core/strong_id.hpp"

namespace tobsv {

// Strong monotonic counters. Each is a distinct type so that an epoch can never be compared with a
// generation by accident.
template <class Tag>
class Counter {
 public:
  using Value = std::uint64_t;

  constexpr Counter() = default;
  static constexpr Counter from(Value value) { return Counter(value); }

  constexpr Value value() const noexcept { return value_; }
  // Zero means "never set". Real counters start at one.
  constexpr bool is_set() const noexcept { return value_ != 0; }

  // Saturating increment. Saturation is the conservative choice: two equal counters classify as a
  // replay rather than as new evidence.
  constexpr Counter next() const {
    return value_ == UINT64_MAX ? Counter(value_) : Counter(value_ + 1);
  }

  friend constexpr bool operator==(const Counter& a, const Counter& b) {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(const Counter& a, const Counter& b) { return !(a == b); }
  friend constexpr bool operator<(const Counter& a, const Counter& b) {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator>(const Counter& a, const Counter& b) { return b < a; }
  friend constexpr bool operator<=(const Counter& a, const Counter& b) { return !(b < a); }
  friend constexpr bool operator>=(const Counter& a, const Counter& b) { return !(a < b); }

  std::string to_string() const { return std::to_string(value_); }

 private:
  explicit constexpr Counter(Value value) : value_(value) {}
  Value value_ = 0;
};

struct EpochTag {};
struct GenerationTag {};
struct RevisionTag {};
struct SequenceTag {};
struct IncarnationTag {};

using Epoch = Counter<EpochTag>;
using Generation = Counter<GenerationTag>;
using Revision = Counter<RevisionTag>;
using Sequence = Counter<SequenceTag>;
using Incarnation = Counter<IncarnationTag>;

struct AttemptIdTag {
  static constexpr std::string_view kKind = "attempt";
  static constexpr std::string_view kPrefix = "att";
};
// Retry identity. When a caller retries a mutation it repeats the attempt identity, and the runtime
// then recognises the retry instead of applying the mutation twice.
using AttemptId = StrongId<AttemptIdTag>;

// The complete ordering position of one piece of evidence from one source.
//
//   epoch        identifies one writer incarnation of the source; a restart or reinitialisation
//                opens a new epoch, and evidence from an older epoch is never promoted to current
//   generation   monotonic within an epoch; the source bumps it when it observes a discontinuity
//   revision     monotonic within a generation; every accepted mutation bumps it
//   incarnation  the process incarnation that produced the evidence
//   sequence     per-source monotonic counter used for within-revision ordering and gap detection
//   attempt      retry identity, carried in the same durable commit as the mutation it describes
struct Fence {
  SourceId source;
  Epoch epoch;
  Generation generation;
  Revision revision;
  Incarnation incarnation;
  Sequence sequence;
  AttemptId attempt;

  Status validate() const;
  std::string to_string() const;
};

enum class FenceOrdering : std::uint8_t {
  kIncomparable = 0,  // different sources, or different process incarnations
  kOlder,
  kEqual,
  kNewer,
};

// Orders two fences. Only evidence from the same source and the same incarnation is comparable:
// assigning an order across incarnations would silently invent an ordering the evidence does not
// have.
FenceOrdering order(const Fence& candidate, const Fence& reference) noexcept;

enum class FenceVerdict : std::uint8_t {
  kAdvance = 0,      // newer than the high-water mark; accept and advance
  kNewEpoch,         // opens a new epoch; accept, resetting generation tracking
  kDuplicate,        // exactly the position already at the high-water mark; idempotent
  kStaleEpoch,       // from an epoch older than the current one; refused
  kStaleGeneration,  // from an older generation within the current epoch; refused
  kReplayRejected,   // an older revision within the current generation; refused
  kConflict,         // claims the current revision slot with different content; refused
  kIncomparable,     // a different incarnation; refused
};

std::string_view to_string(FenceVerdict verdict) noexcept;

// Tracks the high-water fence mark for one source and classifies incoming fences against it.
class FenceTracker {
 public:
  FenceTracker() = default;
  explicit FenceTracker(const SourceId& source) : source_(source) {}

  const SourceId& source() const noexcept { return source_; }
  bool has_high_water() const noexcept { return has_high_water_; }
  const Fence& high_water() const noexcept { return high_water_; }

  // Classifies without mutating. The second argument tells the tracker whether the candidate
  // describes the very same observation as the current high-water position, which is what
  // distinguishes an idempotent retry from a genuine conflict for the same slot.
  FenceVerdict classify(const Fence& candidate, bool same_content) const noexcept;

  // Applies the verdict. Only kAdvance and kNewEpoch move the mark.
  Status observe(const Fence& candidate, bool same_content);

  // Restores the mark from durable state without classification. Used when a store is reopened and
  // the persisted high-water marks are replayed; the caller is responsible for having validated the
  // fence's own integrity first.
  Status force_high_water(const Fence& fence);

  void reset();

 private:
  SourceId source_;
  Fence high_water_;
  bool has_high_water_ = false;
};

}  // namespace tobsv
