# Contributing

Thanks for contributing to Thermal Observatory. This is an observation, explanation and attribution
runtime; the bar for a change is that it makes the record set more trustworthy, more explainable or
easier to consume, without taking authority it does not own.

## Ground rules

- **Preserve the boundary.** This runtime observes. It never actuates cooling, governs thermal
  policy, owns a thermal zone, places workloads or performs recovery. A change that adds any of those
  belongs in an adjacent repository.
- **Preserve the hard distinctions.** Do not collapse observation into ownership, acknowledgement
  into effect, configured state into observed state, or recovered evidence into fresh evidence.
- **Never let coincidence become propagation.** A correlation is not a causal claim, and the graph
  must keep refusing to traverse it.
- **Keep the seven states.** `fresh`, `stale`, `unknown`, `conflicting`, `unsupported`,
  `indeterminate` and `refused` are all different answers. Do not add a code path that treats a
  non-fresh state as success.
- **Do not silently coerce.** A value that cannot be reconciled must fail with a Status that explains
  itself, not with a default, a clamp or a zero.
- **Keep identities, counters and units strongly typed.** Two different identities or two different
  counters must never mix at the type level.
- **Keep every collection bounded.** New externally driven collections need a bound in `Limits` and
  a check on every insertion path.
- **Keep C++20 and compile clean with warnings treated as errors.** MSVC `/W4 /WX` and the
  equivalent GCC and Clang sets must both stay at zero first-party warnings.

## Workflow

1. Fork the repository and create a feature branch.
2. Add a focused test for any behavioural change. A change that alters a rule needs a test that
   fails without it.
3. Build and run the suite:

       cmake --preset release
       cmake --build --preset release
       ctest --preset release --output-on-failure

4. Validate under AddressSanitizer where your toolchain supports it:

       cmake --preset asan
       cmake --build --preset asan
       ctest --preset asan --output-on-failure

5. Commit with a clear message. This project does not add `Co-authored-by` trailers by default; do
   not add one unless you mean it.

## Style

- 2-space indent, 100-column soft limit, configured in `.clang-format`.
- Public headers under `include/tobsv`, sources under `src` mirroring the header tree, tests under
  `tests`.
- No test may declare a timeout, and the framework provides none. A hang is a defect to diagnose.
- Comments explain why a rule exists and what would go wrong without it. Comments do not narrate the
  code.
- Prefer an explicit state over a boolean. Prefer a `Status` with a reason over a `bool`.

## Reporting issues

Include the version (from `thermal-observatory version`), the compiler, the API surface used, and a
minimal reproduction. For a validation concern, show the exact record that was rejected and the
Status that was returned.
