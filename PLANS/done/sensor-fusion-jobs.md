# Sensor fusion as an explicit calculation, with plot-driven background jobs

Date: 2026-09-20
Status: specification for planning. Not an implementation plan.
Baseline: `master` at or after merge commit `13c6115` (registered-calculation
engine merged). Work happens on a new branch; see "Commit Policy".
Behavioral reference: branch `sensor-fusion-clean-port` (based on
`v2026.04.1`, old calculated-value mechanism). It is a reference for what the
numbers must be, not a source of code to copy; see section 4.

This document describes what the change must do and the architectural
boundaries it must respect. It deliberately avoids class layouts, container
choices, and function signatures unless a name is part of the observable
contract. The planning agent decides phasing; the implementation agent decides
code structure within these boundaries.

## 1. Motivation

`sensor-fusion-clean-port` contains a batch GNSS/IMU fusion (GTSAM factor
graph) that works well numerically. It cannot be merged as it stands:

- It is built on the old per-value calculation mechanism that `master` has
  replaced. Most of its integration code is scaffolding for that mechanism:
  seventeen per-output registrations that write siblings into the cache as
  side effects, a thread-local re-entrancy guard, hand-cached failures, and
  dependencies attached after the fact.
- A fit takes from seconds to several minutes. The branch runs it the first
  time any fusion channel is read, inside the getter, behind an
  application-modal progress dialog with a nested event loop. That blocks the
  application, required guards in the plot widget and the idle scheduler to
  survive re-entrancy, and means any reader (a logbook column, the legend)
  can start minutes of work.
- The code is hard to maintain: large uncommented blocks with little
  decomposition.

The new engine already has the concept this needs: a calculation with
evaluation policy *explicit* runs only when requested (engine spec section
7.7, `PLANS/schema-and-calculations.md`). This change uses it. Fusion becomes
one explicit multi-output calculation; the work runs on a background thread;
and the user asks for it from the place they already are, the plot list.

The guiding observation for the interface: users think in plots, not
calculations. "I want to see fusion roll for these tracks" is the request.
Which calculation produces it is an implementation detail the user should
never have to know.

## 2. Scope

In scope:

- Building GTSAM and oneTBB as pinned third-party dependencies on all three
  release platforms, with runtime deployment.
- Shared recording-wide local GNSS coordinates (`Local/...`) as ordinary
  on-demand calculations, with their plots.
- Track simplification for the map layered on that shared frame instead of
  its own private projection.
- The shared time-fit precision fix (centered least squares).
- The fusion algorithm, numerically unchanged, as one explicit multi-output
  registered calculation, rewritten in `master`'s code style.
- Engine support for running an explicit calculation asynchronously:
  prepare on the main thread, compute on a worker, publish on the main
  thread, with staleness detection, cancellation, and progress.
- Engine support for asking, without starting explicit work, which explicit
  calculations stand between a name and its availability.
- An application-wide job queue for explicit calculations, exposed as a
  widget-free model that is complete enough to back a future dock.
- Plot-list rows that show what needs computing, start it, show progress,
  cancel it, and report failures.
- Automated tests for the behavior described here, and documentation.

Out of scope:

- A jobs dock or any other new window. The job model must make one a pure
  view, but none is built here.
- Persisting fusion results across application restarts.
- Any change to the fusion model, initializer, tuning constants, output
  values, or units (output names do change; see section 6). No RAW.UBX input, no heading search, no new initialization scheme.
- Explicit policy or background execution for Python plugin calculations.
- More than one job running at a time.
- The modal progress dialog, the `sensorFusionIsRunning` guard, the idle
  scheduler pause, and the plot-widget rebuild guard from the branch. None of
  them has a reason to exist once nothing runs inside a getter.

## 3. Third-party dependencies

GTSAM (pinned to the version the branch uses, 4.3a0, with its bundled Eigen)
and oneTBB join the third-party superbuild next to GeographicLib and
KDDockWidgets. The branch's CMake (`cmake/SolverSuperbuild.cmake`,
`cmake/SolverDependencies.cmake`, and its edits to the root and `src` CMake
files and the CI workflow) is the working reference for what is needed:

- The application links GTSAM through its exported CMake target so ABI
  definitions and matching Eigen headers reach every translation unit that
  uses GTSAM types.
- Runtime libraries (GTSAM, its METIS and Cephes, TBB and its allocator, and
  shared Boost where applicable) are deployed with the application on
  Windows, macOS, and Linux, derived from the exported targets rather than
  from file globs.
- CI caches the new install directories with the existing third-party cache.
- Large GTSAM elimination trees overflow default thread stacks. Any thread
  that runs a fit has a 64 MiB stack; test executables that run fits on
  their main thread are linked accordingly.

Only the code that needs GTSAM links it. The engine, session model, and job
queue do not depend on GTSAM, and their tests build without it.

## 4. Porting rules

The algorithm is frozen; the code is not.

- **Numerical behavior is preserved exactly.** For identical inputs the port
  must produce the same objective, biases, residuals, output timestamps, and
  output channels as the branch. Restructuring code into functions must not
  reorder floating-point operations inside the numerical kernels. Any
  difference beyond last-bit compiler noise is a defect to be explained, not
  tolerated.
- **Parity is demonstrated, not asserted.** Golden outputs are captured from
  the branch's implementation for a set of self-contained synthetic fixtures
  (the branch's `fusion_regression` self-tests and synthetic solves are the
  starting point) and committed as test data. The ported code is tested
  against them. Real recordings are not in the repository; the comparison
  against a real recording documented in the branch's
  `docs/PORT_VALIDATION.md` is an optional local check, not a CI test.
- **Code style follows `master`.** The branch is read for behavior and
  retyped, not cherry-picked or merged. Functions are small and named for
  their intent, with comments at the density of `src/engine/` and
  `src/calculations/` on `master`. This is a faithful restructuring, not a
  redesign: no new abstractions beyond what clarity needs.
- **Naming is style.** Nothing on the branch has shipped, so none of its
  names are a compatibility constraint. Public names are the ones given in
  this document (they differ from the branch where the branch's were
  historical or inconsistent). Internal names — files, namespaces, types,
  functions, calculation ids — follow `master`'s conventions and say what
  the thing is. In particular nothing is called an EKF: the algorithm is a
  batch factor-graph fit.
- **Tests follow `master`.** QtTest, `tests/tst_<area>.cpp`,
  `flysight_add_test`, the existing support library. The branch's standalone
  regression executables are a source of test ideas and fixtures only.
- The branch's documentation (`docs/SENSOR_FUSION.md`,
  `docs/LOCAL_COORDINATES.md`) describes the model, its limitations, and the
  input contract accurately and is carried over, with the "Calculation
  lifecycle" material rewritten for the behavior specified here.

## 5. Shared calculations

### 5.1 Time fit

The shared `_TIME_FIT_A` / `_TIME_FIT_B` calculation loses precision at
realistic device uptimes because its least-squares sums are not centered
(tens of milliseconds on an exact synthetic clock). The fit is computed with
centered sums. Names, inputs, outputs, and consumers are unchanged. A test
with an exact synthetic clock at high uptime bounds the conversion error at
the microsecond level.

### 5.2 Local coordinates

One on-demand calculation produces a recording-wide north/east/down frame:

- The origin is the first GNSS fix with latitude in [-90, 90], longitude in
  [-180, 180], finite hMSL, and finite horizontal accuracy `0 <= hAcc < 10 m`.
  Speed and speed accuracy play no part. No qualifying fix makes every output
  unavailable.
- Attribute outputs `_LOCAL_ORIGIN_LAT`, `_LOCAL_ORIGIN_LON`,
  `_LOCAL_ORIGIN_HMSL`, `_LOCAL_ORIGIN_INDEX` identify the frame.
- Measurement outputs `Local/north`, `east`, `down` (metres) and `Local/velN`,
  `velE`, `velD` (GNSS velocity rotated into the same frame), one entry per
  GNSS sample, NaN where the sample is invalid. `Local` shares the GNSS time
  axes.
- GeographicLib performs the transform, with CSV hMSL used as an approximate
  ellipsoid height. This approximation is documented, not corrected.
- Markers, zoom, and display preferences do not affect the frame.

Six plots in a "GNSS (Local frame)" category expose these channels. They are
ordinary plots: cheap, on demand, no job.

### 5.3 Simplified track

The map draws `Simplified/lat`, `lon`, `hMSL`, and `_time`. On `master` the
calculation behind them projects the track into a private local Cartesian
frame centred on the first sample, simplifies it, and then searches the
original samples to find which ones survived. It is re-layered on the shared
frame:

- Its horizontal input is `Local/north` and `Local/east`. It performs no
  projection of its own and has no second origin.
- The simplification is unchanged: horizontal Ramer-Douglas-Peucker with a
  0.5 m tolerance.
- It retains the indices of the surviving samples directly, so every output
  is the original sample at those indices. There is no coordinate
  re-matching step, and distinct samples at duplicate positions (endpoints
  in particular) stay distinct.
- Outputs are the four existing names, which keep their meaning, plus
  `Simplified/north`, `east`, and `down` at the same indices. All seven
  outputs always have the same length.
- Samples whose local coordinates are not finite are left out of the
  simplified track rather than poisoning it.
- When the shared frame is unavailable (no qualifying origin), the
  simplified track is unavailable. The map then shows no track and no cursor
  dot for that recording, and excludes it from its bounds; when no visible
  recording has a track the bounds are cleared. There is no fallback to an
  unsimplified or privately projected track. A recording in which no fix
  ever reaches 10 m horizontal accuracy has no track worth drawing.
- The map's models are consumers of these names only. They do not change
  except as needed to behave correctly when a track is unavailable and when
  it becomes available again after a source change.

## 6. The fusion calculation

One registered calculation with explicit policy and a human-readable title
("Sensor fusion") for use in the interface.

**Inputs** (all declared, all required): the GNSS position, velocity, and
accuracy channels; the IMU acceleration and rate channels; the shared UTC time
of both sensors; and the local-frame origin attributes. The calculation
reads effective values only, like every other calculation. The exact list is
what the branch's input adapter consumes.

**Outputs**, published together:

- Measurements under sensor `Fusion` (the branch's `_IMU_GNSS_EKF`):
  `_time`, `north`, `east`, `down` (the branch's `posN`, `posE`, `posD`,
  renamed to match `Local`), `velN`, `velE`, `velD`, `accN`, `accE`, `accD`,
  `roll`, `pitch`, `yaw`, `qx`, `qy`, `qz`, `qw`. All arrays align with
  `_time`: the original IMU samples inside the fitted interval. Roll, pitch,
  and yaw are degrees, unwrapped across the whole fit with the same rule the
  GNSS course uses. Values and units are as on the branch.
- Attribute `_FUSION_DIAGNOSTICS`: compact JSON describing input audit,
  initializer, objective, biases, residuals, and on failure the reason. Its
  content is as on the branch.

**Derived values** stay separate on-demand calculations that declare fusion
outputs as inputs: `Fusion/accH` and `Fusion/_system_time`.
They are unavailable until fusion publishes and appear when it does, through
ordinary invalidation.

**Rejections are results.** Inputs the model cannot use (non-finite data,
non-monotonic time, missing coverage, an IMU gap over 1.6 median intervals, a
GNSS gap over max(2 s, 5 median intervals), and the rest of the branch's
validation) and solver failures are outcomes of the function: the measurement
outputs are unavailable, the diagnostics attribute carries the reason, and
the result is cached like any other. Asking again with the same inputs would
give the same answer, so nothing offers to. The result is dropped, and the
calculation becomes requestable again, when a declared input changes.

**The compute function is pure** in the engine's sense: a function of its
declared inputs that touches no session, no engine, no registry, no
preference store, and no Qt GUI object. This is what allows it to run on
another thread. It additionally receives a facility to report progress text
and to observe a cancellation request. That facility is not an input: it
cannot influence the result except by abandoning it. Cancellation is observed
at the boundaries the branch uses (between graph-construction blocks and
optimizer iterations); a linear solve in progress finishes first.

Seventeen plots in a "Sensor fusion" category expose the measurement outputs
and `accH`, as registered on the branch.

## 7. Engine changes

The engine on `master` is single-threaded and offers a synchronous
`request()`. It stays single-threaded: every engine, registry, and session
access remains on the main thread. Two capabilities are added.

### 7.1 Asynchronous request

Running an explicit calculation is split into three steps:

1. **Prepare** (main thread). Resolve every declared input, computing
   on-demand inputs as needed, and capture their values. If an input is
   unavailable the calculation's status is "missing input" and there is
   nothing to run. Preparing never runs an explicit calculation; an input
   that is itself an unrequested explicit output makes this one blocked
   (section 7.2).
2. **Compute** (any thread). The compute function runs against the captured
   inputs only. Captured values must be safe to read from the worker without
   synchronization and must not be affected by later edits to the session.
3. **Publish** (main thread). The result is installed for all outputs at
   once, and the names read while the calculation was unrequested are
   invalidated so consumers re-read, exactly as the synchronous request does.

Publishing is refused if anything the prepared inputs depended on has been
invalidated since prepare, or if the session or the registration no longer
exists. A refused result is discarded whole. The engine must make this
decision from its own dependency records, not from the caller's diligence.

Cancellation and job-level failure (section 8.3) publish nothing: the
calculation remains "not requested", as if it had never been asked.

The synchronous `request()` remains for tests and for callers without an
event loop, with unchanged semantics. Both paths yield identical results for
identical inputs.

This is the application's first worker thread. The rule that makes it safe is
narrow and must stay narrow: the worker sees captured inputs and the
progress/cancel facility, and nothing else.

### 7.2 Blocker inspection

For a public name in a session, the engine can report the explicit
calculations that currently prevent it from being available, without running
any explicit calculation:

- The report sees through on-demand intermediates. `accH` is blocked by
  fusion although `accH` itself is on demand.
- A calculation whose declared inputs are unavailable is **not** a blocker.
  A track with no IMU data has nothing to compute; it reports missing input,
  never "not requested". Availability of inputs is decided before policy.
- When explicit calculation B depends on unrequested explicit calculation A,
  the blocker is A. Once A publishes, the blocker is B. A consumer that
  wants the name keeps requesting blockers until none remain.
- The report also distinguishes a name that is unavailable because an
  explicit calculation ran and did not produce it (section 6, rejections)
  from one that was never run.
- Inspection may compute cheap on-demand inputs. It never computes an
  explicit calculation, and it never changes what a later read returns.

Engine spec section 7.4 (idempotency) already scopes explicit calculations:
an explicit output is a function of state once requested and unavailable
before. Nothing here weakens that. In particular a published result must
never survive a change to its inputs.

## 8. Job queue

One application-wide queue owns all background calculation work. It lives
with the session model and the engine in the widget-free core, is driven from
the main thread, and is testable without a GUI.

### 8.1 Jobs

A job is one explicit calculation for one session. Requests are
deduplicated: asking for a job that is queued or running is a no-op. Jobs run
one at a time, first come first served. A job for a session that is not
loaded cannot start; the queue does not load sessions.

### 8.2 Lifecycle

A job is queued, then running, then ends in exactly one of:

- **Succeeded.** The result was published. This includes results whose
  outputs are unavailable for a reason that is a function of the inputs;
  the job record carries that reason (from the diagnostics).
- **Cancelled.** By the user, by application shutdown, or because nothing
  wanted it any more while it was still queued. Nothing is published.
- **Superseded.** The engine refused to publish (inputs changed, session
  removed or unloaded, registration removed). Nothing is published. The
  queue does not re-request on its own; whoever still wants the result sees
  it as missing and asks again.
- **Failed.** The environment, not the inputs, prevented completion: the
  worker could not start, or memory was exhausted. Nothing is published and
  nothing is cached, so the calculation can be requested again.

An exception from the compute function other than resource exhaustion is a
calculation failure in the engine's existing sense: it is published and
cached as a failed result, and the job record says so.

A session with a running job should not be unloaded from under it. If the
session model can defer unloading until the job ends, it does; otherwise the
job ends as superseded. Either way nothing crashes and nothing stale is
published.

### 8.3 Cancellation and shutdown

Cancelling a queued job removes it. Cancelling a running job asks the compute
function to stop and the job ends as cancelled when it does; the queue does
not start the next job until then. On application shutdown every job is
cancelled and the application waits for the worker to stop before tearing
down anything the worker could touch. Shutdown must not hang on a job that
ignores cancellation for longer than one solver step, and must not crash.

### 8.4 The model

The queue presents its jobs, current and finished, as a Qt item model. Per
job: the session (id and display name), the calculation (id and title), the
state, the latest progress text, when it was queued, started, and finished,
and the outcome reason where there is one. The model reports changes through
the normal model signals, supports removing finished entries, and keeps
finished entries for the lifetime of the application up to a sensible bound.
Nothing is persisted.

The model is complete in the sense that a dock listing jobs, with their
progress and results, could be written as a view over it and nothing else.
No such dock is part of this work.

## 9. Plot-driven requests

### 9.1 Principle

A plot's checkbox means "show this plot wherever its data is available". It
makes no promise that the data will be computed. Expensive work starts only
from a gesture the user makes on a plot. "Checked but not computed" is an
ordinary, supported state: it is where a plot lands after a restart, when a
new track is shown, when inputs change, and after a cancel.

The logic in this section lives in a widget-free component that can be
tested without the plot list's view. The view only paints what that
component reports and forwards clicks to it.

### 9.2 State of a plot row

For each checked plot, over the currently visible, loaded tracks, each track
is in one of these conditions for that plot:

- **Available.** The plot draws it.
- **Missing.** Blocker inspection reports one or more explicit calculations,
  none of them queued or running.
- **Pending.** At least one of its blockers is queued or running.
- **Failed.** An explicit calculation ran and did not produce the value.
- **Not applicable.** Unavailable for ordinary reasons (no such sensor in
  the recording, missing input). Treated exactly as plots treat missing data
  today: silently absent.

The row aggregates these:

- If any track is pending, the row shows progress and a cancel control
  (a circled x). Progress conveys how many of the tracks the row was waiting
  for are done.
- Otherwise, if any track is missing, the row shows a refresh control (the
  familiar circular-arrows icon) with the number of missing tracks. A track
  that was computed and then invalidated by an input change is simply
  missing again, so stale tracks are counted without special handling. The
  number falls as each track's job publishes and its plot appears.
- Independently of both, if any track failed, the row shows a warning badge
  with the number of failed tracks. There is no retry for these: the same
  inputs give the same failure. A failed track becomes missing, and
  therefore refreshable, when its inputs change.
- A row with nothing pending, missing, or failed looks exactly as it does
  today. So does every row for a plot that is not backed by an explicit
  calculation, and every unchecked row.

The row's tooltip lists the tracks behind the counts: which are pending and
the running job's progress text, which are missing, and which failed with
the reason. Until a dock exists, this is where failures are reported. No
message box is shown for a calculation outcome.

Several plots can wait on the same job (roll, pitch, and yaw share one fit).
Each such row shows the same job's progress. That is accurate: they are
waiting on the same thing.

### 9.3 What starts work

Exactly two gestures:

- **Checking a plot** by direct interaction with its row. Jobs are requested
  for the blockers of every visible track that is missing for that plot.
  This is a one-shot request, not a standing order.
- **Pressing a row's refresh control.** The same request, for that row's
  currently missing tracks.

Nothing else starts work. In particular: restoring checked plots at
startup, applying a profile, showing a track, loading a session, importing or
merging a file, an input change that invalidates a result, and a job ending
as superseded all leave the affected tracks missing, with the refresh control
showing. Reads by the plot, legend, measure tool, logbook columns, the idle
scheduler's column work, the map, exports, and plugins never start an
explicit calculation.

When a requested job finishes and further explicit blockers remain for a
track the row asked about (section 7.2, chained calculations), the request
continues with them without another gesture.

### 9.4 Cancel

A row's cancel control cancels the queued and running jobs that row is
waiting for. The plot stays checked. The affected tracks become missing and
the refresh control returns. Other rows that were waiting on the same jobs
change the same way. No plot is unchecked as a side effect of cancelling.

### 9.5 Jobs nobody wants

When a plot is unchecked or a track is hidden, queued jobs that no checked
plot on a visible track still needs are removed (ending as cancelled). The
running job is left to finish: its result is valid and cached, and hiding a
track for a moment must not throw away minutes of work. Only a cancel
gesture or shutdown stops a running job.

### 9.6 Appearance of results

Nothing special is built for "show the plot when it is done". Publishing
invalidates the names that were read while the calculation was unrequested;
the existing invalidation path repaints the plot, legend, and any dependent
logbook column. Consumers that log a warning when a plot has no data for a
track do not warn for values that are merely uncomputed.

## 10. Documentation

- `docs/SENSOR_FUSION.md` and `docs/LOCAL_COORDINATES.md` carried over and
  corrected for this design (section 4).
- The engine documentation gains the asynchronous request, blocker
  inspection, and the threading rule.
- User-facing documentation explains the refresh control, the counts, the
  warning badge, and cancel.
- README build instructions cover the new dependencies.

## 11. Acceptance

1. The application builds, deploys its solver runtime libraries, and passes
   its tests on Windows, macOS, and Linux through the existing CI workflow.
2. With exact synthetic clock data at high device uptime, UTC conversion
   error is at the microsecond level; existing time-dependent tests pass
   unchanged.
3. Local coordinates: origin selection follows the stated gates; an
   analytically known displacement and velocity rotate correctly into the
   frame; invalid samples give NaN at their index only; no qualifying fix
   makes all outputs unavailable; source changes invalidate the outputs.
   Simplified track: all seven outputs select the same original sample
   indices; every dropped sample lies within 0.5 m of the simplified path;
   duplicate-position endpoints, closed, degenerate, and empty tracks behave
   sensibly; non-finite samples are skipped; the projection runs once per
   recording, in the local-coordinate calculation. With no qualifying origin
   the map shows no track or cursor dot for that recording and recovers when
   the source is corrected.
4. For each committed synthetic fixture, the ported fusion reproduces the
   golden outputs captured from `sensor-fusion-clean-port`.
5. Reading any fusion output, `accH`, the diagnostics attribute, or an
   interpolated logbook value of them, on a session where fusion has not been
   requested, returns unavailable and runs nothing, however many times and in
   whatever order.
6. Requesting fusion runs it once and publishes all outputs together;
   `accH` and the fusion system-time axis become available without being
   requested; a second request runs nothing.
7. Asynchronous and synchronous requests produce identical results.
8. Changing a declared input while a job is running ends the job as
   superseded, publishes nothing, and leaves the calculation requestable.
   Changing one after publication drops the result and every dependent.
9. A recording the model rejects yields unavailable measurements, a
   diagnostics attribute with the reason, a succeeded job carrying that
   reason, no second run on re-request, and a fresh run after its inputs
   change.
10. Cancelling a running job stops it at the next solver boundary, publishes
    nothing, and leaves the calculation requestable. The next queued job then
    starts.
11. A session with no IMU data never appears as missing, pending, or failed
    for any fusion plot, and no job can be created for it.
12. Blocker inspection reports fusion for `accH` on an unrequested session,
    nothing after publication, and never triggers a fit.
13. With two explicit test calculations where B consumes A, one gesture on a
    plot of B's output runs A then B.
14. Never more than one job runs at a time. Duplicate requests create no
    duplicate jobs.
15. Row behavior, tested without widgets: checking a fusion plot with three
    visible fusable tracks queues three jobs; the pending count falls as each
    publishes; unchecking mid-way removes the queued jobs and lets the
    running one finish; cancel leaves the plot checked and the tracks
    missing; showing a fourth track afterwards leaves it missing with a
    count of one and starts nothing; pressing refresh computes it.
16. Starting the application with fusion plots checked and tracks visible
    starts no job. Applying a profile that checks fusion plots starts no job.
17. Removing or unloading a session with a queued or running job, and
    quitting with jobs queued and running, neither crash nor hang, and
    publish nothing stale.
18. The job model reports every transition through model signals and retains
    finished jobs with state, timing, and reason; a test view can render the
    whole history from the model alone.
19. The application stays interactive during a fit: plots pan and zoom,
    tracks can be shown and hidden, and sessions can be edited.
20. None of the branch's modal dialog, re-entrancy guard, idle-scheduler
    pause, or plot-widget rebuild guard exists in the result.

## 12. Principles for the implementers

- The algorithm is frozen and the code is not. If a change to the numerical
  kernels seems necessary, stop and ask.
- The plot is the request. No interface element names a calculation as
  something the user must choose.
- Only a gesture starts expensive work. When in doubt whether something is a
  gesture, it is not.
- One thread owns all state. The worker owns captured inputs and nothing
  else. Do not add locks to make shared access safe; remove the sharing.
- The engine decides staleness. No caller is trusted to remember to check.
- A result that is a function of the inputs is cached, including a failure.
  A failure that is not a function of the inputs is never cached.
- The job model is the single source of truth about work in progress. Rows,
  tooltips, and any future dock are views of it.
- Nothing from the old mechanism is ported for its own sake. If a guard or
  workaround on the branch exists because work ran inside a getter, it has no
  successor here.

## 13. Notes for the planning coordinator

- `PLANS/implementation-plan/` currently holds the completed plan for the
  previous feature. `PLANS/` is untracked. Before writing the new plan, move
  that folder to `PLANS/done/schema-and-calculations-plan/` so the two plans
  do not mix.
- Copy the "Commit Policy" section below into `00-overview.md` as its own
  top-level section with that exact heading, in addition to including this
  specification in full. The implementation orchestrator looks for that
  section in the overview and does not commit without it. Where the policy
  says "Phase N", use the plan's phase numbers; add plan-specific notes
  (parallel tracks that share files, for example) to the copy in the
  overview, not here.
- Testing the fusion calculation itself needs GTSAM; there is no way around
  that and none is wanted. The point is the converse: most of this work is
  not fusion. The time fit, local coordinates, simplification, the engine's
  asynchronous request and blocker inspection, the job queue, and the row
  logic are all exercised with small synthetic explicit calculations defined
  in the tests, and neither they nor the libraries they live in link GTSAM.
  The dependency build is slow and cannot be verified on macOS or Linux
  without a push, so the plan should not make that other work wait on it.
  GTSAM is confined to the fusion code, its registration, and its tests.
- Compiled third-party trees for GTSAM and oneTBB already exist, untracked,
  under `third-party/` on Michael's Windows machine. They may be reused for
  local builds; they are never staged.
- The branch can be read at any time with `git show
  sensor-fusion-clean-port:<path>`. To capture golden outputs, its code may
  be built in a separate worktree outside the working tree being committed.

## Commit Policy

Michael has authorized commits for this plan on a new working branch, so
that each phase can be audited separately afterward. **The implementation
orchestrator makes every commit** (see "Version Control" in
`.claude/prompts/implementation-orchestrator.md`); implementation, revision,
and review agents never run git commands that change repository state. This
section overrides any wording about commits in the phase documents. Nothing
is ever pushed, and nothing is ever committed on `master`.

- **Branch.** Before spawning the first agent, the orchestrator confirms
  that `master` is checked out with no modified tracked files (if there are
  any, stop and ask), then creates the working branch with
  `git switch -c sensor-fusion-jobs master`. All work and every commit for
  this plan happen on `sensor-fusion-jobs`. If the branch already exists
  (a resumed run), switch to it instead; never recreate, reset, or rebase
  it. Nothing from `sensor-fusion-clean-port` is merged, rebased, or
  cherry-picked onto it.
- **Pre-existing untracked paths** are never staged: `PLANS/`, `TEMP/`,
  `experiments/`, `build*/`, `dist/`, `results/`, and the dependency source,
  build, and install trees under `third-party/` (`gtsam/`, `gtsam-build/`,
  `GTSAM-install/`, `oneTBB/`, `oneTBB-build/`, `oneTBB-install/`). If the
  plan adds a dependency as a git submodule, only the submodule entry and
  `.gitmodules` are staged, never the directory contents.
- **One commit per accepted phase**, made when the phase's review agent
  returns ACCEPT, containing exactly the files the phase's implementation
  and revision agents reported, staged by explicit path. Subject:
  `Phase N: <phase name>`. Body: a short summary of the phase's tasks, then
  the session's attribution line (`Co-Authored-By: ...`). Rejected iterations
  are never committed.
- **Tag.** After each phase commit the orchestrator creates the lightweight
  local tag `plan/sensor-fusion-jobs/phase-N-done`. Tags are never moved or
  deleted.
- **Fixes to a closed phase** (found by a later phase, an integration
  debugging agent, or the final review) are committed separately as
  `Phase N fixup: <what>` once reviewed and accepted, under the number of the
  phase being fixed.
- **CI workflow edits** (`.github/workflows/*`) cannot be verified without a
  push. The agent reports those paths separately and the orchestrator commits
  them as `Phase N: CI (unverified)` right after the phase commit, so the
  edit can be dropped or amended on its own later.
- **Escalated phases** are not committed. Their changes stay in the working
  tree, their paths are listed in the final report, and phases that depend on
  them are not started.

Auditing afterward:

```
git log --oneline master..sensor-fusion-jobs
git show --stat plan/sensor-fusion-jobs/phase-3-done
git diff plan/sensor-fusion-jobs/phase-3-done~1 plan/sensor-fusion-jobs/phase-3-done
git log --oneline --grep='^Phase [0-9]* fixup' master..sensor-fusion-jobs
```
