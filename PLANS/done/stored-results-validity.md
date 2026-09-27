# Stored results: validity that mirrors memory

Date: 2026-09-24
Status: specification for planning. Not an implementation plan.
Baseline: branch `store-requested-calculations`, which implements
`PLANS/store-requested-calculations.md`. This document amends that
specification; where the two disagree, this one wins.
Related: `docs/DATA_SCHEMA.md` (sections 11, 12), `docs/CALCULATIONS.md`
(sections 5, 9, 12, 15, 17), `python_plugins/README.md`.

This document says what the change must do and the boundaries it must
respect. It avoids class layouts and function signatures unless a name is
part of the observable contract. The planning agent decides phasing; the
implementation agent decides code structure.

## 1. Motivation

A stored result (a requested calculation's result saved in the logbook's
`cache/` folder) is valid only while its record's code stamps match. One of
those stamps is the calculation environment fingerprint, which covers the
whole registry's candidate order and every declared preference value. So
adding an altitude marker, loading a different set of plug-ins, or changing
the descent-pause preference makes every stored fit in the logbook stale at
its next load, although none of them changes what a fit computes. In memory
the same fits survive those changes: the engine drops a result only when
something it actually reached changes.

A second problem: a record file that cannot be opened or read at load is
deleted as stale. On Windows a transient lock (antivirus, cloud sync) is
enough to destroy a good record that took minutes to compute.

## 2. Goal

A stored result goes stale under exactly the conditions that would drop the
same result in memory, plus a change of the code that computed it. Nothing
unrelated to what the result reached makes it stale.

## 3. When a result in memory is dropped

For reference, the engine drops an installed requested result when:

1. **A value it reached changes on its session**: the samples or unit text
   of a source measurement, or a stored attribute, reached directly or
   through the calculations and conversions its inputs resolved through,
   including a value it looked for and did not find that later appears.
2. **A declared preference it reached changes.**
3. **A registry change alters the answer of a name it resolved**, directly
   or transitively: a registration that would now answer such a name ahead
   of its current provider, or one that answers a name that had no
   provider, is added; the calculation or family that answered such a name
   is removed; or the source-conversion layer changes how such a name is
   answered. A registration that would be tried after the current provider,
   or a name answered by the session's own data, is not affected. (A new
   candidate for a name that had no provider drops the result even if the
   candidate turns out not to provide it: knowing that would require
   running it.) Removing a candidate that was tried and passed over leaves
   the answer alone, but when the result reached some of its lookups only
   through that candidate, the result is dropped too, because its record
   would no longer match those lookups at the next load.

Everything else leaves it installed: edits to values it did not reach
(markers, description, wind), registrations of names it never looked up
(altitude markers, unrelated plug-in outputs), and preferences it did not
reach.

## 4. Validity of a stored result

A record is valid at load when all of the following hold. Together they
replace section 4 of the amended specification.

1. **Inputs** (unchanged): the fingerprint over the values of every leaf the
   result reached, directly or transitively, present or absent, including
   declared preference values, matches the session and preferences as they
   are now. This covers conditions 1 and 2 of section 3.
2. **Resolutions** (new): for every name the result looked up while its
   inputs were gathered, directly or transitively, the record states what
   provided it: a calculation (its instance id and result version), the
   session's own data, or nothing. At load the same lookups are repeated
   against the current registry and must give the same answers. This covers
   condition 3 across runs.
3. **Code**: `CalculationCompatibilityVersion` and the calculation's own
   result version are unchanged.

The calculation environment fingerprint is no longer part of a record.
Section 4.2 of the amended specification loses it, and its bump-rule clause
reads: bump `CalculationCompatibilityVersion`, or the result version of the
calculation concerned, whenever a change can alter what a requested
calculation or anything it reads produces.

A record that fails any check is deleted when its session is loaded and the
calculation reads as not requested, as today. Nothing is recomputed on its
own.

## 5. Registry changes while the application runs

A registry change made while the application runs that drops an installed
requested result (condition 3) also deletes that result's record, exactly
like an input change. Tearing the registry down at shutdown, destroying or
evicting a session, and replacing a session's contents without an input
change delete nothing, as today.

A registry change that does not touch a result leaves both the installed
result and its record alone, and logbook column values over that record
remain cacheable.

## 6. Plug-in code identity

Built-in calculations change their arithmetic only with a new build, which
`CalculationCompatibilityVersion` covers. A Python plug-in can change between
runs under the same calculation id, so every calculation, measurement and
attribute a plug-in registers declares a result version: the plug-in code
identity.

The plug-in code identity is one digest over:

- every `.py` file under the plug-in folder, subfolders included (its name
  relative to the folder and its bytes, in name order), so helper modules
  and packages count; `__pycache__` and hidden folders are left out;
- the plug-in SDK file;
- the Python version and the numpy version the plug-ins run on.

It is computed once when the plug-ins are loaded. Editing any plug-in file,
adding or removing one, or upgrading Python or numpy changes it, so a stored
result whose lookups went through any plug-in calculation goes stale at its
next load. A stored result whose lookups touched no plug-in calculation is
unaffected by plug-in changes.

The logbook column cache in `index.json` is stamped per column: each
column's environment covers the registrations (with their result versions)
and declared preferences that its value can reach, and a change discards
only the cached values of the columns whose environment it alters. So a
plug-in edit or a built-in result-version change discards the cached values
of the columns that reach those registrations, and an unrelated change (an
altitude marker, a preference no column reads) discards none.

## 7. Unreadable records

A record file that exists but cannot be opened or read in full when its
session loads is skipped for that load: it is neither restored nor deleted,
and the calculation reads as not requested for that load. The next load of
the session tries again. A new publish for the same pair replaces it;
deleting the session or the stray pass at start-up removes it.

A record that was read but is not a record, is damaged, or has a format
version this build does not read is deleted as stale, as today.

Logbook column values of a session that depend on a skipped record are not
cached in `index.json` for as long as the record stays skipped, so an
unloaded row never shows a value that disagrees with the record on disk.

## 8. Record format

The record gains the resolutions of section 4.2 and loses the environment
fingerprint, so its format version increases. A record of an earlier format
version is deleted as stale when its session loads. No migration.

## 9. Boundaries

- Everything in `PLANS/store-requested-calculations.md` not amended here
  still holds: restoring is not requesting, publishing writes the record and
  nothing else does (except the deletions listed there and in section 5),
  records are read only for a session being loaded, the session file is
  untouched, and a restored result is bit-identical to a fresh one.
- The engine keeps its threading rules. Repeating the lookups at load uses
  the same resolution the engine uses for a fresh request; it never runs a
  requested calculation.
- Plug-in loading stays a start-up operation. Nothing here reloads plug-ins
  or watches their files.

### 9.1 Logbook columns over stored results

A logbook column's value for a session is the same function of that
session's valid stored results and its other inputs whether or not the
session is loaded. When the logbook's background worker fills a missing
column value for a session that is not loaded, it restores that session's
stored results into its temporary copy of the session, with the same
checks as a load (a stale record is deleted, an unreadable one skipped), and
computes the value from them. This amends the rule of the amended
specification that records are read only for a session being loaded: the
worker's temporary copy counts as a load for reading, never for writing,
and never requests or runs a requested calculation. A value whose requested
result is missing or invalid stays "not requested" until a gesture requests
it.

## 10. Tests

- A stored fit survives each of these and is restored with no job: adding
  and removing an altitude marker; registering and unregistering a
  calculation whose outputs the fit never looks up; changing the
  descent-pause preference; a different plug-in set whose calculations the
  fit never looks up; an application restart after any of these.
- A registry change, while the application runs, that alters the answer of
  a name the fit looked up (for example removing its provider) drops the
  installed fit and deletes its record; registering a candidate that would
  be tried after the current provider keeps both.
- A record whose lookups resolve differently at load (a new candidate for a
  looked-up name, one that computes from the same inputs, registered before
  the load) is deleted and the fit reads not requested.
- With a synthetic requested calculation that reads a plug-in calculation's
  output: editing any plug-in file, adding one, or changing the recorded
  Python or numpy version between runs makes its record stale; editing a
  plug-in file when the requested calculation reads no plug-in output does
  not.
- The plug-in code identity is deterministic: the same files and versions
  give the same digest, and each listed ingredient changes it.
- A plug-in edit discards cached logbook column values over plug-in
  calculations at the next start.
- A record that cannot be read at load (for example held open without
  sharing on Windows, or a directory at its path) is kept, not restored, and
  restored at a later load once readable; its session's dependent column
  values are not cached while it is skipped.
- A record of the previous format version is deleted as stale.
- A column over a requested calculation's output, for a session that is not
  loaded, is refilled from the stored result by the background worker after
  the column's cached value is discarded (for example after the marker time
  it reads changes, or after the column cache is cleared), without loading
  the session into the model and without any job.
- The existing tests of the stored-results feature pass, with the tests
  that asserted environment-change staleness rewritten to the rules above.

## 11. Documentation

`docs/DATA_SCHEMA.md` section 12 (validity, the record's resolutions, the
format version, unreadable records) and section 11 (column values over
skipped records, the environment fingerprint now covering result versions);
`docs/CALCULATIONS.md` sections 9 (the bump rule), 12, 15 and 17;
`python_plugins/README.md` (what the plug-in code identity covers and that
editing a plug-in stales stored results that used it); the user-facing
notes that said unrelated settings changes make stored results stale are
removed.

## 12. Principles

- A stored result is a memory of an in-memory result: it goes stale when
  the in-memory one would be dropped, and when the code changes, and at no
  other time.
- What a result reached decides its validity, never what else is
  registered.
- A transient failure to read is not evidence that a record is wrong.

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
The implementation orchestrator makes every commit; implementation, revision,
and review agents never run git commands that change repository state.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. Copy this section into the plan overview as its own
top-level section with this exact heading.

- **Branch.** Confirm the working tree has no modified tracked files (if there
  are any, stop and ask). Switch to the existing branch
  `store-requested-calculations`; never recreate, reset, or rebase it.
- **Never staged:** `PLANS/`, `TEMP/`, `experiments/`, `build*/`, `dist/`,
  `results/`, and everything under `third-party/` that is not tracked.
- **One commit per accepted phase**, when the review agent returns ACCEPT,
  containing exactly the files the phase's agents reported, staged by explicit
  path. Subject `Validity phase N: <phase name>`; body a short summary, then
  the session's attribution line. Rejected iterations are never committed.
- **Tag** each phase commit `plan/stored-results-validity/phase-N-done`; tags
  are never moved or deleted.
- **Fixes to a closed phase** are committed separately as
  `Validity phase N fixup: <what>` once reviewed and accepted.
- **CI workflow edits** are committed separately as
  `Validity phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.
