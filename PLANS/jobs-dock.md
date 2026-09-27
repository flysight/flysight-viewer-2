# Jobs dock and calculation details

Date: 2026-09-21
Status: specification for planning. Not an implementation plan.
Baseline: the completed sensor-fusion-jobs work including its clean-up round
(`sensor-fusion-jobs` at or after `1f3139a`, or `master` once that branch has
merged). Work happens on a new branch; see "Commit Policy".
Revised 2026-09-21 (evening) for the clean-up round: baseline commit, the
`_exact` tests, the cancel boundary in preparation, and the early stop of a
stale job are now facts, not open items.
Predecessor specification: `PLANS/sensor-fusion-jobs.md`. Its sections 7 to 9
and 12 remain in force; this document extends them and says so where it does.

This document describes what the change must do and the architectural
boundaries it must respect. It deliberately avoids class layouts, container
choices, and function signatures unless a name is part of the observable
contract. The planning agent decides phasing; the implementation agent decides
code structure within these boundaries.

## 1. Motivation

Explicit calculations now run as background jobs started from plot rows. When
one fails, the user learns a single sentence from a tooltip. That is not
enough, for the user or for us.

The case that prompted this: recording `24-09-05/11-17-12` (see section 12).
Its fit runs for about seven minutes and ends with "Batch fusion did not
converge". The recording has a long, clean stationary warm-up and no obvious
reason to fail. Finding the cause (the legacy gyro-scale correction changed the
solver's input, and with physically correct rates this recording no longer
settles) took a throwaway test build, a dump of the kernel's inputs, and a
comparison against another branch. Almost everything that investigation needed
already exists inside the fit while it runs: what data went in, which
stationary window initialized the attitude and what the alternatives were, how
the cost moved on every iteration, and where the residuals are large. On
failure all of it is thrown away; on success most of it is.

Two things are missing:

- **A place to see jobs.** The job model was built so that a dock could be a
  pure view of it. This is that dock.
- **Something worth seeing.** A job needs to carry a structured account of
  what the calculation did, whether it succeeded, was rejected, failed to
  converge, or was cancelled half way.

The guiding observation: a user deciding whether to keep waiting, and a user
(or developer) asking why a fit failed, need the same three answers. What went
in? How was it started? Was it getting anywhere?

## 2. Scope

In scope:

- A "Jobs" dock listing current and finished jobs from the existing job
  model, with a details pane for the selected job.
- A calculation-agnostic **details document** that a calculation may attach
  to its outcome, and that the job record keeps.
- Live details while a job runs, so progress is more than a line of text.
- Details for every way a job can end, including the ones that publish
  nothing.
- Sensor fusion's details: input audit, initialization (including the UTC
  range used and the scan that chose it), solver history, and result summary
  with residuals over time.
- Richer `_FUSION_DIAGNOSTICS`, including on rejection and solver failure.
- Navigation from a plot row to the job behind it.
- Automated tests and documentation.

Out of scope:

- Any change to the fusion model, initializer, tuning constants, convergence
  rule, or output values. Making `24-09-05/11-17-12` converge is later work;
  this feature is what will make that work tractable.
- User-adjustable initialization (markers bracketing the initialization
  range, refreshed on change). Section 8.4 says what must not be precluded.
- Starting work from the dock. The only gestures that start an explicit
  calculation remain the two on a plot row (predecessor section 9.3).
- Persisting jobs or details across restarts.
- More than one job running at a time.
- Time limits, stall detection, or any automatic judgement that a fit is "not
  going to converge". The dock shows the evidence; the user decides.
- Showing the initialization window on the plot or the map.

## 3. Details document

### 3.1 What it is

A details document is a structured, presentation-ready account of one run of
one calculation. It is calculation-agnostic: the dock renders any document
without knowing which calculation produced it. It consists of titled sections,
in order, each holding any of:

- **Labelled values**: a label, a value (text, number with unit, or UTC
  instant), and optionally a qualifier such as a limit the value was checked
  against and whether it passed.
- **UTC intervals**: a labelled start and end. Where the calculation works in
  seconds from an epoch, the document carries UTC; the dock may show both.
- **Tables**: named columns and rows of values, for things like solver
  iterations or a scan of candidates.
- **Series**: one or more named numeric series over a common axis (an index,
  seconds, or UTC), with axis labels and an indication of whether a
  logarithmic scale suits the data. For plotting.
- **Interval tracks**: a set of intervals on a UTC axis, each with a category
  (for example accepted, rejected, chosen) and a short annotation. For a
  timeline strip.
- **Notes**: a sentence of plain text, for a limitation or an explanation of
  why the run stopped.

The document also carries a one-line headline (for example "Did not converge
after 5 passes x 100 iterations") that the job list can show.

A document is plain data: immutable once produced, safe to copy between
threads by value, free of references to sessions, engines, or GUI objects.

### 3.2 Bounded size

Finished jobs are retained (up to the model's bound), so a document must be
small. A document is on the order of a hundred kilobytes at most, regardless
of recording length. Series are reduced to a bounded number of points by the
producer, in a way that preserves extremes (a residual spike must survive the
reduction). Tables are bounded likewise; a scan of hundreds of candidates is
summarized by counts plus the interesting rows, with the full scan expressed
as an interval track rather than a table.

### 3.3 Where it comes from

- A calculation's compute function may attach a details document to what it
  returns, for every outcome it can return (section 5).
- While computing, it may report **live details** through the existing
  progress facility (section 4).
- A calculation that attaches nothing still gets a minimal document made by
  the job queue from what it knows: times, state, reason, progress text.

Details are not an input to anything and not an output name. They never
affect a result, are never read by another calculation, and do not take part
in dependency tracking, caching decisions, or the read-order idempotency
guarantees of the engine specification.

### 3.4 Where it goes

The job record keeps the document of the run it represents: the final one if
the compute function returned one, otherwise the last live one. This is a
snapshot. It survives everything that can happen to the session afterwards:
input changes that drop the result, unloading, removal. It is exposed through
the job model like the record's other fields, and the model reports a change
when it updates.

The document of a run that the engine refused to publish (superseded) is kept
on the job record all the same: it describes work that was really done on the
inputs as they were.

## 4. Live details

The progress facility gains the ability to carry a details document alongside
progress text. The rule that makes the worker thread safe is unchanged and
must stay narrow: what crosses from the worker is handed over by value, and
the worker shares nothing else.

- Live documents are delivered to the main thread in order, like progress
  text, and a late one for a job that has ended is dropped.
- The calculation controls how often it reports. Reporting must be cheap
  relative to the work between reports, and must not change the result in any
  way, including by changing the order or number of floating-point operations
  in the numerical kernels.
- Cancellation boundaries are unchanged. Reporting details adds no new
  boundary and removes none.
- The synchronous `request()` path accepts and ignores live details.

## 5. Details for every ending

| Job ends as | Details kept on the record |
|---|---|
| Succeeded, result produced | The final document. |
| Succeeded, outputs unavailable for a reason that is a function of the inputs (rejection, solver failure) | The final document, which must say why and show how far the run got. |
| Succeeded with a calculation failure (unexpected exception) | The last live document, plus the failure text. |
| Cancelled while running | The last live document, marked as incomplete. |
| Cancelled while queued | The minimal document. |
| Superseded | The final or last live document, marked as not published. |
| Failed (environment) | The last live document, plus the reason. |

"Marked" means the headline and a note make the status unmistakable; a
half-finished solver table must never read as a finished one.

## 6. Sensor fusion details

The fusion calculation produces the following sections. Everything listed is
already computed inside the fit; none of it requires new numerical work beyond
reducing series to a bounded size. Times shown to the user are UTC.

### 6.1 Input

- GNSS: number of fixes, UTC range, median rate.
- IMU: number of samples, UTC range, median rate and median interval.
- Origin: index, latitude, longitude, height, and the rule that selected it.
- Fitted interval: UTC range, number of states, number of output samples.
- Gaps: largest IMU gap against its limit (1.6 median intervals), largest GNSS
  gap against its limit (the larger of 2 s and 5 median intervals).
- The height and time methods, as in the existing audit.
- **Provenance of the inputs that are not raw.** At minimum, whether the
  gyroscope rates were rescaled by the conversion layer and by what factor.
  This is the line that would have explained the prompting case at a glance.
  The fusion compute function is pure and cannot look this up; the facts must
  reach it as declared inputs or be added by the registration from declared
  inputs. Nothing may be read from a session, the registry, or preferences
  inside the compute function.

On rejection this section is filled as far as validation got, and the
violated rule is shown with the offending value and its limit (for example
the gap that was too long and where, in UTC).

### 6.2 Initialization

- Method (stationary window, or the coarse GNSS/force fallback).
- **The UTC range used for initialization**, the anchor instant, and the same
  in seconds from the start of the recording.
- For the chosen window: quietness score, measured gravity magnitude, initial
  gyro bias.
- The scan: number of candidate windows, how many were accepted, and for the
  rejected ones a count per failed gate, using the gate names the kernel
  already records.
- An interval track of all candidate windows over the recording: accepted,
  rejected, and the chosen one, each annotated with its score or its failed
  gates.

When the fallback was used, the section says that no window was accepted and
still shows the scan, which is the explanation.

### 6.3 Solver

- One table row per bias pass: iterations used, cost at the start and end of
  the pass, the size of the bias shift (accelerometer and gyro), and whether
  the pass settled or hit the iteration limit.
- A series of cost against cumulative iteration, with pass boundaries marked,
  suited to a logarithmic scale.
- The stopping rule in words, with the actual thresholds, and which part of
  it was not met when the fit did not converge.
- Elapsed time per pass.

While the job runs this section is live: the table and series grow as
iterations complete.

### 6.4 Result

- Objective, position and velocity residual RMS, accelerometer and gyro bias
  (with the norm of each), largest endpoint correction.
- Series of squared whitened residual against UTC for position, velocity, and
  IMU factors, reduced as in section 3.2.
- The limitations note carried by the existing diagnostics.

For a fit that did not converge this section describes the last iterate and
says plainly that nothing was published. It is the most useful part of a
failure: it shows where in the recording the model and the data disagree.

## 7. `_FUSION_DIAGNOSTICS`

The attribute remains the machine-readable record of a published result.

- On success it gains the solver history and the initialization scan. Every
  existing key keeps its name, meaning, and exact value.
- On rejection and solver failure it carries everything that was known when
  the run stopped (audit, initialization, history, last-iterate summary) in
  addition to the reason, instead of the reason alone.
- The details document and the attribute are two renderings of the same
  facts and must not disagree.

Golden parity is affected and the rule is explicit: the committed goldens were
captured from `sensor-fusion-clean-port`, which never emitted the new keys.
For the diagnostics JSON, parity now means *every key present in the golden is
present in the port's output with a byte-identical value*. New keys are
additions, checked by their own tests against literals or independently
derived values. Channel goldens, the fit trace, and the rejection reasons are
untouched and keep their exact comparison. No golden file is regenerated from
the port.

The comparison has two modes and both must keep passing: the portable bound
(`tests/README.md` section 11, widened from the numbers of the first CI run
on gcc and clang) on every platform, and bit-exact mode, which runs
automatically as the `_exact` CTest twins where the compiler matches
`tests/data/fusion/capture.json` (MSVC, Release). The key-subset rule above
applies to both modes.

## 8. Architectural boundaries

### 8.1 Layers

- The details document type and the extension to the progress facility live
  with the engine (Qt Core only, no threads, no locks).
- The job queue and job model carry documents and stay widget-free.
- Everything that decides *what the pane shows* for a given job (which
  sections, in what order, how a state is worded, what is marked incomplete,
  how times are formatted) lives in a widget-free component that is tested
  without a view, in the manner of the plot-request logic. The dock paints
  what that component reports and forwards clicks.
- Fusion's document is produced inside the fusion library. The dock, the job
  queue, and the engine contain nothing specific to fusion, and nothing
  outside the fusion library links GTSAM. The configure-time confinement check
  and the audit continue to pass unchanged.

### 8.2 Threading

One thread owns all state. The worker hands documents over by value. No lock
or atomic is added anywhere; the audit's "one atomic" and "no locks" rules
continue to hold.

### 8.3 Purity and parity

Producing details must not perturb results. With details reporting enabled,
every channel golden remains bit-identical in exact mode, the fit trace is
unchanged, and the progress-text sequence checked by the parity tests is
unchanged (live details ride along with existing reports or are delivered
separately, but the text sequence is part of the tested behavior). The
cancellation boundaries are: every 256 states of graph construction, every
optimizer iteration, and, silently (no progress text), before each candidate
window of the initialization scan. Details reporting adds none and removes
none; the silent scan poll must stay silent, because the tested text sequence
does not include it.

### 8.4 Room for adjustable initialization

Not built here, but the design must leave the door open: the range of the
recording the initializer may scan is a natural future *declared input* of the
fusion calculation (the reference branch's initializer took earliest and
latest bounds). If that input is added later, changing it invalidates the
result through ordinary dependency tracking and the plot row shows refresh,
with no new mechanism. The initialization section should therefore present the
scan range as a first-class fact ("scanned: whole recording") so that a later
bounded scan reads naturally.

## 9. The dock

### 9.1 Placement and lifetime

A dock named "Jobs", created and registered the way the existing docks are,
reachable from the same menu, remembered in layouts and profiles like any
other dock. Hidden by default in the default layout. It is a view over the
application's single job model; closing it affects nothing.

### 9.2 Job list

The model for this list is the download list of a web browser: a running
history of things the user asked for, newest first, each with its own state
and its own small set of actions, that the user tidies when they feel like
it. Nothing in it needs attention to keep the application working.

One row per job, current and finished, newest at the top. Per row: session name, calculation title, state, the headline
(or progress text while running), when it started, and how long it ran or has
been running (updating while it runs). States are distinguishable without
relying on colour alone. A succeeded job whose outputs are unavailable does
not look like a success: its row carries the same warning sense as the plot
row's badge.

Actions, all acting on existing job-queue operations:

- **Cancel** a queued or running job, from its row.
- **Remove** a finished job, from its row.
- **Clear jobs**: one always-visible control for the whole list. It removes
  every finished job, whatever its outcome, and leaves queued and running
  jobs alone, exactly as clearing a browser's download list leaves active
  downloads in place. It asks for no confirmation, is disabled when there is
  nothing to clear, and if the selected job is among those cleared the details
  pane returns to its "no selection" state.
- **Reveal**: make the job's session the current one in the application, if it
  still exists.

Removing or clearing a job discards its details document and nothing else: it
never touches a session, a published result, or a plot's check state, and a
cleared failure still shows its warning badge on the plot row (the row reports
the state of the calculation, not of the list). Navigation from a row whose
job has been cleared (section 9.4) opens the dock with nothing selected.

There is no run, retry, or refresh action in the dock.

### 9.3 Details pane

Shows the selected job's document: the headline and status, then the sections
in order. Labelled values as a form, tables as tables, series as plots using
the plotting component the application already uses, interval tracks as a
timeline strip with hover annotations. A value checked against a limit shows
the limit and is visibly flagged when it failed.

- The pane updates in place while the selected job runs, without losing the
  user's scroll position or plot zoom on every update.
- With no selection the pane says so. With a minimal document it shows what
  there is, without empty sections.
- Text in the pane can be selected and copied, and the whole document can be
  copied as text, so that a user can send us what they see.
- The pane remains correct and readable in both light and dark themes and at
  the display scalings the application supports.

### 9.4 Navigation from plot rows

A plot row's warning badge and its progress/cancel cluster gain a way to open
the Jobs dock with the relevant job selected (for a row with several, the most
recent failure, or the running job). This is navigation, not a gesture in the
sense of the predecessor's section 9.3: it starts nothing, cancels nothing,
and changes no check state. The existing click targets keep their meaning;
the cancel control still cancels. The row tooltip remains as it is.

### 9.5 Responsiveness

The dock must not make the application less responsive during a fit. Live
updates are coalesced to a rate the eye can use (a few per second). A pane
that is not visible does no rendering work.

## 10. Documentation

- User documentation for the Jobs dock: what the list shows, what each state
  means, how to read the fusion sections, and what to send us when a fit
  fails. `docs/COMPUTED_PLOTS.md` points to it.
- `docs/SENSOR_FUSION.md`: the diagnostics additions, and a short "when a fit
  does not converge" section describing what to look at.
- `docs/CALCULATIONS.md`: the details document, how a calculation attaches
  one, live details and the by-value rule, what the job record keeps.
- The test README: how the diagnostics parity rule changed (section 7).

## 11. Acceptance

1. With the dock closed or open, all existing tests pass unchanged except the
   diagnostics comparison of section 7, and channel goldens are bit-identical
   in exact mode with details enabled.
2. A synthetic explicit calculation that attaches a document with one section
   of each kind produces a job record whose document is equal to what was
   attached; a widget-free test renders the pane's content from the model
   alone.
3. A calculation that attaches nothing yields the minimal document.
4. Live details: documents reported during compute arrive in order on the
   main thread, the record always holds the latest, the model signals each
   change, and one reported after the job ended is dropped.
5. Each row of the table in section 5 is demonstrated with a synthetic
   calculation: cancelled mid-run keeps the last live document marked
   incomplete; superseded keeps its document marked not published; and so on.
6. The record's document survives an input change that drops the result,
   session unload, and session removal.
7. Details never influence results: the engine's read-order oracle passes
   with a calculation that reports details, and sync and async results remain
   identical.
8. Document size stays within the bound for a synthetic recording an order of
   magnitude longer than the fixtures, and a planted residual spike survives
   series reduction.
9. Fusion, success fixture: the four sections are present; the UTC
   initialization range, anchor, scan counts, per-pass rows, and result
   summary equal literals derived from the committed goldens.
10. Fusion, each rejection fixture: the input section names the violated
    rule with the offending value and limit, and later sections are absent
    rather than empty.
11. Fusion, forced non-convergence: the solver section shows every pass
    ending at the iteration limit, the stopping rule names the unmet part,
    the result section describes the last iterate and says nothing was
    published, and `_FUSION_DIAGNOSTICS` carries the same facts.
12. Fusion, fallback initializer fixture: the initialization section says no
    window was accepted and shows the scan.
13. The provenance line reports gyroscope rescaling for a legacy-schema
    session and its absence for a schema-2 session, and the fusion compute
    function still reads nothing but its declared inputs.
14. Every golden key of every committed diagnostics golden is present with a
    byte-identical value in the port's output.
15. Dock, tested without a real window where possible: rows track the model
    through its signals alone; cancel, remove, and reveal call the
    corresponding operations; "Clear jobs" with finished, queued, and running
    jobs present removes exactly the finished ones, is disabled when none is
    finished, resets the details pane when the selected job was cleared, and
    leaves sessions, results, check states, and plot-row badges untouched;
    there is no path from the dock to a job request (audited, like the
    existing gestures rule).
16. Navigation from a plot row opens the dock with the right job selected and
    starts nothing.
17. Nothing outside the fusion library links GTSAM; no lock or additional
    atomic exists; the dock and its logic contain nothing specific to fusion
    (audited).
18. Manual, on `24-09-05/11-17-12`: while the fit runs the dock shows the
    solver section growing and the application stays interactive; when it
    ends the dock shows a non-converged job with input provenance (gyro
    rescaled), the initialization range 11:18:22 to 11:18:52 UTC, the full
    pass table, and residuals over time; cancelling a second attempt half way
    leaves a record marked incomplete with the history so far.

## 12. Reference case

`TEMP/data/Data comp 5 - FS 2 - serie nr 2 - 014667 (test 10)/24-09-05/11-17-12`
(untracked; on Michael's machine). Facts established on 2026-09-21:

- On the baseline the fit does not converge: five passes of one hundred
  iterations, about seven minutes.
- Every kernel input is bit-identical to what `sensor-fusion-clean-port`
  feeds its kernel, except `IMU/wx`, `wy`, `wz`, which `master` multiplies by
  1.14688 for legacy-schema files. The reference branch predates that
  correction.
- A copy of the recording declaring `SCHEMA_VER` 2 converges on the baseline
  in 22 iterations and 25 seconds with objective `197341.2190770153`,
  identical to the reference branch: stationary window 70 to 100 s
  (11:18:22 to 11:18:52 UTC), anchor 85 s, 8860 states, 23449 outputs,
  position RMS 2.96 m, velocity RMS 0.97 m/s, accelerometer bias
  [0.143, -0.142, 0.075] m/s^2.
- Two recordings from the same jump (`.../comp 3 .../24-09-05/11-16-56`,
  `.../comp 6 .../24-09-05/11-17-04`) converge on both builds.

The schema-2 copy is a convenient way to see a converged and a non-converged
job for the same recording side by side. Real recordings are not in the
repository and are not a CI test.

## 13. Principles for the implementers

- The dock is a view. If the dock needs to know something, the job model
  carries it; if the job model needs it, the calculation reports it.
- Details are an account, not a result. They never feed back.
- The algorithm is still frozen. If showing something seems to need a change
  to the numerical kernels, stop and ask.
- The worker hands things over by value. Do not add sharing to make live
  details convenient.
- A failure deserves more detail than a success, not less.
- Never make a partial run look complete.
- Only a gesture on a plot row starts expensive work. The dock is not a
  second place to start it.
- Show UTC. Users think in the time of day the jump happened, and so do the
  other docks.

## 14. Notes for the planning coordinator

- `PLANS/implementation-plan/` holds the completed plan for the predecessor.
  `PLANS/` is untracked. Before writing the new plan, move that folder to
  `PLANS/done/sensor-fusion-jobs-plan/`.
- Copy the "Commit Policy" section below into `00-overview.md` as its own
  top-level section with that exact heading, in addition to including this
  specification in full. The implementation orchestrator looks for that
  section and does not commit without it.
- Most of this work needs no GTSAM: the details document, the progress
  facility, the job model, the pane logic, and the dock are exercised with
  small synthetic explicit calculations defined in the tests, as in the
  predecessor. Only section 6 and 7 need the fusion library.
- In the predecessor every phase edited the same few build and test files,
  which forced the phases to run one after another. If phases here can be
  shaped so that parallel tracks do not share files, do so; otherwise plan
  for sequential execution and say so.
- The predecessor's final review (`PLANS/sensor-fusion-jobs-final-review.md`)
  has been worked through; its remaining open items (a green CI run on macOS
  and Linux, retiring the "unverified" notes, tagging the golden reference
  commit) are not part of this specification. Two behaviours that the review
  added are facts this design builds on: a running job whose ticket the
  engine has marked stale is stopped at the next boundary and ends
  Superseded (the details document of such a run is kept, section 5), and
  the initialization scan is cancellable per candidate window (section 8.3).
  Two things the review deliberately left alone because this feature needs
  them: a non-converged fit still collects its last-iterate residuals, and
  `StationaryWindow` keeps its per-window sample counts.
- Local builds on Michael's machine: use `build-phase1/` (or a new
  `build-*/` directory) configured with
  `-DGTSAM_INSTALL_DIR=<repo>/build-solver-deps/GTSAM-install
  -DONETBB_INSTALL_DIR=<repo>/build-solver-deps/oneTBB-install`. Keep
  `build-solver-deps/`. Never build or reconfigure `build/`: it would
  overwrite the Boost-enabled GTSAM install under `third-party/`. Do not
  launch the real application against Michael's settings and logbook; manual
  steps are written down for him to run on a copied logbook.
- The capture of golden data is not repeated. The reference branch can still
  be read with `git show sensor-fusion-clean-port:<path>`; its built tools
  are in the untracked `build-port/`.

## Commit Policy

Michael has authorized commits for this plan on a new working branch, so that
each phase can be audited separately afterward. **The implementation
orchestrator makes every commit** (see "Version Control" in
`.claude/prompts/implementation-orchestrator.md`); implementation, revision,
and review agents never run git commands that change repository state. This
section overrides any wording about commits in the phase documents. Nothing is
ever pushed, and nothing is ever committed on `master` or on
`sensor-fusion-jobs`.

- **Branch.** Before spawning the first agent, the orchestrator confirms that
  the working tree has no modified tracked files (if there are any, stop and
  ask). The base is `master` if `master` contains commit `1f3139a`
  (`git merge-base --is-ancestor 1f3139a master`), otherwise
  `sensor-fusion-jobs`. It then creates the working branch with
  `git switch -c jobs-dock <base>`. All work and every commit for this plan
  happen on `jobs-dock`. If the branch already exists (a resumed run), switch
  to it instead; never recreate, reset, or rebase it.
- **Pre-existing untracked paths** are never staged: `PLANS/`, `TEMP/`,
  `experiments/`, `build*/`, `dist/`, `results/`, and the dependency source,
  build, and install trees under `third-party/` (`gtsam/`, `gtsam-build/`,
  `GTSAM-install/`, `oneTBB/`, `oneTBB-build/`, `oneTBB-install/`).
- **One commit per accepted phase**, made when the phase's review agent
  returns ACCEPT, containing exactly the files the phase's implementation and
  revision agents reported, staged by explicit path. Subject:
  `Phase N: <phase name>`. Body: a short summary of the phase's tasks, then
  the session's attribution line (`Co-Authored-By: ...`). Rejected iterations
  are never committed.
- **Tag.** After each phase commit the orchestrator creates the lightweight
  local tag `plan/jobs-dock/phase-N-done`. Tags are never moved or deleted.
- **Fixes to a closed phase** (found by a later phase, an integration
  debugging agent, or the final review) are committed separately as
  `Phase N fixup: <what>` once reviewed and accepted, under the number of the
  phase being fixed. A fix to the predecessor's code is committed as
  `sensor-fusion-jobs fixup: <what>`.
- **CI workflow edits** (`.github/workflows/*`) cannot be verified without a
  push. The agent reports those paths separately and the orchestrator commits
  them as `Phase N: CI (unverified)` right after the phase commit.
- **Escalated phases** are not committed. Their changes stay in the working
  tree, their paths are listed in the final report, and phases that depend on
  them are not started.

Auditing afterward:

```
git log --oneline <base>..jobs-dock
git show --stat plan/jobs-dock/phase-3-done
git log --oneline --grep='fixup' <base>..jobs-dock
```

## Appendix: illustrative details pane

Not a layout requirement; it shows the content of section 6 for the reference
case. Values marked (real) come from the converged schema-2 run; the solver
and result numbers for the non-converged run are invented, because today they
are discarded.

```
Sensor fusion - 24-09-05 11:17:12 (014667)                 Did not converge
Started 07:29:41 - finished 07:36:38 - ran 6 min 57 s

INPUT
  GNSS     8872 fixes    11:17:12.0 - 11:46:46.2 UTC    5.0 Hz        (real)
  IMU     23513 samples  11:17:09.7 - 11:46:46.3 UTC   13.2 Hz        (real)
  Gyroscope              legacy schema, rescaled x 1.14688            (real)
  Origin                 fix #12  52.2410451, 6.0443793, 2.9 m        (real)
  Fitted interval        11:17:14.4 - 11:46:46.2 UTC, 8860 states     (real)
  Largest gaps           IMU 0.081 s (limit 0.122), GNSS 0.2 s (limit 2.0)

INITIALIZATION
  Method                 stationary window; heading unknown           (real)
  Range used             11:18:22 - 11:18:52 UTC (70 - 100 s)         (real)
  Anchor                 11:18:37 UTC (85 s)                          (real)
  Scanned                whole recording: 349 windows, 9 accepted
  Rejected by            gyro_variability 212, force_variability 187, ...
  [timeline: accepted / rejected / chosen windows, hover for gates]

SOLVER
  Pass  Iterations  Cost start -> end    Bias shift acc / gyro   Settled
   1       100      8.41e6 -> 3.02e5     0.31    / 4.1e-3        no (limit)
   2       100      3.02e5 -> 2.61e5     0.08    / 9.0e-4        no (limit)
   ...
   5       100      2.44e5 -> 2.43e5     2.1e-3  / 3.0e-5        no (limit)
  Stopped: 5 passes without settling (needs relative cost decrease <= 1e-8
           and bias shift < 1e-5 m/s^2, < 1e-6 rad/s)
  [cost against iteration, log scale, pass boundaries marked]

RESULT  (last iterate; nothing was published)
  Objective 2.43e5, position RMS 3.4 m, velocity RMS 1.1 m/s
  Accelerometer bias [0.21, -0.19, 0.11] m/s^2 (norm 0.30)
  [squared whitened residual against UTC: position, velocity, IMU]
```
