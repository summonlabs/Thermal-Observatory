# The evidence model

Every thermal statement this runtime makes rests on records it was handed. This document describes
those records: what an observation is, what travels with it, how it is identified, what makes it
usable, and exactly which rule turns one subject into one `EvidenceState`.

## TemperatureObservation

One reading, exactly as it arrived, with everything needed to explain later why it was or was not
used.

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `id` | `ObservationId` | yes | content-addressed identity; see below |
| `entity` | `EntityId` | yes | what was observed |
| `sensor` | `SensorId` | yes | which sensor produced the value |
| `site` | `MeasurementSite` | yes, not `kUnknown` | where on the entity the value was measured |
| `celsius` | `double` | yes | the value, finite and plausible |
| `observed_at` | `Timestamp` | yes | when the measurement was taken |
| `received_at` | `Timestamp` | yes | when the runtime was told about it |
| `provenance` | `Provenance` | yes | who says so, how, and on what clock |
| `quality` | `QualityMetadata` | yes, may be unsupplied | what the source said about the value |
| `envelope` | `EnvelopeId` | no | the envelope in force when the reading was taken |
| `fence` | `Fence` | yes | the ordering position of this statement |

`TemperatureObservation::validate` refuses a record that is missing any required identity, that does
not name a measurement site (`ErrorCode::kUnsupported`), that carries a non-finite temperature
(`ErrorCode::kIndeterminate`), that falls outside the plausibility band
(`ErrorCode::kOutOfRange`), that is missing either timestamp (`ErrorCode::kInvalidArgument`), that
fails its provenance or quality validation, that fails fence validation, or whose fence source
differs from its provenance source (`ErrorCode::kConflict`).

The plausibility band is fixed in `vocabulary.hpp` and re-exported by `evidence.hpp`:
`kMinPlausibleCelsius` is `kAbsoluteZeroCelsius` (-273.15) and `kMaxPlausibleCelsius` is
`kPlausibilityCeilingCelsius` (1000.0). Below absolute zero nothing is physical; far above it a
value
is not a data centre measurement, and admitting one would corrupt every aggregate that contained it.

The optional `envelope` field is the envelope that was in force when the reading was taken, not the
envelope the runtime chooses at analysis time. Unset is legal and means "the source did not say
which
envelope applies"; headroom analysis then reports the gap as unsupported evidence rather than
inventing a limit.

## Provenance

`Provenance` answers "who says so, how, and on what clock". Its `source` (`SourceId`) is required,
its
`method` must be non-empty text within the configured length bound and free of control characters,
and three enumerations must be declared by the producer:

* `authority`: `kUnknown`, `kMeasured`, `kDerived`, `kModeled`, `kSynthetic`;
* `kind`: `kUnknown`, `kFacilitySensor`, `kPlatformAgent`, `kExternalTelemetry`,
  `kOperatorDeclaration`, `kSyntheticGenerator`;
* `clock`: `kUnspecified`, `kCollectorWallClock`, `kSensorMonotonic`, `kHostMonotonic`,
  `kSynthetic`.

Leaving any of the three at its default is refused with `ErrorCode::kUnsupported`: a source has to
say how its values were obtained and which clock produced them before any of them are admitted.
`is_synthetic()` is true when the kind is `kSyntheticGenerator` or the authority is
`AuthorityLevel::kSynthetic`. Synthetic evidence is still evidence, but it is evidence about a
generator, and every result that rests on it says so.

The clock domain is carried rather than interpreted. Two observations of one subject that arrive
from
different clock domains cannot be ordered with confidence, and the analysis records that fact as an
`AttributionLimitCode::kClockDomainMismatch` limit instead of ordering them silently. There is no
NTP or PTP discipline in this runtime and it does not attempt to reconcile clock domains.

## QualityMetadata

Quality metadata is optional in the world this runtime observes: many facility sensors publish a
bare
number. The model therefore distinguishes three cases, not two.

| Case | Representation | What the analysis reports |
| --- | --- | --- |
| Nothing was said about quality | `supplied == false`, `is_silent()` true, `is_degraded()` false | an `AttributionLimitCode::kQualityUnsupplied` limit: the value is taken as reported |
| A quality statement was supplied and it is clean | `supplied == true`, no degrading flag | no limit; the value is used as reported |
| A quality statement was supplied and it declares a problem | `supplied == true`, at least one degrading flag, `is_degraded()` true | an `AttributionLimitCode::kQualityDegraded` limit |

An unsupplied statement is never reported as degraded, and the runtime never assumes the good case:
an analysis that depends on quality has to report the missing statement rather than invent it.

`QualityFlag` is a bit set with eight named flags: `kCalibrated`, `kUncalibrated`, `kSuspect`,
`kInterpolated`, `kExtrapolated`, `kSensorFault`, `kRateLimited` and `kDerivedFromModel`.
`QualityFlags::names()` returns the set names in a fixed order and `joined()` returns them separated
by "|", or the single word "none"; the fixed order is what keeps a record's textual form
byte-stable.
`is_degraded()` is true for every flag except `kCalibrated`, and false when nothing was supplied.
`confidence` is optional; when `has_confidence` is set it must be a finite number in `[0, 1]`
(otherwise `ErrorCode::kIndeterminate` or `ErrorCode::kOutOfRange`).

Results carry two booleans rather than the flag set: `ObservationRef::quality_supplied` and
`ObservationRef::quality_degraded`. A consumer that needs the flags reads them from the stored
observation. An `ObservationRef` also carries the source, the value, the observation instant, the
site, the synthetic flag and `fence_revision`, which is the position of that reading in its source's
fencing order and the reason two readings of one subject can be told apart when they share an
instant.

## Fence: the ordering position of one statement

A `Fence` is the complete ordering position of one piece of evidence from one source.

| Component | Type | Meaning |
| --- | --- | --- |
| `source` | `SourceId` | the source the position belongs to |
| `epoch` | `Epoch` | one writer incarnation of the source; a restart or reinitialisation opens a new epoch, and evidence from an older epoch is never promoted to current |
| `generation` | `Generation` | monotonic within an epoch; the source bumps it when it observes a discontinuity |
| `revision` | `Revision` | monotonic within a generation; every accepted mutation bumps it |
| `incarnation` | `Incarnation` | the process incarnation that produced the evidence |
| `sequence` | `Sequence` | per-source monotonic counter used for within-revision ordering and gap detection |
| `attempt` | `AttemptId` | retry identity, carried in the same durable commit as the mutation it describes |

Each counter is a distinct type, so an epoch can never be compared with a generation by accident.
Zero means "never set" and real counters start at one: `Fence::validate` refuses any counter that is
unset and refuses a missing source or attempt. The record codec applies the same rule when it
decodes
a fence, so a zero can never enter through the durable path either. `Counter::next()` saturates at
`UINT64_MAX`; saturation is the conservative choice, because two equal counters classify as a replay
rather than as new evidence.

### Ordering

`order(candidate, reference)` returns:

* `kIncomparable` when the sources differ or the incarnations differ - assigning an order across
  process incarnations would invent an ordering the evidence does not have;
* otherwise `kOlder`, `kEqual` or `kNewer`, comparing epoch, then generation, then revision, then
  sequence in that order.

### Classification

`FenceTracker::classify(candidate, same_content)` returns a `FenceVerdict`, and
`FenceTracker::observe` either moves the high-water mark or returns the corresponding error. The
checks are applied in this order:

| Verdict | Rule | `observe` returns | `EvidenceStore::ingest` yields |
| --- | --- | --- | --- |
| `kAdvance` | no high-water mark yet, or a greater generation, revision or sequence | success, mark moves | `IngestKind::kRecorded` |
| `kNewEpoch` | greater epoch | success, mark moves | `IngestKind::kRecorded` |
| `kDuplicate` | same source, incarnation, epoch, generation, revision and sequence, and `same_content` is true | `ErrorCode::kDuplicateIdentity` | `IngestKind::kDuplicate` |
| `kStaleEpoch` | smaller epoch | `ErrorCode::kStaleEpoch` | `IngestKind::kRefused` |
| `kStaleGeneration` | same epoch, smaller generation | `ErrorCode::kStaleGeneration` | `IngestKind::kRefused` |
| `kReplayRejected` | same generation, smaller revision; or same revision and a smaller sequence | `ErrorCode::kReplayRejected` | `IngestKind::kRefused` |
| `kConflict` | same position, different content | `ErrorCode::kConflict` | `IngestKind::kConflict` |
| `kIncomparable` | a different incarnation | `ErrorCode::kConflict` | `IngestKind::kConflict` |

`same_content` is not a guess: `EvidenceStore::ingest` sets it when the observation identity equals
the last identity accepted from that source. That is what distinguishes an idempotent retry from a
genuine conflict for the same revision slot.

Two rules sit in front of the verdict. Re-delivery of a record whose identity is already held is
idempotent regardless of its fence position, and an observation whose declared identity does not
match its content is refused with `ErrorCode::kConflict` before any fence is considered.

`FenceTracker::force_high_water` restores a mark from durable state without classification. It is
used only when a store is reopened and the persisted high-water marks are replayed, after the
fence's
own integrity has been validated. It refuses a fence that belongs to a different source than the
tracker with `ErrorCode::kConflict`, and it never lowers a mark.

## Content-addressed observation identity

`compute_observation_id` derives an identity from the content of the observation. The digest is
FNV-1a
64-bit over the following fields, in this order, each absorbed with its own terminator:

1. the literal `tobsv.observation.v1`;
2. `entity`;
3. `sensor`;
4. `site`, as its underlying enumeration value;
5. `celsius`, absorbed as the exact IEEE-754 bit pattern, so `-0.0` and `0.0` differ;
6. `observed_at`, as Unix nanoseconds;
7. `provenance.source`;
8. `provenance.authority`;
9. `provenance.kind`;
10. `provenance.clock`;
11. `provenance.method`;
12. `envelope`, as text, empty when unset;
13. the six fence components: epoch, generation, revision, incarnation, sequence and attempt.

The result is `ObservationId::from_digest`, which is the prefix `obs-` followed by the digest in
lower-case hexadecimal, always sixteen digits.

### Why received_at is excluded

`received_at` is required and validated, but it is deliberately not part of the identity digest. The
identity has to be a function of the measurement, not of its delivery:

* a genuine retry re-delivers the same measurement with a new receive time; if the receive time were
  absorbed, the retry would produce a different identity, would not be recognised as a duplicate,
  and
  the same physical reading would be counted twice;
* a store can be exported, re-imported or replayed into a fresh runtime without every identity
  changing;
* the receive time is a property of this runtime's ingestion, not of the evidence, and an identity
  that depended on it would not be reproducible by the producer that created the measurement.

Quality metadata is excluded for the same class of reason: it describes how the value should be
treated, not which measurement it is. The fence, by contrast, is included: the same measurement
re-fenced at a new revision is a new statement from that source, and the per-source rules then
decide whether it is accepted.

The digest is an integrity aid, not an authenticity claim. It is 64-bit FNV-1a, it is not
cryptographic, and an actor who can rewrite a record can rewrite its identity. The runtime does
check
the two against each other: `EvidenceStore::ingest` refuses a mismatch with `ErrorCode::kConflict`,
and the durable codec refuses one with `ErrorCode::kIntegrityFailure`.

Other identities are derived the same way, each with its own domain string: `compute_envelope_id`
(`env-`), `compute_coupling_id` (`cpl-`), `compute_derating_id` (`dr-`) and
`compute_episode_id` (`hot-`). Each absorbs its own fields and sorts its citation keys before
absorbing them, so two relations that cite the same evidence in a different order have the same
identity.

## The freshness policy

`FreshnessPolicy` has three knobs, all validated by `FreshnessPolicy::validate`:

| Member | Default | Rule | Failure |
| --- | --- | --- | --- |
| `window` | 30 seconds (`Limits::default_freshness_window`) | must be positive | `ErrorCode::kInvalidArgument` |
| `max_clock_skew` | 2 seconds | must not be negative | `ErrorCode::kInvalidArgument` |
| `agreement_tolerance_c` | 0.5 | finite and non-negative | `ErrorCode::kInvalidArgument` |

`FreshnessPolicy::from_limits` takes the window from `Limits::default_freshness_window` and leaves
the
other two at their defaults, which is what `ThermalObservatory::analyze_now` does.

The three knobs are used for three different decisions:

* **window**: an observation whose age exceeds it is stale. Age is
  `age_of(observed_at, evaluated_at)`, a saturating subtraction of the observed instant from the
  evaluation instant, so an extreme pair of timestamps clamps rather than wraps.
* **max_clock_skew**: an observation timestamped further into the future than this tolerance cannot
  be placed on the timeline and is indeterminate, not fresh.
* **agreement_tolerance_c**: the spread between the lowest and highest current reading of one
  subject - one reading per source, after supersession - is compared with this tolerance to decide
  between `fresh` and `conflicting`.

A negative age that is within the tolerance stays an ordinary candidate whose age is negative; the
runtime preserves that sign rather than clamping it, so a small clock difference remains visible in
the reported age.

## The SubjectTemperature resolution state machine

A subject is one `(entity, sensor, site)` triple. Two readings of the same entity at different sites
are different subjects and never substitute for one another. The subject key is ordered by entity,
then sensor, then the underlying site value, and every resolution output follows that canonical
order.

`EvidenceStore::resolve` requires a valid policy and a set evaluation instant
(`ErrorCode::kInvalidArgument` otherwise), and an unknown subject is not an error: it resolves to
`unknown` with a reason that names the missing subject.

The algorithm, in order:

1. **Partition every observation of the subject.**
   * no `observed_at` at all, or an age below the negated clock-skew tolerance: the
     **indeterminate**
     list;
   * an age greater than the window: the **stale** list;
   * otherwise: a **fresh candidate**.
2. **If no candidate is fresh**, resolve without a representative:
   * if the stale list is non-empty: state `stale`, and the representative is the newest stale
     observation, ordered by `observed_at`, then fence revision, then identity;
   * else if the indeterminate list is non-empty: state `indeterminate`;
   * else: state `indeterminate` with the reason that observations exist but none carries a usable
     observation timestamp. This fallback is only reachable when a subject ring referenced an
     observation that is no longer held.
   Stale evidence takes precedence over indeterminate evidence, and the reason text names the window
   or the tolerated skew that produced the verdict.
3. **Order the fresh candidates**: descending authority rank (`measured` 4, `derived` 3, `modeled`
   2,
   `synthetic` 1, `unknown` 0), then descending `observed_at`, then descending fence revision, then
   ascending identity.
4. **Supersede within each source.** Walking that order, the first observation seen for a source is
   that source's current reading; every later observation from the same source goes to the
   `superseded` list instead of the candidate set. A source that restates or corrects a value
   replaces its own earlier reading; it does not contradict itself. Only readings from different
   sources can disagree about one subject. Superseded readings are reported rather than dropped:
   `SubjectTemperature::superseded` carries them, and the reason names how many there were.
5. **Choose the representative** as the first remaining candidate, and compute the spread over the
   remaining candidates, which is one current reading per source.
6. **If the spread is within the agreement tolerance**: state `fresh`, every remaining candidate is
   listed as supporting, and the reason names the number of sources, the spread, the representative
   and its authority.
   **Otherwise**: state `conflicting`, the candidates within the tolerance of the representative are
   listed as supporting and the rest as conflicting, and the reason says that the sources disagree
   and by how much. The value is not resolved.
   Both lists are sorted by identity before they are returned.

Every state, and the exact rule that produces it:

| `EvidenceState` | Exact rule | Representative |
| --- | --- | --- |
| `unknown` | the subject has no recorded observation at all | none |
| `stale` | no candidate is fresh and the stale list is not empty | newest stale observation |
| `indeterminate` | no candidate is fresh, the stale list is empty, and at least one observation has no timestamp or is timestamped beyond the tolerated skew into the future | none |
| `fresh` | at least one candidate is fresh and the current readings agree within the tolerance | the best-ranked candidate: authority, then instant, then fence revision, then identity |
| `conflicting` | the current readings span more than the tolerance | the same best-ranked candidate |
| `unsupported` | not produced by resolution; produced by headroom when no envelope applies | n/a |
| `refused` | not produced by resolution; produced by hotspot grouping when one episode would exceed the member bound | n/a |

### Supersession, and why one source cannot conflict with itself

A subject is observed by sources, and one source may send the same subject more than once - a
corrected value, a restated value, a sensor ramping between two samples. Treating those readings as
independent witnesses would make a single sensor ramping from 30 C to 41 C look like two sensors in
conflict. Resolution therefore keeps, per source, only that source's best-ranked current reading:
highest authority, then newest observation instant, then highest fence revision, then smallest
identity. Everything else is listed in `SubjectTemperature::superseded` with its source and fence
revision, so the reading is reported as historical rather than hidden.

The consequence is exact: `conflicting` means that two or more *different* sources disagree, and the
`conflicting` list can never contain two readings of one source. `ObservationRef::fence_revision` is
what makes both the ordering and the supersession explanation possible, because two readings of one
subject can share an observation instant and only the fence tells them apart.

Two states in the vocabulary are therefore outside resolution. `unsupported` means a claim cannot be
supported by the evidence model at all - for example, no envelope exists to measure headroom
against.
`refused` means the runtime declined the operation because of a bound. Neither is ever reported as a
success, and `is_usable()` is true only for `fresh`.

Usage of the resolution step is direct:

```cpp
  EvidenceStore store(limits);
  const Timestamp at = Timestamp::parse("2026-02-14T09:31:07Z").value();
  const FreshnessPolicy policy = FreshnessPolicy::from_limits(limits);

  const Result<SubjectTemperature> subject =
      store.resolve(EntityId::unchecked("node-01"), SensorId::unchecked("sen-outlet-1"),
                    MeasurementSite::kOutlet, at, policy);
  if (subject.ok() && subject.value().is_usable()) {
    const double celsius = subject.value().representative_celsius;
    const AuthorityLevel authority = subject.value().representative_authority;
    const std::string why = subject.value().reason;
  }
```

## Bounds, retention and the cost of forgetting

Every collection in the store is bounded by `Limits`, and each insertion path checks its bound:

* `max_observations` is a global bound. Reaching it refuses new evidence with
  `ErrorCode::kLimitExceeded`; nothing is evicted to make room.
* `max_observations_per_subject` is a per-subject ring. Reaching it retires the oldest observation
  of
  that subject, in `(observed_at, id)` order, and increments `retired_count()`. Retirement is
  counted, never silent, and the analysis reports it as an `AttributionLimitCode::kRetiredEvidence`
  limit because a retired observation can no longer be cited by a derating claim.

Retirement can drop the observation that carried the highest fence for its source. The runtime
therefore checkpoints that source's high-water mark durably at the moment retention changes, so a
restart cannot lower the mark and let superseded evidence back in. See
[persistence.md](persistence.md).
