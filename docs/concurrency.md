# Concurrency

This is an audit of the concurrency that actually exists in this codebase, not a summary of good
intentions. It enumerates every mutex, every condition variable, every thread and every lock
ordering, and then addresses each hazard class explicitly: what the code does and why it is safe.

## Inventory

### Mutexes: exactly two

| Mutex | Declaration | Guards |
| --- | --- | --- |
| the state mutex | `ThermalObservatory::mutex_`, `mutable std::mutex` | `config_`, `evidence_`, `inventory_`, `envelopes_`, `topology_`, `coupling_`, `derating_`, `memory_`, `recovery_`, `durable_path_`, `open_`, and the open/close sequence of the log and the lock file |
| the worker mutex | `CommitWorker::mutex_`, `mutable std::mutex` | `queue_`, `outcomes_`, `next_job_id_`, `submitted_`, `failed_`, `dropped_outcomes_`, `busy_`, `stop_requested_`, `running_`, `log_` |

Every public method of `ThermalObservatory` takes the state mutex: `open`, `close`, `is_open`,
`ingest`, `register_entity`, `register_envelope`, `declare_adjacency`, `record_coupling`,
`record_derating`, `flush_durable` and `analyze`. `analyze_now` takes it once for the configuration
read and then `analyze` takes it again, sequentially, never nested.

Every method of `CommitWorker` takes the worker mutex: `start`, `submit`, `drain`, `shutdown`,
`has_room`, `running`, `pending`, `submitted`, `failed`, `dropped_outcomes` and the `run` loop.

### Condition variable: exactly one

`CommitWorker::idle_` is waited on by two predicates:

* `drain` waits for `queue_.empty() && !busy_`, which is the definition of "everything submitted has
  settled";
* `run` waits for `stop_requested_ || !queue_.empty()`.

It is notified after a successful `submit`, after the worker stores an outcome, when `shutdown` sets
the stop request, and at thread exit. A condition variable wake-up runs no user code; see the
callback section below.

### Threads: exactly one created by the library

`CommitWorker::start` creates one thread, `std::thread([this] { run(); })`. There is no thread pool,
no `std::async`, no `std::jthread` and no detached thread anywhere in `include/` or `src/`. Every
other operation runs on the thread that called it. `ThermalLog::inspect` is a static function that
reads a file with no worker and no lock, so a read-only inspection needs no threads at all.

### Kernel resources

* The lock file handle, `SingleWriterLock::handle_`: a Win32 `HANDLE` or an encoded file descriptor.
  It is not a mutex. It is kernel state that excludes other processes and that the kernel releases
  when the holding process dies.
* The log stream, `ThermalLog::file_` (a `std::FILE*`). It is created by the caller's thread in
  `open`, used by the worker thread while the worker runs, and destroyed by the caller's thread in
  `close`.

## Lock order: state mutex before worker mutex

The only nesting in the codebase is the state mutex held while calling into the worker:

* `ingest`, `register_entity`, `register_envelope`, `declare_adjacency`, `record_coupling` and
  `record_derating` hold the state mutex and call `worker_.has_room()` and `worker_.submit()`;
* `flush_durable` holds the state mutex and calls `worker_.drain()`, `worker_.submitted()` and
  `worker_.failed()`;
* `close` holds the state mutex and calls `worker_.drain()`, `worker_.submitted()` and
  `worker_.failed()`.

**The order is therefore state mutex then worker mutex, and it is acyclic because the worker thread
never takes the state mutex.** The proof is structural rather than a matter of inspection
discipline:

* `CommitWorker` holds exactly one pointer into the rest of the library, `ThermalLog* log_`. It has
  no reference, pointer or callback to `ThermalObservatory`, and `worker.hpp` includes only
  `core/limits.hpp`, `core/result.hpp` and `persistence/log.hpp`.
* `CommitWorker::run` takes only its own mutex, and it releases it before calling
  `log_->append(...)`. `ThermalLog` has no mutex and no back-reference either.
* Nothing in `ThermalLog`, `SingleWriterLock` or the record codec calls back into the observatory.

A cycle would require an edge from the worker thread back to the state mutex. No such edge exists in
the code, so the lock graph is a DAG of depth two. The worker also never holds its own mutex across
the device write, which is what keeps `submit` and `drain` from being held up by device latency.

## Hazard audit

### Read-to-write lock upgrade while a read guard is held

**None exists.** There is no reader/writer lock in this codebase: `std::shared_mutex`,
`std::shared_lock` and `std::recursive_mutex` do not appear in any header or source file. The only
mutex types are the two `std::mutex` members listed above. Every critical section is exclusive from
entry to exit, and no method returns a guard, a lock or a reference that would let a caller hold
one.

The const accessors (`evidence()`, `inventory()`, `envelopes()`, `topology()`, `coupling()`,
`derating()`, `limits()`, `config()`, `recovery()`) return references without taking the lock. That
is
a documented caller contract - read them while no other thread is mutating the runtime - and not a
lock upgrade, because there is no lock to upgrade.

### Write locks held across re-entry

**None.** The case the code is explicit about is `analyze_now`, quoted here from
`src/runtime/observatory.cpp` with its comment lines trimmed:

```cpp
Result<ThermalAnalysis> ThermalObservatory::analyze_now() {
  Result<Timestamp> now = Timestamp::now_utc();
  if (!now.ok()) {
    return now.error();
  }
  ThermalQuery query;
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
```

The configuration is read under the lock, the guard's scope ends, and only then is `analyze` called
-
which takes the same lock again. Holding the state mutex across that call would self-deadlock: the
lock is a `std::mutex`, which is not recursive, and the same thread would block forever waiting for
a
lock it already owns. The nested block is what avoids it, and the comment in the source says exactly
this.

The same discipline appears in `close`, which ends its first critical section before joining the
worker and takes the lock a second time afterwards, and nowhere else: no public method calls another
public method while holding the lock. `enqueue`, `require_open_locked` and `require_room_locked` are
private helpers documented as requiring the lock to be held, and they never call a public method.

### Callbacks or events emitted under a lock

**There are none.** No public type in the library accepts a `std::function`, a function pointer or a
virtual callback interface, and no borrowed caller code runs inside a critical section. The only
signalling that happens while a lock is held is `idle_.notify_all()`, which wakes a waiter inside
the
same class and runs nothing that a caller supplied.

The practical consequence is that a caller cannot deadlock this runtime by re-entering it from a
callback, because there is no callback to re-enter from. It also means a caller that wants to react
to a commit outcome polls `flush_durable` or reads `recovery()`, both of which take the lock,
return,
and release it.

### Inconsistent lock ordering

**None.** There is exactly one nesting (state then worker), it only ever occurs on a caller's
thread,
and the worker thread takes exactly one lock at a time. No call path acquires the worker mutex
before
the state mutex. Because `CommitWorker` is the only class with a second lock and it holds no pointer
back to the observatory, no alternative ordering can arise.

### Shutdown and join while holding worker-required state

**Released before joining, in both places that matter.**

`CommitWorker::shutdown` sets the stop request inside a scope, lets the guard die, and only then
joins, quoted from `src/persistence/worker.cpp` with its comment lines trimmed:

```cpp
Status CommitWorker::shutdown() {
  {
    std::unique_lock lock(mutex_);
    if (!thread_.joinable()) {
      running_ = false;
      return Status::success();
    }
    stop_requested_ = true;
    idle_.notify_all();
  }
  thread_.join();
  std::unique_lock lock(mutex_);
  running_ = false;
  log_ = nullptr;
  return Status::success();
}
```

The worker needs that mutex to observe the request, finish the queued work and store its last
outcome; joining while holding it would hand the worker a lock it can never acquire, and the join
would never return. The guard is therefore scoped to the request, and the state is updated again
after the join.

`ThermalObservatory::close` applies the same rule one level up: it drains the queue under the state
mutex, closes that scope, calls `worker_.shutdown()` (which joins) outside the state lock, and then
re-acquires the state lock to close the log and release the writer lock. The comment in the source
gives the reason: releasing it first keeps a concurrent caller from blocking on a device flush. The
drain happens before the shutdown, so an accepted mutation is never dropped by a close.

### Cancellation races

**There is no cancellation, and therefore no cancellation race.** Nothing in the library exposes a
cancellation token, an abort flag or an interrupt request. `ErrorCode::kCancelled` exists in the
error vocabulary and in `to_string`, and no code path in `src/` ever returns it.

The closest thing to a race is a `submit` that overlaps a `shutdown`, and it is handled by the same
mutex:

* if `submit` wins the mutex first, the job is in the queue before `stop_requested_` is set, and
  `shutdown` drains it - `run` only leaves its loop when the queue is empty *and* a stop has been
  requested;
* if `shutdown` wins, `submit` observes `stop_requested_` and returns
  `ErrorCode::kShuttingDown`; `has_room()` also returns false once a stop is requested, so
  `ThermalObservatory::require_room_locked` refuses the mutation with `ErrorCode::kQueueFull` before
  the store is touched.

Because a queued commit is never abandoned, the outcome vector is complete for every accepted job,
which is what `flush_durable` and `close` rely on. Outcome storage is itself bounded: when the
outcome list reaches four times the queue bound, the oldest outcome is dropped and
`dropped_outcomes()` increments, so a drained-but-never-collected worker cannot grow without limit.

### Stale-authority races

**The fence tracker is updated in the same critical section as the insertion.** The high-water mark
for a source lives in `EvidenceStore::trackers_`. Inside one call to `EvidenceStore::ingest` the
code
classifies the candidate fence against the current mark, applies the verdict, inserts the
observation
into `observations_`, `subjects_`, `sensor_index_` and `last_accepted_`, and advances the mark.
Every
call happens with the observatory's state mutex held, from any public entry point. Two threads
cannot
interleave "classify against the mark" with "insert and advance the mark", so no evidence can be
admitted against a mark that another observation has already superseded.

The durable side matches. When retention retires an observation, the fence checkpoint is enqueued in
the same critical section that recorded the new observation (see [persistence.md](persistence.md)),
so the durable mark cannot lag the in-memory mark and a restart cannot lower it.

`EvidenceStore` itself has no mutex. That is deliberate: it is documented as a bounded in-memory
record set with no ownership of threading, and the runtime is what serializes it. A caller that uses
`EvidenceStore` directly owns the synchronization.

### Concurrent publication

**The commit worker is the only writer of the log and the only thread that touches the `FILE` handle
while the worker runs.** Three facts make that true:

1. `ThermalLog::append` is called from exactly one place in the library, `CommitWorker::run`. The
   only
   other callers in the repository are test suites that drive a log directly.
2. The log is opened in `ThermalObservatory::open` **before** `worker_.start()`, so the handle
   exists
   before the thread that uses it; thread creation is a happens-before edge.
3. The log is closed in `ThermalObservatory::close` **after** `worker_.shutdown()` has joined the
   thread, so no append can be in flight. The close happens under the state mutex, and any
   concurrent
   mutation has already seen `open_ == false` and returned `ErrorCode::kClosed`.

There is no second writable handle to race with: another process is excluded by the lock file, and
`ThermalLog::inspect` and `read_file_bytes` open read-only handles with sharing allowed.

### The file sharing decision

`open_stream` opens a file with `_fsopen(path, mode, _SH_DENYNO)` on Windows and with `std::fopen`
on
POSIX. The Windows detail is deliberate: the CRT's own sharing default for `fopen_s` denies other
handles, which would stop the runtime from inspecting its own log while its writer holds it open and
would stop an operator's read-only tooling from looking at a live store. Exclusion is not the
sharing
mode's job here - the kernel single-writer lock is what excludes a second writer - and reads must
remain possible, so sharing is allowed and the lock does the excluding.

## What a caller must not do

The library's concurrency contract is small and explicit:

* call the observatory from as many threads as you like; the state mutex serializes them;
* do not read the references returned by the const accessors while another thread may be mutating
  the
  runtime;
* do not hold `mutable_limits()` and write through it concurrently with any other call;
* do not call `analyze` from a thread that is inside a callback, because there are no callbacks.

Everything else - the queue bound, the outcome bound, the join ordering, the single writer and the
kernel lock - is handled inside the runtime.
