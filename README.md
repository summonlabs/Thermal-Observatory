# Thermal Observatory

Thermal Observatory is the thermal observation, explanation and attribution layer of the Summon
Software Labs data centre control plane. It answers one question, deterministically and with the
evidence for every part of the answer:

> Given the thermal evidence it holds, where is heat accumulating, how much headroom remains, which
> hotspots and propagation paths does that evidence support, what is derated, and where is the
> attribution uncertain?

It is a portable C++20 library with no dependencies beyond the standard library and the platform's
threading primitives. It ships a command line tool, an installable CMake package, three worked
examples, a benchmark harness and a test suite that proves its claims against real files, real
device flushes and real processes.

---

## What this repository is not

This is the part that matters most, so it comes first.

Thermal Observatory **observes**. It does not:

* actuate cooling, pumps, fans, valves or any other thermal actuator;
* govern thermal policy, set limits, or decide what a safe temperature is;
* own a thermal zone, create one, bound one, or arbitrate between two of them;
* place, migrate or evict workloads;
* perform recovery, failover or degradation management;
* control power.

Those belong to adjacent authorities in the same control plane. This runtime composes with them but
never speaks for them. Three type-level rules enforce the boundary:

1. A `ZoneRef` is a **reference** to a thermal zone owned by another authority. This runtime records
   which zone an entity is said to belong to so that a report can name it, and it has no operation
   that creates, bounds, governs or actuates a zone.
2. `AuthorityLevel` (measured, derived, modeled, synthetic) and `SourceKind` are **declared by the
   producer**. Nothing in this runtime upgrades one level into another, and no answer is reported
   without the authority it rests on.
3. `EvidenceState` has seven values, not two. "We have no evidence", "we have evidence that
   disagrees", and "this runtime is not the authority for that claim" are different answers from
   "fine". Any code that treats a non-`fresh` state as success is a bug.

Observation is not ownership. Acknowledgement is not effect. Configured state is not observed state.
Recovered evidence is not fresh evidence. The runtime is built so that each of those sentences is
enforced by a type rather than by a comment.

---

## The core question, answered concretely

```cpp
#include "tobsv/tobsv.hpp"

ThermalObservatory observatory;
ObservatoryConfig config;
config.log_path = "observatory.log";   // omit for a purely in-memory runtime
observatory.open(config);

observatory.ingest(observation);                    // evidence in
observatory.register_envelope(envelope);            // declared limits in
observatory.record_coupling(relation);              // coupling evidence in

ThermalQuery query;
query.evaluated_at = Timestamp::now_utc().value();
Result<ThermalAnalysis> analysis = observatory.analyze(query);

analysis.value().headroom;      // per subject: signed distance to every declared band
analysis.value().transitions;   // threshold crossings since the previous evaluation
analysis.value().hotspots;      // grouped episodes, with the links that justify each grouping
analysis.value().propagation;   // bounded paths, each with its weakest link
analysis.value().deratings;     // derating claims, checked against the evidence they cite
analysis.value().attribution;   // every reason the answer above is weaker than it looks
analysis.value().reason_steps;  // ordered, human-readable explanation of the composition
analysis.value().digest;        // content fingerprint of the whole document
```

Everything in that result is deterministic. The same records, the same evaluation instant, the same
policy and the same prior evaluation history produce byte-identical JSON and the same digest, on
every run and every platform. One qualification is worth stating plainly: the document includes
threshold transitions, and a transition is by definition a comparison against the previous
evaluation, so two consecutive analyses of an unchanged record set can differ in that one field. Two
independent runtimes that saw the same records in the same order produce the same digest.

---

## Architecture

```
        ingest                    declared                    asserted
          |                        limits                    evidence
          v                          |                          |
   +-------------+   +---------------------+   +-----------------------------+
   |  evidence   |   |      envelope       |   |  coupling / derating /      |
   |  store      |   |  envelope+headroom  |   |  attribution registries     |
   +-------------+   +---------------------+   +-----------------------------+
          |                    |                             |
          +--------------------+-----------------------------+
                               v
                     +-----------------------+
                     |       analysis        |   headroom, transitions, hotspot
                     |  answers the question |   episodes, propagation, derating,
                     +-----------------------+   attribution ledger, reason steps
                               |
                               v
                     +-----------------------+
                     |      persistence      |   versioned CRC log, single-writer
                     |  commit worker, lock  |   lock, torn-tail recovery
                     +-----------------------+
```

| Layer | Headers | Owns |
| --- | --- | --- |
| Core | `tobsv/core/*` | Checked arithmetic, stable digests, CRC-32C, strong identities, strict JSON, time, resource limits |
| Model | `tobsv/model/*` | Vocabulary, fencing counters, observation topology, entity inventory |
| Evidence | `tobsv/evidence/*` | Observations, provenance, quality, freshness resolution, the bounded store |
| Analysis inputs | `tobsv/envelope`, `hotspot`, `coupling`, `derating`, `attribution` | Envelopes and headroom, episode grouping, coupling evidence and traversal, derating appraisals, attribution limits |
| Composition | `tobsv/analysis` | The single deterministic answer and its explanation |
| Persistence | `tobsv/persistence/*` | Record codec, append-only log, single-writer lock, background committer |
| Runtime | `tobsv/runtime` | The composed facade, concurrency ownership, recovery reporting |

All layers are declared in [tobsv.hpp](include/tobsv/tobsv.hpp). The detailed design is in
[docs/architecture.md](docs/architecture.md), the boundary in [docs/boundaries.md](docs/boundaries.md),
the evidence rules in [docs/evidence-model.md](docs/evidence-model.md), the analysis rules in
[docs/hotspots-and-coupling.md](docs/hotspots-and-coupling.md), durability in
[docs/persistence.md](docs/persistence.md), the concurrency audit in
[docs/concurrency.md](docs/concurrency.md), the test strategy in [docs/testing.md](docs/testing.md),
what is not proven in [docs/limitations.md](docs/limitations.md), and the tool in
[docs/cli.md](docs/cli.md).

### State, evidence and authority model

Every observation carries a **fence**: epoch, generation, revision, incarnation, sequence and a
retry attempt identity. The rules are absolute:

* evidence from an older epoch is refused, never promoted to current;
* evidence from an older generation inside the current epoch is refused;
* an older revision inside the current generation is refused as a replay;
* the current revision claimed with different content is refused as a conflict;
* a different process incarnation is **incomparable**, and refusal is the only honest answer;
* replaying the identical observation is idempotent and changes nothing.

A subject - one sensor of one entity at one measurement site - resolves to exactly one of seven
states: `fresh`, `stale`, `unknown`, `conflicting`, `unsupported`, `indeterminate` or
`refused`. Two readings of the same entity at different sites are never substituted for one
another. Within one source the newest reading supersedes that source's earlier readings, ordered by
observation instant and then by the source's own fence, so a sensor ramping from 30 C to 41 C is one
subject reporting a change rather than two sensors in conflict; the superseded readings are listed
separately. Only readings from **different** sources can disagree, and a disagreement beyond the
agreement tolerance produces `conflicting`, not an average. A reading timestamped further into the
future than the tolerated clock skew produces `indeterminate`, because a timestamp that cannot be
placed on the timeline cannot be used.

Thermal coupling is a claim, and claims need citations. A relation of kind `supported_thermal`
requires at least two **distinct** observations; repeating one citation is still one observation. A
relation of kind `coincidental` records that two entities move together and is **never traversed**:
correlation is not propagation, and the graph refuses to pretend otherwise.

---

## Building

Requirements: a C++20 compiler (MSVC 19.3x, GCC 11+, or Clang 14+), CMake 3.22 or newer, and a
generator. There are no third-party dependencies.

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

CMake presets are provided:

```sh
cmake --preset release   # Ninja, MSVC x64, Release, everything on
cmake --build --preset release
ctest --preset release

cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset asan      # Debug plus AddressSanitizer
cmake --build --preset asan
ctest --preset asan
```

Options: `THERMAL_BUILD_TOOLS`, `THERMAL_BUILD_TESTS`, `THERMAL_BUILD_EXAMPLES`,
`THERMAL_BUILD_BENCHMARKS`, `THERMAL_ENABLE_ASAN`, `THERMAL_WARNINGS_AS_ERRORS`. All of the
build products default to on when the project is top level.

First-party warnings are zero in every configuration: MSVC builds with `/W4 /WX /permissive-`, and
GCC and Clang build with a comparable set including `-Werror`.

---

## Using the library

### Ingest evidence

```cpp
TemperatureObservation observation;
observation.entity = EntityId::unchecked("node-01");
observation.sensor = SensorId::unchecked("sensor-outlet-4");
observation.site = MeasurementSite::kOutlet;
observation.celsius = 41.5;
observation.observed_at = Timestamp::parse("2026-02-14T09:31:07Z").value();
observation.received_at = observation.observed_at;
observation.provenance.source = SourceId::unchecked("facility-telemetry");
observation.provenance.authority = AuthorityLevel::kMeasured;
observation.provenance.kind = SourceKind::kFacilitySensor;
observation.provenance.clock = ClockDomain::kCollectorWallClock;
observation.provenance.method = "facility outlet thermocouple";
observation.quality.supplied = true;
observation.quality.flags.add(QualityFlag::kCalibrated);
observation.fence.source = observation.provenance.source;
observation.fence.epoch = Epoch::from(1);
observation.fence.generation = Generation::from(1);
observation.fence.revision = Revision::from(1);
observation.fence.incarnation = Incarnation::from(1);
observation.fence.sequence = Sequence::from(1);
observation.fence.attempt = AttemptId::unchecked("att-0001");
observation.id = compute_observation_id(observation);   // content addressed

IngestOutcome outcome = observatory.ingest(observation);
// outcome.kind is one of recorded, duplicate, refused, conflict
```

An observation whose declared identity does not match its content is refused: the identity is a
function of the measurement, not a label a producer may choose.

### Declare an envelope

An envelope is what an authority declared, not what this runtime decided.

```cpp
ThermalEnvelope envelope;
envelope.entity = EntityId::unchecked("node-01");
envelope.entity_class = EntityClass::kComputeNode;
envelope.site = MeasurementSite::kOutlet;
envelope.nominal_c = 20.0;
envelope.has_warn = true;      envelope.warn_c = 35.0;
envelope.has_high = true;      envelope.high_c = 40.0;
envelope.has_critical = true;  envelope.critical_c = 45.0;
envelope.maximum_c = 50.0;
envelope.declared_by = SourceId::unchecked("facility-policy");
envelope.declared_at = base_instant;
envelope.basis = "facility thermal policy revision 12";
envelope.id = compute_envelope_id(envelope);
observatory.register_envelope(envelope);
```

Bands that the envelope does not declare are reported as missing. A missing band is never unlimited
headroom. An entity with no applicable envelope produces `unsupported` headroom and an explicit
attribution limit, not a default limit.

### Run an analysis

```cpp
ThermalQuery query;
query.evaluated_at = Timestamp::parse("2026-02-14T09:31:07Z").value();
query.freshness.window = Duration::from_seconds(60.0).value();
query.freshness.agreement_tolerance_c = 0.5;
query.hotspots.threshold = ThresholdLevel::kWarn;
query.propagation.max_depth = 4;
query.include_propagation = true;

Result<ThermalAnalysis> analysis = observatory.analyze(query);
```

### Headroom arithmetic

Headroom is the exact signed distance from the observed temperature to a band limit, computed with
checked arithmetic. It is never clamped: a negative headroom is an exceedance and stays negative.
The consumed fraction of a band is undefined, and reported as undefined, when the envelope declares
no nominal band strictly below that limit.

---

## Command line

The tool speaks the same record format the durable log uses, so anything it accepts is something the
log can hold and anything the log holds can be fed back through it.

```sh
thermal-observatory version
thermal-observatory selfcheck --pretty
thermal-observatory apply   --log store.log --in records.jsonl
thermal-observatory analyze --log store.log --at 2026-02-14T09:31:07Z --pretty
thermal-observatory explain --log store.log --at 2026-02-14T09:31:07Z
thermal-observatory inspect --log store.log
thermal-observatory export  --log store.log --out records.jsonl
thermal-observatory bench 32 8 500 500
```

A record file holds one canonical JSON document per line:

```json
{"codec":1,"kind":"entity","payload":{"entity":{"entity":"node-01","entity_class":"compute_node","label":"Rack 1 node 1","site":"hall-1"}}}
```

The payload is nested under the record kind, so a document names its own shape twice and a record
whose body belongs to a different kind is refused rather than half-applied. `export` writes a log
back out in exactly that form, so a store can be replayed into another store with no conversion
step; exporting a log, applying the export to a second log and exporting that produces a
byte-identical file and an identical analysis digest. Every command and option is documented in
[docs/cli.md](docs/cli.md).

---

## Persistence and recovery

The durable store is a single append-only segment with a 64-byte header and length-and-checksum
framed records. The **commit point** of a record is the successful completion of one write of the
whole frame followed by a device flush. Nothing before that point is a committed record.

Recovery is conservative and explicit:

| Condition | Result |
| --- | --- |
| Frame runs past the end of the file | Torn tail. Everything from the start of that frame is discarded and counted. |
| Frame fits, checksum fails, and it ends exactly at end of file | Torn tail. A partial device write can leave a complete length with a partial body. |
| Frame fits, checksum fails, and there are bytes after it | **Interior corruption. The load is refused outright.** |
| File shorter than a header | No record could ever have been committed; the log is re-initialised and the fact is reported. |
| Wrong magic, wrong format version, wrong header checksum | Refused with the specific error code. |

Silently discarding the middle of a log would hide exactly the damage an operator needs to see, so
it is never done. Recovered evidence is replayed into the record set but is **never promoted to
current**: the freshness decision is still made against the evaluation instant, and a reading
recovered a day later resolves to `stale`.

A kernel-enforced single-writer lock guards the store: `LockFileEx` on Windows and `flock` on
POSIX. Both are released automatically when the holding process dies, so a crash does not leave a
permanently locked store. The POSIX lock is advisory, and that limitation is stated rather than
hidden. A second process is refused with `ErrorCode::kLocked`; read-only inspection of a live store
remains possible, because exclusion is the lock's job and not the file sharing mode's.

Full detail, including the byte layout and the recovery policy, is in
[docs/persistence.md](docs/persistence.md).

---

## Concurrency

A single state mutex guards every in-memory structure. A background committer owns the only writable
handle to the log and is the only thread that ever touches it. The lock order is state mutex before
committer mutex, and it is acyclic because the committer thread never takes the state mutex. No
callback is ever invoked while a lock is held. Shutdown releases the committer mutex before joining,
and drains the queue before it stops.

The full audit - including the read-to-write upgrade question, re-entrancy, join ordering,
cancellation, stale-authority races and concurrent publication - is in
[docs/concurrency.md](docs/concurrency.md).

---

## Validation

Every claim below was produced by running the thing being claimed.

### Tests

```sh
ctest --test-dir build/release --output-on-failure
```

Fifteen suites holding 205 tests, plus the packaging proof:

| Suite | What it proves |
| --- | --- |
| `core` | Checked arithmetic, identity grammar, strict JSON, time round trips, CRC and digest vectors |
| `model` | Vocabulary round trips, counter saturation, fence ordering, tracker verdicts, topology and inventory rules |
| `evidence` | Freshness, staleness, conflict, unknown, idempotent replay, epoch and revision refusal, forged identity refusal, retention |
| `envelope` | Band ordering and plausibility, selection specificity, exact headroom, consumed fraction, missing bands, transitions |
| `hotspot` | Transitive adjacency and supported-coupling grouping, coincidence exclusion, temporal splitting, canonical episode identity, per-pair link counting, bounds |
| `coupling` | Citation requirements, strength domain, coincidence never traversed, weakest link, direction, depth and path bounds, cycles |
| `derating` | Uncited claims refused, band cross-check, missing and stale citations, deterministic ordering |
| `analysis` | Composition, digest stability, transition memory, attribution limits, propagation origin, canonical JSON |
| `persistence` | Round trip, torn-tail recovery at many truncation offsets, interior corruption refusal, header checks, bounds, lock exclusion across a real second process, atomic publication, strict codec |
| `restart` | Real reopen, fencing high-water survival, recovered evidence is stale not current, deterministic retention replay, sequence continuation, torn tail on a live store, relation survival |
| `concurrency` | Independent producers without loss, concurrent readers and writers, backpressure, drain-on-shutdown, worker refusal after shutdown, two stores in parallel |
| `property` | Seeded randomized and table-driven properties over arithmetic, ordering, encoding fixed points and order independence |
| `adversarial` | Hostile, truncated, corrupted and malformed input across the whole surface |
| `hardening` | Tamper detection, depth and size bombs, control characters, non-finite values, bounded registries, no mutation by analysis |
| `cli` | The real installed tool through a real child process, including lock exclusion against a live store |
| `package.downstream` | Install into a scratch prefix, then configure, build and run an independent `find_package(ThermalObservatory CONFIG)` consumer |

No test declares a timeout, and the framework has none. A hang is a defect to diagnose, not a result
to accept.

### Static analysis and sanitizers

| Check | Result |
| --- | --- |
| MSVC `/analyze` over all 29 first-party translation units | **REAL** - zero diagnostics |
| First-party compiler warnings, MSVC `/W4 /WX` in Release and Debug | **REAL** - zero |
| AddressSanitizer | **UNSUPPORTED on the validation host** - this MSVC installation has no "C++ AddressSanitizer" component, so `/fsanitize=address` cannot link. The `asan` preset and `THERMAL_ENABLE_ASAN` are offered and the build detects the missing runtime at configure time, but no sanitizer result is claimed here. The Debug configuration was run instead, with the checked debug CRT. |

ASan is not claimed as evidence. It is available through the `asan` preset on a toolchain that has
the component, and the option reports a clear configuration error rather than a link failure when it
does not.

### Real, synthetic and unsupported

| Claim | Status |
| --- | --- |
| Durable log, commit point, device flush, reopen, torn-tail recovery, interior-corruption refusal | **REAL** - real files and real device flushes on the machine that ran the tests |
| Kernel single-writer exclusion, including a second real process being refused | **REAL** |
| Install into a clean prefix and consumption through `find_package` from an independent project | **REAL** |
| Concurrency, shutdown, drain and backpressure behaviour | **REAL** - real threads and a real background committer |
| Headroom, threshold, hotspot, coupling, derating and attribution semantics | **REAL** - proven by the test suites |
| The temperatures and couplings used in tests, examples and benchmarks | **SYNTHETIC** - generated in-process |
| Real facility hardware, BMS, DCIM, cooling plant, electrical or multi-node telemetry | **UNSUPPORTED** - no such hardware was available, and no number here claims to measure it |

### Benchmarks

`thermal_bench [nodes] [revisions] [analyses] [durable commits]` measures completed work only: every
sample is a whole operation that returned.

Measured on AMD Ryzen 7 9800X3D, Windows, MSVC 19.44, Release, 32 nodes, 8 revisions:

| Measurement | Workload | n | total | mean | p50 | p99 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Ingest into memory | SYNTHETIC, real code path | 256 | 1.562 ms | 0.0061 ms | 0.0043 ms | 0.0117 ms | 0.0262 ms |
| Full analysis | SYNTHETIC, real code path | 500 | 997.0 ms | 1.9939 ms | 2.0015 ms | 3.4228 ms | 3.5443 ms |
| Durable commit: ingest plus device flush | REAL device flush | 500 | 411.9 ms | 0.8239 ms | 0.8125 ms | 1.1034 ms | 1.5241 ms |

The analysis figure is one full pass over 32 subjects, each with eight revisions of history,
producing headroom for every subject, threshold transitions, 18 hotspot episodes, propagation paths
and derating appraisals, and ending in a content digest. The durable figure is one complete mutation
through to a device confirmation; an empty flush barrier would have measured nothing and is not
reported.

Run it yourself, because the numbers depend on the machine:

```sh
build/release/benchmarks/thermal_bench.exe 32 8 500 500
```

---

## Installing and consuming the package

```sh
cmake --install build/release --prefix /opt/thermal-observatory
```

```cmake
find_package(ThermalObservatory 1.0 REQUIRED CONFIG)
target_link_libraries(your_target PRIVATE ThermalObservatory::thermal_observatory)
```

The package installs headers, the static library, the CMake config and version files, and licensing
documents. The repository's own `package.downstream` test performs exactly that install into a
scratch prefix and then configures, builds and runs an independent consumer project that knows
nothing about this build tree. `tests/downstream/consumer.cpp` is that consumer, and it exercises
the documented surface: registration, ingestion, idempotent replay, analysis, headroom arithmetic,
envelope refusal and the coincidence rule.

---

## Repository layout

```
include/tobsv/      public headers, grouped by layer
src/                implementation, mirroring the header tree
tools/              the thermal-observatory command line tool
examples/           three worked examples
benchmarks/         the measurement harness
tests/              the test suites and the downstream consumer
docs/               design, boundary, evidence, analysis, persistence, concurrency, testing, limits, cli
cmake/              package config template and the downstream verification script
```

---

## Limitations

There is no real hardware integration of any kind, and none is claimed. Persistence is a single
append-only segment with no compaction. Integrity is CRC-32C, which detects corruption and does not
defend against tampering. The POSIX writer lock is advisory. The clock model has no NTP or PTP
discipline: it reports clock-domain disagreement rather than reconciling it. Hotspot grouping is
only as good as the declared adjacency, and propagation is a bounded traversal over declared
evidence rather than a physical thermal model. There is no distributed or multi-node deployment. The
full list is in [docs/limitations.md](docs/limitations.md).

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.