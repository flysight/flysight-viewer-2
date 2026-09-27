# Idle Scheduler

## Overview

The application has four idle-time workers in `SessionModel` that share the same
pattern: a single-shot `QTimer` at 0ms interval, one-item-per-tick processing,
high-water-mark progress tracking, and priority-based yielding. Each worker is
implemented independently with its own timer, counters, and hand-rolled
preemption checks. This spec factors out the common pattern into a standalone,
reusable `IdleScheduler` class.

**Design goal:** Replace the four ad-hoc worker implementations with a single
scheduler that manages priority, progress, and lifecycle, while the domain logic
(what each task actually does) stays in `SessionModel` as lightweight callbacks.

## Dependencies

- **Context:** `docs/load-save-jobs.md` describes the original save/load worker
  design. `SessionModel` is the sole current consumer.

## Concepts

### What the Scheduler Does

The scheduler is a cooperative, priority-ordered idle-task executor. It owns a
single `QTimer` and a list of registered tasks. On each idle tick it finds the
highest-priority task that has work and calls its step function. When no tasks
have work, it goes idle.

Tasks are registered once at startup and supplied with work over the lifetime of
the application. The scheduler does not own or manage work queues — it asks each
task whether it has work via a callback. The task is the single source of truth
for its own state.

### Priority Is Implicit

The current workers implement priority by explicitly checking each other's state
(e.g., the column worker checks whether the saver, loader, or bulk editor are
active before proceeding). With the scheduler, priority is structural: the
scheduler iterates tasks in priority order, and the first task that reports
having work gets the tick. No task needs to know about any other task.

### Existing Workers

| Worker | Current timer | Current tick handler | Priority | Cancellable |
|--------|---------------|----------------------|----------|-------------|
| Save | `m_saveTimer` | `saveNextSession()` | 1 (highest) | No |
| Load | `m_loadTimer` | `loadNextVisibleSession()` | 2 | Yes |
| Bulk Edit | `m_bulkEditTimer` | `processNextBulkEdit()` | 3 | Yes |
| Column | `m_columnWorkerTimer` | `processNextDirtyColumn()` | 4 (lowest) | Yes |

Each of these becomes a task registration on the scheduler. The tick handler
becomes the step callback. The timer, counters, preemption checks, and
resumption chains are all eliminated.

## Design

### IdleScheduler Class

`IdleScheduler` is a standalone `QObject` with no knowledge of `SessionModel`,
sessions, or logbooks. It lives in its own header/source pair.

#### Progress Struct

```cpp
struct Progress {
    int remaining;
    int total;
};
```

Returned as a pair from a single callback to guarantee atomicity — `remaining`
and `total` are always consistent with each other.

#### Task Registration

Tasks are registered with a struct:

```cpp
struct TaskDef {
    int          priority;
    StepFn       step;        // std::function<void()> — process one work item
    BoolFn       hasWork;     // std::function<bool()> — is there pending work?
    ProgressFn   progress;    // std::function<Progress()> — current progress
    CompleteFn   onComplete;  // std::function<void(bool cancelled)> — batch done
    bool         cancellable = false;
};
```

Registration:

```cpp
void registerTask(TaskId id, const TaskDef &def);
```

`TaskId` is an `int` provided by the caller (typically an enum). Tasks are
registered once and persist for the lifetime of the scheduler.

#### Public API

- **`registerTask(TaskId id, const TaskDef &def)`** — Register a task. The
  caller provides the ID (e.g., from an enum).
- **`wake()`** — Tell the scheduler that work may be available. If idle, the
  scheduler evaluates tasks and starts ticking. If already ticking, continues
  normally (re-evaluates priorities on the next tick). Called by `SessionModel`
  after populating a work source (marking a row dirty, filling a queue, etc.).
- **`cancel(TaskId id)`** — Cancel a cancellable task. Calls its
  `onComplete(true)` callback so the task can perform cleanup (emit signals for
  partial work, etc.). The `onComplete` callback is responsible for clearing the
  work source so that `hasWork()` returns false on subsequent ticks. Must only be
  called on tasks registered with `cancellable = true`.

#### Signals

- **`activeTaskChanged(TaskId id, bool cancellable)`** — Emitted when the
  scheduler switches to a different task. Always carries a valid task ID.
  Emitted before the first `step()` of a new task. The `cancellable` flag
  tells the UI whether to show a cancel button for this task.
- **`progressChanged(TaskId id, int remaining, int total)`** — Emitted when
  progress updates. Emitted in two situations: (1) immediately when the active
  task changes (by querying the new task's `progress()` callback), so the UI
  shows the correct starting state; (2) after each `step()`, by querying the
  active task's `progress()` callback.
- **`schedulerIdle()`** — Emitted when all tasks report no work. Distinct from
  `activeTaskChanged` because going idle is logically different from switching
  tasks — the UI hides the progress bar rather than updating it.

### Scheduler Tick Logic

On each timer tick:

1. Iterate tasks in priority order (lowest priority number = highest priority).
2. Find the first task where `hasWork()` returns true.
3. If no task has work: emit `schedulerIdle()`, stop the timer, return.
4. If this is a different task than the previous tick (or the first tick after
   waking): emit `activeTaskChanged(id)` and `progressChanged(id, ...)`.
5. Call `step()`.
6. Query `progress()` and emit `progressChanged(id, remaining, total)`.
7. Check if `hasWork()` is still true for the active task, or if any task has
   work. If so, re-arm the timer. Otherwise, call `onComplete(false)` for the
   task that just finished, then check for more work (which may trigger a new
   active task or idle).

### Completion and Cancellation

When a task's `hasWork()` returns false after a `step()`, the scheduler calls
its `onComplete(false)` callback. This is where `SessionModel` performs batch
finalization: flushing the index, emitting `modelChanged`, emitting batched
`visibilityChanged`, emitting `dataChanged` for affected row ranges, etc.

Not all tasks are cancellable. The save worker, for example, must not be
cancelled — unsaved data exists only in memory and would be lost. The
`cancellable` flag in `TaskDef` controls whether the UI shows a cancel button
for the task, and `cancel()` must only be called on cancellable tasks.

When `cancel(id)` is called on a cancellable task, the scheduler calls its
`onComplete(true)` callback. The callback clears the work source and performs
any partial-batch cleanup. On the next evaluation, `hasWork()` returns false
and the scheduler moves on.

The `onComplete` callback receives a `bool cancelled` parameter so the task can
distinguish full completion from cancellation — both may require cleanup, but
the specifics may differ.

### Re-entrancy

A task's `step()` or `onComplete()` callback may call `wake()` or `cancel()` on
the scheduler (e.g., saving a session discovers column work is needed). The
scheduler handles this by deferring re-evaluation to the next tick rather than
recursing. Since the timer is single-shot and re-armed after each tick, this
falls out naturally: `wake()` during a callback ensures the timer is armed, and
the next tick re-evaluates from scratch.

### Wake Behavior

When `wake()` is called:

- If the scheduler is idle: evaluate tasks, select the highest-priority one with
  work, emit `activeTaskChanged` and `progressChanged`, then arm the timer.
- If the scheduler is already ticking: do nothing beyond ensuring the timer is
  armed. The next tick will naturally re-evaluate priorities.

This means that if a high-priority task gets new work while a low-priority task
is running, the switch happens on the very next tick.

## How SessionModel Uses the Scheduler

`SessionModel` owns an `IdleScheduler` and registers four tasks at construction,
using an enum for task IDs. The existing step functions (`saveNextSession`,
`loadNextVisibleSession`, `processNextBulkEdit`, `processNextDirtyColumn`)
become the step callbacks. The existing counters and `hasWork` logic become the
`hasWork` and `progress` callbacks. The existing completion logic (index
flushing, signal emission) moves into `onComplete` callbacks.

The four per-worker timers (`m_saveTimer`, `m_loadTimer`, `m_bulkEditTimer`,
`m_columnWorkerTimer`) are removed. All priority checks within tick handlers
(e.g., column worker checking if saver is active) are removed. All explicit
resumption chains (e.g., saver finishing and restarting the loader) are removed.

Where code currently calls `m_saveTimer.start()` or `m_loadTimer.start()`, it
instead calls `m_scheduler.wake()`.

## Progress Bar Integration

The logbook dock connects to the scheduler's signals to drive a single progress
bar with a cancel button:

- **`activeTaskChanged(id, cancellable)`** — Show the progress bar, update the
  label (map task ID to display text), show or hide the cancel button based on
  the `cancellable` flag.
- **`progressChanged(id, remaining, total)`** — Update the bar value.
- **`schedulerIdle()`** — Hide the progress bar.
- **Cancel button click** — Call `scheduler.cancel(activeTaskId)`. The button is
  only visible for cancellable tasks, so the caller always knows this is valid.

The scheduler is standalone — it does not know about progress bars or docks.
This means the same pattern could be used in other docks (e.g., analysis) with
their own progress UI, either with the same scheduler instance or a separate
one.

## Notes for Implementer

### Progress Bar Consolidation

The current UI has separate progress bars per worker. The new design uses a
single progress bar that switches between tasks. When the active task changes
(e.g., save preempts column computation), the bar immediately shows the new
task's label and progress. When resuming a preempted task, the bar shows that
task's current state — the total stays at the original high-water mark, so the
user sees continuous progress.

### Step Functions

The existing tick handlers contain the preemption/resumption logic interleaved
with the actual work. When extracting step callbacks, strip out the
preemption checks and resumption chains — the scheduler handles all of that.
What remains is the pure "do one unit of work" logic.

### Cancellation

The existing `cancelLoader()`, `cancelColumnWorker()`, and `cancelBulkEdit()`
methods become thin wrappers around `m_scheduler.cancel(taskId)`. The work
source cleanup moves into the `onComplete(true)` callback.

## Definition of Done

1. `IdleScheduler` is a standalone class in its own header/source with no
   dependencies on `SessionModel` or any other domain class
2. All four workers are registered as tasks on the scheduler — no per-worker
   timers, no hand-rolled preemption checks, no explicit resumption chains
3. Priority behavior is preserved: save > load > bulk edit > column
4. Progress bar in the logbook dock works correctly, showing the active task's
   label and progress, switching seamlessly on preemption/resumption
5. Cancel button is shown only for cancellable tasks (load, bulk edit, column)
   and hidden for non-cancellable tasks (save); cancelling moves the scheduler
   to the next task with work (or idle)
6. `onComplete` callbacks fire on both completion and cancellation, with the
   cancelled flag set appropriately
7. All existing functionality works correctly — saving, loading, bulk editing,
   column computation
8. Build passes with no errors
