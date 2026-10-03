# Implementation plan: GNSS holes bridged by the IMU

Plan for `PLANS/gnss-holes-bridged.md`, written 2026-10-02 against
`store-requested-calculations` at `bfbcec1` (the specification names
`61f590a`; `bfbcec1` adds one sentence to its section 5 and nothing else is
between them). The specification below is the authority; where the phase
document disagrees with it, the specification wins, except where a decision
below states how the plan reads it.

## Feature specification

## GNSS holes bridged by the IMU

Date: 2026-10-02; amended 2026-10-03 after the first implementation's
escalation (the cap, the growth claim, the slow-tail rule) and again after
the second's review (the never-below clause struck, the cap per interval,
the measurement's wording).
Status: specification for planning. Not an implementation plan. Small enough
to be implemented directly by one agent in one phase. It follows
`PLANS/done/sample-continuity.md`, whose continuity rule the kernel reads
(`src/samplecontinuity.h`, implemented in 1cafa7e..af34628).
Baseline: branch `store-requested-calculations` at `61f590a`; the committed
code is authoritative.
Related: `src/fusion/fusion.cpp` (`planFit`, `kGnssOutageSeconds`,
`kGnssOutageMedians`), `src/fusion/fusionsamples.h/.cpp`
(`requireNoGnssOutage`, `fittedWindow`), `src/fusion/initializer.cpp`
(`segmentBounds`), `src/fusion/fusionoutput.cpp` (the input audit, the
algorithm string), `src/fusion/fitcovariance.h` (the widening window),
`tests/fusion/fusionfixtures.cpp` (`reject_gnss_gap`), `tests/tst_fusion_golden.cpp`,
`tests/tst_fusion_kernel.cpp`, `docs/SENSOR_FUSION.md` sections 2, 6, 7 and
8, `docs/COMPUTED_PLOTS.md`, `docs/DATA_SCHEMA.md` section 12,
`tests/acceptance_map.txt`, `tests/README.md` (the fixture table, section 12).

### 1. Motivation

The kernel rejects a recording whose GNSS fixes are more than max(2 s, 5
median intervals) apart, calling it "disconnected". Two recordings of the
reference corpus fail this way, and in both the hole is in the aircraft
during the climb with the IMU running through it: `24-09-07/08-35-48` (unit
00769, 35 minutes) has one hole of 15.6 s ten minutes before exit, and
`24-09-04/13-35-10` (unit 01086, 56 minutes) has one of 13.2 s and six of
2.4 to 5.4 s in the five minutes before exit. The jumps themselves are
continuous in both. Each recording gets no fusion at all.

The trajectory is connected by the IMU, not by the receiver. The graph
already bridges every interval between fixes with one preintegrated IMU
factor whose covariance grows with the interval; a 15 s interval is the same
factor with more samples. A GNSS hole with a continuous IMU is the case the
fusion exists for, and the model carries its own account of what the bridged
stretch is worth.

### 2. Principles

- **Connectedness is the IMU's.** The one disconnection rule is the IMU gap
  rule of the continuity authority. There is no rule on the spacing of
  fixes short of the cap below: the preintegrated covariance prices the
  hole, and the published accuracies report it.
- **The cap is measured, not guessed.** Within a pass the bias and the scale
  enter an IMU factor to first order, and the settle test rebuilds the graph
  at the fitted values; over one factor spanning a long hole that correction
  is poor enough that the passes stop contracting. On the synthetic
  long-hole recording every hole up to 40 s settles in two or
  three passes and 50 s on the fifth; 52, 54 and 58 s cycle and never
  settle; 60 s settles only on the twelfth of thirty passes. So an interval
  between successive fixes longer than 30 s, twice the longest hole in the
  corpus, is not bridged, whether or not the continuity rule calls it a hole
  (the limit is the factor's span, so a uniformly sparse window is bound by
  it too): the recording is rejected with a reason that names the interval
  and the limit. The limit is a property of the single factor, and a later
  specification that places states inside a long interval removes it.
- **Everything is published.** Every IMU sample inside a hole gets its
  reconstructed state and its accuracies. The growth through a hole is in
  the position and velocity of the sample covariance, which the fit carries
  and the reconstruction composes; the four published accuracies (heading,
  tilt, two accelerations) are bounded by global terms, the heading's
  observability and the bias priors, and grow through a hole only where
  those terms allow, as they do on the reference recording and not on the
  synthetic fixture, where inside the hole they move from one neighbouring
  fix's value towards the other's and the accelerations' follow the
  manoeuvre. Nothing is suppressed; the accuracy tells the user what the
  stretch is worth.
- **The fixes around a hole are ordinary fixes.** They carry their stated
  sigmas, which on the corpus are 100 to 180 m after a hole, and the model
  weights them as it weights every fix.
- **A change that alters what a fit returns changes the algorithm string.**
  Two recordings that were rejections become fits, so the string changes and
  every stored result is dropped once.
- **A slow tail is accepted only when every factor kind fits.** The first
  implementation showed a recording whose GNSS factors were satisfied while
  its IMU factors were ignored (IMU normalized RMS 1707, an accelerometer
  scale of -0.058) accepted as a slow tail, because the rule bounded the
  position and velocity misfit alone. The rule bounds all three.

### 3. What changes

- **The outage rule goes; the cap replaces it.** `requireNoGnssOutage` and
  its two constants are deleted. In their place one kernel constant, the
  longest interval between fixes the fit bridges, 30 s, and one check in
  `planFit` after the window's validation: an interval between successive
  fixes of the fitted window longer than it rejects the recording with a
  reason that names the interval's length, to two decimals, and the limit.
  The holes the continuity authority finds serve the diagnostics; the cap
  walks every interval. Below the cap there is no rule on fix spacing. The
  IMU gap rule, read from the continuity authority, is the only
  disconnection.
- **The segment cutter merges any sparse piece.** Today only the final piece
  of the initializer's cut is merged into its predecessor when it is shorter
  than the minimum or holds fewer than three fixes. With holes, a middle
  piece can hold one or two fixes. Any piece with fewer than three fixes is
  merged into the piece before it, or into the piece after it when it is the
  first; a piece that is the whole window is left alone, and the fitted
  window's own rule of three fixes still rejects a recording with fewer. A
  stretch of whole segment lengths without a fix yields no piece, as it
  already does.
- **The fit and the reconstruction are unchanged in kind.** The IMU factor
  spans the hole as it spans any interval; the reconstruction's per-interval
  pass publishes every IMU sample of the interval; the covariance step's
  adjacent-pair marginals and the widening, whose window already extends to
  the two fixes around a sample when none fall inside it, need nothing.
- **The input audit names the holes.** `input` gains `gnss_holes`, an array
  with one object per hole of the fitted window, each `start_s` (seconds
  since the epoch, like `start_s` of the fit) and `length_s`, in time order,
  empty when there is none. A hole is the continuity authority's hole for
  the GNSS axis of the window.
- **The algorithm string becomes `batch-temperature-bias-v8`.** Every stored
  result, fits and rejections alike, is dropped at its recording's next load
  and the recording is fitted again once when something needs it, as the
  documentation of stored results describes.
- **The slow-tail rule bounds the IMU misfit too.** A final pass at its
  iteration limit is accepted as a slow tail only when its mean relative
  decrease over the window is below the bound and the position, velocity
  and IMU normalized RMS are all below `slowTailMaxNrms`; otherwise the
  rule is `iteration limit`, a solver failure, as today. The tuning field
  keeps its name and value.

### 4. Tests

- **The bridged fixture.** `reject_gnss_gap` (`coarse_maneuver` with fixes
  12 to 23 removed, a 2.6 s hole) becomes the success fixture `bridged_hole`:
  it converges, its golden is captured, and the golden suite holds four fits
  and ten rejections. In the kernel tests, the published attitude and
  accelerations at every IMU sample inside the hole lie within three of their
  own published accuracies of the fixture's generating trajectory; the four
  published accuracies inside the hole are logged against their values at
  the published samples nearest the two fixes around it (bounded by global
  terms, they need not grow, and on this fixture do not); and the position
  and velocity parts of the sample covariance, read through the
  reconstruction's per-interval seam, grow through the hole and collapse at
  the fix after it.
- **The long hole, at the cap and above it.** A second synthetic recording
  with `coarse_maneuver`'s kind of motion and a manoeuvre on both sides of a
  30 s hole, the IMU continuous, converges under the production tuning; the
  test logs its iterations per pass, the largest accuracy inside the hole
  and the residuals at the fix after it. The same recording with a hole
  above the cap is rejected with the reason naming the length and the
  limit, and so is a window whose fixes are uniformly more than 30 s apart,
  which the continuity rule calls no hole. The measurement behind the cap
  (every length to 40 s settles, 50 s on the fifth pass, 52 to 58 s cycle,
  60 s only on the twelfth of thirty passes) is recorded in the
  documentation, not re-run by a test.
- **The sparse piece.** A window whose cut yields a middle piece of two fixes
  is fitted, with the piece merged into its predecessor; a window whose first
  piece has two fixes merges it into its successor.
- **The audit.** `gnss_holes` is in the diagnostics of every fit, empty for
  the three unbroken fixtures and one entry of 2.6 s for `bridged_hole`;
  `git grep` finds no outage constant and no `requireNoGnssOutage` in `src`,
  `tests` or `docs`; the cap is one constant, named by the rejection's
  reason and the documentation.
- **The slow tail.** The slow-tail test gains the case where the IMU
  normalized RMS alone is above the bound and the tail is refused; the
  fusion-improvements item that states the rule is restated "(as
  amended)".
- **The reference recordings.** A manual step, in the form of M11 to M14,
  runs the two recordings of section 1 through the runner and records the
  outcome, the stopping rule, the iterations per pass, the objective, the
  quality, `gnss_holes`, and the largest heading and tilt accuracy inside
  the longest hole against the median accuracy of the recording. The numbers
  go in the report as they are. Under the amended slow-tail rule
  `24-09-04/13-35-10` is expected to end `iteration limit`, a solver
  failure: its second segment's fit reaches its limit and the full fit
  starts from that attitude, which is the solver follow-up the lab notes
  name, not this specification's.
- The acceptance map opens a new hundred for this specification's items.
  `audit_cleanup` and the whole suite green, the exact tests included.

### 5. Documentation

`docs/SENSOR_FUSION.md`: section 6 drops the GNSS gap rejection and states
that a hole in the fixes up to the cap is bridged by the IMU, with the
measurement behind the cap and why a single factor imposes it, and that a
longer interval between fixes is rejected naming the limit; the
stopping-rule paragraph
states the slow tail's three bounds; section 2 says what the user sees over
a hole and that the GNSS plots break there while the fusion plots draw
through; section 7 adds
`gnss_holes` to the input audit and `v8` to the algorithm string's history,
naming both the bridged holes and the continuity rule's 1.5 threshold that
the kernel adopted under the compatibility marker's bump to 3;
section 8 gains the two recordings' numbers from the manual step. The
golden count ("three fits, eleven rejections") becomes four and ten
wherever it is stated. `docs/COMPUTED_PLOTS.md`'s section on holes gains the
sentence about the fusion plots. `docs/DATA_SCHEMA.md` section 12 names
`v8`. `tests/README.md`: the fixture table, section 12's new step, the
specification's appendix and matrix.

### Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, staged by explicit path, with the
session's attribution line; `PLANS/` is never staged.

## Phases

| Phase | Name | Purpose | Dependencies |
|-------|------|---------|--------------|
| 1 | GNSS holes bridged | The whole specification: the outage rule removed from the kernel's plan (§3); the segment cutter's merge of any sparse piece (§3); `gnss_holes` in the input audit (§3); the algorithm string `batch-temperature-bias-v8` and everything that quotes it (§3); the fixture `reject_gnss_gap` turned into the success fixture `bridged_hole`, the long-hole recording, the sparse-piece and audit tests, the re-capture of the goldens (§4); the audit group; the acceptance items in a new hundred with their appendix, matrix and manual step (§4); the documentation of §5 and the documents the kernel's changes reach. | none |

```text
1 GNSS holes bridged
```

One phase. The specification says it is small enough for one agent in one
phase, and its Commit Policy authorizes one commit; a plan of several phases
would ask the orchestrator for commits the policy does not allow. The change
does not split along a green boundary either: deleting the outage rule turns
`reject_gnss_gap` into a success, which changes the golden suite, which
changes the algorithm string, which changes every golden file and every test
and document that quotes the string; and the segment cutter's merge has no
observable case until a hole can reach the fit.

## Key patterns and references

Every path is relative to the repository root. The documenter and the
implementer read the whole file where the line says so; elsewhere the line
says what to look at.

### Project rules

- `CLAUDE.md`: build only in `build-agent/`, the suite sequentially, what
  must stay green, the conventions the audit enforces.
- `CLAUDE.local.md`: this machine's trees and git rules (never push; never
  commit on `master` or `fusion-improvements`).
- `.claude/docs/WORKFLOW-REFERENCE.md`: the stages and the conventions on
  altitude, phases and commits.
- `PLANS/done/sample-continuity-plan/00-overview.md` and
  `01-sample-continuity.md`: the previous one-phase plan, for the shape of a
  phase document, its acceptance criteria, its tests section and its
  decisions.
- `PLANS/done/sample-continuity.md`: the continuity rule this specification
  reads (its sections 3 and 5: the nominal interval, the factor 1.5, the
  runs, the kernel reading the rule for the IMU axis).

### The fusion kernel (`src/fusion/`)

- `src/fusion/fusion.cpp` (read whole): `planFit()` is stage 1 of a run. It
  sets `plan.tuning.maxGap` from `SampleContinuity::holeThreshold()` of the
  whole IMU axis, builds the window with `fittedWindow()`, validates it, and
  then calls `requireNoGnssOutage()` with `max(kGnssOutageSeconds,
  kGnssOutageMedians * nominalInterval(gnssTime))`: the call and the two
  constants at the head of the file go. `fitAndAssemble()` and
  `assembleSuccess()` show where the window, the fit and the diagnostics
  meet; `assembleSuccess()` is the seam the kernel tests use to publish a
  fit of their own.
- `src/fusion/fusionsamples.h/.cpp`: `requireNoGnssOutage()` (declaration
  with its contract comment, definition with its "segments are never joined"
  comment) goes. `fittedWindow()` keeps its rule of three fixes (the
  "Fewer than three GNSS fixes in IMU coverage" rejection) and its IMU range
  `[one before the first fix, one past the last)`; `validateSamples()` runs
  `requireNoImuGapInsideGnssSpan()` on the IMU axis, which is the one
  disconnection that remains. The `Tuning` struct's comment on
  `segmentLength` / `minFinalSegment` states the cutter's merge rule and must
  state the new one.
- `src/fusion/initializer.h/.cpp`: `segmentBounds()` cuts the window's
  fixes into pieces of `segmentLength` from the first fix and merges only the
  final piece (shorter than `minFinalSegment` or fewer than three fixes)
  into the one before it; its contract comment in the header says so.
  `initialize()` fits each piece with `fittedWindow(window, t[a], t[b])` and
  throws `logic_error` if the cut does not hold its fixes. `coarseAttitude()`
  differences velocity between neighbouring fixes (across a hole that is a
  long baseline, which is fine for a coarse start). `initializeSegment()`'s
  prefix loop skips a prefix window with fewer than three fixes with a
  comment that says this "only happens at a GNSS rate far below 1 Hz":
  beside a hole it can happen too; reword, do not change the logic.
- `src/fusion/inputadapter.h/.cpp`: `PreparedInput` carries `audit`, the
  `input` object of the diagnostics, built by `inputAudit()` from the
  channels before the window exists (`epoch_utc_s`, `imu_count`,
  `gnss_count`, `origin_index`, `origin`, `height_method`, `time_method`).
  `gnss_holes` is a property of the fitted window, so it is added after
  `fittedWindow()`; where (in `planFit()` onto `plan.prepared.audit`, or in
  `successDiagnostics()`) is the implementer's.
- `src/fusion/fusionoutput.h/.cpp`: `successDiagnostics()` writes `input`
  from `prepared.audit` and the fit's `start_s` as `window.gnssTime.front()`
  (seconds since the epoch: the frame `gnss_holes[].start_s` uses);
  `failureDiagnostics()` writes `algorithm` and `failure` only, and the
  golden test asserts that key set for rejections, so `gnss_holes` is a
  success key.
- `src/fusion/fusion.h`: `Algorithm[]` is `batch-temperature-bias-v7`, the
  one literal in `src` (the audit counts it), with the comment above it on
  when it changes.
- `src/fusion/fitcovariance.h`: `kWideningHalfWidthS` and
  `wideningFactors()`: the window is the fixes within 2.5 s of a sample,
  extended to the two fixes around it, so a sample inside a hole has a
  window of exactly those two. Nothing changes here; the phase document
  says why.
- `src/fusion/trajectoryreconstruction.h/.cpp`: the per-interval
  reconstruction pass publishes every IMU sample of `[fix k, fix k+1)`
  whatever the interval's length. Nothing changes.
- `src/fusion/imuintegration.cpp`: the attitude propagation reads
  `tuning.maxGap` on the IMU axis. Nothing changes.
- `src/samplecontinuity.h` (read whole): the continuity authority:
  `nominalInterval()`, `holeThreshold()`, `isHoleBefore()`, `runs()`, with
  the definition of a hole (strictly greater than 1.5 nominal intervals) and
  the rule that a caller judging many intervals of one axis asks for the
  threshold once. `gnss_holes` is computed through it.

### Fixtures, goldens and the capture tool

- `tests/fusion/fusionfixtures.h` (read whole): `FusionFixture`
  (`expectSuccess`, the configuration attributes), `fusionFixtures()` ("All
  fourteen fixtures: ... then the eleven rejections"), `fusionFixture(name)`
  and `initializerFixture(name)`, the entry for the kernel's non-golden
  synthetic recordings.
- `tests/fusion/fusionfixtures.cpp` (read whole): the bit-reproducibility
  rules at the head; `coarseManeuver()` (the generator whose motion, biases,
  noise, seed and lattice the new recording extends, and whose truth the
  bridged-hole test compares against: identity attitude, NED acceleration
  `(1.5 - .4t, .8t, -.6 + .1t^2)`); `rejection()`, `removeSamples()`,
  `gnssChannels()`; `rejectionFixtures()` with `reject_gnss_gap`
  (`coarseManeuver()` with GNSS fixes 12..23 removed, "a 2.6 s outage");
  `fusionFixtures()` (the three successes then the rejections);
  `motionStart()` and its neighbours as the pattern for a kernel-test
  recording returned by `initializerFixture()`.
- `tests/fusion_golden_capture.cpp`: iterates `fusionFixtures()`, writes
  `<name>.json` for every fixture and `<name>.channels.txt` for a success,
  prints `** UNEXPECTED **` (exit 2) when success and `expectSuccess`
  disagree, writes `capture.json` with the file and generator hashes. It
  never deletes a file: `reject_gnss_gap.json` is removed by hand.
- `tests/data/fusion/`: the fourteen `.json` goldens, three `.channels.txt`,
  `capture.json`. Every `.json` carries `"algorithm":
  "batch-temperature-bias-v7"`.
- `tests/fusion/fusiongolden.h/.cpp`: the loader and the comparator the
  golden tests use (`loadFusionGolden()`, `fusionChannel()`,
  `fusionChannelNames()`); unchanged unless a success fixture's loading
  needs something new (it should not).
- `tests/fusion/fusionsessions.h/.cpp`: `fixtureSession(name)` builds a
  session from a fixture for the engine-side tests.
- `tests/README.md` section 11 ("Fusion golden regression"): the fixture
  tables (successes with what each exercises, rejections with their reasons),
  the capture history paragraphs (the last is "The capture of 2026-10-01"),
  "### Re-capture procedure" (the PATH, the command with `--revision`, what
  changes and what it means, including "A phase that changes the `algorithm`
  string changes all fourteen `.json` files; the eleven `reject_*.json`
  ..."), and section 10's description of the algorithm-string rule ("the
  goldens say `batch-temperature-bias-v7`").

### Tests of the kernel

- `tests/tst_fusion_golden.cpp`: `kSuccessFixtures` (the three names; the
  rejections are enumerated from `expectSuccess`), `successFixturesMatchGolden`,
  `rejectionFixturesMatchGolden` (asserts the rejection key set is
  `algorithm`, `failure`), `channelsWriterIsTheInverseOfTheLoader` (over the
  success list), `progressMatchesGoldenBoundaries`.
- `tests/tst_fusion_kernel.cpp` (large; read the helpers and the functions
  named): the helpers `fixtureNamed()`, `toChannels()`, `windowOf()`,
  `runPipeline()`, `fixtureFit()` (a `WindowFit`: window, tuning, fit,
  account), `publishedRun()` (through `assembleSuccess()`),
  `diagnosticsOf()`, `fusionChannel()`, `angleDifference()`,
  `rotationFromMatrix()`; `segmentsAreCutOnFixes()` (the `segmentBounds()`
  cases, the pattern for the sparse-piece cases); `validationRejectsEachDefect()`
  and `imuGapRuleIsTheContinuityThreshold()` (the IMU gap rule's tests, which
  stay); `accuraciesFiniteAndPositive()` and `gnssAccuracyScalingNeverLowersThem()`
  (the pattern for reading the four published accuracies of a fit);
  `imuRateMatchesHeldEndsGraphUnderRotation()` (the pattern for comparing a
  published channel with a recording's generating trajectory, sample by
  sample, with `qInfo()` logging of the measured numbers); the lists
  `{"coarse_linear", "coarse_maneuver", "stationary_spin"}` (three places:
  the ceiling test, the scaled-factor test, `gnssAccuracyScalingNeverLowersThem`)
  and `kAccuracyFixtures`; the three `QCOMPARE(... "algorithm" ...,
  "batch-temperature-bias-v7")`; `fitTraceMatchesGolden()`.
- `tests/tst_fusion_session.cpp`: `rejectionIsACachedResult_data()` lists
  `reject_gnss_gap` among five rejection fixtures; `registrationShape` and
  one more place compare `resultVersion` with the v7 string; a loop over
  the three successes.
- `tests/tst_fusion_store.cpp`: a hand-written golden JSON string with the
  v7 string; `codeStampChangeDropsRecordOnLoad` (a record stamped v5 is
  dropped at load: the evidence that a string change drops stored results).
- `tests/tst_fusion_jobs.cpp`: two stamps quoting the v7 string.
- `tests/tst_fusion_runner.cpp`: uses `reject_too_few_fixes` only; its
  `_exact` run compares the runner's output with an in-process run.
- `tests/CMakeLists.txt`: the fusion-tests block (the "fourteen fixtures"
  comment stays true: four and ten).

### Audit and traceability

- `tests/audit/cleanup_audit.cmake`: the `audit_group(...)` convention and
  the `expect_none` / `expect_only` / `expect_count` helpers (read the head
  of the file and the `sample-continuity` group as the pattern for a new
  group); the `fusion-model` group's comment "The goldens say
  batch-temperature-bias-v7" and the `stored-results` group's two rules
  "one authority: the fusion algorithm string" (count 1 in `src`, only in
  `fusion.h`); the `sample-continuity` group's "the readers consult the
  authority" count of `#include "samplecontinuity.h"` (10 files, among them
  `src/fusion/fusion.cpp` and `fusionsamples.cpp`: a new includer changes the
  count and the list, a removed one too); the traceability check at the end
  (the four line forms `<item> <test> <function>`, `<item> audit <slug>`,
  `<item> manual M<n>`, `<item> ci <text>`; a manual line needs
  `**M<n> ` in `tests/README.md`; an audit line needs the group).
- `tests/acceptance_map.txt`: the header's range list (the last entry is
  `1201-1213 sample continuity ...`), then one block per item in the form of
  1201-1213 at the end of the file.
- `tests/README.md`: section 1's table rows for `tst_fusion_golden` and
  `tst_fusion_kernel` (they count "three successes" / "eleven rejections" and
  name test functions); section 9's matrices (9.13 is the latest; the next
  is 9.14); section 10 (one bullet per audit group, in prose that does not
  spell the patterns a group bans when the README is not excluded from the
  rule); section 12.2 (the runner preamble and M11-M14, the form of the new
  manual step: the command with the `TEMP/data/...` path, the outputs under
  `TEMP/runs/`, the Python one-liner that reads the JSON, the numbers "in
  one line"), 12.11 (M54 names the two recordings of this specification);
  appendix M (the latest; the next is N).

### Documents

- `docs/SENSOR_FUSION.md`: section 2 (what the user sees; "Using it");
  section 5 (the initializer: "segments of 600 s from its first fix; a final
  piece shorter than 120 s joins ..."); section 6 (the rejection list with
  the GNSS gap bullet and the IMU gap bullet that cites the continuity
  rule); section 7 (the diagnostics' key list with `input` and its keys, and
  the "Stored results" paragraph with the string's history); section 8 (the
  test table counting "three fits, eleven rejections", the accuracy
  paragraph's "the three fits", the reference-recording tables).
- `docs/COMPUTED_PLOTS.md` section 10 ("Holes in the data"): the paragraph
  "The Sensor fusion plots follow the IMU's samples ...".
- `docs/DATA_SCHEMA.md` section 12 (and the `"records"` example above it
  that quotes the v7 string).
- `docs/CALCULATIONS.md` section 17 ("Stored results": the string's history
  in prose) and section 9 (the compatibility marker, which the v8 history
  sentence of the specification names).

### The reference recordings (this machine)

- `TEMP/data/Data comp 3 - FS 2 - serie nr 2 - 00769 (pers.)/24-09-07/08-35-48`
  and `TEMP/data/Data comp 2 - FS 2 - serie nr 2 - 01086 (pers.)/24-09-04/13-35-10`:
  the two recordings of specification section 1, present and untracked.
  `fusion_runner` with `--csv` gives the twenty-one channels per IMU sample
  (`tests/README.md` M10, M46, M50 show the forms).

## Decisions and constraints

1. **One phase, one commit.** See Phases. The Commit Policy names no tag;
   the orchestrator makes the one commit and no tag.

2. **The baseline is `bfbcec1`.** The specification's own text names
   `61f590a`; `bfbcec1` is the specification's amendment and touches
   `PLANS/` only. The code is that of `61f590a`.

3. **`gnss_holes` is judged on the fitted window's GNSS axis**, as the
   specification says: the continuity authority's `holeThreshold()` of
   `window.gnssTime` (not of the whole recording's), each hole the interval
   between two successive fixes of the window that is strictly above it.
   `start_s` is the time of the fix before the hole, seconds since the
   epoch like the fit's `start_s`; `length_s` is the interval. The array is
   a key of `input` in the success diagnostics only; a rejection's
   diagnostics stay `algorithm` and `failure`. Where the array is computed is
   the implementer's; it is computed once, through the authority, and
   nothing re-derives a hole.

4. **The long-hole recording is a kernel-test recording, not a golden.** The
   specification's golden count (four fits, ten rejections) settles it: it
   joins `motion_start` and its neighbours behind `initializerFixture()`, and
   `fusion_golden_capture` never sees it. Its name is the implementer's
   (`long_hole` is suggested). "`coarse_maneuver`'s motion extended" cannot
   mean the same polynomial over a longer time (its down acceleration grows
   as `.1 t^2`): the recording keeps `coarse_maneuver`'s kind of motion
   (smooth in NED, identity attitude so that the truth stays simple, the
   same biases, noise levels, configuration, lattice and generator rules)
   with bounded accelerations over its whole length, a continuous 100 Hz
   IMU, 5 Hz fixes with one 30 s hole inside (the cap; the generator takes
   the hole's length, and the same recording with a 60 s hole is the
   rejection case above the cap), and enough fixes on each side
   for the initializer, with a manoeuvre on both sides of the hole so that
   yaw is observable in each piece and the test measures the bridge, not
   the initializer's fallback. The function is stated in the generator's
   comment and in `tests/README.md`'s table of the kernel's recordings.
   Michael has confirmed this.

5. **`bridged_hole` is the fourth success fixture.** The same generator call
   and mutation as `reject_gnss_gap` (`coarseManeuver()` with fixes 12..23
   removed), renamed, `expectSuccess` true, listed after `stationary_spin`
   in `fusionFixtures()`, in `kSuccessFixtures` of the golden test and in
   the README's success table; `reject_gnss_gap` leaves
   `rejectionFixtures()`, `tests/data/fusion/reject_gnss_gap.json` is
   removed with `git rm`, and `bridged_hole.json` and
   `bridged_hole.channels.txt` are captured. `tst_fusion_session`'s rejection
   list drops it (four rejections remain; substituting another rejection is
   the implementer's choice).

6. **Which kernel-test lists gain `bridged_hole`.** A list that asserts a
   property of every success fixture (`kAccuracyFixtures`,
   `accuraciesFiniteAndPositive`, `gnssAccuracyScalingNeverLowersThem`,
   `imuRateIsWhatTheFitPublishes`, `fitTraceMatchesGolden` and the other
   golden-trace comparisons, the ceiling and scaled-factor loops) gains it;
   the README's rows and `docs/SENSOR_FUSION.md` section 8's sentences that
   count "the three fits" follow the lists they describe. The documenter
   names the lists; the implementer may add it to more, never to fewer.

7. **The segment cutter's rule, read strictly.** A piece with fewer than
   three fixes merges into its predecessor, the first piece into its
   successor, until no piece has fewer than three unless it is the only
   piece; the final piece's time rule (`minFinalSegment`) is unchanged and
   applies to whatever the final piece is after the merges. A middle piece
   of three fixes is a segment however short in time: the specification
   gives no time rule for middle pieces, and the plan adds none. Michael has
   confirmed this reading: a short middle segment finds no yaw and falls
   back to the coarse start, as a resting segment does today.

8. **The three unbroken goldens change in `algorithm` and `input.gnss_holes`
   only.** None of `coarse_linear`, `coarse_maneuver` and `stationary_spin`
   has a hole or a sparse piece, so the capture must leave their channel
   files byte-identical and their `.json` different in those two places;
   the ten rejections change in `algorithm` alone. The phase's capture
   paragraph in `tests/README.md` section 11 states this, and the
   implementer verifies it with `git diff --stat` before committing. A
   capture that changes anything else is a kernel regression, not a golden
   update.

9. **The audit group is `gnss-holes`.** Its rules: no `requireNoGnssOutage`,
   no `kGnssOutage` constant and no "GNSS gap: fusion unavailable" reason
   text in `src`, `tests`, `docs`, `README.md` or `CMakeLists.txt`, the
   goldens under `tests/data/fusion` searched (a stale capture fails) and
   `tests/README.md` not excluded (the specification names `tests`), so the
   README's section 10 bullet describes the rule in words; the algorithm
   string rules of `stored-results` and the `fusion-model` comment move to
   `v8`. Further rules are the documenter's.

10. **The acceptance items are 1301-1313**, appendix N, matrix 9.14, manual
    step M55 in a new section 12.12 of `tests/README.md`. The documenter
    enumerates the items from the specification's sections 2 to 5 (its
    section 1 has none); every section and every test and documentation
    item of the specification is under exactly one item.

11. **M55 is run by the implementer on this machine** and its numbers go
    into `docs/SENSOR_FUSION.md` section 8 as measured, as the specification
    requires ("section 8 gains the two recordings' numbers from the manual
    step"). The step is written in the form of M11-M14 with the two paths
    above, and reads `gnss_holes` and the CSV for the largest heading and
    tilt accuracy inside the longest hole against the recording's medians.
    The outcome is reported as it is: a recording that does not converge is
    reported, not tuned.

12. **The cap is the specification's, 30 s, measured by the first
    implementation** (every length to 40 s settles, 50 s is marginal, 60 s
    cycles: the measurement is recorded in `docs/SENSOR_FUSION.md`, not
    re-run). The long-hole recording's hole is 30 s and must converge under
    the production tuning; if it does not, that is still an escalation with
    the phase uncommitted, never a quietly lowered cap. The cap is one
    kernel constant in `fusion.cpp` beside where the outage constants were,
    checked in `planFit()` after the window's validation on the fitted
    window's GNSS axis through the continuity authority; the reason names
    the interval, to two decimals, and the limit, in the style of the IMU
   gap's reason; it walks every interval of the fitted window, hole or
   not (second amendment).

15. **The slow tail bounds all three normalized RMS** (specification §2
    last bullet, §3 last bullet): the condition at `factorgraphfit.cpp`'s
    slow-tail acceptance gains the IMU normalized RMS against the same
    `slowTailMaxNrms`; the field keeps its name and value, and its comment,
    `docs/SENSOR_FUSION.md`'s stopping paragraph and the fusion-improvements
    item 244 ("as amended") say three. The test forces the IMU bound alone
    by shrinking the tuning's IMU noise sigmas (the IMU normalized RMS rises
    with the inverse, the GNSS misfit does not), on the forced final pass the
    existing slow-tail cases use. Under this rule `24-09-04/13-35-10` ends
    `iteration limit`; M55 records that as measured.

13. **The IMU gap rule's tests stay as they are.** `validationRejectsEachDefect`,
    `imuGapRuleIsTheContinuityThreshold`, `propagationRefusesIntervalAboveItsThreshold`
    and `reject_imu_gap` are the evidence that the one disconnection remains;
    the phase cites them and changes nothing in them.

14. **Documentation the specification does not list but the change reaches.**
    `docs/SENSOR_FUSION.md` section 5's sentence on the cut (the sparse-piece
    merge), the `Tuning` comment and the `segmentBounds()` contract, and the
    prefix loop's comment, change with the code (CLAUDE.md: a change in
    behaviour updates the document that describes it, in the same change).
    `docs/CALCULATIONS.md` section 17's history sentence names `v8` as
    `DATA_SCHEMA.md`'s does.

### Constraints the code imposes

- The suite runs sequentially in `build-agent/`; the fusion tests time out
  under load. The re-capture runs `fusion_golden_capture` from
  `build-agent/FlySightViewer-build/Release` with the DLL directories on
  `PATH` and `--revision "$(git rev-parse HEAD)"`, as section 11's procedure
  says; the `_exact` tests then prove bit identity on this compiler.
- `fusion_golden_capture` refuses to write when two captures differ (exit
  3) and flags a fixture whose outcome disagrees with `expectSuccess` (exit
  2): `bridged_hole` must have `expectSuccess` true before the capture.
- The rejection diagnostics' key set is asserted by `tst_fusion_golden`:
  `gnss_holes` cannot appear there without changing that test, which the
  specification does not ask for.
- `experiments/fusion_lab/` holds its own copy of the kernel with the
  outage rule; it is ignored by git, outside the audit's pathspec, and is
  not changed.
- The README's section 10 is excluded from some rules because it spells
  their patterns; the new group does not exclude it (decision 9).

## Interfaces between phases

There is one phase. The names that a later specification, the audit and the
documents rely on are fixed here:

- The diagnostics key is `input.gnss_holes`: an array, in time order, of
  objects `{"start_s": <seconds since the epoch of the fix before the
  hole>, "length_s": <seconds>}`, empty when the window has no hole, present
  in every success's diagnostics, absent from a rejection's.
- The algorithm string is `batch-temperature-bias-v8`, in
  `src/fusion/fusion.h` only.
- The success fixture is `bridged_hole`; the rejection fixtures are the ten
  that remain. The long-hole recording is reached through
  `initializerFixture()` by the name the implementer gives it.
- `segmentBounds(gnssTime, segmentLength, minFinalSegment)` keeps its
  signature; its contract comment states the new merge rule.
- `Tuning::maxGap` keeps its name and meaning; it is the only disconnection
  threshold the kernel reads.
- The audit group slug is `gnss-holes`; the acceptance items are 1301-1313
  (1313 the slow-tail rule) and item 244 is restated "(as amended)"; the
  manual step is M55; the README's appendix is N and its matrix 9.14.

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, staged by explicit path, with the
session's attribution line; `PLANS/` is never staged.
