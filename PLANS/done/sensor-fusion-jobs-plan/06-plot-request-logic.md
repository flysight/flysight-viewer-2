# Phase 6: Plot request logic

## Overview

`PlotRequests` is the widget-free component behind the plot list's rows (spec
section 9.1-9.5). For every checked plot that is backed by an explicit
calculation it classifies each visible, loaded track as available / missing /
pending / failed / not applicable from the engine's blocker inspection
(Phase 4) and the job queue (Phase 5), aggregates that into a per-plot
`PlotRowState` (counts, progress, a structured and a ready-made tooltip), and
is the only place in the application that turns a gesture into job requests:
a direct check, a refresh press, chained continuation after a requested job
succeeds, cancel, and pruning of queued jobs nobody wants. It lives in
`flysight_core` (Qt Core + Gui, no Widgets, no GTSAM) and is tested with
synthetic explicit calculations; Phase 7 only paints what it reports and
forwards clicks to it (overview Decisions 8 and 9).

## Dependencies

- **Depends on:** Phase 5 (Job queue and job model), and through it Phase 4
  (Engine: asynchronous request and blocker inspection).
- **Blocks:** Phase 7 (Plot list view and application wiring), Phase 10.
- **Assumptions:**
  - Phase 4's API exists exactly as in `04-engine-async-request-and-blockers.md`:
    `CalculationEngine::blockers(DependencyKey)` -> `BlockerReport`
    (`Available` / `Blocked` / `NotProduced` / `NotApplicable`, `blockers`,
    `notProduced`), `CalculationBlocker` (`registrationId`, `instanceOutput`,
    `instanceId`, `title`), `UnproducedNote` (`calculation`, `status`,
    `detail`), and the guarantee that inspection never runs an explicit
    calculation and never changes what a later read returns. A calculation
    appears in `BlockerReport::blockers` only when its own readiness is
    `Ready`, so every reported blocker is requestable.
  - Phase 5's API exists exactly as in `05-job-queue-and-job-model.md`:
    `JobQueue::request(sessionId, CalculationBlocker)` -> `RequestResult`,
    `activeJobs()`, `job(id)`, `cancel(id)`, `cancelUnwantedQueued(isWanted)`,
    the signals `jobQueued` / `jobStarted` / `jobProgress` /
    `jobCancelRequested` / `jobFinished` / `jobsChanged` / `idle`, `JobRecord`,
    `JobState`, and the end-of-job order of `endJob()`: model transition, then
    `SessionModel::dependencyChanged` for the published names, then
    `jobFinished` / `jobsChanged`, and only then `idle()` if nothing is active.
    Slots connected to the queue's signals may call `request()` and `cancel()`.
  - `tests/support/jobfixture.h` (`JobWorld`, `Gate`, `waitIdle`) and
    `Synthetic::registerExplicitWorld()` (`expA`, `derivA`, `derivA2`, `expB`,
    `derivB`) exist as documented in Phases 4 and 5.
  - `SessionModel` is as on `master` plus Phase 5's additions. "Visible, loaded
    track" means a row with `isLoaded() && visible`, which is exactly the set
    `PlotWidget::updatePlot()` draws (`src/ui/docks/plot/PlotWidget.cpp` lines
    513-526).
  - `PlotModel` (`src/plotmodel.h` / `.cpp`) depends on Qt Core, `QSettings`,
    and `plotregistry.h` only (verified: no Widgets include, no application
    header).
  - Shared files (`src/CMakeLists.txt`, `tests/CMakeLists.txt`,
    `docs/CALCULATIONS.md`, `tests/README.md`, `tests/acceptance_map.txt`) get
    small, additive, separate hunks (overview Decision 10).
  - Commits are made by the orchestrator only (overview "Commit Policy");
    report the exact list of files created and modified.

## Design summary (read before the tasks)

### Classes and files

| Type | File | Role |
|---|---|---|
| `PlotTrackCondition`, `PlotTrackState`, `PlotRowState` | `src/plotrequests.h` | The vocabulary the view paints from: one track's condition, one row's complete state. Plain values. |
| `PlotRequests : QObject` | `src/plotrequests.h` / `.cpp` | Inspection, classification, aggregation, gestures, continuation, cancel, pruning. |
| `PlotModel` | `src/plotmodel.h` / `.cpp` (unchanged source; **moved from the application target into `flysight_core`**) | The store of plot check state the component observes. |
| `PlotFixture` | `tests/support/plotfixture.h` / `.cpp` | Synthetic plots over `JobWorld`'s explicit calculations. |

File and class names follow `master`'s flat lower-case convention next to
`sessionmodel.cpp`, `jobqueue.cpp`. Everything is in `namespace FlySight`.
The style model is `src/ui/docks/legend/LegendPresenter.cpp`: a `QObject`
that listens to models, coalesces with a zero-interval single-shot `QTimer`
(lines 95-99, 143-149), recomputes plain values, and hands them on.

### Decision: `PlotModel` moves into `flysight_core` (overview Decision 2)

`PlotModel` is a `QAbstractItemModel` over `PlotValue` with optional
`QSettings` persistence; it needs nothing outside Qt Core/Gui and
`plotregistry.h`, which is already in `flysight_core`. Moving its two source
lines between targets is a smaller and safer change than a new interface plus
an application-side adapter, and it has three concrete advantages: the
component observes the very object `PlotWidget`, `LegendPresenter`, and the
measure tool read (`enabledPlots()`), so "checked" can never disagree between
what is drawn and what the rows report; the tests exercise the real
programmatic paths (`setPlotEnabled()` used by `applyProfile()`,
`src/profilestatebridge.cpp` line 167; `setPlots()` with settings restore,
`src/plotmodel.cpp` lines 101-103; `setData()`), which is what acceptance 16
is about; and Phase 7 needs no glue. `plotmodel.h` / `.cpp` are not edited.

### Public API (part of the contract for Phase 7)

```cpp
// plotrequests.h --------------------------------------------------------------
enum class PlotTrackCondition { Available, Missing, Pending, Failed, NotApplicable };

struct PlotTrackState {                      // one visible, loaded track for one plot
    QString            sessionId;
    QString            sessionName;          // live: _DESCRIPTION, else the session id
    PlotTrackCondition condition = PlotTrackCondition::NotApplicable;
    QStringList        calculationTitles;    // Missing / Pending: the blockers' titles; Failed: the calculations that did not produce
    QString            reason;               // Failed only; never empty for a failed track
    JobId              job = 0;              // Pending only: the running job if this track waits on it, else its oldest queued job
    JobState           jobState = JobState::Queued;   // Pending only: Queued or Running
    QString            jobProgressText;      // Pending + Running only: the job's latest progress text
    bool operator==(const PlotTrackState &) const;
};

struct PlotRowState {
    enum class Control { None, Cancel, Refresh };

    QString plotId;                          // "<sensorID>/<measurementID>" == PlotModel::PlotValueIdRole
    bool    explicitBacked = false;          // false: the row is never inspected and looks exactly as today

    int pendingCount = 0;                    // tracks currently pending (the count that falls as jobs publish)
    int missingCount = 0;                    // tracks currently missing
    int failedCount  = 0;                    // tracks currently failed
    int waitingTotal = 0;                    // tracks the row is or was waiting for in the current episode
    int waitingDone  = 0;                    // ... of which are no longer waiting (available, failed, or not applicable)

    QString progressLabel;                   // tr("%1 of %2").arg(waitingDone).arg(waitingTotal); empty unless control() == Cancel
    QString jobProgressText;                 // progress text of the running job when this row waits on it; else empty

    QList<PlotTrackState> pending;           // structured tooltip data, in session-model row order
    QList<PlotTrackState> missing;
    QList<PlotTrackState> failed;
    QString toolTip;                         // ready-made plain text; empty when isPlain()

    Control control() const;                 // Cancel if pendingCount > 0, else Refresh if missingCount > 0, else None
    bool    showsWarning() const;            // failedCount > 0, independent of control()
    int     controlCount() const;            // Refresh: missingCount; Cancel: pendingCount; None: 0
    bool    isPlain() const;                 // nothing pending, missing, or failed: paint exactly today's row
    bool operator==(const PlotRowState &) const;
};

class PlotRequests : public QObject {
    Q_OBJECT
public:
    PlotRequests(SessionModel *sessionModel, PlotModel *plotModel, JobQueue *jobQueue,
                 QObject *parent = nullptr);
    ~PlotRequests() override;                // removes the registry observer

    static QString plotId(const QString &sensorId, const QString &measurementId);
    static QString plotId(const PlotValue &plot);

    /// The last computed state. A default state (isPlain(), explicitBacked
    /// false) for an unchecked plot, a plot not backed by an explicit
    /// calculation, and an unknown id.
    PlotRowState rowState(const QString &plotId) const;

    // ---- gestures: explicit calls from the view, never inferred (Decision 9) ----
    /// The user checked this plot by direct interaction with its row. Call
    /// AFTER the check state has been written to the PlotModel. Returns the
    /// number of jobs created. No-op (0) when the plot is not checked.
    int plotCheckedByUser(const QString &plotId);
    /// The user pressed the row's refresh control. The same request.
    int refreshPressed(const QString &plotId);
    /// The user pressed the row's cancel control. Returns the number of jobs
    /// cancelled or asked to cancel. The plot stays checked.
    int cancelPressed(const QString &plotId);

    /// Runs a pending recomputation now (tests; the view never needs it).
    void flush();
    bool hasPendingUpdate() const;

signals:
    void rowStateChanged(const QString &plotId);   // rowState(plotId) differs from what it was
    void rowStatesChanged();                       // once per pass in which at least one row changed
};
```

All members are main-thread only and must not be called from inside a
calculation or an engine callback (they call `blockers()`, which asserts that
no evaluation is open, and `JobQueue::request()`).

### Which public name a plot needs - decision: the y name only

A plot needs `DependencyKey::measurement(pv.sensorID, pv.measurementID)`. The
x-axis name (`sensorID` / `PlotViewSettingsModel::xVariable()`) is **not**
inspected: the x variable is a per-view setting owned by an application
source (`src/plotviewsettingsmodel.h`), so taking it would drag view state
into the core; and for every sensor produced by an explicit calculation the
time axes are produced by the same calculation (`Fusion/_time` is an output
of the fit, `Fusion/_system_time` is derived on demand from it), so a track
whose y is available has its x available, and a track whose y is blocked
gets its x from the same job. "Available" in this phase therefore means "the
y name is available". Phase 9 must keep this true for the fusion sensor (see
Gotchas).

### "Backed by an explicit calculation": static, memoized, exact

A plot is *explicit-backed* when some name in
`CalculationRegistry::instance().staticDependencies(name).names` (which
includes the name itself) has a candidate
(`candidatesFor(n)`) whose `descriptor->policy == EvaluationPolicy::Explicit`.
The closure looks through source conversions (`sourceConversionsFor` for
measurements, as `staticDependencies` itself walks them). In one sentence, the
binding definition: **any name in the static dependency closure has a
candidate with explicit policy, looking through source conversions.** Phase 9
Task 9.5 defines the same predicate privately in `sessionmodel.cpp`
(`dependsOnExplicitCalculation`) and must agree with this one; Phase 10
Task 10.2 unifies the two if that is small (overview, Integration Note 10).

- It is a pure function of the registrations: no engine, no session, no
  calculation runs. The registry memoizes `staticDependencies()`; the
  component memoizes the boolean per plot id in a `QHash<QString, bool>` and
  the closure's names per plot id (`QHash<QString, QSet<DependencyKey>>`), and
  drops both from a registry observer (`CalculationRegistry::addObserver`,
  removed in the destructor). The observer only clears the memos and
  schedules a recomputation (an observer must not register or unregister).
- It is exact, not a heuristic: `staticDependencies()` is documented as a
  superset of any dynamic dependency set, and a blocker is always reached
  through declared inputs, so a plot that is not explicit-backed can never
  report a blocker or a "not produced" note. Such plots are **never
  inspected**: no `blockers()` call, no engine creation, no read. Their
  `PlotRowState` is the default value and no signal is ever emitted for them.
  Ordinary plots therefore cost one hash lookup and look exactly as today.
- The memo is computed lazily, only for checked plots.

### What inspection may cost - the bound

`blockers(name)` starts with an ordinary top-level read of `name` and may
compute cheap on-demand inputs of the explicit calculation (for fusion: the
effective GNSS / IMU channels, the time fit, the local origin). The component
calls it only for **(checked and explicit-backed plots) x (visible and loaded
tracks)**. Those are names `PlotWidget` reads for the same tracks in the same
event-loop pass anyway, plus the inputs the job would capture. It is never
called for unchecked plots, plots that are not explicit-backed, hidden tracks,
stubs, or rows with `loadFailed`. It is reached through
`SessionModel::loadedSession()` under a `RowStabilityGuard` - a plain read
that never loads, evicts, or touches the LRU; the component never calls
`sessionRef()`.

There is **no classification cache**. Every pass re-inspects; the engine's own
caches make a repeated `blockers()` a handful of hash lookups, and passes are
coalesced to at most one per event-loop iteration. A cache keyed on
`dependencyChanged` would be wrong in a subtle way (after A publishes, the
blocker of B's output changes from A to B although B's output itself may not
be re-announced) and is not needed.

### Classifying one track

`inspect(plot, session)` runs under the guard and returns plain values (the
`BlockerReport` and the session's display name), so that nothing is requested,
cancelled, or emitted while a guard is held.

Live jobs are taken from the queue, never from `activeJob()`: at the start of
every pass the component builds `QHash<QPair<QString, QString>, JobRecord>`
keyed `(sessionId, instanceId)` from `activeJobs()` + `job(id)`, **skipping
every job with `state == Running && cancelRequested`** (Phase 5's rule: such a
job is not pending). When both a cancel-requested running job and a newer
queued job exist for one key, the queued one is the live job.

| `BlockerReport::state` | Condition |
|---|---|
| `Available` | **Available** |
| `Blocked`, at least one blocker has a live job | **Pending**; `job` = the running live job among them, else the one with the smallest id |
| `Blocked`, every blocker is in the refused set (below) | **NotApplicable** |
| `Blocked`, otherwise | **Missing** |
| `NotProduced` | **Failed**; `reason` from the notes (below) |
| `NotApplicable` | **NotApplicable** |

`Blocked` wins over `NotProduced` exactly as Phase 4 defines it; notes on a
`Blocked` report are ignored. A track that was computed and later invalidated
reports `Blocked` again and is simply missing (spec 9.2) - there is no "stale"
state. A failed track whose inputs change reports `Blocked` and is refreshable.

Failure reason: for each note, `"<title>: <detail>"`; when `detail` is empty,
`tr("Calculation failed")` for `ResultStatus::Failed`, otherwise
`tr("No result for this recording")`. Several notes are joined with `"; "`.

**The refused set (acceptance 11).** A session without the inputs reports
`NotApplicable` and is never missing, so nothing is ever requested for it.
As a second line of defence, when `request()` answers `MissingInput`,
`NothingToDo`, or `UnknownCalculation` for a blocker, the pair
`(sessionId, instanceId)` is put into `m_refused`, and a track all of whose
blockers are refused is **not applicable, never missing** - otherwise the row
would show a refresh control that can never do anything. Entries for a
session are dropped on any `dependencyChanged` of that session; the whole set
is dropped on `SessionModel::modelReset` and on a registry change.

### Aggregating a row

For a checked, explicit-backed plot, over the visible loaded tracks in
session-model row order:

- `pending` / `missing` / `failed` lists and their counts; available and
  not-applicable tracks appear nowhere (spec: "silently absent").
- `control()`: `Cancel` if any track is pending; else `Refresh` if any is
  missing; else `None`. `showsWarning()` is independent of both.
- `jobProgressText`: the `progressText` of the queue's `runningJob()` when
  that job is the `job` of one of this row's pending tracks (or any live job
  among that track's blockers); otherwise empty. Two rows waiting on the same
  job therefore carry identical text (spec 9.2, last paragraph).
- Progress counts come from the waiting set (next section).
- `toolTip` (plain text, `tr()`, sections omitted when empty, one line per
  track, two-space indent):

  ```
  Computing (1 of 3 done):
    <session name> - <titles>: <job progress text>
    <session name> - <titles>: queued
  Not computed (press refresh to compute):
    <session name>
  Could not be computed:
    <session name> - <reason>
  ```

  A running job without progress text yet shows `tr("running")`; `<titles>`
  is `calculationTitles.join(", ")`. Tests compare the untranslated literals.
- Every other plot (unchecked, not explicit-backed, unknown) has the default
  state. A plot that stops being inspected is reset to the default state and
  announced once.

After a pass the new state of each plot is compared with the stored one
(`operator==`); `rowStateChanged(plotId)` is emitted for each difference and
`rowStatesChanged()` once if there was any. Nothing is emitted for an
unchanged row.

### Request bookkeeping: the waiting set

Per plot id the component keeps `QHash<QString /*sessionId*/, bool
/*continues*/>` - "the tracks the row is waiting for":

- A gesture inserts, with `continues = true`, every visible track of the plot
  that is Missing and got at least one job (`Created` or `AlreadyActive`) or
  that is already Pending. (Pending tracks are included so that a plot checked
  by the user while another row's job is running still continues *its own*
  chain.)
- A pass inserts, with `continues = false`, every track it observes Pending
  that is not in the set yet (a row checked programmatically while another
  row's jobs run waits on them too, but continues nothing).
- A pass removes entries whose session is no longer a visible loaded track,
  and **clears the whole set when the plot has no pending track** (the
  episode is over; the next gesture starts a new denominator).
- `waitingTotal` = entries; `waitingDone` = entries whose track is now
  Available, Failed, or NotApplicable. A waited-for track that fell back to
  Missing (superseded, cancelled from another row) counts as not done.
  Invariant while `control() == Cancel`: `waitingTotal >= pendingCount >= 1`
  and `waitingDone + pendingCount <= waitingTotal`.
- The set is cleared on `cancelPressed()` for that plot and when the plot is
  unchecked; a hidden, unloaded, or removed session is removed from every
  plot's set at once (not only at the next pass), so no continuation can be
  issued for it.
- `continues` is what authorizes chained continuation, and only that. It is
  set by gestures only and reset as described below.

### What starts work: exactly two private call paths

`JobQueue::request()` is called from exactly two private functions:

1. `requestMissing(plotId)` - shared by `plotCheckedByUser()` and
   `refreshPressed()` (spec 9.3: "the same request"):
   1. Return 0 unless the plot is checked **in the `PlotModel`**
      (`isPlotEnabled`, read directly, not from the cached snapshot) and
      explicit-backed.
   2. Build the live-job index; inspect every visible loaded track under one
      guard; release the guard.
   3. For each `Blocked` track: for each blocker without a live job and not
      refused, `request(sessionId, blocker)`. `Created` / `AlreadyActive`:
      the track has a job. `MissingInput` / `NothingToDo` /
      `UnknownCalculation`: add to `m_refused`. `Blocked`, `SessionNotLoaded`,
      `ShuttingDown`: nothing (the track stays as inspection reports it).
   4. Update the waiting set as above, run the pass synchronously (so the
      view sees "pending" before the call returns), return the number of
      `Created` results.
   It is a one-shot request, not a standing order: nothing is remembered that
   could request again later except `continues`.
2. `continueAfter(job)` - chained continuation (spec 9.3 last paragraph),
   called **synchronously from the `jobFinished` slot** when `state ==
   Succeeded`. For the job's session `S` and every checked, explicit-backed
   plot whose waiting set has `S` with `continues == true`: inspect `(plot,
   S)`; if `Blocked`, request every blocker without a live job exactly as in
   step 3; if afterwards the track has no live job (or the report was not
   `Blocked`), set `continues = false`. Because this runs inside
   `JobQueue::endJob()` between `jobFinished` and the idle check, the queue
   never reports `idle()` between the links of a chain, and
   `FlySightTest::waitIdle()` waits for the whole chain. The engine state is
   current at that point: the publication's `dependencyChanged` precedes
   `jobFinished`.

A job that does **not** succeed never continues and never re-requests: in the
`jobFinished` slot for `Cancelled` / `Superseded` / `Failed`, and in the
`jobCancelRequested` slot, every plot's entry for the job's session gets
`continues = false` unless that track still has another live job among its
blockers (inspect to find out; when the session is gone, drop the entry).
The track is then missing and the refresh control returns (spec 8.2
"Superseded", 9.3).

Nothing else calls `request()`. In particular none of these do: the
`PlotModel` `dataChanged` / `modelReset` slots (startup restore, profiles,
the Plots menu, any programmatic check), `visibilityChanged`, `sessionLoaded`,
`modelChanged`, `dependencyChanged`, the registry observer, `jobsChanged`,
the recomputation pass, `rowState()`, and `flush()`.

### Cancel (spec 9.4)

`cancelPressed(plotId)`: build the live-job index, inspect the plot's visible
tracks, collect the ids of every live job among their blockers (a set - one
job may serve several tracks only through several plots, but collect first in
any case because `cancel()` re-enters the component through `jobFinished`),
clear the plot's waiting set, then `cancel(id)` each. Run the pass
synchronously. The `PlotModel` is never written. Queued jobs end at once; the
running job becomes cancel-requested and, by the live-job rule, its tracks are
Missing immediately, for this row **and for every other row waiting on the
same job**, which changes identically because all rows are derived from the
same queue state. Other rows' `continues` flags are reset by the
`jobFinished` / `jobCancelRequested` slots.

### Jobs nobody wants (spec 9.5)

`pruneUnwantedQueued()` is called synchronously when a plot becomes unchecked
(or disappears from the `PlotModel`) and when `visibilityChanged` reports a
non-empty `hidden` set, and does nothing unless the queue holds at least one
`Queued` job. It inspects all (checked, explicit-backed plot) x (visible,
loaded track) pairs, collects `needed` = the `(sessionId, instanceId)` of every
reported blocker, and calls
`cancelUnwantedQueued([&](const JobRecord &r) { return needed.contains({r.sessionId, r.instanceId}); })`.
The queue never offers the running job to the predicate, so it finishes and
its result is published and cached (spec 9.5). Because the unchecked plot's
waiting set was cleared first, that job's success continues nothing for it.
A queued job still needed by another checked plot on a visible track
survives, whoever requested it: "wanted" is derived from checked plots x
visible tracks; gesture bookkeeping is already reflected in it because a plot
must be checked to hold any. Removal of sessions needs no pruning here: the
queue supersedes those jobs itself (Phase 5 "Abandonment").

### When state is recomputed

One private `scheduleUpdate()` (zero-interval single-shot `QTimer`, the
`LegendPresenter` pattern) and one `recompute()` pass. Triggers:

| Source | Signal | Extra synchronous work before scheduling |
|---|---|---|
| `JobQueue` | `jobsChanged` | none |
| `JobQueue` | `jobFinished` | continuation or `continues` reset (above) |
| `JobQueue` | `jobCancelRequested` | `continues` reset |
| `JobQueue` | `jobProgress` | **no pass**: text-only update (below) |
| `SessionModel` | `dependencyChanged(id, key)` | ignored unless `key` is in the union of the static names of the checked explicit-backed plots (`m_relevantNames`, rebuilt when the checked set or the registry changes); drops `m_refused` entries of `id` |
| `SessionModel` | `visibilityChanged(shown, hidden)` | hidden: remove from waiting sets, prune |
| `SessionModel` | `modelChanged`, `sessionLoaded` | none (`sessionLoaded` matters because the background loader announces visibility only at the end of a batch, `src/sessionmodel.cpp` lines 49-90, 1567-1593) |
| `SessionModel` | `modelReset` | clear `m_refused`; waiting sets are reconciled by the pass |
| `PlotModel` | `dataChanged` (any roles containing `Qt::CheckStateRole`, or empty roles), `modelReset` | diff `enabledPlots()` against the stored checked set: unchecked or vanished plots lose their waiting set and trigger a prune; newly checked plots do nothing but schedule |
| registry observer | - | clear memos and `m_refused` |

`recompute()` returns early, resetting any non-default stored state, when no
checked plot is explicit-backed - it does not even enumerate sessions.

`jobProgress(id, text)`: for each stored row state with a pending track whose
`job == id`, update that track's `jobProgressText`, the row's
`jobProgressText`, and `toolTip`, and emit. No inspection. (Optimizer
iterations can arrive many times per second.)

`flush()` stops the timer and runs the pass if one is pending. Gestures and
`cancelPressed()` always run the pass before returning.

## Tasks

### Task 6.1: Move `PlotModel` into `flysight_core`

**Purpose:** Give the widget-free component and its tests the real plot check-state store (Decision 2).

**Files to modify:**
- `src/CMakeLists.txt` - remove the line `plotmodel.cpp              plotmodel.h` from `PROJECT_SOURCES` (line 349) and add it to the `flysight_core` source list directly after the `plotregistry` line (line 278). Task 6.2 adds its own line in the same hunk.

**Technical Approach:**
- No source edit: `plotmodel.h` includes `<QAbstractItemModel>`, `<QSettings>`, `plotregistry.h` only. `flysight_core` already has AUTOMOC for `Q_OBJECT` classes (`sessionmodel.h`, `momentmodel.h`), and its include directory is inherited from `flysight_model` (`PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"`), so every application include of `"plotmodel.h"` keeps working.
- Do not move `plotviewsettingsmodel.*`, `plotrangemodel.*`, or anything else.

**Acceptance Criteria:**
- [ ] `flysight_core`, every test target, and the application build; `plotmodel.cpp` is compiled exactly once (in `flysight_core`).
- [ ] `git diff --stat` for `src/plotmodel.h` and `src/plotmodel.cpp` is empty.
- [ ] `flysight_core` still links Qt Core and Gui only.

**Complexity:** S

---

### Task 6.2: Vocabulary, explicit-backed determination, inspection, and classification

**Purpose:** The read-only half of the component: decide cheaply which plots matter and classify one track (spec 9.2, acceptance 11).

**Files to create:**
- `src/plotrequests.h` / `src/plotrequests.cpp` - the types and class of the design summary.

**Files to modify:**
- `src/CMakeLists.txt` - `plotrequests.cpp           plotrequests.h` in `flysight_core`, directly after the `plotmodel` line of Task 6.1 (same hunk).

**Technical Approach:**
- Header includes `jobmodel.h` (for `JobId`, `JobState`), `plotregistry.h`, `dependencykey.h`, and forward-declares `SessionModel`, `PlotModel`, `JobQueue`. No widget header. `Q_DECLARE_METATYPE` is not needed (signals carry `QString` only).
- Members: `QPointer<SessionModel> m_sessionModel`, `QPointer<PlotModel> m_plotModel`, `QPointer<JobQueue> m_jobQueue`; `QTimer m_updateTimer`; `QHash<QString, PlotValue> m_checked` (the checked-set snapshot, by plot id); `QHash<QString, bool> m_explicitBacked`; `QHash<QString, QSet<DependencyKey>> m_staticNames`; `QSet<DependencyKey> m_relevantNames`; `QHash<QString, PlotRowState> m_states`; `QHash<QString, QHash<QString, bool>> m_waiting`; `QSet<QPair<QString, QString>> m_refused`; `int m_registryObserver`. Every null `QPointer` makes the component inert (default states, gestures return 0), as `LegendPresenter::recompute()` does.
- `plotId()` is `sensorId + "/" + measurementId`; the class comment states that it must equal `PlotModel::PlotValueIdRole` (a test asserts it). Sensor or measurement ids are never parsed back out of a plot id: the `PlotValue` comes from `m_checked`.
- Private helpers, each small and named for its intent: `bool isExplicitBacked(const PlotValue &)` (memoized, the rule of the design summary; use `candidatesFor()` only - source conversions are never explicit); `LiveJobs liveJobs() const` (the index with the cancel-requested rule); `struct Track { QString sessionId; QString sessionName; }`; `QList<Track> visibleTracks() const` and `BlockerReport inspectLocked(const SessionData &, const PlotValue &)`, both used only inside one function that takes `m_sessionModel->stableRows()`, walks `rowAt(i)` for `isLoaded() && visible && !loadFailed` in row order (the loop of `PlotWidget.cpp` lines 513-526), reads the name by the rule of `LegendPresenter.cpp` lines 360-362 (`SessionKeys::Description`, else the session id), calls `session.calculationEngine().blockers(DependencyKey::measurement(...))`, and returns plain values; `PlotTrackState classify(const Track &, const BlockerReport &, const LiveJobs &) const` (the table of the design summary, including the refused rule and the reason text).
- The guard is released before anything is emitted, requested, or cancelled. `blockers()` may compute on-demand values; that is permitted under a guard (it reads, it does not load or evict) and is what `JobQueue::request()` already does with `readiness()`.
- Class comment: the five conditions, the bound on inspection, "only gestures start work" with the two call paths named, and the recomputation triggers, at the comment density of `src/sessionmodel.h`.

**Acceptance Criteria:**
- [ ] `isExplicitBacked` is true for a plot whose name is two on-demand levels above an explicit output, false for a plot over a stored attribute; it changes when the explicit calculation is unregistered / registered (registry observer), without a session existing.
- [ ] For a plot that is not explicit-backed, and for an unchecked plot, no `blockers()` call is made and no calculation runs: with only such plots checked, `CalculationEngine::totalRunCount()` of every session is unchanged by any number of passes, and `rowStateChanged` is never emitted.
- [ ] A session without the explicit calculation's input classifies as NotApplicable for every explicit-backed plot (acceptance 11); a hidden session, a stub row, and a `loadFailed` row are not tracks at all.
- [ ] A track whose job is `Running && cancelRequested` classifies as Missing; with an additional queued job for the same key it classifies as Pending with `job` = the queued job.
- [ ] The component never emits `SessionModel::sessionLoaded` (it never loads a session), and the LRU order is unchanged by a pass.
- [ ] `plotrequests.*` includes no Widgets header; `flysight_core` links Qt Core + Gui only.

**Complexity:** M

---

### Task 6.3: Row aggregation, waiting set, tooltip, change signals, and recomputation triggers

**Purpose:** Turn track conditions into the row state the view paints, and keep it current cheaply (spec 9.2).

**Files to modify:**
- `src/plotrequests.h` / `src/plotrequests.cpp`.

**Technical Approach:**
- `recompute()`: (1) refresh `m_checked` from `m_plotModel->enabledPlots()`; (2) the explicit-backed subset, early return as described; (3) `liveJobs()`; (4) one guarded inspection of all pairs; (5) classify; (6) reconcile `m_waiting` per plot (remove non-tracks, add observed-pending with `continues = false`, clear when `pendingCount == 0`); (7) build each `PlotRowState` including `progressLabel`, `jobProgressText`, `toolTip`; (8) reset stored states of plots no longer inspected; (9) diff, store, emit `rowStateChanged` per changed plot, then `rowStatesChanged()` once.
- `buildToolTip(const PlotRowState &)` is a static pure function (unit-testable by literals); the layout of the design summary is normative.
- Connect the triggers of the table "When state is recomputed" in the constructor; `scheduleUpdate()` / `flush()` / `hasPendingUpdate()` as `LegendPresenter::scheduleUpdate()`. The constructor schedules one initial pass (rows restored at startup show their refresh control without any event).
- `onPlotCheckStateChanged()` diffs the ids of `enabledPlots()` against `m_checked`; it is the single handler for `dataChanged` and `modelReset` of the `PlotModel`. It updates `m_checked` and `m_relevantNames` immediately (so a gesture that follows in the same call stack sees the plot as checked), clears waiting sets of unchecked plots, calls the prune of Task 6.5 when something was unchecked, and schedules.
- `onDependencyChanged()` filters by `m_relevantNames`; `onJobProgress()` is the text-only path.
- `PlotRowState::control()`, `showsWarning()`, `controlCount()`, `isPlain()`, and both `operator==` are inline in the header.

**Acceptance Criteria:**
- [ ] With three fusable visible tracks and one job running, one queued, one published: `pendingCount == 2`, `waitingTotal == 3`, `waitingDone == 1`, `progressLabel == "1 of 3"`, `control() == Cancel`, and the tooltip equals the literal text of the design summary's layout.
- [ ] A row with missing tracks and no pending ones has `control() == Refresh`, `controlCount() == missingCount`; a row with a failed track and nothing else has `control() == None`, `showsWarning()`, `failedCount == 1`, and the reason in `failed[0].reason` and in the tooltip; failed plus pending shows both.
- [ ] A row with nothing pending, missing, or failed has `isPlain()`, an empty `toolTip`, and equals a default `PlotRowState` except for `plotId` / `explicitBacked`.
- [ ] `rowStateChanged` is emitted only for rows whose state differs; a burst of N `dependencyChanged` signals in one event-loop pass causes exactly one pass (instrumented by a private pass counter exposed to the test through a `friend` test class or a `passCount()` test seam documented as such).
- [ ] `jobProgress` changes `jobProgressText` and the tooltip of every row waiting on that job and performs no inspection (the pass counter does not move).
- [ ] An irrelevant `dependencyChanged` (a key outside every checked explicit-backed plot's static names) schedules nothing.

**Complexity:** M

---

### Task 6.4: Gestures and chained continuation

**Purpose:** The only two ways work starts, as one-shot requests, plus continuation of chained blockers without another gesture (spec 9.3, acceptance 13, 16 logic).

**Files to modify:**
- `src/plotrequests.h` / `src/plotrequests.cpp`.

**Technical Approach:**
- `plotCheckedByUser()` and `refreshPressed()` both call `requestMissing()`; implement it and `continueAfter()` exactly as numbered in "What starts work". Collect the `(sessionId, blocker)` pairs under the guard, release it, then call `request()`.
- `onJobFinished(id, state)`: read `m_jobQueue->job(id)` first (retention trimming happens after the signal, Phase 5 `endJob()` step 5); dispatch to `continueAfter()` or to `stopContinuing(sessionId)`; then `scheduleUpdate()`. `onJobCancelRequested(id)` calls `stopContinuing()`.
- `request()` results other than `Created` / `AlreadyActive` are handled per the design summary; the refused set is the only state a refusal leaves behind.
- Re-entrancy: `request()` emits `jobQueued` / `jobsChanged` synchronously, which only schedules a pass. `continueAfter()` runs inside `JobQueue::endJob()`; it must not call `cancel*()` or `shutdown()` and must not spin an event loop.

**Acceptance Criteria:**
- [ ] `plotCheckedByUser()` on a checked plot with three missing visible tracks returns 3 and the queue holds three jobs; on a plot that is not checked in the `PlotModel`, on a plot that is not explicit-backed, and with a null queue it returns 0 and creates nothing.
- [ ] A second gesture while the jobs are active creates no job (returns 0) and leaves the counts unchanged (acceptance 14 seen from the row).
- [ ] With `expA` -> `expB` and a plot over `derivB`'s value, one `plotCheckedByUser()` runs `expA` then `expB`, each once, without another call; `JobQueue::idle` is emitted exactly once, after `expB` ended; the track is Pending throughout and Available at the end (acceptance 13).
- [ ] When the first job of a chain ends Cancelled, Superseded, or Failed, no second job is created; the track is Missing.
- [ ] A chain for a track that was hidden, or whose plot was unchecked, while `expA` ran is not continued: `expA` publishes, no `expB` job is created.
- [ ] `git grep -n "request(" src/plotrequests.cpp` shows `JobQueue::request` called from `requestMissing` and `continueAfter` only.

**Complexity:** M

---

### Task 6.5: Cancel and pruning of unwanted queued jobs

**Purpose:** Spec 9.4 and 9.5.

**Files to modify:**
- `src/plotrequests.h` / `src/plotrequests.cpp`.

**Technical Approach:**
- `cancelPressed()` and `pruneUnwantedQueued()` as in the design summary. Both snapshot what they need under the guard, release it, and only then call into the queue.
- `pruneUnwantedQueued()` first checks for a `Queued` record among `activeJobs()` and returns without inspecting when there is none.
- Neither function writes to the `PlotModel` or to `SessionModel`.

**Acceptance Criteria:**
- [ ] `cancelPressed()` with one running and two queued jobs of the row: returns 3; the two queued jobs are Cancelled at once; the running one is cancel-requested; **before the worker has returned** the row already has `pendingCount == 0`, `missingCount == 3`, `control() == Refresh`; `PlotModel::isPlotEnabled()` is still true.
- [ ] A second checked plot waiting on the same jobs changes to the same counts in the same pass.
- [ ] A refresh pressed while the cancelled job is still winding down creates a new queued job and the track is Pending (Phase 5's "cancel-requested does not deduplicate").
- [ ] Unchecking a plot (programmatically or not) with one running and two queued jobs ends the two queued jobs Cancelled with reason "No longer needed", leaves the running job Running, and that job then ends Succeeded with its value readable.
- [ ] Hiding one of three tracks removes that track's queued job only; hiding the track of the running job does not cancel it.
- [ ] A queued job still needed by another checked plot on a visible track survives the uncheck of the plot that requested it.
- [ ] No plot is unchecked by any function of the component (`git grep -n "setPlotEnabled\|togglePlot\|setData" src/plotrequests.cpp` finds nothing).

**Complexity:** M

---

### Task 6.6: Test support - synthetic plots over the job fixture

**Purpose:** Plots need measurement names; `JobWorld`'s calculations produce attributes. A small fixture bridges them with on-demand calculations, which also makes every row test exercise "sees through on-demand intermediates".

**Files to create:**
- `tests/support/plotfixture.h` / `tests/support/plotfixture.cpp`.

**Files to modify:**
- `tests/CMakeLists.txt` - add the two files to `flysight_test_support`, one line after `jobfixture` (Phase 5).

**Technical Approach:**
- `namespace FlySightTest`. `class PlotFixture` is constructed after `JobWorld` and destroyed before it; it registers on `CalculationRegistry::instance()` and unregisters in its destructor. All bridges are `OnDemand`, take one attribute input, and output a one-element measurement under sensor `Syn`:

  | Bridge id | Input attribute | Output | Value | Behind it |
  |---|---|---|---|---|
  | `plotG` | `G_OUT` | `Syn/g` | `{G_OUT}` | `gated` |
  | `plotG2` | `G_OUT` | `Syn/g2` | `{G_OUT * 2}` | `gated` (second plot on the same job) |
  | `plotDB` | `DB` | `Syn/db` | `{DB}` | `derivB` <- `expB` <- `expA` (chain) |
  | `plotEA` | `EA1` | `Syn/ea` | `{EA1}` | `expA` (rejects `EA_IN < 0`, reason "negative input") |
  | `plotT` | `T_OUT` | `Syn/t` | `{T_OUT}` | `thrower` ("synthetic failure") |
  | `plotPlain` | `P_IN` (stored) | `Syn/plain` | `{P_IN}` | nothing explicit |
  | `plotH` | `H_OUT` | `Syn/h` | `{H_OUT}` | `afterG` <- `gated` (a chain whose first link can be held in the gate) |

  plus one **explicit** calculation of the fixture's own: `afterG` (title "After G", policy `Explicit`, input attribute `G_OUT`, output attribute `H_OUT = G_OUT + 1`, pure, no gate).

  Literals for `G_IN = 4`: `Syn/g = {5}`, `Syn/g2 = {10}`, `Syn/h = {6}`; for `EA_IN = 4`, `EB_IN = 10`: `Syn/db = {19}`, `Syn/ea = {5}`.
- `static QVector<FlySight::PlotValue> plots()` returns the seven `PlotValue`s (category "Synthetic", `plotName` = the measurement id, `role = Dependent`); tests pass them to `PlotModel::setPlots()`.
- Helpers: `void show(SessionModel &, const QStringList &ids, bool visible = true)` (through `setRowsVisibility()`, the application's path), `void giveInput(SessionModel &, const QString &id, const QString &key, double value)` (through `updateAttribute()`).
- If `JobWorld` exposes its calculations' output names differently from Phase 5's table (`<X>_OUT`), follow `jobfixture.h`; do not change `jobfixture.*`.

**Acceptance Criteria:**
- [ ] After `PlotFixture` is destroyed, `CalculationRegistry::instance().registeredIds()` equals its value before construction.
- [ ] The fixture compiles into `flysight_test_support` without Widgets and contains no sleep.

**Complexity:** S

---

### Task 6.7: `tst_plot_requests`

**Purpose:** Prove acceptance 13, 15, the logic half of 16, the row half of 11, and every criterion of Tasks 6.2-6.5 without widgets.

**Files to create:**
- `tests/tst_plot_requests.cpp` - class `PlotRequestsTest`; fixture modelled on `tst_jobqueue` (Phase 5 Task 5.6): `registerBuiltIns`, `useFreshLogbook`, `resetPreferencesToDefaults`, registry snapshot compared in `cleanup()`. `init()` builds, in this order, `JobWorld`, `PlotFixture`, `SessionModel`, `JobQueue`, `PlotModel` (`setPlots(PlotFixture::plots())`), `PlotRequests`; `cleanup()` destroys them in reverse (`PlotRequests`, `PlotModel`, `JobQueue`, `SessionModel`, `PlotFixture`, `JobWorld`).

**Files to modify:**
- `tests/CMakeLists.txt` - after Phase 5's two lines: a comment "Plot request logic (row state, gestures, continuation, cancel, pruning; no widgets)" and `flysight_add_test(tst_plot_requests SOURCES tst_plot_requests.cpp LIBS Threads::Threads)`.

**Technical Approach:**
- Every test asserts "nothing started" with both `QSignalSpy(&queue, &JobQueue::jobQueued).isEmpty()` and an unchanged `queue.model()->rowCount()`.
- Synchronization: `gate.waitEntered()`, `gate.open(n)`, `QTRY_COMPARE(queue.job(id).state, ...)`, `FlySightTest::waitIdle(queue)`, then `requests.flush()` before reading `rowState()`. No sleeps. Expected counts, labels, reasons, and tooltips are literals (tests/README.md section 8).
- Test functions:
  - `plotIdMatchesPlotModelRole`.
  - `ordinaryPlotsAreNeverInspected`, `uncheckedPlotsAreNeverInspected` (Task 6.2).
  - `rowScript` - **the acceptance-15 script**, sessions `s1..s4` each with `G_IN = 4`, plot `Syn/g`, all values literal:
    1. show `s1`, `s2`, `s3`; `setPlotEnabled("Syn", "g", true)`; flush: `missingCount 3`, `control Refresh`, no job. `plotCheckedByUser("Syn/g")` returns 3: three jobs (`s1` running, held in the gate; `s2`, `s3` queued); `pendingCount 3`, `progressLabel "0 of 3"`, `control Cancel`.
    2. `gate.open(1)`: `s1` Succeeded; `pendingCount 2`, `"1 of 3"`; `Syn/g` of `s1` reads `{5}`; `s2` is now held in the gate.
    3. uncheck (`setPlotEnabled(..., false)`): `s3`'s job is Cancelled ("No longer needed"), `s2`'s is still Running; the row state is default. `gate.open(1)`: `s2` Succeeded, `Syn/g` of `s2` reads `{5}`; queue idle.
    4. check again with `plotCheckedByUser`: returns 1 (only `s3` is missing); `pendingCount 1`, `"0 of 1"`. `cancelPressed("Syn/g")` returns 1 while the job is held: at once `isPlotEnabled` true, `pendingCount 0`, `missingCount 1`, `control Refresh`; then the job ends Cancelled, `Syn/g` of `s3` is unavailable, nothing was published.
    5. `refreshPressed` returns 1; `gate.open(1)`; idle: row `isPlain()`.
    6. show `s4`: `missingCount 1`, `control Refresh`, `missing[0].sessionId == "s4"`, no job created, queue idle, and still none after spinning the event loop (`QTest::qWait(0)` twice plus `flush()`).
    7. `refreshPressed` returns 1; `gate.open(1)`; idle: `isPlain()`, `Syn/g` of `s4` reads `{5}`.
  - `chainedBlockersContinue` (acceptance 13): plot `Syn/db`, one session with `EA_IN 4`, `EB_IN 10`; one gesture; job model history is exactly `[expA Succeeded, expB Succeeded]`; `idle` spy count 1; `Syn/db == {19}`; `runCount` of each is 1.
  - `heldChainContinues`: plot `Syn/h`; one gesture; while `gated` is held the track is Pending; `gate.open(1)`; `afterG` runs without another call; `Syn/h == {6}`; `idle` spy count 1.
  - `chainStopsWhenFirstJobDoesNotSucceed` (data rows on plot `Syn/h`, acting while `gated` is held: `cancelPressed`; `queue.cancel(id)` from outside the component; `updateAttribute(G_IN)` so the job ends Superseded): no `afterG` job is ever created, the track is Missing with `control Refresh`, and a later `refreshPressed` completes the whole chain.
  - `chainNotContinuedForHiddenTrackOrUncheckedPlot` (data rows on `Syn/h`: hide the track / uncheck the plot while `gated` is held; `gated` Succeeds and publishes `G_OUT`; no `afterG` job; after re-showing / re-checking programmatically the row shows `missingCount 1`).
  - `programmaticCheckStartsNothing` (`setPlotEnabled`, `togglePlot`, and `setData(CheckStateRole)` - none is a gesture for the component), `profileStyleApplyStartsNothing` (the loop of `applyProfile()`: `setPlotEnabled` over all plots), `startupRestoreStartsNothing` (a `QSettings` with `state/plots/Syn/g = true`, `setSettings`, `setPlots`: the plot comes up checked through `modelReset`; row shows `Refresh`), `showingTrackStartsNothing`, `loadingSessionStartsNothing` (a visible stub row loaded through the model's load path; pattern: stub rows in `tests/tst_session_model_engine.cpp`), `mergeStartsNothing`, `inputInvalidationStartsNothing` (publish, then `updateAttribute(G_IN)`: the track is Missing again, `missingCount 1`, no job - also covers "invalidated after publish becomes missing again"), `supersededJobStartsNothing` (edit `G_IN` while the job is held: Superseded; track Missing; no new job) - together acceptance 16 (logic) and spec 9.3's list.
  - `sessionWithoutInputIsNeverListed` (acceptance 11, row half): three sessions, one without `G_IN`; before, during, and after the jobs of the other two that session appears in none of `pending` / `missing` / `failed`; the gesture returns 2; `request()` for it directly returns `MissingInput`.
  - `gestureNeverRequestsTheUnrequestable`: Phase 4 guarantees that a reported blocker is requestable, so the refused set is a second line of defence that no natural scenario reaches; no production seam is added to reach it. Instead assert the first line: in a mixed scenario (sessions with input, without input, already computed, already rejected) the gesture's return value equals the number of `jobQueued` signals and equals the literal number of requestable tracks, and the sessions without input or with a cached result are in none of the row's lists or in `failed` respectively.
  - `failedBadgeAndReason`: `Syn/ea` with `EA_IN = -1`: after the job Succeeded, `failedCount 1`, `showsWarning()`, `control None`, reason `"Explicit A: negative input"`; refresh is not offered and `refreshPressed` returns 0; after `EA_IN = 4` the track is Missing, and a refresh computes `{5}`. Second row: `Syn/t`: reason `"<title of thrower>: synthetic failure"`.
  - `failedAndPendingTogether`.
  - `sharedJobSameProgress`: `Syn/g` and `Syn/g2` both checked by gesture (second returns 0); identical `pendingCount`, `progressLabel`, `jobProgressText == "step 1"`; one job per session; `cancelPressed` on one row changes both identically and leaves both checked.
  - `rowCheckedProgrammaticallyDuringJobShowsPending` (`continues == false` path: shows progress, continues nothing).
  - `cancelThenRefreshWhileWindingDown`, `uncheckPrunesQueuedKeepsRunning`, `hidePrunesOnlyThatTrack`, `queuedJobNeededByOtherPlotSurvives` (Task 6.5).
  - `tooltipText` (literal, three sections), `changeSignalsAreMinimal`, `dependencyBurstIsCoalesced`, `progressUpdatesWithoutInspection` (Task 6.3).
  - `removedSessionLeavesNoTrace` (remove a session with pending tracks: job Superseded by the queue, row counts fall, no crash), `registryChangeReclassifies` (unregister the explicit calculation: the plot stops being explicit-backed and its row becomes default), `survivesQueueShutdown` (after `queue.shutdown()` gestures return 0 and nothing crashes), `nullCollaborators`.

**Acceptance Criteria:**
- [ ] Every function above exists and passes in Debug and Release on Windows, 50 consecutive runs, well inside the 120 s timeout.
- [ ] The test links `flysight_test_support` and `Threads::Threads` only - no Widgets - and `cleanup()` verifies the global registry and `enrolledEngineCount() == 0`.
- [ ] Acceptance 11 (row half), 13, 15, and 16 (logic) are each demonstrated by a named function recorded in the acceptance map (Task 6.8).

**Complexity:** L

---

### Task 6.8: Documentation and traceability

**Purpose:** Keep the engine documentation, test README, and acceptance map in step with append-only edits.

**Files to modify:**
- `docs/CALCULATIONS.md` - append one section (own hunk), numbered 16 whether or not sections 12-15 of Phases 4 and 5 have landed yet; Phase 7 extends this same section 16 (overview, Integration Note 2): **16. Plot-driven requests** - the five track conditions and how each is derived from `blockers()` and the job model; the row aggregation; "explicit-backed" and why ordinary plots cost nothing; the two gestures as explicit calls and the complete list of things that are not gestures; chained continuation and when it stops; cancel; pruning; the y-name-only rule and what it requires of an explicit sensor's time axes; the public API of `PlotRequests` with its main-thread rule. User-facing documentation of the controls is Phase 10.
- `tests/README.md` - executable count (add 1 to the current count, whatever it is), one row in the test table, one sentence on `plotfixture.h` in section 8.
- `tests/acceptance_map.txt` - append comment-only lines to the `# SFJ` block (Phase 4's convention; if the block is absent because earlier map edits are not committed yet, create its header exactly as Phase 4's document gives it, which is Phase 2's form, 02 Task 2.7). The `# SFJ n` comment lines are converted to audited `100 + n` lines by Phase 10 (overview, Integration Note 1):

  ```
  # SFJ 11 tst_plot_requests sessionWithoutInputIsNeverListed
  # SFJ 13 tst_plot_requests chainedBlockersContinue
  # SFJ 15 tst_plot_requests rowScript
  # SFJ 16 tst_plot_requests startupRestoreStartsNothing
  # SFJ 16 tst_plot_requests profileStyleApplyStartsNothing
  # SFJ 16 tst_plot_requests programmaticCheckStartsNothing
  ```

**Acceptance Criteria:**
- [ ] `audit_cleanup` passes.
- [ ] Every public member of `PlotRequests`, `PlotRowState`, and `PlotTrackState` is named in `docs/CALCULATIONS.md` section 16.
- [ ] Edits to the three shared files are append-only apart from the README count and row.

**Complexity:** S

## Testing Requirements

### Unit Tests
- New: `tst_plot_requests` (Task 6.7), support `plotfixture` (Task 6.6).
- Unchanged and passing: everything else, in particular `tst_jobqueue`, `tst_jobmodel`, `tst_calcengine_blockers`, `tst_session_model_engine`, `tst_workflow`.

### Integration Tests
- `tst_plot_requests` is the integration of `PlotRequests` + `PlotModel` + `JobQueue` + `SessionModel` + real `SessionData` engines + the global registry, driven only through public APIs and the application's own edit paths (`setRowsVisibility`, `updateAttribute`, `mergeSessions`, `removeSessions`, `setPlotEnabled`, `setPlots`).
- Build the application target: `PlotModel` changed library, and nothing constructs a `PlotRequests` yet (Phase 7 wires it). The application must behave exactly as before.

### Manual Verification
- `cmake --build build --config Release`, then `ctest --test-dir build -C Release -L core --output-on-failure`.
- Run `tst_plot_requests` 50 times in a loop in Debug and Release.
- Start the application once: the plot list, plot checking, profiles, and the Plots menu behave as before (only a library boundary moved).
- `git grep -n "QtWidgets\|QWidget\|QTreeView\|gtsam\|QMutex\|std::mutex\|std::atomic" src/plotrequests.*` finds nothing.

## Notes for Implementer

### Gotchas
- Release the `RowStabilityGuard` before every `request()`, `cancel()`, `cancelUnwantedQueued()`, and emit. `loadedSession()` / `rowAt()` references die with the guard: copy the `BlockerReport`, ids, and names out.
- Never call `sessionRef()`: it loads. Visible stubs are simply not tracks until the model has loaded them; `sessionLoaded` schedules the pass that picks them up.
- `JobQueue::idle` fires in `endJob()` after `jobFinished`; continuation must therefore be synchronous in the `jobFinished` slot, or tests (and Phase 7's status display) see a false idle between chain links. Do not move it into the deferred pass.
- `cancel()` of a queued job emits `jobFinished` synchronously and re-enters `onJobFinished()`. Collect job ids before cancelling; do not iterate `m_waiting` or `m_states` across a call into the queue.
- `job(id)` must be read at the top of `onJobFinished()`; after the slot returns the record may be trimmed.
- `PlotModel::dataChanged` arrives before the view's gesture call, so `m_checked` is already current; `requestMissing()` nevertheless re-reads `isPlotEnabled()` so that a wrong call order in Phase 7 degrades to "nothing requested", never to "requested for an unchecked plot".
- `PlotModel::setPlots()` resets the model; plots can disappear. Treat a vanished plot as unchecked.
- The registry observer is invoked synchronously, possibly in the middle of plugin loading; it may only clear memos and start the timer.
- Do not cache classifications across passes (see "What inspection may cost").
- `QSet<QPair<QString, QString>>` needs no custom hash in Qt 6; `DependencyKey` has `qHash` in `src/dependencykey.h`.
- For Phase 9: every time axis of a sensor produced by an explicit calculation must be produced by that calculation or derived on demand from its outputs, so that "y available implies x available" holds for the y-name-only rule.
- For Phase 7: (a) call `plotCheckedByUser()` only for a direct interaction with the row that resulted in **checked**, after the model was written; the Plots menu and its shortcuts (`MainWindow::togglePlot`, `src/mainwindow.cpp` lines 1156-1165, 1314-1320), profiles, and startup restore are not gestures ("when in doubt, it is not", spec 12); (b) paint from `rowState(plotId)` with `plotId` = `PlotModel::PlotValueIdRole`, repaint on `rowStateChanged` / `rowStatesChanged`; an `isPlain()` row must be painted by the unmodified base delegate; (c) to silence the "No data available" warning for uncomputed values, ask the engine (`blockers(name).state` is `Blocked` or `NotProduced`) at the warning site rather than `rowState()`, which may be one event-loop pass behind; (d) create `PlotRequests` after the `JobQueue` and destroy it before the queue.

### Decisions Made
- **Name `PlotRequests`**, files `src/plotrequests.*`: it is neither an item model nor a view presenter; it owns the requests plots make.
- **`PlotModel` moves into `flysight_core`** unchanged (rationale in the design summary) instead of a narrow interface.
- **The y name only is inspected**; the x-axis setting stays in the application.
- **Explicit-backed is static registry reachability**, memoized, exact by the superset property of `staticDependencies()`; computed in the component from public registry API, so `src/engine/*` is untouched.
- **No classification cache; coalesced passes.** Correctness over micro-optimization; `jobProgress` has a text-only fast path.
- **Live jobs are derived from `activeJobs()`**, not `activeJob()`, so the cancel-requested rule and the "newer queued job wins" case are decided in one visible place.
- **One waiting set per plot, with a `continues` flag**, serves both progress denominators and continuation. Tracks observed pending without a gesture join the set for progress only. The set ends with the episode (no pending track), on cancel, and on uncheck. This refines the assignment's "(session, target name)" pairs: the target name is the plot's own name, so the session id suffices.
- **Continuation is synchronous in `jobFinished`**; only `Succeeded` continues; `Cancelled` / `Superseded` / `Failed` / cancel-requested reset `continues`.
- **A gesture also adopts already-pending tracks** (`continues = true`), so a plot checked by the user while another row's job runs completes its own chain.
- **Pruning is derived from checked plots x visible tracks**, runs synchronously on uncheck and hide, and never considers who requested a job. A future non-plot requester would have to be added to the predicate; none exists in this plan.
- **Refused requests make a track not applicable** until that session changes (acceptance 11's second line of defence).
- **Hidden-then-reshown tracks do not regain continuation**: the track shows a refresh control after the running job publishes, if blockers remain.
- **Failed tracks offer no retry**; `refreshPressed()` on a row without missing tracks requests nothing.
- **Tooltip is plain text built in the component** so Phase 7 adds no wording and tests can pin it.
- **Acceptance map lines are comments**, following Phases 4 and 5; Phase 10 reconciles the scheme.

### Open Questions
- None blocking. The refused set (acceptance 11's second line of defence) is not reachable by a natural scenario because Phase 4 guarantees that reported blockers are requestable; it is covered by review, not by a test seam (`gestureNeverRequestsTheUnrequestable` tests the first line).

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria
2. All tests pass (`ctest -L core`, plus `audit_cleanup`), in Debug and Release on Windows, including a 50-run loop of `tst_plot_requests`
3. Code follows patterns established in reference files (`src/ui/docks/legend/LegendPresenter.cpp` for the presenter shape, `src/sessionmodel.*` / `src/jobqueue.*` for comment density and guard discipline; test conventions of `tests/README.md` section 8)
4. No TODOs or placeholder code remains
5. `flysight_core` links Qt Core + Gui only; `src/plotrequests.*` contains no lock, no atomic, no thread, and no Widgets include
6. `src/plotmodel.*`, `src/engine/*`, `src/jobqueue.*`, `src/jobmodel.*`, `src/sessionmodel.*`, `src/mainwindow.*`, and `src/ui/*` are untouched (application wiring and painting are Phase 7)
