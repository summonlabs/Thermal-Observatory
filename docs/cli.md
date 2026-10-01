# The thermal-observatory tool

`thermal-observatory` is the command line front end. It speaks the same record format the durable
log
holds, so anything the tool accepts is something the log can store, and any log can be fed back
through the tool.

## Commands

| Command | What it does | Needs a store | Output |
| --- | --- | --- | --- |
| `version` | prints the product, version, compiler, build configuration and both format versions | no | one JSON object |
| `selfcheck` | validates the built-in limits, the CRC-32C check vector, the stable digest, a timestamp round trip, the codec and malformed-JSON rejection | no | one JSON object with a `checks` list |
| `apply` | applies records from `--in` to a store | yes | a summary with counts and rejection reasons |
| `analyze` | runs the full analysis and prints the `ThermalAnalysis` document | yes | one JSON object |
| `explain` | prints the digest, state, reason steps, attribution ledger, hotspots and propagation | yes | one JSON object |
| `inspect` | reads a durable log without writing to it and reports its recovery state | `--log` only | one JSON object |
| `export` | writes every committed record of a log back out in the record document format | `--log` only | the record documents on standard output, or a summary file with `--out` |
| `bench` | measures completed analyses over whatever the store holds | yes | one JSON object of timings |

`help` and `--help` print the usage text and exit 0. A command other than `version`, `help`,
`selfcheck`, `inspect` and `export` requires either `--log <path>` or `--in-memory`; without one the
tool prints "either --log <path> or --in-memory is required for <command>" and exits 2. `export` is
handled before that check and enforces its own requirement: without `--log` it prints
"export needs --log" and exits 2.

## Options

| Option | Argument | Default | Effect |
| --- | --- | --- | --- |
| `--log <path>` | a file path | none | durable log path; the lock file is that path plus `.lock` |
| `--in-memory` | none | off | keep everything in memory; nothing is written and no lock is taken |
| `--in <path>` | a file path or `-` | none | record file, one record per line; `-` reads standard input |
| `--at <rfc3339>` | a timestamp | the wall clock | evaluation instant, for example `2026-02-14T09:31:07Z` |
| `--freshness <seconds>` | a number | the configured window (30 s) | freshness window, applied only when the value is greater than zero |
| `--agreement <celsius>` | a number | 0.5 | agreement tolerance between sources |
| `--band <level>` | `warn`, `high`, `critical`, `maximum` | `warn` | hotspot threshold and query focus; `nominal` is refused |
| `--no-propagation` | none | propagation on | skip the propagation search |
| `--depth <n>` | an integer | 4 | maximum propagation depth |
| `--iterations <n>` | an integer | 2000 | benchmark iterations |
| `--pretty` | none | compact | indent the JSON output; key order is unchanged |
| `--out <path>` | a file path | standard output | publish the emitted JSON document to this file atomically instead of printing it |
| `--not-durable` | none | off | do not enqueue durable commits (see below) |
| `--help`, `-h` | none | - | print usage and exit 0 |

Parsing notes that matter in practice:

* `--at` is parsed by `Timestamp::parse`; an unparsable value is a usage error and exits 2. The form
  is `YYYY-MM-DDTHH:MM:SS[.fraction]Z` in UTC, with one to nine fractional digits.
* `--freshness` and `--agreement` are parsed with `strtod`. A value the parser cannot read becomes
  `0.0`: for `--freshness` that means the configured default is kept, and for `--agreement` it means
  any spread above zero is a conflict.
* `--depth` and `--iterations` are parsed with `strtoul` and `strtoull`.
* `--out` applies uniformly to every command that emits a JSON document - `version`, `selfcheck`,
  `apply`, `analyze`, `explain`, `inspect` and `bench` - where it writes that command's document to
  the file and prints nothing. Publication uses `write_file_atomic`, so the destination is either the
  previous complete content or the new complete content. `export` is the one command with a second
  thing to write, and its behaviour is described below.
* `--band nominal` is refused by the query validation with
  `invalid_argument: a hotspot threshold of nominal would make every entity hot`, and the tool exits
  2.
* `--not-durable` sets `ObservatoryConfig::durable_commits` to false. With that flag the runtime
  never
  enqueues a commit, so accepted records are not appended to the log at all: the log file is created
  with its header and stays empty. The `durable` field of the `apply` summary reports whether
  `--in-memory` was used, not whether commits were enqueued.

## Reading records

`--in <path>` reads a file of up to 64 MiB and treats each line as one record document:

* blank lines and a line consisting of a lone carriage return are skipped;
* every other line must be one complete JSON document, because the reader is line based: a record
  must not be wrapped across lines;
* `--in -` reads from standard input instead. Every line read there is treated as a record, so a
  trailing blank line is not skipped and will be reported as a refusal
  (`malformed_input: unexpected end of document`).

A line that decodes to a `fence` record is refused with `unsupported` and counted in `refused`.
Fence checkpoints are written by the runtime when retention retires an observation; they are not
something an operator applies by hand, and the refusal says so. A record file that holds nothing but
fence records therefore applies nothing and exits 5.

## Record documents

Every record is one canonical JSON document with three top-level fields:

```json
{"codec":1,"kind":"<kind>","payload":{"<kind>":{...}}}
```

* `codec` must be 1 (`kCodecVersion`). Any other value is refused with `version_mismatch`.
* `kind` must be one of `observation`, `fence`, `envelope`, `entity`, `adjacency`, `coupling`,
  `derating`.
* `payload` must be an object with exactly one key, and that key must equal `kind`, so a record
  whose body belongs to a different kind is refused rather than half-applied.
* Decoding is strict: unknown values are refused with `malformed_input`, an out-of-range field with
  `out_of_range`, a list longer than its bound with `limit_exceeded`.

Records that carry an identity must carry the identity the library derives from their content.
`compute_observation_id`, `compute_envelope_id`, `compute_coupling_id` and `compute_derating_id`
produce it; a record whose declared identity does not match its content is refused with `conflict`
(or, inside the log, `integrity_failure`). `entity` and `adjacency` records carry no identity: a
repeat is refused by the registry that owns it with `duplicate_identity`.

The examples below are accepted verbatim by the tool. Each one is a single line; the lines are long
because a record is one line, and splitting one would make it unreadable to the tool.

### entity

```json
{"codec":1,"kind":"entity","payload":{"entity":{"entity":"node-01","entity_class":"compute_node","label":"Rack 1 node 1","zone":"zone-rack1-a","site":"hall-1"}}}
```

`entity` and `entity_class` are required, the class must not be `unknown`, and `label` must be a
non-empty bounded string. `zone` is a reference to a zone another authority owns; this runtime
stores
it so a report can name it and never acts on it. `site` is the site identifier of the facility.

### adjacency

```json
{"codec":1,"kind":"adjacency","payload":{"adjacency":{"from":"node-01","to":"node-02","kind":"shared_enclosure","topology":"topo-hall-1","declared_by":"facility-topology"}}}
```

Both endpoints are required and must differ. `kind` is one of `physical_containment`,
`shared_enclosure`, `coolant_path_proximity`, `airflow_path_proximity`; `declared_unknown` is
refused. `topology` and `declared_by` name the declaration this edge rests on and the source that
made it. Adjacency is a statement about where heat can travel or where a sensor sits - not a zone,
not
a cooling boundary, not an ownership claim.

### envelope

An entity-bound envelope:

```json
{"codec":1,"kind":"envelope","payload":{"envelope":{"id":"env-c0229a0e3ed4ceba","entity":"node-01","entity_class":"compute_node","site":"outlet","has_nominal":true,"nominal_c":20,"has_warn":true,"warn_c":35,"has_high":true,"high_c":40,"has_critical":true,"critical_c":45,"has_maximum":true,"maximum_c":50,"declared_by":"facility-policy","declared_at":"2026-02-14T09:31:07Z","basis":"facility thermal policy revision 12"}}}
```

A class-bound envelope omits `entity` entirely:

```json
{"codec":1,"kind":"envelope","payload":{"envelope":{"id":"env-cfdb5f5842624944","entity_class":"compute_node","site":"outlet","has_nominal":true,"nominal_c":20,"has_warn":true,"warn_c":35,"has_high":true,"high_c":40,"has_critical":true,"critical_c":45,"has_maximum":true,"maximum_c":50,"declared_by":"facility-policy","declared_at":"2026-02-14T09:31:07Z","basis":"class default for compute nodes"}}}
```

All five `has_` flags and all five values are required even for bands that are not declared, because
the derived identity absorbs every flag with its value. `nominal` and `maximum` must be declared,
every declared band must be plausible and never colder than the band below it, and the ceiling must
be
strictly hotter than the nominal band. An envelope bound to neither an entity nor a class is refused
with `unsupported`; an empty `entity` string is refused with `invalid_argument: entity is empty`, so
a class-bound envelope must leave the field out rather than blank it.

### observation

```json
{"codec":1,"kind":"observation","payload":{"observation":{"id":"obs-2b549b9dfba6080f","entity":"node-01","sensor":"sen-outlet-1","site":"outlet","celsius":41.5,"observed_at":"2026-02-14T09:31:07Z","received_at":"2026-02-14T09:31:07Z","source":"facility-telemetry","authority":"measured","source_kind":"facility_sensor","clock":"collector_wall_clock","method":"facility outlet thermocouple","envelope":"env-c0229a0e3ed4ceba","fence":{"source":"facility-telemetry","epoch":1,"generation":1,"revision":1,"incarnation":1,"sequence":1,"attempt":"att-0001"},"quality_supplied":true,"quality_flags":1,"quality_has_confidence":true,"quality_confidence":0.9}}}
```

A second reading of another subject, from the same source at the next revision, with no envelope
recorded and no quality statement supplied:

```json
{"codec":1,"kind":"observation","payload":{"observation":{"id":"obs-73772a734d4048b2","entity":"node-02","sensor":"sen-outlet-2","site":"outlet","celsius":46.25,"observed_at":"2026-02-14T09:31:07Z","received_at":"2026-02-14T09:31:07Z","source":"facility-telemetry","authority":"measured","source_kind":"facility_sensor","clock":"collector_wall_clock","method":"facility outlet thermocouple","fence":{"source":"facility-telemetry","epoch":1,"generation":1,"revision":2,"incarnation":1,"sequence":2,"attempt":"att-0002"},"quality_supplied":false,"quality_flags":0,"quality_has_confidence":false,"quality_confidence":0}}}
```

Required fields: `id`, `entity`, `sensor`, `site` (not `unknown`), a finite plausible `celsius`,
`observed_at`, `received_at`, `source`, `authority`, `source_kind`, `clock`, `method`, the four
`quality_` fields, and a `fence` object with `source`, `epoch`, `generation`, `revision`,
`incarnation`, `sequence` and `attempt` - every counter at least 1. `envelope` may be omitted.
`quality_flags` is the bit set of `QualityFlag`: 1 is `calibrated`, 2 `uncalibrated`, 4 `suspect`,
8 `interpolated`, 16 `extrapolated`, 32 `sensor_fault`, 64 `rate_limited`, 128
`derived_from_model`. The fence source must equal the provenance source, and the identity must match
the content.

### coupling

```json
{"codec":1,"kind":"coupling","payload":{"coupling":{"id":"cpl-6a140d954736952d","from":"node-01","to":"node-02","kind":"supported_thermal","direction":"symmetric","strength":0.8,"asserted_by":"thermal-analytics","asserted_at":"2026-02-14T09:31:07Z","method":"co-movement over two samples","citations":[{"observation":"obs-2b549b9dfba6080f"},{"observation":"obs-73772a734d4048b2"}],"fence":{"source":"thermal-analytics","epoch":1,"generation":1,"revision":1,"incarnation":1,"sequence":1,"attempt":"att-0003"}}}}
```

`kind` is `physical_adjacency`, `supported_thermal` or `coincidental`; `direction` is `symmetric` or
`from_to`; `strength` is in `(0, 1]`. A citation is either `{"observation":"obs-..."}` or
`{"topology":"topo-..."}`. A `physical_adjacency` relation needs at least one topology citation; a
`supported_thermal` or `coincidental` relation needs at least two distinct observation citations.
The
fence source must equal `asserted_by`.

### derating

```json
{"codec":1,"kind":"derating","payload":{"derating":{"id":"dr-615bd2e2082f4247","entity":"node-02","sensor":"sen-outlet-2","site":"outlet","kind":"clock_throttle","magnitude":0.25,"observed_at":"2026-02-14T09:31:07Z","asserted_at":"2026-02-14T09:31:07Z","asserted_by":"platform-agent-1","source_kind":"platform_agent","citations":["obs-73772a734d4048b2"],"trigger_level":"warn","method":"platform thermal throttle counter","fence":{"source":"platform-agent-1","epoch":1,"generation":1,"revision":1,"incarnation":1,"sequence":1,"attempt":"att-0004"}}}}
```

`kind` is one of `clock_throttle`, `frequency_cap`, `power_limit`, `capacity_reduction`,
`feature_disable`; `magnitude` is in `(0, 1]`; `source_kind` must be declared; `trigger_level` must
not be `nominal`; at least one observation citation is required; and the fence source must equal
`asserted_by`. `sensor` may be omitted when the claim is about the entity rather than one subject.
The record says that someone else is already holding the entity below its capability: the runtime
checks the claim against the cited thermal evidence and never enacts it.

### fence

```json
{"codec":1,"kind":"fence","payload":{"fence":{"source":"facility-telemetry","epoch":1,"generation":1,"revision":2,"incarnation":1,"sequence":2,"attempt":"att-0002"}}}
```

A fence record is a high-water mark checkpoint. The runtime writes one when retention retires an
observation. Applying one by hand is refused with `unsupported`, with the reason that a bare fence
record is written by the runtime and not applied by hand, and the line is counted in `refused`. All
six components and the attempt identity are required, and every counter must be at least 1.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | success |
| 1 | the runtime could not be opened: a corrupt log, a wrong format version, a lock held by another writer, a bad configuration |
| 2 | a usage error or a refused operation: missing or unknown option, an unparsable `--at`, an unknown band, no store selection, an unknown command, `inspect` without `--log`, an unreadable record file, a refused query, a failed analysis, a failed benchmark |
| 3 | the result could not be encoded as JSON |
| 4 | a durable commit failed, or closing the runtime reported a flush failure |
| 5 | `apply` applied nothing and refused at least one record, or `selfcheck` found a failing check |

`apply` reports a refusal per line in its `rejections` list and still exits 0 when at least one
record
was applied. A malformed document, a duplicate registration, a refused identity check and a refused
fence position are all reported that way.

## A complete session

The records above are the file `records.jsonl`, one per line. They describe two compute nodes in one
enclosure, an entity-specific envelope for the first and a class default for the second, one reading
each from one facility source, a supported thermal coupling between them and one derating claim
against the second node.

```sh
thermal-observatory apply   --log store.log --in records.jsonl
thermal-observatory analyze --log store.log --at 2026-02-14T09:31:07Z --pretty
thermal-observatory explain --log store.log --at 2026-02-14T09:31:07Z
thermal-observatory inspect --log store.log
```

`apply` answers with a summary:

```json
{"applied":9,"durable":true,"flush":"ok","observations_held":2,"refused":0,"rejections":[]}
```

`analyze` prints the whole `ThermalAnalysis` document; its first lines are:

```json
{
  "attribution": {
    "dropped": 0,
    "limits": [
      {
        "code": "quality_unsupplied",
        "detail": "the source of the representative observation of entity node-02 published no quality statement, so the value is taken as reported",
        "evidence": [],
        "subject": "node-02"
      }
    ]
  },
  "conflicted_subject_count": 0,
  "derating": [
    {
      "entity": "node-02",
      "entries": [
        {
          "asserted_at": "2026-02-14T09:31:07.000000000Z",
          "asserted_by": "platform-agent-1",
          "cold_citations": [],
          "derating": "dr-615bd2e2082f4247",
          "entity": "node-02",
          "kind": "clock_throttle",
          "magnitude": 0.25,
```

The values that answer the question are in the same document:

| Field | Value in this session |
| --- | --- |
| `state` | `fresh` |
| `subject_count`, `usable_subject_count` | 2, 2 |
| `digest` | `an-55543f255e631090` (a function of the whole document, including the evaluation instant) |
| `headroom` | node-01 sits in the `high` band with 8.5 C to the ceiling; node-02 sits in the `critical` band with 3.75 C to the ceiling |
| `transitions` | both subjects report `none`, because there is no earlier evaluation in this process |
| `hotspots` | one episode, `hot-3472177dfe98fa14`, seeded at node-01, peak 46.25 C, two members, one supported relation and one declared adjacency directly between the members |
| `propagation` | origin node-02, one path to node-01 over one hop, path strength 0.8, weakest kind `supported_thermal` |
| `derating` | the single claim against node-02 is `fresh`: the cited evidence reaches the critical band, at or above the claimed warn trigger |
| `attribution` | one limit: `quality_unsupplied` for node-02 |

`explain` prints the digest, state, reason steps and the three analysis sections, which is the short
form of the same answer:

```json
{"digest":"an-55543f255e631090","state":"fresh","reason_steps":["evaluated at 2026-02-14T09:31:07.000000000Z with a freshness window of 30.0s and an agreement tolerance of 0.5 C","propagation was traced from node-02 (peak 46.25 C) to 1 target(s) with depth 4","2 subject(s) resolved: 2 usable, 0 conflicting, 0 stale, 0 unknown","1 hotspot episode(s) among 2 hot subject(s) at the warn band","1 supported path(s) from node-02 within depth 4","1 derating claim(s) against node-02: 1 supported, 0 unsupported, 0 stale, 0 conflicting","1 attribution limit(s) qualify this analysis, 0 dropped at the configured bound"]}
```

Both commands report the same digest, and it is the digest of the full analysis document: `explain`
prints the analysis digest next to a subset of the document rather than a digest of that subset. The
digest is a content fingerprint of the analysis, not a signature, and it changes when any part of
the
analysis changes - including the evaluation instant.

`inspect` reads the log without opening it for writing:

```json
{"committed_bytes":3965,"discarded_tail_bytes":0,"file_bytes":3965,"next_sequence":10,"path":"store.log","records":9,"short_header_reinitialised":false,"writer_epoch":1}
```

Nine records were committed, the file holds exactly the committed prefix, the tail is intact, and
the
next append will take sequence 10. The `path` field echoes the path as it was given on the command
line.

## Re-applying records

Applying the same file twice to the same store is safe and explicit about what it did. The two
observations are idempotent duplicates and are counted as applied; the entity, adjacency, envelope,
coupling and derating records are refused as duplicates:

```json
{"applied":2,"durable":true,"flush":"ok","observations_held":2,"refused":7,"rejections":["entity: duplicate_identity: entity node-01 is already inventoried","entity: duplicate_identity: entity node-02 is already inventoried","adjacency: duplicate_identity: adjacency node-01-node-02 of kind shared_enclosure is already declared","envelope: duplicate_identity: envelope env-c0229a0e3ed4ceba is already registered","envelope: duplicate_identity: envelope env-cfdb5f5842624944 is already registered","coupling: duplicate_identity: coupling relation cpl-6a140d954736952d is already recorded","derating: duplicate_identity: derating evidence dr-615bd2e2082f4247 is already recorded"]}
```

The exit code is 0 because something was applied. In a session that applies nothing and refuses
something, the exit code is 5.
## Exporting a log

`export` reads the committed records of a durable log and writes them back out in exactly the format
`apply` reads, which is what makes the record format shared in both directions:

```sh
thermal-observatory export --log store.log
thermal-observatory export --log store.log --out exported.jsonl
thermal-observatory export --log store.log --out exported.jsonl --pretty
```

* Without `--out`, the record documents go to standard output, one document per line, and nothing
  else is printed.
* With `--out <file>`, the record documents are written to that file atomically with
  `write_file_atomic` - one document per line, terminated by a single line feed - and a small JSON
  summary is printed to standard output instead:

```json
{"committed_bytes":3965,"discarded_tail_bytes":0,"output":"exported.jsonl","path":"store.log","records":9}
```

  The summary deliberately does not go through the shared emit path, because that path honours
  `--out` and would atomically replace the records the command had just written. `--pretty` indents
  the summary; it never reformats the records, which are canonical by construction.

`export` is dispatched before the general store check and enforces its own: without `--log` it prints
`export needs --log` and exits 2. A log that cannot be inspected - damaged, wrong version, missing -
prints the same refusal `inspect` would and exits 2. A failure to publish the file prints the write
status and exits 2.

The round trip is a property, not a coincidence. Exporting a log, applying the export into a second
log and exporting that log produces a byte-identical file, and both stores return the same analysis
digest:

```sh
thermal-observatory export --log store.log   --out first.jsonl
thermal-observatory apply  --log second.log --in  first.jsonl
thermal-observatory export --log second.log  --out second.jsonl
# first.jsonl and second.jsonl are the same bytes (9 records, 3766 bytes in this session), and
# analyzing either store at the same instant returns the same digest.
```

Two details are worth knowing when moving records between machines: the reader accepts a record file
whose lines end with a carriage return as well as one terminated by a bare line feed, and a fence
checkpoint record that the runtime wrote for its own retention is included in an export but is
refused if it is applied by hand, as described above.
