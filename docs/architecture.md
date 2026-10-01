# Architecture

Thermal Observatory is a portable C++20 library that answers one question from the thermal evidence
it holds: where heat is accumulating, how much headroom remains, which hotspots and propagation
paths that evidence supports, what is derated, and where the attribution is uncertain. It observes,
explains and attributes. It never actuates. The contract behind that sentence is documented
separately in [boundaries.md](boundaries.md).

This document covers the layered structure, what each layer owns and refuses to own, the public
entry point, the composed containers, and the path one observation takes from ingest to a
`ThermalAnalysis`.

## The public entry point

`include/tobsv/tobsv.hpp` is the umbrella header. It includes every other public header of the
library, thirty-one of them, so a consumer includes one file and sees the whole surface. All names
live in namespace `tobsv`.

There are exactly two ways in:

1. **The composed runtime**, `tobsv::ThermalObservatory`. It owns one instance of every container,
   the durable log, the single-writer lock and the background committer. A caller opens it once,
   feeds it evidence and declarations, and asks it for analyses.
2. **The composition function**, `tobsv::analyze`. It takes the containers as arguments and returns
   `Result<ThermalAnalysis>`. This is the same function the facade calls, so a caller that already
   owns the containers can produce an identical answer without the runtime, the lock or the log.

The complete smallest story, which is also `examples/minimal_analysis.cpp`:

```cpp
#include <cstdio>

#include "tobsv/tobsv.hpp"

using namespace tobsv;

int main() {
  ThermalObservatory observatory;
  ObservatoryConfig config;  // an empty log_path means a purely in-memory runtime
  const Status opened = observatory.open(config);
  if (!opened.ok()) {
    std::printf("cannot open the runtime: %s\n", opened.describe().c_str());
    return 1;
  }

  const Timestamp at = Timestamp::parse("2026-02-14T09:31:07Z").value();
  const EntityId node = EntityId::unchecked("node-01");

  EntityRecord record;
  record.entity = node;
  record.entity_class = EntityClass::kComputeNode;
  record.zone = ZoneRef::unchecked("zone-a");  // a reference to another authority's zone
  record.site = SiteId::unchecked("hall-1");
  record.label = "Example node";
  if (!observatory.register_entity(record).ok()) {
    return 1;
  }

  ThermalEnvelope envelope;
  envelope.entity = node;
  envelope.entity_class = EntityClass::kComputeNode;
  envelope.site = MeasurementSite::kOutlet;
  envelope.nominal_c = 20.0;
  envelope.has_warn = true;
  envelope.warn_c = 35.0;
  envelope.has_maximum = true;
  envelope.maximum_c = 50.0;
  envelope.declared_by = SourceId::unchecked("facility-policy");
  envelope.declared_at = at;
  envelope.basis = "declared by the facility for this example";
  envelope.id = compute_envelope_id(envelope);
  if (!observatory.register_envelope(envelope).ok()) {
    return 1;
  }

  TemperatureObservation observation;
  observation.entity = node;
  observation.sensor = SensorId::unchecked("sensor-outlet-4");
  observation.site = MeasurementSite::kOutlet;
  observation.celsius = 41.5;
  observation.observed_at = at;
  observation.received_at = at;
  observation.provenance.source = SourceId::unchecked("example-facility-telemetry");
  observation.provenance.authority = AuthorityLevel::kMeasured;
  observation.provenance.kind = SourceKind::kFacilitySensor;
  observation.provenance.clock = ClockDomain::kCollectorWallClock;
  observation.provenance.method = "example synthetic reading";
  observation.quality.supplied = true;
  observation.quality.flags.add(QualityFlag::kCalibrated);
  observation.fence.source = observation.provenance.source;
  observation.fence.epoch = Epoch::from(1);
  observation.fence.generation = Generation::from(1);
  observation.fence.revision = Revision::from(1);
  observation.fence.incarnation = Incarnation::from(1);
  observation.fence.sequence = Sequence::from(1);
  observation.fence.attempt = AttemptId::unchecked("att-1");
  observation.id = compute_observation_id(observation);

  const IngestOutcome outcome = observatory.ingest(observation);
  if (!outcome.accepted()) {
    std::printf("ingest refused: %s\n", outcome.status.describe().c_str());
    return 1;
  }

  ThermalQuery query;
  query.evaluated_at = at;
  query.freshness = FreshnessPolicy::from_limits(observatory.limits());
  const Result<ThermalAnalysis> analysis = observatory.analyze(query);
  if (!analysis.ok()) {
    std::printf("analysis failed: %s\n", analysis.error().describe().c_str());
    return 1;
  }
  std::printf("%s\n", analysis.value().to_json().dump_indented().value().c_str());
  return 0;
}
```

The same answer without the facade:

```cpp
  Limits limits;
  EvidenceStore store(limits);
  ThermalInventory inventory;
  EnvelopeRegistry envelopes;
  ThermalTopology topology;
  CouplingGraph coupling;
  DeratingRegistry derating;
  TransitionMemory memory;

  ThermalQuery query;
  query.evaluated_at = Timestamp::parse("2026-02-14T09:31:07Z").value();

  const Result<ThermalAnalysis> analysis =
      analyze(query, store, inventory, envelopes, topology, coupling, derating, memory, limits);
  const std::string digest = analysis.ok() ? analysis.value().digest : analysis.error().describe();
```

`ThermalAnalysis` is a plain value. It holds no pointer into the store, the inventory or any
registry, so it stays valid after the containers change, and every collection inside it is ordered
deterministically.

## Layers

The layers are a composition order: each one may use the ones above it and none of the ones below
it. The include graph enforces the direction.

| Layer | Headers | Owns | Refuses to own |
| --- | --- | --- | --- |
| Core | `tobsv/core/*`, `tobsv/version.hpp` | Checked arithmetic, CRC-32C, the FNV-1a stable digest, strong identities, strict JSON, UTC time, resource limits, number formatting | Any thermal meaning; any failure expressed as a bare boolean |
| Model | `tobsv/model/*` | The shared vocabulary, fencing counters, declared observation adjacency, the entity inventory | Zone ownership, cooling boundaries, workload placement |
| Evidence | `tobsv/evidence/*` | Observations, provenance, quality, freshness resolution, the bounded store and its fence trackers | Any authority over what a value means; any averaging of disagreement |
| Analysis inputs | `tobsv/envelope`, `tobsv/headroom`, `tobsv/hotspot`, `tobsv/coupling`, `tobsv/derating`, `tobsv/attribution` | Declared envelopes and headroom arithmetic, episode grouping, coupling evidence and traversal, derating appraisal, the attribution ledger | Authoring limits, deciding derating, asserting causation from correlation |
| Composition | `tobsv/analysis` | The query, the single deterministic answer, transition memory, the digest | Reading or writing files; holding a lock |
| Persistence | `tobsv/persistence/*` | The canonical record codec, the framed append-only log, the single-writer lock, the commit worker, atomic publication and bounded reads | Interpreting records beyond their schema; repairing interior damage |
| Runtime | `tobsv/runtime` | The facade, the one state mutex, the open/close lifecycle, the recovery report | Any policy of its own; any second writer |

### Core

No layer below core exists, and core includes nothing from the rest of the library. Its rules are
mechanical and total:

* `checked::add`, `checked::sub`, `checked::mul`, `checked::narrow`, `checked::div` and friends
  return `std::optional`; overflow, non-finite input and a zero denominator are values, not
  undefined
  behaviour and not infinity.
* `StableDigest` is FNV-1a with a terminator byte after text, after numbers and after flags, so
  concatenation is unambiguous and `-0.0` differs from `0.0`. It is deliberately not cryptographic.
* `StrongId<Tag>` makes every identity a distinct type; an `EntityId` cannot be passed where a
  `SensorId` is required even though both are strings underneath. `validate_identity_token` bounds a
  token to 96 printable ASCII characters beginning with an alphanumeric, which stops an identity
  from breaking record framing.
* `JsonValue` is a strict, canonical JSON model: no comments, no trailing commas, no duplicate keys,
  no leading zeros, no non-finite numbers, bounded depth, bounded nodes, bounded bytes, and
  `dump_compact` writes object keys in lexicographic order with no insignificant whitespace.
* `Limits` carries a hard ceiling for every externally driven collection; a caller value above a
  ceiling is rejected with `ErrorCode::kOutOfRange` rather than silently clamped.

### Model

`vocabulary.hpp` fixes the textual spelling of every closed enumeration, so durable records, CLI
output and exported analyses can never disagree about a name. `generation.hpp` defines the fence
(epoch, generation, revision, incarnation, sequence, attempt) as five distinct counter types plus a
strong attempt identity, and `FenceTracker` implements the acceptance rules.
`topology.hpp` stores declared observation adjacency as an undirected canonical edge set: adjacency
says where heat can travel or where a sensor sits, and it is explicitly not a thermal zone, not a
cooling boundary and not an ownership claim. `inventory.hpp` records what is known about an
observed entity, including a `ZoneRef` that is only ever a reference to a zone another authority
owns.

### Evidence

`TemperatureObservation` is the atom: identity, subject (entity, sensor, site), value, the two
timestamps, provenance, quality, the optional envelope that was in force, and the fence.
`EvidenceStore` is a bounded, ordered, in-memory record set with a per-source `FenceTracker`, a
per-subject ring, and a resolution step that turns one subject into one `SubjectTemperature`. The
store owns no authority: it records, orders, refuses and explains. It has no mutex and no
background work; the runtime holds one lock over it (see [concurrency.md](concurrency.md)).

### Analysis inputs

Each package answers one part of the question and refuses the rest:

* `envelope` and `headroom` apply a declared envelope, compute signed headroom to every declared
  band and report bands the envelope does not declare as missing. The runtime does not author an
  envelope and does not own the policy behind one.
* `hotspot` groups hot subjects into episodes. It never invents a link: only traversable coupling
  and declared adjacency join members, and grouping is transitive over those declarations, so two
  hot entities can be one episode through an entity that is not itself hot.
* `coupling` records what a relation rests on. A relation without citations is refused, a
  coincidental relation is never traversed, and every path reports its weakest link.
* `derating` records a claim that an entity is held below its capability and checks the claim
  against the thermal evidence it cites. It does not enact, schedule or approve derating.
* `attribution` is the ledger of every reason an answer is weaker than it looks. Every weakened
  statement names its weakness instead of folding it into a confidence number.

### Composition

`analysis.hpp` declares `ThermalQuery`, `ThermalAnalysis` and `analyze`. `TransitionMemory` is a
caller-owned map from subject to the band the subject was last reported in; it is what makes a
threshold transition a comparison between two evaluations rather than a guess, and it is pruned to
`Limits::max_observations` subjects so a long-lived runtime does not remember every subject it has
ever seen. The analysis layers
the parts in a fixed order, applies the attribution rules, computes the overall state and the
content digest, and pushes one human-readable step per decision into `reason_steps`.

### Persistence

One record is one canonical JSON document on one line, framed with its length, checksum and
sequence. The log is append-only; the commit point is a completed write followed by a device flush.
The single-writer lock is kernel enforced. The commit worker owns the only writable handle. Details,
including the byte layout and the recovery policy, are in [persistence.md](persistence.md).

### Runtime

`ThermalObservatory` composes everything above. It holds the one state mutex, applies the
open/close lifecycle, acquires and releases the lock file, replays committed records on open, and
reports what recovery did in `RecoveryReport`. It adds no policy: every public method validates its
input with the layer that owns it and enqueues the durable record the layer produced.

## Data flow: from ingest to a ThermalAnalysis

1. **A caller hands in an observation.** `ThermalObservatory::ingest` takes the state mutex,
   requires an open runtime (`ErrorCode::kClosed`) and a commit queue with room
   (`ErrorCode::kQueueFull`).
2. **The store validates and orders it.** `EvidenceStore::ingest` checks the store configuration,
   derives the content identity when the caller left it unset, calls
   `TemperatureObservation::validate`, recomputes `compute_observation_id` and refuses a mismatch
   with `ErrorCode::kConflict`, returns `IngestKind::kDuplicate` for an identity already held, and
   otherwise classifies the fence with `FenceTracker::classify`. A stale epoch, a stale generation,
   an older revision and a reordered sequence are refused with their own codes; a different
   incarnation is a conflict.
3. **Bounds and retention are applied.** The global observation bound refuses with
   `ErrorCode::kLimitExceeded`. The per-subject ring is full: the oldest observation of that subject
   is retired, counted in `retired_count()` and reported later as an attribution limit. The
   observation is then inserted into the identity map, the subject ring and the sensor index, and
   recorded as the last accepted observation of its source.
4. **The durable record is enqueued.** A recorded observation is encoded with
   `encode(const TemperatureObservation&)`, wrapped by `wrap_record(RecordKind::kObservation, ...)`
   and submitted to the commit worker. When retention retired something, the high-water fence of
   that
   source is enqueued as a `RecordKind::kFence` checkpoint in the same critical section, so a
   restart cannot lower the mark.
5. **The commit worker publishes it.** The worker appends the frame with `durable = true`; the frame
   is committed when the write completed and the device flush returned. `flush_durable()` drains the
   queue and reports the first commit failure, which makes durability observable from the caller.
6. **A query is evaluated.** `ThermalObservatory::analyze` takes the state mutex and calls
   `tobsv::analyze` with the containers it owns. `analyze_now()` reads the configured freshness
   window and episode gap under the lock, releases it, and then calls `analyze`, which takes the
   lock
   again.
7. **Resolution.** `EvidenceStore::resolve_all` resolves every subject at the evaluation instant
   against the query's `FreshnessPolicy`, producing one of the seven `EvidenceState` values per
   subject (see [evidence-model.md](evidence-model.md)).
8. **Headroom and transitions.** Each resolved subject is handed to `compute_headroom` with the
   entity class from the inventory and the envelope registry. The band is compared with the previous
   evaluation from `TransitionMemory` and `classify_transition` names the transition.
9. **Grouping and traversal.** `group_hotspots` groups hot subjects into episodes over coupling and
   adjacency, splitting on the episode gap. `find_propagation` then traces bounded simple paths from
   the hottest subject to the requested targets.
10. **Derating and attribution.** Every entity that carries a claim is appraised by
    `appraise_derating`, which checks each cited observation for presence, freshness and band. The
    ledger is completed with the limits the earlier steps produced, the overall state is derived,
    and
    `reason_steps` is closed with a summary of the composition.
11. **The digest is computed.** `analysis.to_json().dump_compact()` is hashed with `stable_hash` and
    the result is stored as `digest` with an `an-` prefix. A verifier blanks the field and
    re-hashes;
    this is a content fingerprint, not a signature.

## The composed containers

`ThermalObservatory` owns exactly this set:

| Member | Type | Written by | Read by |
| --- | --- | --- | --- |
| `config_` | `ObservatoryConfig` | `open` | every method |
| `evidence_` | `EvidenceStore` | `ingest`, replay | `analyze`, `flush_durable`, accessor |
| `inventory_` | `ThermalInventory` | `register_entity`, replay | `analyze` |
| `envelopes_` | `EnvelopeRegistry` | `register_envelope`, replay | `analyze` |
| `topology_` | `ThermalTopology` | `declare_adjacency`, replay | `analyze` |
| `coupling_` | `CouplingGraph` | `record_coupling`, replay | `analyze` |
| `derating_` | `DeratingRegistry` | `record_derating`, replay | `analyze` |
| `memory_` | `TransitionMemory` | `analyze` | `analyze` |
| `writer_lock_` | `SingleWriterLock` | `open`, `close` | `open`, `close` |
| `log_` | `ThermalLog` | the commit worker | `open`, `close` |
| `worker_` | `CommitWorker` | `open`, `close`, every mutation | `close`, `flush_durable` |
| `recovery_` | `RecoveryReport` | `open`, `close`, `flush_durable` | `recovery` |
| `durable_path_` | `std::string` | `open`, `close` | every mutation |
| `open_` | `bool` | `open`, `close` | `is_open`, every method |
| `mutex_` | `std::mutex` | - | every public method |

Two properties of this set matter:

* every in-memory container above is guarded by the single `mutex_`, and none of them has an
  internal lock of its own;
* the durable log and the lock file are the only two OS resources, and both are acquired in `open`
  and released in `close`. A runtime with an empty `log_path` acquires neither.

The const accessors (`evidence()`, `inventory()`, `envelopes()`, `topology()`, `coupling()`,
`derating()`, `limits()`, `config()`, `recovery()`) return references, not copies. They are safe to
read while no other thread is mutating the runtime, and the caller is responsible for not reading
them concurrently with a mutation; `mutable_limits()` is likewise unsynchronized and exists for
configuration before `open`.

## Determinism

Determinism is a design property, not an accident of the implementation:

* every container that feeds an answer is ordered by identity (`std::map`, sorted vectors), never by
  insertion order;
* every derived identity is a function of content (see [evidence-model.md](evidence-model.md)), and
  every digest absorbs sorted keys so that two runs agree byte for byte;
* canonical JSON output fixes spelling, key order and number formatting, and a non-finite number
  cannot be encoded at all;
* the analysis digest is a function of the analysis document, so two runs with the same records, the
  same evaluation instant and the same policy produce the same digest.

The remaining inputs are the ones a caller controls: `Limits`, the query policy and the evaluation
instant. Change any of them and the answer may legitimately change.

## Where to read next

* [boundaries.md](boundaries.md) - what this runtime refuses, and which code it returns.
* [evidence-model.md](evidence-model.md) - observations, provenance, quality, fences and resolution.
* [hotspots-and-coupling.md](hotspots-and-coupling.md) - envelopes, headroom, episodes, coupling and
  traversal.
* [persistence.md](persistence.md) - the byte layout, the commit point and recovery.
* [concurrency.md](concurrency.md) - the locks, the threads and the audit behind them.
* [testing.md](testing.md) - the suites, the property approach and how to run them.
* [limitations.md](limitations.md) - what is not proven.
* [cli.md](cli.md) - the command line tool and the record format.
