# Implementation Plan: Sensor fusion as an explicit calculation, with plot-driven background jobs

Source specification: `PLANS/sensor-fusion-jobs.md` (reproduced in full below).
Baseline: `master` at or after `13c6115`. Working branch: `sensor-fusion-jobs`
(see "Commit Policy"). Behavioral reference: branch `sensor-fusion-clean-port`,
readable with `git show sensor-fusion-clean-port:<path>`; never merged,
rebased, or cherry-picked.

## Feature Specification

The complete specification follows, unabridged. Its headings are demoted two
levels so they nest under this section; section numbers ("section 7.2") in the
phase documents refer to these numbered headings. The specification's own
"Commit Policy" is also reproduced as the top-level "Commit Policy" section of
this overview, which is the copy the implementation orchestrator acts on.

<!-- BEGIN SPECIFICATION -->

### Sensor fusion as an explicit calculation, with plot-driven background jobs

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

#### 1. Motivation

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

#### 2. Scope

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

#### 3. Third-party dependencies

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

#### 4. Porting rules

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

#### 5. Shared calculations

##### 5.1 Time fit

The shared `_TIME_FIT_A` / `_TIME_FIT_B` calculation loses precision at
realistic device uptimes because its least-squares sums are not centered
(tens of milliseconds on an exact synthetic clock). The fit is computed with
centered sums. Names, inputs, outputs, and consumers are unchanged. A test
with an exact synthetic clock at high uptime bounds the conversion error at
the microsecond level.

##### 5.2 Local coordinates

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

##### 5.3 Simplified track

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

#### 6. The fusion calculation

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

#### 7. Engine changes

The engine on `master` is single-threaded and offers a synchronous
`request()`. It stays single-threaded: every engine, registry, and session
access remains on the main thread. Two capabilities are added.

##### 7.1 Asynchronous request

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

##### 7.2 Blocker inspection

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

#### 8. Job queue

One application-wide queue owns all background calculation work. It lives
with the session model and the engine in the widget-free core, is driven from
the main thread, and is testable without a GUI.

##### 8.1 Jobs

A job is one explicit calculation for one session. Requests are
deduplicated: asking for a job that is queued or running is a no-op. Jobs run
one at a time, first come first served. A job for a session that is not
loaded cannot start; the queue does not load sessions.

##### 8.2 Lifecycle

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

##### 8.3 Cancellation and shutdown

Cancelling a queued job removes it. Cancelling a running job asks the compute
function to stop and the job ends as cancelled when it does; the queue does
not start the next job until then. On application shutdown every job is
cancelled and the application waits for the worker to stop before tearing
down anything the worker could touch. Shutdown must not hang on a job that
ignores cancellation for longer than one solver step, and must not crash.

##### 8.4 The model

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

#### 9. Plot-driven requests

##### 9.1 Principle

A plot's checkbox means "show this plot wherever its data is available". It
makes no promise that the data will be computed. Expensive work starts only
from a gesture the user makes on a plot. "Checked but not computed" is an
ordinary, supported state: it is where a plot lands after a restart, when a
new track is shown, when inputs change, and after a cancel.

The logic in this section lives in a widget-free component that can be
tested without the plot list's view. The view only paints what that
component reports and forwards clicks to it.

##### 9.2 State of a plot row

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

##### 9.3 What starts work

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

##### 9.4 Cancel

A row's cancel control cancels the queued and running jobs that row is
waiting for. The plot stays checked. The affected tracks become missing and
the refresh control returns. Other rows that were waiting on the same jobs
change the same way. No plot is unchecked as a side effect of cancelling.

##### 9.5 Jobs nobody wants

When a plot is unchecked or a track is hidden, queued jobs that no checked
plot on a visible track still needs are removed (ending as cancelled). The
running job is left to finish: its result is valid and cached, and hiding a
track for a moment must not throw away minutes of work. Only a cancel
gesture or shutdown stops a running job.

##### 9.6 Appearance of results

Nothing special is built for "show the plot when it is done". Publishing
invalidates the names that were read while the calculation was unrequested;
the existing invalidation path repaints the plot, legend, and any dependent
logbook column. Consumers that log a warning when a plot has no data for a
track do not warn for values that are merely uncomputed.

#### 10. Documentation

- `docs/SENSOR_FUSION.md` and `docs/LOCAL_COORDINATES.md` carried over and
  corrected for this design (section 4).
- The engine documentation gains the asynchronous request, blocker
  inspection, and the threading rule.
- User-facing documentation explains the refresh control, the counts, the
  warning badge, and cancel.
- README build instructions cover the new dependencies.

#### 11. Acceptance

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

#### 12. Principles for the implementers

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

#### 13. Notes for the planning coordinator

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

#### Commit Policy

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

<!-- END SPECIFICATION -->

## Phases

| Phase | Name | Purpose | Dependencies |
|-------|------|---------|--------------|
| 1 | Solver dependencies | Build GTSAM 4.3a0 (official repository pin, bundled Eigen, built without Boost) and oneTBB in the third-party superbuild, export them to the app build, deploy their runtime libraries on all three platforms, cache them in CI, and document the build in the README. Nothing links them yet except a minimal link/run smoke check. | None |
| 2 | Time fit and local coordinates | Centered least-squares time fit (spec 5.1); the recording-wide `Local/...` calculation, its origin attributes, and the six "GNSS (Local frame)" plots (spec 5.2); `docs/LOCAL_COORDINATES.md`. | None |
| 3 | Simplified track on the shared frame | Re-layer `Simplified/...` on `Local/north`/`east` with retained indices and three new outputs; make the map models behave when a track is unavailable and when it comes back (spec 5.3). | Phase 2 |
| 4 | Engine: asynchronous request and blocker inspection | Prepare / compute / publish split with engine-decided staleness, the progress-and-cancel facility, and blocker inspection that sees through on-demand intermediates and distinguishes "ran and did not produce" from "never run" (spec 7); engine docs. | None |
| 5 | Job queue and job model | Application-wide, widget-free, one-at-a-time queue of explicit calculations with the four end states, deduplication, cancellation, shutdown, session unload/removal handling, a 64 MiB-stack worker, and the Qt item model of current and finished jobs (spec 8). | Phase 4 |
| 6 | Plot request logic | Widget-free component that computes per-plot row state (available / missing / pending / failed / not applicable, counts, tooltip content), turns the two gestures into job requests, continues chained blockers, cancels, and prunes queued jobs nobody wants (spec 9.1-9.5), tested with synthetic explicit calculations. | Phase 5 |
| 7 | Plot list view and application wiring | Paint refresh / progress+cancel / warning badge in the plot-list rows and forward clicks; distinguish a direct check from programmatic checks (startup restore, profiles); create the queue and request component in the application; orderly shutdown; silence "no data" warnings for merely uncomputed values (spec 9.2-9.6, 8.3). | Phase 6 |
| 8 | Fusion kernel port and golden parity | Capture golden outputs from `sensor-fusion-clean-port` for synthetic fixtures against the shipped (Boost-free) GTSAM, cross-checked byte for byte against the Boost-enabled build the branch was validated with; retype the input validation, initializer, factor-graph fit, and output extraction as a pure, GTSAM-linked `flysight_fusion` library in `master`'s style with progress/cancel boundaries; prove parity with QtTest (spec 4, 6, acceptance 4). | Phase 1 |
| 9 | Fusion calculation, derived values, and plots | Register the one explicit multi-output "Sensor fusion" calculation (`Fusion/...`, `_FUSION_DIAGNOSTICS`), the on-demand `Fusion/accH` and `Fusion/_system_time`, and the seventeen "Sensor fusion" plots; link the application to the fusion library; session-level tests for acceptance 5-12 with the real fit through the queue. | Phases 2, 4, 5, 8 |
| 10 | Documentation, acceptance map, and audit | Remove the now-unused Boost from the build, README and CI; `docs/SENSOR_FUSION.md` carried over and rewritten for this lifecycle; user-facing docs for refresh / counts / badge / cancel; acceptance map for criteria 1-20; end-to-end row test with the real fusion plots; audit that none of the branch's modal dialog, re-entrancy guard, scheduler pause, or rebuild guard exists and that nothing outside the fusion code links GTSAM. | Phases 1-9 |

## Dependency Graph

```
Track A (shared calculations)      2 ──► 3 ──────────────────────┐
                                   │                             │
Track B (engine, jobs, rows)       │   4 ──► 5 ──► 6 ──► 7 ──────┤
                                   │   │     │                   ├──► 10
Track C (solver, fusion)     1 ──► 8 ──┼─────┼──► 9 ─────────────┘
                                   │   │     │    ▲
                                   └───┴─────┴────┘   (9 needs 2, 4, 5, 8)
```

- Phases 1, 2, and 4 have no dependencies and start in parallel.
- Track B never waits on Track C: everything in phases 4-7 is tested with
  small synthetic explicit calculations defined in the tests, and none of the
  libraries they touch link GTSAM (spec section 13).
- Phase 1 cannot be verified on macOS or Linux without a push; its CI edits
  are committed separately as "CI (unverified)". Phase 8 needs only a working
  local (Windows) GTSAM: the Boost-free build Phase 1 leaves in
  `build-solver-deps/` (Integration Note 14), so a Phase 1 that is accepted
  locally unblocks Phase 8. The pre-existing untracked install trees under
  `third-party/` are a Boost-enabled build and serve only Phase 8's
  cross-check.
- Phase 9 is the only point where the tracks join before the final phase.

## Key Patterns & References

Paths are relative to the repository root on `master` unless prefixed
`branch:`, which means `git show sensor-fusion-clean-port:<path>`.

### Process and conventions
- `.claude/prompts/implementation-orchestrator.md` — how phases are executed and committed ("Version Control")
- `.claude/docs/WORKFLOW-REFERENCE.md` — the overall plan/implement/review workflow
- `PLANS/schema-and-calculations.md` — the engine specification; section 7.4 (idempotency) and 7.7 (explicit policy) bind this work
- `PLANS/calculations-implementation-plan/` — the previous, completed plan; a model for phase-document depth in this codebase
- `docs/CALCULATIONS.md` — engine and registration documentation; gains async request, blocker inspection, threading rule
- `docs/DATA_SCHEMA.md` — name and unit conventions for sensors, measurements, attributes
- `README.md` — configure/build/install steps; gains the new dependencies
- `tests/README.md` — test conventions, harness, how acceptance criteria are mapped
- `tests/acceptance_map.txt` — acceptance-criterion-to-test map from the previous plan
- `tests/audit/cleanup_audit.cmake` — scripted "this must not exist in the tree" audit; pattern for acceptance 20

### Engine (flysight_model, Qt Core only)
- `src/engine/calctypes.h` — `EvaluationPolicy` (OnDemand / Explicit), value references, `ResultStatus` (incl. `NotRequested`, `MissingInput`)
- `src/engine/calculationdescriptor.h` — descriptor: id, declared inputs, outputs, policy, compute function
- `src/engine/calculationengine.h` / `.cpp` — synchronous `request()`, `RequestOutcome`, `resultStatus()`, provider inspection, dependency graph, invalidation, cycle handling; the code that gains prepare/publish and blocker inspection
- `src/engine/calculationregistry.h` / `.cpp` — registration, lookup by output, registration removal
- `src/engine/calculationresult.h` / `.cpp` — the multi-output result container
- `src/engine/evaluationcontext.h` / `.cpp` — what a compute function sees; the purity boundary
- `src/engine/sessionstate.h` — `ISessionState`, the engine's read-only view of a session
- `src/dependencykey.h` — keys carried by invalidation signals
- `src/sessiondata.h` / `.cpp` — owns the per-session engine (`calculationEngine()`), effective-value reads, copy/move rules for engine and cache

### Registered calculations (flysight_core)
- `src/calculations/registration.h` — registration helper conventions
- `src/calculations/builtincalculations.cpp` / `.h` — the single entry point that registers every built-in
- `src/calculations/timecalculations.cpp` / `.h` — `_TIME_FIT_A` / `_TIME_FIT_B` and the shared UTC time; the uncentered sums to fix
- `src/calculations/gnsscalculations.cpp` / `.h` — multi-input GNSS calculations; course unwrapping rule reused for roll/pitch/yaw
- `src/calculations/imucalculations.cpp` / `.h` — IMU channel calculations (pattern for `accH`)
- `src/calculations/simplificationcalculations.cpp` / `.h` — current private-projection RDP with coordinate re-matching
- `src/calculations/attributecalculations.cpp`, `attributeregistration.cpp` — attribute-output calculations (pattern for `_LOCAL_ORIGIN_*`, `_FUSION_DIAGNOSTICS`)
- `src/calculations/interpolationcalculations.cpp` — interpolated logbook values (acceptance 5)
- `src/calculations/derivativehelper.cpp`, `isadensity.cpp` — small, commented numerical helpers: the style target for the port

### Branch reference: shared calculations
- `branch:src/calculations/timecalculations.cpp` — centered time fit
- `branch:src/calculations/localcoordinatecalculations.cpp` / `.h` — origin gates, LocalCartesian transform, velocity rotation
- `branch:src/calculations/simplificationcalculations.cpp` / `.h` — index-retaining RDP on the shared frame
- `branch:src/calculations/anglehelper.h`, `branch:src/calculations/gnsscalculations.cpp` — shared unwrap rule
- `branch:docs/LOCAL_COORDINATES.md` — documentation to carry over
- `branch:tests/time_regression.cpp`, `localcoordinate_regression.cpp`, `simplification_regression.cpp`, `map_regression.cpp` — test ideas and fixtures only

### Branch reference: fusion
- `branch:src/batchfusion.cpp` / `.h` — the GTSAM factor-graph fit, initializer, tuning constants, cancellation boundaries
- `branch:src/fusioninput.cpp` / `.h` — the input adapter: the exact declared-input list and the validation/rejection rules
- `branch:src/imugnssekf.cpp` / `.h` — output assembly (renamed here; nothing is called an EKF)
- `branch:src/calculations/sensorfusioncalculations.cpp` / `.h` — the seventeen per-output registrations and derived values being replaced by one explicit calculation
- `branch:src/mainwindow.cpp` — the seventeen plot registrations to carry over (and the modal dialog that is not)
- `branch:src/idlescheduler.cpp`, `branch:src/ui/docks/plot/PlotWidget.cpp`, `branch:src/sessiondata.h`, `branch:src/calculatedvalue.cpp` — guards that must NOT be ported (acceptance 20)
- `branch:tests/fusion_regression.cpp`, `fusion_session_regression.cpp`, `full_window_regression.cpp`, `branch:scripts/run-full-window-regression.py` — synthetic fixtures and self-tests for golden capture
- `branch:docs/SENSOR_FUSION.md`, `branch:docs/PORT_VALIDATION.md` — model, limitations, input contract; optional local real-recording check
- `build-port/Release/` (untracked, Michael's machine) — an existing build of the branch including `fusion_regression.exe` and the GTSAM/TBB DLLs

### Build, third-party, deployment, CI
- `CMakeLists.txt` — superbuild options (`FLYSIGHT_BUILD_THIRD_PARTY`, `FLYSIGHT_BUILD_TESTS`, `FLYSIGHT_BUILD_PYTHON_TESTS`), install-dir discovery
- `cmake/ThirdPartySuperbuild.cmake` — how GeographicLib and KDDockWidgets are built and installed
- `third-party/CMakeLists.txt` — third-party project wiring
- `cmake/BoostDiscovery.cmake` — Boost lookup for the application's header-only `boost::geometry` use (GTSAM is built without Boost; Phase 10 deletes this file)
- `cmake/DeployThirdPartyWindows.cmake`, `DeployThirdPartyMacOS.cmake`, `DeployThirdPartyLinux.cmake`, `cmake/fix_macos_rpaths.sh`, `cmake/CreateAppDir.cmake` — runtime deployment per platform
- `src/CMakeLists.txt` — `flysight_model` / `flysight_core` / application targets, link rules, install rules
- `tests/CMakeLists.txt` — `flysight_add_test`, `flysight_test_support`, the optional-Python-test gating pattern
- `.github/workflows/build.yml` — CI, third-party cache keys and paths
- `.gitmodules` — how existing third-party sources are pinned
- `branch:cmake/SolverSuperbuild.cmake`, `branch:cmake/SolverDependencies.cmake`, `branch:cmake/ThirdPartySuperbuild.cmake`, `branch:CMakeLists.txt`, `branch:src/CMakeLists.txt`, `branch:.github/workflows/build.yml`, `branch:README.md` — the working reference for GTSAM/oneTBB (exported target, target-derived deployment, 64 MiB stack link flags)

### Session model, scheduler, and other readers (flysight_core)
- `src/sessionmodel.h` / `.cpp` — rows, loaded vs stub sessions, LRU eviction (`evictIfNeeded`, `evictSession`), `RowStabilityGuard`, `removeSessions`, `dependencyChanged`, `visibilityChanged`, `forEachLoadedSession`
- `src/idlescheduler.h` / `.cpp` — idle column work; a reader that must never start explicit work
- `src/logbookcolumn.cpp`, `src/logbookmanager.cpp` — logbook column reads
- `src/dataexporter.cpp`, `src/pluginadapters.cpp`, `src/pluginsessionview.h`, `src/sessiondata_bindings.cpp` — export and plugin reads (effective values only)
- `src/sessionimport.cpp`, `src/sessionmerge.cpp` — import/merge: source changes that invalidate and supersede

### Plot list, plots, and application wiring
- `src/plotregistry.h` / `.cpp` — `PlotValue` (category, sensor, measurement, units) and registration
- `src/plotmodel.h` / `.cpp` — the category/plot tree behind the plot list; `setData` check handling, `togglePlot`, `setPlots`
- `src/ui/docks/plotselection/PlotSelectionDockFeature.h` / `.cpp` — the plot-list `QTreeView`; where a delegate and click forwarding attach
- `src/ui/docks/AppContext.h`, `DockFeature.h`, `DockRegistry.cpp` — how application-wide objects reach dock features
- `src/mainwindow.cpp` / `.h` — built-in plot registration (~line 961), `PlotModel` ownership, registration of built-in calculations, close/shutdown path
- `src/main.cpp` — application start-up order
- `src/profilestatebridge.cpp`, `src/profile.cpp`, `src/profilemanager.cpp` — programmatic plot checking (profiles, startup restore) that must not start work
- `src/plotviewsettingsmodel.cpp` — persisted plot check state
- `src/ui/docks/plot/PlotWidget.cpp` — graph building from session reads; the "No data available for plot" warning (~line 532) to silence for uncomputed values
- `src/ui/docks/legend/LegendPresenter.cpp`, `LegendTableModel.cpp`, `src/plottool/measuretool.cpp` — readers repainted through ordinary invalidation
- `src/resources.qrc`, `src/resources/` — icons (refresh, cancel, warning)
- `src/ui/docks/logbook/` — an existing item view with custom painting, if any delegate pattern is needed

### Map
- `src/ui/docks/map/TrackMapModel.h` / `.cpp` — consumes `Simplified/lat`, `lon`, `hMSL`, `_time`; bounds
- `src/ui/docks/map/MapCursorDotModel.h` / `.cpp` — cursor dot per recording
- `src/ui/docks/map/MapBridge.cpp`, `MapWidget.cpp` — bounds consumers

### Tests
- `tests/tst_calcengine.cpp` — engine tests with a synthetic `World`; `explicitPolicy()` (previous acceptance 14) is the starting point for async/blocker tests
- `tests/tst_calcengine_safety.cpp` — safety tests incl. synthetic explicit calculation `explicitE`
- `tests/tst_calcengine_oracle.cpp` — read-order independence oracle (idempotency must survive blocker inspection)
- `tests/tst_calcregistry.cpp` — registry tests
- `tests/tst_builtins_golden.cpp`, `tests/tst_builtins_engine.cpp` — golden-value pattern for built-ins; time, GNSS, simplification cases
- `tests/tst_session_engine.cpp`, `tests/tst_session_model_engine.cpp` — `SessionData` / `SessionModel` with the engine: invalidation, eviction
- `tests/tst_workflow.cpp` — widget-free end-to-end workflow
- `tests/tst_python_bridge.cpp` — optional, separately gated test with heavy dependencies (pattern for GTSAM-gated tests)
- `tests/support/fakesessionstate.*`, `fixturebuilder.*`, `builtinfixture.*`, `testenvironment.*`, `oraclecatalogue.*`, `logbookprobe.*`, `testutil.h`, `testmain.h` — the support library

## Decisions & Constraints

Decisions made during discovery. Phase documenters refine within them; they do
not reopen them.

1. **Previous plan folder.** `PLANS/implementation-plan/` did not exist when
   planning began; the previous plan already lives at
   `PLANS/calculations-implementation-plan/`. Nothing was moved.
2. **Library boundaries.**
   - Engine changes (phase 4) stay in `flysight_model`: Qt Core only, no
     threads created there. The engine provides prepare and publish on the
     main thread and a compute step that is callable from any thread; it does
     not own the worker.
   - The job queue and job model (phase 5) and the plot request logic
     (phase 6) live in `flysight_core`, next to `SessionModel`. If the request
     logic needs plot check state from `PlotModel` (currently an application
     source), it takes it through a narrow widget-free interface or
     `PlotModel` moves into `flysight_core`; the phase 6 documenter chooses,
     but the component and its tests must link without Widgets.
   - A new static library `flysight_fusion` (phases 8-9) is the only target
     that links GTSAM. It contains the kernel, the fusion registration, the
     derived `accH` / `_system_time` calculations, and one registration entry
     point. `flysight_core` never references it; the application calls the
     entry point next to the built-in registration. Fusion tests link it and
     are gated by a CMake option modelled on `FLYSIGHT_BUILD_PYTHON_TESTS`,
     so every other test builds and runs without GTSAM.
3. **Kernel independence.** The phase 8 kernel takes progress reporting and
   cancellation through its own minimal callback parameters so it depends on
   phase 1 only. Phase 9 adapts the engine's facility (phase 4) to them.
4. **Worker thread.** The queue's worker thread is created with a 64 MiB
   stack (phase 5) regardless of which calculation runs; test executables
   that run fits on their main thread get the linker stack setting (phases 8,
   9). This is the application's only worker thread.
5. **Captured inputs.** Engine values are captured for the worker as
   immutable, detached copies or shared-immutable handles; no locks are added
   anywhere (spec section 12).
6. **Public names** are exactly those in the specification: sensors `Local`,
   `Simplified`, `Fusion`; attributes `_LOCAL_ORIGIN_LAT/LON/HMSL/INDEX`,
   `_FUSION_DIAGNOSTICS`; categories "GNSS (Local frame)" and "Sensor
   fusion"; calculation title "Sensor fusion". Internal names follow
   `master`; nothing is named EKF.
7. **Golden capture** (phase 8) builds the branch in a separate git worktree
   outside the working tree (or reuses `build-port/`), adds a throwaway dump
   harness there, and commits only the resulting data files under `tests/`
   on the working branch. The harness is built against the shipped solver
   (upstream GTSAM, no Boost) and, as a cross-check, against the old
   Boost-enabled build; the two outputs must be byte-identical (Phase 8).
   The harness is described in the test README so the capture is
   repeatable.
8. **No widgets in logic.** Phase 7 adds only painting, hit-testing, tooltips,
   and wiring. Any decision about state, counts, or what to request belongs
   to phase 6 and is tested there.
9. **Gesture detection.** "Checked by direct interaction" is identified at
   the view (phase 7) and passed to the phase 6 component as an explicit
   call; the component never infers a gesture from a model change.
10. **Shared files across parallel tracks.** `src/CMakeLists.txt`,
    `tests/CMakeLists.txt`, `CMakeLists.txt`, `src/calculations/builtincalculations.cpp`,
    `src/mainwindow.cpp`, `docs/CALCULATIONS.md`, `README.md`, and
    `tests/acceptance_map.txt` are edited by more than one phase. Each phase
    keeps its edits to these files small, additive, and in clearly separate
    hunks (see the note in "Commit Policy").

### Acceptance criteria ownership

| Spec acceptance | Owning phase(s) |
|---|---|
| 1 | 1 (build/deploy/CI), 10 (final check) |
| 2 | 2 |
| 3 | 2 (local coordinates), 3 (simplified track, map) |
| 4 | 8 |
| 5, 6, 9, 11 | 9 (11 also 6 with synthetic calculations) |
| 7, 8, 12 | 4 (synthetic), 9 (fusion) |
| 10, 14, 17, 18 | 5 (17 also 7 for application shutdown) |
| 13, 15 | 6 |
| 16 | 6 (logic), 7 (startup and profile wiring) |
| 19 | 7, 9 (manual verification) |
| 20 | 10 (audit) |

### Risks and open questions

- macOS and Linux builds of GTSAM/oneTBB and their deployment cannot be
  verified locally; expect `Phase 1 fixup` commits after the first push.
- Last-bit parity depends on compiler flags matching the branch build
  (fast-math off, TBB on) and on the GTSAM build options, which match the
  branch's except for the two Boost switches: GTSAM is built without Boost
  (`GTSAM_ENABLE_BOOST_SERIALIZATION=OFF`, `GTSAM_USE_BOOST_FEATURES=OFF`),
  from the official repository rather than the fork. Source inspection says
  neither affects arithmetic; Phase 8 does not rely on that: it captures the
  goldens against the shipped configuration, runs the same harness against
  the old Boost-enabled fork build, and requires byte-identical output for
  every fixture. If they differ, the phase stops and Michael decides. Phase 8
  records the flags, options and the cross-check result in `capture.json`.
- A Boost-free GTSAM is built by GTSAM's own CI on Ubuntu only. The shared
  MSVC build is verified locally in Phase 1; macOS only after a push. The
  documented way back is Phase 1's "Appendix: fallbacks".
- Deferring LRU eviction while a job runs (spec 8.2) touches
  `SessionModel`'s eviction path, which has its own invariants (row
  stability, failed saves). If deferral is not clean, the fallback is
  "superseded", which the spec allows.

## Integration Notes (binding corrections)

Found by the integration check after all phase documents were written. Where
a phase document disagrees with a note here, this section wins. The phase
documents have been edited to match; the notes remain as the record.

1. **Acceptance map.** The final scheme is Phase 10's: item `100 + n` for
   spec acceptance `n`, audited. Phases 2, 3 and 8 add real `100 + n` lines.
   Phases 4, 5, 6, 7 and 9 add `# SFJ n ...` comment lines because the audit
   on `master` only accepts items 1-19 until Phase 10 extends it; Phase 10
   converts every `# SFJ` comment into an audited line. Block headers in the
   map follow Phase 2's form.
2. **`docs/CALCULATIONS.md` section numbers** are fixed regardless of commit
   order: 12-14 engine (Phase 4), 15 job queue (Phase 5), 16 plot-driven
   requests (Phases 6 and 7), 17 sensor fusion (Phase 9). A phase that lands
   before a lower-numbered one still uses its own number.
3. **`tests/README.md` executable counts** are relative. Every phase adds its
   own executables to whatever count it finds ("+N"); absolute "24 to 25"
   style numbers in phase documents are illustrative. Phase 10 sets the
   final count. New README sections are numbered and go before Appendix A.
4. **"No EKF" checks** are scoped: no identifier, file name, calculation id,
   sensor or attribute containing `ekf` (any case) or `_IMU_GNSS_EKF`, and no
   `posN`/`posE`/`posD` output, in `src/` and `tests/*.cpp|*.h`. A bare
   `grep -rni ekf src tests` is not the test: `SP_MediaSeekForward` in
   `src/ui/docks/video/VideoWidget.cpp` matches it, and `tests/README.md`
   legitimately names the branch's files in the golden-capture procedure.
5. **Branch guards that must not appear** are exactly: the fusion
   `QProgressDialog` and its nested event loop, `sensorFusionIsRunning`, the
   idle-scheduler pause, `m_rebuildingPlot` with its `QScopedValueRollback` /
   `qScopeGuard` pair, the thread-local re-entrancy guard, and the
   `ImuGnssEkf = "_IMU_GNSS_EKF"` session key. `m_pendingRebuildLevel` is
   `master`'s own and stays. Edits to `src/sessiondata.h` are allowed
   (Phases 2 and 9 add `SessionKeys` there).
6. **Who links `gtsam`:** `flysight_fusion` is the only product target. The
   test-side targets `tst_solver_smoke`, `solver_deploy_probe`, and
   `tst_fusion_kernel` also name it; other fusion tests get it through
   `flysight_fusion`.
7. **Counts:** seventeen "Sensor fusion" plots; eighteen outputs of
   `builtin.fusion.fit` (seventeen measurements plus `_FUSION_DIAGNOSTICS`).
8. **`FLYSIGHT_BUILD_WIDGET_TESTS`** (Phase 7) is declared in the root
   `CMakeLists.txt` and forwarded through `_APP_CMAKE_ARGS` exactly as
   Phase 1 forwards `FLYSIGHT_BUILD_FUSION_TESTS`.
9. **Boost.** GTSAM is built without Boost (Michael's decision after
   planning), so Boost is never a GTSAM requirement: Phase 1 adds no Boost
   requirement, README text or CI step, and leaves the application's existing
   header-only Boost lookup, README text and CI steps exactly as on `master`
   (phases 1 and 3 run in parallel, and until Phase 3 lands
   `simplificationcalculations.cpp` still uses `boost::geometry`). After
   Phase 3 nothing at all needs Boost. Phase 10 removes it completely:
   `find_package(Boost REQUIRED)`, the `Boost::boost` link items and the
   comment in `src/CMakeLists.txt`, `cmake/BoostDiscovery.cmake`, the README
   and CONTRIBUTING prerequisite (phase commit), and the CI Boost install and
   verification steps (`Phase 10: CI (unverified)`). If Phase 1's fallback to
   a Boost-enabled GTSAM is ever taken, the previous rule applies instead
   (Boost described as a GTSAM requirement; `find_package(Boost)` stays).
10. **"Explicit-backed" predicate.** Phase 6 and Phase 9 (Task 9.5) each
    define it privately; they must agree on "any name in the static
    dependency closure has a candidate with explicit policy, looking through
    source conversions". Phase 10 Task 10.2 unifies them if that is small.
11. **Criteria of the form "`git diff master -- <shared file>` shows one
    added hunk"** mean one hunk *attributable to that phase*; other phases'
    hunks in shared files are expected.
12. **Answered open questions.** Phase 5's question on cached logbook columns
    over explicit outputs is answered by Phase 9 Task 9.5 (cached as
    unavailable). Phase 4's question on publishing after a cancel request is
    answered by Phase 5 (cancel wins).
13. **Remaining gaps, by design:** acceptance 1 on macOS/Linux is confirmed
    only after a push; the real `MainWindow` start-up/profile path of
    acceptance 16, application quit in 17, and pan/zoom in 19 are manual
    steps (Phase 7's script, run in Phase 10).

14. **Which GTSAM, and where it lives locally.** The shipped solver is
    upstream GTSAM `814a734` built without Boost. CI and fresh clones install
    it at the default `third-party/GTSAM-install` / `third-party/oneTBB-install`.
    On Michael's machine those two directories hold the older **Boost-enabled
    fork build**, which must never be staged, deleted, cleaned or overwritten
    and is used only by Phase 8's cross-check. There, Phase 1 builds the
    Boost-free solver once into `build-solver-deps/GTSAM-install` and
    `build-solver-deps/oneTBB-install` (git-ignored, kept after Phase 1), and
    **every local application configure in every later phase** passes
    `-DGTSAM_INSTALL_DIR=<repo>/build-solver-deps/GTSAM-install
    -DONETBB_INSTALL_DIR=<repo>/build-solver-deps/oneTBB-install`. A configure
    that forgets this stops at Phase 1's Boost-free guard; that error is the
    reminder, not a defect. Phases 2-7 are affected only in this way (the
    application configure requires GTSAM once Phase 1 has landed).

### Open questions for Michael (non-blocking)

- **Resolved (Michael, after planning) — GTSAM pin.** Phase 1 pins GTSAM to
  the official repository, `https://github.com/borglab/gtsam.git` at
  `814a734d68cbf5068a4bf20d63ba67c4935905bb` (4.3a0), not to fork commit
  `8938b9f` of `crwper/gtsam`. The fork's only change (a commented-out
  `add_subdirectory (js)` in bundled GeographicLib's CMake) matters only with
  `GTSAM_INSTALL_GEOGRAPHICLIB=ON`, and the plan builds with `OFF`. "Let's
  try": if upstream fails to configure or build for a reason that patch
  fixes, Phase 1 switches the pin to the fork (one repository/SHA pair in
  `cmake/SolverSuperbuild.cmake`) and reports it.
- **Resolved (Michael, after planning) — GTSAM without Boost.** GTSAM is
  built with `-DGTSAM_ENABLE_BOOST_SERIALIZATION=OFF
  -DGTSAM_USE_BOOST_FEATURES=OFF` on all three platforms. This removes the
  Boost <= 1.88 ceiling (Homebrew is at 1.92; `boost_system` was removed in
  1.89), the `boost@1.85` CI pin, shared Boost + ICU runtime deployment on
  macOS/Linux and with it the macOS deployment-target question, and the
  `Boost_USE_STATIC_LIBS` / `BOOST_ROOT` plumbing for GTSAM; after Phase 3
  nothing needs Boost, and Phase 10 removes it from the build, README and CI.
  Consequence for parity: Phase 8 captures its goldens against this
  configuration and cross-checks them byte for byte against the old
  Boost-enabled build on Windows; a difference stops the phase and comes back
  to Michael. "Let's try": the previous Boost-enabled design is preserved in
  Phase 1's "Appendix: fallbacks" (Boost 1.65-1.88 and its component list,
  `boost@1.85` on macOS, the Boost/ICU closure step); going back is Michael's
  decision, not an agent's.
- Phase 7: refresh and cancel have no keyboard or menu surface (spec 9.3,
  "when in doubt, it is not a gesture"). Documented as a known limitation.

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

### Plan-specific notes

- Phase numbers in this policy are the phase numbers of this plan (1-10);
  tags run from `plan/sensor-fusion-jobs/phase-1-done` to
  `plan/sensor-fusion-jobs/phase-10-done`.
- Three tracks can run in parallel (phases 1 / 2-3 / 4-7, then 8 and 9) and
  they share files: `CMakeLists.txt`, `src/CMakeLists.txt`,
  `tests/CMakeLists.txt`, `src/calculations/builtincalculations.cpp`,
  `src/mainwindow.cpp`, `docs/CALCULATIONS.md`, `README.md`, and
  `tests/acceptance_map.txt`. When two phases in progress have both edited
  one of these files, the orchestrator does not stage the whole file for the
  first phase to be accepted: it either serializes those two phases' commits
  (hold the accepted phase's commit until the file contains only its own
  hunks, by having the other track's agent report and temporarily not touch
  that file) or, simpler and preferred, avoids the situation by not starting
  a phase that edits a shared file while another phase with uncommitted edits
  to the same file is open. `git add -p` is interactive and is not used.
- Phase 1's edits to `.github/workflows/build.yml` are the expected
  `Phase 1: CI (unverified)` commit. Phase 10 Task 10.12 (removing the Boost
  steps from the same file) is the expected `Phase 10: CI (unverified)`
  commit. Phase 10 also deletes one tracked file,
  `cmake/BoostDiscovery.cmake`; the agent deletes it in the working tree and
  reports it, and the orchestrator stages the deletion by explicit path.
- On Michael's machine the Boost-free solver install made by Phase 1 lives in
  `build-solver-deps/` at the repository root. It is covered by `build*/` in
  the never-staged list above and by `.gitignore`; it is named here because,
  unlike other `build*/` directories, it must be **kept** between phases
  (phases 8-10 build against it) and must not be cleaned by an agent. The
  pre-existing `third-party/GTSAM-install/` and `oneTBB-install/` stay in the
  never-staged list unchanged; they are the old Boost-enabled build, kept for
  Phase 8's cross-check, and no phase writes to them.
- Phase 8's golden capture builds `sensor-fusion-clean-port` in a separate
  worktree outside this working tree (or reuses the untracked `build-port/`).
  Nothing from that worktree, and no dump harness source that exists only on
  the branch side, is staged; only the captured data files and the ported
  tests under `tests/` are.
