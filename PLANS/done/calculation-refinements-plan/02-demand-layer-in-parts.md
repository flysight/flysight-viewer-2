# Phase 2: Demand layer in parts

## Overview

This phase restructures `CalculationDemand` into the four parts of spec §11
without changing what it decides. The parts are the **presentation values**
(`src/demandstate.{h,cpp}`), the **settle clock**
(`src/demandsettleclock.{h,cpp}`), the **fill** (`src/demandfill.{h,cpp}`) and
the **reconciler**, which is what stays in `src/calculationdemand.{h,cpp}`.
The phase also removes the demand layer's unused generality of spec §9:

- the memory of its own offer, and the rule that only a chosen next job it
  offered itself is withdrawn;
- the check before each offer whether the candidate equals the chosen next
  job (the demand layer acts on the executor's answer instead);
- `DemandTrack::settling` and `DemandTrack::job`;
- `DemandState::waiting`. The counts stay.

The walk, the classification, `Memory` and the per-cell `Settlement` move
as they are, and Phase 3 rewrites them inside the reconciler. The interfaces
between the reconciler and the other three parts are designed so that Phase 3
does not touch the fill, the settle clock or the presentation values.

## Dependencies

- **Depends on:** Phase 1 (Contracts below the demand layer).
- **Blocks:** Phase 3 (One walk and one pair memory), Phase 4 (What the user
  sees), Phase 5 (Documentation, acceptance map and audit).
- **Assumptions:** Phase 1 is committed and tagged
  `plan/calculation-refinements/phase-1-done`. The full suite and
  `audit_cleanup` pass there. In particular, as Phase 1 leaves the code:
  - `CalculationDemand` has no `isFillEnding`, `m_fillEnding`,
    `rowDisplayName`, `rebuildRelevantNames`, `m_relevantNames`,
    `m_relevantNamesDirty` or `m_columnsDirty`.
  - It has `bool isRelevantName(const DependencyKey &)` and
    `classifyUnloaded(int row, const SessionRow &, const ColumnInfo &)`.
  - `syncColumns()` runs at the start of every non-inert pass and reads
    `SessionModel::columnRequestedCalculations()`.
  - `CalculationDemand` has no `onSessionDataChanged` and no connection to
    the model's `dataChanged`: a bulk edit reaches it as a `dependencyChanged`
    (the model's bulk edit publishes through `publishInvalidation`).
  - The fill's task is registered with `canStep = [this] { return canLoad(); }`
    and `hasFillWork()` is `!isInert() && !m_queue->isShutDown() && m_fillRemaining > 0`.
  - The end of `recompute()` starts a fresh count when the pending sessions
    rise from zero: `m_fillHighWater = fillRemaining` when the previous
    `m_fillRemaining` was 0 and `fillRemaining > 0`, else the maximum.
  - `JobQueue` has no `idle`, `jobQueued`, `activeJobs()` or `AfterEnd`.
  - Tests use `activeJobIds(const JobQueue&)` (`tests/support/jobfixture.h`)
    and spy on `m_queue->model()`'s `rowsInserted` for created jobs.

  Line numbers below are those of `ec6bd43`. Phase 1 has shifted some of
  them. Locate every passage by its name or its quoted text.

## Tasks

### Task 2.1: The presentation values (`src/demandstate.{h,cpp}`)

**Purpose:** Give the views a header that holds only the plain values they
present: the track, the per-source state, the counts and the tooltip text
(spec §11, first bullet). Remove the fields that nothing the user sees reads
(spec §9).

**Files to create:**
- `src/demandstate.h`: `DemandCondition`, `DemandTrack`, `DemandState`.
- `src/demandstate.cpp`: `DemandState::addTrack`, `DemandState::finish`, `DemandState::buildToolTip`.

**Files to modify:**
- `src/calculationdemand.h`: delete the three types and `buildToolTip` / `kToolTipListLimit` / `addTrack` / `finishState`, and `#include "demandstate.h"`.
- `src/calculationdemand.cpp`: delete the bodies of `buildToolTip` (~1075-1106), `addTrack` (~1120-1147) and `finishState` (~1149-1154), and call the new members instead.
- `src/ui/docks/plotselection/PlotRowDelegate.h` / `.cpp`, `src/ui/docks/logbook/LogbookHeaderView.h` / `.cpp`: includes only (see below).
- `src/ui/docks/DemandIndicator.h` (line 6) and `src/ui/docks/plotselection/PlotRowLayout.h` (line ~20): the comment "(calculationdemand.h)" becomes "(demandstate.h)". These are comment-only edits. Phase 4 owns everything else in them.

**Technical Approach:**

1. **Move** `DemandCondition`, `DemandTrack` and `DemandState` from
   `calculationdemand.h` (lines 30-100) into `src/demandstate.h` (include
   guard `DEMANDSTATE_H`, namespace `FlySight`). It includes only
   `<QCoreApplication>` (for `Q_DECLARE_TR_FUNCTIONS`), `<QList>`,
   `<QString>` and `<QStringList>`: no model, executor or widget header.
   Keep every doc comment. Two comments change:
   - The `DemandCondition::Waiting` doc keeps "(inside the input-settle wait,
     chosen next, or behind other work)". That text describes the condition,
     not a field.
   - `DemandState::runningCount` keeps its "(JobQueue::kMaxRunningJobs)" as
     text. Do not include `jobqueue.h`.
2. **`DemandTrack` loses two fields.**
   - `bool settling = false;` (line 49).
   - `JobId job = 0;` and its comment (lines 50-55).
   - `operator==` (lines 58-64) drops `&& settling == other.settling` and
     `&& job == other.job`.

   The remaining fields are `sessionId`, `sessionName`, `condition`,
   `calculationTitles`, `reason`, `jobFailure` and `progressText`. No `JobId`
   remains, so the header needs no `jobmodel.h`.
3. **`DemandState` loses its waiting list.**
   - Delete `QList<DemandTrack> waiting;` and its comment (lines 79-81).
   - `operator==` (lines 90-98) drops `&& waiting == other.waiting`.
   - Rewrite the comment of `running, failed` as "each in session-model row
     order; waiting tracks are counted (waitingCount), never listed".
   - `toolTip`'s comment becomes "buildToolTip(*this); empty when isPlain()".
4. **Add to `DemandState`**, keeping `isWorking()`, `showsWarning()` and
   `isPlain()` as they are:
   ```cpp
   /// Tracks listed per section of a tooltip at most (running, failed); a
   /// longer section ends "and N more".
   static constexpr int kToolTipListLimit = 10;
   /// Counts one track: wanted unless NotApplicable; done = Done + Failed;
   /// Running and Failed tracks are also appended to running / failed.
   void addTrack(const DemandTrack &track);
   /// Sets progressLabel ("<doneCount> of <wantedCount>" while isWorking(),
   /// else empty) and toolTip; call once every track is added.
   void finish();
   /// The ready-made tooltip of a state ... (the doc of today's
   /// CalculationDemand::buildToolTip, calculationdemand.h 359-363). A pure
   /// function of its argument.
   static QString buildToolTip(const DemandState &state);
   ```
   The last line of the struct body is `Q_DECLARE_TR_FUNCTIONS(DemandState)`.
   The macro ends in `private:`, so nothing may follow it.
5. **`src/demandstate.cpp`.** Move the bodies unchanged:
   - `buildToolTip` from calculationdemand.cpp ~1075-1106, with
     `kToolTipListLimit` read as the member.
   - `addTrack` from ~1120-1147. Delete its `listWaiting` parameter and the
     `if (listWaiting) state.waiting.append(track);` branch. The Waiting case
     only counts.
   - `finishState` from ~1149-1154 becomes `finish()`: `progressLabel =
     isWorking() ? tr("%1 of %2")...` and `toolTip = buildToolTip(*this)`.

   `tr()` now resolves to the `DemandState` context. The repository has no
   `.ts` files, so no translation is lost.
6. **The views include the presentation values and forward-declare the
   component.** No behaviour changes.
   - `PlotRowDelegate.h` line 9 and `LogbookHeaderView.h` line 10: replace
     `#include "calculationdemand.h"` with `#include "demandstate.h"` and add
     `class CalculationDemand;` inside `namespace FlySight`. The existing
     `QPointer<CalculationDemand>` members compile with the forward
     declaration, as `LogbookCellDelegate.h` already shows.
   - `PlotRowDelegate.cpp` and `LogbookHeaderView.cpp`: add
     `#include "calculationdemand.h"`. They connect to its signals and call
     `plotState()`, `columnState()`, `columnId()` and `working*Ids()`.
   - `LogbookCellDelegate.*`, `LogbookView.*`, `PlotSelectionDockFeature.cpp`,
     `LogbookDockFeature.cpp`, `AppContext.h` and `PlotWidget.cpp` stay as
     they are.
7. **Verified:** no view and no tooltip reads the removed fields. A grep of
   `src/ui` for `.waiting`, `.settling` and `.job` finds nothing.
   `buildToolTip` reads only `isWorking()`, `doneCount`, `wantedCount`,
   `running` (with `sessionName`, `calculationTitles` and `progressText`),
   `failedCount` and `failed` (with `sessionName` and `reason`).

**Acceptance Criteria:**
- [ ] `src/demandstate.h` defines `DemandCondition`, `DemandTrack` and `DemandState`, and includes no header of `src/` and no widget header.
- [ ] `DemandTrack` has no `settling` and no `job`. `DemandState` has no `waiting`. Neither `operator==` compares them.
- [ ] `DemandState::buildToolTip`, `addTrack`, `finish` and `kToolTipListLimit` exist. `CalculationDemand` no longer declares `buildToolTip`, `kToolTipListLimit`, `addTrack` or `finishState`.
- [ ] `PlotRowDelegate.h` and `LogbookHeaderView.h` include `demandstate.h` and not `calculationdemand.h`.
- [ ] `tooltipText` and `toolTipListsAtMostTenFailures` pass with `DemandState::buildToolTip` and produce the same strings.

**Complexity:** M

---

### Task 2.2: The settle clock (`src/demandsettleclock.{h,cpp}`)

**Purpose:** Hold the per-session deadlines of the input-settle wait and the
earliest-deadline timer behind the two questions of spec §11: "is this
session settling" and "when does the next wait end".

**Files to create:**
- `src/demandsettleclock.h`, `src/demandsettleclock.cpp`: class `DemandSettleClock`.

**Files to modify:**
- `src/calculationdemand.h` / `.cpp`: remove `m_settleUntil`, `m_settleTimer`, `m_settleDelayMs`, `dropExpiredSettles()`, `armSettleTimer()` and `onSettleTimeout()`, and use the clock (Task 2.4).

**Technical Approach:**

This is a plain C++ class, not a `QObject`, with no signal. It holds a `QTimer`
and calls one callback. It knows nothing of sessions beyond their ids. It
includes `<QDeadlineTimer>`, `<QHash>`, `<QSet>`, `<QString>`, `<QTimer>` and
`<functional>`: no model, executor, engine or demand header.

```cpp
class DemandSettleClock
{
public:
    /// `waitEnded` is called from the timer, after the waits that have ended
    /// were forgotten: the owner schedules a pass.
    DemandSettleClock(int delayMs, std::function<void()> waitEnded);
    DemandSettleClock(const DemandSettleClock &) = delete;
    DemandSettleClock &operator=(const DemandSettleClock &) = delete;

    void setDelay(int milliseconds);            ///< clamped at 0; affects later start() calls only
    int  delay() const;
    void start(const QString &sessionId);       ///< starts or restarts the session's wait of delay(); re-arms
    void endAll();                              ///< every wait ends now; the timer stops; waitEnded is not called
    void keepOnly(const QSet<QString> &sessionIds);   ///< forgets every other session's wait; re-arms
    void dropExpired();                         ///< forgets the waits that have ended; calls nothing

    bool isSettling(const QString &sessionId) const;  ///< its wait exists and has not ended
    bool hasSettling() const;                   ///< some session is settling
    /// When the next wait ends: the earliest deadline the clock holds (one
    /// that has ended is due now); QDeadlineTimer::Forever when it holds none.
    QDeadlineTimer nextWaitEnd() const;

private:
    void arm();                                 ///< single shot for nextWaitEnd(), or stop
    void onTimeout();                           ///< dropExpired(); arm(); m_waitEnded()

    QHash<QString, QDeadlineTimer> m_until;
    QTimer m_timer;                             ///< single shot
    int m_delayMs;
    std::function<void()> m_waitEnded;
};
```

Map each piece of today's code to the clock:

| Today (calculationdemand.cpp, ec6bd43) | Clock |
|---|---|
| ctor: `m_settleTimer.setSingleShot(true); connect(&m_settleTimer, &QTimer::timeout, this, &CalculationDemand::onSettleTimeout);` (73-74) | ctor: `m_timer.setSingleShot(true); QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] { onTimeout(); });` |
| `setInputSettleDelay`: `m_settleDelayMs = qMax(0, milliseconds);` (1332-1335) | `setDelay` |
| `endInputSettleWaits`: `m_settleUntil.clear(); m_settleTimer.stop();` (1339-1340) | `endAll()` (the owner still calls `scheduleUpdate()`) |
| `isSettling` / `hasSettlingSessions` (1344-1354) | `isSettling` / `hasSettling`, same bodies |
| `dropExpiredSettles` (1356-1364) | `dropExpired` |
| `armSettleTimer` (1366-1378): the minimum over all entries of `qMax<qint64>(0, deadline.remainingTime())`, then stop or `start(int(qMin(earliest, INT_MAX)))` | `nextWaitEnd()` returns the smallest `QDeadlineTimer` of `m_until` (compare with `operator<`), or `QDeadlineTimer(QDeadlineTimer::Forever)` when empty. `arm()` stops the timer when `nextWaitEnd().isForever()`, else starts it for `qMin<qint64>(qMax<qint64>(0, next.remainingTime()), std::numeric_limits<int>::max())`. The interval is identical to today's. |
| `onSettleTimeout`: `dropExpiredSettles(); armSettleTimer(); scheduleUpdate();` (1380-1385) | `onTimeout()`: `dropExpired(); arm(); m_waitEnded();` |
| `onDependencyChanged`: `m_settleUntil.insert(sessionId, QDeadlineTimer(m_settleDelayMs)); armSettleTimer();` (1437-1438) | `start(sessionId)` |
| `onSessionModelReset`: `m_settleUntil.removeIf(... !ids.contains(it.key())); ... armSettleTimer();` (1479-1482) | `keepOnly(ids)` |

The clock is created in the `CalculationDemand` constructor as
`std::make_unique<DemandSettleClock>(kInputSettleMs, [this] { scheduleUpdate(); })`.
`kInputSettleMs` stays on `CalculationDemand`. It is the demand layer's policy,
and tests read `CalculationDemand::kInputSettleMs`.

The class comment states the contract: "per-session deadlines of the
input-settle wait and one single-shot timer armed for the earliest; answers
whether a session is settling and when the next wait ends; calls its owner
when a wait ends. What starts a wait is the owner's decision." The
INPUT-SETTLE WAIT paragraph of `calculationdemand.h` keeps what starts a wait.

**Acceptance Criteria:**
- [ ] `DemandSettleClock` includes no header of `src/`, and no `SessionModel`, `JobQueue`, `BlockerReport` or `DemandTrack` appears in `src/demandsettleclock.*` (audit, Task 2.6).
- [ ] `CalculationDemand` has no `QDeadlineTimer` or settle `QTimer` member. Its seams `setInputSettleDelay`, `inputSettleDelay`, `endInputSettleWaits`, `isSettling` and `hasSettlingSessions` forward to the clock.
- [ ] `inputBurstRunsOneJob`, `supersededJobIsRunAgainAfterInputsSettle` (the real timer), `staleRunningJobIsWaitingAtOnce`, `dependencyBurstIsCoalesced` and `nullCollaborators` pass.
- [ ] The new test `settleClockAnswersItsQuestions` passes (see Testing).

**Complexity:** M

---

### Task 2.3: The fill (`src/demandfill.{h,cpp}`)

**Purpose:** Hold the holds, the load candidates, the counters and the
scheduler task behind the contract of spec §11. The fill is given, after each
pass, the set of sessions with a pending column cell and the load candidates
in order. Its API speaks of session ids only.

**Files to create:**
- `src/demandfill.h`, `src/demandfill.cpp`: class `DemandFill`.

**Files to modify:**
- `src/calculationdemand.h` / `.cpp`: remove `m_held`, `m_loadCandidates`, `m_fillRemaining`, `m_fillHighWater`, `m_fillTaskRegistered`, `releaseHolds()`, `releaseAllHolds()`, `registerFillTask()`, the bodies of `hasFillWork()`, `canLoad()` and `runLoadStep()`, and the fill numbers at the end of `recompute()` (Task 2.4).
- `src/jobqueue.h` (lines 128-130): "(kMaxHeldSessions, calculationdemand.h)" becomes "(DemandFill::kMaxHeldSessions, demandfill.h)".
- `src/sessionmodel.h` (lines 195-197): the `ColumnFillTask` comment "(calculationdemand.h)" becomes "(demandfill.h)". This is a comment only. The audit forbids `JobQueue` and `CalculationDemand` there, but not this file name.

**Technical Approach:**

```cpp
class DemandFill
{
public:
    /// Sessions held loaded for column demand at most: the running job's and
    /// the chosen next job's (the executor's bound plus one).
    static constexpr int kMaxHeldSessions = JobQueue::kMaxRunningJobs + 1;

    /// What the fill asks of the component that owns it. Each hook is called
    /// on the main thread, outside any row stability guard.
    struct Hooks {
        std::function<bool()> enabled;              ///< work may be done: the owner is not inert and the executor is not shut down
        std::function<void()> runPendingPass;       ///< before a load: run the owner's pending pass, if any
        std::function<void()> runPass;              ///< run a pass now (the candidate went, was loaded or shown meanwhile)
        std::function<void(const QString &sessionId)> loadFailed;               ///< loadPinnedSession() failed; not held
        std::function<void(const QString &requestedId, const QString &heldId)> loaded; ///< held under heldId (the corrected id)
    };

    DemandFill(SessionModel *sessionModel, Hooks hooks);
    DemandFill(const DemandFill &) = delete;
    DemandFill &operator=(const DemandFill &) = delete;
    ~DemandFill();                  ///< detach()

    void registerTask();            ///< SessionModel::ColumnFillTask; call once
    /// Unregisters the task (when registered and the model lives) and releases
    /// every hold. Idempotent; nothing is called back.
    void detach();

    /// After each pass: releases every hold whose row is gone or not loaded,
    /// or whose session is not in `pendingSessions` (every hold when the model
    /// is gone); takes `loadCandidates` (row order, at most kMaxHeldSessions);
    /// remaining = pendingSessions.size(), and the total is the high-water mark,
    /// started afresh when remaining rises from 0; wakes the scheduler when
    /// hasWork(), the counts, the candidates or the number of holds changed.
    void update(const QSet<QString> &pendingSessions, const QStringList &loadCandidates);

    QStringList heldSessionIds() const { return m_held; }   ///< in load order
    bool hasWork() const;           ///< O(1): enabled() and remaining > 0
    bool canLoad() const;           ///< hasWork(), a hold is free and a candidate waits
    void step();                    ///< the task's step: at most one hidden load

private:
    QPointer<SessionModel> m_model;
    Hooks m_hooks;
    QStringList m_held;             // sessions pinned by loadPinnedSession(), in load order
    QStringList m_candidates;       // the next loads, in row order
    int m_remaining = 0;            // sessions with a pending cell
    int m_highWater = 0;            // the fill's total; 0 between fills
    bool m_registered = false;
};
```

`demandfill.h` includes `<functional>`, `<QPointer>`, `<QSet>`,
`<QString>`, `<QStringList>` and `"jobqueue.h"`, the last for
`kMaxRunningJobs` only. It forward-declares `SessionModel`.
`demandfill.cpp` includes `"idlescheduler.h"` and `"sessionmodel.h"`.

**Bodies, moved from today's code (after Phase 1):**

- `registerTask()`: today's `registerFillTask()` (~844-862) with the
  members renamed:
  - priority 5;
  - step `[this] { step(); }`;
  - hasWork `[this] { return hasWork(); }`;
  - progress `[this] { return Progress{m_remaining, m_highWater}; }`;
  - onComplete `[this](bool) { if (m_remaining == 0) m_highWater = 0; }`,
    keeping the comment "The next fill starts its own count. A cancel()
    changes nothing while sessions remain.";
  - cancellable `false`;
  - canStep `[this] { return canLoad(); }`.

  Guard it with `if (!m_model || m_registered) return;`.
- `detach()`:
  1. `if (m_model && m_registered) m_model->scheduler().unregisterTask(SessionModel::ColumnFillTask);`
  2. `m_registered = false;`
  3. Today's `releaseAllHolds()` (~835-842): unpin each held id when the
     model lives, then clear the list.
- `hasWork()`: `m_hooks.enabled && m_hooks.enabled() && m_remaining > 0`.
  Today's `!isInert() && !m_queue->isShutDown()` is now the owner's `enabled`
  hook, so the fill never names the executor.
- `canLoad()`: `hasWork() && m_held.size() < kMaxHeldSessions && !m_candidates.isEmpty()`.
- `update()`:
  1. Snapshot `std::make_tuple(hasWork(), m_remaining, m_highWater,
     m_candidates, m_held.size())`.
  2. Release holds. When the model is gone, clear `m_held`. Otherwise run
     today's `releaseHolds()` body (~818-833): `release = row < 0 ||
     !std::as_const(*m_model).rowAt(row).isLoaded() ||
     !pendingSessions.contains(held)`, calling `unpinSession(held)` for each
     released session.
  3. `m_candidates = loadCandidates;`
  4. Counts, with Phase 1's rule: if `m_remaining == 0 && remaining > 0`,
     then `m_highWater = remaining`; else `m_highWater = qMax(m_highWater,
     remaining)`. Then `m_remaining = remaining`.
  5. If the model lives and the snapshot changed, call
     `m_model->scheduler().wake()`.
- `step()`: today's `runLoadStep()` (~884-935, less Phase 1's removed ending
  branch):
  ```
  m_hooks.runPendingPass();                       // the candidates as demand is now
  if (!canLoad()) return;
  const QString id = m_candidates.takeFirst();
  row = m_model->getSessionRow(id); if (row < 0) { m_hooks.runPass(); return; }
  { const SessionRow &sr = std::as_const(*m_model).rowAt(row);
    if ((sr.isLoaded() && !sr.loadFailed) || sr.visible) { m_hooks.runPass(); return; } }
  const QString held = m_model->loadPinnedSession(id);
  if (held.isEmpty()) { m_hooks.loadFailed(id); return; }
  m_held.append(held);
  m_hooks.loaded(id, held);
  ```
  Keep today's comments ("One hidden load: the session-id correction happens
  here …", "The column worker or resolveIdentityStubs() may have remapped the
  id silently …"). A hook may run a pass that calls `update()` re-entrantly.
  `step()` returns right after every hook, so this is safe.

**The class comment** is today's HIDDEN LOADS paragraph of
`calculationdemand.h` (~227-259, in Phase 1's wording), moved here and
rewritten in the fill's own terms. Start it with spec §11's contract:

> Given, after each pass, the set of sessions with a pending column cell and
> the load candidates in order, the fill keeps at most kMaxHeldSessions hidden
> sessions loaded and pinned for column demand, releases a hold when its
> session has no pending cell or no loaded row, loads the next candidate when
> a hold is free and the scheduler steps it, reports progress as the sessions
> remaining of the high-water mark, loads nothing once the executor is shut
> down (the owner's `enabled` hook), and holds nothing once the component goes
> (detach()).

Then keep the case list (pool capacity, column disabled mid-load, a held
session shown, reset, not applicable once loaded, a failed load, the executor
shut down), the priority remark ("Saves, visible loads, bulk edits and column
work have a higher priority …") and "Not cancellable".

The fill "exposes nothing of the walk". Its interface takes and returns
session ids only, and it never calls the executor. The owner's hooks are its
only callers back.

**Acceptance Criteria:**
- [ ] `DemandFill` is the only file of the demand layer that calls `loadPinnedSession(`, `pinSession(` or `unpinSession(`, and the only one that names `ColumnFillTask` in code (audit, Task 2.6).
- [ ] `src/demandfill.*` never calls the executor, and names no `BlockerReport`, `DemandTrack`, `DemandState`, `DemandCondition` or `RowStabilityGuard` (audit, Task 2.6).
- [ ] `kMaxHeldSessions = JobQueue::kMaxRunningJobs + 1` is defined once, in `src/demandfill.h`.
- [ ] Every fill test passes unchanged in what it asserts: `enablingColumnFillsEveryUnloadedSession`, `fillTaskReportsProgressWhileWaiting`, `fillTaskIsLowestAndNotCancellable`, Phase 1's `fillEndingBehindAnotherTaskStartsNextCountFresh`, `chainedColumnKeepsItsHold`, `disablingColumnReleasesHeldSessions`, `heldSessionShownStaysLoaded`, `removedOrRepopulatedHeldSessionIsReleased`, `identityStubIsOfferedUnderItsRealId`, `unloadableSessionIsSettledAsFailed`, `noLoadsAfterExecutorShutdown`, `demandDestroyedReleasesHoldsAndTask` and `savesAndBulkEditsPrecedeLoadStep`.

**Complexity:** L

---

### Task 2.4: The reconciler: `CalculationDemand` rewired to its parts

**Purpose:** Leave the walk, the classification, the memory, the choice and
the pass in `calculationdemand.{h,cpp}`. Their logic does not change. They
reach the other parts only through the interfaces of Tasks 2.1-2.3.

**Files to modify:**
- `src/calculationdemand.h`, `src/calculationdemand.cpp`.

**Technical Approach:**

1. **Header.**
   - Include `"demandstate.h"`. Forward-declare `class DemandFill;` and
     `class DemandSettleClock;`, and hold them as
     `std::unique_ptr<DemandSettleClock> m_settle;` and
     `std::unique_ptr<DemandFill> m_fill;`. The views include
     `calculationdemand.h` in their `.cpp` files, and this keeps them from
     seeing the fill and the clock.
   - Remove `<QDeadlineTimer>` and every member and method named in Tasks
     2.1-2.3 and 2.5.
   - Public constants: keep `kInputSettleMs`. Delete `kMaxHeldSessions`
     (now `DemandFill::kMaxHeldSessions`) and `kToolTipListLimit` (now
     `DemandState::kToolTipListLimit`).
   - Keep every public read method, signal and test seam with its present
     signature. These are `plotId`, `columnId`, `plotState`, `columnState`,
     both `isCellPending`, `workingPlotIds`, `workingColumnIds`,
     `isMerelyUncomputed`, `flush`, `hasPendingUpdate`, `passCount`,
     `setInputSettleDelay`, `inputSettleDelay`, `endInputSettleWaits`,
     `isSettling`, `hasSettlingSessions`, `heldSessionIds`, `hasFillWork`,
     `canLoad`, `runLoadStep` and `recordSetLookups`.
   - The fill and clock seams become one-line forwarders in the `.cpp`:
     `heldSessionIds()` → `m_fill->heldSessionIds()`, `hasFillWork()` →
     `m_fill->hasWork()`, `canLoad()` → `m_fill->canLoad()`, `runLoadStep()`
     → `m_fill->step()`, and the settle seams → `m_settle`.
2. **Construction** (`CalculationDemand::CalculationDemand`, ~62-131).
   1. First, before any `connect`, create the clock (Task 2.2) and the fill:
      ```cpp
      m_fill = std::make_unique<DemandFill>(m_sessionModel.data(), DemandFill::Hooks{
          /*enabled*/        [this] { return !isInert() && !m_queue->isShutDown(); },
          /*runPendingPass*/ [this] { if (hasPendingUpdate()) recompute(); },
          /*runPass*/        [this] { recompute(); },
          /*loadFailed*/     [this](const QString &id) { onFillLoadFailed(id); },
          /*loaded*/         [this](const QString &id, const QString &held) { onFillLoaded(id, held); }});
      ```
   2. Replace `if (!isInert()) registerFillTask();` with
      `if (!isInert()) m_fill->registerTask();`.
   3. Delete the settle-timer lines (73-74).
3. **Destruction** (~133-140). The body becomes
   `CalculationRegistry::instance().removeObserver(m_registryObserver); m_fill->detach();`.
   This keeps today's order (observer, then task, then holds) whatever the
   member order. The hooks capture `this`, and `detach()` calls none of them.
4. **The two load hooks.** These are private members, and their bodies are
   moved from `runLoadStep()`:
   - `onFillLoadFailed(id)`: `const Settlement failed = loadFailedSettlement();
     for (const ColumnInfo &column : std::as_const(m_columns))
     m_settled.insert(CellKey(id, column.id), failed); scheduleUpdate();`
     (~918-922, with the comment "Not held, and not loaded again in this run
     …").
   - `onFillLoaded(id, held)`: `if (held != id) { m_recordSets.remove(id);
     m_recordReasons.remove(id); m_columnReports.remove(id);
     eraseSettlements(id); } scheduleUpdate();` (~926-934, with its comments).
5. **The walk.**
   - `walkColumns(const JobRecord &running, const JobRecord &chosen)`
     becomes `walkColumns(const JobRecord &running, const QSet<QString> &held)`.
     `chosen` was only passed on to `classify()`, which loses it (item 6).
   - In the unloaded branch (~649-651), the load-candidate test becomes:
     ```cpp
     if (waiting && !sr.visible && !m_settle->isSettling(sessionId) && !held.contains(sessionId)
         && walk.loadCandidates.size() < DemandFill::kMaxHeldSessions)
     ```
   - In the loaded branch (~628),
     `if (track.condition == DemandCondition::Waiting && !track.settling)`
     becomes `if (track.condition == DemandCondition::Waiting &&
     !m_settle->isSettling(sessionId))`. The fact is the same, now asked of
     the clock rather than of a field.
   - `tally` (~585-591) calls `walk.states[column].addTrack(track)`.
   - The walk still reads a plain value of the holds and asks the clock. It
     calls neither the fill nor the executor, and never loads or pins.
6. **Classification.** `classify(const Track &, const BlockerReport &, const
   JobRecord &running, const JobRecord &chosen)` loses `chosen`.
   - In its body (~454-500), delete `bool isChosen`, the `if (chosen.id != 0
     && …) isChosen = true;` line, `state.job = running.id;`,
     `state.settling = isSettling(track.sessionId);` and
     `state.job = isChosen ? chosen.id : 0;`.
   - The Waiting branch sets only `condition` and `calculationTitles`.
   - In `classifyUnloaded`, delete `track.settling = isSettling(sessionId);`
     (~748).
7. **The pass** (`recompute()`, ~1212-1328, as Phase 1 leaves it).
   1. Delete the `fillSnapshot` lambda and `fillBefore`. The fill wakes the
      scheduler itself.
   2. In the reconciling scope, `dropExpiredSettles()` becomes
      `m_settle->dropExpired()`.
   3. **Inert branch:** `m_fill->update({}, {}); withdrawChoice();`. For
      `withdrawChoice()` see Task 2.5. `update()` with no pending session
      releases every hold, as `releaseAllHolds()` did.
   4. **Non-inert branch**, in this order:
      1. `syncCheckedSet(); syncColumns();`
      2. `plots` / `inspections` as today.
      3. `const JobRecord running = m_queue->job(m_queue->runningJob());`,
         read once.
      4. `walk = walkColumns(running, QSet<QString>(held.cbegin(), held.cend()))`
         when `m_columns` is not empty, where
         `const QStringList held = m_fill->heldSessionIds();`.
      5. At the place of today's `releaseHolds(walk.pendingSessions);`:
         `const bool shutDown = m_queue->isShutDown();
         m_fill->update(walk.pendingSessions, shutDown ? QStringList() :
         walk.loadCandidates);`.
      6. `if (!shutDown)`: the tiers as today, with
         `plotCandidates(inspections, m_sessionModel->focusedSessionId(),
         running)`, then `offerChoice(candidates)`.
      7. After the offers, plot classification as today, re-reading `running`
         ("After the offers: the executor's jobs as they are now"). Drop the
         `chosen` read, because `classify()` no longer takes it. Build each
         state with `buildState()`, which now calls `state.addTrack(track)`
         and `state.finish()`.
      8. Column states: `state.finish()` instead of `finishState(state)`.
   5. Delete the local `loadCandidates` and `fillRemaining` and the whole
      "column fill's numbers" block after the scope: they are in `update()`.
      `applyStates(...)` follows as today.
   6. Delete the trailing wake: `if (m_sessionModel && fillSnapshot() !=
      fillBefore) m_sessionModel->scheduler().wake();`.

   `update()` now runs before the offers, and today's counts were stored
   after them. Nothing reads the fill between these points. The executor's
   signals during an offer only schedule (`m_reconciling`). `unpinSession()`
   only queues an eviction pass. `wake()` only starts the scheduler's timer.
   So the order is not observable.
8. **`plotCandidates(inspections, focusedId, const JobRecord &running)`.**
   It no longer reads the executor. Replace ~966-971
   (`if (const JobId runningId = m_queue->runningJob()) { … }`) with
   `if (running.id != 0 && !running.cancelRequested) runningKey =
   PairKey(running.sessionId, running.instanceId);`. The record is the one
   the walk used, read at the same moment as today's read. Replace
   `isSettling(sessionId)` (~975) with `m_settle->isSettling(sessionId)`.
   Keep the `if (!m_queue) return candidates;` guard.
9. **Slots.**
   - `onDependencyChanged` (~1408-1440): `m_settleUntil.insert(…);
     armSettleTimer();` becomes `m_settle->start(sessionId);`. The rest is
     Phase 1's version.
   - `onSessionModelReset` (~1465-1485): the `m_settleUntil.removeIf(…)` and
     `armSettleTimer()` lines become `m_settle->keepOnly(ids);`.
   - `endInputSettleWaits()`: `m_settle->endAll(); scheduleUpdate();`.
   - `onJobProgress` (~1573-1603) can no longer match `track.job`. It matches
     the running tracks by the executor's record of the job:
     ```cpp
     const JobRecord record = m_queue ? m_queue->job(id) : JobRecord();
     if (record.id == 0 || record.state != JobState::Running)
         return;                       // not a job any track can describe
     ... for (DemandTrack &track : state.running) {
             if (track.sessionId != record.sessionId || track.progressText == text) continue;
     ...
     state.toolTip = DemandState::buildToolTip(state);
     ```
     This is equivalent. A Running track exists only for the running job
     that was not asked to stop (`classify()`), there is one running job
     (`kMaxRunningJobs == 1`), and the executor emits `jobProgress` only for
     its running job (`JobQueue::onProgress`: `if (!m_run || m_run->jobId !=
     id) return;`). The hand-made `jobProgress(JobId(999), …)` of
     `progressUpdatesWithoutInspection` finds no record and changes nothing,
     as today.
10. **Class comment** (calculationdemand.h ~102-314).
    - TRACK CONDITIONS table, last row: "Waiting (settling inside the
      input-settle wait)" becomes "Waiting".
    - Replace HIDDEN LOADS with two sentences: the column fill (demandfill.h)
      loads sessions that are not loaded for column demand; the pass gives
      it the sessions with a pending cell and the load candidates, which are
      waiting, hidden, unloaded, not settling sessions in row order up to
      DemandFill::kMaxHeldSessions.
    - INPUT-SETTLE WAIT keeps what starts a wait and adds "the clock
      (demandsettleclock.h) holds the deadlines".
    - PRESENTATION refers to demandstate.h for the values, the tooltip and
      its limit.
    - THE CHOICE follows Task 2.5.
    - Add a new paragraph, **PARTS**, placed before LIFETIME:

      > The presentation values (demandstate.h) are what the views read. The
      > fill (demandfill.h) holds sessions loaded for column demand and runs
      > the scheduler task. The settle clock (demandsettleclock.h) answers
      > whether a session is settling and when the next wait ends. This file
      > is the reconciler: the walk, the classification, the memory, the
      > choice and the pass. The walk reads the rows under one guard, asks
      > the clock and reads a copy of the holds. It calls neither the
      > executor nor the fill, and it never loads or pins. After it, the
      > pass gives the fill the pending sessions and the load candidates,
      > offers, and announces.

**Acceptance Criteria:**
- [ ] `src/calculationdemand.*` contain no `m_held`, `m_loadCandidates`, `m_fillRemaining`, `m_fillHighWater`, `m_fillTaskRegistered`, `m_settleUntil`, `m_settleTimer`, `m_settleDelayMs`, `QDeadlineTimer`, `releaseHolds`, `registerFillTask`, `armSettleTimer`, `dropExpiredSettles`, `onSettleTimeout`, `loadPinnedSession(`, `unpinSession(` or `ColumnFillTask`.
- [ ] `classify()`, `walkColumns()` and `plotCandidates()` have the signatures of items 5, 6 and 8. Nothing in the walk (`inspect`, `inspectUnderGuard`, `walkColumns`, `columnReports`, `classifyUnloaded`, `recordSet`) calls `m_queue` or `m_fill`.
- [ ] The public read interface, the signals and the test seams of `CalculationDemand` keep their names and signatures. `kMaxHeldSessions`, `kToolTipListLimit` and `buildToolTip` are the only public names that moved.
- [ ] `progressUpdatesWithoutInspection`, `sharedJobSameProgress` and `tst_fusion_rows::rollPitchYawShareOneJob` show the progress text on every row that waits on the running job.
- [ ] Every test of `tst_calculation_demand`, `tst_logbook_indicators`, `tst_plot_row_delegate`, `tst_result_columns`, `tst_fusion_rows` and `tst_fusion_store` passes with only the edits of the Testing section.

**Complexity:** L

---

### Task 2.5: The choice acts on the executor's answer; no own-offer memory

**Purpose:** Remove the own-offer memory and rule, and the pre-offer
equality check (spec §9, first two bullets).

**Files to modify:**
- `src/calculationdemand.h`: delete `JobId m_offeredJob = 0;` (~568) and `withdrawOwnOffer()` (~495). Add `void withdrawChoice();`. Rewrite THE CHOICE and THE ONLY CALLER.
- `src/calculationdemand.cpp`: `offerChoice()` (~1001-1043) and `withdrawOwnOffer()` (~1045-1053).

**Technical Approach:**

1. **Delete the pre-offer check** at the top of the loop in `offerChoice()`:
   ```cpp
        if (const JobId chosen = m_queue->chosenNextJob()) {
            const JobRecord record = m_queue->job(chosen);
            if (PairKey(record.sessionId, record.instanceId) == key) {
                m_offeredJob = chosen;      // already the choice: nothing changes
                return;
            }
        }
   ```
   `JobQueue::offer()` answers the same question first:
   ```cpp
    if (m_shutDown)
        return {Kind::ShuttingDown, 0};
    // Equal to the chosen next job, or to the running job that was not asked
    // to stop: nothing changes, not even a chosen next job of another key
    if (const JobId existing = activeJob(sessionId, calculation.instanceId))
        return {Kind::AlreadyActive, existing};
   ```
   (jobqueue.cpp 181-187). `activeJob()` compares the same
   `(sessionId, instanceId)` key. The offer changes nothing on this path.
2. **Act on the answer.** The switch becomes:
   - `Created`: `return;`. Delete `m_offeredJob = result.job;`.
   - `AlreadyActive`: `if (result.job == m_queue->chosenNextJob()) return;`
     with the comment "already the choice: nothing changes". Otherwise
     `continue;`. That case is the running job not asked to stop. The
     candidates exclude its pair (the `runningKey` of the walk and of
     `plotCandidates`), so it is not expected. It is kept as "not a choice".
   - `MissingInput` / `NothingToDo` / `UnknownCalculation`: unchanged.
   - `Blocked` / `SessionNotLoaded`: `continue;`. Add the comment "Blocked is
     not expected: the candidates list upstream first (the documentation
     says so); nothing is remembered". No other guard.
   - `ShuttingDown`: `return;`.
3. **Withdraw the chosen next job when nothing is chosen.**
   - Replace `withdrawOwnOffer()` with
     `void CalculationDemand::withdrawChoice() { if (m_queue) m_queue->withdrawChosenNext(); }`
     and the comment "This component is the only offerer (audit), so the
     chosen next job is always its own: nothing to choose withdraws it."
   - It is called at the end of `offerChoice()` ("Nothing to choose: demand
     no longer wants the chosen next job") and in the inert branch of the
     pass. `withdrawChosenNext()` returns false and does nothing when there
     is no chosen next job.
4. **Header text.**
   - THE CHOICE (~208-225): "The first candidate the executor accepts is its
     chosen next job (an equal chosen next job is kept as it is)" becomes
     "Each candidate is offered in turn: the first the executor creates is
     the chosen next job, and one it answers AlreadyActive for as the chosen
     next job is kept as it is".
   - "With no candidate accepted, the chosen next job this component offered
     is withdrawn; one it did not offer is left alone." becomes "With no
     candidate accepted, the chosen next job is withdrawn: this component is
     the only offerer, so it is always its own."
   - THE ONLY CALLER keeps its first sentence.

**Behaviour note.** A chosen next job that the demand layer did not offer is
now withdrawn by the next pass that chooses nothing. In the product there is
no such job: the audit (`one call of offer( in product code: the demand
layer`) keeps it so. In tests there is one. `tst_fusion_store` offers fits
directly while a demand layer lives, and a pending pass could withdraw them
before they start. The Testing section fixes those tests deterministically.

**Acceptance Criteria:**
- [ ] `m_offeredJob` and `withdrawOwnOffer` do not exist. `offerChoice()` reads no `chosenNextJob()` before an offer.
- [ ] Repeated passes that find the same first candidate keep the same chosen next job: same id, `Queued`, no new job record (new test `chosenNextJobFollowsTheExecutorsAnswer`).
- [ ] A chosen next job that nothing wants is withdrawn ("No longer needed", never started) by the next pass, even one the demand layer did not offer (same test).
- [ ] `changingDemandReplacesChosenNext`, `waitingPairNeededByAnotherPlotSurvives`, `changeSignalsAreMinimal`, `resultAppearingWhileWaitingDropsThePair`, `hidingASessionDropsItsWaitingPair` and `uncheckingDropsWaitingPairsKeepsRunning` pass unchanged.
- [ ] `withdrawChosenNext(` with a `.` or `->` in front, and `offer(` with a `.` or `->` in front, still appear in `src/calculationdemand.cpp` only, and `offer(` once in `src`.

**Complexity:** S

---

### Task 2.6: Source lists and the audit

**Purpose:** Build the new files. Keep `audit_cleanup` green and make it cover
the new files. Encode the part boundaries of spec §11 as rules.

**Files to modify:**
- `src/CMakeLists.txt`: the `flysight_core` list at line ~303.
- `tests/audit/cleanup_audit.cmake`.

**Technical Approach:**

1. **`src/CMakeLists.txt`.** After `calculationdemand.cpp
   calculationdemand.h` (line 303), add:
   ```
   demandstate.cpp            demandstate.h
   demandfill.cpp             demandfill.h
   demandsettleclock.cpp      demandsettleclock.h
   ```
   Extend the comment above (~298-302) with "(in parts: the presentation
   values, the column fill and the settle clock beside it)".
   - None of the classes is a `Q_OBJECT`, so AUTOMOC needs nothing.
   - **`tests/CMakeLists.txt` needs no change.** No test target compiles
     `calculationdemand.cpp`. It is in `flysight_core`, which every test links
     through `flysight_test_support` (tests/CMakeLists.txt ~63). The two
     widget tests (`tst_plot_row_delegate` ~168-174, `tst_logbook_indicators`
     ~179-185) compile only view sources and get `demandstate.cpp` from the
     library.
   - `cmake/SolverDependencies.cmake` needs no change: no new test target.
2. **Audit: one name for the demand layer's files.** At the head of the
   fusion block, before `set(FUSION_CORE …)` (line 272), add:
   ```cmake
   # The demand layer's files: the component (the reconciler) and its parts.
   set(DEMAND_LAYER "src/calculationdemand.*" "src/demandstate.*" "src/demandfill.*" "src/demandsettleclock.*")
   set(DEMAND_FILES "(calculationdemand|demandstate|demandfill|demandsettleclock)")
   ```
   Then update every rule whose pathspec or regex names the demand layer:

   | Line | Rule | Change |
   |---|---|---|
   | 272 | `FUSION_CORE` (used by "no dialog or message box for a calculation outcome") | `"src/calculationdemand.*"` becomes `${DEMAND_LAYER}` |
   | 349 | "the kernel is pure" | `…|jobqueue|calculationdemand|preferences/|…` becomes `…|jobqueue|${DEMAND_FILES}|preferences/|…`. The variable expands inside the quoted regex, so the alternation stays grouped. |
   | 355 | "nobody but the application references the fusion library" | `"src/calculationdemand.*"` becomes `${DEMAND_LAYER}` |
   | 406-410 | offer / withdraw only in `^src/calculationdemand\\.cpp$` | **unchanged**: the choice stays in the reconciler. Add to the comment above: "not the fill or the settle clock". |
   | 427-430 | "the logic components see no widget" | `"src/calculationdemand.*"` becomes `${DEMAND_LAYER}` |
   | 700-702 | "only the application and its views know the demand layer" (`CalculationDemand`) | `^src/calculationdemand\\.(cpp|h)$` becomes `^src/${DEMAND_FILES}\\.(cpp|h)$`. The parts may name the component in comments. |
   | 705-708 | "nothing below the demand layer includes it" | the regex `#include [\"<](\\.\\./)*calculationdemand\\.h` becomes `#include [\"<](\\.\\./)*${DEMAND_FILES}\\.h`. The pathspecs are unchanged, so `DemandIndicator.*` and `PlotRowLayout.h` may include `demandstate.h` no more than today's header. |
   | 725-726 | "pending is the view's presentation of demand" | `^src/calculationdemand\\.(cpp|h)$` becomes `^src/${DEMAND_FILES}\\.(cpp|h)$` |
   | 740-741 | "the load step is the demand layer's scheduler task" (`ColumnFillTask`) | `^src/calculationdemand\\.(cpp|h)$` becomes `^src/(calculationdemand|demandfill)\\.(cpp|h)$` |
   | 744-745 | "hidden loads go through loadPinnedSession" | `^src/calculationdemand\\.(cpp|h)$` becomes `^src/(calculationdemand|demandfill)\\.(cpp|h)$`. The header comment may still name it. |
   | 749-751 | "the demand layer never loads a session or reads a record itself" | pathspec `"src/calculationdemand.*"` becomes `${DEMAND_LAYER}` |
   | 756-757 | "the load bound follows the executor's bound" | path `src/calculationdemand.h` becomes `src/demandfill.h` |

   **Escaping.** In CMake a quoted argument such as
   `"^src/${DEMAND_FILES}\\.(cpp|h)$"` expands the variable and leaves
   `\.`. Check the result by planting a violation once, as the file's head
   comment asks. For example, add a comment `// CalculationDemand` to
   `src/idlescheduler.h`, see the rule trip, then remove it. Do this for
   rules 700 and 705.
3. **Audit: the part boundaries of spec §11.** These are three new rules in
   `audit_group(demand)`, after "the demand layer never loads a session or
   reads a record itself". Each has an "Allow:" comment in the file's style.
   ```cmake
   # The walk and the pass never load or pin: the fill does, and only it.
   # Allow: none expected.
   expect_only("the demand layer loads and pins through its fill only"
     "[.>](loadPinnedSession|pinSession|unpinSession)\\(" "^src/demandfill\\.cpp$" ${DEMAND_LAYER})
   # The fill and the settle clock never call the executor (the fill learns of
   # a shutdown through its owner's hook). Allow: none expected.
   expect_none("the fill and the settle clock never call the executor"
     "[.>](offer|withdrawChosenNext|runningJob|chosenNextJob|publishingJob|isShutDown|isIdle|job)\\("
     "src/demandfill.*" "src/demandsettleclock.*")
   # They expose nothing of the walk: session ids in, session ids and times out.
   # Allow: none expected.
   expect_none("the fill and the settle clock know nothing of the walk"
     "BlockerReport|blockers\\(|DemandTrack|DemandState|DemandCondition|RowStabilityGuard|Settlement"
     "src/demandfill.*" "src/demandsettleclock.*")
   ```
   These rules belong to this phase's structure. Phase 5 adds the rules for
   removed names and the one-computation rule. Plant a hit once for each new
   rule, then remove it.
4. **Acceptance map.** No test function is renamed or removed, so
   `tests/acceptance_map.txt` is unchanged. The two new test functions (see
   Testing) are cited by Phase 5.

**Acceptance Criteria:**
- [ ] `cmake build-phase1/FlySightViewer-build` reconfigures, and `cmake --build build-phase1 --config Release` builds the application and every test.
- [ ] `audit_cleanup` passes, and each rule in the table covers the new files. A planted `#include "demandstate.h"` in `src/sessionmodel.h`, and a planted `m_model->pinSession(` line in `src/calculationdemand.cpp`, each trip their rule. Remove both afterwards.
- [ ] `expect_count("the load bound follows the executor's bound" … 1 src/demandfill.h)` finds exactly one line.
- [ ] `tests/acceptance_map.txt` is unchanged, and the audit's traceability check passes.

**Complexity:** M

---

### Task 2.7: Tests: the edits the removed fields force, the direct offers, two new tests

**Purpose:** The same facts are asserted through the counts, the executor's
records and the existing seams (spec §9 third bullet, §14 "Removed
generality"). Tests that offer directly while a demand layer lives are made
deterministic. The new behaviour of the choice and the settle clock is
covered.

**Files to modify:**
- `tests/tst_calculation_demand.cpp`, `tests/tst_fusion_rows.cpp`, `tests/tst_fusion_store.cpp`, `tests/tst_plot_row_delegate.cpp`, `tests/tst_logbook_indicators.cpp`.

**Technical Approach:** See "Testing Requirements" below. Every edit is
listed there with its replacement.

**Acceptance Criteria:**
- [ ] `grep -rnE "\.settling\b|\.waiting\b|state\.[a-z]+\.at\([0-9]+\)\.job\b|CalculationDemand::(buildToolTip|kToolTipListLimit|kMaxHeldSessions)" tests src` finds nothing. `Seen::job` in `tst_fusion_rows` and `OfferResult::job` are other fields and are allowed.
- [ ] No test function is renamed or removed. The two new ones are declared in `private slots:`.
- [ ] The full suite passes sequentially.

**Complexity:** L

## Testing Requirements

### Unit Tests

**New test functions** (in `tests/tst_calculation_demand.cpp`; declare them
in `private slots:`):

1. `chosenNextJobFollowsTheExecutorsAnswer`. Declare it after
   `executorHoldsAtMostRunningAndChosenNext`.
   1. `giveInput({"s1","s2","s3"}, "G_IN", 4)`, `show({"s1","s2"})`,
      `check("g")`, `gate().waitEntered()`, `m_demand->flush()`.
   2. `const JobId next = m_queue->chosenNextJob();` is s2's
      (`chosenNext().sessionId == "s2"`). Record
      `const int records = m_queue->model()->rowCount();` (2) and a
      `QSignalSpy` on `JobQueue::jobFinished`.
   3. Three passes that find the same choice: `check("plain")`, `flush`,
      `check("plain", false)`, `flush`, `check("plain")`, `flush`. After
      them `m_queue->chosenNextJob() == next`, `stateOf(next) ==
      JobState::Queued`, `rowCount() == records` and `finishedSpy.count() ==
      0`. The executor answered AlreadyActive, and nothing was replaced.
   4. `check("g", false)`: `stateOf(next) == Cancelled` with "No longer
      needed". This part exists today.
   5. The new part is a chosen next job that the demand layer did not offer:
      - `const JobRecord foreign = m_queue->job(m_queue->offer("s3", QStringLiteral("gated")).job);`
        is `Queued`. Do not spin the event loop.
      - `show({"s4"})` (this schedules a pass), then `m_demand->flush()`.
      - `stateOf(foreign.id) == Cancelled`, reason `kNoLongerNeeded`, and
        `!m_queue->job(foreign.id).startedAt.isValid()`.
   6. Finally `check("plain", false)`, `gate().open(1)`, `waitDemandIdle()`,
      and `QCOMPARE(gate().maxRunning.load(), 1)`.
2. `settleClockAnswersItsQuestions`. Declare it in a new section
   `// Parts` before `// Presentation`, and `#include "demandsettleclock.h"`.
   1. `int ended = 0; DemandSettleClock clock(60000, [&ended] { ++ended; });`.
   2. `!clock.hasSettling()` and `clock.nextWaitEnd().isForever()`.
   3. `clock.start("a")`: `isSettling("a")`, `!isSettling("b")` and
      `nextWaitEnd().remainingTime() > 50000`.
   4. `clock.setDelay(50); clock.start("b")`: `nextWaitEnd().remainingTime() <= 50`.
   5. `clock.keepOnly({"a"})`: `!isSettling("b")` and
      `nextWaitEnd().remainingTime() > 50000`.
   6. `clock.start("c")`: `QTRY_COMPARE(ended, 1)`, then `!isSettling("c")`,
      `isSettling("a")` and `hasSettling()`.
   7. `clock.endAll()`: `!hasSettling()`, `nextWaitEnd().isForever()`, and
      after `QTest::qWait(0)`, `ended == 1`.
   8. `clock.setDelay(-5)` gives `delay() == 0`.

**Existing tests: the edits the removed fields force.** Line numbers are at
ec6bd43, before Phase 1's shifts.

`tests/tst_calculation_demand.cpp`:

| Line(s) | Today | Replacement (same fact) |
|---|---|---|
| 355 | `CalculationDemand::kMaxHeldSessions` | `DemandFill::kMaxHeldSessions` (add `#include "demandfill.h"`) |
| 741 | `QCOMPARE(sessionIdsOf(state.waiting), QStringList({"s2", "s3"}));` | delete; `waitingCount == 2` (736) stays |
| 742 | `QCOMPARE(state.running.at(0).job, job1);` | `QCOMPARE(running().id, job1);` (`sessionIdsOf(state.running) == {"s1"}` at 740 stays) |
| 744-745 | `waiting.at(0).job == job2` / `waiting.at(1).job == JobId(0)` | `QCOMPARE(chosenNext().sessionId, QStringLiteral("s2"));` and `QCOMPARE(jobOf("s3", "gated").id, JobId(0));` |
| 746 | `QVERIFY(!state.waiting.at(0).settling);` | `QVERIFY(!m_demand->isSettling("s2"));` |
| 799 | `QCOMPARE(state.waiting.at(0).sessionName, QStringLiteral("Jump 4"));` | `QCOMPARE(m_queue->job(job5).sessionName, QStringLiteral("Jump 4"));`. The waiting track is not listed. The name of s4 is the model's display name, which the job record also carries (Phase 1). |
| 873 | `QCOMPARE(state.running.at(0).job, jobOf("s1", "gated").id);` | `QCOMPARE(running().id, jobOf("s1", "gated").id); QCOMPARE(state.running.at(0).sessionId, QStringLiteral("s1"));` |
| 930 | `QVERIFY(state.waiting.at(0).settling);` | `QVERIFY(m_demand->isSettling("s1"));` |
| 1366-1367 | `waiting.at(0).settling` / `waiting.at(0).job == JobId(0)` | `QVERIFY(m_demand->isSettling("s1")); QCOMPARE(m_queue->chosenNextJob(), JobId(0));` |
| 1375 | `QVERIFY(state.waiting.at(0).settling);` | `QVERIFY(m_demand->isSettling("s1"));` |
| 1410 | `QVERIFY(state.waiting.at(0).settling);` (in the loop) | `QVERIFY(m_demand->isSettling("s1"));` |
| 1474 | `... && !sessionIdsOf(state.waiting).contains("s3")` | `... && state.wantedCount == 2`. s3, the only session without input, is in no count: s1 and s2 are the two wanted tracks at every moment the lambda is used. |
| 1522 | `QVERIFY(state.waiting.isEmpty());` | `QCOMPARE(state.waitingCount, 0);` |
| 1547-1548 | `sessionIdsOf(state.waiting) == {"s2"}` / `waiting.at(0).job == waiting` | `QCOMPARE(state.waitingCount, 1);`. Lines 1543-1545 already assert that the chosen next job is s2's expA. |
| 1852 | `QCOMPARE(g2.running.at(0).job, g.running.at(0).job);` | `QCOMPARE(g2.running.at(0).sessionId, g.running.at(0).sessionId); QCOMPARE(g.running.at(0).sessionId, running().sessionId);` |
| 1853 | `QCOMPARE(g2.waiting.at(0).job, g.waiting.at(0).job);` | `QCOMPARE(chosenNext().sessionId, QStringLiteral("s2"));`. The `waitingCount` equality stays (1846). |
| 1876 | `QCOMPARE(state.running.at(0).job, gated);` | `QCOMPARE(running().id, gated); QCOMPARE(state.running.at(0).sessionId, QStringLiteral("s1"));` |
| 1989, 2004, 2018, 2030, 3956, 3961, 3974, 3989 | `CalculationDemand::buildToolTip(` | `DemandState::buildToolTip(` |
| 2113-2114 | `waiting.at(0).sessionName == "Renamed"` / `waiting.at(0).settling` | `QVERIFY(m_demand->isSettling("s1"));`. Then, to keep "the name is read live at the next pass": `gate().open(3); settle(); QVERIFY(gate().waitEntered()); QTRY_COMPARE(row("Syn/g").running.value(0).sessionName, QStringLiteral("Renamed")); QVERIFY(waitDemandIdle());`. s1 is first in row order and the gate lets it in, so `waitEntered()` sees s1's job. Keep `quiet.holds()` before this block and let `quiet` go out of scope first: put the three `Quiet` lines in a `{ }` block. |
| 2197, 2203 | `QCOMPARE(sessionIdsOf(state.waiting), QStringList({"s3"}));` | `QCOMPARE(state.waitingCount, 1);`. After 2203 also add `QCOMPARE(chosenNext().sessionId, QStringLiteral("s3"));`. 2195 already asserts it before. |
| 2476 | `QVERIFY(state.waiting.isEmpty());       // counted, not listed` | delete (`waitingCount == 4` at 2475 stays) |
| 3444 | `QVERIFY(state.running.at(0).job != 0);` | `QCOMPARE(running().sessionId, QStringLiteral("s1"));` |
| 3449 | `QVERIFY(state.waiting.isEmpty());` | delete |
| 3859 | `QVERIFY(state.waiting.isEmpty());` | delete |
| 3923, 3927 | `kToolTipListLimit` comment / `CalculationDemand::kToolTipListLimit` | `DemandState::kToolTipListLimit` |

`tests/tst_fusion_rows.cpp`:

| Line(s) | Today | Replacement |
|---|---|---|
| 228-229 | `sessionIdsOf(state.running) + sessionIdsOf(state.waiting) + sessionIdsOf(state.failed)` | `sessionIdsOf(state.running) + sessionIdsOf(state.failed)`. A waiting `absent` would make a count exceed 1, which the next check reports. |
| 275-277 | `waiting.at(0).sessionId == "s2"`, `.job == job`, `.calculationTitles == {kTitle}` | delete. Lines 258-262 already assert the chosen next job, its title, and that it is `fitJobOf("s2")`. |
| 294 | `state.running.at(0).job != job` | `state.running.at(0).sessionId != QLatin1String("s2") \|\| m_queue->runningJob() != job` |
| 351 | `QCOMPARE(sessionIdsOf(state.waiting), QStringList({"s1", "s2", "s3"}));` | delete (`waitingCount == 3` stays) |
| 416 | `QCOMPARE(state.waiting.at(0).job, job4);` | delete (415 asserts `chosenNextJob() == job4`) |
| 437-440 | `whileS3Runs.running.at(0).job == job4`, `waiting.at(0).sessionId == "s4"`, `.job == job5`, `.calculationTitles == {kTitle}` | In the `onFirstProgress` lambda also record `runningWhileS3Runs = m_queue->runningJob();`. Assert `QCOMPARE(runningWhileS3Runs, job4);` and `QCOMPARE(m_queue->job(job5).calculationTitle, kTitle);`. The session of job5 is asserted at ~427. |
| 455-470 | `seen.at(i).job` | unchanged: the `Seen` struct's own field |
| 500 | `QCOMPARE(row(id).waiting.at(0).job, job);` | `QCOMPARE(row(id).waitingCount, 1);`, and once after the loop `QCOMPARE(m_queue->chosenNextJob(), job);` |
| 516 | `QCOMPARE(state.running.at(0).job, job);` | `QCOMPARE(state.running.at(0).sessionId, QStringLiteral("s2"));`, and in the lambda record `runningDuring = m_queue->runningJob();` and assert `QCOMPARE(runningDuring, job)` after the wait |
| 542 | `QCOMPARE(state.waiting.at(0).calculationTitles, QStringList({kTitle}));` | `QCOMPARE(m_queue->model()->record(0).calculationTitle, kTitle);`, placed after the `rowCount() == 1` line |
| 576 | `QCOMPARE(row(kRoll).waiting.at(0).sessionId, QStringLiteral("s2"));` | delete. `fitJobOf("s2").id != 0` and `fitJobOf("n1").id == 0` below assert which session waits. |
| 649 | `QVERIFY(state.waiting.at(0).settling);` | `QVERIFY(m_demand->isSettling(QStringLiteral("r1")));` |

`tests/tst_fusion_store.cpp`:

| Line(s) | Today | Replacement |
|---|---|---|
| 561 | `if (m_queue->chosenNextJob() != offered.id \|\| state.waiting.at(0).job != offered.id)` | `if (m_queue->chosenNextJob() != offered.id)`. The `waitingCount == 1` check at 553 stays. |
| 908-909 | `waiting.at(0).settling` / `waiting.at(0).job == JobId(0)` | delete 908 (`isSettling("a")` at 910 stays). 909 becomes `QCOMPARE(m_queue->chosenNextJob(), JobId(0));`. |
| 1270 | `QCOMPARE(state.waiting.at(0).sessionId, QStringLiteral("a"));` | `QCOMPARE(state.wantedCount, 1);`. "a" is the only session. |
| 667, 834, 931, 972, 1304, 1439 | `QCOMPARE(m_queue->offer("a", kFit).kind, Kind::Created); QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));` | `QCOMPARE(fitDirectly(QStringLiteral("a")), QString());` (new helper below) |

The new private helper `[[nodiscard]] QString fitDirectly(const QString &id)`
of `FusionStoreTest` has this doc comment: "The fit of `id` offered to the
executor directly, as a store test needs, with no demand layer alive: the
demand layer is the only offerer and withdraws a chosen next job that nothing
wants. The demand layer is made again afterwards, as after a restart. Empty on
success." Its body:
1. `m_demand.reset();`
2. `const JobQueue::OfferResult result = m_queue->offer(id, kFit);`
3. It returns an error text if `result.kind != Kind::Created`, if
   `!waitIdle(*m_queue, kFitTimeoutMs)`, or if
   `m_queue->job(result.job).state != JobState::Succeeded`.
4. `m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());`
5. `return {};`

Without it, a pass that is pending when the test offers can run before the
executor's queued start and withdraw the test's job. The fit would then end
Cancelled. The zero-interval timer and the queued start have no guaranteed
order. The offers at 635 and 703 expect `NothingToDo`, create no job and stay.

`tests/tst_plot_row_delegate.cpp`:

| Line(s) | Today | Replacement |
|---|---|---|
| 330 | `\|\| sessionIdsOf(state.waiting) != QStringList({"s2"}))` | `\|\| state.waitingCount != 1 \|\| m_queue->job(m_queue->chosenNextJob()).sessionId != QLatin1String("s2"))` |
| 629 | `QCOMPARE(sessionIdsOf(state.waiting), QStringList({"s2"}));` | `QCOMPARE(state.waitingCount, 1); QCOMPARE(m_queue->job(m_queue->chosenNextJob()).sessionId, QStringLiteral("s2"));` |

`tests/tst_logbook_indicators.cpp`, line 630:
`CalculationDemand::buildToolTip(state)` becomes `DemandState::buildToolTip(state)`.

**Signal counts.** `DemandTrack::operator==` no longer compares `settling`
or `job`. A pass in which only a waiting track's settle state or chosen next
job changed therefore announces nothing. The tests that count
`plotStateChanged`, `columnStateChanged` or `statesChanged` are these:
`changeSignalsAreMinimal`, `ordinaryPlotsAreNeverInspected`,
`uncheckedPlotsAreNeverInspected`, `progressUpdatesWithoutInspection`,
`registryChangeReclassifies`, `ordinaryColumnsCreateNoDemand`,
`columnStateCountsAndPendingCells`, `workingIdsFollowStates`,
`plotStateChangeRepaintsRow` and `chainedBlockersContinue`. In each of them,
every counted emission comes with a change of counts, conditions, titles,
progress text or the default state. None depends on the removed fields. If
one fails, the count asserted a removed field and must be restated, not
worked around.

### Integration Tests

- The full suite, sequentially:
  `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`.
  The load-sensitive fusion tests (`tst_fusion_rows`, `tst_fusion_store`,
  `tst_fusion_jobs`, `tst_fusion_session`) must pass without `-j`.
- `audit_cleanup`, including its traceability check.

### Manual Verification

1. Build `build-phase1` and run the application. Check a fusion plot with a
   visible session. The row turns and shows "0 of 1", its hover lists the
   running fit with progress text, and when the fit finishes the row goes
   plain. This is identical to before the phase.
2. Enable a logbook column over a requested output on a logbook with unloaded
   sessions. The header turns and the progress line counts the fill down to
   its end. Disable the column mid-way: the line ends, and no session stays
   pinned (the pool shrinks at the next eviction).
3. Edit an input of a session that a checked requested plot reads. The row
   works at once, and the job starts about one second after the last edit.

## Notes for Implementer

### Build and test (from the overview's Decisions & Constraints)
- Build **only** `build-phase1/`: `cmake --build build-phase1 --config Release`. **Never build `build/`.**
- This phase **adds source files**. Reconfigure the inner project once, after adding them to `src/CMakeLists.txt`: `cmake build-phase1/FlySightViewer-build`. Then build.
- Test with `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`, **sequentially, never `-j`**, with no stray `ctest` or `tst_*` process running (executor-driven fusion tests are load-sensitive). One target: `-R tst_calculation_demand`. The audit: `-R audit_cleanup`.
- No new test target is added, so `_FLYSIGHT_GTSAM_REACHERS` in `cmake/SolverDependencies.cmake` needs no change.
- Commits are the orchestrator's. Never run a git command that changes repository state.

### Gotchas
- **`Q_DECLARE_TR_FUNCTIONS` ends in `private:`.** Make it the last line of `DemandState`. Otherwise every member after it becomes private, and the tests' aggregate-style writes (`state.failed = …`) stop compiling.
- **The hooks capture `this`.** `~CalculationDemand` must call `m_fill->detach()` itself, right after removing the registry observer. The fill's own destructor calls `detach()` again, which is idempotent. `DemandFill` and `DemandSettleClock` are non-copyable: `TaskDef` lambdas and the timer connection capture their `this`. Create both parts at the top of the constructor body, before any `connect` and before `syncCheckedSet(); scheduleUpdate();`.
- **The inert case.** `hasWork()` must be false for an inert component even while an old `m_remaining > 0` remains. That is why `enabled` includes `!isInert()`. It matches today's `hasFillWork()`, which tested `isInert()` live. `m_fill->update({}, {})` in the inert branch releases every hold; with a null session model it only clears the list.
- **Audit text rules match comments too.** Never write `->offer(`, `.offer(`, `->withdrawChosenNext(`, `->pinSession(`, `->unpinSession(` or `.loadPinnedSession(` in a comment of a demand-layer file. Write "JobQueue::offer()" or "loadPinnedSession()" with no member-access prefix. Do not name `BlockerReport`, `DemandTrack`, `DemandState`, `Settlement` or `RowStabilityGuard` anywhere in `src/demandfill.*` or `src/demandsettleclock.*`, comments included.
- **`AlreadyActive` for the running job** is not a choice: continue. Only an answer that names the chosen next job ends the loop. Do not "simplify" it to `return` for every `AlreadyActive`.
- **`onJobProgress` matches by session.** Check `record.state == JobState::Running` so that a stale or hand-made id changes nothing.
- **Do not change the order of the pass beyond what Task 2.4 lists.** Column cells are classified before the offers, and plot tracks after them, as today. Phase 3 changes the walk, not this phase.
- **`fillTaskReportsProgressWhileWaiting` and `fillEndingBehindAnotherTaskStartsNextCountFresh`** (Phase 1) observe `progressChanged(ColumnFillTask, …)`. The progress lambda must read the fill's own `m_remaining` / `m_highWater`, and `update()` must wake the scheduler exactly when today's snapshot comparison did.
- `kMaxHeldSessions` must be spelled `static constexpr int kMaxHeldSessions = JobQueue::kMaxRunningJobs + 1;` for the audit's count regex.

### Decisions Made
- **File layout.** The parts are flat files beside the component, following the repository's one-class-per-file-pair style:
  - `src/demandstate.{h,cpp}`: `DemandCondition`, `DemandTrack`, `DemandState`.
  - `src/demandsettleclock.{h,cpp}`: `DemandSettleClock`.
  - `src/demandfill.{h,cpp}`: `DemandFill`.

  The reconciler is `CalculationDemand` itself in `src/calculationdemand.{h,cpp}`. Spec §11 puts the pass and the choice in the reconciler, and those are the component's core. A separate reconciler class would only forward the slots. The audit names the four files through `DEMAND_LAYER` / `DEMAND_FILES`.
- **The parts are plain C++ classes.** They have no `QObject` and no signals, and call back through `std::function`, following the `TaskDef` pattern of `idlescheduler.h`. The fill's callbacks are the `Hooks` struct, and the clock has one `waitEnded` callback. The component stays the only `QObject` and the only thing the views observe.
- **The fill never names the executor.** It learns "the executor is shut down / the component is inert" through its `enabled` hook. That way the audit can forbid every executor call in the fill.
- **The walk reads a copy of the holds** (`QSet<QString> held` parameter) instead of calling the fill. This keeps today's `!m_held.contains(sessionId)` exactly. **The running record is read once before the walk** and passed to `walkColumns()` and `plotCandidates()`. The plot classification after the offers re-reads it, as today.
- **`classify()` and `walkColumns()` lose `chosen`.** It was used only for `DemandTrack::job`.
- **Presentation behaviour is on `DemandState`:** `addTrack()`, `finish()`, `static buildToolTip()` and `kToolTipListLimit`. The views and tests include only `demandstate.h` (or `calculationdemand.h`) for them. `CalculationDemand::buildToolTip` is gone, and the tests call `DemandState::buildToolTip`.
- **`kMaxHeldSessions` moves to `DemandFill`.** The fill's contract owns the bound, and the walk reads `DemandFill::kMaxHeldSessions`. **`kInputSettleMs` stays on `CalculationDemand`.** It is the component's policy, and the clock is constructed with it.
- **Progress text follows the executor's record**, matched by session among the running tracks, not by a job id stored in the track.
- **The executor's answer decides.** `AlreadyActive` naming the chosen next job ends the choice. Nothing chosen withdraws the chosen next job, and so does the inert branch. An inert component has never offered, but the executor's chosen next job, if any, is by the audit the demand layer's own.
- **`tst_fusion_store` gets `fitDirectly()`.** It removes the demand layer while it offers directly, because the removed own-offer rule no longer protects a test's direct offer.
- **Three new audit rules encode the part boundaries of spec §11** (loads and pins only in the fill; the fill and the clock never call the executor; they know nothing of the walk). They are added now because they check this phase's structure. Phase 5 adds the rules for removed names.
- **"Hold nothing once the executor is shut down" (spec §11) is implemented as today's behaviour.** Once the executor is shut down, the fill has no work and loads nothing. Its holds are released by the normal release rule or by `detach()` when the component goes. Releasing them at the shutdown itself would contradict two existing tests: `noLoadsAfterExecutorShutdown` asserts `heldSessionIds() == {"s1","s2"}` after the shutdown (~3805), and `demandDestroyedReleasesHoldsAndTask` asserts that s1 and s2 are still pinned after the shutdown (~3824-3825). This phase is behaviour-preserving. In the product, `MainWindow` destroys the demand layer right after closing the executor, so the difference is invisible. See Open Questions.

### Open Questions
- **Spec §11's "hold nothing once the executor is shut down."** This plan keeps today's behaviour (see Decisions). If Michael wants the literal reading, it is a small change:
  - in `DemandFill::update()`, release every hold when `!m_hooks.enabled()`;
  - `noLoadsAfterExecutorShutdown` then expects `heldSessionIds().isEmpty()` after the shutdown;
  - `demandDestroyedReleasesHoldsAndTask` checks the pins before the shutdown instead.

  Phase 3 or a fixup can do it.

### Hand-off to later phases
- **Phase 3** replaces the internals of `walkColumns` / `inspect` / `classify` / `classifyUnloaded` / `Memory` / `Settlement` in `calculationdemand.cpp`. The interfaces it must keep:
  - after the walk, `m_fill->update(pendingSessions, loadCandidates)`;
  - `m_settle->isSettling(id)` / `start(id)` / `keepOnly(ids)` / `dropExpired()`;
  - `DemandState::addTrack()` / `finish()`;
  - the fill hooks `onFillLoadFailed(id)` (to become a pair-memory failed load) and `onFillLoaded(id, held)`.
- **Phase 4** (views): the views include `demandstate.h` plus, in their `.cpp`, `calculationdemand.h`. `DemandIndicator.*` and `PlotRowLayout.h` must still not include any demand-layer header (audit rule 705).
- **Phase 5** (documentation and audit). Passages that this phase makes false:
  - `docs/CALCULATIONS.md`:
    - ~1198 (`Waiting` "with `settling`");
    - ~1246 (the `waiting` list);
    - ~1248 and ~1262 (`buildToolTip` is now `DemandState::buildToolTip`);
    - ~1259 (`settling` field);
    - ~1396 (the own-offer rule);
    - ~1421 (`with settling`);
    - ~1540 (`CalculationDemand::kMaxHeldSessions`, now `DemandFill::kMaxHeldSessions`);
    - ~1610 and ~1615 (the constants and API table);
    - §16 in general: the parts and their files.
  - `tests/README.md`: ~1137, ~1226 and the `tst_calculation_demand` row.
  - The root `README.md` source tree (~369): add the three part files.
  - Removed names for the demand group's rules: `m_offeredJob`, `withdrawOwnOffer`, `DemandTrack::settling`, `DemandTrack::job`, `DemandState::waiting`, `CalculationDemand::buildToolTip`.
  - New test functions to cite: `chosenNextJobFollowsTheExecutorsAnswer`, `settleClockAnswersItsQuestions`.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria.
2. All tests pass: the full `ctest` run on `build-phase1`, sequential, including `audit_cleanup` and its traceability check.
3. The code follows the patterns of the reference files: plain values out of the one guarded read, direct connections that only drop state and schedule, one owner per fact, and the `TaskDef` style of callbacks.
4. No TODOs or placeholder code remain. No comment in `src/` names a removed member (`m_offeredJob`, `withdrawOwnOffer`, `settling` as a field, the track's job id, the state's waiting list), and `calculationdemand.h` describes the parts.
