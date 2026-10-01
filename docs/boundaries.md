# Boundaries

Thermal Observatory is the thermal observation, explanation and attribution layer of a data centre
control plane. This document states exactly where its authority ends, how the type system enforces
that end, and which operations it refuses, with the code each refusal returns.

## What this runtime owns

* **Observation.** It records temperature observations with their provenance, quality, fencing and
  bounds, and it decides whether each one is admissible.
* **Explanation.** Every answer it produces carries reason text: per-subject resolution reasons,
  per-band headroom reasons, per-episode reasons, per-path reasons, per-claim derating reasons and
  an ordered `reason_steps` list for the whole analysis.
* **History.** It keeps an append-only durable record of what it was told, in the order it accepted
  it, with the fencing position of every record.
* **Headroom and hotspot analysis.** It computes the signed distance from an observed temperature to
  each declared band, groups hot subjects into episodes, and reports what joined each group.
* **Coupling evidence and attribution.** It records what a coupling relation rests on, traverses
  only the relations that support propagation, and collects every reason an answer is weaker than it
  looks.

## What this runtime does not own

* **Actuation.** No type in `include/tobsv` names a fan, pump, valve, cooling unit or any other
  actuator, and no function writes to a device. The closest concept is `DeratingKind`: a
  `DeratingEvidence` record states that an entity *is* being held below its capability. The runtime
  checks the claim against the thermal evidence the claimant cited. It never makes the claim true.
* **Thermal policy.** The runtime does not author an envelope, does not decide what a safe
  temperature is and does not set a limit. A `ThermalEnvelope` is what some authority declared: it
  carries `declared_by`, `declared_at` and a free-text `basis`, and the runtime only applies it.
* **Thermal zones.** It does not create, bound, govern or arbitrate a zone.
* **Workload placement.** No type names a workload, job, virtual machine or placement decision, and
  no operation moves, evicts or admits one.
* **Recovery.** It performs no failover, no degradation management and no restoration of anything it
  observes. The word "recovery" appears in this codebase only for one thing: reading back its own
  durable log after a torn tail, which is recovery of a file, not of a system.
* **Power control and networking.** There is no power management type and no socket code anywhere:
  the runtime never contacts an external service.

## How the type system enforces the boundary

### ZoneRef is a reference, never a thing this runtime can create or bound

`ZoneRef` is `StrongId<ZoneRefTag>`, declared with the kind string "thermal zone reference" and the
derived-identity prefix "zone". It appears in exactly two places:

* `EntityRecord::zone`, an optional field of an inventory record;
* `ThermalInventory::zone_of`, which returns the recorded reference or an empty one for an entity
  that was never registered.

Nothing else in the library reads a zone. Envelope selection is by entity, entity class and
measurement site. Hotspot grouping is by coupling relation and declared adjacency. Propagation walks
coupling relations. A zone reference therefore travels with a report so that a reader can name the
zone an entity is said to belong to, and it influences no decision this runtime makes. There is no
operation that creates a zone, sets a boundary on one, compares two, or resolves a conflict between
them, because there is no such function in any header.

`ThermalTopology` is deliberately not a zone model either. Its own documentation says adjacency is a
statement about where heat can travel or where a sensor sits, "and nothing more: it is not a thermal
zone, not a cooling boundary, not an ownership claim and not a placement decision".

### AuthorityLevel and SourceKind are declared by the producer and never upgraded

`Provenance` carries `AuthorityLevel` (measured, derived, modeled, synthetic), `SourceKind`
(facility sensor, platform agent, external telemetry, operator declaration, synthetic generator) and
a `ClockDomain`. All three are declared by the caller:

* `Provenance::validate` refuses a source that leaves its authority at `kUnknown`, its kind at
  `kUnknown` or its clock at `kUnspecified`, each with `ErrorCode::kUnsupported`. A source has to
  say
  how its values were obtained before any of them are admitted.
* Nothing in the library assigns a new `AuthorityLevel` to existing evidence. The store ranks
  candidates only to *order* them when choosing a representative (`measured` above `derived` above
  `modeled` above `synthetic`); the chosen value keeps the level it arrived with.
* `Provenance::is_synthetic()` is true when the kind is a synthetic generator or the authority is
  synthetic, and that flag is copied into every result that rests on the value:
  `ObservationRef::synthetic`, `SubjectTemperature::representative_synthetic`,
  `HeadroomReport::evidence_synthetic`, `HotspotMember::synthetic` and, for derating,
  `DeratingEvidence::is_synthetic()`.

Because authority is declared, it is also not verified. The runtime reports `kDerivedAuthority` when
a representative is derived or modeled and `kSyntheticSource` when it is synthetic, and a consumer
decides what that is worth.

### Every result carries the authority it rests on

| Result | What it carries |
| --- | --- |
| `ObservationRef` | observation identity, source, fence revision, authority, site, quality-supplied and quality-degraded flags, synthetic flag |
| `SubjectTemperature` | the representative's identity, authority, synthetic flag and quality flags, plus the supporting, conflicting, superseded, stale and indeterminate observation lists |
| `HeadroomReport` | the envelope that was applied, the observation that was used, and whether that observation was synthetic |
| `HotspotMember` | the observation that made the subject hot, its synthetic and quality-degraded flags |
| `PropagationStep` | the relation identity, its kind and its strength |
| `DeratingEntry` | the asserting source, the source kind, the synthetic flag, the cited observations that are missing, stale or too cold, and the band the citations actually reach |
| `ThermalAnalysis` | the whole attribution ledger and the ordered reason steps |
| `ThermalEnvelope`, `TopologyEdge`, `CouplingRelation` | `declared_by` or `asserted_by`, the declaration instant and the declared basis or method |

### EvidenceState is seven values, not two

`EvidenceState` has `fresh`, `stale`, `unknown`, `conflicting`, `unsupported`, `indeterminate` and
`refused`. `is_usable()` is true only for `fresh`, and callers must ask for it explicitly rather
than
testing truthiness. The difference is load-bearing:

* no observation recorded produces `unknown`, never a default temperature;
* current readings from two different sources that disagree beyond the agreement tolerance produce
  `conflicting`, never an average, and never a self-conflict from one source restating a value;
* a subject whose only limit is missing produces headroom with state `unsupported`, never unlimited
  headroom;
* an entity that is hot but connected to nothing is reported as an episode of one member and is
  also listed in `ungrouped` with an attribution limit, so the gap in the connectivity evidence is
  visible rather than folded into the episode list.

### Correlation is never causation

`CouplingKind::kCoincidental` records that two entities move together.
`CouplingRelation::is_traversable()`
returns false for it, `group_hotspots` skips it, and `find_propagation` skips it and counts it in
`coincidental_excluded`. The analysis also records an `AttributionLimitCode::kCoincidentalCoupling`
limit naming how many such relations were not traversed. There is no code path in which a
correlation becomes a propagation claim.

`AttributionLimitCode::kAuthorityBoundary` is reserved in the ledger vocabulary for a claim that
belongs to an adjacent authority. No current code path raises it; the ledger's other eighteen codes
are the ones this runtime produces.

## Refused operations and their codes

| Operation | Refused by | Code |
| --- | --- | --- |
| Applying a fence record by hand, through the tool | the tool's record dispatch | `ErrorCode::kUnsupported` |
| Registering an entity that declares no class | `EntityRecord::validate` | `ErrorCode::kUnsupported` |
| Registering an envelope bound to neither an entity nor an entity class | `ThermalEnvelope::validate` | `ErrorCode::kUnsupported` |
| An envelope whose opening band and ceiling are missing, or whose bands are out of order | `ThermalEnvelope::validate` | `ErrorCode::kInvalidArgument` |
| A source that declares no authority, no source kind or no clock domain | `Provenance::validate` | `ErrorCode::kUnsupported` |
| An observation that does not say where it was measured | `TemperatureObservation::validate` | `ErrorCode::kUnsupported` |
| A temperature below absolute zero or above the plausibility ceiling | `TemperatureObservation::validate` | `ErrorCode::kOutOfRange` |
| A non-finite temperature | `TemperatureObservation::validate` | `ErrorCode::kIndeterminate` |
| An observation whose declared identity does not match its content | `EvidenceStore::ingest` | `ErrorCode::kConflict` |
| A coupling relation, envelope or derating record whose identity does not match its content | the registry's `add` | `ErrorCode::kConflict` |
| A coupling relation that cites nothing | `CouplingRelation::validate` | `ErrorCode::kUnsupported` |
| A declared adjacency that does not cite the topology it rests on | `CouplingRelation::validate` | `ErrorCode::kUnsupported` |
| A thermal or coincidental claim citing fewer than two distinct observations | `CouplingRelation::validate` | `ErrorCode::kUnsupported` |
| A coupling relation that names itself, or a topology edge that joins an entity to itself | `CouplingRelation::validate`, `TopologyEdge::validate` | `ErrorCode::kInvalidArgument` |
| A coupling strength outside `(0, 1]` | `CouplingRelation::validate` | `ErrorCode::kOutOfRange` |
| A derating claim that does not say what was reduced | `DeratingEvidence::validate` | `ErrorCode::kUnsupported` |
| A derating claim that declares no source kind, or cites no observation | `DeratingEvidence::validate` | `ErrorCode::kUnsupported` |
| A derating claim whose trigger is the nominal band | `DeratingEvidence::validate` | `ErrorCode::kInvalidArgument` |
| A derating magnitude outside `(0, 1]` | `DeratingEvidence::validate` | `ErrorCode::kOutOfRange` |
| A hotspot threshold, or a query focus, of nominal | `HotspotPolicy::validate`, `ThermalQuery::validate` | `ErrorCode::kInvalidArgument` |
| A commit queue with no room for new evidence | `ThermalObservatory::ingest` | `ErrorCode::kQueueFull` |
| Any mutation on a runtime that was never opened or was closed | every public method | `ErrorCode::kClosed` |
| A second process opening the same durable store | `SingleWriterLock::acquire` | `ErrorCode::kLocked` |

Three refusals are states rather than `Status` codes, because the request was legitimate and the
evidence simply does not support an answer:

* no envelope applies to a subject: `HeadroomReport::state` is `unsupported` and all five bands are
  listed as carrying no headroom;
* a derating claim's cited evidence never reaches the band the claim names:
  `DeratingEntry::state` is `conflicting` and the entry names the band the citations actually
  reach;
* nothing traversable leaves the origin: `PropagationResult::state` is `unsupported` and the reason
  says that coincidence is recorded but never traversed.

The fence rules are the same shape. Evidence with an older epoch is refused with
`ErrorCode::kStaleEpoch`, an older generation within the epoch with `ErrorCode::kStaleGeneration`,
an
older revision or a reordered sequence inside the generation with `ErrorCode::kReplayRejected`, and
evidence from a different process incarnation is refused as incomparable with
`ErrorCode::kConflict`. Recovery never promotes an older record: the freshness of recovered
evidence is decided against the evaluation instant like any other evidence, so a reading recovered a
day later resolves to `stale`.

## What the surface does not contain

The boundary is also visible in what is absent. The record vocabulary has exactly seven kinds
(`observation`, `fence`, `envelope`, `entity`, `adjacency`, `coupling`, `derating`), none of which
names an action to take. `DeratingKind` has five values, all of which describe a reduction that
someone else already applied. The tool has exactly seven commands (`version`, `selfcheck`, `apply`,
`analyze`, `explain`, `inspect`, `bench`), and no flag that changes a limit, a zone or a policy.
There is no code path that opens a device, a driver, a socket or a subprocess.
