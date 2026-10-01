# Hotspots and coupling

This document describes the analysis side of the runtime: the declared envelope, the band model, the
headroom arithmetic, threshold transitions, hotspot episode grouping, the coupling vocabulary and
the
bounded propagation traversal.

Two rules run through all of it. Headroom is an exact signed distance and is never clamped. A
correlation is never treated as a cause: temporal coincidence is recorded, reported and never
traversed.

## ThermalEnvelope

An envelope is what an authority declared, not what this runtime decided.

| Field | Type | Notes |
| --- | --- | --- |
| `id` | `EnvelopeId` | content-addressed; `compute_envelope_id` |
| `entity` | `EntityId` | unset means the envelope applies to a class |
| `entity_class` | `EntityClass` | `kUnknown` means the envelope is not class bound |
| `site` | `MeasurementSite` | `kUnknown` means any site of the entity |
| `has_nominal`, `nominal_c` | `bool`, `double` | the bottom of the usable band |
| `has_warn`, `warn_c` | `bool`, `double` | optional |
| `has_high`, `high_c` | `bool`, `double` | optional |
| `has_critical`, `critical_c` | `bool`, `double` | optional |
| `has_maximum`, `maximum_c` | `bool`, `double` | the envelope ceiling |
| `declared_by` | `SourceId` | the authority that declared it |
| `declared_at` | `Timestamp` | when it was declared |
| `basis` | `std::string` | the declared justification, bounded and control-character free |

An absent band is absent, not defaulted: each band carries its own `has_` flag, and
`ThermalEnvelope::limit` returns 0 for a band that is not declared, which is why callers must ask
`has_level` first. `band_of` and `compute_headroom` do exactly that.

`validate` enforces:

* an identity;
* a binding to an entity or to an entity class, otherwise `ErrorCode::kUnsupported`;
* a `declared_by` authority and a `declared_at` instant, otherwise `ErrorCode::kInvalidArgument`;
* a `basis` that is non-empty, within the length bound and free of control characters;
* both a nominal band and a ceiling;
* every declared band inside the plausibility band (`ErrorCode::kOutOfRange` otherwise);
* bands that never get colder as the level rises (`ErrorCode::kInvalidArgument`);
* a ceiling strictly hotter than the nominal band.

`compute_envelope_id` absorbs the literal `tobsv.envelope.v1`, the entity, the class, the site, then
for each of the five levels the "declared" flag and the limit value, then the declaring source. Two
envelopes that differ only in an undeclared band's stored number therefore differ, because the flag
and the number are both absorbed - which is why the codec requires all five flags and all five
values
in a stored envelope, declared or not.

`EnvelopeRegistry` validates, checks the derived identity against the declared one
(`ErrorCode::kConflict`), refuses a duplicate identity (`ErrorCode::kDuplicateIdentity`) and
enforces
`Limits::max_envelopes` (`ErrorCode::kLimitExceeded`).

### Selection is by specificity and total

`EnvelopeRegistry::select(entity, entity_class, site)` scores every envelope and returns the single
best one, or `nullptr`:

* an entity-bound envelope that names a different entity is excluded; a match scores 4;
* a site-bound envelope that names a different site is excluded; a match scores 2;
* a class-bound envelope that names a different class is excluded; a match scores 1.

Only a strictly greater score replaces the current best, and the registry iterates in ascending
envelope identity order, so a tie is won by the lexicographically smallest envelope identity. For
any
subject there is at most one winner. A `nullptr` is not an error: it becomes `unsupported` headroom
and an `AttributionLimitCode::kNoEnvelope` limit.

## ThresholdLevel bands

`ThresholdLevel` is ordered from the coldest bound to the envelope ceiling: `kNominal`, `kWarn`,
`kHigh`, `kCritical`, `kMaximum`. `raise` returns the next hotter band and saturates at `kMaximum`;
`lower` returns the next colder band and saturates at `kNominal`.

`band_of(envelope, celsius, out)` returns false when the temperature is not a finite number, because
no band can be claimed for a value that is not a number. Otherwise it walks the five levels in order
and keeps the last one that the envelope declares and whose limit the temperature has reached:

```cpp
  ThresholdLevel band = ThresholdLevel::kNominal;
  const bool known = band_of(envelope, 41.5, band);  // true; band = hottest limit <= 41.5
```

The comparison is `celsius >= limit`, so a temperature exactly at a limit sits in that band. A band
the envelope does not declare is skipped entirely, and a temperature below every declared limit sits
in `kNominal`.

## Headroom arithmetic

For one resolved subject and one envelope, `compute_headroom` produces a `HeadroomReport` with one
`HeadroomValue` per declared band, and lists the undeclared bands in `missing_levels`.

For each declared band:

* `headroom_c` is `checked::sub(limit, observed)`: the exact signed distance from the observed
  temperature to the band limit. It is **not clamped**. A negative headroom is an exceedance and
  stays
  negative; the runtime never reports zero headroom for a subject that is over its limit.
* `exceeded` is `observed > limit`, a strict comparison, so sitting exactly on a limit is not an
  exceedance at that limit but does place the subject in that band (`current_level` uses
  `observed >= limit`).
* `consumed_fraction` is the share of the declared usable band that has been consumed:
  `(observed - nominal_c) / (limit - nominal_c)`, computed with `checked::div`. It is defined only
  when the envelope declares a nominal band strictly below that limit, and the report says so
  through `consumed_fraction_defined`. When the fraction is undefined, the JSON form omits the field
  entirely rather than printing a fabricated ratio. The fraction is **not** clamped to `[0, 1]`: a
  subject above a limit reports a fraction above 1, which is information, not an error.

Notes that the report makes explicit:

* the nominal band's own fraction is always undefined, because its limit equals the nominal value;
* `state` is `fresh` when headroom was computed, mirrors the subject's state when the subject is not
  fresh (with no headroom computed at all and every band listed as carrying none),
  `indeterminate` when the representative value is not a finite number, and `unsupported` when no
  envelope applies;
* `envelope_applied` and `envelope` record which envelope won selection, so a reader can see that a
  class default was used instead of an entity-specific declaration;
* `evidence` and `evidence_synthetic` name the observation the arithmetic used and whether it came
  from a generator;
* `reason` names the observed value, the envelope, the resulting band, the remaining distance to the
  ceiling, and the bands the envelope does not declare.

Usage:

```cpp
  const EntityClass entity_class = inventory.classify(subject.entity);
  const Result<HeadroomReport> report = compute_headroom(subject, entity_class, envelopes, limits);
  if (report.ok() && report.value().current_level_known) {
    const ThresholdLevel band = report.value().current_level;
    const HeadroomValue* ceiling = report.value().level(ThresholdLevel::kMaximum);
    const double headroom_c = ceiling != nullptr ? ceiling->headroom_c : 0.0;
  }
```

## Threshold transitions and TransitionMemory

A transition is a comparison between two evaluations of one subject, never a guess.

`classify_transition(have_previous, previous, report, evaluated_at)` applies these rules in order:

| Condition | Transition |
| --- | --- |
| the report is not `fresh`, or the band is not known | `kUnknown` |
| there is no previous evaluation of this subject | `kNone` |
| the band is unchanged | `kUnchanged` |
| the band rose from nominal | `kEntered` |
| the band rose from any non-nominal band | `kEscalated` |
| the band fell to nominal | `kExited` |
| the band fell but stayed above nominal | `kDeescalated` |

Every record carries the entity, sensor, site, the previous and current bands, the evaluation
instant, the observation identity and a reason that names the movement in words.

`TransitionMemory` is the small map from subject to the band it was last reported in. It is owned by
the caller - or by `ThermalObservatory` - and `analyze` writes to it only for subjects that were
`fresh` with a known band, so an unresolved evaluation cannot poison the next comparison. It is
in-memory only: no transition state is written to the durable log, so the first analysis after a
reopen reports `kNone` for every subject because no earlier evaluation exists in that process. It is
also pruned rather than unbounded: once the map holds more subjects than `Limits::max_observations`,
`TransitionMemory::retain` drops every subject that did not appear in the analysis just completed.

## Hotspot episode grouping

`HotspotPolicy` has three fields:

| Field | Default | Rule |
| --- | --- | --- |
| `threshold` | `kWarn` | the coldest band that counts as hot; `kNominal` is refused with `ErrorCode::kInvalidArgument` because it would make every entity hot |
| `episode_gap` | 120 seconds | must be positive, and no more than one hundred times `Limits::default_episode_gap` |
| `allow_adjacency` | true | whether declared physical adjacency may join two hot entities |

`group_hotspots(headroom, topology, coupling, policy, limits)` groups hot subjects into episodes.
The steps, in order:

1. **Candidacy.** A headroom report counts as evaluable when it is `fresh` and its band is known. It
   is hot when its band's rank is at least the policy threshold's rank. It also has to carry a
   headroom value for the threshold band itself; a subject whose envelope does not declare that band
   is skipped rather than admitted with a substituted band. `excursion_c` is the negated headroom to
   the threshold band, so it is positive above the band.
2. **No hot subject** produces state `unknown` with a reason that names the threshold and the number
   of evaluable subjects. Zero hotspots is an answer, not a failure.
3. **Union-find over entity identity.** The representative of a set is always the smallest identity,
   so grouping does not depend on insertion order. Members are sorted by entity, then sensor, then
   site before any link is examined.
4. **Grouping is transitive and is built from the declarations, not from the hot set.** When
   `allow_adjacency` is true, every declared topology edge is unioned. Then every stored coupling
   relation that is traversable is unioned, except a `physical_adjacency` relation when
   `allow_adjacency` is false. A `coincidental` relation is skipped and never joins an episode.
   Because the union is built over the whole declared structure rather than only over hot entities,
   two hot entities can be grouped through a third entity that is not itself hot - a rack does not
   have to be hot for the heat inside it to be one problem - and membership follows connectivity
   rather than the hotness of any individual link's endpoints.
5. **Link counts describe the direct links between members.** Over the same declaration set, each
   unordered entity pair accumulates the set of distinct supported relations joining it, whether a
   declared adjacency joins it (from the topology or from an adjacency-kind relation), and the list
   of relation identities. For one episode, `supported_links` is the sum over its member pairs of
   the
   number of distinct supported relations directly joining that pair, and `adjacency_links` counts
   the member pairs that have a declared adjacency. `support_relations` is the sorted, de-duplicated
   list of those relation identities. Both counts describe links *directly between members*, and the
   episode reason says so, because members may also be connected through entities that are not hot.
6. **Temporal splitting.** Each component's members are sorted by observation instant, then entity,
   then sensor, and the sorted run is split wherever the gap between consecutive members exceeds the
   policy's episode gap. A negative gap - a member observed earlier than its predecessor - also
   starts a new episode, so an out-of-order timeline cannot chain two episodes together. A component
   whose members were all observed within the gap stays one episode however large it is, up to the
   member bound.
7. **The member bound refuses, it does not truncate.** If any slice would contain more than
   `Limits::max_hotspot_members` members, the whole result becomes state `refused` with no episodes
   and a reason naming the bound, because silently truncating an episode would misrepresent what was
   hot.
8. **Episode identity.** `compute_episode_id` absorbs the literal `tobsv.hotspot.v1`, the threshold
   level, and then the sorted list of `entity|sensor|site|evidence` keys of its members. Two runs
   that group the same subjects from the same observations produce the same episode identity, in any
   insertion order.
9. **Episode content.** `members` (sorted by entity, sensor, site), `peak_c`, `seed` (the smallest
   member entity identity), `observed_at` (the latest member observation), `earliest_at`, the link
   counts, the support relations, and a reason that names the size, the seed, the peak, the band and
   the member identities.
10. **Ordering and the episode bound.** Episodes are ordered by descending peak, then ascending
    seed,
    then ascending identity. At most `Limits::max_hotspot_episodes` are kept; the number dropped at
    that bound is reported in `dropped_episodes` and named in the reason rather than discarded
    silently.
11. **Ungrouped entities.** A hot entity whose component holds no other hot member is still reported
    as an episode of one member, and it is also listed in `ungrouped`, where the analysis raises an
    `AttributionLimitCode::kUngroupedHotspot` limit for it: hot, but joined to no other hot entity
    by
    anything this runtime is allowed to use. The episode says where the heat is; the limit says that
    the connectivity evidence is silent about what is around it.

An episode that is joined only by declared adjacency leaves the source of the heat ambiguous, and
the
analysis records that as `AttributionLimitCode::kJointEnclosureAmbiguity` for the episode's seed.
Declared adjacency says two entities share an enclosure; it does not say which one heated the other.

## CouplingKind

`CouplingKind` is the central distinction of the runtime:

| Kind | Meaning | Traversable |
| --- | --- | --- |
| `kPhysicalAdjacency` | declared by a topology: the entities are adjacent, with no thermal claim attached | yes |
| `kSupportedThermal` | asserted by an authority with citations that support propagation | yes |
| `kCoincidental` | observed to move together, explicitly **not** supported for propagation | no |

`CouplingRelation::is_traversable()` is `kind != kCoincidental`. `allows(from, to)` answers whether
the relation permits that direction under its `CouplingDirection`.

### Citations are mandatory

`CouplingCitation` is either a topology reference or an observation reference, and its `key()` is
`t:` plus the topology identity or `o:` plus the observation identity. Requirements per kind:

| Kind | Requirement | Failure |
| --- | --- | --- |
| `kPhysicalAdjacency` | at least one topology citation naming the declaration it rests on | `ErrorCode::kUnsupported` |
| `kSupportedThermal` | at least two **distinct** observation citations | `ErrorCode::kUnsupported` |
| `kCoincidental` | at least two **distinct** observation citations | `ErrorCode::kUnsupported` |

A coincidental relation is still a claim about co-movement, so it needs the same two observations as
a
supported one; what it does not get is traversal. Repeating one citation is still one observation:
`distinct_observation_citations` sorts and de-duplicates before counting, and a citation with an
empty identity or an empty topology reference is refused with `ErrorCode::kInvalidArgument`. An
empty
citation list is refused before the per-kind rule is applied.

`CouplingRelation::validate` also requires both endpoints (distinct, or
`ErrorCode::kInvalidArgument`), an asserting source, an assertion instant, a fence whose source
equals the asserting source (`ErrorCode::kConflict` otherwise), a method string within the text
bound, a strength in the open unit interval `(0, 1]` (`ErrorCode::kOutOfRange` otherwise), and at
most `Limits::max_citations_per_relation` citations.

`CouplingGraph::add` checks the derived identity against the declared one (`ErrorCode::kConflict`),
refuses a duplicate (`ErrorCode::kDuplicateIdentity`), enforces `Limits::max_couplings`, and
enforces `Limits::max_edges_per_entity` on both endpoints, counting distinct incident relations so a
symmetric relation is not counted twice. A symmetric relation is indexed under both endpoints; a
directed relation is only reachable from its from-end, which is what stops a directed relation from
being traversed backwards.

## Propagation traversal

`PropagationPolicy` bounds the search:

| Field | Default | Rule |
| --- | --- | --- |
| `max_depth` | 4 | at least one, and no more than `Limits::max_coupling_depth` (default 8) |
| `max_paths` | 16 | positive, and no more than `Limits::max_propagation_paths` (default 64) |
| `allow_adjacency` | true | whether declared adjacency may be used as a path segment |

`find_propagation(origin, targets, graph, policy, limits)` enumerates simple, bounded paths:

* the origin must be set (`ErrorCode::kInvalidArgument` otherwise);
* targets are filtered to set identities other than the origin, then sorted and de-duplicated, so
  the
  enumeration does not depend on the order the caller listed them;
* the origin's outgoing relations are inspected first: coincidental relations are counted in
  `coincidental_excluded`, adjacency relations are skipped when `allow_adjacency` is false, and if
  nothing traversable remains the result is state `unsupported` with a reason that says coincidence
  is recorded but never traversed;
* an empty target list after filtering produces state `unsupported` with the reason that no target
  entity other than the origin was requested.

The traversal is a depth-first walk over simple paths, with a visited stack that prevents an entity
from appearing twice in one path, so a cycle cannot produce an endless walk. Three bounds apply and
each is reported:

* **depth**: the walk stops descending at `max_depth`. If traversable relations were left unexplored
  at that point, `depth_limited` is set.
* **paths**: at most `max_paths` paths are emitted. Further arrivals at a target increment
  `truncated_paths`; once that count reaches the path bound itself, `search_bounded` is set and the
  exploration budget is exhausted.
* **exploration**: the visit budget is `max(1024, max_paths * 64)` node visits. Running out sets
  `search_bounded`, which says the search stopped because its budget ran out rather than because the
  graph was exhausted.

Neighbour order is by relation identity, so the enumeration is identical on every run.

### The weakest-link rule

A path's `path_strength` starts at 1.0 and its `weakest_kind` starts at `kSupportedThermal`. For
each step, the path takes the step's strength when it is lower; on an equal strength it takes the
step's kind when that kind ranks lower (`kSupportedThermal` 2, `kPhysicalAdjacency` 1,
`kCoincidental` 0). A chain is therefore reported at its weakest link, and a path that had to use a
declared adjacency says so in `weakest_kind` even though adjacency is a legitimate segment. Each
`PropagationStep` also carries its own relation identity, kind and strength, so a reader can see
exactly which link was weakest.

`PropagationResult` reports `state` (`fresh` when at least one path was found, `unsupported`
otherwise), the paths, `truncated_paths`, `depth_limited`, `search_bounded`,
`coincidental_excluded` and a reason that names each of those conditions when it is set. A bounded
search is reported as bounded.

Within an analysis, propagation starts from the hottest subject - the highest observed temperature,
with the lexicographically smallest entity identity breaking a tie - and targets either the
explicitly
requested entities or, when the query requests none, the other hot entities. The origin and the peak
are named in the analysis reason steps.

## Temporal coincidence is never traversable

This is stated in the code in four places, and it is worth stating plainly here:

1. `CouplingRelation::is_traversable()` returns false for `kCoincidental`;
2. `group_hotspots` skips a coincidental relation, so correlation never joins a hotspot episode;
3. `find_propagation` skips it and counts it in `coincidental_excluded`;
4. the analysis adds an `AttributionLimitCode::kCoincidentalCoupling` limit naming how many
   coincidental relations were recorded and not traversed.

A coincidental relation is retained, reported and available to a reader who wants to know that two
entities move together. It is never a propagation path, at any depth, with any policy. There is no
flag that turns it on.
