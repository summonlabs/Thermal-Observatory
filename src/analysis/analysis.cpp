// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/analysis/analysis.hpp"

#include <algorithm>
#include <set>

#include "tobsv/core/format.hpp"
#include "tobsv/core/hash.hpp"

namespace tobsv {
namespace {

int level_rank(ThresholdLevel level) { return static_cast<int>(static_cast<unsigned>(level)); }

JsonValue headroom_array(const std::vector<HeadroomReport>& reports) {
  JsonValue out = JsonValue::array();
  for (const HeadroomReport& report : reports) {
    out.push(report.to_json());
  }
  return out;
}

JsonValue transition_array(const std::vector<ThresholdTransitionRecord>& records) {
  JsonValue out = JsonValue::array();
  for (const ThresholdTransitionRecord& record : records) {
    out.push(record.to_json());
  }
  return out;
}

JsonValue derating_array(const std::vector<DeratingAppraisal>& appraisals) {
  JsonValue out = JsonValue::array();
  for (const DeratingAppraisal& appraisal : appraisals) {
    out.push(appraisal.to_json());
  }
  return out;
}

}  // namespace

bool TransitionMemory::lookup(const SubjectKey& key, ThresholdLevel& out) const {
  const auto it = levels_.find(key);
  if (it == levels_.end()) {
    return false;
  }
  out = it->second;
  return true;
}

void TransitionMemory::record(const SubjectKey& key, ThresholdLevel level) {
  levels_.insert_or_assign(key, level);
}

void TransitionMemory::retain(const std::vector<SubjectKey>& live, std::size_t bound) {
  if (levels_.size() <= bound) {
    return;
  }
  std::vector<SubjectKey> sorted = live;
  std::sort(sorted.begin(), sorted.end(), [](const SubjectKey& a, const SubjectKey& b) {
    return a < b;
  });
  for (auto it = levels_.begin(); it != levels_.end();) {
    if (std::binary_search(sorted.begin(), sorted.end(), it->first)) {
      ++it;
      continue;
    }
    it = levels_.erase(it);
  }
}

Status ThermalQuery::validate(const Limits& limits) const {
  if (!evaluated_at.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "the query has no evaluation instant");
  }
  const Status freshness_status = freshness.validate();
  if (!freshness_status.ok()) {
    return freshness_status;
  }
  const Status hotspots_status = hotspots.validate(limits);
  if (!hotspots_status.ok()) {
    return hotspots_status;
  }
  const Status propagation_status = propagation.validate(limits);
  if (!propagation_status.ok()) {
    return propagation_status;
  }
  if (focus == ThresholdLevel::kNominal) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "a focus band of nominal would report every subject");
  }
  if (propagation_targets.size() > limits.max_entities) {
    return Status::failure(ErrorCode::kLimitExceeded,
                           "the query names more propagation targets than the entity bound");
  }
  return Status::success();
}

JsonValue ThermalAnalysis::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("digest", JsonValue::text(digest));
  out.set("evaluated_at", JsonValue::text(evaluated_at.to_string()));
  out.set("state", JsonValue::text(std::string(to_string(state))));
  out.set("subject_count", JsonValue::integer(static_cast<std::int64_t>(subject_count)));
  out.set("usable_subject_count", JsonValue::integer(static_cast<std::int64_t>(usable_subject_count)));
  out.set("conflicted_subject_count",
          JsonValue::integer(static_cast<std::int64_t>(conflicted_subject_count)));
  out.set("stale_subject_count", JsonValue::integer(static_cast<std::int64_t>(stale_subject_count)));
  out.set("unknown_subject_count",
          JsonValue::integer(static_cast<std::int64_t>(unknown_subject_count)));
  out.set("headroom", headroom_array(headroom));
  out.set("transitions", transition_array(transitions));
  out.set("hotspots", hotspots.to_json());
  out.set("propagation", propagation.to_json());
  out.set("derating", derating_array(deratings));
  out.set("attribution", attribution.to_json());
  JsonValue steps = JsonValue::array();
  for (const std::string& step : reason_steps) {
    steps.push(JsonValue::text(step));
  }
  out.set("reason_steps", steps);
  return out;
}

Result<ThermalAnalysis> analyze(const ThermalQuery& query, const EvidenceStore& store,
                                const ThermalInventory& inventory,
                                const EnvelopeRegistry& envelopes, const ThermalTopology& topology,
                                const CouplingGraph& coupling, const DeratingRegistry& derating,
                                TransitionMemory& memory, const Limits& limits) {
  const Status query_status = query.validate(limits);
  if (!query_status.ok()) {
    return query_status;
  }

  ThermalAnalysis analysis;
  analysis.evaluated_at = query.evaluated_at;
  analysis.reason_steps.push_back(
      "evaluated at " + query.evaluated_at.to_string() + " with a freshness window of " +
      format_seconds(query.freshness.window) + " and an agreement tolerance of " +
      format_real(query.freshness.agreement_tolerance_c) + " C");

  Result<std::vector<SubjectTemperature>> resolved =
      store.resolve_all(query.evaluated_at, query.freshness);
  if (!resolved.ok()) {
    return resolved.error();
  }
  analysis.subject_count = resolved.value().size();

  // Clock domains are collected per subject so that a subject reported from two clocks can be
  // flagged instead of silently ordered.
  std::map<SubjectKey, std::set<ClockDomain>> clocks;
  for (const TemperatureObservation& observation : store.observations()) {
    SubjectKey key;
    key.entity = observation.entity;
    key.sensor = observation.sensor;
    key.site = observation.site;
    clocks[key].insert(observation.provenance.clock);
  }

  for (const SubjectTemperature& subject : resolved.value()) {
    const EntityClass entity_class = inventory.classify(subject.entity);
    const bool registered = inventory.find(subject.entity) != nullptr;
    Result<HeadroomReport> report =
        compute_headroom(subject, entity_class, envelopes, limits);
    if (!report.ok()) {
      return report.error();
    }
    analysis.headroom.push_back(report.value());

    switch (subject.state) {
      case EvidenceState::kFresh: ++analysis.usable_subject_count; break;
      case EvidenceState::kConflicting: ++analysis.conflicted_subject_count; break;
      case EvidenceState::kStale: ++analysis.stale_subject_count; break;
      case EvidenceState::kUnknown: ++analysis.unknown_subject_count; break;
      default: break;
    }

    if (!registered) {
      const Status added = analysis.attribution.add(
          AttributionLimitCode::kUnregisteredEntity, subject.entity,
          "entity " + subject.entity.value() +
              " is observed but not inventoried, so no entity class is available and only "
              "class-agnostic envelopes can apply",
          limits);
      (void)added;
    }
    switch (subject.state) {
      case EvidenceState::kUnknown:
        (void)analysis.attribution.add(AttributionLimitCode::kNoEvidence, subject.entity,
                                       "no observation has ever been recorded for entity " +
                                           subject.entity.value() + ", sensor " +
                                           subject.sensor.value(),
                                       limits);
        break;
      case EvidenceState::kStale:
        (void)analysis.attribution.add(AttributionLimitCode::kStaleEvidence, subject.entity,
                                       "the freshest observation of entity " +
                                           subject.entity.value() + ", sensor " +
                                           subject.sensor.value() +
                                           " is older than the freshness window",
                                       limits);
        break;
      case EvidenceState::kConflicting:
        (void)analysis.attribution.add(AttributionLimitCode::kConflictingSensors, subject.entity,
                                       "fresh observations of entity " + subject.entity.value() +
                                           ", sensor " + subject.sensor.value() +
                                           " disagree by more than the agreement tolerance",
                                       limits);
        break;
      case EvidenceState::kIndeterminate:
        (void)analysis.attribution.add(AttributionLimitCode::kIndeterminateTimeline,
                                       subject.entity,
                                       "observations of entity " + subject.entity.value() +
                                           ", sensor " + subject.sensor.value() +
                                           " cannot be placed on the evaluation timeline",
                                       limits);
        break;
      default: break;
    }
    if (report.value().state == EvidenceState::kUnsupported) {
      (void)analysis.attribution.add(
          AttributionLimitCode::kNoEnvelope, subject.entity,
          "no envelope applies to entity " + subject.entity.value() + " at site " +
              std::string(to_string(subject.site)) + ", so no headroom is defined",
          limits);
    }
    if (subject.state == EvidenceState::kFresh) {
      if (!subject.representative_quality_supplied) {
        (void)analysis.attribution.add(
            AttributionLimitCode::kQualityUnsupplied, subject.entity,
            "the source of the representative observation of entity " + subject.entity.value() +
                " published no quality statement, so the value is taken as reported",
            limits);
      } else if (subject.representative_quality_degraded) {
        (void)analysis.attribution.add(
            AttributionLimitCode::kQualityDegraded, subject.entity,
            "the source declared the representative observation of entity " +
                subject.entity.value() + " degraded",
            limits);
      }
      if (subject.representative_synthetic) {
        (void)analysis.attribution.add(
            AttributionLimitCode::kSyntheticSource, subject.entity,
            "the representative observation of entity " + subject.entity.value() +
                " came from a synthetic generator and is evidence about a generator, not hardware",
            limits);
      }
      if (subject.representative_authority == AuthorityLevel::kDerived ||
          subject.representative_authority == AuthorityLevel::kModeled) {
        (void)analysis.attribution.add(
            AttributionLimitCode::kDerivedAuthority, subject.entity,
            "the representative observation of entity " + subject.entity.value() +
                " has authority " + std::string(to_string(subject.representative_authority)) +
                " rather than measured",
            limits);
      }
    }

    SubjectKey key;
    key.entity = subject.entity;
    key.sensor = subject.sensor;
    key.site = subject.site;
    const auto clock_it = clocks.find(key);
    if (clock_it != clocks.end() && clock_it->second.size() > 1) {
      std::vector<std::string> names;
      for (const ClockDomain domain : clock_it->second) {
        names.emplace_back(to_string(domain));
      }
      (void)analysis.attribution.add(
          AttributionLimitCode::kClockDomainMismatch, subject.entity,
          "observations of entity " + subject.entity.value() + ", sensor " +
              subject.sensor.value() + " arrive from several clock domains (" +
              join(names, ",") + "), so their ordering is not certain",
          limits);
    }
  }

  // Threshold transitions against the previous evaluation of each subject.
  for (const HeadroomReport& report : analysis.headroom) {
    SubjectKey key;
    key.entity = report.entity;
    key.sensor = report.sensor;
    key.site = report.site;
    ThresholdLevel previous = ThresholdLevel::kNominal;
    const bool have_previous = memory.lookup(key, previous);
    ThresholdTransitionRecord record =
        classify_transition(have_previous, previous, report, query.evaluated_at);
    analysis.transitions.push_back(record);
    if (report.state == EvidenceState::kFresh && report.current_level_known) {
      memory.record(key, report.current_level);
    }
  }
  // Subjects that no longer appear are forgotten once the memory is over its bound, so the map
  // cannot grow with the history of everything the runtime has ever seen.
  std::vector<SubjectKey> live_subjects;
  live_subjects.reserve(analysis.headroom.size());
  for (const HeadroomReport& report : analysis.headroom) {
    SubjectKey key;
    key.entity = report.entity;
    key.sensor = report.sensor;
    key.site = report.site;
    live_subjects.push_back(key);
  }
  memory.retain(live_subjects, limits.max_observations);

  Result<HotspotResult> hotspots =
      group_hotspots(analysis.headroom, topology, coupling, query.hotspots, limits);
  if (!hotspots.ok()) {
    return hotspots.error();
  }
  analysis.hotspots = hotspots.value();
  for (const HotspotEpisode& episode : analysis.hotspots.episodes) {
    if (episode.adjacency_links > 0 && episode.supported_links == 0) {
      (void)analysis.attribution.add(
          AttributionLimitCode::kJointEnclosureAmbiguity, episode.seed,
          "episode " + episode.id.value() +
              " is joined only by declared adjacency, so which member is the source of the heat "
              "cannot be attributed from thermal evidence alone",
          limits);
    }
  }
  for (const EntityId& entity : analysis.hotspots.ungrouped) {
    (void)analysis.attribution.add(AttributionLimitCode::kUngroupedHotspot, entity,
                                   "entity " + entity.value() +
                                       " is hot but shares no traversable coupling or declared "
                                       "adjacency with another hot entity",
                                   limits);
  }

  // Propagation is traced from the hottest subject outwards. One origin keeps the search bounded
  // and the explanation short; the choice is recorded in the reason steps.
  if (query.include_propagation) {
    EntityId origin;
    double peak = 0.0;
    std::vector<EntityId> hot_entities;
    for (const HotspotEpisode& episode : analysis.hotspots.episodes) {
      for (const HotspotMember& member : episode.members) {
        hot_entities.push_back(member.entity);
        if (!origin.is_set() || member.celsius > peak ||
            (member.celsius == peak && member.entity < origin)) {
          origin = member.entity;
          peak = member.celsius;
        }
      }
    }
    for (const EntityId& entity : analysis.hotspots.ungrouped) {
      hot_entities.push_back(entity);
      const HeadroomReport* report = nullptr;
      for (const HeadroomReport& candidate : analysis.headroom) {
        if (candidate.entity == entity && candidate.state == EvidenceState::kFresh) {
          report = &candidate;
          break;
        }
      }
      if (report != nullptr &&
          (!origin.is_set() || report->observed_celsius > peak ||
           (report->observed_celsius == peak && entity < origin))) {
        origin = entity;
        peak = report->observed_celsius;
      }
    }
    std::sort(hot_entities.begin(), hot_entities.end());
    hot_entities.erase(std::unique(hot_entities.begin(), hot_entities.end()), hot_entities.end());

    if (!origin.is_set()) {
      analysis.propagation.origin = EntityId{};
      analysis.propagation.state = EvidenceState::kUnknown;
      analysis.propagation.reason =
          "no hot entity was found, so no propagation origin could be established";
    } else {
      std::vector<EntityId> targets = query.propagation_targets;
      if (targets.empty()) {
        for (const EntityId& entity : hot_entities) {
          if (entity != origin) {
            targets.push_back(entity);
          }
        }
      }
      Result<PropagationResult> propagation =
          find_propagation(origin, targets, coupling, query.propagation, limits);
      if (!propagation.ok()) {
        return propagation.error();
      }
      analysis.propagation = propagation.value();
      analysis.reason_steps.push_back(
          "propagation was traced from " + origin.value() + " (peak " + format_real(peak) +
          " C) to " + std::to_string(targets.size()) + " target(s) with depth " +
          std::to_string(query.propagation.max_depth));
      if (analysis.propagation.depth_limited) {
        (void)analysis.attribution.add(AttributionLimitCode::kDepthLimited, origin,
                                       "the propagation search from " + origin.value() +
                                           " stopped at the configured depth of " +
                                           std::to_string(query.propagation.max_depth),
                                       limits);
      }
      if (analysis.propagation.search_bounded) {
        (void)analysis.attribution.add(AttributionLimitCode::kSearchBounded, origin,
                                       "the propagation search from " + origin.value() +
                                           " exhausted its exploration budget",
                                       limits);
      }
    }
  } else {
    analysis.propagation.state = EvidenceState::kUnsupported;
    analysis.propagation.reason = "propagation was not requested";
  }

  // Derating appraisal for every entity that carries a claim.
  std::set<EntityId> derating_entities;
  for (const EntityId& entity : inventory.entities()) {
    derating_entities.insert(entity);
  }
  for (const EntityId& entity : store.entities()) {
    derating_entities.insert(entity);
  }
  for (const EntityId& entity : derating_entities) {
    Result<DeratingAppraisal> appraisal =
        appraise_derating(entity, derating, store, inventory, envelopes, query.evaluated_at,
                          query.freshness, limits);
    if (!appraisal.ok()) {
      return appraisal.error();
    }
    if (appraisal.value().entries.empty()) {
      continue;
    }
    analysis.deratings.push_back(appraisal.value());
    if (appraisal.value().state != EvidenceState::kFresh) {
      (void)analysis.attribution.add(
          AttributionLimitCode::kPartialDerating, entity,
          "derating claims against " + entity.value() + " are " +
              std::string(to_string(appraisal.value().state)) +
              ", so the derating explanation is not fully supported",
          limits);
    }
  }

  if (coupling.coincidental_count() > 0) {
    (void)analysis.attribution.add(
        AttributionLimitCode::kCoincidentalCoupling, EntityId{},
        std::to_string(coupling.coincidental_count()) +
            " relation(s) record temporal coincidence only and are never traversed as propagation",
        limits);
  }
  if (store.retired_count() > 0) {
    (void)analysis.attribution.add(
        AttributionLimitCode::kRetiredEvidence, EntityId{},
        std::to_string(store.retired_count()) +
            " observation(s) were retired by the per-subject retention bound and are no longer "
            "available as citations",
        limits);
  }

  const std::size_t usable = analysis.usable_subject_count;
  if (analysis.subject_count == 0) {
    analysis.state = EvidenceState::kUnknown;
  } else if (analysis.conflicted_subject_count > 0) {
    analysis.state = EvidenceState::kConflicting;
  } else if (usable > 0) {
    analysis.state = EvidenceState::kFresh;
  } else if (analysis.stale_subject_count > 0) {
    analysis.state = EvidenceState::kStale;
  } else {
    analysis.state = EvidenceState::kUnsupported;
  }

  analysis.reason_steps.push_back(
      std::to_string(analysis.subject_count) + " subject(s) resolved: " +
      std::to_string(usable) + " usable, " +
      std::to_string(analysis.conflicted_subject_count) + " conflicting, " +
      std::to_string(analysis.stale_subject_count) + " stale, " +
      std::to_string(analysis.unknown_subject_count) + " unknown");
  analysis.reason_steps.push_back(analysis.hotspots.reason);
  analysis.reason_steps.push_back(analysis.propagation.reason);
  for (const DeratingAppraisal& appraisal : analysis.deratings) {
    analysis.reason_steps.push_back(appraisal.reason);
  }
  analysis.reason_steps.push_back(
      std::to_string(analysis.attribution.size()) +
      " attribution limit(s) qualify this analysis, " +
      std::to_string(analysis.attribution.dropped()) + " dropped at the configured bound");

  // The digest covers the analysis document with the digest field left empty; a verifier blanks the
  // field and re-hashes. This is a content fingerprint, not a signature.
  Result<std::string> encoded = analysis.to_json().dump_compact();
  if (!encoded.ok()) {
    return encoded.error();
  }
  analysis.digest = "an-" + hex_u64(stable_hash(encoded.value()));
  return analysis;
}

}  // namespace tobsv
