// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/hotspot/hotspot.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "tobsv/core/format.hpp"
#include "tobsv/core/hash.hpp"

namespace tobsv {
namespace {

int level_rank(ThresholdLevel level) { return static_cast<int>(static_cast<unsigned>(level)); }

// Disjoint set over the hot entities. The representative is always the smallest identity so that
// the grouping does not depend on insertion order.
class UnionFind {
 public:
  void add(const EntityId& entity) {
    if (parent_.find(entity) == parent_.end()) {
      parent_.emplace(entity, entity);
    }
  }

  EntityId find(const EntityId& entity) {
    auto it = parent_.find(entity);
    if (it == parent_.end()) {
      parent_.emplace(entity, entity);
      return entity;
    }
    if (it->second == entity) {
      return entity;
    }
    const EntityId root = find(it->second);
    it = parent_.find(entity);
    it->second = root;
    return root;
  }

  void unite(const EntityId& a, const EntityId& b) {
    const EntityId root_a = find(a);
    const EntityId root_b = find(b);
    if (root_a == root_b) {
      return;
    }
    if (root_b < root_a) {
      parent_[root_a] = root_b;
    } else {
      parent_[root_b] = root_a;
    }
  }

 private:
  std::map<EntityId, EntityId> parent_;
};

// Distinct links joining one unordered pair of entities. Counting distinct relations rather than
// traversals matters because a symmetric relation is reachable from both of its endpoints.
struct LinkCounts {
  std::set<CouplingId> supported;
  bool adjacency_declared = false;
  std::vector<CouplingId> relations;
};

}  // namespace

Status HotspotPolicy::validate(const Limits& limits) const {
  if (threshold == ThresholdLevel::kNominal) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "a hotspot threshold of nominal would make every entity hot");
  }
  if (episode_gap.nanos() <= 0) {
    return Status::failure(ErrorCode::kInvalidArgument, "episode gap must be positive");
  }
  if (episode_gap.nanos() > limits.default_episode_gap * 100) {
    return Status::failure(ErrorCode::kOutOfRange, "episode gap is implausibly large");
  }
  return Status::success();
}

JsonValue HotspotMember::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("entity", JsonValue::text(entity.value()));
  out.set("sensor", JsonValue::text(sensor.value()));
  out.set("site", JsonValue::text(std::string(to_string(site))));
  out.set("celsius", JsonValue::real(celsius));
  out.set("band", JsonValue::text(std::string(to_string(level))));
  out.set("excursion_celsius", JsonValue::real(excursion_c));
  out.set("evidence", JsonValue::text(evidence.value()));
  out.set("observed_at", JsonValue::text(observed_at.to_string()));
  out.set("synthetic", JsonValue::boolean(synthetic));
  out.set("quality_degraded", JsonValue::boolean(quality_degraded));
  return out;
}

JsonValue HotspotEpisode::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("episode", JsonValue::text(id.value()));
  out.set("threshold", JsonValue::text(std::string(to_string(threshold))));
  out.set("seed", JsonValue::text(seed.value()));
  out.set("peak_celsius", JsonValue::real(peak_c));
  out.set("member_count", JsonValue::integer(static_cast<std::int64_t>(members.size())));
  out.set("observed_at", JsonValue::text(observed_at.to_string()));
  out.set("earliest_at", JsonValue::text(earliest_at.to_string()));
  out.set("supported_links", JsonValue::integer(static_cast<std::int64_t>(supported_links)));
  out.set("adjacency_links", JsonValue::integer(static_cast<std::int64_t>(adjacency_links)));
  JsonValue relations = JsonValue::array();
  for (const CouplingId& relation : support_relations) {
    relations.push(JsonValue::text(relation.value()));
  }
  out.set("support_relations", relations);
  JsonValue member_array = JsonValue::array();
  for (const HotspotMember& member : members) {
    member_array.push(member.to_json());
  }
  out.set("members", member_array);
  out.set("reason", JsonValue::text(reason));
  return out;
}

JsonValue HotspotResult::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("state", JsonValue::text(std::string(to_string(state))));
  out.set("evaluable_subjects", JsonValue::integer(static_cast<std::int64_t>(evaluable_subjects)));
  out.set("hot_subjects", JsonValue::integer(static_cast<std::int64_t>(hot_subjects)));
  out.set("dropped_episodes", JsonValue::integer(static_cast<std::int64_t>(dropped_episodes)));
  JsonValue ungrouped_array = JsonValue::array();
  for (const EntityId& entity : ungrouped) {
    ungrouped_array.push(JsonValue::text(entity.value()));
  }
  out.set("ungrouped", ungrouped_array);
  JsonValue episode_array = JsonValue::array();
  for (const HotspotEpisode& episode : episodes) {
    episode_array.push(episode.to_json());
  }
  out.set("episodes", episode_array);
  out.set("reason", JsonValue::text(reason));
  return out;
}

EpisodeId compute_episode_id(const std::vector<HotspotMember>& members, ThresholdLevel threshold) {
  StableDigest digest;
  digest.absorb_text("tobsv.hotspot.v1");
  digest.absorb_number(static_cast<std::uint64_t>(threshold));
  std::vector<std::string> keys;
  keys.reserve(members.size());
  for (const HotspotMember& member : members) {
    keys.push_back(member.entity.value() + "|" + member.sensor.value() + "|" +
                   std::string(to_string(member.site)) + "|" + member.evidence.value());
  }
  std::sort(keys.begin(), keys.end());
  for (const std::string& key : keys) {
    digest.absorb_text(key);
  }
  return EpisodeId::from_digest(digest.value());
}

Result<HotspotResult> group_hotspots(const std::vector<HeadroomReport>& headroom,
                                     const ThermalTopology& topology, const CouplingGraph& coupling,
                                     const HotspotPolicy& policy, const Limits& limits) {
  const Status policy_status = policy.validate(limits);
  if (!policy_status.ok()) {
    return policy_status;
  }

  HotspotResult result;
  std::vector<HotspotMember> hot;
  for (const HeadroomReport& report : headroom) {
    if (report.state != EvidenceState::kFresh || !report.current_level_known) {
      continue;
    }
    ++result.evaluable_subjects;
    if (level_rank(report.current_level) < level_rank(policy.threshold)) {
      continue;
    }
    const HeadroomValue* band = report.level(policy.threshold);
    if (band == nullptr) {
      // The subject is hot but the envelope does not declare the band the policy keys on. That is
      // an unsupported grouping input, not a member.
      continue;
    }
    HotspotMember member;
    member.entity = report.entity;
    member.sensor = report.sensor;
    member.site = report.site;
    member.celsius = report.observed_celsius;
    member.level = report.current_level;
    member.excursion_c = -band->headroom_c;
    member.evidence = report.evidence;
    member.observed_at = report.observed_at;
    member.synthetic = report.evidence_synthetic;
    hot.push_back(member);
  }
  result.hot_subjects = hot.size();

  if (hot.empty()) {
    result.state = EvidenceState::kUnknown;
    result.reason = "no subject reached the " + std::string(to_string(policy.threshold)) +
                    " band among " + std::to_string(result.evaluable_subjects) +
                    " evaluable subject(s)";
    return result;
  }

  std::sort(hot.begin(), hot.end(), [](const HotspotMember& a, const HotspotMember& b) {
    if (a.entity != b.entity) return a.entity < b.entity;
    if (a.sensor != b.sensor) return a.sensor < b.sensor;
    return static_cast<unsigned>(a.site) < static_cast<unsigned>(b.site);
  });

  UnionFind sets;
  std::map<std::pair<EntityId, EntityId>, LinkCounts> links;
  std::set<EntityId> hot_entities;
  for (const HotspotMember& member : hot) {
    sets.add(member.entity);
    hot_entities.insert(member.entity);
  }

  // Grouping is transitive. Two hot entities inside the same rack, or joined through a third entity
  // that is itself cool, belong to one episode: the rack does not have to be hot for the heat in it
  // to be one problem. Every declared adjacency and every traversable relation is therefore unioned
  // regardless of whether its own endpoints are hot. Coincidence is still never traversed.
  if (policy.allow_adjacency) {
    for (const TopologyEdge& edge : topology.edges()) {
      sets.unite(edge.from, edge.to);
    }
  }
  for (const CouplingId& id : coupling.ids()) {
    const CouplingRelation* relation = coupling.find(id);
    if (relation == nullptr || !relation->is_traversable()) {
      continue;
    }
    if (relation->kind == CouplingKind::kPhysicalAdjacency && !policy.allow_adjacency) {
      continue;
    }
    sets.unite(relation->from, relation->to);
  }

  // Direct links between members, counted once per distinct relation, for the episode explanation.
  for (const CouplingId& id : coupling.ids()) {
    const CouplingRelation* relation = coupling.find(id);
    if (relation == nullptr || !relation->is_traversable()) {
      continue;
    }
    if (relation->kind == CouplingKind::kPhysicalAdjacency && !policy.allow_adjacency) {
      continue;
    }
    const auto key =
        std::make_pair(std::min(relation->from, relation->to), std::max(relation->from, relation->to));
    LinkCounts& counts = links[key];
    counts.relations.push_back(relation->id);
    if (relation->kind == CouplingKind::kPhysicalAdjacency) {
      counts.adjacency_declared = true;
    } else {
      counts.supported.insert(relation->id);
    }
  }
  if (policy.allow_adjacency) {
    for (const TopologyEdge& edge : topology.edges()) {
      const auto key = std::make_pair(std::min(edge.from, edge.to), std::max(edge.from, edge.to));
      links[key].adjacency_declared = true;
    }
  }

  std::map<EntityId, std::vector<HotspotMember>> components;
  for (const HotspotMember& member : hot) {
    components[sets.find(member.entity)].push_back(member);
  }

  for (auto& entry : components) {
    std::vector<HotspotMember>& members = entry.second;
    std::sort(members.begin(), members.end(),
              [](const HotspotMember& a, const HotspotMember& b) {
                if (a.observed_at != b.observed_at) return a.observed_at < b.observed_at;
                if (a.entity != b.entity) return a.entity < b.entity;
                return a.sensor < b.sensor;
              });

    // Split the component wherever the observation timeline has a gap wider than the policy allows.
    std::size_t start = 0;
    while (start < members.size()) {
      std::size_t end = start + 1;
      while (end < members.size()) {
        const Duration gap = age_of(members[end - 1].observed_at, members[end].observed_at);
        if (gap.nanos() > policy.episode_gap.nanos() || gap.nanos() < 0) {
          break;
        }
        ++end;
      }
      std::vector<HotspotMember> slice(members.begin() + static_cast<std::ptrdiff_t>(start),
                                       members.begin() + static_cast<std::ptrdiff_t>(end));
      if (slice.size() > limits.max_hotspot_members) {
        result.state = EvidenceState::kRefused;
        result.episodes.clear();
        result.reason =
            "hotspot grouping was refused: one episode would contain " +
            std::to_string(slice.size()) + " members, above the configured bound of " +
            std::to_string(limits.max_hotspot_members) +
            "; raise the threshold band or the member bound";
        return result;
      }

      HotspotEpisode episode;
      episode.threshold = policy.threshold;
      episode.members = slice;
      std::sort(episode.members.begin(), episode.members.end(),
                [](const HotspotMember& a, const HotspotMember& b) {
                  if (a.entity != b.entity) return a.entity < b.entity;
                  if (a.sensor != b.sensor) return a.sensor < b.sensor;
                  return static_cast<unsigned>(a.site) < static_cast<unsigned>(b.site);
                });
      episode.seed = episode.members.front().entity;
      episode.observed_at = episode.members.front().observed_at;
      episode.earliest_at = episode.members.front().observed_at;
      episode.peak_c = episode.members.front().celsius;
      std::vector<std::string> evidence_names;
      for (const HotspotMember& member : episode.members) {
        episode.peak_c = std::max(episode.peak_c, member.celsius);
        if (member.observed_at > episode.observed_at) {
          episode.observed_at = member.observed_at;
        }
        if (member.observed_at < episode.earliest_at) {
          episode.earliest_at = member.observed_at;
        }
        evidence_names.push_back(member.entity.value());
      }
      // The counts are over distinct links, not over member pairs. Two hot sensors on one entity
      // share that entity, so both member pairs resolve to the same entity pair; counting per pair
      // would report the one relation between those entities twice.
      std::set<CouplingId> distinct_supported;
      std::set<std::pair<EntityId, EntityId>> distinct_adjacency;
      for (std::size_t i = 0; i < episode.members.size(); ++i) {
        for (std::size_t j = i + 1; j < episode.members.size(); ++j) {
          const auto key =
              std::make_pair(std::min(episode.members[i].entity, episode.members[j].entity),
                             std::max(episode.members[i].entity, episode.members[j].entity));
          const auto link_it = links.find(key);
          if (link_it == links.end()) {
            continue;
          }
          distinct_supported.insert(link_it->second.supported.begin(),
                                    link_it->second.supported.end());
          if (link_it->second.adjacency_declared) {
            distinct_adjacency.insert(key);
          }
          episode.support_relations.insert(episode.support_relations.end(),
                                           link_it->second.relations.begin(),
                                           link_it->second.relations.end());
        }
      }
      episode.supported_links = distinct_supported.size();
      episode.adjacency_links = distinct_adjacency.size();
      std::sort(episode.support_relations.begin(), episode.support_relations.end());
      episode.support_relations.erase(
          std::unique(episode.support_relations.begin(), episode.support_relations.end()),
          episode.support_relations.end());
      episode.id = compute_episode_id(episode.members, episode.threshold);
      episode.reason =
          "episode of " + std::to_string(episode.members.size()) + " hot subject(s) seeded at " +
          episode.seed.value() + ", peak " + format_real(episode.peak_c) + " C in the " +
          std::string(to_string(episode.threshold)) + " band; " +
          std::to_string(episode.supported_links) +
          " supported thermal link(s) and " + std::to_string(episode.adjacency_links) +
          " declared adjacency link(s) directly between members (members may also be joined "
          "through entities that are not themselves hot); members: " + join(evidence_names, ",");
      result.episodes.push_back(std::move(episode));
      start = end;
    }
  }

  for (const auto& entry : components) {
    if (entry.second.size() == 1) {
      result.ungrouped.push_back(entry.first);
    }
  }
  std::sort(result.ungrouped.begin(), result.ungrouped.end());

  std::sort(result.episodes.begin(), result.episodes.end(),
            [](const HotspotEpisode& a, const HotspotEpisode& b) {
              if (a.peak_c != b.peak_c) return a.peak_c > b.peak_c;
              if (a.seed != b.seed) return a.seed < b.seed;
              return a.id < b.id;
            });
  if (result.episodes.size() > limits.max_hotspot_episodes) {
    result.dropped_episodes = result.episodes.size() - limits.max_hotspot_episodes;
    result.episodes.resize(limits.max_hotspot_episodes);
  }

  result.state = EvidenceState::kFresh;
  result.reason = std::to_string(result.episodes.size()) + " hotspot episode(s) among " +
                  std::to_string(result.hot_subjects) + " hot subject(s) at the " +
                  std::string(to_string(policy.threshold)) + " band";
  if (result.dropped_episodes > 0) {
    result.reason += "; " + std::to_string(result.dropped_episodes) +
                     " further episode(s) were dropped at the configured bound";
  }
  if (!result.ungrouped.empty()) {
    result.reason += "; " + std::to_string(result.ungrouped.size()) +
                     " hot entity/entities are connected to nothing and form no episode";
  }
  const Status text = validate_text(result.reason, "hotspot reason", limits.max_text_length * 8);
  if (!text.ok()) {
    result.reason = std::to_string(result.episodes.size()) + " hotspot episode(s)";
  }
  return result;
}

}  // namespace tobsv