# Phase 2: The status bar

## Purpose

This phase gives background work its one home. A new component,
`StatusBarFeature`, fills the main window's status bar with:

- the **activity area**: a label, a progress bar and a cancel button;
- the **warning**.

It covers spec §5, §6 and §7 (the warning's presentation), and §12 for the
main window and the scheduler. From §9 it takes the logbook's progress line,
its cancel button, the dock feature's scheduler wiring, the fixed minimum
size hint, and the fill's display progress. Once the status bar shows the
scheduler's tasks and the computations, the logbook's progress line has no
job left and goes in the same change, so no fact is shown twice or nowhere.
The per-source arcs and badges stay until phase 3.

## Dependencies

- **Depends on phase 1.** This phase assumes the following exists exactly as
  the overview ("Phase 1 provides") and the phase 1 document state it:
  - `DemandProgress`, `SessionFailures` (with `kListLimit`, `text()` and
    `listText()`);
  - `CalculationDemand::progress()`, `failures()`, `progressChanged()` and
    `failuresChanged()`.

  Phase 1 fixed the text form: `listText()` has no header line.
- **Blocks phase 3**, which removes the per-source indicators once the status
  bar is where progress is shown.
- **Code at the start:** the committed head after phase 1. `LogbookView`
  still has its progress line. `LogbookDockFeature` still wires
  `IdleScheduler::activeTaskChanged`, `progressChanged`, `schedulerIdle` and
  the cancel button. `DemandFill::registerTask()` still registers
  `/*progress*/ [this] { return Progress{m_remaining, m_highWater}; }`.
  `MainWindow` has no status bar.

## What changes

### The status bar component (new: `src/ui/statusbar/StatusBarFeature.{h,cpp}`)

`FlySight::StatusBarFeature` is a `QObject` (overview decision 6).

- **Not a dock feature.** It is not a `DockFeature`, and `DockRegistry` does
  not know it.
- **Constructor:**
  `explicit StatusBarFeature(const AppContext &ctx, QStatusBar *statusBar, QObject *parent = nullptr);`
- **What it adds to the bar:** the activity area and the warning, each once.
  The widgets are children of the bar.
- **What it reads:**
  - `ctx.sessionModel->scheduler()`;
  - `ctx.calculationDemand`, held weakly (`QPointer`), which may be null.
- **What it does not use:** `ctx.jobQueue`, `ctx.workingClock`, and
  `QStatusBar::showMessage()`. A temporary message hides a bar's normal
  widgets.
- **Class comment.** Its contract, in prose:
  - it presents the scheduler's tasks and the demand layer's progress and
    failures, and decides nothing;
  - the scheduler has no active-task query, so the component must exist before
    the scheduler's first tick. A component made later shows no task until
    the next `activeTaskChanged`.

**Follow the scheduler from construction.** Connect `activeTaskChanged`,
`progressChanged` and `schedulerIdle` in the constructor. Keep three things:
the active task's id, its `cancellable` flag, and the last
`(remaining, total)` reported *for that id*.

- A report for another id changes nothing, as `LogbookView::onProgressChanged`
  does today.
- `schedulerIdle` clears the active task.
- The demand layer's `progress()` and `failures()` are read at construction,
  and again on its `progressChanged` and `failuresChanged`.

**Items** (spec §4, §5):

- **Scheduler tasks.** The active task is an item when its id is `SaveTask`,
  `LoadTask`, `BulkEditTask` or `ColumnTask`. Their labels are "Saving
  sessions", "Loading sessions", "Updating sessions" and "Computing columns".
- **The fill.** `ColumnFillTask` is never an item.
- **Other ids.** No other id is registered in the product (the `WorkerTask`
  enum is the one namespace), and none is an item. Today's "Working"
  fallback label goes.
- **The computations.** "Computing results" is an item while the demand layer
  lives and `progress().count > 0`.

**Counts.** A label reads `"<label>: <done> / <total>"`, and the bar has range
0..total and value done. The label is `tr()` text.

- **A task:** done = total − remaining.
- **The computations** (overview decision 3): total = `highWater`, and done =
  `highWater − count`.
- `"Computing results: 5 / 12"` means five of twelve done.

**The one rule** (spec §6). The shown item is:

1. the active task, if it is an item;
2. otherwise the computations, if they are an item;
3. otherwise nothing. The activity area is then empty: no text, no bar, no
   button.

While the fill is the active task, the computations are therefore shown.

**The hover** of the activity area is a tooltip on its label and bar. It is
empty when nothing is shown. It has one line per item in progress, the shown
item first, each in the label's form. The computations' line is followed by
one line indented by two spaces when `progress().sessionName` is not empty.
That line is the session name, then `": " + progressText` when the text is not
empty. For example:

```text
Updating sessions: 1 / 4
Computing results: 0 / 3
  Jump 1: step 1
```

**The cancel button** is a `QToolButton`, as the logbook's is today. It shows
exactly while the shown item is a scheduler task whose `activeTaskChanged`
flag was `cancellable`. A click calls `IdleScheduler::cancel(<that id>)`.

- It is absent while saving (not cancellable) and while the computations are
  shown.
- It is never shown for the fill.

**The warning** (spec §7) shows exactly while the demand layer lives and
`failures()` is not empty. It is independent of the activity area, so it is
shown beside computations and stands alone once they end.

- **Glyph:** the style's `QStyle::SP_MessageBoxWarning` icon (overview
  decision 1) at the small icon size.
- **Text:** "1 recording could not be computed", or "<n> recordings could not
  be computed", where n is `failures().size()`. Each entry is one recording.
- **Tooltip:** exactly `SessionFailures::listText(failures())`.
- It is a label, not a control: nothing dismisses it.

**The height never changes** (overview decision 7). Idle, working, with the
cancel button and with the warning, the bar has one height. Either:

- keep each widget's size while it is hidden
  (`QSizePolicy::setRetainSizeWhenHidden`), or
- give the bar a fixed height.

**The demand layer destroyed first.** `~MainWindow` deletes the demand layer
before the window's children. The component connects the demand layer's
`&QObject::destroyed`, and from then on shows no computations and no warning,
at once. Scheduler tasks are still shown. A null demand layer at construction
behaves the same way from the start. Use the component or its widgets as
connection context, so that nothing runs once either is gone.

**Names the audit forbids here.** `src/ui` is searched by these rules, so do
not use these words for a name or in a comment:

- `progressLabel`: "the views keep no label, no cluster and no clock logic";
- `cancelPressed`: "no gesture entry points";
- "cancel control" or "cancel icon": "no refresh or cancel control". Say
  "cancel button".
- `JobQueue` or `JobModel`: "no jobs window, no view of the queue". Say "the
  executor".
- `controlRect(`, `controlCount(`, `Control::Cancel`: "the plot rows'
  controls are gone".

### The main window (`src/mainwindow.cpp`)

- **Where it is made.** Create the component with
  `new StatusBarFeature(ctx, statusBar(), this)` immediately after
  `m_features = DockRegistry::createAll(ctx, this);` and the `addDockWidget`
  loop. That is before the layout restore, the menus and a first-launch
  profile, and before the event loop can tick the scheduler.
- **The status bar itself.** `statusBar()` creates it, so `mainwindow.ui`
  needs no change.
- **Comments.** Update the comment above the executor/demand creation and the
  one in `~MainWindow`: the status bar holds the demand layer weakly, as the
  views do. The destruction order does not change.
- **`AppContext.h`.** Update the comment of `calculationDemand` to name the
  status bar among its readers.
- **Check by hand.** The harness cannot construct `MainWindow`. After the
  build, start the application once and confirm the bar is at the bottom of
  the window.

### The logbook view and its dock feature

- **`LogbookView.h/.cpp` lose:**
  - `m_progressBar`, `m_cancelButton` and `m_activeTaskId`;
  - the slots `onActiveTaskChanged`, `onProgressChanged` and `onSchedulerIdle`,
    with their five labels;
  - the signal `cancelRequested`;
  - the `minimumSizeHint()` override;
  - the includes and the layout they needed.

  The view's layout holds the tree only. The class comment stops naming the
  progress line. The constructor's signature is unchanged; phase 3 changes
  it.
- **`LogbookDockFeature.cpp` loses** the block from
  `IdleScheduler &sched = ctx.sessionModel->scheduler();` through the cancel
  connection, and `#include "idlescheduler.h"`.

### The fill (`src/demandfill.{h,cpp}`)

The fill registers **no progress function**: `/*progress*/ nullptr`.
`reportProgress()` already skips such a task (overview decision 2).

- **Goes:** `m_highWater`, its bookkeeping in `update()`, and the
  `onComplete` that reset it. `onComplete` becomes null, since it has nothing
  left to do.
- **Stays:** `m_remaining`, or a flag, as the implementer prefers, because
  `hasWork()` needs it.
- **Waking the scheduler.** `update()` must still wake the scheduler when
  `hasWork()`, the candidates or the number of holds change. A change of the
  count alone no longer needs a wake, because there is no report to refresh.
- **Unchanged** (the scheduler contract): priority 5, `hasWork`, `canStep`,
  `cancellable = false`, and completion when the work is gone.
- **Comments.** Rewrite the class comment and `update()`'s comment where they
  say "reports progress", "high-water mark" or "the logbook's progress line".
- **Untouched:** `src/sessionmodel.*` and the rest of the demand layer.
  `src/idlescheduler.*` changes only as the next section says.

### The scheduler's query (`src/idlescheduler.{h,cpp}`)

Spec §12 as amended: the scheduler keeps its contract and gains one
read-only query.

- `bool hasWork() const;` true while some registered task's `hasWork()` is
  true, the fill included, whether or not the task can step. It neither ticks
  nor wakes, and changes no signal, completion rule or priority.
- Declare it beside `isTicking()`, with a comment that states it in the
  scheduler's own terms. The audit's "the idle scheduler learns nothing about
  jobs or demand" forbids `Demand`, `Calculation`, `Job` and `Executor` (any
  case of the first letter) in these two files, and "the idle scheduler is
  never paused for a calculation" forbids `Fusion` and `JobQueue`; the
  comment names none of them.

### Test support (`tests/support/testenvironment.{h,cpp}`)

**The problem.** `waitForIdle()` detects work by `progressChanged` ("A tick
that finds work emits progressChanged") and recognises an already-idle
scheduler by a sentinel timer, because the scheduler had no query. Without
the fill's progress function, a tick that only steps the fill or rests on it
emits nothing, and the helper would return while the fill still has work,
which it never did before.

**The change.** Keep the helper's documented contract: it returns true only
when no task has work, the fill included. Wake the scheduler, then process
events until `!scheduler.hasWork() && !scheduler.isTicking()`, or until the
timeout. The progress spy, the idle spy and the sentinel timer go. The helper
registers nothing and causes no scheduler signal of its own, so tests that spy
on the scheduler across a wait see only what the tasks cause. Update the
helper's comment and its declaration's comment.

### Build

- **`src/CMakeLists.txt`:** add `ui/statusbar/StatusBarFeature.h` and
  `ui/statusbar/StatusBarFeature.cpp` to the application's sources, beside the
  `ui/docks/...` block.
- **`tests/CMakeLists.txt`:** see Tests.

## Interfaces

**Consumed from phase 1:**

- `CalculationDemand::progress()`, `failures()`, `progressChanged()` and
  `failuresChanged()`;
- `DemandProgress::{count, highWater, sessionName, progressText}`;
- `SessionFailures::listText()`.

The component does not use `sessionFailures()`, which is phase 3's.

**Consumed from the code as it is:**

- `IdleScheduler::activeTaskChanged(int, bool)`,
  `progressChanged(int, int, int)`, `schedulerIdle()` and `cancel(TaskId)`;
- `SessionModel::WorkerTask` and `SessionModel::scheduler()`;
- `AppContext::sessionModel` and `calculationDemand`.

**Provided** (the contract, as the overview lists it):

- `src/ui/statusbar/StatusBarFeature.{h,cpp}`, class
  `FlySight::StatusBarFeature`, with the constructor above.
  - It is created in `MainWindow`'s constructor after the dock features,
    from the `AppContext` and `statusBar()`.
  - Its test seams are its own. Tests may find its widgets by `objectName`.
    Any accessor added for tests is `const` and returns what a widget shows.
    Phase 5 cites the test functions by name from the tree.
- The labels "Saving sessions", "Loading sessions", "Updating sessions",
  "Computing columns" and "Computing results" live in the status bar only.
- The logbook view has no progress line, no cancel button, no scheduler slot
  and no `minimumSizeHint` override.
- `LogbookDockFeature` connects nothing of the scheduler.
- `DemandFill` registers no progress function.
- `bool IdleScheduler::hasWork() const`, as above.
- `FlySightTest::waitForIdle()` waits on `hasWork()` and `isTicking()`, with
  its contract unchanged.
- The audit rule "the load step is the demand layer's scheduler task" allows
  the status bar's file instead of `LogbookView.cpp`.
- The audit rule "only the application and its views know the demand layer"
  allows `src/ui/statusbar/StatusBarFeature.(cpp|h)`.

## Acceptance criteria

1. **Always present** (§5; decision 7). The status bar exists from
   construction. It is empty when nothing is in progress and nothing has
   failed. Its height is the same idle, with a task shown, with the cancel
   button shown, with the warning, and with both.
2. **Tasks** (§5, §11). For each of the four tasks, the label reads
   `"<label>: <total−remaining> / <total>"` from the scheduler's report for
   that id, and the bar shows the same. A report for another id changes
   nothing. `schedulerIdle` empties the activity area.
3. **Computations** (§5; decision 3). While `progress().count > 0`, the label
   reads `"Computing results: <highWater−count> / <highWater>"`. It counts
   sessions with waiting or running tracks of plots and columns together,
   each session once. A new burst after the count reached 0 starts its own
   total.
4. **Fill** (§5, §12). No label other than "Computing results" is ever shown
   while `ColumnFillTask` is the active task. The cancel button is then
   absent. No `IdleScheduler::progressChanged` is ever emitted with the id
   `ColumnFillTask`.
5. **Rule** (§6). With a task and computations in progress, the task is
   shown. When the task ends, "Computing results" returns without the
   computations having stopped.
6. **Hover** (§6). The tooltip lists every item in progress, the shown one
   first, each with its own count. For the computations, it also gives the
   recording being computed and its step, in the form above. It is empty when
   nothing is shown.
7. **Cancel** (§6). The button is visible exactly while the shown item is a
   task reported cancellable, so for `LoadTask`, `BulkEditTask` and
   `ColumnTask`. It is never visible for `SaveTask`, the fill or the
   computations. A click reaches `IdleScheduler::cancel()` for the shown
   task, and that task completes as cancelled.
8. **Warning** (§7). The warning is visible exactly while `failures()` is not
   empty. It shows the standard warning icon and counts recordings: two
   failed calculations of one recording count as one. Its tooltip equals
   `SessionFailures::listText(failures())`, which caps the list at ten and
   says "(tried again at the next start)" where it applies. It is shown
   beside "Computing results" while computations continue, and alone once
   they end. Disabling the last source over the calculation hides it.
   Clicking it changes nothing.
9. **Restart** (§7). After a restart, a stored rejection wanted by a restored
   column is counted at the demand layer's first pass (from an index without
   `"recordReasons"`, once the column worker's pass has reported the reason),
   without loading the session and without a job. A failure that is not
   stored is absent after the restart until it fails again.
10. **Demand layer gone** (decision 6). After the demand layer is destroyed,
    the computations and the warning are gone at once, a scheduler task is
    still shown, and nothing crashes. A component built over a null demand
    layer shows tasks only.
11. **Logbook** (§9, §12). `LogbookView` has no `QProgressBar`, no cancel
    button, no scheduler slot, no `cancelRequested` and no
    `minimumSizeHint`. `LogbookDockFeature` names no `IdleScheduler`. The
    audit checks both.
12. **Fill contract** (§9; decision 2). `DemandFill` registers no progress
    function and keeps no high-water mark. Its tests of priority, resting,
    completion, holds and shutdown pass.
13. **One place** (§2, §12). In `src`:
    - the five labels appear only in `StatusBarFeature.cpp`;
    - only `StatusBarFeature.cpp` connects the scheduler's three signals;
    - `new StatusBarFeature` appears only in `mainwindow.cpp`.

    The audit checks all three.
14. **The scheduler's query** (§12 as amended). `hasWork()` is true while
    any task has work, including a task that cannot step, and false once none
    has; calling it neither ticks nor wakes the scheduler. `waitForIdle()`
    returns only when no task has work and no tick is due, registers no task
    and emits nothing on the scheduler. Every existing caller passes.
15. **Green.** The whole suite and `audit_cleanup` pass in `build-agent/`,
    Release, run sequentially.

## Tests

### New executable: `tst_status_bar` (`tests/tst_status_bar.cpp`)

**Registration.** Register it in the `FLYSIGHT_BUILD_WIDGET_TESTS` block of
`tests/CMakeLists.txt`, following `tst_logbook_indicators`:

- sources: `tst_status_bar.cpp` plus
  `"${FLYSIGHT_SRC_DIR}/ui/statusbar/StatusBarFeature.cpp"` and `.h`;
- `LIBS Qt${QT_VERSION_MAJOR}::Widgets`;
- labels `"core;widgets"`.

Update the file's head comment, and the block's comment, which says
tst_plot_row_delegate and tst_logbook_indicators are the only tests that link
Qt Widgets. The block's description of tst_logbook_indicators also drops
"progress line".

**Main.** The test writes its own `main()`, as `tst_logbook_indicators` does:
deterministic seed, `QApplication`, Fusion style, `TestEnvironment`.

**Fixture.** Follow `tst_logbook_indicators::init()`:

- `JobWorld`, `PlotFixture`, a real `SessionModel` with s1..s4 named "Jump
  1".."Jump 4", `JobQueue`, `PlotModel`, `CalculationDemand`;
- a `QMainWindow`, shown offscreen, whose `statusBar()` the component fills;
- a context with the model and the demand layer.

**Reading and driving.**

- Tests read what the user sees: the label's text, the bar's value and range,
  the tooltips, visibility, and the bar's height.
- They click the real button with `QTest::mouseClick`.
- They may emit the scheduler's signals by hand, since Qt signals are public.
- To see the label at a task's activation, connect a slot after the
  component's.

**Functions.** The names are suggestions; phase 5 cites whatever the tree
holds.

- **`schedulerTasksShowTheirLabelsAndCounts`** (criteria 1, 2):
  - by hand, each task's label, bar and button;
  - a report for another id changes nothing;
  - idle is empty;
  - live, a real bulk edit is shown as "Updating sessions: k / n".
- **`computationsAreOneItem`** (criteria 3, 4):
  - setup: a checked plot over visible sessions and a column over stubs,
    with the gate held;
  - one "Computing results" count across both, falling as the gate opens;
  - while the fill is active, "Computing results" is shown with no button;
  - a later burst starts its own total;
  - no `ColumnFillTask` report.
- **`taskShownOverComputationsAndHoverListsBoth`** (criteria 5, 6): a bulk
  edit while a gated computation runs. The label is "Updating sessions"; the
  hover is exactly the three-line form, with "Jump 1: step 1"; then
  "Computing results" returns.
- **`cancelOnlyForACancellableShownTask`** (criterion 7):
  - by hand, visibility for every task, for the fill and for the
    computations alone;
  - live, a click during a real bulk edit or visible load cancels it. Its
    remaining rows are not edited, or the rows still queued for loading are
    unticked.
- **`warningBesideComputationsThenAlone`** (criterion 8):
  - a stored rejection (EA1, s1 at -1) completes first;
  - `G_OUT` with the gate held shows the warning beside "Computing results";
  - the warning stands alone after the gate opens.
- **`warningCountsRecordingsAndListsThem`** (criterion 8):
  - s1 failing two calculations counts "1 recording";
  - a second recording counts 2;
  - the tooltip equals `listText(failures())` and includes the retry
    wording.
- **`warningListsAtMostTenRecordings`** (criterion 8): twelve failing
  recordings. The text says twelve; the tooltip ends with "and 2 more".
- **`warningAbsentWhenNothingFailedAndNotDismissable`** (criterion 8): no
  warning with no failure. A click on the warning keeps it. Disabling the
  column hides it.
- **`warningAfterRestart`** (criterion 9). Follow the "next start" of
  `unreadableRecordPendingIsNotDemandPending`: a new model from the index, a
  new demand layer and component, then `startColumnWorker()`.
  - The stored rejection is counted with no `sessionLoaded` and `Quiet`
    holding.
  - An unstored failure is absent until its retry fails again.
- **`heightNeverChanges`** (criterion 1): the height in every state of
  criterion 1.
- **`survivesDemandDestroyedFirst`** (criterion 10): computations and warning
  shown, then the demand layer reset, then a task by hand. Also a component
  over a null demand layer.

### Amended tests

**`tst_logbook_indicators`:**

- Remove `fillProgressLineHasItsOwnText`.
- In `pendingCellBecomesValueWhenRecordIsWritten`, remove the scheduler
  wiring, the `QToolButton` lookup and the `cancelEverShown` and
  `fillActivations` checks. Its pending-cell assertions and the
  `resetSpy` check stay.
- Update the file's head comment.
- Drop includes that become unused.

**`tst_calculation_demand`** has three tests that read the fill's
`progressChanged`. Each keeps what it asserts about the scheduler, and reads
counts from the demand layer's `progress()`:

- **`fillTaskReportsProgressWhileWaiting`: rename it** (suggested
  `fillTaskRestsWhileWaiting`). It keeps:
  - the resting with both holds taken (no tick, no idle, no load);
  - one load per session;
  - the fill active at each job's end;
  - no idle before the last job ends, and idle after it;
  - the next fill starting after an edit.

  Replace the `"P r/t"` events with `activeTaskChanged` and `schedulerIdle`
  events, and assert that no report carries `ColumnFillTask`.
- **`fillEndingBehindAnotherTaskStartsNextCountFresh`: keep the name.** Its
  counts become `progress().count` and `highWater`: the next fill's first
  value is count 1 of 1.
- **`fillTaskIsLowestAndNotCancellable`: keep the name.**
  - The activations and `cancel()` changing nothing stay.
  - The falling count 4, 3, 2, 1, 0 of 4 is read from `progress()` on each
    `progressChanged` of the demand layer.

Remove `progressOf()` if nothing uses it any more.

**`tests/support/testenvironment.*`:** `waitForIdle()`, as described above.

**`tst_session_model_engine`:** add a test of the query beside
`schedulerCompletesWaitingTaskWhoseWorkIsGone` (suggested
`schedulerHasWorkFollowsItsTasks`): false with no task; true with a task that
has work and can step, and with one that has work and cannot step; false once
their work is gone; and a call while resting leaves `isTicking()` false.

### Audit (`tests/audit/cleanup_audit.cmake`, group `demand`)

**Change:**

- **"only the application and its views know the demand layer":** add
  `^src/ui/statusbar/StatusBarFeature\\.(cpp|h)$` to the allowed regex.
- **"the load step is the demand layer's scheduler task":** replace
  `^src/ui/docks/logbook/LogbookView\\.cpp$` with
  `^src/ui/statusbar/StatusBarFeature\\.cpp$`. Its comment says the status
  bar maps the fill to the computations.
- **"the fill has a progress text of its own":** remove it. Spec §9 removes
  its subject. It is replaced by the first new rule below.
- **"the views learn of the demand layer's end in one place":** add
  `^src/ui/statusbar/StatusBarFeature\\.cpp$`. The status bar is the second
  view of the demand layer's end. Phase 3 revisits the rule when
  `DemandIndicatorView.*` goes.
- **"the demand views handle no event of their own":** add
  `"src/ui/statusbar/StatusBarFeature.*"` to its pathspec. The warning is not
  a control.

**Add** (plant a hit once to prove each):

- `expect_only("the status bar names background work", "\"(Saving|Loading|Updating) sessions|\"Computing (columns|results)", "^src/ui/statusbar/StatusBarFeature\\.cpp$", src)`;
- `expect_only("the status bar is the one view of the scheduler's tasks", "IdleScheduler::(activeTaskChanged|progressChanged|schedulerIdle)", "^src/ui/statusbar/StatusBarFeature\\.cpp$", src)`;
- `expect_only("the main window makes the status bar", "new StatusBarFeature", "^src/mainwindow\\.cpp$", src)`;
- `expect_none("the logbook presents no task progress", "QProgressBar|IdleScheduler|minimumSizeHint|cancelRequested", "src/ui/docks/logbook")`;
- `expect_none("the fill reports no progress of its own", "Progress\\{|[Hh]igh[-]?[Ww]ater", "src/demandfill.*")`.

### Acceptance map (`tests/acceptance_map.txt`)

Every line must name a function the tree has. A line whose test no longer
asserts its item is re-pointed to the test that does. New items (701 on) and
the "(as amended)" restatements are phase 5's.

| Line today | After this phase |
|---|---|
| `534 tst_logbook_indicators pendingCellBecomesValueWhenRecordIsWritten` | `534 tst_status_bar cancelOnlyForACancellableShownTask` |
| `539 tst_logbook_indicators pendingCellBecomesValueWhenRecordIsWritten` | `539 tst_status_bar computationsAreOneItem` |
| `539 tst_logbook_indicators fillProgressLineHasItsOwnText` | `539 tst_status_bar schedulerTasksShowTheirLabelsAndCounts` |
| `539 tst_calculation_demand fillTaskReportsProgressWhileWaiting` | removed (the line above covers it) |
| `559`, `647`, `660 tst_logbook_indicators fillProgressLineHasItsOwnText` | the same items, `tst_status_bar computationsAreOneItem` (559) and `schedulerTasksShowTheirLabelsAndCounts` (647, 660) |
| `530`, `559`, `609`, `612`, `642`, `653 tst_calculation_demand fillTaskReportsProgressWhileWaiting` | the renamed function |

All other lines stay, including every other line that cites
`pendingCellBecomesValueWhenRecordIsWritten`.

### README (`tests/README.md`)

- Add a row for `tst_status_bar` to the table of section 1, under "The
  executor, the demand layer and the demand views". It names what the
  functions above prove.
- Update the sentences that count the executables (section 1: 50 executables;
  `ctest -N` 51, or 58 with the exact runs).
- Update the sentences that name the tests linking Qt Widgets: section 1, the
  `FLYSIGHT_BUILD_WIDGET_TESTS` row of section 3, section 8's "are the only
  ones", and "two tests" in the `tst_plot_row_delegate` and
  `tst_logbook_indicators` rows.
- Leave the rest to phase 5 (overview decision 11), which will find the
  removed names: the other rows' descriptions, sections 9, 10 and 12, the
  appendices, and every `docs/` change.

## Decisions

- **The constructor's signature is fixed here.** It is the one call from
  `MainWindow` and from the test, and the overview names both of its
  arguments.
- **Label, hover and warning texts are fixed here** so that the tests and
  phase 5's documentation agree. The hover's indentation matches
  `listText()`. The warning's text avoids "(s)", and its tooltip is
  `listText()` unchanged: no header line, because the text says what the
  list is.
- **No fallback label.** No product task has an id outside the four, and the
  scheduler reports the fill, which maps to the computations. "No generality
  nothing uses."
- **`waitForIdle()` keeps its contract**, rather than narrowing it and
  relying on every caller to use `waitDemandIdle()` first. A silent early
  return would make tests flaky, not fail. It asks the scheduler's new query
  rather than registering a probe task, which would add scheduler signals to
  every test that spies on the scheduler across a wait (overview decision 12).
- **The status bar learns of the demand layer's end itself.** It cannot
  repaint its way to an empty state as the header does, and the destroyed
  signal is the only notice.
- **The warning's placement in the bar is the implementer's choice**, as long
  as it sits beside the activity area and the height is constant.

Ready with caveats:

- the `waitForIdle()` change touches every test executable; it adds no
  scheduler signal, but a caller that relied on the helper having spied on
  `progressChanged` is read before the spy goes;
- `QMainWindow::statusBar()` under KDDockWidgets' main window is checked by
  running the application once, since `MainWindow` cannot be built in the
  harness;
- the audit rule "the views learn of the demand layer's end in one place"
  temporarily allows a second place, which phase 3 settles.
