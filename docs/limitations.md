# Limitations

This is the honest list. Every item below is a consequence of the code in this repository, not a
hedge. Where a claim is limited, the limit is stated with the reason it exists.

## There is no hardware integration, and none is claimed

The library opens no device, no driver and no privileged API. It opens no socket at all: there is no
networking code in `src/`. Everything the runtime knows arrived through a function call or a record
file. There is no BMS integration, no DCIM integration, no cooling-plant integration, no electrical
telemetry integration and no multi-node telemetry integration, and the repository contains no code
that would perform one.

Every temperature, coupling and derating claim used by the test suites, the examples and the
benchmark harness is synthetic and generated in process. The harness labels that fact in its own
output: it reports either `EMPTY` (no evidence loaded, measuring the fixed cost of an analysis over
an
empty store) or `SYNTHETIC-OR-LOADED` (the cost of analysing whatever was actually loaded). There is
no benchmark of real facility hardware, because no real facility hardware was available.

## The runtime never actuates anything

No type in `include/tobsv` names a fan, pump, valve, cooling unit or power control, and no function
writes to a device. `DeratingEvidence` records that an entity *is* being held below its capability;
the runtime checks the claim against the cited thermal evidence and reports the result. It cannot
cause, schedule, approve or reverse a derating. The tool has no flag that changes a limit, a zone, a
policy or a placement. The only output of the runtime is a value returned to its caller and a record
appended to its own log.

## Persistence is one append-only segment

* There is **no compaction and no rotation**. The durable log keeps every record it ever accepted,
  including records that retention has since removed from memory. Its only bound is
  `Limits::max_segment_bytes` (1 GiB by default); an append that would grow the segment past that
  bound is refused with `ErrorCode::kLimitExceeded`, and at that point the store stops accepting new
  evidence until an operator archives or replaces the file. There is no built-in archival tool.
* Retention is an in-memory policy only. A retired observation is gone from the record set but its
  frame is still in the log, and a reopen replays it and retires it again deterministically.
* The format version is fixed at 1 and a reader refuses any other major version outright rather than
  making a best-effort interpretation. There is no migration path and no downgrade path.
* Interior corruption is refused, never repaired. There is no repair tool in this repository: a log
  with damage in the middle cannot be loaded by this build, and the bytes after the damage are not
  scanned.
* A torn tail is discarded - counted, reported, and gone. Those bytes were never a committed record,
  so there is nothing to recover from them.
* `read_file_bytes` sizes a file through `std::ftell`, whose result type is `long`. On a platform
  where `long` is 32 bits, the practical size of a file that can be read in one call is bounded by
  that type as well as by the configured byte bound.

## Integrity is CRC-32C, which detects corruption and not tampering

The log header and every frame carry a CRC-32C, and records are addressed by a 64-bit FNV-1a content
digest. Both are integrity aids:

* CRC-32C detects accidental corruption. An actor who can rewrite a payload can rewrite its
  checksum, so it is not a defence against tampering.
* The identity digests are not cryptographic, and the runtime does not attempt to make them
  collision resistant against a deliberate adversary. A content-addressed identity is checked
  against
  the content it claims to describe, which catches accident and carelessness, not forgery.
* Authority is declared, not verified. A source's authority level and integration kind are supplied
  by the caller, and a durable store from an untrusted source must be treated as untrusted input.
  The analysis reports the declared authority in every result so that a consumer can judge it.

## The POSIX writer lock is advisory

On Windows the single-writer lock is a byte-range `LockFileEx` and the kernel enforces it. On POSIX
it
is an `flock`, which is released when the holder dies and which a program that deliberately bypasses
`flock` is not stopped by. Neither lock is a security boundary: it is a correctness mechanism that
prevents two cooperating writers from interleaving frames in one file.

## The clock model has no discipline and cannot reconcile clock domains

* There is no NTP client and no PTP client. `Timestamp::now_utc` reads the system wall clock, and
  the
  configuration has no notion of a time source.
* Observations carry a declared `ClockDomain`, and a subject whose observations arrive from more
  than
  one domain gets an `AttributionLimitCode::kClockDomainMismatch` limit. The runtime **reports** the
  disagreement; it does not reconcile the domains, and it does not attempt to estimate an offset.
* An observation timestamped further into the future than the tolerated clock skew cannot be placed
  on the timeline and resolves to `indeterminate`. The runtime does not correct the timestamp and
  does
  not silently clamp it into the window.
* Ages are computed with a saturating subtraction. An extreme pair of timestamps clamps rather than
  wrapping, which is safe and also means an absurd age is reported as a saturated value rather than
  as a diagnostic.
* Leap seconds are not representable: a timestamp with a seconds field of 60 is refused as
  malformed.
* Nothing is localized. Every instant is UTC with a fixed nine-digit fraction.

## Hotspot grouping is only as good as the declared adjacency

Adjacency is a declaration carrying a topology reference and a declaring source. The runtime does
not
verify it, and it has no other source of physical structure:

* two entities that share an enclosure but were never declared adjacent cannot be grouped, however
  obviously they are related;
* grouping is transitive over the declarations, so a hot entity can be one episode with another
  through entities that are not themselves hot; the episode lists only the hot subjects, and the
  reason says that members may also be joined through entities that are not hot;
* two entities declared adjacent are grouped even if the declaration is wrong;
* an episode joined only by adjacency leaves the source of the heat ambiguous, which the analysis
  reports as `AttributionLimitCode::kJointEnclosureAmbiguity` - the runtime knows it cannot tell
  which
  member heated the other, and says so instead of guessing;
* grouping also depends on time: members observed further apart than the episode gap are split into
  separate episodes even when they are physically connected, and the gap is a policy number rather
  than a measurement.

## Propagation is a bounded traversal over declared evidence, not a thermal model

`find_propagation` walks a graph of coupling relations that somebody declared. It contains no heat
transfer equation, no conductance, no airflow model, no coolant model, no time constant and no
capacity. A relation's `strength` is a declared number in `(0, 1]`, and the weakest-link rule is
arithmetic on those declarations. Consequences:

* a coupling that was never declared is invisible, so "no path" means "no declared path", not "no
  physical path";
* a path's strength is not a temperature prediction and does not say how much heat moves, how fast,
  or in what direction heat actually flows - direction is only what the relation declares;
* the search is bounded by depth, path count and an exploration budget, and any of the three can
  stop
  it early. The result says which bound applied (`depth_limited`, `truncated_paths`,
  `search_bounded`) rather than presenting a partial answer as exhaustive.

## There is no distributed or multi-node deployment

There is one runtime, one store and one writer. There is no replication, no consensus, no quorum, no
cross-host merging of evidence and no coordination between runtimes. Two hosts cannot share a store:
the lock is a local file lock, and the file is local. Merging two stores means feeding the records
of
one into the other, and the fence rules will then refuse contradictory positions for one source
rather than reconcile them - two writers that used the same source identity but different
incarnations are incomparable by design.

## Analysis is not attribution of cause

The runtime reports where heat is accumulating, how much headroom remains, which hotspots and paths
the evidence supports, and what is derated. It never says which workload, tenant, process or device
caused a temperature. `AttributionLedger` is a ledger of what weakens the answer, not a claim about
what produced the heat.

## Memory is bounded, and the bounds are the operator's responsibility

The whole record set lives in memory, bounded by `Limits`. The defaults are what actually enforce
that: raising them toward the hard ceilings in `limits.hpp` raises memory use proportionally, and
nothing in the runtime reports how close a deployment is to its bound. Reaching the global
observation bound refuses new evidence with `ErrorCode::kLimitExceeded` rather than evicting, so a
misconfigured bound looks like a refusal rather than a slowdown. The commit queue likewise refuses
with `ErrorCode::kQueueFull` rather than growing without limit.

The plausibility band is a hard filter: a value below -273.15 C or above 1000 C is refused, and a
non-finite value is refused. A genuinely exotic sensor cannot be represented in this model.

## Envelope selection ignores recency

Selection is by specificity (entity, then site, then class) and a tie is broken by the
lexicographically smallest envelope identity. `declared_at` is recorded and reported but plays no
part in selection, and there is no notion of superseding an envelope. Registering a newer, equally
specific envelope therefore does not necessarily displace the older one; the operator must remove
the
losing declaration from the store, which this runtime cannot do because the registry has no removal
operation.

## Transition history does not survive a restart

`TransitionMemory` is in memory, owned by the caller or by the runtime, and it is cleared by
`ThermalObservatory::open`. After a reopen, the first analysis of each subject reports
`ThresholdTransition::kNone` with the reason that no earlier evaluation is available, which is true
-
and which also means a threshold crossing that happened across a restart is not reported as a
crossing. The memory is also pruned: once it holds more subjects than `Limits::max_observations`,
every subject that did not appear in the analysis just completed is forgotten. A runtime that keeps
meeting new subjects therefore does not remember them for the lifetime of the process, and equally a
subject that disappears from the evidence can lose its transition history without any restart.

## Quality metadata is optional, and it is not part of an observation's identity

An observation's identity digest covers the measurement, its provenance and its fence, and
deliberately does not cover the quality flags, the confidence or the receive time (see
[evidence-model.md](evidence-model.md)). Two deliveries of the same measurement that differ only in
quality therefore have the same identity, and the second is an idempotent duplicate rather than a
quality update. A source that wants to revise the quality of a measurement has to move its fence
revision, which makes the record a new statement.

## Some vocabulary entries have no producer in this build

The enumerations are complete vocabularies, and a few members exist so that a consumer can name a
condition rather than because this build produces it:

* `ErrorCode::kCancelled` is never returned by any code path; there is no cancellation.
* `AttributionLimitCode::kAuthorityBoundary` is never raised; it is reserved for a claim that
  belongs
  to an adjacent authority.
* `EvidenceState::kUnsupported` and `EvidenceState::kRefused` are not produced by subject
  resolution; they come from headroom (no applicable envelope) and from hotspot grouping (an episode
  above the member bound) respectively.
## The writer lock is keyed on the path, not on the file's identity

The single-writer lock is a second file whose name is the log path plus `.lock`, and the exclusion
is over that file. Two processes that reach the same log through two different names - a hard link,
a symbolic link, an 8.3 short name, or a substitute drive - compute two different lock paths, take
two different locks, and both append to the same log with no kernel exclusion between them. The
guard is therefore only as strong as the agreement about the path. Deployments that need stronger
exclusion should reach each store through one canonical path, which is what a service manager
normally provides.

## AddressSanitizer is not available on the machine this release was validated on

The `asan` preset and the `THERMAL_ENABLE_ASAN` option exist and are correct, but the MSVC
installation used for the 1.0.0 validation has no "C++ AddressSanitizer" component, so
`/fsanitize=address` cannot link and the build now reports that at configure time rather than
failing later with a missing `clang_rt` library. **No sanitizer result is claimed for this
release.** What was run instead, and what the claims rest on, is the Debug configuration with the
checked debug CRT across all 205 tests, and MSVC `/analyze` over all 29 first-party translation
units with zero diagnostics. On a toolchain with the component installed, the preset produces a
sanitized build and the same suites run under it.

## Integrity and range limits that were tightened for 1.0.0

Three behaviours are worth stating because an operator may notice them:

* A log path that names a directory is refused. Before this was explicit, a zero-length directory
  could be removed and replaced by a log file, because a directory reports a size of zero.
* A file that exists but holds no bytes is treated as having no header and is re-initialised. It
  previously passed as a valid empty log, and the first commit then overwrote the magic at offset
  zero and left every committed record unreachable.
* `max_observations_per_subject` must be at least 2. A ring of one would retire a subject's only
  observation to make room for its replacement and then refuse the replacement, destroying evidence
  while reporting a refusal.
