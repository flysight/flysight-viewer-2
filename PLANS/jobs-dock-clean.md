# Jobs dock and calculation details

Date: 2026-09-21
Status: specification for planning. Not an implementation plan.
Baseline: branch `sensor-fusion-jobs` (or `master` once it has merged).
Predecessor: `PLANS/sensor-fusion-jobs.md`; its sections 7 to 9 and 12 remain
in force. Work happens on a new branch; see "Commit Policy".

This document says what the change must do and the boundaries it must
respect. It avoids class layouts and function signatures unless a name is part
of the observable contract. The planning agent decides phasing; the
implementation agent decides code structure.

## 1. Motivation

Explicit calculations run as background jobs started from plot rows. When one
fails, the user gets one sentence in a tooltip. Everything that would explain
the failure — what data went in, which stationary window initialized the
attitude, how the cost moved on every iteration, where the residuals are large
— exists inside the fit while it runs and is thrown away when it ends.

A user deciding whether to keep waiting, and a user or developer asking why a
fit failed, need the same three answers: what went in, how was it started, was
it getting anywhere.

## 2. Scope

In scope:

- A "Jobs" dock listing current and finished jobs, with a details pane for the
  selected job.
- A calculation-agnostic details document that a calculation may attach to its
  outcome and report live while it runs; the job record keeps it.
- Details for every way a job can end, including the ones that publish nothing.
- Sensor fusion's details: input audit, initialization (with the UTC range used
  and the scan that chose it), solver history, result summary with residuals
  over time.
- `_FUSION_DIAGNOSTICS` carrying the same facts, including on rejection and
  solver failure.
- Navigation from a plot row to the job behind it.
- Tests and documentation.

Out of scope:

- Any change to the fusion model, initializer, tuning, convergence rule, or
  outputs.
- User-adjustable initialization (markers bracketing the initialization range).
  Section 7.4 says what must not be precluded.
- Starting work from the dock. The only gestures that start an explicit
  calculation remain the two on a plot row.
- Persisting jobs or details across restarts.
- More than one running job.
- Automatic judgement that a fit is "not going to converge". The dock shows the
  evidence; the user decides.
- Showing the initialization window on the plot or map.

## 3. Details document

A details document is a structured, presentation-ready account of one run of
one calculation. It is calculation-agnostic: the dock renders any document
without knowing which calculation produced it. It is titled sections, in
order, holding any of:

- labelled values (text, number with unit, or UTC instant; optionally the limit
  the value was checked against and whether it passed);
- UTC intervals;
- tables;
- numeric series over an index, seconds, or UTC axis, with an indication of
  whether a logarithmic scale suits them, for plotting;
- interval tracks on a UTC axis, each interval with a category and a short
  annotation, for a timeline strip;
- notes: a sentence of plain text.

It also carries a one-line headline for the job list.

Rules:

- Plain data: immutable once produced, copied between threads by value, no
  reference to sessions, engines, or GUI objects.
- Bounded: on the order of a hundred kilobytes regardless of recording length.
  Series are reduced by the producer in a way that preserves extremes; large
  scans become counts plus an interval track, not a table.
- Not an input, not an output name: never affects a result, never read by
  another calculation, no part in dependency tracking, caching, or the
  engine's read-order guarantees.
- A calculation that attaches nothing gets a minimal document made by the job
  queue from what it knows (times, state, reason, progress text).

The job record keeps the final document, or the last live one if the compute
function returned none. It is a snapshot: it survives input changes that drop
the result, unloading, and removal. The document of a run the engine refused
to publish is kept all the same.

## 4. Live details

The progress facility gains the ability to carry a details document alongside
progress text. What crosses from the worker is handed over by value; the
worker shares nothing else, and no lock or atomic is added.

- Live documents arrive on the main thread in order; a late one for a job that
  has ended is dropped.
- The calculation decides how often to report. Reporting must not change the
  result in any way, including the order or number of floating-point
  operations in the numerical kernels.
- Cancellation boundaries are unchanged: reporting adds none and removes none.
- The synchronous `request()` path accepts and ignores live details.

## 5. Details for every ending

| Job ends as | Details kept |
|---|---|
| Succeeded, result produced | the final document |
| Succeeded, outputs unavailable for a reason that is a function of the inputs (rejection, non-convergence) | the final document: why, and how far the run got |
| Succeeded with a calculation failure (unexpected exception) | the last live document plus the failure text |
| Cancelled while running | the last live document, marked incomplete |
| Cancelled while queued | the minimal document |
| Superseded | the final or last live document, marked not published |
| Failed (environment) | the last live document plus the reason |

A partial run must never read as a complete one: the headline and a note make
the status unmistakable.

## 6. Sensor fusion details

Everything below is already computed inside the fit; nothing needs new
numerical work beyond reducing series to a bounded size. Times shown to the
user are UTC.

**Input.** GNSS and IMU: counts, UTC range, median rate. Origin: index,
position, and the rule that selected it. Fitted interval: UTC range, states,
output samples. Largest IMU and GNSS gaps against their limits. Height and time
methods. Provenance of inputs that are not raw: at minimum whether the
gyroscope rates were rescaled by the conversion layer and by what factor. The
compute function is pure and cannot look this up; the facts reach it as
declared inputs, or the registration adds them from declared inputs. On
rejection this section is filled as far as validation got, with the violated
rule, the offending value, and its limit.

**Initialization.** Method (stationary window or the coarse fallback). The UTC
range used and the anchor instant, and the same in seconds from the start of
the recording. For the chosen window: quietness score, measured gravity, initial
gyro bias. The scan: candidates, accepted, and for the rejected ones a count per
failed gate using the gate names the kernel records. An interval track of all
candidate windows: accepted, rejected, chosen, each annotated. When the
fallback was used, the section says no window was accepted and still shows the
scan. The scan range is a stated fact ("scanned: whole recording").

**Solver.** One row per bias pass: iterations used, cost at start and end, bias
shift (accelerometer and gyro), settled or hit the limit, elapsed time. A
series of cost against cumulative iteration with pass boundaries marked,
logarithmic. The stopping rule in words with the actual thresholds, and which
part was not met when the fit did not converge. This section is live while the
job runs.

**Result.** Objective, position and velocity residual RMS, biases with norms,
largest endpoint correction. Series of squared whitened residual against UTC
for position, velocity, and IMU factors. The existing limitations note. For a
fit that did not converge: the last iterate, stated plainly as not published.

**`_FUSION_DIAGNOSTICS`** stays the machine-readable record of a published
result. It gains the solver history and the initialization scan; every existing
key keeps its name, meaning, and exact value. On rejection and solver failure
it carries everything known when the run stopped, not the reason alone. The
attribute and the details document are two renderings of the same facts.

Golden parity rule: the committed goldens were captured from the reference
branch, which never emitted the new keys. For the diagnostics JSON, parity
means every key present in the golden is present with a byte-identical value;
new keys are additions checked by their own tests. Channel goldens, the fit
trace, and rejection reasons are untouched and keep their exact comparison, in
both the portable and the bit-exact test modes. No golden is regenerated from
the port.

## 7. Boundaries

### 7.1 Layers

- The details document type and the progress-facility extension live with the
  engine (Qt Core only, no threads, no locks).
- The job queue and job model carry documents and stay widget-free.
- Everything that decides what the pane shows for a job (sections, wording of
  states, incomplete markers, time formatting) lives in a widget-free component
  tested without a view. The dock paints what it reports and forwards clicks.
- Fusion's document is produced inside the fusion library. The dock, queue, and
  engine contain nothing specific to fusion; nothing outside the fusion library
  links GTSAM. The existing confinement check and audit keep passing.

### 7.2 Threading

One thread owns all state. The worker hands documents over by value. No lock
or atomic is added.

### 7.3 Purity and parity

With details reporting enabled, every channel golden remains bit-identical in
exact mode, the fit trace is unchanged, and the progress-text sequence the
parity tests check is unchanged. The cancellation boundaries are every 256
states of graph construction, every optimizer iteration, and a silent poll
before each candidate window of the initialization scan; the silent poll must
stay silent.

### 7.4 Room for adjustable initialization

Not built here. The range the initializer may scan is a natural future declared
input of the fusion calculation; changing it would then invalidate the result
through ordinary dependency tracking and the plot row would show refresh. The
design must not preclude that.

## 8. The dock

**Placement.** A dock named "Jobs", created, registered, and remembered in
layouts and profiles like the existing docks; hidden by default. A view over
the application's single job model; closing it affects nothing.

**Job list.** Modelled on a browser's download list: a history of things the
user asked for, newest first, tidied when the user feels like it. Per row:
session name, calculation title, state, the headline (or progress text while
running), when it started, how long it ran or has been running (live). States
are distinguishable without relying on colour alone; a succeeded job whose
outputs are unavailable carries the same warning sense as the plot row's
badge.

Actions, each acting through an existing job-queue operation:

- Cancel a queued or running job, from its row.
- Remove a finished job, from its row.
- Clear jobs: one always-visible control that removes every finished job and
  leaves queued and running jobs alone. No confirmation; disabled when there is
  nothing to clear; if the selected job is cleared the pane returns to "no
  selection".
- Reveal: make the job's session current, if it still exists.

Removing or clearing discards the details and nothing else: no session, result,
check state, or plot-row badge changes. There is no run, retry, or refresh
action in the dock.

**Details pane.** Headline and status, then the sections in order: values as a
form, tables as tables, series as plots with the plotting component the
application already uses, interval tracks as a timeline strip with hover
annotations, checked values visibly flagged when they failed their limit. Updates
in place while the selected job runs without losing scroll position or plot
zoom. No selection says so; a minimal document shows no empty sections. Text
can be selected and copied; the whole document can be copied as text. Correct
in both themes and at the supported display scalings.

**Navigation from plot rows.** A row's warning badge and its progress/cancel
cluster gain a way to open the dock with the relevant job selected (the most
recent failure, or the running job). This is navigation, not a gesture: it
starts nothing, cancels nothing, changes no check state. Existing click targets
keep their meaning. A row whose job has been cleared opens the dock with nothing
selected.

**Responsiveness.** Live updates are coalesced to a few per second; a pane that
is not visible does no rendering work. The application stays as responsive
during a fit as it is today.

## 9. Documentation

- User documentation for the Jobs dock; `docs/COMPUTED_PLOTS.md` points to it.
- `docs/SENSOR_FUSION.md`: the diagnostics additions and a "when a fit does not
  converge" section.
- `docs/CALCULATIONS.md`: the details document, how a calculation attaches one,
  live details, what the job record keeps.
- The test README: the diagnostics parity rule of section 6.

## 10. Acceptance

1. All existing tests pass, except the diagnostics comparison changed by the
   rule in section 6; channel goldens are bit-identical in exact mode with
   details enabled.
2. A synthetic explicit calculation attaching a document with one section of
   each kind yields a job record whose document equals what was attached; a
   widget-free test renders the pane's content from the model alone.
3. A calculation that attaches nothing yields the minimal document.
4. Live documents arrive in order; the record holds the latest; the model
   signals each change; one reported after the job ended is dropped.
5. Every row of the section 5 table is demonstrated with a synthetic
   calculation.
6. The record's document survives an input change that drops the result,
   session unload, and session removal.
7. Details never influence results: the engine's read-order oracle passes with
   a calculation that reports details; sync and async results stay identical.
8. Document size stays within the bound for a recording an order of magnitude
   longer than the fixtures; a planted residual spike survives reduction.
9. Fusion, success fixture: all four sections present; UTC initialization
   range, anchor, scan counts, per-pass rows, and result summary equal literals
   derived from the committed goldens.
10. Fusion, each rejection fixture: the input section names the violated rule
    with value and limit; later sections absent rather than empty.
11. Fusion, forced non-convergence: every pass ends at the iteration limit, the
    stopping rule names the unmet part, the result section describes the last
    iterate and says nothing was published, and `_FUSION_DIAGNOSTICS` carries
    the same facts.
12. Fusion, fallback initializer: the section says no window was accepted and
    shows the scan.
13. The provenance line reports gyroscope rescaling for a legacy-schema session
    and its absence for a schema-2 session; the compute function still reads
    nothing but declared inputs.
14. Every key of every committed diagnostics golden is present with a
    byte-identical value.
15. Dock, without a real window where possible: rows track the model through
    its signals alone; cancel, remove, reveal call the corresponding
    operations; Clear jobs removes exactly the finished jobs, is disabled when
    none is finished, resets the pane when the selected job was cleared, and
    leaves sessions, results, check states, and badges untouched; no path from
    the dock to a job request (audited).
16. Navigation from a plot row opens the dock with the right job selected and
    starts nothing.
17. Nothing outside the fusion library links GTSAM; no lock or additional
    atomic; the dock and its logic contain nothing specific to fusion
    (audited).
18. Manual, on the reference case (section 11): while the fit runs the solver
    section grows and the application stays interactive; when it ends the dock
    shows a non-converged job with gyro provenance, the initialization range
    11:18:22 to 11:18:52 UTC, the full pass table, and residuals over time;
    cancelling a second attempt half way leaves a record marked incomplete.

## 11. Reference case

`TEMP/data/Data comp 5 - FS 2 - serie nr 2 - 014667 (test 10)/24-09-05/11-17-12`
(untracked, on Michael's machine). On the baseline the fit does not converge:
five passes of one hundred iterations, about seven minutes. Every kernel input
is identical to what the reference branch fed its kernel except the gyro
channels, which the conversion layer rescales by 1.14688 for legacy-schema
files. A copy declaring `SCHEMA_VER` 2 converges in 22 iterations with
objective `197341.2190770153`: window 70 to 100 s (11:18:22 to 11:18:52 UTC),
anchor 85 s, 8860 states. The two copies give a converged and a non-converged
job for the same recording side by side.

## 12. Principles

- The dock is a view. If it needs to know something, the job model carries it;
  if the model needs it, the calculation reports it.
- Details are an account, not a result. They never feed back.
- The algorithm is frozen. If showing something seems to need a change to the
  numerical kernels, stop and ask.
- The worker hands things over by value. Do not add sharing to make live
  details convenient.
- A failure deserves more detail than a success, not less.
- Never make a partial run look complete.
- Only a gesture on a plot row starts expensive work.
- Show UTC.

## Commit Policy

Michael has authorized commits for this plan on a new working branch. The
implementation orchestrator makes every commit; implementation, revision, and
review agents never run git commands that change repository state. Nothing is
ever pushed, and nothing is ever committed on `master` or `sensor-fusion-jobs`.
Copy this section into the plan overview as its own top-level section with
this exact heading.

- **Branch.** Confirm the working tree has no modified tracked files (if there
  are any, stop and ask). Base: `master` if it contains `sensor-fusion-jobs`,
  otherwise `sensor-fusion-jobs`. Create with `git switch -c jobs-dock <base>`;
  if it exists, switch to it; never recreate, reset, or rebase it.
- **Never staged:** `PLANS/`, `TEMP/`, `experiments/`, `build*/`, `dist/`,
  `results/`, and everything under `third-party/` that is not tracked.
- **One commit per accepted phase**, when the review agent returns ACCEPT,
  containing exactly the files the phase's agents reported, staged by explicit
  path. Subject `Phase N: <phase name>`; body a short summary, then the
  session's attribution line. Rejected iterations are never committed.
- **Tag** each phase commit `plan/jobs-dock/phase-N-done`; tags are never moved
  or deleted.
- **Fixes to a closed phase** are committed separately as
  `Phase N fixup: <what>` once reviewed and accepted; a fix to predecessor code
  as `sensor-fusion-jobs fixup: <what>`.
- **CI workflow edits** are committed separately as `Phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.
