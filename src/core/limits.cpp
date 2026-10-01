// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/core/limits.hpp"

namespace tobsv {
namespace {

Status require_le(std::size_t value, std::size_t hard_ceiling, const char* name) {
  if (value == 0) {
    return Status::failure(ErrorCode::kInvalidArgument, std::string(name) + " must be non-zero");
  }
  if (value > hard_ceiling) {
    return Status::failure(ErrorCode::kOutOfRange,
                           std::string(name) + " exceeds the hard ceiling of " +
                               std::to_string(hard_ceiling));
  }
  return Status::success();
}

Status require_duration(Nanos value, Nanos hard_max, const char* name) {
  if (value <= 0) {
    return Status::failure(ErrorCode::kInvalidArgument, std::string(name) + " must be positive");
  }
  if (value > hard_max) {
    return Status::failure(ErrorCode::kOutOfRange, std::string(name) + " is implausibly large");
  }
  return Status::success();
}

}  // namespace

Status Limits::validate() const {
  Status status = require_le(max_entities, ceiling::kEntities, "max_entities");
  if (!status.ok()) return status;
  status = require_le(max_sources, ceiling::kSources, "max_sources");
  if (!status.ok()) return status;
  status = require_le(max_sensors, ceiling::kSensors, "max_sensors");
  if (!status.ok()) return status;
  status = require_le(max_observations, ceiling::kObservations, "max_observations");
  if (!status.ok()) return status;
  status = require_le(max_observations_per_subject, ceiling::kObservations,
                      "max_observations_per_subject");
  if (!status.ok()) return status;
  if (max_observations_per_subject < 2) {
    // A ring of one would retire a subject's only observation to make room for its replacement and
    // then refuse the replacement, destroying evidence while reporting a refusal.
    return Status::failure(ErrorCode::kInvalidArgument,
                           "max_observations_per_subject must be at least 2 so that a subject can "
                           "always hold its own current reading");
  }
  status = require_le(max_sensors_per_entity, ceiling::kSensors, "max_sensors_per_entity");
  if (!status.ok()) return status;
  status = require_le(max_topology_edges, ceiling::kRelations, "max_topology_edges");
  if (!status.ok()) return status;
  status = require_le(max_edges_per_entity, ceiling::kRelations, "max_edges_per_entity");
  if (!status.ok()) return status;
  status = require_le(max_envelopes, ceiling::kEntities, "max_envelopes");
  if (!status.ok()) return status;
  status = require_le(max_couplings, ceiling::kRelations, "max_couplings");
  if (!status.ok()) return status;
  status = require_le(max_citations_per_relation, ceiling::kCitations,
                      "max_citations_per_relation");
  if (!status.ok()) return status;
  status = require_le(max_coupling_depth, ceiling::kCouplingDepth, "max_coupling_depth");
  if (!status.ok()) return status;
  status = require_le(max_propagation_paths, ceiling::kPropagationPaths, "max_propagation_paths");
  if (!status.ok()) return status;
  status = require_le(max_hotspot_members, ceiling::kMembers, "max_hotspot_members");
  if (!status.ok()) return status;
  status = require_le(max_hotspot_episodes, ceiling::kEpisodes, "max_hotspot_episodes");
  if (!status.ok()) return status;
  status = require_le(max_derating_evidence, ceiling::kRelations, "max_derating_evidence");
  if (!status.ok()) return status;
  status = require_le(max_attribution_limits, ceiling::kReasonSteps, "max_attribution_limits");
  if (!status.ok()) return status;
  status = require_le(max_reason_steps, ceiling::kReasonSteps, "max_reason_steps");
  if (!status.ok()) return status;
  status = require_le(max_text_length, ceiling::kTextLength, "max_text_length");
  if (!status.ok()) return status;
  status = require_le(max_citations, ceiling::kCitations, "max_citations");
  if (!status.ok()) return status;
  status = require_le(max_json_depth, ceiling::kJsonDepth, "max_json_depth");
  if (!status.ok()) return status;
  status = require_le(max_json_nodes, ceiling::kJsonNodes, "max_json_nodes");
  if (!status.ok()) return status;
  status = require_le(max_json_bytes, ceiling::kJsonBytes, "max_json_bytes");
  if (!status.ok()) return status;
  status = require_le(max_record_bytes, ceiling::kRecordBytes, "max_record_bytes");
  if (!status.ok()) return status;
  status = require_le(max_segment_bytes, ceiling::kSegmentBytes, "max_segment_bytes");
  if (!status.ok()) return status;
  status = require_le(max_commit_queue_depth, ceiling::kQueueDepth, "max_commit_queue_depth");
  if (!status.ok()) return status;
  status = require_le(max_loaded_records, ceiling::kLoadedRecords, "max_loaded_records");
  if (!status.ok()) return status;
  status = require_le(max_text_length, max_record_bytes, "max_text_length");
  if (!status.ok()) return status;

  constexpr Nanos kMaxWindow = 30LL * 24 * 60 * 60 * 1000 * 1000 * 1000;  // 30 days
  status = require_duration(default_freshness_window, kMaxWindow, "default_freshness_window");
  if (!status.ok()) return status;
  status = require_duration(default_episode_gap, kMaxWindow, "default_episode_gap");
  if (!status.ok()) return status;

  if (max_observations_per_subject > max_observations) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "max_observations_per_subject exceeds max_observations");
  }
  if (max_sensors_per_entity > max_sensors) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "max_sensors_per_entity exceeds max_sensors");
  }
  if (max_edges_per_entity > max_topology_edges) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "max_edges_per_entity exceeds max_topology_edges");
  }
  if (max_citations_per_relation > max_citations) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "max_citations_per_relation exceeds max_citations");
  }
  return Status::success();
}

Status check_capacity(std::size_t current, std::size_t bound, std::string_view what) {
  if (current >= bound) {
    return Status::failure(ErrorCode::kLimitExceeded,
                           std::string(what) + " is at its bound of " + std::to_string(bound));
  }
  return Status::success();
}

Status validate_text(std::string_view text, std::string_view what, std::size_t max_length) {
  if (text.empty()) {
    return Status::failure(ErrorCode::kInvalidArgument, std::string(what) + " is empty");
  }
  if (text.size() > max_length) {
    return Status::failure(ErrorCode::kLimitExceeded,
                           std::string(what) + " exceeds " + std::to_string(max_length) +
                               " characters");
  }
  for (std::size_t index = 0; index < text.size(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(text[index]);
    if (ch < 0x20U || ch == 0x7FU) {
      return Status::failure(ErrorCode::kInvalidArgument,
                             std::string(what) + " contains a control character at offset " +
                                 std::to_string(index));
    }
  }
  return Status::success();
}

}  // namespace tobsv