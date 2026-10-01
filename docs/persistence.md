# Persistence

The durable store is one append-only segment. It has a fixed 64-byte header, and every record is a
length-and-checksum framed canonical JSON document. There is exactly one writer per store, enforced
by a kernel lock, and exactly one thread inside a process that touches the writable file handle.

This document covers the byte layout, the commit point, the recovery policy and the code each
condition returns, the single-writer lock, atomic publication, retention and its fence checkpoint,
and what a reopen does.

## On-disk layout

### Header, 64 bytes

The header is written once, when the log is created, and is never rewritten afterwards.

| Offset | Size | Field | Value |
| --- | --- | --- | --- |
| 0 | 8 | magic | the ASCII bytes `TOBSVLOG` |
| 8 | 4 | format version | `kLogFormatVersion`, currently 1, little-endian `u32` |
| 12 | 4 | header size | 64, little-endian `u32` |
| 16 | 8 | writer epoch | the epoch the creating runtime was configured with, for diagnosis only |
| 24 | 8 | created at | the creation instant in Unix nanoseconds, little-endian `i64` |
| 32 | 4 | flags | zero, reserved |
| 36 | 4 | reserved | zero |
| 40 | 8 | reserved | zero |
| 48 | 4 | header CRC-32C | CRC-32C over bytes `[0, 48)`, little-endian `u32` |
| 52 | 4 | reserved | zero |
| 56 | 8 | reserved | zero |

Every integer is written least-significant byte first. The magic is compared with `memcmp` over
exactly its eight bytes, the format version is compared for equality with `kLogFormatVersion`, the
header size must be exactly 64, and the checksum is recomputed over the first 48 bytes. The writer
epoch is recorded for diagnosis; replay rejection does not depend on it, because the durable fences
carried by each record are what order the evidence.

### Record frame

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | payload length, little-endian `u32` |
| 4 | 4 | CRC-32C of the payload bytes, little-endian `u32` |
| 8 | 8 | frame sequence, little-endian `u64`, starting at 1 |
| 16 | payload length | the payload: one canonical JSON record document |

The frame overhead is a constant 16 bytes. Sequence numbers are contiguous from 1 with no gaps; a
frame whose sequence is not the expected next one is refused as corruption rather than scanned past.

The payload is one canonical JSON document produced by the codec: lexicographically ordered keys, no
insignificant whitespace, shortest round-tripping number form. The document names its own shape
twice,
once as `kind` and once as the single key of `payload`, so a record whose body belongs to a
different
kind is refused rather than half-applied.

## The commit point

The commit point of a record is **the successful completion of a single write of the whole frame
followed by a durable device flush**. Nothing before that point is a committed record.

`ThermalLog::append(payload, durable)`:

1. refuses a closed log with `ErrorCode::kClosed`, an empty payload with
   `ErrorCode::kInvalidArgument`, a payload above `Limits::max_record_bytes` with
   `ErrorCode::kLimitExceeded`, and an append that would grow the segment past
   `Limits::max_segment_bytes` with `ErrorCode::kLimitExceeded`;
2. refuses every append while the log is poisoned, with `ErrorCode::kIoFailure` and a reason that
   says to reopen it so the uncommitted tail is trimmed;
3. builds the frame and writes it with one `fwrite`;
4. on a short write, rolls the file back to the last committed offset and marks the log poisoned,
   returning `ErrorCode::kIoFailure`. The sequence and the committed offset are not advanced. The
   rollback matters: appending again from the advanced file position would place a valid frame after
   a torn one, which is interior corruption rather than a recoverable tail;
5. when `durable` is true, flushes the stream and then commits it to the device (`_commit` on
   Windows, `fsync` on POSIX). That flush is the commit point. If it fails, the bytes are in the
   operating system's hands but the device has not confirmed them, so they are not committed: the
   tail is rolled back the same way, the log is poisoned, and the failure is returned;
6. only then advances the in-memory sequence and the committed offset and returns the sequence.

A poisoned log accepts no further appends until it is closed and reopened, and
`ThermalLog::poisoned`
reports that state. The reopen trims whatever uncommitted bytes remain and the next commit starts
again at the last committed offset, so a failed device write cannot turn a recoverable tail into
interior damage.

The commit worker always appends with `durable = true`, so every record that a mutation reported as
accepted is on the device before the worker records its outcome. `ThermalObservatory::flush_durable`
is the barrier that makes that observable: it drains the worker and returns the first commit failure
it saw, or success when nothing failed.

## Recovery policy

The single parse of a log is `ThermalLog::inspect`, which never mutates the file. `open` calls it
and
then acts on the answer. The policy is conservative and explicit.

| Condition | Detection | Result | Error code |
| --- | --- | --- | --- |
| File is empty | size is 0 | no record could have been committed; a header is written | none |
| Partial header | size is in `(0, 64)` | creation never committed; the file is removed and a fresh header is written, and the fact is reported in `LoadResult::reinitialised_short_header` and `RecoveryReport::short_header_reinitialised` | none |
| Wrong magic | first eight bytes differ | the load is refused | `ErrorCode::kCorruptRecord` |
| Wrong format version | `u32` at 8 differs from `kLogFormatVersion` | the load is refused; the runtime does not attempt a best-effort interpretation | `ErrorCode::kVersionMismatch` |
| Wrong header size | `u32` at 12 is not 64 | the load is refused | `ErrorCode::kCorruptRecord` |
| Bad header CRC | CRC-32C over `[0, 48)` differs from `u32` at 48 | the load is refused; the header is not trusted | `ErrorCode::kIntegrityFailure` |
| Fewer than 16 bytes remain after the last record | remaining bytes are below the frame overhead | torn tail: everything from that offset is discarded, `discarded_tail_bytes` is set, and the load stops without error | none |
| A frame runs past the end of the file | `offset + 16 + length > file_size` | torn tail: same handling, because the frame cannot be complete | none |
| A zero-length frame with room after it | `payload_length == 0` and the frame ends before the file ends | the load is refused; a zero-length frame is not a record | `ErrorCode::kCorruptRecord` |
| A zero-length frame as the last 16 bytes | `payload_length == 0` and the frame ends at or past the end | torn tail: discarded | none |
| A frame above the record bound with room after it | `payload_length > Limits::max_record_bytes` and the frame fits | the load is refused | `ErrorCode::kCorruptRecord` |
| A frame above the record bound that does not fit | same test, frame runs past the end | torn tail: discarded | none |
| A frame that fits, fails its checksum, and ends exactly at the end of the file | CRC mismatch and `frame_end == file_size` | torn tail: a partial device write can leave a complete length with a partial body, so this is discarded | none |
| A frame that fits, fails its checksum, with bytes after it | CRC mismatch and `frame_end < file_size` | **interior corruption: the load is refused outright** | `ErrorCode::kIntegrityFailure` |
| A sequence that jumps | frame sequence is not the expected next one | the load is refused | `ErrorCode::kCorruptRecord` |
| More records than the load bound | payload count reaches `Limits::max_loaded_records` | the load is refused | `ErrorCode::kLimitExceeded` |
| The file does not exist and the mode requires it | `kOpenExisting` on a missing path | refused | `ErrorCode::kNotFound` |
| Retrieving a committed record that cannot be decoded | `decode_record` fails during replay | the runtime closes the log, releases the lock and refuses the open, wrapping the decode failure | the decode failure's own code |

Silently discarding the middle of a log would hide exactly the damage an operator needs to see, so
interior corruption is never trimmed. A torn tail is different: it is unambiguously the bytes that
were never committed, and discarding it is the only way to keep the committed prefix usable.

A log opened for writing truncates a torn tail to the last committed offset before the first append.
The truncation happens after the load has already reported the discarded length, and it can only
remove bytes that were never part of a committed record.

### Opening modes

`ThermalLog::open(path, mode, writer_epoch, limits)` has three modes:

* `kCreateNew` fails with `ErrorCode::kDuplicateIdentity` when a file with at least a complete
  header
  is already there. A file shorter than a header that is not empty is removed and re-created.
* `kOpenExisting` fails with `ErrorCode::kNotFound` when the path is absent.
* `kOpenOrCreate`, which is what the runtime uses, opens a valid log or creates one.

Opening an already-open log returns `ErrorCode::kInvalidArgument`. Opening calls
`ensure_directory` on the parent path first, so a missing directory is created rather than reported.

## The single-writer lock

A store has exactly one writer, and the runtime takes a lock file to prove it. The lock file path is
the log path with `.lock` appended.

* **Windows**: `CreateFileW` with `GENERIC_READ | GENERIC_WRITE` and sharing read plus write, then
  `LockFileEx` with `LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY` over one byte at offset 0.
  `ERROR_LOCK_VIOLATION` or `ERROR_IO_PENDING` becomes `ErrorCode::kLocked`; any other failure
  becomes `ErrorCode::kIoFailure`.
* **POSIX**: `open` with `O_CREAT | O_RDWR | O_CLOEXEC`, then `flock(LOCK_EX | LOCK_NB)`.
  `EWOULDBLOCK` or `EAGAIN` becomes `ErrorCode::kLocked`; any other failure becomes
  `ErrorCode::kIoFailure`.

Both are enforced by the kernel, both are released automatically when the holding process exits, and
neither can be broken by a second process that goes through this class. The POSIX lock is advisory:
a
program that deliberately bypasses `flock` is not stopped by it, and that limitation is stated
rather
than hidden.

The call never blocks: a contended lock fails immediately with `ErrorCode::kLocked` instead of
waiting,
so an operator learns that another writer exists rather than hanging behind it. After a successful
acquisition the holder writes `owner_pid=<pid>` into the lock file and flushes it, for post-mortem
diagnosis; the writer epoch in the log header is a separate, independently useful fact. Releasing
unlocks and closes the handle, and a handle that already holds the lock refuses a second acquisition
with `ErrorCode::kLocked` rather than deadlocking on itself.

## Atomic publication

`write_file_atomic(path, bytes)` publishes a whole file atomically:

1. the bytes are written to `path + ".tmp"` in the same directory;
2. the temporary is flushed to the device (`fflush` then `_commit` or `fsync`);
3. the temporary is renamed over the destination.

A reader therefore sees either the previous complete content or the new complete content, never a
mixture. A short write or a failed flush removes the temporary and returns `ErrorCode::kIoFailure`.
Where a rename over an existing file is not permitted by the platform, the destination is removed
and
the rename is retried; the temporary is still complete at that point, so the replace is still not
observable as a partial file. If even that fails, the temporary is removed and the failure is
reported.

The durable log itself is appended, never rewritten, so it does not use this path. Atomic
publication
is part of the public persistence surface for callers that publish a whole artefact - an exported
analysis, a report, a snapshot - and the test suites use it to construct and repair log files.

`read_file_bytes(path, max_bytes)` is the matching read path: it opens with sharing allowed, refuses
a missing file with `ErrorCode::kNotFound`, refuses anything larger than the caller's bound with
`ErrorCode::kLimitExceeded`, and reports a short read with `ErrorCode::kIoFailure`. It never
returns a partial buffer as if it were complete.

## Retention, and the fence checkpoint retention triggers

Retention is per subject, not per store. The store keeps at most
`Limits::max_observations_per_subject` observations of one `(entity, sensor, site)` subject, ordered
by observation instant and identity. Recording one more observation of a full subject retires the
oldest one:

* `retired_count()` increments, so the fact is counted and never silent;
* the analysis reports an `AttributionLimitCode::kRetiredEvidence` limit, because a retired
  observation can no longer be cited by a derating claim;
* the global bound `Limits::max_observations` is a different rule: reaching it **refuses** new
  evidence with `ErrorCode::kLimitExceeded`. Nothing is evicted to make room for it globally.

Retirement creates a durability hazard that the runtime closes explicitly. The observation that was
retired may have been the one carrying the highest fence for its source. If that position is not
remembered durably, a restart would rebuild a lower high-water mark and could accept evidence the
running process had already superseded.

`ThermalObservatory::ingest` therefore watches `retired_count()` across each ingest, and when it
grows it enqueues a fence checkpoint for the source of the observation it just recorded. The loop
that does it, quoted from `src/runtime/observatory.cpp`, is:

```cpp
        for (const Fence& fence : evidence_.high_water_marks()) {
          if (fence.source == stored->provenance.source) {
            const Status checkpointed = enqueue(RecordKind::kFence, encode(fence));
            if (!checkpointed.ok()) {
              outcome.status = checkpointed;
            }
            break;
          }
        }
```

The checkpoint is a `RecordKind::kFence` record written in the same critical section as the
insertion, so the durable mark and the in-memory mark cannot drift apart. `restore_high_water`
applies such a record on replay without classification, because the fence it carries was already
validated by the codec.

## What a reopen does

`ThermalObservatory::open(config)`:

1. validates the configuration: the limits must validate, the writer epoch must be at least 1, and a
   non-empty log path must name a file rather than a directory (`ErrorCode::kInvalidArgument`);
2. takes the state lock. An already-open runtime is refused with `ErrorCode::kInvalidArgument`;
3. rebuilds the store from the configured limits, clears the transition memory and resets the
   recovery report;
4. for a durable configuration: creates the parent directory if needed, acquires the lock file
   (`ErrorCode::kLocked` when another writer holds it), then opens the log with `kOpenOrCreate`,
   which performs the recovery described above and exposes the result through `load_result()`;
5. decodes and replays every committed payload in order. A payload that cannot be decoded is a hard
   stop: the log is closed, the lock is released and the open fails with that record's code, because
   continuing would mean silently dropping evidence the log claims is committed;
6. records what happened in `RecoveryReport`: records loaded, discarded tail bytes, observations
   replayed, records refused on replay, whether the log was created, and whether a short header was
   reinitialised;
7. starts the commit worker over the open log.

Replay is deliberately not a special path. A stored observation goes through the same
`EvidenceStore::ingest` as a live one, so the identity check, the fence classification, the
per-subject ring and the retention counters all apply again, and an observation that the durable
high-water mark has already superseded is refused at replay and counted in
`records_refused_on_replay`. A stored fence restores its source's high-water mark. Every other
record
kind is handed to the registry that owns it.

### Recovered evidence is never promoted to current

This is the rule that matters most about reopen. Recovery rebuilds the record set and the fencing
marks. It does not touch freshness:

* there is no "recovered" flag on an observation and no state in which recovered evidence is
  considered current;
* the freshness decision is made per analysis, against the query's evaluation instant and the
  configured window, exactly as for evidence ingested a moment ago;
* an observation recovered a day after it was taken therefore resolves to `stale`, its headroom is
  not computed, and the analysis reports a stale subject rather than a current one;
* the per-source high-water marks are restored so that superseded evidence cannot re-enter, which is
  the opposite of promotion: recovery can only make the store stricter about what it accepts, never
  more permissive about what it believes;
* `TransitionMemory` is cleared by `open`, so the first analysis after a reopen reports no
  transition for every subject instead of inventing one from a comparison that never happened.

The test suite proves the important half of this against real files: recovered evidence resolves
`stale`, and it is not current.

## Closing

`ThermalObservatory::close` is idempotent. It sets the runtime closed under the state lock, drains
the
commit queue and records how many commits settled and how many failed, releases the state lock,
joins
the worker, and then closes the log and releases the writer lock. The order matters: the queue is
drained before the worker stops so that accepted mutations are committed, and the join happens
outside the state lock so a concurrent caller cannot block on a device flush. If the log close
itself
reports a flush failure, that failure is returned rather than swallowed.
