// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tobsv/core/json.hpp"
#include "tobsv/core/limits.hpp"
#include "tobsv/core/result.hpp"
#include "tobsv/core/strong_id.hpp"

namespace tobsv {

// Every reason a thermal statement in this runtime might be weaker than it looks. The list is part
// of the public contract: an analysis that rests on weakened evidence names the weakness here
// rather than folding it into a confidence number.
enum class AttributionLimitCode : std::uint8_t {
  kUnregisteredEntity = 0,      // observed but not inventoried, so no class-bound envelope applies
  kNoEnvelope,                  // the entity has no envelope, so no limit exists to measure against
  kQualityUnsupplied,           // the source published no quality statement
  kQualityDegraded,             // the source declared the value degraded
  kSyntheticSource,             // the value came from a generator, not hardware
  kDerivedAuthority,            // the value was not measured directly
  kConflictingSensors,          // fresh observations of one subject disagree
  kStaleEvidence,               // the freshest evidence is outside its window
  kNoEvidence,                  // nothing was ever recorded for the subject
  kIndeterminateTimeline,       // a timestamp is missing or in the future
  kClockDomainMismatch,         // observations of one subject come from different clock domains
  kCoincidentalCoupling,        // correlation was observed and is not propagation evidence
  kRetiredEvidence,             // bounded retention dropped observations
  kDepthLimited,                // a traversal stopped at its configured depth
  kSearchBounded,               // a traversal exhausted its exploration budget
  kUngroupedHotspot,            // a hot entity is connected to nothing
  kJointEnclosureAmbiguity,     // several hot entities share one enclosure and cannot be separated
  kAuthorityBoundary,           // the requested claim belongs to an adjacent authority
  kPartialDerating,             // some derating claims could not be checked
};

std::string_view to_string(AttributionLimitCode code) noexcept;
bool parse_attribution_limit_code(std::string_view text, AttributionLimitCode& out) noexcept;

struct AttributionLimit {
  AttributionLimitCode code = AttributionLimitCode::kNoEvidence;
  EntityId subject;  // may be unset for a runtime-wide limit
  std::string detail;
  std::vector<ObservationId> evidence;

  JsonValue to_json() const;
};

// Bounded, de-duplicated, order-stable collection of attribution limits.
class AttributionLedger {
 public:
  Status add(AttributionLimit limit, const Limits& limits);
  Status add(AttributionLimitCode code, const EntityId& subject, std::string detail,
             const Limits& limits);

  const std::vector<AttributionLimit>& entries() const noexcept { return entries_; }
  bool empty() const noexcept { return entries_.empty(); }
  std::size_t size() const noexcept { return entries_.size(); }
  std::size_t dropped() const noexcept { return dropped_; }
  bool contains(AttributionLimitCode code) const;
  bool contains(AttributionLimitCode code, const EntityId& subject) const;

  JsonValue to_json() const;

 private:
  std::vector<AttributionLimit> entries_;
  std::size_t dropped_ = 0;
};

}  // namespace tobsv
