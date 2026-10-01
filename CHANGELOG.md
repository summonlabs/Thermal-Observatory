# Changelog

All notable changes to Thermal Observatory are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[semantic versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.1] - 2026

Patch release. Fixes the Linux build. Nothing about the runtime's behaviour,
its record format or its public interface changed.

### Fixed

* The analysis composition no longer defines a helper it does not use. Hotspot grouping and
  derating appraisal keep and use their own copies of it; the one left behind when threshold
  transition classification moved was dead. GCC and Clang reject an unused function of internal
  linkage under `-Werror`; MSVC does not warn about it, so only the Linux jobs saw it.
* The command line tool includes `<algorithm>` for `std::sort` rather than relying on the MSVC
  standard library to provide it transitively, which libstdc++ does not.
* The test framework header ends with a newline, as a source file must.
* The test helper builds its child process command line for the shell that will actually run it.
  The extra pair of quotes that defeats cmd.exe's quote removal left an unterminated string in a
  POSIX shell, and the tool's location is now probed rather than assumed to sit beside the test
  directory, so a multi-configuration generator resolves as well.
* The writer lock includes `<cstdint>` and `<system_error>` for `intptr_t` and
  `std::generic_category`, both of which only its POSIX branch uses and which no Windows compiler
  therefore ever saw.
* A directory offered where a file is expected is refused explicitly and identically on every
  platform. Windows fails at the open, while Linux opens a directory successfully, reports the
  directory's own size and fails only on the first read, so a size bound was reported for something
  that was never a file. The same check on the atomic publish path stops the fallback from removing
  an empty directory before renaming over it.

### Validation

* Windows with MSVC 19.44, Release and Debug: 205 tests, 0 failures.
* Linux with GCC and with Clang against libstdc++: 205 tests, 0 failures, in CI and locally.
* The install and independent `find_package` consumer proof passes on all three toolchains.

## [1.0.0] - 2026

First release.

### Added

* Strongly typed identities for entities, sensors, sites, envelope references, observation topology,
  sources, envelopes, observations, coupling relations, hotspot episodes, derating evidence, rules
  and retry attempts, with a validated identity grammar and content-addressed derivation.
* Temperature observation model with declared provenance, optional quality metadata, measurement
  site, envelope reference and a six-part fence, and a content-addressed observation identity that
  excludes the receive time so that a genuine retry is idempotent.
* Fence tracking with explicit epoch, generation, revision, incarnation, sequence and attempt
  semantics, and named verdicts for advance, new epoch, duplicate, stale epoch, stale generation,
  replay rejection, conflict and incomparability.
* Bounded evidence store with per-subject retention, a global observation bound, retirement
  accounting, and a subject resolution state machine producing fresh, stale, unknown, conflicting,
  unsupported, indeterminate or refused, including per-source supersession so that a source sending
  a second reading replaces its own earlier one instead of contradicting itself.
* Declared thermal envelopes with ordered, plausibility-checked bands, specificity-based selection,
  exact signed headroom arithmetic, band-consumption fractions that report when they are undefined,
  and threshold transition classification over a transition memory.
* Hotspot episode grouping by union-find over hot entities connected transitively through
  traversable coupling or declared adjacency, so that two hot entities sharing an entity that is not
  itself hot form one episode, with temporal splitting, canonical episode identity, one count per
  distinct link between members, and explicit bounds.
* Coupling evidence model separating declared physical adjacency, supported thermal coupling and
  recorded coincidence, with per-kind citation requirements, and bounded deterministic propagation
  traversal over supported edges only, reporting the weakest link of each path.
* Derating evidence registry and appraisal that checks every claim against the thermal evidence it
  cites and reports supported, unsupported, stale and conflicting claims separately.
* Attribution ledger with nineteen explicit limit codes, bounded and de-duplicated, carried by every
  analysis.
* Deterministic analysis composition producing headroom, transitions, hotspot episodes, propagation
  paths, derating appraisals, attribution limits, ordered reason steps and a content digest, with a
  transition memory that is pruned to the subjects an analysis actually saw so that it cannot grow
  with the history of everything the runtime has ever observed.
* Versioned append-only durable log with a 64-byte checked header, length-and-checksum framed
  records, an exactly defined commit point, conservative torn-tail recovery, outright refusal of
  interior corruption, strict rejection of a wrong magic, version or header checksum, and a rollback
  of the uncommitted tail when a write or a device flush fails part-way so that a torn frame can
  never be followed by a committed one.
* Versioned durable record codec with canonical JSON encoding, strict schema decoding, and content
  re-verification of every observation, envelope, coupling relation and derating claim on load.
* Kernel-enforced single-writer lock using byte-range locking on Windows and `flock` on POSIX, with
  automatic release on process death and read-only inspection of a live store.
* Background commit worker owning the only writable log handle, with a bounded queue that refuses
  work, drain-on-shutdown, and a documented acyclic lock order.
* `ThermalObservatory` facade composing the record set, registries, transition memory, durability
  and recovery into one deterministic entry point, with a recovery report.
* `thermal-observatory` command line tool with `version`, `selfcheck`, `apply`, `analyze`,
  `explain`, `inspect`, `export` and `bench`, speaking the durable record format in both
  directions: `export` writes a log back out as the very documents `apply` reads, so a store can be
  replayed into another store with no conversion step and with byte-identical results. Every
  document-emitting command honours `--out`.
* CMake package export with an independent `find_package` proof, three worked examples, a benchmark
  harness and fifteen CTest suites covering unit, property, seeded randomized, adversarial,
  failure-injection, real multiprocess, real restart and installed-runtime behaviour.
* Documentation covering architecture, the systems boundary, the evidence model, analysis rules,
  persistence and recovery, the concurrency audit, the test strategy, known limitations and the tool.