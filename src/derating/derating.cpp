// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/derating/derating.hpp"

#include <algorithm>

#include "tobsv/core/checked.hpp"
#include "tobsv/core/format.hpp"
#include "tobsv/core/hash.hpp"

namespace tobsv {
namespace {

int level_rank(ThresholdLevel level) { return static_cast<int>(static_cast<unsigned>(level)); }

void sort_ids(std::vector<ObservationId>& ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

JsonValue ids_to_json(const std::vector<ObservationId>& ids) {
  JsonValue out = JsonValue::array();
  for (const ObservationId& id : ids) {
    out.push(JsonValue::text(id.value()));
  }
  return out;
}

}  // namespace

std::string_view to_string(DeratingKind kind) noexcept {
  switch (kind) {
    case DeratingKind::kUnknown: return "unknown";
    case DeratingKind::kClockThrottle: return "clock_throttle";
    case DeratingKind::kFrequencyCap: return "frequency_cap";
    case DeratingKind::kPowerLimit: return "power_limit";
    case DeratingKind::kCapacityReduction: return "capacity_reduction";
    case DeratingKind::kFeatureDisable: return "feature_disable";
  }
  return "unknown";
}

bool parse_derating_kind(std::string_view text, DeratingKind& out) noexcept {
  if (text == "unknown") { out = DeratingKind::kUnknown; return true; }
  if (text == "clock_throttle") { out = DeratingKind::kClockThrottle; return true; }
  if (text == "frequency_cap") { out = DeratingKind::kFrequencyCap; return true; }
  if (text == "power_limit") { out = DeratingKind::kPowerLimit; return true; }
  if (text == "capacity_reduction") { out = DeratingKind::kCapacityReduction; return true; }
  if (text == "feature_disable") { out = DeratingKind::kFeatureDisable; return true; }
  return false;
}

Status DeratingEvidence::validate(const Limits& limits) const {
  if (!id.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "derating evidence has no identity");
  }
  if (!entity.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "derating evidence " + id.value() + " names no entity");
  }
  if (kind == DeratingKind::kUnknown) {
    return Status::failure(ErrorCode::kUnsupported,
                           "derating evidence " + id.value() + " does not say what was reduced");
  }
  if (!checked::is_finite(magnitude) || magnitude <= 0.0 || magnitude > 1.0) {
    return Status::failure(ErrorCode::kOutOfRange,
                           "derating evidence " + id.value() +
                               " must declare a magnitude in (0, 1]");
  }
  if (!asserted_by.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "derating evidence " + id.value() + " names no asserting source");
  }
  if (source_kind == SourceKind::kUnknown) {
    return Status::failure(ErrorCode::kUnsupported,
                           "derating evidence " + id.value() + " does not declare its source kind");
  }
  if (!asserted_at.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "derating evidence " + id.value() + " has no assertion timestamp");
  }
  if (!observed_at.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "derating evidence " + id.value() + " has no observation timestamp");
  }
  const Status fence_status = fence.validate();
  if (!fence_status.ok()) {
    return fence_status;
  }
  if (fence.source != asserted_by) {
    return Status::failure(ErrorCode::kConflict,
                           "derating evidence " + id.value() + " is fenced by source " +
                               fence.source.value() + " but attributed to " +
                               asserted_by.value());
  }
  const Status method_status = validate_text(method, "derating method", limits.max_text_length);
  if (!method_status.ok()) {
    return method_status;
  }
  if (citations.empty()) {
    return Status::failure(ErrorCode::kUnsupported,
                           "derating evidence " + id.value() +
                               " cites no thermal observation; an uncited derating claim is not "
                               "evidence");
  }
  const Status capacity =
      check_capacity(citations.size(), limits.max_citations, "derating citations");
  if (!capacity.ok()) {
    return capacity;
  }
  for (const ObservationId& citation : citations) {
    if (!citation.is_set()) {
      return Status::failure(ErrorCode::kInvalidArgument,
                             "derating evidence " + id.value() + " carries an empty citation");
    }
  }
  if (trigger_level == ThresholdLevel::kNominal) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "derating evidence " + id.value() +
                               " cannot claim the nominal band as its thermal trigger");
  }
  return Status::success();
}

DeratingId compute_derating_id(const DeratingEvidence& evidence) {
  StableDigest digest;
  digest.absorb_text("tobsv.derating.v1");
  digest.absorb_text(evidence.entity.value());
  digest.absorb_text(evidence.sensor.value());
  digest.absorb_number(static_cast<std::uint64_t>(evidence.site));
  digest.absorb_number(static_cast<std::uint64_t>(evidence.kind));
  digest.absorb_real(evidence.magnitude);
  digest.absorb_number(static_cast<std::uint64_t>(evidence.observed_at.unix_nanos()));
  digest.absorb_text(evidence.asserted_by.value());
  digest.absorb_number(static_cast<std::uint64_t>(evidence.source_kind));
  digest.absorb_number(static_cast<std::uint64_t>(evidence.trigger_level));
  std::vector<std::string> keys;
  keys.reserve(evidence.citations.size());
  for (const ObservationId& citation : evidence.citations) {
    keys.push_back(citation.value());
  }
  std::sort(keys.begin(), keys.end());
  for (const std::string& key : keys) {
    digest.absorb_text(key);
  }
  return DeratingId::from_digest(digest.value());
}

Status DeratingRegistry::add(const DeratingEvidence& evidence, const Limits& limits) {
  const Status valid = evidence.validate(limits);
  if (!valid.ok()) {
    return valid;
  }
  const DeratingId derived = compute_derating_id(evidence);
  if (derived != evidence.id) {
    return Status::failure(ErrorCode::kConflict,
                           "derating identity " + evidence.id.value() +
                               " does not match its content, which hashes to " + derived.value());
  }
  if (entries_.find(evidence.id) != entries_.end()) {
    return Status::failure(ErrorCode::kDuplicateIdentity,
                           "derating evidence " + evidence.id.value() + " is already recorded");
  }
  const Status capacity =
      check_capacity(entries_.size(), limits.max_derating_evidence, "derating evidence");
  if (!capacity.ok()) {
    return capacity;
  }
  entries_.emplace(evidence.id, evidence);
  by_entity_[evidence.entity].push_back(evidence.id);
  std::sort(by_entity_[evidence.entity].begin(), by_entity_[evidence.entity].end());
  return Status::success();
}

const DeratingEvidence* DeratingRegistry::find(const DeratingId& id) const {
  const auto it = entries_.find(id);
  return it == entries_.end() ? nullptr : &it->second;
}

std::vector<const DeratingEvidence*> DeratingRegistry::for_entity(const EntityId& entity) const {
  std::vector<const DeratingEvidence*> out;
  const auto it = by_entity_.find(entity);
  if (it == by_entity_.end()) {
    return out;
  }
  for (const DeratingId& id : it->second) {
    const auto entry = entries_.find(id);
    if (entry != entries_.end()) {
      out.push_back(&entry->second);
    }
  }
  return out;
}

std::vector<DeratingId> DeratingRegistry::ids() const {
  std::vector<DeratingId> out;
  out.reserve(entries_.size());
  for (const auto& entry : entries_) {
    out.push_back(entry.first);
  }
  return out;
}

JsonValue DeratingEntry::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("derating", JsonValue::text(evidence.id.value()));
  out.set("entity", JsonValue::text(evidence.entity.value()));
  out.set("kind", JsonValue::text(std::string(to_string(evidence.kind))));
  out.set("magnitude", JsonValue::real(evidence.magnitude));
  out.set("trigger_level", JsonValue::text(std::string(to_string(evidence.trigger_level))));
  out.set("asserted_by", JsonValue::text(evidence.asserted_by.value()));
  out.set("asserted_at", JsonValue::text(evidence.asserted_at.to_string()));
  out.set("observed_at", JsonValue::text(evidence.observed_at.to_string()));
  out.set("synthetic", JsonValue::boolean(evidence.is_synthetic()));
  out.set("state", JsonValue::text(std::string(to_string(state))));
  out.set("observed_band_known", JsonValue::boolean(observed_band_known));
  out.set("observed_band", JsonValue::text(std::string(to_string(observed_band))));
  out.set("peak_citation_celsius", JsonValue::real(peak_citation_celsius));
  out.set("missing_citations", ids_to_json(missing_citations));
  out.set("stale_citations", ids_to_json(stale_citations));
  out.set("cold_citations", ids_to_json(cold_citations));
  out.set("reason", JsonValue::text(reason));
  return out;
}

JsonValue DeratingAppraisal::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("entity", JsonValue::text(entity.value()));
  out.set("state", JsonValue::text(std::string(to_string(state))));
  JsonValue entry_array = JsonValue::array();
  for (const DeratingEntry& entry : entries) {
    entry_array.push(entry.to_json());
  }
  out.set("entries", entry_array);
  out.set("reason", JsonValue::text(reason));
  return out;
}

Result<DeratingAppraisal> appraise_derating(const EntityId& entity,
                                            const DeratingRegistry& registry,
                                            const EvidenceStore& store,
                                            const ThermalInventory& inventory,
                                            const EnvelopeRegistry& envelopes,
                                            const Timestamp& evaluated_at,
                                            const FreshnessPolicy& freshness,
                                            const Limits& limits) {
  const Status policy_status = freshness.validate();
  if (!policy_status.ok()) {
    return policy_status;
  }
  if (!evaluated_at.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "derating appraisal requires a set evaluation instant");
  }

  DeratingAppraisal appraisal;
  appraisal.entity = entity;

  const std::vector<const DeratingEvidence*> claims = registry.for_entity(entity);
  if (claims.empty()) {
    appraisal.state = EvidenceState::kUnknown;
    appraisal.reason = "no derating claim is recorded against entity " + entity.value();
    return appraisal;
  }

  for (const DeratingEvidence* claim : claims) {
    DeratingEntry entry;
    entry.evidence = *claim;
    bool any_fresh = false;
    bool any_band_known = false;
    for (const ObservationId& citation : claim->citations) {
      const TemperatureObservation* observation = store.find(citation);
      if (observation == nullptr) {
        entry.missing_citations.push_back(citation);
        continue;
      }
      const Duration age = age_of(observation->observed_at, evaluated_at);
      if (!observation->observed_at.is_set() || age.nanos() > freshness.window.nanos() ||
          age.nanos() < -freshness.max_clock_skew.nanos()) {
        entry.stale_citations.push_back(citation);
        continue;
      }
      any_fresh = true;
      const EntityClass entity_class = inventory.classify(observation->entity);
      const ThermalEnvelope* envelope =
          envelopes.select(observation->entity, entity_class, observation->site);
      if (envelope == nullptr) {
        entry.cold_citations.push_back(citation);
        continue;
      }
      ThresholdLevel band = ThresholdLevel::kNominal;
      if (!band_of(*envelope, observation->celsius, band)) {
        entry.cold_citations.push_back(citation);
        continue;
      }
      any_band_known = true;
      if (band >= entry.observed_band || !entry.observed_band_known) {
        entry.observed_band = band;
        entry.observed_band_known = true;
      }
      entry.peak_citation_celsius =
          entry.peak_citation_celsius == 0.0 ? observation->celsius
                                             : std::max(entry.peak_citation_celsius,
                                                        observation->celsius);
    }

    sort_ids(entry.missing_citations);
    sort_ids(entry.stale_citations);
    sort_ids(entry.cold_citations);

    if (!entry.missing_citations.empty()) {
      entry.state = EvidenceState::kUnsupported;
      entry.reason = "derating claim " + claim->id.value() + " cites " +
                     std::to_string(entry.missing_citations.size()) +
                     " observation(s) that are not held by this runtime, so the claim cannot be "
                     "checked";
    } else if (!any_fresh) {
      entry.state = EvidenceState::kStale;
      entry.reason = "every observation cited by derating claim " + claim->id.value() +
                     " is older than the freshness window of " +
                     format_seconds(freshness.window);
    } else if (!any_band_known) {
      entry.state = EvidenceState::kUnsupported;
      entry.reason = "no envelope applies to the entities cited by derating claim " +
                     claim->id.value() + ", so no thermal band can be established";
    } else if (level_rank(entry.observed_band) < level_rank(claim->trigger_level)) {
      entry.state = EvidenceState::kConflicting;
      entry.reason = "derating claim " + claim->id.value() + " names the " +
                     std::string(to_string(claim->trigger_level)) +
                     " band as its thermal trigger, but the cited observations only reach the " +
                     std::string(to_string(entry.observed_band)) + " band";
    } else {
      entry.state = EvidenceState::kFresh;
      entry.reason = "derating claim " + claim->id.value() + " is supported: the cited thermal "
                     "evidence reaches the " + std::string(to_string(entry.observed_band)) +
                     " band, at or above the claimed " +
                     std::string(to_string(claim->trigger_level)) + " trigger";
    }
    appraisal.entries.push_back(std::move(entry));
  }

  std::sort(appraisal.entries.begin(), appraisal.entries.end(),
            [](const DeratingEntry& a, const DeratingEntry& b) { return a.evidence.id < b.evidence.id; });

  std::size_t supported = 0;
  std::size_t unsupported = 0;
  std::size_t stale = 0;
  std::size_t conflicting = 0;
  for (const DeratingEntry& entry : appraisal.entries) {
    switch (entry.state) {
      case EvidenceState::kFresh: ++supported; break;
      case EvidenceState::kStale: ++stale; break;
      case EvidenceState::kConflicting: ++conflicting; break;
      default: ++unsupported; break;
    }
  }
  if (supported > 0 && unsupported == 0 && stale == 0 && conflicting == 0) {
    appraisal.state = EvidenceState::kFresh;
  } else if (supported > 0) {
    appraisal.state = EvidenceState::kConflicting;
  } else if (stale > 0 && unsupported == 0 && conflicting == 0) {
    appraisal.state = EvidenceState::kStale;
  } else if (conflicting > 0) {
    appraisal.state = EvidenceState::kConflicting;
  } else {
    appraisal.state = EvidenceState::kUnsupported;
  }
  appraisal.reason = std::to_string(appraisal.entries.size()) + " derating claim(s) against " +
                     entity.value() + ": " + std::to_string(supported) + " supported, " +
                     std::to_string(unsupported) + " unsupported, " + std::to_string(stale) +
                     " stale, " + std::to_string(conflicting) + " conflicting";
  const Status text =
      validate_text(appraisal.reason, "derating reason", limits.max_text_length * 4);
  if (!text.ok()) {
    appraisal.reason = std::to_string(appraisal.entries.size()) + " derating claim(s)";
  }
  return appraisal;
}

}  // namespace tobsv
