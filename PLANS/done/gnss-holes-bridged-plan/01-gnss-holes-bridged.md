# Phase 1: GNSS holes bridged

The whole of `PLANS/gnss-holes-bridged.md`, as reproduced in
`00-overview.md`, in one commit. The overview's decisions 1-14 apply as
written; this document applies them to the code as it is at `bfbcec1`.

**Amended 2026-10-03** after the first implementation's escalation, to the
specification as amended (overview, its copy) and overview decisions 12 and
15: the cap at 30 s with the rejection above it (sections 1 and 6, criteria
1 and 8), the growth asserted on the sample covariance with the published
accuracies never lower (criteria 2 and 7), and the slow-tail rule bounding
the IMU normalized RMS (section 4a, criterion 13, item 1313). The tree
holds the first implementation uncommitted; the implementer continues from
it, changing what these amendments change and nothing else.

**Findings against the overview (none blocking; each is settled below).**

1. The kernel tests publish a fit through `assembleSuccess(prepareInput(channels),
   ...)` directly (`publishedRun()`, `wideningGrowsWithAnUnderstatedSigma`),
   with a fresh audit from `prepareInput()`. A `gnss_holes` inserted into
   `plan.prepared.audit` by `planFit()` would be missing from what that seam
   publishes. The array is therefore computed in `successDiagnostics()`,
   which already receives the window (decision D1).
2. `tst_fusion_kernel::initializerFixturesAreDeterministic` asserts each
   initializer recording's stated rate (12.5 or 26 Hz). The long-hole
   recording logs its IMU at 100 Hz and states 104 (overview decision 4), so
   that assertion gains a case when the recording joins the loop.
3. `wideningIsOneAtTheModel` and `firstNodeHeadingIsTheHeadingCheck` iterate
   `kAccuracyFixtures`, which gains `bridged_hole` (overview decision 6).
   The first asserts that the widening is exactly one on every fixture of
   the list. Inside the hole the window is the two fixes around it, a ratio
   over little redundancy, so the factor there may exceed one although the
   fixture's noise is under its stated sigmas. The rule: if the widening
   exceeds one only at samples strictly inside the hole, the assertion
   excludes those samples for `bridged_hole`, with the reason in the test's
   comment, and the measured factor goes in the report; a widening above one
   anywhere else on the fixture is a defect. The fixture stays in the list
   either way.
4. The traceability check requires a test or audit line for every item; a
   manual line never stands alone. The reference-recording item (1310)
   therefore cites the audit group, which pins the two recordings' rows in
   `docs/SENSOR_FUSION.md` section 8.

## Purpose

The kernel stops rejecting a recording for the spacing of its GNSS fixes. A
hole in the fixes with the IMU running through it is bridged by the one
preintegrated IMU factor that already spans every interval; every IMU sample
inside the hole is published with its state and its accuracies, which grow
through the hole and collapse at the next fix. The initializer's cutter no
longer assumes that only the final piece can be sparse. The diagnostics name
the holes of the fitted window. Because two rejections become fits, the
algorithm string moves to `batch-temperature-bias-v8` and every stored result
is dropped once. One phase, one commit: the change does not split along a
green boundary (overview, Phases).

## Dependencies

Depends on nothing and blocks nothing. It starts from
`store-requested-calculations` at `bfbcec1`, whose code is `61f590a`'s. It
consumes `src/samplecontinuity.h` as it is: `nominalInterval()`,
`holeThreshold()`, `isHoleBefore()` over a `TimeAxis` made from a
`std::vector<double>`, and the rule that a caller judging many intervals of
one axis asks for the threshold once. Nothing in the authority changes.

## What changes

### 1. The kernel's plan (spec §2 first bullet, §3 first bullet; overview decision 13)

- `requireNoGnssOutage()` goes from `src/fusion/fusionsamples.h/.cpp`, with
  its contract comment and its "segments are never joined" comment;
  `kGnssOutageSeconds`, `kGnssOutageMedians` and the call in `planFit()`
  (`src/fusion/fusion.cpp`) go with it. In their place the cap (overview
  decision 12): one constant in `fusion.cpp`, the longest GNSS hole the fit
  bridges, 30 s, with a comment giving the measurement and the reason (the
  first-order bias and scale corrections over one factor); and one check in
  `planFit()` after `validateSamples()`, before the configuration's checks,
  that walks every interval between successive fixes of the fitted window,
  hole or not (second amendment: the limit is the factor's span; the holes
  the continuity authority finds serve the diagnostics), and throws
  `std::invalid_argument` for the first interval longer than the cap, with
  a reason in the IMU gap's style naming the interval to two decimals and
  the limit ("GNSS fixes 60.00 s apart; fusion bridges at most 30 s").
  Below the cap `planFit()` runs no check on the spacing of fixes.
- The IMU gap rule is the only disconnection: `plan.tuning.maxGap` from
  `SampleContinuity::holeThreshold(full.imuTime)`,
  `requireNoImuGapInsideGnssSpan()` in `validateSamples()`, and
  `requireNoImuGap()` in the propagation (`src/fusion/imuintegration.cpp`),
  all unchanged, with their messages. `validationRejectsEachDefect`,
  `imuGapRuleIsTheContinuityThreshold`,
  `propagationRefusesIntervalAboveItsThreshold` and `reject_imu_gap` are not
  touched.
- `fittedWindow()` keeps its rule of three fixes and its message, and its IMU
  range `[one before the first fix, one past the last)`.
- `requireUsableRecording()`, `requireReadingsOnLattice()` and
  `requireStatedRates()` keep their places and order: the configuration's
  checks still come after every check of the recording.

### 2. The segment cutter (spec §3 second bullet; overview decision 7)

- `segmentBounds(gnssTime, segmentLength, minFinalSegment)` in
  `src/fusion/initializer.cpp` keeps its signature and its cut: fix `i`
  belongs to the piece whose `[t0 + k L, t0 + (k+1) L)` contains it, and a
  stretch of whole segment lengths without a fix yields no piece.
- The merge rule becomes: a piece with fewer than three fixes is merged into
  the piece before it, or into the piece after it when it is the first,
  until no piece has fewer than three unless it is the only piece. Then the
  final piece's time rule as today: shorter than `minFinalSegment` merges it
  into the piece before it, applied to whatever the final piece is after the
  fix-count merges. A middle piece of three fixes stays, however short in
  time. A window that is one piece is left alone; a window with fewer than
  three fixes never reaches the cutter (`fittedWindow()` rejects it).
- Every case of `segmentsAreCutOnFixes` holds unchanged: the only pieces that
  rule touches today are final ones, and a sparse final piece is merged under
  both the old and the new rule.
- The contract comment of `segmentBounds()` in `initializer.h` and the
  `Tuning` comment on `segmentLength` / `minFinalSegment` in
  `fusionsamples.h` state the new rule. The comment in `growPrefix()` that
  says fewer than three fixes "only happens at a GNSS rate far below 1 Hz"
  is reworded (a prefix window beside a hole can have them too); the `continue`
  is unchanged.
- `initialize()` keeps its `logic_error` check: a merged piece `[a, b]` still
  holds exactly the fixes `a..b`, so `fittedWindow(window, t[a], t[b])`
  passes it. `coarseAttitude()` is unchanged; at a fix beside a hole its
  difference spans the hole, a long baseline that is fine for a coarse start.

### 3. The input audit (spec §3 fourth bullet; overview decision 3)

- `input` gains `gnss_holes`: an array, in time order, of objects
  `{"start_s": <s>, "length_s": <s>}`, one per hole of the fitted window's
  GNSS axis, empty when there is none. `start_s` is the time of the fix
  before the hole in the frame of the fit's `start_s` (seconds since the
  epoch, `window.gnssTime[k - 1]`); `length_s` is `window.gnssTime[k] -
  window.gnssTime[k - 1]`. A hole is `SampleContinuity::isHoleBefore(window.gnssTime,
  k, holeThreshold(window.gnssTime))`: the threshold of the window's own
  axis, asked once, not the whole recording's.
- It is computed in `successDiagnostics()` (`src/fusion/fusionoutput.cpp`),
  which inserts it into a copy of `prepared.audit` before writing `input`
  (decision D1). `fusionoutput.cpp` then includes `samplecontinuity.h`.
  `inputAudit()` in `inputadapter.cpp` is unchanged: it runs before the
  window exists.
- A rejection's diagnostics stay `{"algorithm", "failure"}` (and `stopping`
  / `quality` for a failed or completed pass): `failureDiagnostics()` is
  unchanged, and `rejectionFixturesMatchGolden` keeps asserting that key set.
- Expected values: empty on `coarse_linear`, `coarse_maneuver` and
  `stationary_spin`; one entry on `bridged_hole`, `start_s` about 2.2 (the
  fix `j = 11` of the generator, in seconds since the epoch, which is fix
  `j = 0` at -0.163 s) and `length_s` about 2.6, the exact bits being the
  golden's; one entry on the long-hole recording.

### 4. The algorithm string (spec §2 fourth bullet, §3 fifth bullet)

`Algorithm[]` in `src/fusion/fusion.h` becomes `batch-temperature-bias-v8`;
its comment is unchanged. Every quote of `batch-temperature-bias-v7` at
`bfbcec1` (verified with `git grep`), and what happens to it:

- `src/fusion/fusion.h`: the one literal in `src`; becomes v8.
- `tests/data/fusion/*.json` (fourteen files): re-captured (section 6);
  `reject_gnss_gap.json` is removed, `bridged_hole.json` added.
- `tests/tst_fusion_kernel.cpp`: the three `QCOMPARE(... "algorithm" ...)` in
  `biasSettledByCostTest`, `failureDiagnosticsShape` and
  `constantTemperatureKeepsSlopeAtPrior` compare v8.
- `tests/tst_fusion_session.cpp`: `registrationShape` (the descriptor's
  `resultVersion`) and `restoredFitIsIndistinguishable` (the snapshot's)
  compare v8.
- `tests/tst_fusion_store.cpp`: `kSolverFailureDiagnostics`, the hand-written
  diagnostics of `restoredSolverFailureShowsBadge`, says v8.
  `codeStampChangeDropsRecordOnLoad` stamps v5 as the stale version and
  needs nothing: a record stamped with any earlier string is dropped at load,
  which is the evidence that the change drops stored results.
- `tests/tst_fusion_jobs.cpp`: the stamps of `columnOnFusionOutputIsCachedFromRecord`
  and `workerRefillsColumnFromStoredFit` say v8.
- `tests/audit/cleanup_audit.cmake`: the `fusion-model` comment "The goldens
  say batch-temperature-bias-v7" and the two `stored-results` rules "one
  authority: the fusion algorithm string" move to v8.
- `tests/README.md`: the section 10 sentence "the goldens say
  `batch-temperature-bias-v7`" and the re-capture procedure's "The
  `algorithm` string is `batch-temperature-bias-v7` since the lattice rule"
  move to v8 (with v7 as history); the matrix row 1043 names v8 as the
  current string, v6 and v7 as history; the capture paragraph of 2026-10-01
  ("changed to `batch-temperature-bias-v7`") is history and stays.
- `docs/SENSOR_FUSION.md` section 7 (the key list's `algorithm` and the
  "Stored results" history), `docs/DATA_SCHEMA.md` (the `"records"` example
  above section 12), `docs/CALCULATIONS.md` section 17: v8, with the history
  sentence that section 5 of the specification prescribes.

### 4a. The slow-tail rule (spec §2 last bullet, §3 last bullet; overview decision 15)

- `factorgraphfit.cpp`, the slow-tail acceptance: the condition gains
  `result.quality.imuNrms < c.slowTailMaxNrms` beside the position and
  velocity bounds; the comment above it ("judged ... on the misfit to the
  GNSS measurements") says all three factor kinds. `Tuning::slowTailMaxNrms`'s
  comment in `fusionsamples.h` says "position, velocity and IMU".
- No golden changes: no fixture ends on a slow tail.
- `slowTailAtTheIterationLimit_data` gains the row "imu nrms bound fails":
  the forced final pass of the existing rows with the tuning's IMU noise
  sigmas shrunk (by a factor of 100 or whatever the fixture needs) so that
  the IMU normalized RMS is above the bound while the position and velocity
  ones are below it, and the tail is refused with `iteration limit`. The
  function logs the three NRMS of each row.

### 5. What does not change, and why (spec §3 third bullet)

- The graph: one preintegrated IMU factor per interval between fixes,
  whatever its length (`factorgraphfit`, `imuintegration`); the fixes around
  a hole carry their stated sigmas and are weighted as every fix.
- The reconstruction: `reconstructAtImuRate()` publishes every IMU sample of
  `[fix k, fix k+1)` per interval (`trajectoryreconstruction.h`).
- The covariance step and the widening: the window of `wideningFactors()`
  is the fixes within `kWideningHalfWidthS` of a sample, extended to the two
  fixes around it, so a sample inside a hole longer than 5 s has a window of
  exactly those two, `N = 2`, with redundancy `6N - 9 = 3`; the contract
  already states it (`fitcovariance.h`).
- The attitude propagation reads `tuning.maxGap` on the IMU axis.
- `experiments/fusion_lab/` keeps its own copy of the kernel; it is ignored
  by git and outside the audit.

### 6. Fixtures, goldens and the capture (spec §4 first and second bullets; overview decisions 4, 5, 8)

- `tests/fusion/fusionfixtures.cpp`: the `reject_gnss_gap` block leaves
  `rejectionFixtures()`; `bridged_hole` is `coarseManeuver()` with GNSS fixes
  12..23 removed by `removeSamples(gnssChannels(f), 12, 12)`, named
  `bridged_hole`, `expectSuccess` true, listed fourth in `fusionFixtures()`
  after `stationarySpin()`. Its generator comment says what it is: a 2.6 s
  hole, the IMU continuous. `fusionFixtures().size()` stays 14.
- The long-hole recording (`long_hole`, already written) keeps its design
  with the hole at 30 s: the fixes removed are those strictly inside
  `(29.9 s, 59.9 s)`, and the generator takes the hole's length so that the
  same recording with the 60 s hole is the rejection case above the cap
  (`holeAboveTheCapIsRejected`, below), which `initializerFixture()` does
  not list. Its generator comment and the README's table state both. As
  first written it joins `motionStart()` and its neighbours behind
  `initializerFixture()`: `coarse_maneuver`'s kind of motion with bounded acceleration
  over its whole length (smooth in NED, identity attitude, the same biases,
  noise levels, seed rules, configuration and lattice), a continuous 100 Hz
  IMU stating 104 Hz, 5 Hz fixes with one 30 s hole inside (60 s in the
  rejection case above the cap) and enough fixes
  on each side for the initializer, under the bit-reproducibility rules at
  the head of the file. The motion must put a manoeuvre on both sides of
  the hole, so that yaw is observable in each piece and the test measures
  the bridge rather than the initializer's fallback: a periodic, zero-mean
  acceleration of `scaleRecording()`'s form on a horizontal axis throughout
  does that for every window, the hole included. The function goes in the
  generator's comment and in the README's table of the kernel's recordings.
  `fusion_golden_capture` never sees it.
- `tests/fusion/fusionfixtures.h`: the comments of `fusionFixtures()`
  ("the eleven rejections") and `initializerFixture()` (its list) follow.
- `tests/tst_fusion_golden.cpp`: `kSuccessFixtures` gains `bridged_hole`; the
  rejection rows come from `expectSuccess` and need nothing.
- The capture, after the kernel change and with `expectSuccess` set: from
  `build-agent/FlySightViewer-build/Release` with the DLL directories on
  `PATH` and `--revision "$(git rev-parse HEAD)"`, as section 11's procedure
  says. Expected: fourteen lines, four `succeeded`, ten `rejected`, no
  `** UNEXPECTED **`, "wrote 19 files", exit 0. Then `git rm
  tests/data/fusion/reject_gnss_gap.json` (the tool never deletes).
- What the capture must change: `algorithm` in all fourteen `.json`;
  `input.gnss_holes` (empty) in the three unbroken successes' diagnostics;
  `bridged_hole.json` and `bridged_hole.channels.txt`, new; `capture.json`.
  What it must not: the three channel files, byte-identical (`git diff
  --quiet -- tests/data/fusion/{coarse_linear,coarse_maneuver,stationary_spin}.channels.txt`),
  and everything else in their `.json` (trace, progress, rows, objective,
  residuals, stopping, quality, model, initializer, accuracy); the ten
  rejections in `algorithm` alone. The implementer verifies with `git diff
  --stat` and `git diff` before committing; anything else is a kernel
  regression, not a golden update.

### 7. Order of work

1. The kernel: sections 1 to 4 (the string last or first, it does not
   matter; everything is one commit).
2. The fixtures: `bridged_hole` with `expectSuccess` true, the long-hole
   recording, the lists of section "Tests" below.
3. The kernel tests that read no golden (`bridgedHoleFollowsTheTruth`,
   `longHoleConverges`, `sparsePiecesAreMerged`, `gnssHolesInTheAudit`)
   written and green: the kernel is judged against the generating trajectory
   before anything is captured from it. A long hole that does not converge
   stops here (overview decision 12): no cap, no weakened test; the measured
   outcome goes to the orchestrator with the phase uncommitted.
4. The capture, then `cmake build-agent/FlySightViewer-build` (the exact-test
   gate reads `capture.json` at configure time), then `-L fusion` and
   `-L exact`: `successFixturesMatchGolden`, `fitTraceMatchesGolden` and the
   `_exact` runs prove bit identity on this compiler.
5. The audit group, the map, the README, the documents; `audit_cleanup` and
   the whole suite, sequentially, in `build-agent/`.
6. M55 on this machine; its numbers into `docs/SENSOR_FUSION.md` section 8
   and the report.

## Interfaces

Provided, as the overview fixes them:

- `input.gnss_holes` in every success's diagnostics, absent from a
  rejection's: `[{"start_s": <seconds since the epoch of the fix before the
  hole>, "length_s": <seconds>}, ...]`, in time order, `[]` without a hole.
- `Fusion::Algorithm` is `batch-temperature-bias-v8`, in `src/fusion/fusion.h`
  only.
- `segmentBounds(gnssTime, segmentLength, minFinalSegment)`: the signature
  unchanged; the contract comment states the merge rule of section 2.
- `Tuning::maxGap`: unchanged, the only disconnection threshold the kernel
  reads.
- Fixtures: `bridged_hole` the fourth success; the ten rejections; the
  long-hole recording through `initializerFixture()`.
- Audit group `gnss-holes`; acceptance items 1301-1313; manual step M55 in
  section 12.12; appendix N; matrix 9.14.

Consumed: `src/samplecontinuity.h` as it is.

## Acceptance criteria

Each criterion is traced to the specification (§) and its item; the items
are 1301-1313 (decision D2; 1313 added by the amendment).

1. (§2 connectedness and the cap; §3 the outage rule goes; 1301) `git grep`
   over `src tests docs README.md CMakeLists.txt` finds no
   `requireNoGnssOutage`, no `kGnssOutage` and no "GNSS gap: fusion
   unavailable". Below the cap `planFit()` runs no check on fix spacing; a
   hole above it is rejected with a reason naming its length and the 30 s
   limit, before the configuration's checks. `reject_imu_gap` still rejects
   with "IMU gap at 0.353000 s; fusion unavailable across missing data", and
   an IMU interval of 1.55 nominal intervals is still rejected while 1.45 is
   not. A 2.6 s hole and a 30 s hole are fitted under the production tuning;
   a 60 s hole is rejected.
2. (§2 everything is published; §3 unchanged in kind; 1302) On `bridged_hole`
   every IMU sample inside the hole is published, with all four accuracies
   finite and positive; what the pipeline publishes is the reconstruction on
   the test's own fit bit for bit, with the hole's interval one IMU factor
   whose end covariance is the factor's; the widening of every sample
   outside the hole is one, and inside it is one or the measured value, by
   the rule of finding 3. The growth: the position and velocity blocks of
   the sample covariance `P_j` of the hole's interval, read through
   `reconstructInterval()`, rise from the fix before the hole to a maximum
   inside it and collapse at the fix after it; the four published
   accuracies at every sample inside the hole are logged against their
   values at the published samples nearest the two fixes around it (the
   never-below clause was measured false and struck by the second
   amendment).
3. (§2 the fixes around a hole are ordinary fixes; 1303) On `bridged_hole`
   the converged graph has one scaled IMU factor per interval, the hole's
   included, each preintegrated at the fitted scale; doubling every GNSS
   sigma lowers no published accuracy; the position and velocity residuals
   at the fix after the long hole are ordinary entries of `residuals` and the
   test logs them.
4. (§3 the cutter; §4 the sparse piece; 1304) On hand-built axes: a middle
   piece of one or two fixes is absent from the cut and its fixes end the
   piece before it; a first piece of two fixes is absent and its fixes start
   the piece after it; two consecutive sparse middle pieces both end in
   their predecessor; a middle piece of exactly three fixes stays; a sparse
   final piece and a short final piece merge as today; a stretch of whole
   segment lengths without a fix yields no piece. Every existing case of
   `segmentsAreCutOnFixes` passes unchanged. On the long-hole recording
   under a test `segmentLength` that leaves one or two fixes in a middle
   piece, the initializer's account shows that piece absent, its fixes in the
   predecessor, and the fit converges.
5. (§3 the input audit; 1305) `input.gnss_holes` has the shape and frame of
   "Interfaces": `start_s` equals the window's fix time before the hole,
   `length_s` the interval, judged by `SampleContinuity::isHoleBefore()`
   against `holeThreshold()` of the window's GNSS axis; it is present in the
   diagnostics of every success, those published through `assembleSuccess()`
   included, and absent from every rejection's, whose key set is still
   `algorithm`, `failure`.
6. (§2, §3 the algorithm string; 1306) `Fusion::Algorithm` is
   `batch-temperature-bias-v8`; the fit's `resultVersion` and every
   diagnostics object say so; a record stamped with an earlier string is
   dropped at load; `batch-temperature-bias-v7` occurs nowhere in `src`,
   `tests` (the goldens included) or `docs`, and in `tests/README.md` only
   as history.
7. (§4 the bridged fixture; 1307) `bridged_hole` converges; its golden is
   captured and matches, exact mode included; the golden suite holds four
   fits and ten rejections; `reject_gnss_gap` does not exist. At every IMU
   sample inside the hole the published heading is within three `headingAcc`
   of 0, the tilt within three `tiltAcc` of 0, the acceleration error's
   component along the published horizontal direction within three
   `accHAcc` and its down component within three `accDAcc` of the
   generating `(1.5 - .4t, .8t, -.6 + .1t^2)`; each published accuracy at
   every sample inside the hole is logged against its value at the
   published samples nearest the two fixes around the hole from outside
   (not asserted: second amendment); and the position and velocity parts
   of the interval's sample covariance grow through the hole and collapse
   at the fix after it (criterion 2). The test logs the measured maxima,
   the covariance growth and the sample count inside the hole.
8. (§4 the long hole at the cap and above it; 1308) The long-hole
   recording, its hole 30 s, converges under the production tuning; the
   test logs its iterations per pass, the largest of each accuracy inside
   the hole, and the squared whitened position and velocity residuals at
   the fix after the hole. The same recording with a 60 s hole is rejected
   with the reason naming 60.00 s and the 30 s limit, and so is a window
   whose fixes are uniformly 31 s apart with no hole by the continuity
   rule. The recording is deterministic and states its configuration like
   the others. A non-convergence at 30 s is an escalation (overview
   decision 12).
13. (§2 and §3 the slow tail; 1313; item 244 as amended) The slow-tail
    acceptance requires the position, velocity and IMU normalized RMS all
    below `slowTailMaxNrms`; `slowTailAtTheIterationLimit` refuses a forced
    tail whose IMU normalized RMS alone is above the bound; no golden
    changes; under the rule `24-09-04/13-35-10` ends `iteration limit` in
    M55 and section 8.
9. (§4 the audit; 1309) `gnss_holes` is in the diagnostics of every fit:
   `[]` for the three unbroken fixtures, one entry of about 2.6 s on
   `bridged_hole`, one of 30 s on the long-hole recording; the four
   success goldens carry it and the ten rejections do not;
   `audit_cleanup` passes with the group `gnss-holes`.
10. (§4 the reference recordings; 1310) M55 has been run on this machine on
    `24-09-07/08-35-48` and `24-09-04/13-35-10`; its numbers are in
    `docs/SENSOR_FUSION.md` section 8 and the report as measured, a
    non-converging recording reported, not tuned.
11. (§4 the map and the suite; 1311) `tests/acceptance_map.txt` has the
    items 1301-1313 with at least one test or audit line each, M55 on 1310;
    `audit_cleanup` and the whole suite are green, the `_exact` runs
    included.
12. (§5; 1312) The documents say what "Documentation" below lists; the audit
    pins the sentences it can.

## Tests

Run in `build-agent/` only, sequentially, Release (`CLAUDE.local.md`).

**`tst_fusion_kernel`** (the function names below are cited by the map;
the implementer keeps them or changes the map with them).

- `bridgedHoleFollowsTheTruth` (new; criterion 7): `publishedRun("bridged_hole")`
  (through `assembleSuccess()`), the samples strictly inside the hole found
  from `input.gnss_holes`, compared with the generator's identity attitude
  and NED acceleration sample by sample, in the manner of
  `imuRateMatchesHeldEndsGraphUnderRotation` (`qInfo()` logging of the
  measured numbers; `angleDifference()` and `rotationFromMatrix()` help).
- `longHoleConverges` (new; criteria 1, 3, 8): `runPipeline()` on the
  long-hole recording (30 s hole) with `Tuning{}` and a `PipelineTrace`;
  asserts `Succeeded` and `trace.converged`; logs iterations per pass, the
  largest of each accuracy inside the hole and the residuals at the fix
  after it.
- `holeAboveTheCapIsRejected` (new; criteria 1, 8): the same generator with
  a 60 s hole through `runPipeline()`: `Rejected`, the reason naming 60 s
  and 30 s, the diagnostics `{"algorithm", "failure"}`; a hole of exactly
  the cap's length is not rejected by it (the cap is "longer than"); and
  the recording without a hole with its fixes thinned to one every 31 s,
  no hole by the continuity rule, is rejected too, while one every 30 s
  passes the plan (second amendment).
- `slowTailAtTheIterationLimit` (amended; criterion 13): the new data row
  of section 4a.
- `sparsePiecesAreMerged` (new; criterion 4): `segmentBounds()` on hand-built
  axes in `segmentsAreCutOnFixes`'s style, and `initialize()` (or the
  pipeline) on the long-hole recording under a test `segmentLength`, reading
  the account's `firstFix` / `lastFix`.
- `gnssHolesInTheAudit` (new; criteria 5, 9): the array on the four success
  fixtures and the long-hole recording through `publishedRun()`, its values
  against the fixtures' construction (not against a golden), its order, and
  that `failureDiagnostics()` has no such key.
- Amended: `kAccuracyFixtures`, the `{"coarse_linear", "coarse_maneuver",
  "stationary_spin"}` lists of `dampingCeilingChangesNothingBelowIt`,
  `fitRepreintegratesAtTheFittedScale` and `gnssAccuracyScalingNeverLowersThem`,
  and the list of `accuraciesFiniteAndPositive` gain `"bridged_hole"`;
  `initializerFixturesAreDeterministic`'s list gains the long-hole recording
  and its rate assertion the 104 Hz case; the three algorithm `QCOMPARE`s say
  v8. `fitTraceMatchesGolden_data`, `imuRateEndsAreTheFit_data` and
  `imuRateIsWhatTheFitPublishes_data` enumerate `expectSuccess` and gain
  `bridged_hole` without an edit. `segmentsAreCutOnFixes`,
  `validationRejectsEachDefect`, `imuGapRuleIsTheContinuityThreshold` and
  `propagationRefusesIntervalAboveItsThreshold` are unchanged. The long-hole
  recording may join `kAccuracyFixtures` as well; if it does, the README's
  row and section 8's counts follow.

**`tst_fusion_golden`.** `kSuccessFixtures` gains `bridged_hole`
(`successFixturesMatchGolden`, `channelsWriterIsTheInverseOfTheLoader`);
`fixturesAreDeterministic` (14, outcomes from `expectSuccess`) and
`rejectionFixturesMatchGolden` (the key set) are unchanged in text.

**`tst_fusion_session`.** `rejectionIsACachedResult_data` drops
`reject_gnss_gap` (four rejections remain; substituting another is the
implementer's choice); `registrationShape` and `restoredFitIsIndistinguishable`
compare v8. **`tst_fusion_store`**: `kSolverFailureDiagnostics` says v8.
**`tst_fusion_jobs`**: the two stamps say v8. **`tst_fusion_runner`**:
unchanged (`reject_too_few_fixes`, `coarse_maneuver`).

**Audit: group `gnss-holes`** in `tests/audit/cleanup_audit.cmake`, in the
`sample-continuity` block's shape with `Allow:` comments, `WB_START` /
`WB_END` only, each rule planted once to prove it matches. The goldens under
`tests/data/fusion` are inside the `tests` pathspec, so a stale capture
fails; `tests/README.md` is not excluded (overview decision 9), so section
10's bullet describes the rules in words without spelling their patterns.
The rules:

- `requireNoGnssOutage|kGnssOutage` absent from `src tests docs README.md
  CMakeLists.txt`; the reason text "GNSS gap: fusion unavailable" absent
  from the same; `reject_gnss_gap` absent from the same;
- the cap: the number 30 as a hole length appears in `src` once, in the
  constant's definition in `fusion.cpp` (the reason text is formatted from
  it), and `docs/SENSOR_FUSION.md` states it once with the measurement;
- the slow tail: `slowTailMaxNrms` is compared in `factorgraphfit.cpp` on
  exactly three lines (position, velocity, IMU);
- `batch-temperature-bias-v7` absent from `src tests docs README.md
  ":!tests/README.md"` (the README's capture history and row 1043 quote it
  as history);
- in `tests/data/fusion`: `"algorithm": "batch-temperature-bias-v8"` on
  exactly 14 lines, `"outcome": "succeeded"` on 4, `"outcome": "rejected"`
  on 10, `"gnss_holes"` on 4;
- `"gnss_holes"` on exactly one line of `src`, in `src/fusion/fusionoutput.cpp`
  (the one writer);
- `docs/SENSOR_FUSION.md`: no "GNSS gap longer than" and no "max(2 s";
  section 6's bridging sentence pinned by a phrase of the implementer's
  choice ("bridged by the IMU" suggested) once; `08-35-48` and `13-35-10`
  once each (section 8's rows); `batch-temperature-bias-v8` on two lines
  (section 7's key list and history);
- `docs`: no "three fits", "eleven rejections" or "three golden successes";
  `docs/COMPUTED_PLOTS.md`: the fusion-plots sentence of section 10 pinned
  by a phrase once; `docs/DATA_SCHEMA.md` and `docs/CALCULATIONS.md`:
  `batch-temperature-bias-v8` once each.

Also: the `fusion-model` comment and the two `stored-results` rules move to
v8 (section 4); the `sample-continuity` group's "the readers consult the
authority" count stays 10, since the holes are walked once in
`fusionsamples.cpp` (as implemented, in place of D1's writer-side walk); the file's head comment gains a bullet for the group; the
traceability block's comment, range test and message gain 1301-1313 and a
`foreach(item RANGE 1301 1313)`.

**`tests/acceptance_map.txt`.** Header: fourteen specifications and ranges,
the `1301-1313` entry in the form of `1201-1213`'s, "9.1 to 9.14", the
coverage sentence. A last section "GNSS holes bridged by the IMU
(PLANS/gnss-holes-bridged.md): thirteen items" in the 1201 section's form,
one comment line per item, then:

```text
1301 tst_fusion_golden successFixturesMatchGolden
1301 tst_fusion_golden rejectionFixturesMatchGolden
1301 tst_fusion_kernel longHoleConverges
1301 tst_fusion_kernel validationRejectsEachDefect
1301 tst_fusion_kernel imuGapRuleIsTheContinuityThreshold
1301 audit gnss-holes
1302 tst_fusion_kernel bridgedHoleFollowsTheTruth
1302 tst_fusion_kernel imuRateIsWhatTheFitPublishes
1302 tst_fusion_kernel imuRateEndsAreTheFit
1302 tst_fusion_kernel accuraciesFiniteAndPositive
1302 tst_fusion_kernel wideningIsOneAtTheModel
1303 tst_fusion_kernel fitRepreintegratesAtTheFittedScale
1303 tst_fusion_kernel gnssAccuracyScalingNeverLowersThem
1303 tst_fusion_kernel longHoleConverges
1304 tst_fusion_kernel sparsePiecesAreMerged
1304 tst_fusion_kernel segmentsAreCutOnFixes
1305 tst_fusion_kernel gnssHolesInTheAudit
1305 tst_fusion_golden rejectionFixturesMatchGolden
1305 audit gnss-holes
1305 audit sample-continuity
1306 tst_fusion_session registrationShape
1306 tst_fusion_session restoredFitIsIndistinguishable
1306 tst_fusion_store codeStampChangeDropsRecordOnLoad
1306 tst_fusion_store restoredSolverFailureShowsBadge
1306 tst_fusion_jobs columnOnFusionOutputIsCachedFromRecord
1306 tst_fusion_jobs workerRefillsColumnFromStoredFit
1306 tst_fusion_kernel biasSettledByCostTest
1306 tst_fusion_kernel failureDiagnosticsShape
1306 audit stored-results
1306 audit gnss-holes
1307 tst_fusion_golden fixturesAreDeterministic
1307 tst_fusion_golden successFixturesMatchGolden
1307 tst_fusion_golden channelsWriterIsTheInverseOfTheLoader
1307 tst_fusion_kernel fitTraceMatchesGolden
1307 tst_fusion_kernel bridgedHoleFollowsTheTruth
1307 audit gnss-holes
1308 tst_fusion_kernel longHoleConverges
1308 tst_fusion_kernel holeAboveTheCapIsRejected
1308 tst_fusion_kernel initializerFixturesAreDeterministic
1309 tst_fusion_kernel gnssHolesInTheAudit
1309 tst_fusion_golden successFixturesMatchGolden
1309 audit gnss-holes
1310 audit gnss-holes
1310 manual M55
1311 tst_fusion_golden successFixturesMatchGolden
1311 audit gnss-holes
1312 audit gnss-holes

# 1313 - (2, 3) the slow tail is accepted only when the position, velocity and IMU normalized RMS are all below the bound; 13-35-10 ends iteration limit
1313 tst_fusion_kernel slowTailAtTheIterationLimit
1313 audit gnss-holes
```

Item 244 is restated "(as amended)" in the map and in appendix C's row:
the slow tail's three bounds. The 1301 line `longHoleConverges` keeps its
place and `holeAboveTheCapIsRejected` joins 1301 as well.

No existing item is restated.

**`tests/README.md`.**

- Section 1: the rows of `tst_fusion_golden` (four successes, `bridged_hole`,
  ten rejections) and `tst_fusion_kernel` (the four new functions, the lists
  that gained `bridged_hole`, the long-hole recording); the executable
  counts are unchanged (54, 55, 62).
- 9.14, the matrix of 1301-1313, in 9.13's form; section 10: a bullet for
  `gnss-holes` in prose, and the traceability bullet's ranges gain
  1301-1313.
- Section 11: `bridged_hole` moves from the rejection table to the success
  table with what it exercises (a 2.6 s hole bridged, `gnss_holes` one
  entry); the recordings table gains the long-hole recording with its
  function and measured numbers; "the three successes" / "the eleven
  rejections" / "`<fixture>.channels.txt` (the three successes)" become four
  and ten wherever they state the present; a capture paragraph "The capture
  of 2026-10-0n (GNSS holes bridged)" stating what changed and what did not
  as section 6 above lists; the re-capture procedure: four `succeeded`, ten
  `reject_*`, "wrote 19 files", the `_time` loop over the four from now on
  (for this capture the three that have a HEAD file), "changes all fourteen
  `.json` files; the ten `reject_*.json`", the string v8 with v7 as history.
  Past capture paragraphs are history and stay.
- Section 12: "Eleven scripts"; a new 12.12 "GNSS holes bridged by the IMU"
  with **M55** in M11-M14's form, using 12.2's preamble: for each of
  `TEMP/data/Data comp 3 - FS 2 - serie nr 2 - 00769 (pers.)/24-09-07/08-35-48`
  and `TEMP/data/Data comp 2 - FS 2 - serie nr 2 - 01086 (pers.)/24-09-04/13-35-10`,
  `"$R" --csv TEMP/runs/m55-<name>.csv "<path>" > TEMP/runs/m55-<name>.json
  2> TEMP/runs/m55-<name>.log; echo $?`, then a Python one-liner in M50's
  form that prints the outcome (`failure` or none), `stopping.rule`,
  `stopping.passes`, `seeds[0].iterations`, `objective`, `quality`,
  `input.gnss_holes` (count and the longest), and from the CSV the largest
  `headingAcc` and `tiltAcc` among the rows whose `_time - input.epoch_utc_s`
  lies inside the longest hole against the medians over all rows. Expected
  from the specification's section 1: one hole of about 15.6 s on
  `08-35-48`; seven on `13-35-10`, the longest about 13.2 s. The numbers go
  in the report and in section 8 as they are.
- Appendix N lists the thirteen items in appendix M's form.

**Documentation.**

- `docs/SENSOR_FUSION.md`: section 2 says what the user sees over a GNSS
  hole (the fusion plots draw through it, the accuracies grow through the
  hole and collapse at the next fix, the GNSS plots break there); section 5's
  cut sentence states the sparse-piece merge; section 6 drops the GNSS gap
  bullet and states that a hole in the fixes is bridged by the IMU with its
  accuracy growing through it; section 7 adds `gnss_holes` to the input
  audit's keys and, in "Stored results", v8 to the history, naming both the
  bridged holes and the continuity rule's 1.5 threshold that the kernel
  adopted under the compatibility marker's bump to 3; section 8's test table
  counts four fits and ten rejections, its sentences on "the three fits"
  follow the lists they describe, and it gains the two recordings' rows from
  M55 (fixes, IMU samples, outcome and rule, iterations and passes,
  objective, the three normalized RMS, holes and the longest, largest
  heading and tilt accuracy inside the longest hole against the medians).
- `docs/COMPUTED_PLOTS.md` section 10: the fusion-plots paragraph gains the
  sentence that the fusion plots draw through a GNSS hole, with their
  accuracies growing through it.
- `docs/DATA_SCHEMA.md`: the `"records"` example and section 12 name v8.
- `docs/CALCULATIONS.md` section 17: the history sentence names v8; section
  9 needs nothing.

## Decisions

- **D1. `gnss_holes` is computed in `successDiagnostics()`.** The overview
  left the site open. `assembleSuccess()` is the seam the kernel tests
  publish through with a fresh `prepareInput()` audit (finding 1), and the
  window reaches `successDiagnostics()` already; computing it there puts it
  in every success's diagnostics by construction, once, through the
  authority. The alternative, `planFit()` writing `plan.prepared.audit`,
  would leave the tests' seam without it.
- **D2. Item numbering**, thirteen items (1313 added by the amendment): 1301 the one disconnection (§2 first
  bullet, §3 first bullet); 1302 everything published and the fit unchanged
  in kind (§2 second, §3 third); 1303 the fixes around a hole are ordinary
  (§2 third); 1304 the cutter and the sparse-piece test (§3 second, §4
  third); 1305 the input audit (§3 fourth); 1306 the algorithm string (§2
  fourth, §3 fifth); 1307 the bridged fixture (§4 first); 1308 the long hole
  (§4 second); 1309 the audit test (§4 fourth); 1310 the reference recordings
  (§4 fifth); 1311 the map and the suite (§4 sixth); 1312 the documentation
  (§5). Every bullet of sections 2 to 5 is under exactly one item.
- **D3. The truth comparison uses the accuracy's own direction.** `accHAcc`
  is the sigma along the published horizontal acceleration's direction, so
  the test compares that component of the error, and the down component
  against `accDAcc`; heading and tilt errors against `headingAcc` and
  `tiltAcc`. "Exceed those at the fixes on either side" is read as the
  largest inside against the nearest published sample outside on each side.
- **D4. The long-hole recording's motion** is the implementer's within the
  overview's bounds and one requirement: a manoeuvre on both sides of the
  hole, so that yaw is observable in each piece (section 6). The suggestion
  above (a periodic zero-mean horizontal acceleration throughout) meets it
  and is not otherwise binding. Michael has confirmed the kernel-test
  recording and this requirement (overview decision 4).
- **D5. The sparse-piece initializer case rides on the long-hole recording**
  under a test `segmentLength`, as `driftingBiasSegmentsConverge` uses 60 /
  12: no further recording is needed for the specification's "is fitted".

Status: implemented (d40ebcc, 5f6857b, 8dc3768) and amended a second time
after the review of that implementation: the never-below clause struck, the
cap per interval with the reason to two decimals, the hole walk in
`fusionsamples.cpp` with the include count at 10, and the measurement's
wording; those follow-ups were made directly. Items are 1301-1313; matrix
9.14 and appendix N list thirteen.
