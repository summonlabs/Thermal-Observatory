// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/evidence/store.hpp"

#include <algorithm>

#include "tobsv/core/format.hpp"

namespace tobsv {
namespace {

// Ranking used to pick a representative among agreeing observations. Higher wins. The ranking is
// about how the value was obtained, not about which source is more important.
int authority_rank(AuthorityLevel level) {
  switch (level) {
    case AuthorityLevel::kMeasured: return 4;
    case AuthorityLevel::kDerived: return 3;
    case AuthorityLevel::kModeled: return 2;
    case AuthorityLevel::kSynthetic: return 1;
    case AuthorityLevel::kUnknown: return 0;
  }
  return 0;
}

void sort_refs_by_id(std::vector<ObservationRef>& refs) {
  std::sort(refs.begin(), refs.end(),
            [](const ObservationRef& a, const ObservationRef& b) { return a.id < b.id; });
}

}  // namespace

std::string_view to_string(IngestKind kind) noexcept {
  switch (kind) {
    case IngestKind::kRecorded: return "recorded";
    case IngestKind::kDuplicate: return "duplicate";
    case IngestKind::kRefused: return "refused";
    case IngestKind::kConflict: return "conflict";
  }
  return "refused";
}

JsonValue IngestOutcome::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("kind", JsonValue::text(std::string(to_string(kind))));
  out.set("accepted", JsonValue::boolean(accepted()));
  out.set("observation", JsonValue::text(id.value()));
  out.set("status", JsonValue::text(status.describe()));
  out.set("high_water", JsonValue::text(high_water.to_string()));
  return out;
}

bool operator<(const SubjectKey& a, const SubjectKey& b) {
  if (a.entity != b.entity) return a.entity < b.entity;
  if (a.sensor != b.sensor) return a.sensor < b.sensor;
  return static_cast<unsigned>(a.site) < static_cast<unsigned>(b.site);
}

EvidenceStore::EvidenceStore(const Limits& limits) : limits_(limits) {
  configuration_status_ = limits_.validate();
}

ObservationRef EvidenceStore::make_ref(const TemperatureObservation& observation) const {
  ObservationRef ref;
  ref.id = observation.id;
  ref.source = observation.provenance.source;
  ref.celsius = observation.celsius;
  ref.observed_at = observation.observed_at;
  ref.fence_revision = observation.fence.revision;
  ref.authority = observation.provenance.authority;
  ref.site = observation.site;
  ref.quality_supplied = observation.quality.supplied;
  ref.quality_degraded = observation.quality.is_degraded();
  ref.synthetic = observation.provenance.is_synthetic();
  return ref;
}

void EvidenceStore::retire_front(const SubjectKey& key) {
  const auto subject_it = subjects_.find(key);
  if (subject_it == subjects_.end() || subject_it->second.ordered.empty()) {
    return;
  }
  const ObservationId victim = subject_it->second.ordered.front();
  subject_it->second.ordered.erase(subject_it->second.ordered.begin());
  const auto observation_it = observations_.find(victim);
  if (observation_it != observations_.end()) {
    const EntityId entity = observation_it->second.entity;
    const SensorId sensor = observation_it->second.sensor;
    const SourceId source = observation_it->second.provenance.source;
    observations_.erase(observation_it);
    const auto entity_it = sensor_index_.find(entity);
    if (entity_it != sensor_index_.end()) {
      const auto sensor_it = entity_it->second.find(sensor);
      if (sensor_it != entity_it->second.end()) {
        if (sensor_it->second > 0) {
          --sensor_it->second;
        }
        if (sensor_it->second == 0) {
          entity_it->second.erase(sensor_it);
        }
      }
      if (entity_it->second.empty()) {
        sensor_index_.erase(entity_it);
      }
    }
    const auto last_it = last_accepted_.find(source);
    if (last_it != last_accepted_.end() && last_it->second == victim) {
      last_accepted_.erase(last_it);
    }
  }
  if (subject_it->second.ordered.empty()) {
    subjects_.erase(subject_it);
  }
  ++retired_;
}

IngestOutcome EvidenceStore::ingest(const TemperatureObservation& observation) {
  IngestOutcome outcome;
  outcome.id = observation.id;

  if (!configuration_status_.ok()) {
    outcome.kind = IngestKind::kRefused;
    outcome.status = Status::failure(ErrorCode::kRefused,
                                     "store configuration is invalid: " +
                                         configuration_status_.describe());
    ++refused_;
    return outcome;
  }

  TemperatureObservation record = observation;
  if (!record.id.is_set()) {
    record.id = compute_observation_id(record);
    outcome.id = record.id;
  }

  const Status valid = record.validate(limits_);
  if (!valid.ok()) {
    outcome.kind = IngestKind::kRefused;
    outcome.status = valid;
    ++refused_;
    return outcome;
  }

  const ObservationId derived = compute_observation_id(record);
  if (derived != record.id) {
    outcome.kind = IngestKind::kConflict;
    outcome.status = Status::failure(
        ErrorCode::kConflict, "observation identity " + record.id.value() +
                                  " does not match its content, which hashes to " +
                                  derived.value());
    ++conflicts_;
    return outcome;
  }

  if (trackers_.find(record.provenance.source) == trackers_.end() &&
      trackers_.size() >= limits_.max_sources) {
    outcome.kind = IngestKind::kRefused;
    outcome.status = Status::failure(
        ErrorCode::kLimitExceeded,
        "the store already tracks its bound of " + std::to_string(limits_.max_sources) +
            " evidence sources, so source " + record.provenance.source.value() +
            " cannot be admitted");
    ++refused_;
    return outcome;
  }
  FenceTracker& tracker =
      trackers_.emplace(record.provenance.source, FenceTracker(record.provenance.source))
          .first->second;
  if (tracker.has_high_water()) {
    outcome.high_water = tracker.high_water();
  }

  // Re-delivery of a record already held is idempotent regardless of its fence position.
  if (observations_.find(record.id) != observations_.end()) {
    outcome.kind = IngestKind::kDuplicate;
    outcome.status = Status::success();
    outcome.high_water = tracker.has_high_water() ? tracker.high_water() : record.fence;
    ++duplicates_;
    return outcome;
  }

  const auto last_it = last_accepted_.find(record.provenance.source);
  const bool same_content =
      last_it != last_accepted_.end() && last_it->second.is_set() && last_it->second == record.id;

  const FenceVerdict verdict = tracker.classify(record.fence, same_content);
  switch (verdict) {
    case FenceVerdict::kStaleEpoch:
      outcome.kind = IngestKind::kRefused;
      outcome.status = Status::failure(
          ErrorCode::kStaleEpoch, "observation " + record.id.value() + " is fenced at epoch " +
                                      record.fence.epoch.to_string() +
                                      ", older than the recorded epoch " +
                                      tracker.high_water().epoch.to_string());
      ++refused_;
      return outcome;
    case FenceVerdict::kStaleGeneration:
      outcome.kind = IngestKind::kRefused;
      outcome.status = Status::failure(
          ErrorCode::kStaleGeneration,
          "observation " + record.id.value() + " is fenced at generation " +
              record.fence.generation.to_string() + ", older than the recorded generation " +
              tracker.high_water().generation.to_string());
      ++refused_;
      return outcome;
    case FenceVerdict::kReplayRejected:
      outcome.kind = IngestKind::kRefused;
      outcome.status = Status::failure(
          ErrorCode::kReplayRejected, "observation " + record.id.value() +
                                          " replays a position already superseded by " +
                                          tracker.high_water().to_string());
      ++refused_;
      return outcome;
    case FenceVerdict::kConflict:
    case FenceVerdict::kIncomparable:
      outcome.kind = IngestKind::kConflict;
      outcome.status = Status::failure(
          ErrorCode::kConflict, "observation " + record.id.value() +
                                    " claims the position held by " +
                                    tracker.high_water().to_string() + " with different content");
      ++conflicts_;
      return outcome;
    case FenceVerdict::kDuplicate:
      outcome.kind = IngestKind::kDuplicate;
      outcome.status = Status::success();
      ++duplicates_;
      return outcome;
    case FenceVerdict::kAdvance:
    case FenceVerdict::kNewEpoch:
      break;
  }

  const Status capacity = check_capacity(observations_.size(), limits_.max_observations,
                                         "recorded observations");
  if (!capacity.ok()) {
    outcome.kind = IngestKind::kRefused;
    outcome.status = capacity;
    ++refused_;
    return outcome;
  }

  SubjectKey key;
  key.entity = record.entity;
  key.sensor = record.sensor;
  key.site = record.site;
  auto subject_it = subjects_.find(key);
  if (subject_it == subjects_.end()) {
    subject_it = subjects_.emplace(key, SubjectEntry{}).first;
  }

  const Status subject_capacity = check_capacity(
      subject_it->second.ordered.size(), limits_.max_observations_per_subject,
      "observations for one subject");
  if (!subject_capacity.ok()) {
    // The per-subject ring is full: retire the oldest observation so that a long-running source
    // cannot exhaust memory. Retirement is counted and reported, never silent.
    retire_front(key);
    subject_it = subjects_.find(key);
    if (subject_it == subjects_.end()) {
      outcome.kind = IngestKind::kRefused;
      outcome.status = subject_capacity;
      ++refused_;
      return outcome;
    }
  }

  const Status observe_status = tracker.observe(record.fence, same_content);
  if (!observe_status.ok()) {
    outcome.kind = IngestKind::kRefused;
    outcome.status = observe_status;
    ++refused_;
    return outcome;
  }

  const EntityId entity = record.entity;
  const SensorId sensor = record.sensor;
  const SourceId source = record.provenance.source;
  const ObservationId new_id = record.id;
  const Timestamp observed_at = record.observed_at;
  observations_.emplace(new_id, std::move(record));
  ++sensor_index_[entity][sensor];

  SubjectEntry& entry = subject_it->second;
  // Positions the new observation in ascending (observed_at, id) order. The comparator asks
  // "does the probe precede this element", which is the contract std::upper_bound expects.
  const auto position = std::upper_bound(
      entry.ordered.begin(), entry.ordered.end(), new_id,
      [this, &observed_at](const ObservationId& probe, const ObservationId& element) {
        const auto element_it = observations_.find(element);
        const Timestamp element_at =
            element_it == observations_.end() ? Timestamp{} : element_it->second.observed_at;
        if (element_at != observed_at) {
          return observed_at < element_at;
        }
        return probe < element;
      });
  entry.ordered.insert(position, new_id);

  last_accepted_[source] = new_id;
  outcome.kind = IngestKind::kRecorded;
  outcome.status = Status::success();
  outcome.high_water = tracker.high_water();
  return outcome;
}

const TemperatureObservation* EvidenceStore::find(const ObservationId& id) const {
  const auto it = observations_.find(id);
  return it == observations_.end() ? nullptr : &it->second;
}

bool EvidenceStore::has_observation(const ObservationId& id) const {
  return observations_.find(id) != observations_.end();
}

std::size_t EvidenceStore::sensor_count() const {
  std::vector<SensorId> all;
  for (const auto& entry : sensor_index_) {
    for (const auto& sensor : entry.second) {
      all.push_back(sensor.first);
    }
  }
  std::sort(all.begin(), all.end());
  all.erase(std::unique(all.begin(), all.end()), all.end());
  return all.size();
}

std::vector<EntityId> EvidenceStore::entities() const {
  std::vector<EntityId> out;
  out.reserve(sensor_index_.size());
  for (const auto& entry : sensor_index_) {
    out.push_back(entry.first);
  }
  return out;
}

std::vector<SensorId> EvidenceStore::sensors() const {
  std::vector<SensorId> out;
  for (const auto& entry : sensor_index_) {
    for (const auto& sensor : entry.second) {
      out.push_back(sensor.first);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<ObservationId> EvidenceStore::observation_ids() const {
  std::vector<ObservationId> out;
  out.reserve(observations_.size());
  for (const auto& entry : observations_) {
    out.push_back(entry.first);
  }
  return out;
}

bool EvidenceStore::would_retire(const EntityId& entity, const SensorId& sensor,
                                 MeasurementSite site) const {
  SubjectKey key;
  key.entity = entity;
  key.sensor = sensor;
  key.site = site;
  const auto it = subjects_.find(key);
  if (it == subjects_.end()) {
    return false;
  }
  return it->second.ordered.size() >= limits_.max_observations_per_subject;
}

std::vector<ObservationId> EvidenceStore::observations_for(const EntityId& entity) const {
  std::vector<ObservationId> out;
  for (const SubjectKey& key : subjects_for(entity)) {
    const auto it = subjects_.find(key);
    if (it == subjects_.end()) {
      continue;
    }
    out.insert(out.end(), it->second.ordered.begin(), it->second.ordered.end());
  }
  std::sort(out.begin(), out.end(), [this](const ObservationId& a, const ObservationId& b) {
    const auto a_it = observations_.find(a);
    const auto b_it = observations_.find(b);
    const Timestamp a_at = a_it == observations_.end() ? Timestamp{} : a_it->second.observed_at;
    const Timestamp b_at = b_it == observations_.end() ? Timestamp{} : b_it->second.observed_at;
    if (a_at != b_at) return a_at < b_at;
    return a < b;
  });
  return out;
}

std::vector<SensorId> EvidenceStore::sensors_for(const EntityId& entity) const {
  std::vector<SensorId> out;
  const auto it = sensor_index_.find(entity);
  if (it == sensor_index_.end()) {
    return out;
  }
  out.reserve(it->second.size());
  for (const auto& entry : it->second) {
    out.push_back(entry.first);
  }
  return out;
}

std::vector<SubjectKey> EvidenceStore::subjects_for(const EntityId& entity) const {
  std::vector<SubjectKey> out;
  SubjectKey probe;
  probe.entity = entity;
  auto it = subjects_.lower_bound(probe);
  for (; it != subjects_.end() && it->first.entity == entity; ++it) {
    out.push_back(it->first);
  }
  return out;
}

const Fence* EvidenceStore::high_water_for(const SourceId& source) const {
  const auto it = trackers_.find(source);
  if (it == trackers_.end() || !it->second.has_high_water()) {
    return nullptr;
  }
  return &it->second.high_water();
}


std::vector<Fence> EvidenceStore::high_water_marks() const {
  std::vector<Fence> out;
  for (const auto& entry : trackers_) {
    if (entry.second.has_high_water()) {
      out.push_back(entry.second.high_water());
    }
  }
  std::sort(out.begin(), out.end(), [](const Fence& a, const Fence& b) {
    return a.source < b.source;
  });
  return out;
}

Status EvidenceStore::restore_high_water(const Fence& fence) {
  FenceTracker& tracker =
      trackers_.emplace(fence.source, FenceTracker(fence.source)).first->second;
  return tracker.force_high_water(fence);
}

Result<SubjectTemperature> EvidenceStore::resolve(const EntityId& entity, const SensorId& sensor,
                                                  MeasurementSite site,
                                                  const Timestamp& evaluated_at,
                                                  const FreshnessPolicy& policy) const {
  const Status policy_status = policy.validate();
  if (!policy_status.ok()) {
    return policy_status;
  }
  if (!evaluated_at.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "resolution requires a set evaluation instant");
  }

  SubjectTemperature out;
  out.entity = entity;
  out.sensor = sensor;
  out.site = site;

  SubjectKey key;
  key.entity = entity;
  key.sensor = sensor;
  key.site = site;
  const auto subject_it = subjects_.find(key);
  if (subject_it == subjects_.end() || subject_it->second.ordered.empty()) {
    out.state = EvidenceState::kUnknown;
    out.reason = "no observation has ever been recorded for entity " + entity.value() +
                 ", sensor " + sensor.value() + ", site " + std::string(to_string(site));
    return out;
  }

  std::vector<ObservationRef> fresh;
  for (const ObservationId& id : subject_it->second.ordered) {
    const auto observation_it = observations_.find(id);
    if (observation_it == observations_.end()) {
      continue;
    }
    const TemperatureObservation& observation = observation_it->second;
    ObservationRef ref = make_ref(observation);
    if (!observation.observed_at.is_set()) {
      out.indeterminate.push_back(ref);
      continue;
    }
    const Duration age = age_of(observation.observed_at, evaluated_at);
    if (age.nanos() < -policy.max_clock_skew.nanos()) {
      out.indeterminate.push_back(ref);
      continue;
    }
    if (age.nanos() > policy.window.nanos()) {
      out.stale.push_back(ref);
      continue;
    }
    fresh.push_back(ref);
  }

  if (fresh.empty()) {
    if (!out.stale.empty()) {
      out.state = EvidenceState::kStale;
      const ObservationRef& newest = *std::max_element(
          out.stale.begin(), out.stale.end(), [](const ObservationRef& a, const ObservationRef& b) {
            if (a.observed_at != b.observed_at) return a.observed_at < b.observed_at;
            if (a.fence_revision != b.fence_revision) return a.fence_revision < b.fence_revision;
            return a.id < b.id;
          });
      out.representative_celsius = newest.celsius;
      out.representative_at = newest.observed_at;
      out.representative_id = newest.id;
      out.representative_authority = newest.authority;
      out.age = age_of(newest.observed_at, evaluated_at);
      out.reason = "every observation of entity " + entity.value() + ", sensor " +
                   sensor.value() + ", site " + std::string(to_string(site)) +
                   " is older than the freshness window of " + format_seconds(policy.window) +
                   "; the newest is " + format_seconds(out.age) + " old";
    } else if (!out.indeterminate.empty()) {
      out.state = EvidenceState::kIndeterminate;
      out.reason = "every observation of entity " + entity.value() + ", sensor " +
                   sensor.value() + ", site " + std::string(to_string(site)) +
                   " is timestamped further into the future than the tolerated clock skew of " +
                   format_seconds(policy.max_clock_skew);
    } else {
      out.state = EvidenceState::kIndeterminate;
      out.reason = "observations exist for entity " + entity.value() + ", sensor " +
                   sensor.value() + " but none carries a usable observation timestamp";
    }
    sort_refs_by_id(out.stale);
    sort_refs_by_id(out.conflicting);
    sort_refs_by_id(out.indeterminate);
    return out;
  }

  // Newest first: how the value was obtained, then the observation instant, then the source's own
  // fencing order, and finally the identity so that the order is total and reproducible.
  std::sort(fresh.begin(), fresh.end(), [](const ObservationRef& a, const ObservationRef& b) {
    const int rank_a = authority_rank(a.authority);
    const int rank_b = authority_rank(b.authority);
    if (rank_a != rank_b) return rank_a > rank_b;
    if (a.observed_at != b.observed_at) return a.observed_at > b.observed_at;
    if (a.fence_revision != b.fence_revision) return a.fence_revision > b.fence_revision;
    return a.id < b.id;
  });

  // Supersession. Within one source the newest reading of a subject replaces that source's earlier
  // readings; only readings from different sources can disagree with one another. Without this a
  // single sensor ramping from 30 C to 41 C would be reported as two sensors in conflict.
  std::map<SourceId, ObservationRef> newest_per_source;
  std::vector<ObservationRef> candidates;
  for (const ObservationRef& ref : fresh) {
    const auto inserted = newest_per_source.emplace(ref.source, ref);
    if (!inserted.second) {
      out.superseded.push_back(ref);
      continue;
    }
    candidates.push_back(ref);
  }

  const ObservationRef representative = candidates.front();
  out.representative_celsius = representative.celsius;
  out.representative_at = representative.observed_at;
  out.representative_id = representative.id;
  out.representative_authority = representative.authority;
  out.representative_synthetic = representative.synthetic;
  out.representative_quality_supplied = representative.quality_supplied;
  out.representative_quality_degraded = representative.quality_degraded;
  out.age = age_of(representative.observed_at, evaluated_at);

  double lowest = candidates.front().celsius;
  double highest = candidates.front().celsius;
  for (const ObservationRef& ref : candidates) {
    lowest = std::min(lowest, ref.celsius);
    highest = std::max(highest, ref.celsius);
  }
  const double spread = highest - lowest;

  if (spread <= policy.agreement_tolerance_c) {
    out.state = EvidenceState::kFresh;
    out.supporting = candidates;
    out.reason = std::to_string(candidates.size()) + " source(s) report entity " + entity.value() +
                 ", sensor " + sensor.value() + " within " +
                 format_real(policy.agreement_tolerance_c) + " C (spread " + format_real(spread) +
                 " C); the representative is " + representative.id.value() + " with authority " +
                 std::string(to_string(representative.authority));
    if (!out.superseded.empty()) {
      out.reason += "; " + std::to_string(out.superseded.size()) +
                    " earlier reading(s) from the same source(s) were superseded";
    }
  } else {
    out.state = EvidenceState::kConflicting;
    for (const ObservationRef& ref : candidates) {
      if (ref.celsius >= representative.celsius - policy.agreement_tolerance_c &&
          ref.celsius <= representative.celsius + policy.agreement_tolerance_c) {
        out.supporting.push_back(ref);
      } else {
        out.conflicting.push_back(ref);
      }
    }
    out.reason = std::to_string(candidates.size()) + " source(s) disagree about entity " +
                 entity.value() + ", sensor " + sensor.value() + ": they span " + format_real(spread) +
                 " C, which exceeds the agreement tolerance of " +
                 format_real(policy.agreement_tolerance_c) + " C; the value is not resolved";
  }

  sort_refs_by_id(out.supporting);
  sort_refs_by_id(out.conflicting);
  sort_refs_by_id(out.stale);
  sort_refs_by_id(out.indeterminate);
  sort_refs_by_id(out.superseded);
  return out;
}

Result<std::vector<SubjectTemperature>> EvidenceStore::resolve_entity(
    const EntityId& entity, const Timestamp& evaluated_at, const FreshnessPolicy& policy) const {
  std::vector<SubjectTemperature> out;
  for (const SubjectKey& key : subjects_for(entity)) {
    Result<SubjectTemperature> resolved = resolve(key.entity, key.sensor, key.site, evaluated_at, policy);
    if (!resolved.ok()) {
      return resolved.error();
    }
    out.push_back(resolved.value());
  }
  return out;
}

Result<std::vector<SubjectTemperature>> EvidenceStore::resolve_all(
    const Timestamp& evaluated_at, const FreshnessPolicy& policy) const {
  std::vector<SubjectTemperature> out;
  out.reserve(subjects_.size());
  for (const auto& entry : subjects_) {
    Result<SubjectTemperature> resolved =
        resolve(entry.first.entity, entry.first.sensor, entry.first.site, evaluated_at, policy);
    if (!resolved.ok()) {
      return resolved.error();
    }
    out.push_back(resolved.value());
  }
  return out;
}

Result<std::vector<SubjectTemperature>> EvidenceStore::resolve_usable(
    const Timestamp& evaluated_at, const FreshnessPolicy& policy) const {
  Result<std::vector<SubjectTemperature>> all = resolve_all(evaluated_at, policy);
  if (!all.ok()) {
    return all.error();
  }
  std::vector<SubjectTemperature> out;
  for (SubjectTemperature& subject : all.value()) {
    if (subject.is_usable()) {
      out.push_back(subject);
    }
  }
  return out;
}

std::vector<TemperatureObservation> EvidenceStore::observations() const {
  std::vector<TemperatureObservation> out;
  out.reserve(observations_.size());
  for (const auto& entry : observations_) {
    out.push_back(entry.second);
  }
  return out;
}

}  // namespace tobsv