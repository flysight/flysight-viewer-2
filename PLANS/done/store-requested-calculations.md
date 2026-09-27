# Storing requested calculation results with the session

Date: 2026-09-23
Status: specification for planning. Not an implementation plan.
Baseline: `master` if it contains branch `fusion-improvements`, otherwise
`fusion-improvements` (the fusion engine this document assumes).
Related: `docs/CALCULATIONS.md` (sections 8, 12, 15, 16), `docs/DATA_SCHEMA.md`
(sections 5, 9, 11), `docs/SENSOR_FUSION.md`.

This document says what the change must do and the boundaries it must
respect. It avoids class layouts and function signatures unless a name is
part of the observable contract. The planning agent decides phasing; the
implementation agent decides code structure.

## 1. Motivation

An explicitly requested calculation (`EvaluationPolicy::Explicit`; today
only sensor fusion) can take tens of seconds to minutes per recording. Its
result lives only in the loaded session's memory: it is not written to the
session file, and it is lost when the session is unloaded. Hidden sessions
enter the logbook's least-recently-used pool and are unloaded beyond the
cache capacity, so with a hundred tracks a user who shows them all, fits
them, hides them and shows them again has to fit half of them again; a
restart loses all of them. On-demand calculations are recomputed in
milliseconds and need nothing of this.

## 2. Scope

In scope:

- Storing, on disk, in the logbook's cache, the result of every requested
  calculation that ended as a function of its inputs: a successful result
  with all its outputs, or a rejection or solver failure with its reason and
  diagnostics.
- Restoring such a result when the session is loaded, so that readers,
  plot rows and logbook columns see it exactly as if it had just been
  published, provided it is still valid.
- Validity: a stored result is used only while everything it was computed
  from is unchanged and the code that computed it is the same.
- Removing stored results with their session and dropping stale ones.
- Logbook columns that depend on a requested calculation, which today are
  cached as unavailable.
- Tests and documentation.

Out of scope:

- Storing on-demand or plugin calculation results.
- Starting any calculation implicitly. A stored result is a memory, not a
  request: when it is stale or missing, the calculation reads as not
  requested, exactly as today, until a gesture requests it.
- Any change to the session file (`sessions/<uuid>.csv`): its bytes, its
  format, what it enumerates. Section 5 of the data schema stays true:
  derived sensors never appear in enumeration or in the session file.
- Any change to the kernel, the job queue's gestures, or the plot rows'
  semantics beyond counting a restored result as computed.
- Sharing stored results between logbooks or machines.

## 3. What is stored

For each (session, requested calculation id) at most one record, written
when the result is published (the moment the engine installs it, on the
main thread) and replaced by the next publish for the same pair:

- the calculation id and the outcome: `Ok` with outputs, or `Ok` with the
  reason text of a rejection / solver failure and no measurements (the
  fusion kernel's `Rejected` and `SolverFailed`, which are functions of the
  inputs and are cached in memory today);
- every output the publish installed: measurements (name, samples, unit if
  any) and attributes (name, value), the diagnostics attribute included;
- the validity stamp of section 4.

Never stored: a result the engine did not install (`Cancelled`,
`ResourceExhausted`, a refused publish), and an `UndeclaredRead` or
`InvalidOutput` status. Such a run also deletes nothing: a record on disk
is removed only when it is found stale on load, when its session is
deleted, when the input it depends on changes (the same invalidation that
drops the in-memory result), or when the next publish for the same pair
replaces it.

Values round-trip bit for bit: a restored measurement equals the published
one in every double (the session file's rule for numbers applies:
shortest round-trip decimal, `-0` kept, non-finite written as such), and a
restored attribute string is byte-identical. The golden tests must not be
able to tell a restored fusion result from a freshly computed one.

## 4. Validity

A stored result is valid when all of the following match what is current
when the session is loaded:

1. **Inputs.** A fingerprint over the values the result depended on, as
   the engine's dependency records name them at publish: every source
   measurement (samples and unit text), every attribute and every declared
   preference that the calculation reached, directly or transitively. The
   fingerprint is computed from those values, not from the whole session
   file, so an edit the result does not depend on (moving a marker, a
   description) leaves it valid, and an edit it does depend on (a merge that
   adds IMU data, a changed `SCHEMA_VER`) invalidates it. The record lists
   the dependency names beside the fingerprint so the check can be
   repeated on load.
2. **Code.** `CalculationCompatibilityVersion` and the calculation
   environment fingerprint, the two stamps `index.json` already uses
   (`docs/DATA_SCHEMA.md` section 11), plus a per-calculation result version
   that the registration declares and bumps whenever the calculation's
   arithmetic changes (for fusion, the kernel's `algorithm` string serves).
   The compatibility-version rule of `docs/CALCULATIONS.md` section 9 gains
   the clause: bump it, or the calculation's result version, whenever a
   change can alter what a requested calculation produces.

A record that fails any check is deleted when the session is loaded and
the calculation reads as not requested. Nothing is recomputed on its own.

## 5. When results are restored, and what sees them

- On load of a session (from the logbook on start-up, on show, on
  reveal, on import-merge of an existing session), every valid record for
  that session is installed into the engine as a published result before
  any reader asks: the same outputs, the same status and detail, the same
  dependency edges as a fresh publish, so that later invalidation behaves
  identically (an input change drops it, and drops the record).
- Plot rows count a session with a restored result as computed: no refresh
  count, no job. A stale or absent record leaves the row exactly as today.
- Blocker inspection reports a restored result as it reports a published
  one (a failure's `NotProduced` with its detail included).
- Logbook columns that depend on a requested calculation are computed from
  the restored or published result when the session is loaded, and their
  values are cached in `index.json` like any other column, stamped so that
  dropping the record drops them. Section 11's "cached as unavailable"
  sentence no longer applies to a session with a valid record; without one
  the column stays unavailable, as today.
- Unloading a session (eviction, hide beyond the cache capacity, quit)
  loses nothing: the record is already on disk.

Publishing writes the record; nothing else does. Writing goes through the
logbook manager with the same atomicity as a session save (a temporary
file replaced at the end; a failed write leaves the previous record, if
any, intact and the in-memory result untouched; the write is tried again at
the next publish). A record is deleted when its session is deleted from the
logbook and when it is found stale.

## 6. Where

One record file per (session, calculation) in the logbook's `cache/`
folder, a sibling of `sessions/`, named from the session's file stem and
the calculation id. `sessions/` holds only the recordings; everything in
`cache/` is derived and may be deleted while the application is closed,
after which every requested calculation reads as not requested until
requested again. The encoding is the planner's choice under
these constraints: bit-exact round trip of doubles (section 3), one record
readable without the session, a size of the order of the session file
(seventeen fusion channels at IMU rate), and a version field so that a
future format can refuse or migrate an old record. Human readability is not
required.

Existing logbooks have no records: every requested calculation reads as
not requested until requested, as today. No migration.

## 7. Boundaries

- The engine keeps its threading rules: publish, restore and the writing of
  records happen on the main thread; the record's serialization may run on
  the worker only if it touches nothing the engine owns.
- The store never reads a record for a session that is not being loaded,
  and never starts a calculation.
- Purity holds: with or without a record, the value every reader sees for
  a requested output is the same function of the session's inputs.
- The session file is unchanged: saving a session with a stored result
  gives the same bytes as saving it without.

## 8. Tests

- A fusion fixture fitted, saved, unloaded and reloaded yields the
  seventeen channels and the diagnostics bit-identical to the goldens, with
  no job created; the row shows no refresh count.
- The same after an application restart (the test's logbook directory
  survives across two `SessionModel` lifetimes).
- A rejection and a solver failure are restored with their reason; the row
  shows the warning as today; no job runs.
- Editing an attribute the result does not depend on keeps the record;
  merging IMU data (or changing any dependency) drops it, the calculation
  reads not requested, and the file is gone.
- Bumping `CalculationCompatibilityVersion`, the environment fingerprint,
  or the calculation's result version drops the record on load.
- A record whose write fails (a read-only directory) leaves the in-memory
  result usable and the previous record intact.
- Deleting a session removes its records; a stray record whose session
  does not exist is ignored and removed by the next logbook scan.
- Logbook column over `Fusion/roll`: cached from a valid record, `unavailable`
  without one, invalidated when the record is dropped.
- Saving a session with a stored result produces the same session-file
  bytes as without it.

## 9. Documentation

`docs/DATA_SCHEMA.md` gains the record files (what they hold, when they
are valid, that the session file is untouched) and amends section 11;
`docs/CALCULATIONS.md` amends sections 8 and 9 and describes restore in
section 12 or 15; `docs/SENSOR_FUSION.md` replaces "results are kept in
memory only" by the new behaviour.

## 10. Principles

- A requested result is expensive and deterministic; keep it as long as
  its inputs and its code are the same, and not a moment longer.
- The session file is the recording. Derived data lives beside it, never
  in it.
- Restoring is not requesting: nothing starts on its own.
- A restored result must be indistinguishable from a fresh one, to the bit.

## Commit Policy

Michael has authorized commits for this plan on a new working branch. The
implementation orchestrator makes every commit; implementation, revision, and
review agents never run git commands that change repository state. Nothing is
ever pushed, and nothing is ever committed on `master` or `fusion-improvements`.
Copy this section into the plan overview as its own top-level section with
this exact heading.

- **Branch.** Confirm the working tree has no modified tracked files (if there
  are any, stop and ask). Base: `master` if it contains `fusion-improvements`,
  otherwise `fusion-improvements`. Create with
  `git switch -c store-requested-calculations <base>`; if it exists, switch to
  it; never recreate, reset, or rebase it.
- **Never staged:** `PLANS/`, `TEMP/`, `experiments/`, `build*/`, `dist/`,
  `results/`, and everything under `third-party/` that is not tracked.
- **One commit per accepted phase**, when the review agent returns ACCEPT,
  containing exactly the files the phase's agents reported, staged by explicit
  path. Subject `Phase N: <phase name>`; body a short summary, then the
  session's attribution line. Rejected iterations are never committed.
- **Tag** each phase commit `plan/store-requested-calculations/phase-N-done`;
  tags are never moved or deleted.
- **Fixes to a closed phase** are committed separately as
  `Phase N fixup: <what>` once reviewed and accepted.
- **CI workflow edits** are committed separately as `Phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.
