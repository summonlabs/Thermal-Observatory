# Security

## Reporting

Report suspected vulnerabilities privately to the maintainers through the repository's security
advisory channel. Please include a reproduction, the affected version and the configuration. Include
the output of `thermal-observatory selfcheck --pretty` and `thermal-observatory version`.

## Design notes relevant to security

* **No telemetry transmission.** The runtime never contacts an external service and opens no network
  socket at all. Records are written to local files chosen by the operator.
* **No hardware or privileged access.** The library opens no device, no driver and no privileged API.
  Persistence is ordinary file input and output.
* **Bounded input.** JSON documents, records, citations, collections, traversal depth, propagation
  paths, registry sizes, retention and the durable commit queue all have explicit bounds, and
  externally derived sizes use checked arithmetic. A hostile input produces a refusal with a reason,
  not an allocation failure.
* **Deterministic refusal, never repair.** A malformed record is refused. An observation whose
  identity does not match its content is refused. A frame whose checksum fails with data after it is
  treated as interior corruption and the whole load is refused. Nothing is ever silently repaired
  into a plausible-looking record.
* **Integrity, not authenticity.** Records and the log header are CRC-32C checked, which detects
  corruption and does not defend against tampering: an actor who can rewrite a payload can rewrite
  its checksum. Identity digests are not cryptographic. A durable store from an untrusted source
  must be treated as untrusted input: the loader validates structure, bounds, identities and content
  digests, but it cannot prove authorship.
* **Authority is declared, not verified.** A source's authority level and kind are supplied by the
  caller. A deployment that ingests from an untrusted producer should place a trusted boundary in
  front of it; the runtime reports the declared authority in every result so that a consumer can
  decide for itself.
* **Single writer.** A kernel-enforced lock prevents two processes from writing one store. On POSIX
  the lock is advisory, which is stated here rather than implied away. On Windows it is a byte-range
  lock, which is enforced.
* **Denial of service.** An operator can still exhaust memory by ingesting up to the configured
  retention bound, and can fill the durable commit queue; in the latter case the runtime refuses new
  evidence with `ErrorCode::kQueueFull` rather than growing without limit. Choose `Limits` for the
  host.
