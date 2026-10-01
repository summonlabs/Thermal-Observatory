// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/attribution/attribution.hpp"

#include <algorithm>

namespace tobsv {

std::string_view to_string(AttributionLimitCode code) noexcept {
  switch (code) {
    case AttributionLimitCode::kUnregisteredEntity: return "unregistered_entity";
    case AttributionLimitCode::kNoEnvelope: return "no_envelope";
    case AttributionLimitCode::kQualityUnsupplied: return "quality_unsupplied";
    case AttributionLimitCode::kQualityDegraded: return "quality_degraded";
    case AttributionLimitCode::kSyntheticSource: return "synthetic_source";
    case AttributionLimitCode::kDerivedAuthority: return "derived_authority";
    case AttributionLimitCode::kConflictingSensors: return "conflicting_sensors";
    case AttributionLimitCode::kStaleEvidence: return "stale_evidence";
    case AttributionLimitCode::kNoEvidence: return "no_evidence";
    case AttributionLimitCode::kIndeterminateTimeline: return "indeterminate_timeline";
    case AttributionLimitCode::kClockDomainMismatch: return "clock_domain_mismatch";
    case AttributionLimitCode::kCoincidentalCoupling: return "coincidental_coupling";
    case AttributionLimitCode::kRetiredEvidence: return "retired_evidence";
    case AttributionLimitCode::kDepthLimited: return "depth_limited";
    case AttributionLimitCode::kSearchBounded: return "search_bounded";
    case AttributionLimitCode::kUngroupedHotspot: return "ungrouped_hotspot";
    case AttributionLimitCode::kJointEnclosureAmbiguity: return "joint_enclosure_ambiguity";
    case AttributionLimitCode::kAuthorityBoundary: return "authority_boundary";
    case AttributionLimitCode::kPartialDerating: return "partial_derating";
  }
  return "no_evidence";
}

bool parse_attribution_limit_code(std::string_view text, AttributionLimitCode& out) noexcept {
  for (unsigned value = 0; value <= static_cast<unsigned>(AttributionLimitCode::kPartialDerating);
       ++value) {
    const auto code = static_cast<AttributionLimitCode>(value);
    if (to_string(code) == text) {
      out = code;
      return true;
    }
  }
  return false;
}

JsonValue AttributionLimit::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("code", JsonValue::text(std::string(to_string(code))));
  if (subject.is_set()) {
    out.set("subject", JsonValue::text(subject.value()));
  }
  out.set("detail", JsonValue::text(detail));
  JsonValue evidence_array = JsonValue::array();
  for (const ObservationId& id : evidence) {
    evidence_array.push(JsonValue::text(id.value()));
  }
  out.set("evidence", evidence_array);
  return out;
}

Status AttributionLedger::add(AttributionLimit limit, const Limits& limits) {
  if (limit.detail.empty()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "attribution limit " + std::string(to_string(limit.code)) +
                               " carries no explanation");
  }
  const Status text =
      validate_text(limit.detail, "attribution detail", limits.max_text_length * 2);
  if (!text.ok()) {
    return text;
  }
  if (limit.evidence.size() > limits.max_citations) {
    return Status::failure(
        ErrorCode::kLimitExceeded,
        "attribution limit " + std::string(to_string(limit.code)) + " cites too many observations");
  }
  std::sort(limit.evidence.begin(), limit.evidence.end());
  limit.evidence.erase(std::unique(limit.evidence.begin(), limit.evidence.end()),
                       limit.evidence.end());

  // One limit per (code, subject): repeating the same weakness for every subject would drown the
  // report, and the first explanation recorded is the most specific one.
  for (const AttributionLimit& existing : entries_) {
    if (existing.code == limit.code && existing.subject == limit.subject) {
      return Status::success();
    }
  }
  if (entries_.size() >= limits.max_attribution_limits) {
    ++dropped_;
    return Status::success();
  }
  entries_.push_back(std::move(limit));
  return Status::success();
}

Status AttributionLedger::add(AttributionLimitCode code, const EntityId& subject,
                              std::string detail, const Limits& limits) {
  AttributionLimit limit;
  limit.code = code;
  limit.subject = subject;
  limit.detail = std::move(detail);
  return add(std::move(limit), limits);
}

bool AttributionLedger::contains(AttributionLimitCode code) const {
  for (const AttributionLimit& limit : entries_) {
    if (limit.code == code) {
      return true;
    }
  }
  return false;
}

bool AttributionLedger::contains(AttributionLimitCode code, const EntityId& subject) const {
  for (const AttributionLimit& limit : entries_) {
    if (limit.code == code && limit.subject == subject) {
      return true;
    }
  }
  return false;
}

JsonValue AttributionLedger::to_json() const {
  JsonValue out = JsonValue::object();
  JsonValue array = JsonValue::array();
  for (const AttributionLimit& limit : entries_) {
    array.push(limit.to_json());
  }
  out.set("limits", array);
  out.set("dropped", JsonValue::integer(static_cast<std::int64_t>(dropped_)));
  return out;
}

}  // namespace tobsv
