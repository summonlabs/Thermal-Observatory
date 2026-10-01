# Testing

Every claim this runtime makes about itself is backed by a test that runs the real code against real
files, real device flushes and real child processes. This document describes what each suite covers,
how the randomized checks are made reproducible, how a hang is treated, and how to run everything
locally.

## How the suites are organised

* One executable, `tobsv_tests`, is built from fifteen suite translation units plus a shared support
  library. Each suite is registered as its own CTest test, named `suite.<name>`, which runs
  `tobsv_tests --suite <name>`, so a failure names the suite that failed.
* The runner takes `--suite <name>` to run one suite and `--list` to print every case name without
  running it. It exits 0 when nothing failed and 1 otherwise.
* A failing assertion prints the file, the line, the expression and the complete `Status`, so a
  failure explains itself without a debugger. `tobstest::describe_failure`, `status_code` and
  `status_ok` are overloaded for both `Status` and `Result<T>`, so a result and a status are checked
  with one spelling and one quality of message.
* `tests/test_support.hpp` adds streaming operators for every vocabulary type in the product
  namespace. A failed comparison therefore prints `conflicting` rather than failing to compile.
* `ScratchDirectory` gives each test a real directory under the system temporary directory and
  removes it afterwards. The persistence, restart, adversarial, hardening and cli suites use it, so
  their proofs are about real files.
* The cli suite runs the real tool as a child process: `tobstest::run_cli` resolves the built
  `tools/thermal-observatory` next to the test executable (through `GetModuleFileNameW` on Windows
  and `/proc/self/exe` elsewhere) and executes it with `std::system`, redirecting both streams to a
  capture file that the test then parses. The path is resolved at run time rather than baked into a
  compile definition, because a Windows path in a macro is a string literal full of backslash
  escapes and a build tree may contain spaces; `THERMAL_CLI_PATH`, when a call site names it,
  expands
  to that run-time lookup. `tests/CMakeLists.txt` makes the test executable depend on the tool
  target, and warns at configure time when `THERMAL_BUILD_TOOLS` is off, because then there is no
  tool
  for the suite to run. Nothing about the tool is mocked.
* `ctest` is invoked without a timeout and the framework has none. A hang is a defect to diagnose,
  not a result to accept.

The suites register 199 cases in total; `tobsv_tests --list` prints the exact set for a build.

## What each suite covers

| Suite | CTest name | What it proves |
| --- | --- | --- |
| core | `suite.core` | Checked arithmetic, identity grammar, strict JSON, time round trips, CRC-32C and digest vectors |
| model | `suite.model` | Vocabulary round trips, counter saturation, fence ordering and verdicts, topology and inventory rules |
| evidence | `suite.evidence` | Freshness, staleness, conflict between sources, unknown, idempotent replay, epoch and revision refusal, forged identities, retention |
| envelope | `suite.envelope` | Band ordering and plausibility, selection specificity, exact headroom, consumed fraction, missing bands, transitions |
| hotspot | `suite.hotspot` | Adjacency and supported-coupling grouping, coincidence exclusion, temporal splitting, canonical episode identity, bounds |
| coupling | `suite.coupling` | Citation requirements, strength domain, coincidence never traversed, weakest link, direction, depth and path bounds, cycles |
| derating | `suite.derating` | Uncited claims refused, band cross-check, missing and stale citations, deterministic ordering |
| analysis | `suite.analysis` | Composition, digest stability, transition memory, attribution limits, propagation origin, canonical JSON |
| persistence | `suite.persistence` | Round trip, torn-tail recovery, interior-corruption refusal, header checks, bounds, lock exclusion across a real second process, atomic publication, strict codec |
| restart | `suite.restart` | Real reopen, fencing high-water survival, recovered evidence is stale rather than current, deterministic retention replay, sequence continuation, torn tail on a live store, relation survival |
| concurrency | `suite.concurrency` | Independent producers without loss, concurrent readers and writers, backpressure, drain on shutdown, worker refusal after shutdown, two stores in parallel |
| property | `suite.property` | Seeded randomized and table-driven properties over arithmetic, ordering, encoding fixed points and order independence |
| adversarial | `suite.adversarial` | Hostile, truncated, corrupted and malformed input across the whole surface |
| hardening | `suite.hardening` | Tamper detection, depth and size bombs, control characters, non-finite values, bounded registries, no mutation by analysis |
| cli | `suite.cli` | The real installed tool through a real child process, including lock exclusion against a live store |

### core

Twenty cases: `Status::describe` determinism, `Result` carrying either a value or a status, checked
integer addition, subtraction and multiplication reporting overflow instead of wrapping, range-exact
narrowing, checked floating point refusing non-finite input and zero denominators, the stable digest
separating text from numbers and `-0.0` from `0.0`, fixed-width lower-case hex, CRC-32C against
known
vectors and incremental updates equal to a one-shot computation, the identity grammar, derived
identities, duration scaling overflow and magnitude saturation, strict timestamp parsing and
fixed-width round trips, the shortest round-tripping real form, `Limits::validate` rejecting a zero,
a value above a ceiling and an inconsistent pair of bounds, capacity and text checks at the
boundary,
strict JSON parsing refusing every repair it could have made, canonical dumping with non-finite
reals
reported, and UTF-8 validation agreeing with what the parser accepts.

### model

Seventeen cases: round trips for the evidence state, measurement site, entity class, threshold
level,
clock, ingest kind and record kind vocabularies; threshold neighbours; counter saturation and total
ordering; fence validation rejecting each missing component; the fence text naming every component;
ordering incomparable across sources and incarnations; the tracker classifying advances and opening
new epochs, then duplicates, conflicts and stale positions, and refusing an incomparable
incarnation;
`force_high_water` restoring without classification; and the topology keeping a canonical entity
set,
rejecting incomplete edges, rejecting duplicates, enforcing the degree bound and returning sorted,
unique, symmetric neighbours.

### evidence

Fifteen cases: a fresh subject resolving to its representative; a missing subject resolving to
`unknown` rather than to something healthy; evidence outside the window resolving to `stale`;
disagreeing sources of one subject resolving to `conflicting`; replaying an observation being
idempotent; an older revision never being promoted to current; an older epoch being refused and a
newer one accepted; two different observations being unable to share one revision slot; a forged
identity being refused; non-finite and implausible temperatures being refused; quality silence and
quality degradation being different states; a future timestamp being `indeterminate` rather than
fresh; the per-subject ring retiring the oldest observation and counting it; the global observation
bound refusing rather than evicting; and resolution order being canonical and stable.

### envelope

Thirteen cases: bands that must be ordered with a ceiling above nominal; an envelope without an
owner
being `unsupported`; a forged identity refused by the registry; a duplicate registration refused;
selection preferring the most specific envelope; headroom being the exact signed distance to each
band; the consumed fraction being defined only where a usable band exists; undeclared bands being
listed rather than assumed; no envelope producing `unsupported` headroom rather than an enormous
one;
a stale subject yielding no headroom at all; `band_of` never decreasing as temperature rises;
transitions being named and never invented; and an unresolved subject yielding an unknown
transition.

### hotspot

Ten cases: cool entities producing no episodes and a reason that says so; adjacent hot entities
forming one episode; temporal coincidence never joining an episode; a supported thermal coupling
joining one; a wide gap splitting one component into two episodes; episode identity not depending on
insertion order; a nominal threshold refused; an episode above the member bound refused rather than
truncated; episodes ordered by peak and then by seed; and the episode bound dropping the least
severe
episode while reporting how many were dropped.

### coupling

Fifteen cases: a relation without citations not being evidence; one observation being coincidence
rather than coupling; repeating one citation still being one observation; adjacency having to cite
the topology that declared it; strength confined to the open unit interval; a relation that names
itself refused; a forged relation identity refused by the graph; propagation never crossing a
coincidental relation; a chain reporting its weakest link; a directed relation not being traversed
backwards; depth being bounded and the bound reported; the path bound reported rather than silently
applied; enumeration identical on every run; a cycle not producing an endless walk; and the coupling
degree bound enforced.

### derating

Eleven cases: an uncited claim not being evidence; a magnitude outside the open unit interval
refused; a nominal trigger band refused; a claim backed by evidence in its band being supported; a
claim whose evidence never reaches the band being `conflicting`; a claim citing an observation the
runtime does not hold being `unsupported`; a claim resting on stale evidence being `stale`; a forged
identity refused by the registry; an entity without claims appraising to `unknown`; several claims
appraised and summarised; and the appraisal being deterministic by claim identity.

### analysis

Sixteen cases: an empty runtime saying `unknown` rather than healthy; a hot subject producing
headroom, a transition and an episode; the digest being stable for identical inputs and changing
with
the evidence; transitions being carried across evaluations by the memory; an unregistered entity and
a missing envelope both being reported as attribution limits; synthetic and silent evidence being
recorded as limits; degraded quality being distinguished from absent quality; clock-domain
disagreement being an explicit limit; coincidence being reported and never traversed; an episode
joined only by adjacency recording its ambiguity; retired evidence being declared as a limit;
propagation starting at the hottest subject and being explained; a conflicting subject making the
whole analysis `conflicting`; the document being canonical JSON with the expected shape; a nominal
focus band refused; and an unset evaluation instant refused.

### persistence

Thirteen cases: a created log round-tripping its records; a torn tail recovered and trimmed;
interior corruption refused rather than trimmed; a header that does not verify refused; a partial
header treated as never created; record and segment bounds enforced before writing; opening a log
that does not exist being explicit; `kCreateNew` refusing to overwrite a live log; the single-writer
lock excluding a second holder; a real second process being unable to open the same store; atomic
publication never leaving a partial file; a log exported, applied into a second log and exported
again - the two exports are the same records in the same order and the replayed store returns the
same headroom; and the codec being strict about unknown versions and shapes.

### restart

Eight cases, all against real files: durable state surviving a real reopen; the fencing high-water
mark surviving a reopen; recovered evidence being stale and not current; retirement being reapplied
deterministically on replay; appends after a reopen continuing the sequence; a torn tail on a live
store recovering without losing committed records; coupling and derating claims surviving a reopen;
and closing twice or reopening after a close both being safe.

### concurrency

Eight cases, all with real threads: independent producers all landing without loss; analyses running
while another thread ingests; the commit queue refusing work instead of growing; shutdown draining
every queued commit; the commit worker refusing work after shutdown; two independent stores not
interfering; `analyze_now` returning instead of deadlocking on its own lock; and `flush_durable`
being a safe barrier when repeated.

### property

Ten properties, each over a fixed seed. The generator is a SplitMix64 implementation defined in the
test file rather than taken from the standard library, so the sequence is identical on every run and
on every toolchain. The comment in the suite states the rule: a property test that samples
differently per build is not a test. Seeds are literals (`0x5EED`, `0xB0B0`, `0xA55`, `0x4E41`,
`0xC0FFEE`, `0x1500`, `0x71E5`, `0xF3A1`, `0xD1CE`, `0x516E`), and the generated corpora include
deliberate extremes: the largest and smallest doubles, subnormals, a signed zero, values that need
seventeen digits to round trip, and the exact ends of the temperature plausibility band.

The properties are:

* headroom equals the declared limit less the observation, exactly, for 300 random temperatures,
  with
  the consumed fraction matching its definition and the nominal band never claiming one;
* `band_of` never decreases as temperature rises, over random ordered pairs, a rising sequence and
  the non-finite inputs that must produce no band at all;
* checked integer arithmetic matches hand-written boundary reasoning at the limits of `int64`, plus
  400 random values where no overflow is possible so the checked form must agree with the plain
  operator, and subtraction must invert addition wherever both are defined;
* narrowing is exact whenever it succeeds, over 500 random 64-bit values into signed and unsigned
  32-bit targets;
* counters never decrease under repeated increments, and the ceiling increment is the identity;
* `dump_compact` and `parse` form a fixed point over 150 generated JSON documents plus Unicode
  escapes;
* timestamps round trip through their fixed-width text form for 400 generated instants;
* the fence tracker never accepts a decreasing position, over 400 random fences, and both acceptance
  and refusal must actually occur or the property would be vacuous;
* episode grouping is independent of insertion order: the full report is compared byte for byte
  across 24 shuffles of six hot subjects;
* the stable digest changes whenever one absorbed byte changes.

### adversarial

Thirteen cases built around damaged bytes and hostile documents:

* a log is built with four framed records and then cut in the middle of each frame in turn; every
  cut
  must load the records before it, report the discarded tail, and then accept a new append that
  continues the sequence with no gap. The same log is damaged inside the first frame with bytes
  after
  it, which must be refused as interior corruption with `integrity_failure`, and damaged in its
  final
  frame, which must be treated as a torn tail;
* an incomplete header (40 bytes) must be reported as re-initialised and then be usable, an empty
  file
  must be reported as created, and a missing file must fail with `not_found` rather than reading as
  empty;
* a wrong magic, a wrong format version and a wrong header size must each be refused with their own
  code;
* a header with a flipped bit inside the checksummed prefix and a header with a flipped checksum
  field
  must both be refused with `integrity_failure`, and opening such a file for writing must refuse it
  rather than repair it;
* thirteen malformed record documents (empty, non-JSON, an array, a missing codec, an unknown codec
  version, an unknown kind, a non-object payload, an empty payload, a fence with an empty source, a
  zero counter, trailing content) must each be refused, while the well-formed record still decodes;
* a stored observation with one field edited must fail its content check with `integrity_failure`
  and
  leave the store untouched;
* hostile temperatures (NaN, positive and negative infinity, -273.16, 1000.1, 1.0e9, -1.0e9) must be
  refused, while exactly -273.15 and exactly 1000.0 are accepted;
* observations with no entity, no sensor, no site, no observation timestamp or no source must be
  refused with the right code, and the store must hold nothing;
* an observation fenced by a source other than its provenance source must be refused with
  `conflict`, while the consistent version is accepted;
* two forged observation identities must be refused with `conflict` and counted, leaving the store
  empty;
* coupling relations must refuse an empty citation list, a single observation cited twice, a
  self-relation, strengths of 0, -0.25, 1.0001, NaN and infinity, and an adjacency claim without a
  topology citation - while accepting a supported relation over two distinct observations and an
  adjacency resting on a declared topology;
* a coincidental relation must be recorded, must report itself as not traversable, and must leave
  the
  propagation result with no paths, a non-zero exclusion count, state `unsupported` and a reason
  naming coincidence;
* envelopes with an out-of-order band, an inverted ceiling, a missing nominal band, a missing
  ceiling,
  an implausible band or no owner must be refused, a forged identity must be refused, and a
  class-wide
  envelope must still lose selection to the entity-bound one;
* duplicate and conflicting fence slots must be refused through the tracker (with the mark
  unchanged)
  and through the store (counted as one duplicate and one conflict with one observation held);
* oversized text and citation lists must hit `limit_exceeded`, at the bound and one past it, and an
  uncited derating claim must be `unsupported`.

### hardening

Twenty-two cases: a tampered stored observation failing its content check; a deeply nested record
refused by the depth budget; control characters unable to enter a durable record; an over-long
identity refused before storage; a derived identity never accepted as a parsed user identity; a log
path that is a directory failing cleanly; non-finite envelope bands refused; a non-finite number
unable to be encoded at all; the attribution ledger bounded and reporting what it dropped; bounded
registries refusing rather than growing; a duplicate coupling refused while a different one is
accepted; limits with a zero or an absurd bound rejected; analysis never mutating the record set; an
observation without an observation timestamp refused; an observation without a site `unsupported`; a
source that declares nothing `unsupported`; a fence whose source disagrees with provenance a
`conflict`; the extreme legal temperature accepted and reported exactly; non-ASCII text surviving
the
durable codec byte for byte; invalid UTF-8 refused by the encoder; reading a directory as a file
failing instead of looping; and a record that fails to decode never being partially applied.

### cli

Eight cases, each executing the real tool:

* `version` prints the product identity, the version, a non-empty compiler, and both format
  versions;
* `selfcheck` exits 0, reports `ok`, lists at least five checks, contains no `MISMATCH` or
  `UNSTABLE` text, and reports the CRC-32C check value the test recomputes itself
  (`e3069283`, the published value for the vector string);
* `apply` reports three applied, zero refused, one observation held and `flush: ok`; a second apply
  of the same records keeps the observation count at one and refuses only duplicate identities;
* `analyze --pretty` is the same document broken across lines, with one subject, one usable subject,
  no stale subject, the exact evaluation instant, and headroom to the ceiling of exactly 8.5 C for
  an
  observation of 41.5 C against a 50 C ceiling;
* `inspect` reports three records, next sequence 4, no discarded tail, writer epoch 1,
  `committed_bytes == file_bytes` and no short-header reinitialisation;
* three broken lines (a document ending inside its payload, an unknown kind, and text that is not
  JSON) are all refused, nothing is applied, and the exit code is non-zero with each reason prefixed
  by `record rejected`;
* a second process cannot acquire the writer lock: the test opens the store in-process, runs `apply`
  against the same log, requires a non-zero exit whose message contains `lock` and `already held`,
  then closes the runtime and requires the same command to succeed with three applied;
* `explain` prints a non-empty digest and state, non-empty reason steps, and carries the attribution
  ledger, the hotspots and the propagation result.

### package.downstream

The packaging proof is not a test case in the executable; it is a CTest test that runs
`cmake -P cmake/DownstreamVerify.cmake`. That script:

1. installs the library into a scratch prefix with `cmake --install`, in the configuration that was
   built;
2. configures `tests/downstream` - a separate CMake project that knows nothing about this build tree
   -
   with `CMAKE_PREFIX_PATH` pointing at that prefix, the same build type, and the same C++ compiler
   the library was built with, so the consumer is never linked against a different C runtime;
3. builds it in that configuration, finds the produced executable, and runs it.

`tests/downstream/CMakeLists.txt` resolves the package with
`find_package(ThermalObservatory 1.0 REQUIRED CONFIG)` and links
`ThermalObservatory::thermal_observatory`. `tests/downstream/consumer.cpp` is that consumer, written
the way an external project would write one: it validates the default limits, opens an in-memory
runtime, registers an entity (with a zone reference it does not own), registers an envelope, ingests
an observation, requires the replay of that observation to be idempotent, runs an analysis, checks
the usable subject count (exactly one), checks that the headroom to the critical band is exactly
3.5 C for a 41.5 C reading against a 45 C critical limit and that the reported band is the high
band,
requires an envelope without a ceiling to be refused, records a cited coincidence relation, requires
coincidence
not to be traversed as propagation, encodes the analysis, and prints a one-line verdict.

The same consumer is also built inside this repository as `thermal_downstream_check`, linked through
the exported interface rather than through the package, so a break in the public surface is caught
during the normal build as well.

## The no-timeout rule

The rule is stated in `tests/CMakeLists.txt`: the framework has no timeout and no test declares one,
because a hang is a defect to diagnose rather than a result to accept. There is no alarm, no
watchdog
and no `--timeout` on any CTest test. A suite that stops making progress keeps running until someone
looks at it, which is the intended signal: the runtime's blocking points are a queue, a condition
variable, a join and a device flush, and a hang in any of them is a correctness bug in the
concurrency design described in [concurrency.md](concurrency.md), not a slow test.

## Running everything locally

Configure, build and test with the presets:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure

cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

The same without presets:

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Useful options at configure time: `THERMAL_BUILD_TESTS`, `THERMAL_BUILD_TOOLS`,
`THERMAL_BUILD_EXAMPLES`, `THERMAL_BUILD_BENCHMARKS` and `THERMAL_ENABLE_ASAN`, all `ON` by default
when the project is top level except the sanitizer. The cli suite needs `THERMAL_BUILD_TOOLS=ON`,
because it executes the tool; without it the configure step warns that the suite will not find one.
Warnings are always errors in this tree - `/W4 /WX` on MSVC and `-Werror` elsewhere - because the
warning interface target sets them unconditionally for every first-party target.

One suite at a time, either through CTest or directly:

```sh
ctest --test-dir build/release -R suite.persistence --output-on-failure
./build/release/tobsv_tests --suite persistence
./build/release/tobsv_tests --list
```

### Windows with MSVC

The `release` and `debug` presets name Ninja and `cl` explicitly, so they need a developer
environment: run them from a Visual Studio developer prompt, or set up the environment first (the
project's CI uses `ilammy/msvc-dev-cmd`). Then:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

MSVC builds with `/W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc /MP`, and the
first-party warning count is expected to stay at zero. Without a preset, any generator works:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Linux with GCC or Clang

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build
ctest --test-dir build --output-on-failure

cmake -S . -B build-clang -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build build-clang
ctest --test-dir build-clang --output-on-failure
```

GCC and Clang build with a comparable warning set, every warning treated as an error:

```sh
-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
-Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
-Werror
```

The project's CI runs the full configure, build, test and package sequence on `ubuntu-latest` for
both compilers, and on `windows-latest` with MSVC, so a change that breaks either toolchain is
caught by the same suite.

## Running the AddressSanitizer preset

**The 1.0.0 validation host could not run this.** Its MSVC installation has no "C++ AddressSanitizer"
component, so `/fsanitize=address` fails to link; the build detects this at configure time and
stops with an explanation instead of failing later with a missing `clang_rt` library. No sanitizer
result is claimed for the release. The Debug configuration was run instead, with the checked debug
CRT, and MSVC `/analyze` was run over every first-party translation unit with zero diagnostics. On
a toolchain that has the component, the preset below works as written.

The `asan` preset inherits `debug` - Ninja, MSVC x64, `Debug` - and sets `THERMAL_ENABLE_ASAN=ON`:

```sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure
```

AddressSanitizer is offered only where the toolchain can actually link it. On MSVC it needs the
optional C++ AddressSanitizer component; without it the link fails on a missing
`clang_rt.asan_dynamic_runtime_thunk` library, which says nothing useful. Configure therefore
compiles and links a trivial probe with `/fsanitize=address` first, and stops with a clear error
naming the missing component and the `-DTHERMAL_ENABLE_ASAN=OFF` alternative rather than letting the
build fail later.

What the option adds, per toolchain:

```sh
MSVC:          /fsanitize=address /Zi   (compile)    /INCREMENTAL:NO   (link)
GCC and Clang: -fsanitize=address -fno-omit-frame-pointer -g   (compile)
               -fsanitize=address                              (link)
```

Either way it defines `THERMAL_ASAN_ENABLED=1` for the compiled code. The sanitizer flags are
attached with `$<BUILD_INTERFACE:...>` and `PRIVATE` linkage, so they never leak into the installed
package and a downstream consumer is not forced into a sanitized build.

The suites that matter most under the sanitizer are the ones that allocate from external input:
`adversarial`, `hardening`, `persistence`, `restart`, `concurrency` and `cli`. Because the
`package.downstream` test builds a separate project, it is unaffected by the sanitizer settings of
this tree; it is a packaging proof, not a memory-safety one.