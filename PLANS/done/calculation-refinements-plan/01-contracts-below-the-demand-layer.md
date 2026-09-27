# Phase 1: Contracts below the demand layer

## Overview

This phase gives the components below the demand layer the contracts that the
demand layer currently works around (spec §5, §6, §7 store side, §8, §9
executor side). The idle scheduler completes a waiting task whose work is gone.
The logbook manager announces a changed record reason. The result store
announces a record write that failed. The session model becomes the one source
of each column's static dependency closure, its requested calculations and a
row's display name, and its bulk edit announces its edit as a dependency
change. The executor loses its idle and queued signals and its two-job query,
and it names sessions by the model's display name. In `CalculationDemand` the
phase removes only the workarounds that these contracts make obsolete: the
fill's ending state and step, the single-row display-change slot (its reason
comparison and its bulk-edit clearing alike), the demand layer's own column
tables with their dirty flags and relevant-name union, and its own
display-name code. Every other part of the demand layer, including its
settlements, memory, walk and fields, stays as it is for Phases 2 and 3.

## Dependencies

- **Depends on:** None. The phase can begin immediately.
- **Blocks:** Phase 2 (Demand layer in parts), and through it Phases 3-5.
- **Assumptions:** The branch `store-requested-calculations` is at `ec6bd43`
  or later, and all tests and `audit_cleanup` pass there. `build-phase1/`
  is configured. This phase adds no source file, so no reconfigure is needed.

## Tasks

### Task 1.1: The idle scheduler completes a waiting task whose work is gone

**Purpose:** A task that waits on something outside the scheduler can lose its
work without a step. The scheduler must then complete it: report its final
progress and call `onComplete(false)`. This replaces the fill's fake final
step (spec §5).

**Files to modify:**
- `src/idlescheduler.h`: update the class comment and the doc of `TaskDef::canStep`. No new API.
- `src/idlescheduler.cpp`: add the completion rule at the head of `tick()`.
- `tests/tst_session_model_engine.cpp`: add one test function (see Testing).

**Technical Approach:**

Today the only completion paths are the following. After a step, `tick()`
checks the task that stepped (idlescheduler.cpp ~143):

```cpp
    // Check completion of the task that stepped
    if (const Entry *entry = find(steppedId); entry && entry->def.hasWork && !entry->def.hasWork()) {
        const CompleteFn complete = entry->def.onComplete;
        if (complete)
            complete(false);
        if (m_activeTask == steppedId)
            m_activeTask = -1;
    }
```

`cancel(id)` (idlescheduler.cpp ~48) calls `onComplete(true)` and clears
`m_activeTask` when `id` was the active task. When no task has work,
`tick()` goes idle (~104, `// No work — go idle`) and completes nothing.

**The rule.** Add it as the first thing `tick()` does, before the existing
scan for `activeId` and `steppedId`:

1. If `m_activeTask >= 0`, the scheduler looks up the entry registered under
   it (`find()`). The rule applies only when all of these hold: the entry
   exists, it was registered with a non-null `canStep` (a *waiting* task: see
   the decision below), its `hasWork` is non-null, and `hasWork()` now
   returns false.
2. Remember `const TaskId gone = m_activeTask;` and call
   `reportProgress(gone)`: the final progress, reported once.
3. A slot of `progressChanged` may unregister the task. Look the entry up
   again. If it is still registered, copy its `onComplete`, set
   `m_activeTask = -1`, and call the copy with `false` (not cancelled). If
   it is gone, only set `m_activeTask = -1`. An unregistered task never has
   `onComplete` called, which is the existing `unregisterTask` contract.
4. Continue with the existing body unchanged. Because `m_activeTask` is now
   -1, the scan reports whatever has work next as a change
   (`activeTaskChanged` plus its progress). If nothing has work, the existing
   branch emits `schedulerIdle`, because `m_idle` is still false. So the
   completion comes *before* the next active task is reported or the
   scheduler goes idle, as spec §5 requires.

Doing this at the head of the tick matters. `onComplete` may give work to
other tasks, or take it away. The scan that follows sees the state that
`onComplete` left.

**No double completion.**
- The step path already sets `m_activeTask = -1` when the stepped task was the
  active one and completed, so the rule cannot fire for it on the next tick.
- `cancel()` sets `m_activeTask = -1` after `onComplete(true)`. A cancelled
  task that then has no work is therefore never completed a second time,
  which keeps spec §5's "completes once, as cancelled, exactly as today".
- The rule clears `m_activeTask` itself, so it fires at most once for each
  time a task is reported active.

**Who wakes the scheduler.** The rule runs only on a tick. A scheduler that
rests on a waiting task (the timer is not re-armed) must be woken by whatever
takes the task's work away. The component that owns a waiting task already
has this duty for changes of `canStep`. The fill meets it:
`CalculationDemand::recompute()` calls `scheduler().wake()` whenever its fill
snapshot changes (calculationdemand.cpp ~1326), and `hasFillWork()` is part
of that snapshot.

**The rule applies to tasks that can wait (registered with `canStep`) only,
as spec §5 has it.** The reason, reading `SessionModel`'s code:

- **LoadTask** (`hasWork`: `!m_loadQueue.isEmpty()`). `setRowsVisibility`
  empties the queue without a step when the queued stubs are hidden
  (`m_loadQueue.removeOne`, sessionmodel.cpp ~895). It also empties it when a
  small batch loads everything synchronously (`m_loadQueue.clear()`, ~925).
  `removeSessions` (~992) and `flushDirtySessions` (~1855) do the same.
- **SaveTask** (`hasWork`: some row `dirty && !saveFailed`). An eviction
  saves a dirty row (`evictSession` → `saveLoadedRow`) during a synchronous
  load. `removeSessions` erases dirty rows. `flushDirtySessions` saves
  everything.
- **ColumnTask** (`hasWork`: some row `needsColumnWork`). A load
  (`sessionRef` → `fillMissingColumns`), `removeSessions` or
  `flushDirtySessions` can remove the last row that needs work.
- **BulkEditTask** (`hasWork`: `!m_bulkEditQueue.isEmpty()`).
  `flushDirtySessions` clears the queue (~1861).

If the rule applied to these tasks, they would complete in these cases where
today they never complete. One case would be a new bug: hiding the rest of a
background load batch would make `LoadTask`'s `onComplete(false)` emit
`visibilityChanged(m_loadedDuringBatch, {})` for sessions the user has just
hidden. None of these four registrations passes `canStep` (sessionmodel.cpp
34-118: each `TaskDef` ends at `cancellable`). Scoping the rule to waiting
tasks therefore keeps their completion moments unchanged by construction. The
fill is the only waiting task in product code.

**Header text** (`src/idlescheduler.h`). Extend the class comment after the
paragraph on waiting tasks. It must say that a task registered with `canStep`
may also lose its work outside the scheduler, without a step, and that when
the task the scheduler last reported active is such a task and has no work any
more, the next tick reports its progress one last time and calls its
`onComplete(false)` before it reports the next active task or goes idle. It
must also say that every other task is completed only through its step (its
`hasWork()` is false after the step) or through `cancel()`, and that whoever
takes a resting waiting task's work away wakes the scheduler. Add one sentence
to the `canStep` doc that refers to this. The audit forbids some words in
these files: see Gotchas.

**Acceptance Criteria:**
- [ ] A task registered with `canStep` that is reported active and loses its work without a step gets exactly one `progressChanged(id, …)` followed by exactly one `onComplete(false)` on the next tick, before any `activeTaskChanged` for another task and before `schedulerIdle`, and it is never stepped for it.
- [ ] A waiting task that was `cancel()`ed gets `onComplete(true)` once and no `onComplete(false)` afterwards when it has no work.
- [ ] A task registered without `canStep` that loses its work without a step is not completed (today's behaviour). `SaveTask`, `LoadTask`, `BulkEditTask` and `ColumnTask` registrations are unchanged.
- [ ] A task unregistered while active is never completed.
- [ ] `src/idlescheduler.*` still pass the audit rules "the idle scheduler learns nothing about jobs or demand" and "the idle scheduler is never paused for a calculation".

**Complexity:** M

---

### Task 1.2: The fill drops its ending state and step

**Purpose:** With Task 1.1 in place, the fill no longer needs a step that loads
nothing to report its end (spec §5 consequences).

**Files to modify:**
- `src/calculationdemand.h`: remove `isFillEnding()` and `m_fillEnding`, and rewrite the HIDDEN LOADS paragraph and the `hasFillWork()` test-seam comment.
- `src/calculationdemand.cpp`: `registerFillTask()`, `hasFillWork()`, `isFillEnding()` (delete it), `runLoadStep()`, and the fill numbers at the end of `recompute()`.
- `tests/tst_calculation_demand.cpp`: adjust `fillTaskReportsProgressWhileWaiting` and one comment, and add one test (see Testing).

**Technical Approach:**

Remove the following:
- `bool m_fillEnding` (header ~561) and `bool isFillEnding() const` (header
  ~490, cpp ~872).
- In `registerFillTask()` (cpp ~844): the line `m_fillEnding = false;` in
  `onComplete`, and the clause `|| isFillEnding()` in `canStep`. `canStep`
  becomes `[this] { return canLoad(); }`. `onComplete` keeps only
  `if (m_fillRemaining == 0) m_fillHighWater = 0;` and its comment.
- In `hasFillWork()` (cpp ~867): `(m_fillRemaining > 0 || m_fillEnding)`
  becomes `m_fillRemaining > 0`. Rewrite its comment ("The fill has work
  while a session has a pending cell").
- In `runLoadStep()` (cpp ~884): the block
  ```cpp
      if (isFillEnding()) {
          // The last step of a fill loads nothing: ...
          m_fillEnding = false;
          return;
      }
  ```
- In `recompute()` (cpp ~1315):
  ```cpp
      if (fillRemaining == 0 && m_fillRemaining > 0)
          m_fillEnding = true;
      else if (fillRemaining > 0)
          m_fillEnding = false;
  ```

**New in `recompute()`: a fill that starts from nothing starts its own
count.** Replace `m_fillHighWater = qMax(m_fillHighWater, m_fillRemaining);`
with the following. When the previous `m_fillRemaining` was 0 and the new
`fillRemaining` is greater than 0, set `m_fillHighWater = fillRemaining`.
Otherwise take the maximum as before. Then assign `m_fillRemaining`.

This is needed because the scheduler completes only the task it *last reported
active* (spec §5). A fill that loses its work while another task is active
(for example, a save or column work takes the ticks while the last job ends)
is never completed, so its `onComplete` never resets the high-water mark.
Today the ending state kept the fill's work alive until the fill was stepped,
so completion was guaranteed. Without the fresh start, the next fill would
report the stale total. The progress is still "sessions with a pending cell of
the fill's high-water mark". The mark still resets when the task completes,
and in addition a fill that starts while no session had a pending cell starts
its own count.

Keep the wake at the end of `recompute()` unchanged: `hasFillWork()` is in
`fillSnapshot`, so the pass that finds the last pending cell resolved wakes
the scheduler. The scheduler's new rule then reports
`progressChanged(ColumnFillTask, 0, n)` and calls `onComplete(false)`, which
resets the mark, and the scheduler goes idle.

**Comments to update:**
- The HIDDEN LOADS paragraph in `calculationdemand.h` (~227-233). Replace
  "one last step, which loads nothing, reports the fill complete. Its other
  steps are loads" with: the scheduler completes the fill when it has no
  pending cell left, reporting its final progress; its steps are loads.
- The comment at `tests/tst_calculation_demand.cpp` ~3130 ("The fill's last
  step reports it complete, and then it has no work").

**Acceptance Criteria:**
- [ ] `isFillEnding`, `m_fillEnding` and the ending branch of `runLoadStep()` no longer exist, and `canStep` is `canLoad()` alone.
- [ ] Right after the pass that resolves the last pending cell (in the `jobFinished` slot of the last job, after the demand layer's own slot), `hasFillWork()` is false.
- [ ] The progress line still reports `ColumnFillTask` from its first report to a final `(0, n)`, immediately followed by `schedulerIdle`, with the total `n` constant within a fill (`fillTaskReportsProgressWhileWaiting`, `fillTaskIsLowestAndNotCancellable`).
- [ ] A fill that ends while another task is active does not leak its total into the next fill: the next fill's first report is `(k, k)`.

**Complexity:** M

---

### Task 1.3: The logbook manager announces a changed record reason

**Purpose:** Recording a reason that differs from what the index held emits
`calculationRecordsChanged(sessionId, calculationId)`, the same signal as a
record write or removal (spec §6).

**Files to modify:**
- `src/logbookmanager.h`: the doc of `setCalculationRecordReason` (~209-216, "Emits nothing; marks the index for a flush when the value changed"), the RECORD STAMPS paragraph of the class comment (~88-99), and the doc of the `calculationRecordsChanged` signal (~377). Add a private helper declaration.
- `src/logbookmanager.cpp`: `setCalculationRecordReason` (~1305) and its three internal callers (~1159, ~1241, ~1259).
- `src/calculationresultstore.h`: the bullet "The logbook index learns each record's outcome …" (~40-45). Add that a changed reason is announced as a record change.
- `tests/tst_logbook_index.cpp`, `tests/tst_result_store.cpp`: see Testing.

**Technical Approach:**

1. Move the current body of `setCalculationRecordReason` into a new private
   helper, `bool storeRecordReason(const QString &sessionId, const QString
   &calculationId, const QString &reason)`. It returns true exactly when the
   stored value changed, which is exactly when it sets `m_indexNeedsFlush =
   true` today. The current body has two early returns for "nothing
   changed": the empty reason with nothing stored, and `stored == reason`.
   Keep the "non-empty reasons only" rule.
2. The public `setCalculationRecordReason` becomes:
   `if (storeRecordReason(...)) emit calculationRecordsChanged(sessionId, calculationId);`.
   An unchanged reason emits nothing.
3. Change the three internal callers to call `storeRecordReason`, so that each
   path keeps **exactly one** emission:
   - `writeCalculationRecord` step f (~1159):
     `setCalculationRecordReason(sessionId, calculationId, record.result.detail); emit calculationRecordsChanged(...)`.
   - `removeCalculationRecordOfStem`, absent-file branch (~1241) and removed
     branch (~1259). Both emit right after.

   Tests count these emissions (`tst_result_columns::managerDropsDependentValues`:
   `QCOMPARE(spy.count(), 1)` after a write; `tst_result_records::writeFailureCacheFolderNotCreated`).
4. Leave `adoptRecordReasons` (initialize), `remapSessionId` and
   `removeSession` alone. They do not go through the setter and emit nothing,
   as today.

The only external caller is `CalculationResultStore::restoreSession` step 4
(calculationresultstore.cpp ~285-286), which reports the reason of every
record it read and kept. A reason that changes there (a record of an earlier
build, or an index whose `"recordReasons"` was lost) is now announced from
inside the restore. Records that restores already delete or skip emit
`calculationRecordsChanged` from inside the same restore today, so both
listeners handle this already:
- `SessionModel::onCalculationRecordsChanged` (sessionmodel.cpp ~1653) drops
  the row's cached and pending values of the columns over that calculation
  and defers. For a stub row it calls `startColumnWorker()`. For a loaded row
  it calls `queueRecordColumnRefresh`.
- `CalculationDemand::onCalculationRecordsChanged` (cpp ~1536) drops the
  memos and settlements and schedules a pass.

Consequence, which is expected and should be noted in the code comment: when a
reason is learned, the values over that record are dropped and computed again
once, with identical values. Only the first restore that learns the reason
does this. A later restore finds the same reason and emits nothing, so there
is no loop.

**Acceptance Criteria:**
- [ ] `setCalculationRecordReason` with a value different from the stored one (including clearing a non-empty one) emits `calculationRecordsChanged(session, calculation)` once. The same value, or clearing an absent one, emits nothing. `indexNeedsFlush()` behaves as before.
- [ ] `writeCalculationRecord` and `removeCalculationRecord` still emit exactly one `calculationRecordsChanged` per call, whether or not the reason changed.
- [ ] A restore (a load, or the column worker's copy) that learns a new reason emits `calculationRecordsChanged` for that pair. A restore that finds the reason it already knows emits nothing for it.

**Complexity:** S

---

### Task 1.4: The bulk edit announces its edit; the demand layer drops its display-change slot

**Purpose:** Task 1.3 announces the reason, so the demand layer no longer
needs the model's display change for it. The slot's other role, learning of
a bulk edit of a session that is not loaded, goes the same way: the bulk
edit announces its edit as a dependency change, as a direct edit does, and
the demand layer observes no display change of the model at all (spec §6).

**Files to modify:**
- `src/sessionmodel.cpp`: `processNextBulkEdit` (~2229-2360), both edit paths and the trailing emission.
- `src/sessionmodel.h`: the doc of `dependencyChanged` (~373) and the BULK EDIT paragraph of the class comment, if one names what the bulk edit emits.
- `src/calculationdemand.cpp`: delete `onSessionDataChanged` (~1493-1532) and its connection in the constructor (~104); the comment above `recordSet()` (~758-760); the comments of `onDependencyChanged` (~1408-1440).
- `src/calculationdemand.h`: delete the declaration of `onSessionDataChanged`; the SETTLEMENTS, MEMORY and WHEN A PASS RUNS paragraphs where they name the single-row display change.
- `tests/tst_session_model_engine.cpp`, `tests/tst_calculation_demand.cpp`: see Testing.

**Technical Approach:**

**1. The bulk edit publishes what it changed.** Today `processNextBulkEdit`
ends with a bare display change (`emit dataChanged(index(row, 0), index(row,
columnCount() - 1), {Qt::DisplayRole})`, ~2356) on every path, and neither
edit path emits `dependencyChanged`. A direct edit (`setData`, ~562) ends
with `publishInvalidation(index.row(), changedNames)`, which emits the row's
display change and one `dependencyChanged(sessionId, key)` per changed name.
The bulk edit does the same:

- **Loaded path.** `session.setAttribute(attributeKey, newVal)` returns the
  names the engine invalidated, exactly as `item.setAttribute(...)` does in
  `setData` (~522 and ~536, `changedNames = item.setAttribute(...)`). Keep
  the returned set.
- **Stub path.** The edit is made on the temporary copy, which no engine of
  the row's own reflects. Publish the attribute's own name,
  `{DependencyKey::attribute(attributeKey)}`: every requested source whose
  static closure reads the attribute has that key in its closure, which is
  what the demand layer's relevance test reads (Task 1.7,
  `isRelevantName`). The same set is used on the branch that promotes the
  copy to a loaded row after a failed save.
- **At the end of the step**, where the bare `dataChanged` is emitted today:
  when an edit was applied (either path, whether or not the save succeeded),
  call `publishInvalidation(row, names)` with the set above, and nothing
  else; `publishInvalidation` emits the display change itself (with the
  display, edit and check-state roles, as a direct edit does). When no edit
  was applied (a failed-load placeholder, a stub whose load failed, an
  unsupported format), keep today's bare display change so the view
  repaints as it does now. `row` may have been remapped by
  `setRowSessionId` earlier in the step; `publishInvalidation` reads the
  row's current id, which is what is wanted.
- `dependencyChanged`'s doc gains: "A bulk edit publishes the names it
  changed on both of its paths (a loaded session, and the temporary copy of
  one that is not loaded), so a listener needs no other signal to learn of
  it."

The demand layer's slot for `dependencyChanged` (`onDependencyChanged`)
already does, for a relevant key, exactly what the display-change slot did
for a bulk edit and more: it drops the session's report memo, clears the
session's memory and settlements (`m_memory.removeIf`, `eraseSettlements`),
starts the session's settle wait, and schedules a pass. A bulk edit of a
session that is not loaded therefore starts that session's settle wait,
which is the treatment an input change gets (spec §6 of the demand-driven
specification); the fill's load candidates exclude a settling session until
the wait ends.

**2. The demand layer drops the slot.** Delete `onSessionDataChanged` and
the line `connect(m_sessionModel, &QAbstractItemModel::dataChanged, this,
&CalculationDemand::onSessionDataChanged);`. The three facts the slot
carried are all announced elsewhere now:
- a reason the index learned: `calculationRecordsChanged` (Task 1.3);
- a bulk edit of a session that is not loaded: `dependencyChanged` (item 1);
- a loaded row's engine changed by the bulk edit's loaded path (the memo
  drop `m_columnReports.remove(sessionId)`): `dependencyChanged`, whose
  slot drops the memo first thing.

The column worker's display change for every stub it processes reaches
nothing in the demand layer any more. Nothing in the demand layer may
connect to `QAbstractItemModel::dataChanged` of the session model after
this task; Phase 5 adds an audit rule for it.

**3. Comments.**
- The comment above `recordSet()` changes from "(calculationRecordsChanged)
  or of what a restore learned of them (a single-row display change)" to
  "a record change, which the manager also emits when a restore teaches it
  a record's reason".
- SETTLEMENTS (calculationdemand.h ~183-187): "Cleared: for a session, by
  its relevant input change (a bulk edit publishes one)"; delete "and by a
  single-row display change while it is not loaded (a bulk edit, the
  column worker)".
- MEMORY (~273-274): unchanged in substance ("cleared by its input
  change").
- WHEN A PASS RUNS (~283-284): delete "and a single-row display change of
  a session that is not loaded when it changed what this component knows
  of it".
- `onDependencyChanged`'s comments: add "The bulk edit publishes its edit
  this way too, for a loaded session and for a stub."

**Why the reason path is safe at the end of Phase 1.** Trace for
`storedRejectionIsBadgedAfterRestartWithoutLoad`, third section (an index
without `"recordReasons"` and without the EA1 value of s2):
1. The column worker step (`processNextDirtyColumn` → `restoreForColumnWorker`
   → `restoreSession` step 4) calls `setCalculationRecordReason(s2, expA,
   "negative input")`. The value changed, so the manager emits
   `calculationRecordsChanged(s2, expA)`.
2. `SessionModel::onCalculationRecordsChanged` finds nothing to drop (EA1 is
   missing, not cached), so nothing is dropped.
3. `CalculationDemand::onCalculationRecordsChanged` removes `m_recordSets[s2]`,
   `m_recordReasons[s2]`, `m_columnReports[s2]` and the settlements of the
   columns over expA (there are none), then schedules a pass.
4. That pass calls `classifyUnloaded` for (s2, EA1). `recordSet(s2)` is looked
   up again and returns `{expA}` with reason "negative input", so the cell is
   Failed with reason "Explicit A: negative input", titles `["Explicit A"]`,
   `jobFailure` false, and name "Jump 2" (the model's display name, Task
   1.7). No load happens.

The worker's later `dataChanged` for the stub reaches nothing.

**Why the bulk-edit path is safe.** Trace for
`bulkEditMakesSettledSessionApplicable`: s2, a stub settled not applicable
for `DESC_OUT` (its `_DESCRIPTION` was removed), gets a bulk edit of
`_DESCRIPTION`. The stub path loads the copy, applies the edit, saves, and
publishes `{attribute("_DESCRIPTION")}`. `onDependencyChanged` finds the key
in the closure of `DESC_OUT` (the description calculation reads it), clears
s2's memory and settlements, starts s2's settle wait and schedules a pass.
The pass finds s2 waiting and settling: no load yet. When the wait ends
(the test's `settle()`), the next pass lists s2 as a load candidate, the
fill loads it, the job runs, and the column fills, as the test asserts.

**Acceptance Criteria:**
- [ ] A bulk edit of an attribute on a stub emits exactly one `dependencyChanged(sessionId, DependencyKey::attribute(key))` and one row `dataChanged`, in that order of `publishInvalidation`; on a loaded row it emits one `dependencyChanged` per name `setAttribute` returned. A bulk edit that applies nothing (a failed-load placeholder) emits no `dependencyChanged`.
- [ ] `src/calculationdemand.cpp` has no `onSessionDataChanged` and no connection to `QAbstractItemModel::dataChanged`; `CalculationDemand` calls `calculationRecordReason` only in `recordSet()`.
- [ ] `storedRejectionIsBadgedAfterRestartWithoutLoad` passes unchanged in what it asserts, and it observes the `calculationRecordsChanged(s2, expA)` that carried the reason.
- [ ] With the manager's signals blocked during the worker's pass, the demand layer does *not* list s2 as failed after that pass. A manual `calculationRecordsChanged(s2, expA)` then lists it.
- [ ] `bulkEditMakesSettledSessionApplicable` passes with the settle call of the Testing section; a bulk edit of an attribute that is in no requested source's closure clears nothing and starts no settle wait; the column worker's processing of a settled stub clears nothing (`notApplicableSessionIsSettledWithoutAJob`, `columnFailuresAreBadgedNotReloaded` pass unchanged).

**Complexity:** M

---

### Task 1.5: The result store announces a record write that failed

**Purpose:** Publish the fact "the record of an Ok result could not be
encoded or written" with its reason for the (session, calculation). Phase 3
turns it into a shown failure (spec §7). Nothing consumes it in product code
in this phase.

**Files to modify:**
- `src/calculationresultstore.h`: make the class a `QObject` with one signal, and update the class comment.
- `src/calculationresultstore.cpp`: emit in the write-failure branch.
- `src/sessionmodel.h`, `src/sessionmodel.cpp`: a relay signal and its connection.
- `tests/tst_result_store.cpp`: see Testing.

**Technical Approach:**

1. `class CalculationResultStore : public QObject` with `Q_OBJECT`,
   `#include <QObject>`, and `explicit CalculationResultStore(QObject *parent
   = nullptr);`. It stays a by-value member of `SessionModel`, declared
   before `m_rows`. Tests that create one directly
   (`tst_result_store.cpp::alreadyInstalledIsKept`: `CalculationResultStore
   store;`) keep compiling. The class is in `flysight_core`, whose AUTOMOC
   processes the header, so no CMake change is needed.
2. Add the signal
   `void recordWriteFailed(const QString &sessionId, const QString &calculationId, const QString &reason);`.
   Its doc must say: the record of an Ok result installed for the pair could
   not be encoded or written; `reason` is the manager's error text and is
   never empty; it is emitted from inside the engine's explicit-result
   listener, so a directly connected slot may only record state and schedule;
   nothing is persisted.
3. Emit it where the failure is counted, in `onExplicitResultEvent`
   (calculationresultstore.cpp ~94-98):
   ```cpp
        if (logbook.writeCalculationRecord(sessionId, CalculationRecord::stamped(*snapshot), &error)) {
            ++m_stats.recordsWritten;
        } else {
            ++m_stats.writeFailures;
        }
   ```
   In the `else` branch, after the counter, emit
   `recordWriteFailed(sessionId, event.instanceId, reason)` where `reason` is
   `error`, or `tr("The result could not be stored")` when `error` is empty.
   Use `QObject::tr` in the store's context. This is the only path.
   `writeCalculationRecord` returns false for every failure: an unknown
   session (step a, which emits no record change), a refused encoding
   (step b), a failed index flush (step d), a folder that cannot be created,
   and open, write or commit failures (step e). The two early returns above
   it are not failures: `event.status != ResultStatus::Ok`, and
   `!snapshot` (a deferred invalidation already dropped the result, and its
   Dropped event follows). A family instance is filtered out at the top.
   `stats().writeFailures` therefore equals the number of emissions.
4. **Signal order, which Phase 3 relies on.** For failures after step a, the
   manager's `fail()` lambda (logbookmanager.cpp ~1090-1100) emits
   `calculationRecordsChanged(sessionId, calculationId)` *inside*
   `writeCalculationRecord`, so that signal comes **before**
   `recordWriteFailed` for the same pair. A later successful write of the
   pair emits `calculationRecordsChanged` again (step f, ~1160). That is the
   record change Phase 3 uses to clear the remembered failure. An
   unknown-session failure emits only `recordWriteFailed`.
5. **Relay.** Add the signal
   `void calculationRecordWriteFailed(const QString &sessionId, const QString &calculationId, const QString &reason);`
   to `SessionModel`. In its constructor, next to the
   `calculationRecordsChanged` connection (~147), connect
   `&m_resultStore, &CalculationResultStore::recordWriteFailed` to
   `this, &SessionModel::calculationRecordWriteFailed` (signal to signal,
   direct). Doc: "The result store could not write the record of an Ok
   result it was given (CalculationResultStore::recordWriteFailed); emitted
   from inside an engine callback." Consumers (the demand layer in Phase 3)
   connect to the model and never name the store, which keeps the audit rule
   "one result store, owned by the session model" (`CalculationResultStore`
   only in `src/calculationresultstore.*` and `src/sessionmodel.*`) green.
6. Add a bullet to the store's class comment: "A record that cannot be
   written is announced (recordWriteFailed) with the manager's reason; the
   result stays installed, and nothing retries it."

**Acceptance Criteria:**
- [ ] A write that fails because a directory stands at the record's path emits `SessionModel::calculationRecordWriteFailed("s1", kExpA, reason)` once, with a reason that starts "Couldn't write file", after the manager's `calculationRecordsChanged("s1", kExpA)` for the same pair.
- [ ] A record the format refuses (the QVariantList attribute of `writeFailureKeepsPreviousRecord`) emits it once, with the encoder's reason, which names `LY_OUT`.
- [ ] A later successful write of the pair emits `calculationRecordsChanged` for the pair and no `calculationRecordWriteFailed`. `stats().writeFailures` equals the number of emissions in every test.
- [ ] No product code other than `SessionModel` connects to either signal. `audit_cleanup` passes.

**Complexity:** M

---

### Task 1.6: The session model exposes column knowledge and a row's display name

**Purpose:** Make the session model the one source of each column's static
dependency closure and its requested calculations (E(c)), and of a session's
display name (spec §8).

**Files to modify:**
- `src/sessionmodel.h`: three public accessors, the lazy-rebuild members, and doc comments.
- `src/sessionmodel.cpp`: the accessor bodies, `rebuildColumnDependencies()` made const, and the registry observer.
- `tests/tst_result_columns.cpp`: see Testing.

**Technical Approach:**

**Existing tables.** `rebuildColumnDependencies()` (sessionmodel.cpp ~1433)
fills two vectors that are parallel to `m_columns`:
- `m_columnDependencies[i]`: the union of
  `registry.staticDependencies(name)` (names *and* preferences) over
  `logbookColumnNames(col)`.
- `m_columnExplicitCalculations[i]`:
  `logbookColumnExplicitCalculations(col, registry)`.

It runs in `rebuildColumns()` (a column change, inside the model reset) and
in `checkCalculationEnvironment()`. The environment check is *queued* after a
registry change (the registry observer at ~141 calls `queueEnvironmentCheck()`,
which posts `flushPendingInvalidations`). Between a registration and that
queued flush the tables are stale.

The demand layer's own tables are current at its next pass after a registry
change. Its observer sets `m_columnsDirty`, and `syncColumns()` reads the
registry. A 0-ms `QTimer` and a queued meta-call have no guaranteed order, so
the model's tables must be current whenever an outside reader asks for them.

**New public API** (return by value; `QStringList` and `StaticDependencies`
are implicitly shared):
```cpp
/// The requested calculations of enabled column `column` (E(c):
/// logbookColumnExplicitCalculations() of its definition); empty for a column
/// that depends on none, and for an index out of range. Current under the
/// registrations at the moment of the call. Indices are valid between two
/// column rebuilds (a column change resets the model). A plain read: nothing
/// is emitted, no row changes; allowed under a RowStabilityGuard. Must not be
/// called from inside a registry observer.
QStringList columnRequestedCalculations(int column) const;
/// The static dependency closure of the column's names (the union of
/// CalculationRegistry::staticDependencies() over logbookColumnNames()):
/// names and preferences. Same rules as above.
StaticDependencies columnDependencyClosure(int column) const;
/// How the logbook names the session of row `row`: the loaded session's
/// _DESCRIPTION (SessionKeys::Description); for a row that is not loaded, or
/// a failed-load placeholder, the description the logbook index caches for
/// the row; when that is empty, the session id. Empty for a row out of
/// range. A plain read (allowed under a RowStabilityGuard).
QString sessionDisplayName(int row) const;
```

**Lazy freshness.**
- Add `mutable bool m_columnTablesStale = false;`, and make
  `m_columnDependencies` and `m_columnExplicitCalculations` `mutable`.
- Make `rebuildColumnDependencies()` a `const` member that clears
  `m_columnTablesStale` at its end.
- Add `void ensureColumnTables() const` that calls
  `rebuildColumnDependencies()` when the flag is set.
- The registry observer (~141) becomes
  `[this]() { m_columnTablesStale = true; queueEnvironmentCheck(); }`. It
  only sets a flag. The registry forbids calling it from an observer
  (calculationregistry.h: "An observer must not call it from inside its
  callback").
- The two column accessors call `ensureColumnTables()` first, then bounds-check
  `column` against the table size (0 ≤ column < size) and return the entry or
  an empty value.
- The model's own internal readers keep reading the members directly:
  `invalidateColumns`, `fillMissingColumns`, `settleExplicitColumns`,
  `restoreForColumnWorker`, `onCalculationRecordsChanged`, `evictSession`,
  `columnsReadPreference`. The paths that store values already call
  `applyPendingEnvironmentCheck()` first. A rebuild triggered early by an
  outside read only makes these internal readers current sooner; the
  environment check rebuilds again and discards the changed columns as
  before.

**`sessionDisplayName(row)`**, which is today's
`CalculationDemand::rowDisplayName` (calculationdemand.cpp ~784-801), moved:
```cpp
    if (sr.isLoaded() && !sr.loadFailed)
        name = sr.session->getAttribute(SessionKeys::Description).toString();
    else
        name = LogbookManager::jsonToVariant(
                   LogbookManager::instance().cachedValuesForSession(sr.sessionId).value(descriptionKey)).toString();
    return name.isEmpty() ? sr.sessionId : name;
```
Here `descriptionKey` is a function-local `static const QString` computed once
as `FlySight::logbookColumnDefinitionKey(<a SessionAttribute column with
attributeKey SessionKeys::Description>)` (the free function in
`src/logbookcolumn.h`, which today's `rowDisplayName` uses; `LogbookManager`
has no `columnDefKey`). A loaded row does **not** fall back to the
index's cached value, because the live attribute wins. This matches both
current implementations: the demand layer's `rowDisplayName`, and the
executor's `getAttribute(SessionKeys::Description)` with the id as fallback.

Document the three accessors in the class comment near CACHED COLUMN VALUES
("the model is the one source of each enabled column's closure and requested
calculations and of a row's display name; the demand layer and the executor
read them"). Say "the demand layer" and "the executor", never the class names
(audit).

**Acceptance Criteria:**
- [ ] For each enabled column `i`, `columnRequestedCalculations(i) == logbookColumnExplicitCalculations(column(i), registry)`. `columnDependencyClosure(i).names` equals the union of `staticDependencies(name).names` over `logbookColumnNames(column(i))`. Out-of-range indices give empty values.
- [ ] Right after a registration that changes a column's E(c) or closure, and **before the event loop runs** (so before the queued environment check), both accessors return the new values.
- [ ] `sessionDisplayName`: a loaded row with a description gives the description; a loaded row without one gives the id; a stub gives the index's cached description; a stub without a cached description gives the id; a failed-load placeholder gives the cached description; out of range gives an empty string.
- [ ] Nothing the model already did changes. `tst_result_columns`, `tst_column_cache` and `tst_session_model_engine` pass as before.

**Complexity:** M

---

### Task 1.7: The demand layer reads column knowledge and display names from the model

**Purpose:** Remove the demand layer's own E(c) computation, its dirty flags,
its relevant-name union and its display-name code (spec §8). The demand layer
still computes a plot's closure from the registry.

**Files to modify:**
- `src/calculationdemand.h`: remove `rebuildRelevantNames()`, `m_relevantNames`, `m_relevantNamesDirty`, `m_columnsDirty` and `rowDisplayName()`. Add `bool isRelevantName(const DependencyKey &key);`. Update the comments on `ColumnInfo::calculations`, on `DemandTrack::sessionName` and in the class comment.
- `src/calculationdemand.cpp`: `syncCheckedSet`, `syncColumns`, `recompute`, `inspect`, `walkColumns`, `classifyUnloaded`, `onDependencyChanged`, `onSessionModelAboutToBeReset`, `onCalculationRecordsChanged`, `onRegistryChanged`. Delete `rowDisplayName` and `rebuildRelevantNames`.
- `tests/tst_calculation_demand.cpp`: see Testing.

**Technical Approach:**

1. **`syncColumns()` reads the model and runs at the start of every pass.**
   In `recompute()`, `if (m_columnsDirty) syncColumns();` becomes
   `syncColumns();`, still inside the non-inert branch before
   `inspectedPlots()`. In `syncColumns()` (~287), replace
   `logbookColumnExplicitCalculations(column, registry)` with
   `model.columnRequestedCalculations(i)`. Keep everything else in that
   function:
   - `info.names = logbookColumnNames(column)`, which is what inspection
     reads, not a closure.
   - The storable filter (ids without `'#'`) and the titles from
     `registry.title(id)`.
   - The id comparison that clears `m_columnReports` and erases the
     settlements of departed columns.

   Remove `m_columnsDirty = false;` and `m_relevantNamesDirty = true;`.
   `m_columns` is now "the requested enabled columns as the session model
   reported them at the start of the last pass". Comment it that way. It is a
   per-pass copy with no dirty flag.

   The cost is O(columns) plus a title lookup per storable id, per pass. A
   column change reaches the demand layer only through the model reset, which
   schedules a pass. A registry change reaches it through its own observer,
   which schedules a pass that reads the model's lazily current tables
   (Task 1.6).
2. **Settlements and the report memo stay correct without the dirty flag.**
   - A registry change still clears `m_settled`, `m_memory` and
     `m_columnReports` (`onRegistryChanged`).
   - A reset still clears `m_columnReports`, `m_recordSets` and
     `m_recordReasons` (`onSessionModelAboutToBeReset`).
   - A column that stays across a reset has the same E(c), because E(c) is a
     function of the definition and the registrations.
   - Between a reset and the next pass, `m_columns` still lists the columns
     that settlements exist for. So `eraseSettlements()`,
     `onCalculationRecordsChanged()` and `runLoadStep()`'s failed-load loop
     keep iterating `m_columns`.
   - In `onCalculationRecordsChanged`, `if (m_columnsDirty ||
     column.calculations.contains(calculationId))` becomes
     `if (column.calculations.contains(calculationId))`.
3. **Relevant names without a union.** Delete `rebuildRelevantNames()`,
   `m_relevantNames` and `m_relevantNamesDirty`. Delete the line
   `m_relevantNamesDirty = true;` in `syncCheckedSet()` and the two relevant
   names lines in `onRegistryChanged()`. Add a private
   `bool isRelevantName(const DependencyKey &key)` that does two checks:
   - **Plots.** For each id in `m_checkedOrder`: if
     `isRequested(m_checked.value(id))` and
     `m_staticNames.value(id).contains(key)`, return true. `isRequested()`
     fills `m_staticNames`; it is the plot closure the demand layer still
     computes.
   - **Columns.** If `m_sessionModel` is set, then for each `i <
     model.columnCount()`: if `!model.columnRequestedCalculations(i).isEmpty()`
     and `model.columnDependencyClosure(i).names.contains(key)`, return true.
     Only requested columns count, exactly as today's union over
     `m_columns`. Including ordinary columns would start settle waits for
     edits that affect no requested calculation.

   `onDependencyChanged` loses `if (m_columnsDirty) syncColumns(); if
   (m_relevantNamesDirty) rebuildRelevantNames();` and tests
   `if (!isRelevantName(key)) return;`. Everything else in it stays.
4. **Display names from the model.** Delete `rowDisplayName()`.
   - `inspect()` (~399-403) sets `track.sessionName =
     model.sessionDisplayName(row);`. This is identical for a visible, loaded,
     non-placeholder row: its description, else its id.
   - `walkColumns()` (~615) uses `name = model.sessionDisplayName(row);`.
   - `classifyUnloaded` gains the row index:
     `classifyUnloaded(int row, const SessionRow &sr, const ColumnInfo &column)`.
     Its four `rowDisplayName(sr)` calls become
     `m_sessionModel->sessionDisplayName(row)`, and `walkColumns()` passes
     `row`. `m_sessionModel` is non-null there: `walkColumns()` runs only when
     the component is not inert.
5. **Remove the dirty-flag plumbing.** In `onSessionModelAboutToBeReset`,
   delete `m_columnsDirty = true;`. In `onRegistryChanged`, delete
   `m_columnsDirty = true;` and the two relevant-names lines, and keep the
   remaining lines.
6. **Comments.**
   - `DemandTrack::sessionName` (header ~43-44): "SessionModel::sessionDisplayName() of the row".
   - `ColumnInfo::calculations` (~434): "E(c) as SessionModel::columnRequestedCalculations() gives it".
   - Class comment WHAT IS WANTED (~115): "(the column is REQUESTED:
     SessionModel::columnRequestedCalculations() is not empty)".
   - The comment above `syncColumns()` (~284-286).

   After this task no comment or code in `src/calculationdemand.*` names
   `logbookColumnExplicitCalculations`. Phase 5's audit rule relies on that.

The walk, the settlements, the memory, `Candidate`, `ColumnWalk`, the offer
logic and every field not listed above stay exactly as they are. Phase 2
removes the waiting list, `settling`, the job id and the own-offer memory.
Phase 3 rewrites the walk and the memory.

**Acceptance Criteria:**
- [ ] `src/calculationdemand.*` contain no `logbookColumnExplicitCalculations`, `staticDependencies` call on a column name, `m_columnsDirty`, `m_relevantNames`, `rebuildRelevantNames`, `rowDisplayName` or `SessionKeys::Description`. The only `staticDependencies` call left is the plot closure in `isRequested()`.
- [ ] A registration that makes an enabled ordinary column requested is seen by the demand layer's next `flush()`, before the model's queued environment check has run.
- [ ] A change of a name in the closure of a requested column starts the session's settle wait. A change of a name that is only in the closure of an ordinary (unrequested) column does not.
- [ ] Every existing `tst_calculation_demand`, `tst_logbook_indicators`, `tst_plot_row_delegate`, `tst_result_columns`, `tst_fusion_jobs` and `tst_fusion_store` assertion on track names (for example "Jump 2", "Jump 3") passes unchanged.

**Complexity:** M

---

### Task 1.8: The executor loses idle, queued and the two-job query, and names sessions by the model

**Purpose:** Remove executor generality that has no product caller (spec §9
executor side), and use the model's display name for job records (spec §8).
`cancel(JobId)` and the job history stay for the jobs dock.

**Files to modify:**
- `src/jobqueue.h`, `src/jobqueue.cpp`: the removals and the naming change.
- `src/jobmodel.h`: the `JobRecord::sessionName` comment (~46).
- `src/calculationdemand.h` and `.cpp`: comments that name `idle()`.
- `tests/support/jobfixture.h`, `tests/support/jobfixture.cpp`: `Quiet`, plus a helper.
- `tests/tst_jobqueue.cpp`, `tests/tst_calculation_demand.cpp`, `tests/tst_fusion_jobs.cpp`, `tests/tst_fusion_store.cpp`, `tests/tst_plot_row_delegate.cpp`: replace every use.

**Technical Approach:**

**Remove from `JobQueue`:**
- The signals `jobQueued(JobId)` (~204) and `idle()` (~210).
- The query `QList<JobId> activeJobs() const` (header ~169-171, cpp ~281-289).
- The busy-period bookkeeping: `bool m_idleAnnounced`, `void
  announceIdleIfIdle()` (cpp ~214-220), the line `m_idleAnnounced = false;`
  in `offer()`, and the enum `AfterEnd` with `endJob`'s `afterEnd`
  parameter. Once idle() is gone, `AfterEnd::Nothing` only skips a
  `scheduleStart()` call, which does nothing there: nothing is running (the
  replaced job was the chosen next job, so `isIdle()` is true and nothing is
  scheduled), or a job runs and `startNext()` respects the running bound. It
  existed only so that no idle() fell between a replaced chosen next job and
  its replacement.

**Edits in `jobqueue.cpp`:**
- `endJob` tail: `if (afterEnd == AfterEnd::Nothing) return; if
  (!isIdle()) scheduleStart(); else announceIdleIfIdle();` becomes
  `if (!isIdle()) scheduleStart();`.
- `offer()` replacement loop: `endJob(previous, JobState::Cancelled, tr("No
  longer needed"), std::nullopt, {}, AfterEnd::Nothing);`. Drop the last
  argument. `if (m_shutDown) { announceIdleIfIdle(); return
  {Kind::ShuttingDown, 0}; }` becomes `if (m_shutDown) return
  {Kind::ShuttingDown, 0};`.
- `offer()` after `append`: keep the early `return {Kind::Created, id};` for
  a job that a `rowsInserted` slot already ended. Then `emit jobsChanged();
  scheduleStart();` without `emit jobQueued(id);`. Reword the "Pin and open
  the busy period BEFORE the row appears" comment. The pin still comes before
  `append()`, so that a `rowsInserted` slot's `cancel()` finds the pin that
  `endJob()` releases.
- **Session name** (offer ~192-200, 231). Inside the existing guarded block,
  where `loadedSession(sessionId)` returned a session, replace
  `sessionName = session->getAttribute(SessionKeys::Description).toString();`
  with
  `sessionName = m_sessionModel->sessionDisplayName(m_sessionModel->getSessionRow(sessionId));`.
  Keep `record.sessionName = sessionName.isEmpty() ? sessionId :
  sessionName;` as a guard for an out-of-range row. Drop
  `#include "sessiondata.h"` only if nothing else in the file needs it.

**Comments in `jobqueue.h`:**
- ORDER OF A JOB'S END (6): "the chosen next job, which a slot may have
  offered during (3), is scheduled". Delete "A chosen next job that an offer
  replaces skips (6), so that no idle() falls between it and its
  replacement."
- `offer()` doc: delete ", and no idle() is emitted between it and the new one".
- `withdrawChosenNext()` doc: delete "idle() follows when nothing runs."
- `jobsChanged` doc stays ("after every one of the above except jobProgress").
- `cancel()` doc: add "No product code calls it today: it is kept for the jobs
  dock, a later view of the job history (the audit rule 'no product code
  cancels a job' says the same)."
- `JobRecord::sessionName` in `jobmodel.h`: "snapshot at the offer: the
  session model's display name of the row (sessionDisplayName())".

**Comments in the demand layer that name idle()**
(`calculationdemand.h` ~288-291 WHEN A PASS RUNS, `calculationdemand.cpp`
~1566-1567 in `onJobFinished`). Replace "before it decides between idle()
and the next start" with "before it schedules the next start".

**Tests.** Replace each use; no test function is renamed or removed.

| Use | Replacement |
|---|---|
| `QSignalSpy(x, &JobQueue::jobQueued)` counting created jobs (`tst_jobqueue` runsAndPublishes, refusesMissingInput; `tst_calculation_demand` ~1513; `tst_fusion_jobs` ~556; `tst_fusion_store` ~1099/1326/1454; `tst_plot_row_delegate` ~617/652) | `QSignalSpy(m_queue->model(), &QAbstractItemModel::rowsInserted)`. Every created job appends one record. |
| `jobfixture.h` `Quiet` (`m_spy(&queue, &JobQueue::jobQueued)`) | spy on `queue.model()`'s `rowsInserted`; keep the row-count check |
| `connect(m_queue, &JobQueue::jobQueued, …)` (`tst_calculation_demand` ~3636, savesAndBulkEditsPrecedeLoadStep) | connect to `m_queue->model()`'s `rowsInserted`, reading `m_queue->model()->record(first)` |
| `activeJobs()` list comparisons (`tst_jobqueue` ~281, 523, 559, 571, 573, 697, 833, 1179, 1466, 1808) | a new fixture helper `QList<FlySight::JobId> activeJobIds(const FlySight::JobQueue &queue)` in `tests/support/jobfixture.h/.cpp`: the ids of `queue.model()->records()` with `isActive()`, in model order. The running job is always older than the chosen next job, so the order equals the old query's. |
| `activeJobs().size()` (`tst_calculation_demand` ~730, ~1962) and the allowed-pins loop (~364) | `activeJobIds(*m_queue)`, or `runningJob()` / `chosenNextJob()` |
| `QSignalSpy(…, &JobQueue::idle)` in `tst_jobqueue` runsAndPublishes, offerReplacesChosenNext, withdrawEndsChosenNext, cancelFromRowsInsertedLeavesNoPin, shutdownWithQueuedAndRunning, shutdownIsIdempotentAndRefusesOffers | delete the spy and its counts. Where the count stood for a state, assert `isIdle()` / `!isIdle()` at that point. In offerReplacesChosenNext, delete `bStateAtIdle` and its connection; the facts that remain are that the replaced job never started (`startedSpy`) and that B succeeded. In cancelFromRowsInsertedLeavesNoPin, delete the "not announced as queued" assertion (`queuedSpy.count() == 0`); keep `finishedSpy`. |
| `tst_calculation_demand::chainedBlockersContinue` (~824, "The executor was never idle between the links") | connect a slot to `jobFinished` **after** the demand layer exists, so it runs after the demand layer's own slot. For the first link's end, record `m_queue->isIdle()` and `m_queue->chosenNextJob() != 0`. Assert that the executor was not idle and the next link was chosen. |
| `tst_calculation_demand::heldChainContinues` (~865 `idleSpy`) | the same technique, or delete the idle count if it is not asserted |

**Acceptance Criteria:**
- [ ] `grep -rnE "jobQueued|activeJobs\(|JobQueue::idle|announceIdleIfIdle|m_idleAnnounced|AfterEnd" src tests` finds nothing. `tests/README.md` is Phase 5's.
- [ ] `JobQueue::cancel(JobId)` and `JobModel` are unchanged apart from comments.
- [ ] A job record's `sessionName` equals `SessionModel::sessionDisplayName()` of its row at the offer: `runsAndPublishes` still gets "First jump" for s1 and "s2" for s2, whose description was removed.
- [ ] Every test that used the removed API passes, asserting the same facts through `rowsInserted`, `isIdle()`, `runningJob()`/`chosenNextJob()` and the records.

**Complexity:** L

---

### Task 1.9: Keep the audit and the acceptance map green; record the documentation debt

**Purpose:** Each phase leaves `audit_cleanup` and `tests/acceptance_map.txt`
valid (overview, Decisions & Constraints).

**Files to modify:**
- `tests/audit/cleanup_audit.cmake`: only if a rule trips. None is expected; see below.
- `tests/acceptance_map.txt`: no change. This phase renames and removes no cited test function. The new test functions are cited by Phase 5.

**Technical Approach:**
- Run the audit (`ctest … -R audit_cleanup`). The rules this phase could trip, and how the design avoids each one:
  - "the idle scheduler learns nothing about jobs or demand"
    (`[Dd]emand|[Cc]alculation|[Jj]ob|[Ee]xecutor` in `src/idlescheduler.*`)
    and "the idle scheduler is never paused for a calculation"
    (`[Ff]usion|[Jj]ob[Qq]ueue|calculations/|IsRunning`). The new scheduler
    comments use none of these words ("waits on something outside the
    scheduler").
  - "the model, the logbook and the scheduler know nothing of the executor"
    (`JobQueue|JobModel|jobqueue\.h|jobmodel\.h` in `src/sessionmodel.*`,
    `src/logbookmanager.*` …). The new model comments say "the executor".
  - "only the application and its views know the demand layer"
    (`CalculationDemand`). New comments below the demand layer say "the
    demand layer".
  - "one result store, owned by the session model" (`CalculationResultStore`
    only in the store and `sessionmodel.*`). The relay (Task 1.5) keeps
    every other file from naming it.
  - "nothing below the demand layer includes it": unchanged.
  - "no product code cancels a job": unchanged.
- The new executor-name rules and the "requested calculations computed only
  by the session model and the registry" rule are **Phase 5's**
  (overview: "Phase 5 adds the new rule … and finishes the demand and
  gestures groups"). Do not add them here.
- Do not edit `docs/`. Phase 5 rewrites it. The passages that become false in
  this phase, for Phase 5:
  - `docs/CALCULATIONS.md`:
    - §15.2 (~732-737: "with no `idle()` between", "announced (`jobQueued`,
      `jobsChanged`)", "`idle()` follows when nothing runs"; ~804-808: step
      (6) `idle()`, "reports no `idle()` between links").
    - §15.7 table (~999 `activeJobs()`; ~1004 signals `jobQueued`, `idle()`).
    - §16.2/16.3 (~1314, ~1336: the single-row display change "when it
      changed what the demand layer knows of it", which the demand layer no
      longer observes; a bulk edit arrives as a dependency change).
    - §16.4 (~1403-1404 `idle()`).
    - §16.7 (~1484).
    - §16.8 (~1517 "one last step, which loads nothing, reports the fill").
    - §16.9 (~1622, the scheduler API row: add the completion of a waiting
      task).
    - §16.11 (~1746 the idle scheduler's contract).
    - The scheduler's completion wherever it is described.
    - §15.8 (the store's write-failure announcement).
  - `docs/DATA_SCHEMA.md` §11 (~354-358): when the index learns a reason, and
    that it now announces it.
  - `tests/README.md`: the `tst_session_model_engine` row (~80), and any text
    on `idle()`, `jobQueued` or the fill's last step.

  Phase 5 must **not** write "the existing tasks lose work only by stepping or
  by cancel" (they do lose work outside a step; spec §5 says so). Write "the
  rule applies to tasks that can wait (`canStep`); the others complete
  through their step or cancel, as before".

**Acceptance Criteria:**
- [ ] `audit_cleanup` passes with no rule edited, or with the edit reported and justified in the implementer's report.
- [ ] `tests/acceptance_map.txt` is unchanged and the audit's traceability check passes.

**Complexity:** S

## Testing Requirements

### Unit Tests

New test functions (declare each in the test class's `private slots:` next to
its neighbours):

1. `tst_session_model_engine.cpp::schedulerCompletesWaitingTaskWhoseWorkIsGone`
   (after `schedulerWaitingTaskDoesNotSpin`, following its pattern at
   ~826-892). Use test ids 93-95 and unregister them in a `qScopeGuard`.
   Record one event list from `activeTaskChanged` ("A<id>"),
   `progressChanged` ("P<id> r/t"), `schedulerIdle` ("I") and each task's
   `onComplete` ("C<id>:<cancelled>").
   - (a) Task 93 (priority 8, `canStep` false, `hasWork` a flag, progress
     `{remaining, 3}`) with work. Wake: "A93", "P93 …", then rest
     (`!isTicking()`). Clear the flag and set remaining 0 without a step.
     Wake: the events after that are exactly "P93 0/3", "C93:0", "I". There
     are no steps. Another wake adds no "C93".
   - (b) Task 93 waits again, and task 94 (priority 9, plain) has no work. In
     one turn, 93 loses its work and 94 gains work. Wake: "C93:0" comes before
     "A94".
   - (c) Task 93 waits and is `cancel()`ed: "C93:1" once. Then it loses its
     work and the scheduler is woken: no "C93:0".
   - (d) A plain task 95 (no `canStep`) whose `hasWork` is cleared by a
     `progressChanged` slot after its first step, the way
     `schedulerTaskCanBeUnregistered` removes its task. It gets no "C95" and
     the scheduler goes idle. This is today's behaviour for tasks without
     `canStep`.
2. `tst_logbook_index.cpp::recordReasonChangeIsAnnounced` (after
   `recordReasonsRoundTrip`, using its `makeSession`/`saveSession` setup).
   Spy on `calculationRecordsChanged`.
   - Setting "no" emits once with (g1, x). Setting "no" again emits nothing.
   - Setting "other" emits once. Clearing emits once. Clearing again emits
     nothing.
   - A `writeCalculationRecord` whose reason differs from the stored one emits
     exactly once. `removeCalculationRecord` emits exactly once.
3. `tst_result_store.cpp::writeFailureIsAnnounced`, reusing the two failure
   setups of `writeFailureLeavesResultUsable` (a directory at
   `recordPath("s1", "exp%41")`) and `writeFailureKeepsPreviousRecord` (the
   `kListy` QVariantList).
   - Record a sequence from `LogbookManager::calculationRecordsChanged` and
     `SessionModel::calculationRecordWriteFailed`. For each failure it is
     "changed:s1/<id>" then "failed:s1/<id>". The reason is not empty. For
     the directory case it starts "Couldn't write file". For the refused
     encoding it contains "LY_OUT".
   - `stats().writeFailures` equals the number of "failed" entries.
   - After `rmdir` and a new Ok publish (as in `writeFailureLeavesResultUsable`),
     there is one more "changed:s1/expA" and no more "failed". Use
     `WarningCapture` like the existing tests.
4. `tst_result_columns.cpp::sessionModelExposesColumnKnowledge`, next to
   `columnExplicitCalculations`.
   - `columnRequestedCalculations(kD)` is empty,
     `columnRequestedCalculations(kX)` is `{kCalcX}` and
     `columnRequestedCalculations(kY)` is `{kCalcY}`. Index -1 and
     `columnCount()` give empty.
   - `columnDependencyClosure(kX).names` equals
     `CalculationRegistry::instance().staticDependencies(DependencyKey::attribute("X_OUT")).names`.
   - Register (then unregister in a scope guard, as
     `registryChangeKeepsLoadedRowConfirmed` does) an explicit calculation
     whose output an enabled column reads. That can be an extra enabled
     column over an output nothing produces, added with
     `LogbookColumnStore::instance().setColumns(...)`, where `cleanup()`
     restores the columns. Without spinning the event loop,
     `columnRequestedCalculations` of that column returns the new id.
5. `tst_result_columns.cpp::sessionDisplayNameOfEveryRowKind`. Cover a loaded
   row with a description, a loaded row whose description was removed, a
   stub (make it with the fixture's eviction or restart) with the index's
   cached description, a stub whose cached description was removed from the
   index, a failed-load placeholder (remove the session's `.csv`, then load
   the row), and out of range. The expected values are in the Task 1.6
   criteria.
6. `tst_calculation_demand.cpp::recordReasonReachesDemandThroughRecordChange`.
   Use the setup of the third section of
   `storedRejectionIsBadgedAfterRestartWithoutLoad`: an index without
   `"recordReasons"` and without s2's EA1 value, a new model, executor and
   demand layer. Factor that setup into a local helper that both tests use.
   - Then `{ const QSignalBlocker blocker(&LogbookManager::instance());
     m_model->startColumnWorker(); QVERIFY(waitForIdle(*m_model)); }`.
   - The reason is learned (`calculationRecordReason` gives "negative
     input"). After `m_demand->flush()`, `col("EA1")` still has
     `failedCount == 0`: the display change carries no reason.
   - Then `emit LogbookManager::instance().calculationRecordsChanged("s2",
     "expA"); m_demand->flush();` makes s2 failed with "Explicit A: negative
     input" and name "Jump 2". No load, and `Quiet` holds.
7. `tst_calculation_demand.cpp::columnKnowledgeComesFromTheSessionModel`.
   - (a) Enable an ordinary column over an attribute that no calculation
     produces, and `flush()`: the state is not requested. Register, with
     `m_extra->add`, an explicit calculation producing it from an input that
     s1 has. `flush()` at once, without spinning: the column is requested and
     s1's cell is pending.
   - (b) With the column requested and s1 loaded and hidden, editing that
     input starts s1's settle wait (`isSettling`). Editing an attribute that
     is only in the closure of an enabled ordinary column does not.
8. `tst_calculation_demand.cpp::fillEndingBehindAnotherTaskStartsNextCountFresh`.
   - Set up two stubs s1 and s2 with `G_IN`, so both are held at once.
     Enable `G_OUT`, wait until the fill has reported `(2, 2)` and rests
     (`gate().waitEntered()`, two holds).
   - Register a plain test task (id 96, priority 0, a no-op step, `hasWork` a
     flag) with work, and wake the scheduler, so that it becomes active.
   - `gate().open(2)`, then wait until `!m_demand->hasFillWork()`.
   - Clear the flag, wait for the scheduler to be idle, and unregister 96.
   - Start a fill of one session: edit s2's `G_IN`, `settle()`, and hold its
     job at the gate. The first `ColumnFillTask` report is `(1, 1)`, not
     `(1, 2)`.

9. `tst_session_model_engine.cpp::bulkEditAnnouncesADependencyChange`. Spy
   on `SessionModel::dependencyChanged` and `dataChanged`.
   - A bulk edit of `_DESCRIPTION` on a stub row: after the scheduler is
     idle, the spy holds exactly one `dependencyChanged(<its id>,
     DependencyKey::attribute("_DESCRIPTION"))`, emitted before the row's
     `dataChanged`. The row is still a stub afterwards.
   - The same edit on a loaded row: one `dependencyChanged` per name the
     engine invalidated, the attribute's own name among them.
   - A bulk edit on a failed-load placeholder (remove the session's file,
     load the row): no `dependencyChanged`.
Existing tests to update:
- `tst_calculation_demand::fillTaskReportsProgressWhileWaiting`:
  - Connect a slot to `jobFinished` after the demand layer exists. At the
    last job's end, record `m_demand->hasFillWork()` and assert that it was
    false.
  - Line ~2912 `QCOMPARE(fillProgress.last(), progressOf(0, 1));` must become
    `QTRY_COMPARE`. `waitDemandIdle()` now returns as soon as the fill has no
    work, which can be before the scheduler's completion tick.
  - The existing "P 0/4 then I" check stays.
- `tst_calculation_demand::storedRejectionIsBadgedAfterRestartWithoutLoad`,
  third section: add a spy on `calculationRecordsChanged` and assert that it
  saw (s2, expA) during the worker's pass.
- `tst_calculation_demand::bulkEditMakesSettledSessionApplicable`: the bulk
  edit now starts s2's settle wait. After `startBulkEdit(...)`, add
  `QVERIFY(waitForIdle(*m_model)); settle();` before
  `QTRY_COMPARE(loadsOf(loadedSpy, "s2"), 2);`. Add after the edit: an
  assertion that a bulk edit of an attribute outside every requested
  closure (a second attribute column the description calculation does not
  read) leaves `loadsOf("s2")` unchanged and `!m_demand->isSettling("s2")`.
- `tst_result_store::recordReasonRecordedAtWriteAndRestore`: spy on
  `calculationRecordsChanged`.
  - `session("s1")`, which learns the reason at the load, emits (s1, kExpA).
  - The column worker's restore that learns it emits (s1, kExpA).
  - A reload of s1 whose reason is already known emits nothing for (s1,
    kExpA). Evict it with `makeStubs()` or the capacity preference, then
    load it again.
- The comment at `tst_calculation_demand.cpp` ~3130.
- Everything in the Task 1.8 table.

### Integration Tests

- The full suite, sequentially (see Notes). The load-sensitive fusion tests
  (`tst_fusion_jobs`, `tst_fusion_store`, `tst_fusion_session`) must pass
  without `-j`.
- `audit_cleanup` (the traceability check included).

### Manual Verification

1. Build with `cmake --build build-phase1 --config Release` and run the
   installed application from `build-phase1`.
2. Enable a logbook column over a requested output (for example a fusion
   output column) on a logbook with many unloaded sessions. The progress line
   shows "Computing columns: k / n", counts up to n / n, and disappears when
   the last result is stored. Nothing flickers and nothing stays at n / n.
3. Disable the column while it works. The line ends or switches to the column
   worker, and a later re-enable starts from "0 / m" with its own total.
4. With the jobs history visible through any debug means, or the job model
   in a debugger: a job's session name is the logbook's description of the
   row.
5. Hide a batch of sessions while the background loader runs. The plots and
   the progress line behave as before: the loader task is unchanged.

## Notes for Implementer

### Build and test (from the overview's Decisions & Constraints)
- Build **only** `build-phase1/`: `cmake --build build-phase1 --config Release`. **Never build `build/`.**
- Test with `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`, **sequentially, never `-j`**, and with no stray `ctest` or `tst_*` process running (executor-driven fusion tests are load-sensitive). One target: add `-R tst_calculation_demand`. The audit: `-R audit_cleanup`.
- This phase adds no source file. Reconfigure the inner project (`cmake build-phase1/FlySightViewer-build`) only if you add one. A new test source that links fusion would go in `_FLYSIGHT_GTSAM_REACHERS` (`cmake/SolverDependencies.cmake`), but none is planned.
- Commits are the orchestrator's. Never run a git command that changes repository state.

### Gotchas
- **Audit word bans.** `src/idlescheduler.*` must not contain `Demand`, `Calculation`, `Job`, `Executor` (any case of the first letter), `Fusion`, `JobQueue`, `calculations/` or `IsRunning`, in comments too. `src/sessionmodel.*` and `src/logbookmanager.*` must not contain `JobQueue`, `JobModel`, `jobqueue.h` or `jobmodel.h`. Only the store and `sessionmodel.*` may name `CalculationResultStore`. Below the demand layer, write "the demand layer", never `CalculationDemand`.
- **Two emissions per record write.** If the write and removal paths kept calling the public `setCalculationRecordReason`, a write whose reason changes would emit `calculationRecordsChanged` twice. Existing tests count exactly one. Use the private helper.
- **Lazy tables and registry observers.** `columnRequestedCalculations` and `columnDependencyClosure` may call into the registry. Never call them from inside a registry observer. The demand layer's observer only sets flags and schedules, as it does today. Return by value, and do not keep references into the model's tables.
- **The scheduler's rule re-finds the entry after `reportProgress`.** A `progressChanged` slot may unregister the task (see `schedulerTaskCanBeUnregistered`). Copy `onComplete` before calling it, as the step path does.
- **`waitDemandIdle()` returns earlier.** `plotfixture.cpp`'s `waitDemandIdle` includes `!demand.hasFillWork()`, which is now true as soon as the last pending cell is resolved, before the scheduler's completion tick. A test that reads the fill's final progress or `schedulerIdle` after it must use `QTRY_*`. `fillTaskReportsProgressWhileWaiting` ~2912 is the known case. Search for others that compare the fill's last report without `QTRY`.
- **Executor shutdown while the fill waits.** The rule completes the fill with its progress at that moment, `(remaining > 0, n)`, and `onComplete` does not reset the mark (`m_fillRemaining > 0`). This is intended. `noLoadsAfterExecutorShutdown` expects idle and no loads, and both still hold.
- **A newly learned reason drops and recomputes the values over its record once**, with identical values (Task 1.3). With the column worker, a stub's sibling column over the same calculation can be recomputed in the same step, and `startColumnWorker()` restarts the worker's counts. Stale deletions in the worker's restore already take this path. Do not try to suppress it: spec §6 requires the same record-changed signal.
- **Slot order in tests.** The demand layer connects to `jobFinished` in its constructor. A test slot connected later runs after it, which is how "the next link was already chosen" and "the fill has no work" are observed at a job's end.
- **`QSignalBlocker` on the manager** (test 6) also blocks the model's own record slot. That is harmless in that scenario because EA1 is missing, not cached. Keep the blocked region to the worker's pass.
- `classifyUnloaded` gets a row parameter only for the display name. Phase 3 replaces the function, so keep the change minimal.

### Decisions Made
- **The scheduler's work-gone completion applies to waiting tasks only
  (`TaskDef::canStep` non-null), as spec §5 has it** (revised after the plan
  review; the existing tasks do lose work outside a step). Task 1.1 lists the paths for
  `LoadTask`, `SaveTask`, `ColumnTask` and `BulkEditTask`. Applying the rule
  to them would change their completion moments. For `LoadTask` it would
  create a bug: `visibilityChanged(shown)` for sessions that were just
  hidden. With the scope, spec §5's requirement that the existing tasks
  complete exactly as before holds by construction, and the fill, the only
  waiting task, gets the rule. The rule fires for the task *last reported
  active*, exactly as spec §5 says. An explicit `TaskDef` flag was the
  alternative, but `canStep` already marks "waits on something outside the
  scheduler", which is exactly the kind of task whose work can end outside a
  step.
- **The fill starts its own count when it rises from zero pending sessions**
  (Task 1.2). Completion is no longer guaranteed for a fill that loses its
  work while another task is active. The old ending state guaranteed it. The
  onComplete reset stays.
- **The store's announcement is relayed by the session model.**
  `CalculationResultStore` becomes a `QObject` with
  `recordWriteFailed(sessionId, calculationId, reason)`. `SessionModel`
  re-emits it as
  `calculationRecordWriteFailed(sessionId, calculationId, reason)`. Consumers
  connect to the model, so the audit's "one result store, owned by the
  session model" holds and the demand layer never names the store. The signal
  comes after the manager's `calculationRecordsChanged` for the same pair,
  except for an unknown session. A later successful write emits
  `calculationRecordsChanged` for the pair, and Phase 3 clears the failure on
  it.
- **Session model API.** `columnRequestedCalculations(int)`,
  `columnDependencyClosure(int)` (per enabled column index, by value, lazily
  current after a registry change through `m_columnTablesStale`) and
  `sessionDisplayName(int row)`. There is no convenience query for "a
  requested column reads key". The demand layer's `isRelevantName()` loops
  over the columns (O(columns)), which reads and does not compute.
- **The demand layer keeps `m_columns` as a per-pass copy of the model's
  requested columns**, re-read at the start of every pass with no dirty flag.
  It holds id, names, E(c), storable ids and titles. This keeps the
  settlement, memo and walk code of Phases 2-3 untouched. It is a read, not a
  computation, of E(c).
- **`LogbookManager`'s own call of `logbookColumnExplicitCalculations`
  (logbookmanager.cpp ~112, `explicitCalculationsByDefKey`, used by
  `validateRecordStamps`, `dropRecordDependentValues` and `flushIndex`) stays
  as the index's registry-side use.** The manager is below the session model
  (the model includes it, never the reverse). It must work at `initialize()`
  before any model exists. It works over the columns of `index.json` and of
  `LogbookColumnStore`, not the model's rows, and CACHE VALIDITY is decided
  "here and nowhere else" (logbookmanager.h). **For Phase 5's rule:**
  `logbookColumnExplicitCalculations(` may appear only in
  `src/logbookcolumn.(cpp|h)` (the definition), `src/sessionmodel.(cpp|h)`
  (the model's tables) and `src/logbookmanager.(cpp|h)` (the index's
  validity), for example
  `expect_only("one computation of a column's requested calculations" "logbookColumnExplicitCalculations\\(" "^src/logbookcolumn\\.(cpp|h)$|^src/sessionmodel\\.(cpp|h)$|^src/logbookmanager\\.(cpp|h)$" src)`.
  Phase 5 may add `explicitDependencies\(` to the demand layer's forbidden
  names. After this phase, no line of `src/calculationdemand.*` names either
  call.
- **`JobQueue::AfterEnd` is removed with the idle bookkeeping.** Once idle()
  is gone its only effect is a `scheduleStart()` that does nothing.
- **Test replacements for removed executor API** use the job model:
  `rowsInserted` for created jobs, the records for active jobs through a new
  `activeJobIds()` fixture helper, and `isIdle()`. No test function is
  renamed, so `tests/acceptance_map.txt` is unchanged. Items 544, 545 and 560
  keep their citations. Their assertions on `idle()` go.
- **The bulk edit publishes its edit through `publishInvalidation`, as a
  direct edit does, and the demand layer's display-change slot goes
  entirely.** A bulk edit of a stub publishes the attribute's own dependency
  key; the loaded path publishes what `setAttribute` returned. The demand
  layer's existing `dependencyChanged` slot then clears the session's memory
  and settlements and starts its settle wait, and no signal of the column
  worker is observed. Phase 3's pair memory inherits this: a bulk edit is an
  input change, and nothing else clears a session's facts.
- **No new audit rules in this phase.** The rules for removed names, the
  one-computation rule and "the demand layer observes no display change of
  the model" (no `QAbstractItemModel::dataChanged` connection in the demand
  layer's files) are Phase 5's.

### Open Questions
- None. The scoping of the scheduler's rule to tasks that can wait, and the
  bulk edit's announcement of its edit, are both in the specification (§5
  and §6, revised after the plan review).

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria.
2. All tests pass: the full `ctest` run on `build-phase1`, sequential, including `audit_cleanup`.
3. Code follows the patterns of the reference files (plain-value reads under guards, direct connections that only drop state and schedule, one emitter per fact).
4. No TODOs or placeholder code remain, and no comment in `src/` names a removed API (`idle()`, `jobQueued`, `activeJobs`, `isFillEnding`, `rowDisplayName`, `onSessionDataChanged`, the reason comparison, the display-change clearing).
