// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/runtime/observatory.hpp"

#include <utility>

#include "tobsv/persistence/codec.hpp"
#include "tobsv/persistence/files.hpp"

namespace tobsv {

Status ObservatoryConfig::validate() const {
  const Status limits_status = limits.validate();
  if (!limits_status.ok()) {
    return limits_status;
  }
  if (writer_epoch == 0) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "the writer epoch must be at least 1 so that a restart opens a new one");
  }
  if (!log_path.empty()) {
    const std::string name = log_path.filename().string();
    if (name.empty()) {
      return Status::failure(ErrorCode::kInvalidArgument,
                             "the durable log path must name a file, not a directory");
    }
  }
  return Status::success();
}

JsonValue ThermalObservatory::RecoveryReport::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("durable", JsonValue::boolean(durable));
  out.set("lock_acquired", JsonValue::boolean(lock_acquired));
  out.set("log_created", JsonValue::boolean(log_created));
  out.set("short_header_reinitialised", JsonValue::boolean(short_header_reinitialised));
  out.set("records_loaded", JsonValue::integer(static_cast<std::int64_t>(records_loaded)));
  out.set("discarded_tail_bytes",
          JsonValue::integer(static_cast<std::int64_t>(discarded_tail_bytes)));
  out.set("observations_replayed",
          JsonValue::integer(static_cast<std::int64_t>(observations_replayed)));
  out.set("records_refused_on_replay",
          JsonValue::integer(static_cast<std::int64_t>(records_refused_on_replay)));
  out.set("commits_settled", JsonValue::integer(static_cast<std::int64_t>(commits_settled)));
  out.set("commit_failures", JsonValue::integer(static_cast<std::int64_t>(commit_failures)));
  out.set("detail", JsonValue::text(detail));
  return out;
}

ThermalObservatory::ThermalObservatory() : evidence_(Limits{}) {}

ThermalObservatory::~ThermalObservatory() { (void)close(); }

Status ThermalObservatory::require_open_locked() const {
  if (!open_) {
    return Status::failure(ErrorCode::kClosed,
                           "the observatory is not open; call open() before using it");
  }
  return Status::success();
}

Status ThermalObservatory::require_room_locked(std::size_t records) const {
  if (!config_.durable_commits || durable_path_.empty()) {
    return Status::success();
  }
  if (!worker_.has_room_for(records)) {
    return Status::failure(ErrorCode::kQueueFull,
                           "the durable commit queue cannot take " + std::to_string(records) +
                               " more record(s); flush_durable() before submitting more evidence");
  }
  return Status::success();
}

Status ThermalObservatory::enqueue(RecordKind kind, JsonValue payload) {
  if (!config_.durable_commits || durable_path_.empty()) {
    return Status::success();
  }
  Result<JsonValue> wrapped = wrap_record(kind, std::move(payload));
  if (!wrapped.ok()) {
    return wrapped.error();
  }
  Result<std::string> encoded = wrapped.value().dump_compact();
  if (!encoded.ok()) {
    return encoded.error();
  }
  Result<std::uint64_t> job = worker_.submit(encoded.value());
  if (!job.ok()) {
    return job.error();
  }
  return Status::success();
}

Status ThermalObservatory::open(const ObservatoryConfig& config) {
  const Status config_status = config.validate();
  if (!config_status.ok()) {
    return config_status;
  }
  std::unique_lock lock(mutex_);
  if (open_) {
    return Status::failure(ErrorCode::kInvalidArgument, "the observatory is already open");
  }
  if (closing_) {
    return Status::failure(ErrorCode::kShuttingDown,
                           "the observatory is closing; open it again once close() has returned");
  }
  // Opening resets every composed structure. Reopening the same object - after a close, or after a
  // failed open - must start from the durable state on disk, not from whatever the previous session
  // had already recovered, or every replayed record would be refused as a duplicate.
  config_ = config;
  evidence_ = EvidenceStore(config_.limits);
  inventory_ = ThermalInventory{};
  envelopes_ = EnvelopeRegistry{};
  topology_ = ThermalTopology{};
  coupling_ = CouplingGraph{};
  derating_ = DeratingRegistry{};
  memory_.clear();
  recovery_ = RecoveryReport{};
  durable_path_ = config_.log_path.string();

  const Status store_status = evidence_.configuration_status();
  if (!store_status.ok()) {
    return store_status;
  }

  if (!config_.log_path.empty()) {
    // The lock file lives beside the log, so its directory has to exist before the lock is taken.
    const Status directory = ensure_directory(config_.log_path.parent_path());
    if (!directory.ok()) {
      recovery_.detail = directory.describe();
      return directory;
    }
    std::filesystem::path lock_path = config_.log_path;
    lock_path += ".lock";
    const Status locked = writer_lock_.acquire(lock_path);
    if (!locked.ok()) {
      recovery_.detail = locked.describe();
      return locked;
    }
    recovery_.lock_acquired = true;
    recovery_.durable = true;

    const bool existed = std::filesystem::exists(config_.log_path);
    const Status opened =
        log_.open(config_.log_path, ThermalLog::OpenMode::kOpenOrCreate, config_.writer_epoch,
                  config_.limits);
    if (!opened.ok()) {
      writer_lock_.release();
      recovery_.lock_acquired = false;
      recovery_.detail = opened.describe();
      return opened;
    }
    recovery_.log_created = !existed;

    // The log reports what its own open recovered. Re-reading the file here would duplicate the
    // parse and, on platforms where a second handle is refused, would fail outright.
    const ThermalLog::LoadResult& loaded = log_.load_result();
    recovery_.records_loaded = loaded.payloads.size();
    recovery_.discarded_tail_bytes = loaded.discarded_tail_bytes;
    recovery_.short_header_reinitialised = loaded.reinitialised_short_header;

    std::uint64_t refused = 0;
    for (const std::string& payload : loaded.payloads) {
      Result<DecodedRecord> record = decode_record(payload, config_.limits);
      if (!record.ok()) {
        // A record that was committed but cannot be decoded is a hard stop. Continuing would mean
        // silently dropping evidence the log claims is committed.
        (void)log_.close();
        writer_lock_.release();
        recovery_.lock_acquired = false;
        recovery_.detail = record.error().describe();
        return Status::failure(record.error().code(),
                               "a committed record could not be decoded: " +
                                   record.error().describe());
      }
      apply_record_locked(record.value(), refused);
    }
    recovery_.records_refused_on_replay = refused;

    const Status started = worker_.start(&log_, config_.limits);
    if (!started.ok()) {
      (void)log_.close();
      writer_lock_.release();
      recovery_.lock_acquired = false;
      recovery_.detail = started.describe();
      return started;
    }
  }

  open_ = true;
  recovery_.detail = config_.log_path.empty()
                         ? "operating in memory only; nothing in this session is durable"
                         : "durable state opened at " + durable_path_;
  return Status::success();
}

void ThermalObservatory::apply_record_locked(const DecodedRecord& record, std::uint64_t& refused) {
  switch (record.kind) {
    case RecordKind::kObservation: {
      const IngestOutcome outcome = evidence_.ingest(record.observation);
      if (outcome.accepted()) {
        ++recovery_.observations_replayed;
      } else {
        ++refused;
      }
      break;
    }
    case RecordKind::kFence: {
      const Status status = evidence_.restore_high_water(record.fence);
      if (!status.ok()) {
        ++refused;
      }
      break;
    }
    case RecordKind::kEnvelope: {
      if (!envelopes_.add(record.envelope, config_.limits).ok()) {
        ++refused;
      }
      break;
    }
    case RecordKind::kEntity: {
      if (!inventory_.add(record.entity, config_.limits).ok()) {
        ++refused;
      }
      break;
    }
    case RecordKind::kAdjacency: {
      if (!topology_.add_entity(record.adjacency.from, config_.limits).ok()) {
        ++refused;
      }
      if (!topology_.add_entity(record.adjacency.to, config_.limits).ok()) {
        ++refused;
      }
      if (!topology_.add_edge(record.adjacency, config_.limits).ok()) {
        ++refused;
      }
      break;
    }
    case RecordKind::kCoupling: {
      if (!coupling_.add(record.coupling, config_.limits).ok()) {
        ++refused;
      }
      break;
    }
    case RecordKind::kDerating: {
      if (!derating_.add(record.derating, config_.limits).ok()) {
        ++refused;
      }
      break;
    }
  }
}

Status ThermalObservatory::close() {
  std::vector<CommitWorker::Outcome> outcomes;
  {
    std::unique_lock lock(mutex_);
    if (!open_ && durable_path_.empty()) {
      return Status::success();
    }
    open_ = false;
    closing_ = true;
    if (!durable_path_.empty()) {
      const Status drained = worker_.drain(outcomes);
      recovery_.commits_settled = worker_.submitted();
      recovery_.commit_failures = worker_.failed();
      if (!drained.ok()) {
        recovery_.detail += "; " + drained.describe();
      }
    }
  }
  // The join happens outside the state lock. The worker never takes the state lock, so this cannot
  // deadlock, and releasing it first keeps a concurrent caller from blocking on a device flush.
  (void)worker_.shutdown();
  std::unique_lock lock(mutex_);
  const Status closed = log_.close();
  writer_lock_.release();
  recovery_.lock_acquired = false;
  durable_path_.clear();
  closing_ = false;
  return closed;
}

bool ThermalObservatory::is_open() const {
  std::unique_lock lock(mutex_);
  return open_;
}

Status ThermalObservatory::flush_durable() {
  std::unique_lock lock(mutex_);
  const Status status = require_open_locked();
  if (!status.ok()) {
    return status;
  }
  if (durable_path_.empty()) {
    return Status::success();
  }
  std::vector<CommitWorker::Outcome> outcomes;
  const Status drained = worker_.drain(outcomes);
  recovery_.commits_settled = worker_.submitted();
  recovery_.commit_failures = worker_.failed();
  return drained;
}

IngestOutcome ThermalObservatory::ingest(const TemperatureObservation& observation) {
  std::unique_lock lock(mutex_);
  IngestOutcome refused;
  refused.id = observation.id;
  const Status open_status = require_open_locked();
  if (!open_status.ok()) {
    refused.kind = IngestKind::kRefused;
    refused.status = open_status;
    return refused;
  }
  // Ingest enqueues a fence checkpoint in addition to the observation when, and only when, the
  // subject's ring is about to retire its oldest entry. Both slots are reserved up front so that
  // the second submit cannot fail after the first has already been accepted into the record set.
  // Reserving two slots unconditionally would make a queue bound below two unusable.
  const bool checkpoint_follows =
      evidence_.would_retire(observation.entity, observation.sensor, observation.site);
  const Status room = require_room_locked(checkpoint_follows ? 2 : 1);
  if (!room.ok()) {
    refused.kind = IngestKind::kRefused;
    refused.status = room;
    return refused;
  }
  const std::size_t retired_before = evidence_.retired_count();
  IngestOutcome outcome = evidence_.ingest(observation);
  if (outcome.kind == IngestKind::kRecorded) {
    const TemperatureObservation* stored = evidence_.find(outcome.id);
    if (stored != nullptr) {
      const Status enqueued = enqueue(RecordKind::kObservation, encode(*stored));
      if (!enqueued.ok()) {
        outcome.kind = IngestKind::kRefused;
        outcome.status = enqueued;
        return outcome;
      }
      if (evidence_.retired_count() > retired_before) {
        // Retirement can drop the observation that carried the highest fence for its source. The
        // high-water mark is therefore checkpointed durably at the moment retention changes, so a
        // restart cannot lower it and let superseded evidence back in.
        const Fence* mark = evidence_.high_water_for(stored->provenance.source);
        if (mark != nullptr) {
          const Status checkpointed = enqueue(RecordKind::kFence, encode(*mark));
          if (!checkpointed.ok()) {
            outcome.status = checkpointed;
          }
        }
      }
    }
  }
  return outcome;
}

Status ThermalObservatory::register_entity(const EntityRecord& record) {
  std::unique_lock lock(mutex_);
  Status status = require_open_locked();
  if (!status.ok()) return status;
  status = require_room_locked();
  if (!status.ok()) return status;
  status = inventory_.add(record, config_.limits);
  if (!status.ok()) return status;
  return enqueue(RecordKind::kEntity, encode(record));
}

Status ThermalObservatory::register_envelope(const ThermalEnvelope& envelope) {
  std::unique_lock lock(mutex_);
  Status status = require_open_locked();
  if (!status.ok()) return status;
  status = require_room_locked();
  if (!status.ok()) return status;
  status = envelopes_.add(envelope, config_.limits);
  if (!status.ok()) return status;
  return enqueue(RecordKind::kEnvelope, encode(envelope));
}

Status ThermalObservatory::declare_adjacency(const TopologyEdge& edge) {
  std::unique_lock lock(mutex_);
  Status status = require_open_locked();
  if (!status.ok()) return status;
  status = require_room_locked();
  if (!status.ok()) return status;
  status = topology_.add_entity(edge.from, config_.limits);
  if (!status.ok() && status.code() != ErrorCode::kDuplicateIdentity) return status;
  status = topology_.add_entity(edge.to, config_.limits);
  if (!status.ok() && status.code() != ErrorCode::kDuplicateIdentity) return status;
  status = topology_.add_edge(edge, config_.limits);
  if (!status.ok()) return status;
  return enqueue(RecordKind::kAdjacency, encode(edge));
}

Status ThermalObservatory::record_coupling(const CouplingRelation& relation) {
  std::unique_lock lock(mutex_);
  Status status = require_open_locked();
  if (!status.ok()) return status;
  status = require_room_locked();
  if (!status.ok()) return status;
  status = coupling_.add(relation, config_.limits);
  if (!status.ok()) return status;
  return enqueue(RecordKind::kCoupling, encode(relation));
}

Status ThermalObservatory::record_derating(const DeratingEvidence& evidence) {
  std::unique_lock lock(mutex_);
  Status status = require_open_locked();
  if (!status.ok()) return status;
  status = require_room_locked();
  if (!status.ok()) return status;
  status = derating_.add(evidence, config_.limits);
  if (!status.ok()) return status;
  return enqueue(RecordKind::kDerating, encode(evidence));
}

Result<ThermalAnalysis> ThermalObservatory::analyze(const ThermalQuery& query) {
  std::unique_lock lock(mutex_);
  const Status open_status = require_open_locked();
  if (!open_status.ok()) {
    return open_status;
  }
  return tobsv::analyze(query, evidence_, inventory_, envelopes_, topology_, coupling_, derating_,
                        memory_, config_.limits);
}

Result<ThermalAnalysis> ThermalObservatory::analyze_now() {
  Result<Timestamp> now = Timestamp::now_utc();
  if (!now.ok()) {
    return now.error();
  }
  ThermalQuery query;
  // The state lock is taken exactly once, here and then again inside analyze(); it is not held
  // across the call, because std::mutex is not recursive and a second acquisition in the same
  // thread would deadlock against itself.
  {
    std::unique_lock lock(mutex_);
    const Status open_status = require_open_locked();
    if (!open_status.ok()) {
      return open_status;
    }
    query.freshness = FreshnessPolicy::from_limits(config_.limits);
    query.hotspots.episode_gap = Duration::from_nanos(config_.limits.default_episode_gap);
  }
  query.evaluated_at = now.value();
  return analyze(query);
}

}  // namespace tobsv
