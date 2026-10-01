# Phase 2: Publishing the reconstruction

## Purpose

Spec §6-§8, the rest of §9 and §10, and all of §11's `docs/` part: the fit
publishes phase 1's pass instead of the linear reconstruction, which is
removed; the diagnostics give the account of §7; the algorithm string
becomes `batch-temperature-bias-v4`; the goldens are captured again; and the
documents describe what is now published. The numbers change, the string
changes and the goldens are captured again in one phase because any one of
them without the others leaves a golden test red (decision 1).

## Dependencies

- Depends on phase 1; blocks phase 3 (traceability).
- At the start, as phase 1 leaves it: `ImuRateTrajectory` and
  `reconstructAtImuRate(window, fit, tuning)` exist in
  `src/fusion/trajectoryreconstruction.h` beside `DenseTrajectory` and
  `reconstructTrajectory()`, and nothing in `src` calls them;
  `preintegrateImu()` has its per-step observer; `tst_fusion_kernel` has
  phase 1's reconstruction tests and helpers (the once-per-run fixture fit,
  the held-ends reference graph); `fitAndAssemble()` in `fusion.cpp` still
  calls `reconstructTrajectory(plan.window, fit)`; `Fusion::Algorithm` is
  `batch-temperature-bias-v3`; `tests/data/fusion/` is unchanged since
  2026-09-22.
- Phase 1's report gives the test function names, the equivalence agreement
  on `coarse_maneuver` and the rotating recording, and per success fixture the
  worst consistency ratio and whole-axis error. If the orchestrator does not
  pass it on, run `tst_fusion_kernel` and read the numbers its tests print
  with `qInfo()`.

## What changes

### The kernel (`src/fusion/`)

- `fitAndAssemble()` (`fusion.cpp`): after convergence it calls
  `reconstructAtImuRate(plan.window, fit, plan.tuning)` once, where
  `reconstructTrajectory(plan.window, fit)` is called today, and hands the
  result to `fillOutputChannels()` and `successDiagnostics()`. No checkpoint
  is added (decision 9; the audit's count of three boundary kinds stays).
- `fillOutputChannels()` and `successDiagnostics()` (`fusionoutput.h`, `.cpp`)
  take `const ImuRateTrajectory &` in place of `const DenseTrajectory &`. The
  channel assembly (epoch added, rpy in degrees unwrapped, quaternion xyzw) is
  unchanged. Their comments say what they now read.
- `successDiagnostics()` keys (decision 10): `imu_outputs` is
  `trajectory.time.size()`; `max_endpoint_correction_deg` is
  `maxEndpointCorrectionDeg`; new `max_step_correction_m_s2`
  (`maxStepCorrection`), `max_step_correction_time_s`
  (`maxStepCorrectionTime`), `max_velocity_mismatch_m_s`
  (`maxVelocityMismatch`); `display_position_velocity` is removed and
  `dense_output` added; `limitations` loses its second sentence. Every other
  key, and `failureDiagnostics()`, is unchanged. The two texts are the
  implementer's within Decision 5; the documentation quotes them verbatim.
- `trajectoryreconstruction.h`, `.cpp`: `DenseTrajectory`,
  `reconstructTrajectory()` and `propagateThroughInterval()` are deleted, with
  the includes only they used. The file comment and the contract comments of
  `ImuRateTrajectory` / `reconstructAtImuRate()` stand alone (no "beside the
  present reconstruction"). The file stays, so `src/CMakeLists.txt` and the
  `fusion-tooling` header pattern do not change.
- `fusion.h`: `Algorithm` becomes `"batch-temperature-bias-v4"` (its comment
  unchanged). The `Result` comment says what position, velocity and
  acceleration are now: the fitted state at every IMU sample, the fitted
  states at the fixes joined by the IMU (spec §5, `docs/SENSOR_FUSION.md`
  section 4); acceleration the bias-corrected reading rotated by that
  attitude, plus gravity, plus the model's share of the correction, so that
  integrated it reproduces the velocity. It no longer says "interpolated for
  display".
- `fusionregistration.cpp`: nothing changes (`resultVersion` is
  `Fusion::Algorithm`).

### Tests (existing functions amended; new ones under Tests)

- `tst_fusion_kernel`: the three tests that call `reconstructTrajectory()`
  are rewritten against `reconstructAtImuRate()` (with the `Tuning` their
  samples were validated or fitted with), keeping what still holds:
  - `reconstructionTimingAndEndpointCorrection` (name free, it has no map
    line): 83 samples from .04 to .86; `maxEndpointCorrectionDeg` is the .2
    rad yaw in degrees; the attitude is `Rz(.2 (t − .037)/.826)` at every
    sample (still true: zero gyro, a vertical force and uniform density-only
    step noise make the yaw share proportional to elapsed time, and yaw about
    the force axis does not couple into velocity); acceleration zero;
    `maxVelocityMismatch` and `maxStepCorrection` zero. Tolerances stated
    from the measurement.
  - `exactConstantVelocityFit` (name kept: map items 213, 226): the same
    fit assertions; position, velocity and acceleration against the exact
    trajectory within 1e-8 at every published sample; the `fittedWindow`
    throw.
  - `reconstructionUsesIntervalBias` (name kept: map item 219): the same two
    cases on `maxEndpointCorrectionDeg` (< 1e-9 degrees under the
    temperature model, > .5 under the constant one), printed with `qInfo()`.
- `tst_fusion_kernel::initializerDiagnosticsShape`: the key list of Decision
  10 (sorted as `QJsonObject` sorts). The algorithm literals in
  `biasSettledByCostTest`, `failureDiagnosticsShape` and
  `constantTemperatureKeepsSlopeAtPrior` become v4.
- `tst_fusion_golden::comparatorHoldsItsBounds`: the rule of Decision 3.
- `tst_fusion_store`: `kSolverFailureDiagnostics` says v4; the
  `resultVersion` row of `codeStampChangeDropsRecordOnLoad` stamps
  `batch-temperature-bias-v3` (Decision 2). `restoredAfterRestartIsBitIdentical`
  needs no change: against the new goldens it is the proof that a fit stored
  after the change restores bit for bit.
- `tst_fusion_jobs` (two stamps), `tst_fusion_session::registrationShape`
  and the snapshot check near it, `tst_result_records` (four literals): v4.
- `tst_fusion_jobs::columnShowsValueStraightAfterPublication`: the roll
  literal `0.27409834397858546` is read again from the new
  `coarse_maneuver` golden; the displayed `"0.3"` is kept if the new value
  still rounds to it, else set from the golden.
- `tst_fusion_derived::deviceFrameMountGivesTheFitsOwnAngles` reads the new
  golden within 1e-6 and needs no change; nor do the golden readers of
  `tst_fusion_session`, `tst_fusion_rows` and `tst_fusion_runner`.
- `tests/fusion/fusiongolden.cpp`: `compareJson()` gains Decision 3's rule;
  the `isExactKey()` set and `portableFloor()` are otherwise unchanged.

### The goldens (`tests/data/fusion/`)

Captured again with `fusion_golden_capture` on `build-agent/`, which links
the installs under `third-party/*-install` (decision 14), following
`tests/README.md` section 11 as this phase rewrites it: build Release;
capture into `tests/data/fusion/` with `--revision "$(git rev-parse HEAD)"`
and `PATH` holding Qt's bin, `third-party/GTSAM-install/bin` and
`third-party/oneTBB-install/bin`; reconfigure `build-agent/FlySightViewer-build`
so the exact-test gate reads the new `capture.json`; run the suite. Then, for
each success fixture, compare the first field of every data line of the new
`<fixture>.channels.txt` with `git show HEAD:tests/data/fusion/<fixture>.channels.txt`
(decision 12): line counts and every `_time` field byte-identical. The
result goes into the report and section 11. A difference is a defect of
this change, not a finding to record: stop and report it.

The fit itself is unchanged (spec §2), which the capture must show: in each
success `.json`, everything but `diagnostics.algorithm`,
`diagnostics.dense_output` (in place of `display_position_velocity`),
`diagnostics.limitations`, `diagnostics.max_endpoint_correction_deg` and the
three new keys is identical to HEAD's (`trace`, `progress`, `rows`,
`objective`, `residuals`, `seeds`, `stopping`, `quality`, `model`,
`initializer`, `input`, `start_s`, `end_s`, `imu_outputs`); each
`reject_*.json` differs only in `algorithm`.

### The audit (`tests/audit/cleanup_audit.cmake`)

- `stored-results`: both "one authority: the fusion algorithm string" rules
  name `batch-temperature-bias-v4`.
- `fusion-model`: the comment "The goldens say batch-temperature-bias-v3"
  names v4. The retired-strings rule is unchanged (Decision 7).
- New group `fusion-reconstruction`, in its own banner block in the file's
  style after the fusion-plots groups and before "leftover markers", plus a
  bullet in the file-head summary; acceptance item numbers are left to phase
  3. Two rules, each with an `Allow:` comment saying why:
  - "the linear reconstruction is gone": `expect_none` of
    `DenseTrajectory|reconstructTrajectory|endpointCorrection|propagateThroughInterval|display_position_velocity`
    over `src tests docs README.md ":!tests/README.md"` (the goldens under
    `tests/data/fusion` are searched, so a stale capture fails it).
  - "no text says the fused output is interpolated": `expect_none` of
    `interpolated linearly onto|states interpolated|interpolation of (the )?optimi[sz]ed GNSS states|interpolated for display|[Dd]isplay-only (linear )?interpolation|not (an|a full) IMU-rate smoothing posterior|distributed correction`
    over the same paths. It must not match "linearly interpolated midpoint
    inputs" (`SENSOR_FUSION.md` section 4, the integration, which stays) nor
    the unrelated interpolation comments elsewhere in `src`; both patterns
    today match only the lines this phase removes or rewrites (checked with
    `git grep -nE` when this document was written).
- `tests/README.md` is excluded because phase 3 describes the group in
  section 10; this phase does not, and its report says so.

### Documentation

- `docs/SENSOR_FUSION.md` section 4: the "Dense reconstruction" paragraph
  replaced by the reconstruction (spec §5, §6): forward integration from the
  fitted state with the interval's bias through the fit's own
  preintegration; the mismatch at the next fix; its sharing as the
  conditional mean of the step chain given both ends (a step carrying more
  noise takes more; an attitude share turns every later reading); one pass
  about the forward states, the ends exact; the output is, to within one
  linearization, what a fit with a state at every IMU sample would give (what
  it leaves out: section 8). Acceleration as spec §6 gives it, with the step
  correction of decision 6 in words, the consequence §6 requires stated
  (whole interval, any run to within the spread of the corrections, not step
  by step; the rotated reading in steady flight), and that it is not a
  derivative of the GNSS velocity. The outputs table's position, velocity and
  acceleration rows; the `_time` row or the paragraph under it says, in one
  sentence, that the axis is the IMU samples whatever the GNSS rate,
  including a GNSS rate above the IMU's. The key list: `algorithm`
  (`batch-temperature-bias-v4`); the three new keys with units beside
  `max_endpoint_correction_deg`, each defined; `dense_output` and
  `limitations` quoted verbatim.
- Section 7: the stored-results paragraph names v4 and says that the first
  start after the update finds every stored fit stale at its recording's
  load, each fitted again once when something switched on needs it, counted
  in the status bar like any fit (Decision 6). The cancellation paragraph
  says the reconstruction after the last iteration is not a boundary and runs
  to its end (a fraction of a second), and "at most one solver step" becomes
  "at most one solver step and the reconstruction" (Decision 8).
- Section 8: the `tst_fusion_kernel` row names the reconstruction, the
  publication and the time-axis tests. A paragraph on validating the
  reconstruction: the equivalence test and its reference (a state at every
  edge, one-step factors with the same per-step covariance and interval
  bias, the fix states and biases held), the agreement phase 1 measured on
  both recordings, absolute and relative to the mismatch; why the reference
  holds the ends (freed, the fix states drift along the unobservable yaw:
  0.9 × the mismatch on `coarse_maneuver`, 0.18 degrees on `stationary_spin`,
  5.7 degrees on `rest_throughout`, FINDINGS Q3); what the one pass leaves
  out (one linearization, whose error in the split is quadratic in the
  mismatch, about 2.2e-5 |d_v|^2 m/s and 5.4e-4 |d_att|^2 degrees; the fix
  states and biases are the fit's and are not solved again; chained one-step
  and multi-step preintegration differ, 1.1e-6 degrees on `stationary_spin`;
  no uncertainty is published); the consistency bound (decision 8's formula)
  and phase 1's measured ratios and whole-axis errors; and that under fast
  rotation the corrections carry the integration's own discretization error
  (the reading is rotated by the attitude at the start of each step, off by
  about ½|ω × f|Δt, which exact readings with true ends turn into a mismatch
  the pass shares out): the table phase 1 measured at 13, 25 and 100 Hz
  (FINDINGS Q4 gives 0.017-0.133 m/s² against truth at 25 and 100 Hz, where
  the rotated reading alone is 1e-4-1.2e-3), and the remedy in one
  sentence: rotating the reading by the mid-step attitude, a change to the
  fit's integration that a later specification makes. Words to avoid: the
  `fusion-model` rule forbids "frozen", "stationary window", "candidate
  window" and others in this file; say "held".
- `docs/CALCULATIONS.md` section 17: "Outputs of the fit" says in one
  sentence what the measurements are (the fitted state and the model's
  acceleration at every IMU sample, `SENSOR_FUSION.md` section 4); "Stored
  results" says the string is `batch-temperature-bias-v4` since the IMU-rate
  reconstruction and the first-start consequence. The sentence counted by
  "the bump rule names the result version (docs)" is not repeated.
- `docs/COMPUTED_PLOTS.md` section 1: one paragraph after the one naming the
  category: between GNSS fixes the fusion plots follow the IMU at its own
  rate, pulled onto the fitted fixes, so a line no longer runs straight
  between fixes or bends where samples straddle one; the first start after
  this update computes the kept sensor fusion results again once, in the
  status bar like any computation.
- `docs/DATA_SCHEMA.md` section 11: the `records` example says v4. Section
  12: one sentence that the sensor fusion record holds the same seventeen
  measurements and diagnostics in the same format (format version 2
  unchanged), and that records written before the change are stale by their
  result version.
- `tests/README.md` section 11: the history sentence adds this capture; a
  short record of it (date, why: the published channels are the IMU-rate
  reconstruction and the algorithm string changed; what changed and what did
  not, as under "The goldens"; the `_time` comparison's result per fixture);
  the procedure names the tree `<tree>` (a tree configured as section 2
  describes, as `CLAUDE.md` does) and the installs `third-party/GTSAM-install`
  and `third-party/oneTBB-install`, in the bash and the PowerShell form, and
  adds the `_time` check as a step for any capture that must not move the
  axis; "The `algorithm` string is `batch-temperature-bias-v4` since the
  IMU-rate reconstruction". Section 10's `fusion-model` item says the goldens
  say v4. Section 1 rows: `tst_fusion_kernel` (the rewritten three, the
  publication and time-axis tests, the new diagnostics keys), `tst_fusion_golden`
  (the comparator's rule for the time of a maximum), `tst_fusion_store` (the
  stale stamp is the previous algorithm string). Rows of executables whose
  only change is a literal stay.

### What must not change

The fit (graph, noise, initializer, solver, stopping; every number of the
trace), `preintegrateImu()` and phase 1's pass (a defect found in it is fixed
and reported, not worked around), the seventeen channels and their time axis,
the registration, the record format, the failure diagnostics,
`tests/acceptance_map.txt`, `tests/README.md` sections 9, 10 (but the one
string) and the appendices, anything under `PLANS/`.

## Interfaces

Consumed from phase 1: `ImuRateTrajectory` and `reconstructAtImuRate()`
exactly as the overview states them; phase 1's test helpers and reported
numbers.

Provided to phase 3 (named in the report):

- `fitAndAssemble()` publishes `reconstructAtImuRate()`; `DenseTrajectory`,
  `reconstructTrajectory()` and `endpointCorrection` exist nowhere in `src`
  or `tests`.
- The success diagnostics' keys of decision 10 and
  `Fusion::Algorithm == "batch-temperature-bias-v4"`.
- Audit group `fusion-reconstruction` (two rules, no item numbers) and the
  amended rules in `stored-results` and the `fusion-model` comment; section
  10's description of the new group is phase 3's.
- The names of every test function added or rewritten, with the spec
  bullet each covers; the files the re-capture changed; the `_time`
  comparison's result; the runner-up margin of Decision 3.

## Acceptance criteria

1. `git grep -nE "DenseTrajectory|reconstructTrajectory|endpointCorrection|propagateThroughInterval" -- src tests`
   is empty; `fitAndAssemble()` calls `reconstructAtImuRate()` once;
   `fillOutputChannels()` and `successDiagnostics()` take
   `const ImuRateTrajectory &`; `checkpoint(` still counts 3. (§9, decisions
   1, 9.)
2. On every success fixture, the seventeen published channels of
   `runPipeline()` equal, bit for bit, those computed from
   `reconstructAtImuRate()` on phase 1's fixture fit (time plus epoch; rpy
   and quaternion by `fillOutputChannels()`'s rule), and the four numbers in
   the diagnostics equal its four summaries. (§6, §7, §9.)
3. The success diagnostics' keys are exactly today's with
   `display_position_velocity` replaced by `dense_output` and
   `max_step_correction_m_s2`, `max_step_correction_time_s`,
   `max_velocity_mismatch_m_s` added; `limitations` does not contain
   "smoothing posterior"; rejected and failed fits' diagnostics differ from
   today's only in `algorithm`. (§7, decision 10.)
4. `Fusion::Algorithm` is `batch-temperature-bias-v4`; in `src`, `tests`
   (outside `tests/README.md` and `tests/data/fusion/`) and `docs`,
   `batch-temperature-bias-v3` appears only in the stale-record row of
   `codeStampChangeDropsRecordOnLoad`. (§8, decision 11.)
5. `capture.json` records compiler 19.44.35220.0, `solver.gtsam_dir` under
   `third-party/GTSAM-install`, the HEAD revision at capture, and the tool
   exited 0; the golden files differ from HEAD exactly as "The goldens"
   states. (§2, §8, decision 14.)
6. Each success fixture's `_time` column is byte-identical to HEAD's (160,
   540 and 995 lines), recorded in the report and `tests/README.md` section
   11. (§8, decision 12.)
7. The time-axis test holds on the three success fixtures and on the
   synthetic GNSS-faster-than-IMU recording (Tests). (§6, §10, decision 12.)
8. A record stamped `batch-temperature-bias-v3` is deleted as stale at its
   session's load (`codeStampChangeDropsRecordOnLoad`, `resultVersion` row),
   and `restoredAfterRestartIsBitIdentical` passes against the new golden.
   (§8, §10.)
9. The three rewritten tests assert what "Tests (existing functions
   amended)" lists; `exactConstantVelocityFit` and
   `reconstructionUsesIntervalBias` keep their names.
10. `comparatorHoldsItsBounds` asserts both branches of Decision 3, in both
    modes. (§10 "the golden comparison covers it".)
11. The audit has group `fusion-reconstruction` with the two rules above;
    `stored-results` names v4. The whole suite passes on `build-agent/`,
    run sequentially, the `*_exact` tests and `audit_cleanup` included.
12. The documents contain what "Documentation" lists; in particular
    `SENSOR_FUSION.md` quotes `dense_output` and `limitations` exactly as
    `successDiagnostics()` writes them, and states the GNSS-rate sentence,
    the first-start consequence and the fast-rotation limitation. (§6, §7,
    §8, §11.)

## Tests

In `tst_fusion_kernel` (decision 13), names the implementer's:

- **Publication and time axis, per success fixture** (criteria 2, 7),
  data-driven over the fixtures `fitTraceMatchesGolden_data` enumerates, one
  `runPipeline()` per fixture (the fits dominate the executable's time;
  reuse phase 1's cached fixture fit for the comparison). Expected `_time`:
  `input.epoch_utc_s` of the diagnostics plus each epoch-relative IMU time of
  `prepareInput()` of the fixture's channels in `[start_s, end_s)`, compared
  with `==` element by element, and in number `imu_outputs`.
- **Time axis, GNSS faster than IMU** (criterion 7): Channels built in the
  test as the probe's `gnssFasterThanImu()` builds them (100 s, GNSS 10 Hz
  from .05 s, IMU 5 Hz, a level turning acceleration, exact data, 25 degC,
  origin 0; about 2.6 s to fit), through `runPipeline()`: Succeeded, the
  expected axis as above (499 samples, .2 to 99.8 s after the epoch),
  every channel that length and finite, the four diagnostics numbers finite.
  It may be shortened if it still has intervals without an IMU sample.
- The amended functions listed under "What changes".

In `tst_fusion_golden::comparatorHoldsItsBounds`: in portable mode a
`max_step_correction_time_s` one step off passes when the golden's
`max_step_correction_m_s2` is at or below the floor and fails when it is
above; in exact mode it fails either way.

Run: `cmake --build build-agent --config Release`, the capture, the
reconfigure, then `ctest --test-dir build-agent/FlySightViewer-build -C Release --output-on-failure`,
sequentially, after checking for stray `ctest` / `tst_*` processes. No
acceptance-map line (phase 3).

## Decisions

1. **Test names in the map are kept.** `exactConstantVelocityFit` and
   `reconstructionUsesIntervalBias` are named by map items 213, 219 and 226,
   which the audit checks and phase 3 owns; renaming them would break the
   audit in this phase. The third rewritten test has no map line.
2. **The stale stamp is the retired string.** The `resultVersion` row
   stamps `batch-temperature-bias-v3`, the string this change leaves in
   users' logbooks, instead of v2: the test is then the real transition.
3. **The time of a maximum in portable mode.** `max_step_correction_time_s`
   is an argmax: on `coarse_linear` the step corrections are rounding, and
   another compiler picks another step. Portable mode compares it exactly
   when the golden's `max_step_correction_m_s2` in the same object exceeds
   the portable floor, and not at all otherwise; exact mode always compares
   it bit for bit. The midpoint of two copied times is the same bits on
   every IEEE platform, so when the argmax is determined the value is exact.
   The report gives, for `coarse_maneuver` and `stationary_spin`, the gap
   between the largest step correction and the largest at another step; a
   gap under 1e-6 m/s² is a caveat for CI, which cannot be run here.
4. **The publication test goes through phase 1's fixture fit** because only
   a bit-for-bit comparison shows that what `run()` publishes is the pass,
   not something like it. If the helper's fit is not bit-identical to the
   pipeline's, the helper is fixed.
5. **The two texts.** `dense_output` names the reconstruction, for example
   "IMU-rate reconstruction: between fixes the IMU integrated from the fitted
   state, the mismatch with the next fitted state shared over the steps by
   their noise, in one linearized pass". `limitations` keeps "Local batch
   convergence; heading may be ambiguous." and may add one sentence on what
   the one pass leaves out. Neither may match the new audit patterns.
6. **"The first start recomputes every stored fit"** (§8) is written in
   terms of the existing rules, which it relies on: a stale record is deleted
   at its recording's load and the fit runs again when something switched on
   needs it, which after a start is every recording a fusion column covers
   and every visible one a checked fusion plot covers.
7. **The retired-strings rule is not extended to v3**: the stale-record test
   needs the literal, and a stale capture is already caught by the golden
   tests' string comparison and by the new group's first rule.
8. **Section 7's cancellation text changes** although §11 asks for it only
   if the boundaries mention the reconstruction: "at most one solver step"
   would otherwise be inexact by the reconstruction's run time.
9. **The `_time` comparison is against `git show HEAD:`**, since phase 1
   left the goldens untouched; no copy is kept.
10. Caveat, not a question: the portable bound has not seen the new
    accelerations on other compilers. If CI exceeds it, the remedy is section
    11's (a wider bound with the numbers recorded), never the goldens.
