// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "tobsv/analysis/analysis.hpp"
#include "tobsv/core/json.hpp"
#include "tobsv/persistence/codec.hpp"
#include "tobsv/persistence/lock.hpp"
#include "tobsv/persistence/log.hpp"
#include "tobsv/persistence/worker.hpp"

namespace tobsv {

struct ObservatoryConfig {
  Limits limits;
  // Empty means the runtime works entirely in memory. Everything else in this configuration is
  // then ignored.
  std::filesystem::path log_path;
  // Written into the log header for diagnosis. Replay rejection does not depend on it: the durable
  // fences carried by each record are what order the evidence.
  std::uint64_t writer_epoch = 1;
  bool durable_commits = true;

  Status validate() const;
};

// The composed runtime: one bounded evidence store, one inventory, one envelope registry, one
// observation topology, one coupling graph, one derating registry, one durable log and one
// committer thread.
//
// Concurrency
//   A single state mutex guards every structure above. Public methods take it, do their work and
//   return; no callback is ever invoked while it is held. The only other lock is the committer's
//   own mutex, and it is always taken second. The committer thread never takes the state mutex, so
//   the lock graph is acyclic. See docs/concurrency.md for the full audit.
class ThermalObservatory {
 public:
  ThermalObservatory();
  ~ThermalObservatory();

  ThermalObservatory(const ThermalObservatory&) = delete;
  ThermalObservatory& operator=(const ThermalObservatory&) = delete;
  ThermalObservatory(ThermalObservatory&&) = delete;
  ThermalObservatory& operator=(ThermalObservatory&&) = delete;

  struct RecoveryReport {
    bool durable = false;
    bool lock_acquired = false;
    bool log_created = false;
    bool short_header_reinitialised = false;
    std::uint64_t records_loaded = 0;
    std::uint64_t discarded_tail_bytes = 0;
    std::uint64_t observations_replayed = 0;
    std::uint64_t records_refused_on_replay = 0;
    std::uint64_t commits_settled = 0;
    std::uint64_t commit_failures = 0;
    std::string detail;

    JsonValue to_json() const;
  };

  // Acquires the single-writer lock, recovers any durable state and starts the committer. Recovery
  // never promotes old evidence to current: it rebuilds the record set and the fencing marks, and
  // every freshness decision is still made against the evaluation instant.
  Status open(const ObservatoryConfig& config);
  Status close();
  bool is_open() const;

  IngestOutcome ingest(const TemperatureObservation& observation);
  Status register_entity(const EntityRecord& record);
  Status register_envelope(const ThermalEnvelope& envelope);
  Status declare_adjacency(const TopologyEdge& edge);
  Status record_coupling(const CouplingRelation& relation);
  Status record_derating(const DeratingEvidence& evidence);

  // The barrier that makes durable commits observable. Returns the first commit failure, if any.
  Status flush_durable();

  Result<ThermalAnalysis> analyze(const ThermalQuery& query);
  // Convenience: evaluates at the wall clock instant with the configured defaults.
  Result<ThermalAnalysis> analyze_now();

  const EvidenceStore& evidence() const noexcept { return evidence_; }
  const ThermalInventory& inventory() const noexcept { return inventory_; }
  const EnvelopeRegistry& envelopes() const noexcept { return envelopes_; }
  const ThermalTopology& topology() const noexcept { return topology_; }
  const CouplingGraph& coupling() const noexcept { return coupling_; }
  const DeratingRegistry& derating() const noexcept { return derating_; }
  const Limits& limits() const noexcept { return config_.limits; }
  Limits& mutable_limits() noexcept { return config_.limits; }
  const ObservatoryConfig& config() const noexcept { return config_; }
  const RecoveryReport& recovery() const noexcept { return recovery_; }
  const std::string& durable_path() const noexcept { return durable_path_; }

 private:
  // Enqueues one already-encoded record. The caller holds the state lock and has already checked
  // that the queue has room.
  Status enqueue(RecordKind kind, JsonValue payload);
  Status require_open_locked() const;
  // Reserves room for that many durable records, so that an operation which submits more than one
  // record cannot have its first submit accepted and its second refused.
  Status require_room_locked(std::size_t records = 1) const;
  void apply_record_locked(const DecodedRecord& record, std::uint64_t& refused);

  mutable std::mutex mutex_;
  ObservatoryConfig config_;
  EvidenceStore evidence_;
  ThermalInventory inventory_;
  EnvelopeRegistry envelopes_;
  ThermalTopology topology_;
  CouplingGraph coupling_;
  DeratingRegistry derating_;
  TransitionMemory memory_;
  SingleWriterLock writer_lock_;
  ThermalLog log_;
  CommitWorker worker_;
  RecoveryReport recovery_;
  std::string durable_path_;
  bool open_ = false;
  // Set while close() is between its two critical sections, so that a concurrent open() is refused
  // with an explicit reason instead of observing a half-closed object.
  bool closing_ = false;
};

}  // namespace tobsv
