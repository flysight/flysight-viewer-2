# Phase 3: Traceability

## Purpose

Spec §10's last bullet and the `tests/README.md` / `tests/acceptance_map.txt`
half of §11 (overview decisions 15, 16). Phases 1 and 2 changed the kernel,
added and rewrote the tests, added the audit rules and wrote every `docs/`
change. This phase makes the record:

- the specification's testable statements as items 901-940: appendix J of
  `tests/README.md`, section 9.10, and the map's lines, each traced to a test
  function, audit group or manual step that phases 1 and 2 created or kept;
- the two earlier items this specification amends, 847 and 861, restated
  "(as amended)";
- the audit: the traceability range, the item numbers in the group headers
  phases 1 and 2 touched, and the file-head summary;
- `tests/README.md` section 10 for the rules phases 1 and 2 added, section 12
  for two manual steps, and the stale tree and install paths of 12.2 and
  section 4.

**No code changes.** The files touched are `tests/README.md`,
`tests/acceptance_map.txt` and `tests/audit/cleanup_audit.cmake` (comments
and the traceability block only). No file under `src/` or `docs/`, no
`tests/*.cpp`, no support file, no golden and no `CMakeLists.txt` changes. A
stale comment or a missing document statement is reported as a fixup of the
phase that left it, not fixed here.

## Dependencies

- **Depends on phase 2** (tag `plan/fusion-reconstruction/phase-2-done` and
  any accepted fixups), and through it on phase 1. **Blocks nothing.**
- **May assume**, as phases 1 and 2 leave the tree:
  - `reconstructAtImuRate()` is what `run()` publishes; `DenseTrajectory`,
    `reconstructTrajectory()` and `endpointCorrection` exist nowhere;
  - the success diagnostics carry `dense_output`,
    `max_step_correction_m_s2`, `max_step_correction_time_s`,
    `max_velocity_mismatch_m_s` and `max_endpoint_correction_deg`, and no
    longer the key they replaced; `Fusion::Algorithm` is
    `batch-temperature-bias-v4`;
  - audit group `fusion-model` with phase 1's rule "the step model has one
    author" (or the name phase 1 gave it); group `fusion-reconstruction`
    (phase 2's two rules, no item numbers, and a file-head bullet without
    items); `stored-results` naming v4;
  - `tests/README.md` section 11 as phase 2 rewrote it (the procedure names
    the tree `<tree>` and the installs `third-party/GTSAM-install` and
    `third-party/oneTBB-install`; the capture's record with the `_time`
    comparison), section 1's rows as phases 1 and 2 left them, section 10's
    `fusion-model` bullet saying the goldens say v4;
  - existing test names kept by phase 2 (decision 1 there):
    `exactConstantVelocityFit`, `reconstructionUsesIntervalBias`.
- **Test function names are obtained, never invented.** The new and rewritten
  functions are named in the phase 1 and phase 2 reports (each with the §10
  bullet or criterion it covers). If a report was not passed on, list what
  the tree holds, `git diff plan/fusion-reconstruction/phase-1-done~1 HEAD --
  tests/*.cpp`, and read each added function's comment and assertions. Cite
  the function that asserts the fact. The labels in the table below (`P1-…`,
  `P2-…`) stand for those functions; one label may be several functions and
  one function several labels. If no function asserts a fact the table
  names, report it; never drop a line to make the audit pass.
- **Text the audit searches in `tests/`**, and therefore in the map
  (`tests/README.md` is allowed or excluded in each):
  - phase 1's `fusion-model` rule: the member-call form of
    `integrateMeasurement(`, `UpdatePreintegrated`, and the three
    `…Covariance` setters;
  - phase 2's `fusion-reconstruction` rules: the removed names, the old
    diagnostics key, and the phrases of its second pattern ("not an IMU-rate
    smoothing posterior", "interpolated for display", "states interpolated",
    "distributed correction" and the others);
  - `fusion-model`'s retired strings (v1, v2) and the branch name (whose
    count in `tests/README.md` is pinned at 4: this phase adds no mention).

  The map says what changed without those words (for example "`limitations`
  no longer denies that the output is the IMU-rate posterior"), and spells no
  algorithm string: phase 2's criterion that `batch-temperature-bias-v3`
  appears in `tests` only in the stale-record row must stay true.
  Appendix J and 9.10 may name `batch-temperature-bias-v4`.
- **Cite the specification by its title**, "The fused state at every IMU
  sample", not by path; it moves to `PLANS/done/` when the work is finished.

## What changes

### `tests/acceptance_map.txt`

- **Head.** "Nine specifications, nine item ranges" becomes ten. The 801-863
  entry gains "It is amended by the specification of 901-940: items 847 and
  861 are stated as amended." A new entry in 801-863's form: `901-940` the
  fused state at every IMU sample (the reconstruction between fixes, the
  published channels at the IMU rate with the model's acceleration, the
  diagnostics' account of the reconstruction, the algorithm string and the
  goldens captured again): item = 900 + clause number; stated in full in
  tests/README.md, appendix J; it amends the specification of 801-863. The
  ranges sentence gains 901-940 (in both lists), and "sections 9.1 to 9.9"
  becomes 9.10.
- **New block** at the end, after
  `# ---- The fused state at every IMU sample: item = 900 + clause number ----`:
  per item of the table under Tests, the comment line
  `# <item> - (<section>) <clause>` (or `(<section>, as settled)`), then its
  lines in the four forms.
- **Amended items.** The comment lines of 847 and 861 are rewritten in place
  (Decisions, D3); every existing line of both stays.

### `tests/README.md`

- **Section 9 intro**: nine becomes ten (specifications, ranges, tables).
- **9.9**: its intro gains "Items 847 and 861 are stated as amended by the
  specification 'The fused state at every IMU sample' (9.10)"; rows 847 and
  861 are restated with Section "12, as amended" and "13, as amended". While
  editing the intro, correct its phrase for clause 14, which still describes
  the superseded reading ("heading takes the course's reference and its
  availability"), to the clause as it stands (a compass heading measured from
  north, not referenced to the course reference), and report it.
- **New 9.10 "The fused state at every IMU sample (items 901-940)"** after
  9.9, in 9.9's form. Its intro: the forty clauses, stated in full in
  appendix J; item = 900 + clause number, the same four line forms, every
  item a test or audit line; sections 1-4 have no item, their principles
  carried by the clauses that apply them, the observable statements of §2
  and §3 being clauses 12 (the channels, no uncertainty published), 16 (one
  time axis), 20 (the record format), 25 (the fit and its estimate at the
  fixes unchanged), 27 (one authority for the integration) and 30 (the
  registration and everything above it); clauses 31-39 are its §10 tests,
  one per bullet, and 40 its §11, which also carries the documentation
  sentences of §2 and §6; the settled clauses, each with one phrase on how
  (D2), naming plainly where the plan departs from the specification's
  letter; it amends those of 9.9 (items 847, 861). Then
  `| # | Section | Clause | Evidence |` with 40 rows, evidence in the full
  `tst_x::function` form plus the audit and manual lines, as 9.9 does.
- **New appendix J** at the end,
  `## Appendix J. The acceptance items of the fused state at every IMU sample (901-940)`.
  Its intro follows appendix I's (the specification amends appendix I; one
  sentence each with the section number in front; the numbers are this
  list's; which sections have no item; 31-39 and 40; the settled clauses,
  9.10 says how). Then "1. (5) …" to "40. (11) …", each the map's clause,
  capitalized and ending with a full stop. A settled clause states the
  plan's reading in its own words, including the honest part (D2).
- **Appendix I**: clauses 47 and 61 restated "(12, as amended)" and "(13, as
  amended)", and the intro names them as amended by appendix J's
  specification.
- **Section 10**, one clause per rule, the group's items in parentheses:
  - `fusion-model`: the items become 212, 218, 234, 247, 902, 903, 927, 929,
    939, 940; add phase 1's rule as it is in the script (an integration call,
    the library's static tangent update or a per-step covariance setter
    anywhere in `src` or `tests` but `imuintegration.cpp`: the step model has
    one author, and the reconstruction and its tests read it). This file may
    spell the pattern (the rule allows it).
  - New bullet `fusion-reconstruction` (items 917, 926, 939, 940), placed
    after `fusion-tooling`'s: the linear reconstruction's names or the
    replaced diagnostics key appear in `src`, `tests` (the goldens included:
    a hit there is a stale capture), `docs` or the root `README.md`; or a
    text there says the fused output is interpolated (list the phrases).
    "This file is excluded because this section spells the patterns."
  - `fusion-tooling`: items gain 928, 939. `solver-confinement`: items gain
    928, 939. `stored-results`: items gain 921, 930 (its text names no
    literal and needs no change).
  - The traceability bullet gains 901-940 in both lists.
- **Section 12**: "Seven scripts" becomes "Eight scripts". New **12.8 The
  fused state at every IMU sample** after 12.7, in 12.7's form, with M45 and
  M46 (Tests). In **12.2's preamble** the tree and install paths follow
  section 11 as phase 2 left it: `cmake --build <tree> --config Release`,
  `R=<tree>/FlySightViewer-build/Release/fusion_runner.exe`, and a `PATH`
  with `third-party/GeographicLib-install/bin`,
  `third-party/GTSAM-install/bin` and `third-party/oneTBB-install/bin`; no
  step's content changes. **Section 4**'s parenthesis naming the solver
  runtime directories gets the same two install paths.
- **Section 1**: the rows of the executables the new block cites and that
  already name items gain theirs in that form, "(fusion-reconstruction items
  …)": `tst_fusion_kernel`, `tst_fusion_golden`, `tst_fusion_store`,
  `tst_fusion_session`, `tst_fusion_runner`, `tst_result_records`,
  `tst_result_columns`, `tst_fusion_rows`, where each is cited. Nothing else
  in section 1 changes.

### `tests/audit/cleanup_audit.cmake`

- **File head**: phase 1's rule and phase 2's bullet are gathered into one
  bullet for this specification after the 801-863 one, ending "(items
  901-940)": the step model has one author, and the linear reconstruction,
  its diagnostics key and any text calling the fused output interpolated
  stay gone. No fact is stated twice.
- **Group comments**: `fusion-model (items 212, 218, 234, 247, 902, 903, 927,
  929, 939, 940)`, `fusion-reconstruction (items 917, 926, 939, 940)`,
  `fusion-tooling (items 231, 233, 848, 928, 939)`, and `stored-results`
  gaining 921, 930. `solver-confinement`'s comment names no items today and
  stays so.
- **Traceability**: the comment's list gains "and 901-940 (the fused state
  at every IMU sample, item = 900 + clause number)"; the range condition
  gains `OR (item GREATER_EQUAL 901 AND item LESS_EQUAL 940)` and the message
  the range; a `foreach(item RANGE 901 940)` requires a test or audit line,
  as for 801-863.
- **No rule is added or changed**; the rule count equals phase 2's.

### Documents: checked, not written

Clause 940's evidence is the audit, and the statements are phase 2's. Read
the documents once and confirm each is present; report a missing one as a
phase 2 fixup:

1. `SENSOR_FUSION.md` §4: the reconstruction paragraph (the output is, to
   within one linearization, what a fit with a state at every IMU sample
   gives); the outputs table's position, velocity and acceleration rows; the
   one sentence on the time axis whatever the GNSS rate; the key list with
   v4, the three new keys with units, `dense_output` and `limitations` quoted
   as the kernel writes them.
2. §7: v4, the first start after the update, the reconstruction not a
   boundary. §8: the equivalence test, its reference and why it holds the
   ends, the agreement measured, what the one pass leaves out, the
   consistency bound with the measured ratios, the fast-rotation limitation.
3. `CALCULATIONS.md` §17, `COMPUTED_PLOTS.md` §1 (the plots between fixes),
   `DATA_SCHEMA.md` §11 and §12, `tests/README.md` §11 (the capture, why,
   and the `_time` comparison per success fixture).

## Interfaces

- **Consumes:** the test functions as the tree holds them after phase 2 and
  the two reports; the audit groups as phases 1 and 2 left them; manual ids
  up to M44; section 11 and the documents as phase 2 left them.
- **Provides:** nothing to a later phase. Items 901-940 and their clause
  texts become the specification's traceability record.

## Acceptance criteria

1. `cmake -DREPO=. -P tests/audit/cleanup_audit.cmake` passes with the rule
   count phase 2 left; the report gives both numbers. (§10 last bullet.)
2. Items 901-940 each have at least one test or audit line; every cited
   function exists as `::<function>()`; `**M45 ` and `**M46 ` each exist once
   in `tests/README.md`. Two plants each trip the audit and are undone:
   `941 audit fusion-model` (outside the ranges), and removing every line of
   one 9xx item (that item reported). (§11.)
3. Every test function phases 1 and 2 added or rewrote is cited by at least
   one 9xx line, or the report says why it is not.
4. Items 847 and 861 read "(as amended)" in the map, in 9.9 and in appendix
   I; the map head and the 9.9 and appendix I intros name the amending
   specification. (§11 "amended items restated".)
5. 9.10 has 40 rows, appendix J 40 clauses, the map 40 comment lines
   901-940, the same in substance; every settled clause says how in 9.10's
   intro and in its own words. Section 9's intro and the map head say ten.
6. Section 10 describes phase 1's rule and the `fusion-reconstruction`
   group, carries the item lists above, and its traceability bullet names
   901-940.
7. Section 12 says "Eight scripts" and has 12.8 with M45 and M46;
   `git grep -n -E "build-phase1|build-solver-deps" -- tests/README.md` is
   empty.
8. `git grep -n -E "batch-temperature-bias-v[0-9]" -- tests/acceptance_map.txt`
   is empty.
9. The documents check is done: each statement present or reported.
10. `cmake --build build-agent --config Release` has nothing to rebuild; the
    whole suite passes sequentially,
    `ctest --test-dir build-agent/FlySightViewer-build -C Release --output-on-failure`,
    after checking for stray `ctest` / `tst_*` processes.
11. `git status` shows changes only in the three files under Purpose.

## Tests

No test is added or changed; the audit's traceability check proves every
line resolves.

**Labels** for the functions phases 1 and 2 name (obtain them as under
Dependencies): `P1-eq-coarse` equivalence on `coarse_maneuver` (phase 1
criterion 7); `P1-eq-rot` equivalence on the rotating recording (8);
`P1-ends` the ends and P_n (5); `P1-axis` the pass's time axis and a sample
on a fix (6); `P1-sum` the four summaries (12); `P1-share` sharing by noise
(9); `P1-zero` zero mismatch (10); `P1-cons` consistency (11); `P2-pub`
publication per success fixture (phase 2 criterion 2); `P2-axis` the time
axis per success fixture (7); `P2-fast` GNSS faster than IMU (7); `timing`
the rewritten `reconstructionTimingAndEndpointCorrection` under whatever
name phase 2 left it. Existing: `fk` `tst_fusion_kernel`, `fg`
`tst_fusion_golden`, `fst` `tst_fusion_store`, `fs` `tst_fusion_session`,
`fr` `tst_fusion_rows`, `ru` `tst_fusion_runner`, `rr` `tst_result_records`,
`rc` `tst_result_columns`, `a:` audit, `M` manual.

The Clause column is the substance; wording may be polished as long as
appendix J, 9.10 and the map say the same.

| Item | § | Clause | Lines |
|---|---|---|---|
| 901 | 5 | for every fix interval of a fit that succeeds, the pass integrates gyro and accelerometer together from the fitted state at the interval's first fix with the biases the fit assigns to that interval: the attitude advances with the gyro, the reading, bias removed, is rotated by it and gravity added, and velocity and position advance with the result | P1-eq-coarse; P1-ends; fk reconstructionUsesIntervalBias, exactConstantVelocityFit |
| 902 | 5 | the integration is the fit's own: its edges are every IMU sample in the interval and the two fix times, with the same midpoint interpolation of the readings and the same per-step noise as the fit's IMU factors | P1-ends; a: fusion-model |
| 903 | 5 | the pass accumulates at every edge the covariance of the noise the steps so far have added, from the transition and noise of the solver library's own preintegration update; at the second fix it is the covariance of the fit's IMU factor for the interval | P1-ends; a: fusion-model |
| 904 | 5, as settled | the mismatch is the fitted state at the interval's second fix in the local coordinates of the forward state there, nine components; not the factor's residual, which the library computes the other way round and whose negative misses the fitted end (decision 2) | P1-ends |
| 905 | 5, as settled | the correction at each edge is the conditional mean of the step chain given both ends, linearized about the forward states, computed in the preintegration's tangent coordinates where the covariance lives and mapped into each forward state's local coordinates by the library's retraction; the formula applied literally in the local coordinates is wrong at first order under rotation (decision 3) | P1-eq-coarse; P1-eq-rot |
| 906 | 5 | the share is nothing at the first fix and the whole mismatch at the second, one nine-component correction applied jointly, so that an attitude share turns every later reading | P1-ends; P1-eq-rot |
| 907 | 5 | where the per-step noise is uniform the velocity share grows with elapsed time; a step that carries more noise takes more of the mismatch | P1-share |
| 908 | 5 | the corrected state is the forward state with its share applied by the retraction that inverts the local coordinates; at the second fix it is the fitted state, exactly | P1-ends; P1-axis |
| 909 | 5 | the corrections are computed once, about the forward states, and applied once; the ends are exact by construction and the linearization costs only the split inside the interval, within the equivalence test's bound | P1-eq-coarse; P1-eq-rot; P1-ends |
| 910 | 5, as settled | a step correction for every step: the corrected velocity change over the step length, less the mean of its two edges' readings, bias removed, each rotated by the corrected attitude at its edge, less gravity (decision 6) | P1-cons; P1-zero |
| 911 | 5, as settled | where the mismatch is zero the pass is the forward integration; the step corrections are zero where the recording does not rotate, and under rotation equal minus the integration's rotation lag (decision 6) | P1-zero |
| 912 | 6 | the fit publishes the seventeen channels of the fusion document's section 4, no more (no uncertainty is published), on the same time axis | fs registrationShape; ru outputTableMatchesGolden; P2-pub; P2-fast; M46 |
| 913 | 6 | at every IMU sample the attitude (quaternion, and roll, pitch and yaw from it) is the corrected attitude at the sample's edge, and velocity and position the corrected state there | P2-pub; P1-eq-coarse; fk exactConstantVelocityFit; M45 |
| 914 | 6, as settled | the acceleration is the sample's own reading, bias removed, rotated by the corrected attitude, plus gravity, plus the mean of the corrections of the steps beside its edge (a step beside a fix being the part-step in that interval); only a sample exactly on the first fix has one such step, since the last fix is never published; where the corrections are negligible it is the rotated reading | P1-zero; P1-cons; P2-pub; M45 |
| 915 | 6, as settled | integrated by the kernel's rule, the published acceleration reproduces the published velocity change over the fitted interval, and over any run within the spread of the corrections at its ends (decision 8's bound); not step by step | P1-cons |
| 916 | 6 | the time axis is the IMU samples in the half-open interval from the first fix to the last, whatever the GNSS rate, a GNSS rate above the IMU's included: no fix time added, nothing resampled, a sample on a fix published once | P1-axis; P2-axis; P2-fast; M45; M46 |
| 917 | 7, as settled | the success diagnostics' account of the dense output names the reconstruction (`dense_output` in place of the previous key), and `limitations` no longer denies that the output is the IMU-rate posterior; the documentation quotes both texts (decision 10) | fk initializerDiagnosticsShape; fg successFixturesMatchGolden; a: fusion-reconstruction; M46 |
| 918 | 7, as settled | the success diagnostics report the largest step correction (`max_step_correction_m_s2`), its step's midpoint in seconds since the epoch (`max_step_correction_time_s`) and the largest velocity mismatch of any interval (`max_velocity_mismatch_m_s`), beside `max_endpoint_correction_deg`, which stays (decision 10) | P1-sum; P2-pub; fg successFixturesMatchGolden, comparatorHoldsItsBounds; fk reconstructionUsesIntervalBias; timing; M46 |
| 919 | 7, as settled | rejected and failed fits' diagnostics are unchanged but for the algorithm string (§8) | fg rejectionFixturesMatchGolden; fk failureDiagnosticsShape |
| 920 | 8 | the record stores and restores the same channels, with the new values, in the record format as it is | fst restoredAfterRestartIsBitIdentical; rr layoutIsPinned |
| 921 | 8, as settled | the algorithm string changes (to `batch-temperature-bias-v4`, named only in appendix J and 9.10), so a fit stored under the previous one is stale at its recording's next load, by the existing validity rules | fst codeStampChangeDropsRecordOnLoad; fs registrationShape; a: stored-results; M46 |
| 922 | 8, as settled | after the change every stored fit is computed again once, when something switched on needs it (a stale record is deleted at its recording's load), and the status bar shows it as any computation (phase 2 decision 6) | fst codeStampChangeDropsRecordOnLoad; rc staleRecordDeletedByWorkerCreatesDemand; M45 |
| 923 | 8 | the goldens are captured again from the changed kernel with the existing capture tool, and the golden tests compare against them | fg successFixturesMatchGolden, rejectionFixturesMatchGolden; fk fitTraceMatchesGolden; ru outputTableMatchesGolden |
| 924 | 8, as settled | every fixture's time axis is unchanged: checked once at the re-capture, each success fixture's `_time` byte for byte against the previous capture (section 11), and permanently against the IMU samples of its fitted interval (decision 12) | P2-axis; P1-axis |
| 925 | 9 | the fit is unchanged (graph, noise model, initializer, solver, stopping rule), and so are its states at the fixes and its biases: the pass reads the converged solution and changes nothing in it (sections 2 and 3) | fk fitTraceMatchesGolden; fg successFixturesMatchGolden; P1-ends; M46 |
| 926 | 9 | the pass replaces the kernel's linear reconstruction, which no code, test, golden or document names | a: fusion-reconstruction; P2-pub |
| 927 | 9 | the step model is read from the library's preintegration as it advances, never written again: one integration call and one author of the per-step covariance, and the tests check the pass's covariance against the factor's | a: fusion-model; P1-ends |
| 928 | 9 | GTSAM stays confined to the kernel and the rules on the fusion tooling hold | a: solver-confinement; a: fusion-tooling |
| 929 | 9, as settled | the pass runs after the fit's last iteration inside the same cancellable job and is not a cancellation boundary (decision 9) | a: fusion-model; fg cancelAtEachKindOfBoundary, progressMatchesGoldenBoundaries |
| 930 | 9 | the fit calculation publishes the same channels and its result version follows the algorithm string; the registration, the plot registry and everything above them are unchanged | fs registrationShape; a: stored-results; fr allEightFusionPlotsAreExplicitBacked |
| 931 | 10, as settled | test: equivalence: against a graph with a state at every edge, one-step IMU factors with the same per-step covariance and interval bias, and the fix states and biases held at the fit's values, the reconstruction agrees within a tolerance stated against the mismatch, on `coarse_maneuver` and a rotating recording; a reference with free fix states and biases drifts along the unobservable yaw, so no such tolerance exists for it (decision 7) | P1-eq-coarse; P1-eq-rot |
| 932 | 10 | test: on every success fixture, the corrected state at each interval's second fix is the fitted state to rounding, and the pass's covariance there equals the factor's | P1-ends |
| 933 | 10 | test: a step carrying more noise takes the larger share; with uniform noise the velocity share grows with elapsed time | P1-share |
| 934 | 10, as settled | test: on a recording that does not rotate and whose readings integrate exactly to the fitted states, the reconstruction is the forward integration, the step corrections are zero and the acceleration is the rotated reading (decision 6) | P1-zero |
| 935 | 10 | test: on every success fixture the published acceleration integrated by the kernel's rule reproduces the published velocity change over the fitted interval, and over runs within the stated spread | P1-cons |
| 936 | 10, as settled | test: the published time axis of every success fixture and of a synthetic recording whose GNSS rate exceeds its IMU rate is exactly the IMU samples of the fitted interval; the before-and-after identity is the re-capture's check (decision 12) | P1-axis; P2-axis; P2-fast |
| 937 | 10, as settled | test: the diagnostics carry §7's account and the golden comparison covers it; in portable mode the time of the largest step correction is compared only where that correction exceeds the portable floor (phase 2 decision 3) | fk initializerDiagnosticsShape; fg successFixturesMatchGolden, comparatorHoldsItsBounds; P2-pub |
| 938 | 10 | test: a fit stored under the previous algorithm string is stale at the next load, and one stored after the change restores bit for bit | fst codeStampChangeDropsRecordOnLoad, restoredAfterRestartIsBitIdentical |
| 939 | 10 | test: the audit's confinement rules hold, and the documents describe the reconstruction, what it publishes and what its one pass leaves out | a: solver-confinement; a: fusion-tooling; a: fusion-model; a: fusion-reconstruction |
| 940 | 11 | `docs/` describe the reconstruction and that its output is the IMU-rate posterior to within one linearization, what that leaves out, what is published (the acceleration's consequence, the time axis in one sentence), the diagnostics keys, the algorithm string and the first start, and what the fusion plots show between fixes; tests/README.md section 11 records the goldens captured again and why; the map gives the specification its range with the amended items restated | a: fusion-reconstruction; a: fusion-model |

**Manual steps**, 12.8, each `**M<k> <title> (<items>).**`, pass / fail and
the numbers in the phase report:

- **M45 The fused plots between fixes, and the first start (913, 914, 916,
  922).** The 12.1 preamble, on a logbook copy whose recordings have fits
  stored by a build from before this change, with a logbook column over a
  sensor fusion measurement enabled. Start: the status bar computes every
  recording with IMU data once; quit and start again: nothing is computed.
  With Sensor fusion > Elevation and Vertical acceleration checked beside
  their GNSS counterparts, zoom into a few seconds of freefall: the fused
  lines have a point at every IMU sample, with no straight run between
  fixes and no bend where two samples straddle one; through the parachute
  opening the fused vertical acceleration shows the opening at the IMU
  rate, and the fused elevation's slope changes where it does.
- **M46 The runner's account of the reconstruction (912, 916, 917, 918, 921,
  925).** 12.2's preamble and M10's recording, with `--csv`: exit 0;
  `algorithm` is `batch-temperature-bias-v4`; `dense_output` and
  `limitations` are the texts `docs/SENSOR_FUSION.md` section 4 quotes, and
  the replaced key is absent; the four numbers are finite,
  `max_step_correction_time_s` lies between `start_s` and `end_s`, and all
  four are recorded; the CSV's row count equals `imu_outputs`, its first
  `_time` is at or after `start_s` and its last before `end_s`, and
  consecutive `_time` differences are the recording's IMU interval;
  `stopping.rule`, `seeds[0].iterations` and `objective` are M11's. If
  `TEMP/data` holds the recording, the implementer runs M46 once and
  reports the numbers.

## Decisions

- **D1. Items from §5 onward, one per §10 bullet, one for §11.** §1-§4 have
  no item; the observable statements of §2 and §3 are filed where 9.10's
  intro says. §2's and §6's documentation sentences are carried by 940,
  §7's ("the documentation and the diagnostics say the same") by 917.
- **D2. Settled clauses**, and where the plan departs from the letter, which
  appendix J and 9.10 say plainly: 904 (d is not the factor's residual), 905
  (the formula in the covariance's coordinates, mapped), 910 (which reading
  and which attitude), 911 and 934 (zero step corrections only without
  rotation), 914 (one adjacent step only on the first fix), 915 (the spread
  is decision 8's bound), 917 and 918 (the keys' names), 919 (the algorithm
  string does change), 921 (v4), 922 (recomputed when needed, not all at the
  start), 924 and 936 (before-and-after once; permanently against the
  definition), 929 (no boundary), 931 (the reference holds the ends, not the
  GNSS factors and free biases §10 describes), 937 (the portable rule).
- **D3. Items 847 and 861 are amended.** Read today, "the algorithm string
  is the same", "stored results stay valid" and "the golden fixtures … pass
  unchanged" state the facts §2 and §8 reverse, and their evidence now
  asserts v4 (`registrationShape`) and a stale v3 record
  (`codeStampChangeDropsRecordOnLoad`). Restated: 847 (12, as amended): the
  fusion plots leave the fusion kernel and the fit calculation as they are
  (inputs, outputs, diagnostics, algorithm string) and derive what they add
  from the fit's published outputs, whose values, diagnostics and algorithm
  string are those of the specification of 901-940, with its goldens and
  stored results. 861 (13, as amended): test: the fusion plots leave the fit
  as the golden fixtures and the stored-result tests hold it, against the
  goldens and the algorithm string of the specification of 901-940. Their
  lines stay.
- **D4. Checked and still true**, each with its reason, so not amended:
  104 (its map statement, "reproduces its goldens", holds; appendix B's
  wording is one of the four pinned historical mentions); 213, 219, 226
  (their functions keep their names; 219's "in the reconstruction" holds by
  decision 5); 218 (the covariance is still set before each call); 225, 227
  (their keys remain); 230, 235, 337 (seventeen channels, now bit-identical
  to the new goldens); 234 (no boundary added); 237 (this is another capture
  by the same harness); 306, 507, 606 (each says its own specification left
  the kernel alone and names no value this one changes; 507's "its outputs"
  are the channels the record holds); 312, 316, 317, 408 (validity by the
  algorithm string, which this specification applies); 804, 808, 809, 813,
  854 (they name the fit's channels, which remain the fit's own). A search of
  appendices A-I that finds another statement no longer true is restated
  the same way and reported.
- **D5. Two manual steps** for what no test reaches: a real recording's
  fused plots between fixes and the first start over fits stored by an
  earlier build (M45), and the runner's account on a real recording (M46).
- **D6. The stale paths of 12.2 and section 4** are the fact phase 2
  corrected in section 11 (decision 14), stated in the same form.
- **D7. No new audit rule.** The range grows and section 10 follows the
  rules as they are, so the rule count is unchanged and checkable.

Ready, with caveats: the `P1-…` and `P2-…` labels resolve only against the
phase 1 and 2 reports or the tree; M45 needs fits stored by a build from
before phase 2, which an agent cannot make in `build/`.
