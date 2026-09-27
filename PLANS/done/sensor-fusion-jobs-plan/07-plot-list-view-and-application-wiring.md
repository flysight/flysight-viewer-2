# Phase 7: Plot list view and application wiring

## Overview

This phase makes the plot list paint what `PlotRequests` (Phase 6) reports and
forward the user's clicks to it, and it puts the job queue into the running
application. A row delegate paints, right-aligned in a plot row, the refresh
control with the missing count, or the progress label ("k of n") with the
cancel control, and independently the warning badge with the failed count; it
supplies the row tooltip; it recognizes the only two gestures (a check made
by direct interaction with the row, a press on the refresh control) plus the
cancel press, and passes them on as explicit calls. `MainWindow` creates and
owns the `JobQueue` and the `PlotRequests`, tears them down in a safe order
with `JobQueue::shutdown()` first in the close path, and the plot widget stops
warning about values that are merely uncomputed. Nothing in this phase decides
state, counts, or what to request (overview Decisions 8 and 9).

## Dependencies

- **Depends on:** Phase 6 (Plot request logic), and through it Phase 5 (Job
  queue and job model) and Phase 4 (Engine: asynchronous request and blocker
  inspection).
- **Blocks:** Phase 10.
- **Assumptions:**
  - Phase 6's API exists exactly as in `06-plot-request-logic.md`:
    `PlotRequests(SessionModel*, PlotModel*, JobQueue*, QObject *parent)`,
    `rowState(plotId)`, `plotCheckedByUser(plotId)` (call only after the
    model holds Checked), `refreshPressed(plotId)`, `cancelPressed(plotId)`,
    `flush()`, signals `rowStateChanged(plotId)` / `rowStatesChanged()`;
    `PlotRowState` with `control()`, `controlCount()`, `showsWarning()`,
    `failedCount`, `progressLabel`, `toolTip`, `isPlain()`. A plot id equals
    `PlotModel::PlotValueIdRole`. `PlotModel` is in `flysight_core`.
  - Phase 5's API exists exactly as in `05-job-queue-and-job-model.md`:
    `JobQueue(SessionModel*, QObject *parent)`, `shutdown()` (idempotent,
    joins the worker without a timeout, later requests answer
    `ShuttingDown`), `isIdle()`, and `~JobQueue()` calling `shutdown()`.
    `SessionModel::publishCalculationInvalidation()` emits
    `dependencyChanged` per name, a `dataChanged` for the row, **and
    `modelChanged`** (Phase 5 Task 5.1). Task 7.6 relies
    on that last signal.
  - Phase 4's `CalculationEngine::blockers(DependencyKey)` returns a
    `BlockerReport` whose `state` is one of `Available`, `Blocked`,
    `NotProduced`, `NotApplicable`, never runs an explicit calculation, and
    may be called while a `SessionModel::RowStabilityGuard` is held.
  - `tests/support/jobfixture.h` (`JobWorld`, `Gate`, `waitIdle`) and
    `tests/support/plotfixture.h` (`PlotFixture`, plots `Syn/g`, `Syn/g2`,
    `Syn/ea`, `Syn/plain`, ...) exist as documented in Phases 5 and 6.
  - Phase 9 (the real fusion plots) may or may not be complete. Nothing in
    this phase depends on it; see "Manual Verification" for what is verified
    now and what is handed to Phase 10.
  - Shared files (root `CMakeLists.txt`, `src/CMakeLists.txt`, `src/mainwindow.cpp`,
    `src/mainwindow.h`, `tests/CMakeLists.txt`, `tests/README.md`,
    `tests/acceptance_map.txt`, `docs/CALCULATIONS.md`) get small, additive,
    separate hunks (overview Decision 10).
  - Commits are made by the orchestrator only (overview "Commit Policy");
    report the exact list of files created and modified.

## Design summary (read before the tasks)

### Files and classes

| Type | File | Role |
|---|---|---|
| `PlotRowGeometry`, `PlotRowMetrics`, `layoutPlotRow()` | `src/ui/docks/plotselection/PlotRowLayout.h` (header only, Qt Core types only) | Pure geometry of the control cluster and its hit rectangle. Testable without Widgets. |
| `PlotRowDelegate : QStyledItemDelegate` | `src/ui/docks/plotselection/PlotRowDelegate.h` / `.cpp` | Paints the cluster, hit-tests, detects the check gesture, shows the tooltip, repaints a row when its state changes. Depends on `PlotRequests`, `PlotModel`'s roles, and Qt Widgets only - not on `AppContext`, KDDockWidgets, or `MainWindow` - so a test can compile it directly. |
| `PlotSelectionDockFeature` | existing | Installs the delegate (three lines). |
| `AppContext` | existing | Gains `jobQueue` and `plotRequests`. |
| `MainWindow` | existing | Creates, owns, and tears down `JobQueue` and `PlotRequests`. |

File names follow the PascalCase convention of `src/ui/docks/plotselection/`.
Everything is in `namespace FlySight`. There is no delegate, `editorEvent`,
or `helpEvent` anywhere in `src/` today (verified by grep), so there is no
in-repo style model for the delegate itself; follow the comment density and
member naming of `PlotSelectionDockFeature.cpp` and `LegendPresenter.cpp`.

### Decision: painted glyphs, no bundled icons

`src/resources.qrc` holds one PNG and the application does not link Qt Svg
(`src/CMakeLists.txt` line 101), so SVG icons would add a Qt component, an
icon-engine plugin, and its deployment on three platforms. The three glyphs
are instead drawn with `QPainter` (antialiased `QPainterPath` strokes) in the
row's text colour taken from the option's palette. That is resolution
independent on high-DPI screens, follows light and dark themes and the
selected-row colour for free, and needs no resource, CMake, or deployment
change.

- **Refresh** (circular arrows): inside the square icon rect inset by one pen
  width, two arcs of 140 degrees each, starting at 20 and 200 degrees, each
  ending in a small filled triangular arrowhead tangent to the arc.
- **Cancel** (circled x): a circle outline inset by one pen width, and two
  diagonals spanning the middle 40% of the rect.
- **Warning**: a filled upward triangle with rounded joins in a fixed amber
  (`QColor(0xE6, 0x9F, 0x00)`, readable on light, dark, and highlight
  backgrounds) with an exclamation mark (a short line and a dot) in
  `QColor(0x20, 0x20, 0x20)`.
- Pen width `qMax(1.0, iconSide / 9.0)`, round caps. Glyph colour:
  `option.palette.color(cg, selected ? QPalette::HighlightedText : QPalette::Text)`
  with `cg` chosen as `QStyledItemDelegate` does (Disabled when the item is
  not enabled, Inactive when the view is not active, else Normal).
- Each glyph is one small file-static function
  `void drawRefreshGlyph(QPainter *, const QRectF &, const QColor &)` etc.

### Row layout (normative)

Left to right in a left-to-right layout; mirrored as a whole in right-to-left:

```
[check] plot name (elided) ........ [warn][failedCount]   [label][control] |
```

- `label` is `QString::number(state.controlCount())` for `Control::Refresh`
  and `state.progressLabel` ("k of n") for `Control::Cancel`. The job's
  progress text is **not** painted; it is in the tooltip only.
- The control icon occupies the right-most slot, so refresh and cancel appear
  in the same place (the browser reload/stop convention) and the cluster does
  not jump when the state flips. The warning group is to its left and is
  present only when `state.showsWarning()`.
- Metrics, derived in the delegate from the option:
  `iconSide = qMin(option.rect.height() - 2, option.fontMetrics.height())`,
  `spacing = qMax(2, iconSide / 4)`, `rightMargin = spacing`. The gap between
  the warning group and the control group is `2 * spacing`.
- Icons are vertically centred in the item rect; text rects span the item
  rect's full height and are drawn `Qt::AlignVCenter`.
- `controlHit` - the rectangle that counts as "on the control" - is the
  control icon's column over the full row height, extended from
  `controlIcon.left() - spacing / 2` to the item rect's right edge. The label
  and the warning group are not clickable.
- `clusterWidth` is the distance from the left edge of the left-most present
  element to the item rect's right edge.
- The row height is unchanged: `sizeHint()` is **not** overridden, so rows
  never change height when their state changes and plain rows keep today's
  geometry.
- The plot name is elided, never the cluster: the delegate computes the
  style's text rect (`QStyle::SE_ItemViewItemText`), subtracts
  `clusterWidth + spacing` and twice the style's text margin
  (`PM_FocusFrameHMargin + 1`), and replaces `opt.text` with
  `opt.fontMetrics.elidedText(opt.text, Qt::ElideRight, qMax(0, available))`
  before handing the option to the style. One `CE_ItemViewItem` call then
  paints background, selection, hover, focus, check box, and text over the
  **full** item rect, exactly where they are today; the cluster is painted on
  top afterwards, clipped to the item rect.

```cpp
// PlotRowLayout.h ------------------------------------------------------------
struct PlotRowMetrics  { int iconSide = 0; int spacing = 0; int rightMargin = 0; };
struct PlotRowGeometry {
    QRect warningIcon, warningCount;   // null when no warning is shown
    QRect controlLabel, controlIcon;   // null when no control is shown
    QRect controlHit;                  // null when no control is shown
    int   clusterWidth = 0;            // 0 when nothing is shown
};
PlotRowGeometry layoutPlotRow(const QRect &itemRect, const PlotRowMetrics &metrics,
                              bool showsWarning, int warningCountWidth,
                              bool showsControl, int controlLabelWidth,
                              Qt::LayoutDirection direction);
```

Text widths are passed in as integers (the delegate measures them with
`option.fontMetrics.horizontalAdvance()`), so the function needs no font and
no GUI application. Right-to-left is produced by computing left-to-right and
mirroring every rect about the item rect's vertical centre line.

### Plain rows are untouched

`paint()` starts with: no `PlotRequests`, no plot id (a category row), or
`rowState(id).isPlain()` -> `QStyledItemDelegate::paint()` and return. The
same early return guards `editorEvent()`'s control handling and
`helpEvent()`. Every row of a plot that is not backed by an explicit
calculation, every unchecked row, and every row with nothing pending,
missing, or failed is therefore painted and handled by the unmodified base
class (spec 9.2 last bullet), which a test pins by comparing rendered images.

### Gestures and hit-testing (normative)

`editorEvent()` is the single entry point; the view delivers mouse press,
release, double-click, and key press events to it regardless of the view's
`NoEditTriggers` setting (that is how the check box works today).

1. `id = index.data(PlotModel::PlotValueIdRole).toString()`. If the
   delegate has no `PlotRequests` or `id` is empty, return the base class
   result.
2. **Control handling**, for `QEvent::MouseButtonPress`, `MouseButtonRelease`,
   `MouseButtonDblClick` with the left button, when
   `state.control() != None`; `inside` means `geometry.controlHit` contains
   the event position:
   - Press and `inside`: remember `m_pressedIndex = QPersistentModelIndex(index)`
     and `m_pressedControl = state.control()`; return `true` (the row is not
     selected by this press, exactly as a press on the check box behaves).
   - Double-click and `inside`: forget the press; return `true`. (Qt delivers
     no release to the delegate after a double-click, so a rapid double click
     on refresh requests once and can never land on the cancel control that
     replaced it.)
   - Release: take and clear the remembered press. If it was armed for this
     index: when `inside` and `state.control() == m_pressedControl`, call
     `refreshPressed(id)` or `cancelPressed(id)`; in every armed case return
     `true` (a press on the control followed by a release elsewhere in the
     row does nothing, not even a check toggle).
   - Any other press clears the remembered press.
3. **Check gesture**, for every event not consumed above:
   `before = index.data(Qt::CheckStateRole)`; `handled = QStyledItemDelegate::editorEvent(...)`;
   `after = index.data(Qt::CheckStateRole)`. If `before != Qt::Checked` and
   `after == Qt::Checked`, call `m_requests->plotCheckedByUser(id)`. Return
   `handled`.

Step 3 identifies a check "by direct interaction with the row" precisely: the
base class writes `Qt::CheckStateRole` only for a left-button release inside
the check indicator and for Space / Select on the current row, and it does so
synchronously inside this call, so the model already holds Checked (and
`PlotRequests` has already seen the `dataChanged`) when `plotCheckedByUser()`
is called. Nothing else reaches this function: `MainWindow::togglePlot`
(Plots menu and shortcuts, `src/mainwindow.cpp` lines 1156-1165, 1314-1320),
`applyProfile()` (`src/profilestatebridge.cpp` line 167), and the settings
restore in `PlotModel::setPlots()` (`src/plotmodel.cpp` lines 101-103) write
the model directly and are, by construction, not gestures. The delegate never
listens to `dataChanged` and never infers a gesture from a model change.
Unchecking is not a gesture; `PlotRequests` prunes on its own.

**Keyboard.** Space on the current row is a direct interaction with the row
and therefore the check gesture (it goes through step 3). There is no
keyboard surface for refresh or cancel, no context menu, no main-menu action,
and no shortcut: spec 9.3 allows exactly two gestures and spec 12 says "when
in doubt, it is not". A keyboard user starts work by unchecking and checking
the row. Phase 10 documents this as a known limitation.

### Tooltip

`helpEvent()` handles `QEvent::ToolTip`: `text = toolTipFor(index)`
(`rowState(id).toolTip`, empty for plain rows). Non-empty:
`QToolTip::showText(event->globalPos(), text, view->viewport(), option.rect)`
and return `true`. Empty: return the base class result (which hides any
tooltip, as today). The whole row shows the tooltip, not only the cluster.
`PlotModel` gains no `Qt::ToolTipRole` and no knowledge of `PlotRequests`.
The delegate adds no wording of its own: every string it shows comes from
`PlotRowState`.

### Repaint

The delegate's constructor takes the view:
`PlotRowDelegate(PlotRequests *requests, QAbstractItemView *view)` (the view
is the parent). It connects `PlotRequests::rowStateChanged(plotId)` to a slot
that maps the id to an index with
`model->match(model->index(0, 0), PlotModel::PlotValueIdRole, plotId, 1, Qt::MatchExactly | Qt::MatchRecursive)`
and calls `view->update(index)`. `plotmodel.*` is not edited. The plot tree
has on the order of a hundred leaves and the signal is emitted only for rows
that changed, so the lookup is negligible even for progress-text updates.
`rowStatesChanged()` is not used. There is no animation and no timer in the
view: progress is the text "k of n", which changes only when a job ends.

### Application wiring and teardown

- `MainWindow` creates `m_jobQueue = new JobQueue(model, this)` and
  `m_plotRequests = new PlotRequests(model, m_plotModel, m_jobQueue, this)`,
  in that order, in the constructor body directly after `m_measureModel` is
  created (`src/mainwindow.cpp` line 196) and before the `AppContext` is
  filled: after the session model is populated and the built-in calculations
  are registered, before any dock feature exists. `PlotModel::setPlots()`
  runs later (line 269) and reaches `PlotRequests` as an ordinary
  `modelReset`; the first-launch `applyProfile()` (line 291) reaches it as
  ordinary `dataChanged`. Neither is a gesture, so **startup starts no job**
  (acceptance 16).
- `closeEvent()` calls `m_jobQueue->shutdown()` **first**, before
  `flushDirtySessions()`, `saveDockLayout()`, and the base class. When the
  queue is not idle the call is bracketed by
  `QApplication::setOverrideCursor(Qt::WaitCursor)` /
  `restoreOverrideCursor()`, because the wait lasts up to one solver step.
  Nothing in `closeEvent()` can veto the close today; a future veto must be
  decided before `shutdown()` is called, because a shut-down queue refuses
  every later request. State this in a comment at the call.
- `~MainWindow()` deletes `m_plotRequests`, then `m_jobQueue`, explicitly and
  before `delete ui`. `QObject` deletes children in creation order, which
  would destroy the `SessionModel` (created in the initializer list) before
  the queue; the explicit deletes make the order
  `PlotRequests` -> `JobQueue` (its destructor calls the idempotent
  `shutdown()`) -> everything else, also on a path that never ran
  `closeEvent()`. The delegate holds `QPointer<PlotRequests>` and degrades to
  the base delegate when it is gone.
- No signal of `JobQueue` or `PlotRequests` is connected to anything that
  shows a dialog. No message box, status-bar message, or progress dialog is
  added for any calculation outcome (spec 9.2). The `QProgressDialog` at
  `src/mainwindow.cpp` line 581 is the file-import dialog that exists on
  `master`; it stays and nothing like it is added.

### Warning suppression rule (spec 9.6)

A grep of every `qWarning` in `src/` finds exactly one reader that warns when
a plot value is absent: `PlotWidget::updatePlot()`,
`src/ui/docks/plot/PlotWidget.cpp` line 532. The legend
(`LegendPresenter.cpp`, `LegendTableModel.cpp`), the measure tool, the map
models, `logbookcolumn.cpp`, and `dataexporter.cpp` have no such warning
(`dataexporter.cpp` lines 158 and 165 concern stored attributes only). The
"Cannot calculate ... due to missing ..." warnings in `src/calculations/` are
about absent inputs of on-demand calculations, not about uncomputed explicit
values, and are not touched.

Rule at the one site: when `yData.isEmpty()`, ask the engine,
`session.calculationEngine().blockers(DependencyKey::measurement(sensorID, measurementID)).state`;
warn only when the state is neither `Blocked` nor `NotProduced`; `continue`
in every case as today. The engine is asked, not `PlotRequests::rowState()`,
which may be one event-loop pass behind (Phase 6 note c) and which the plot
widget has no business knowing. The call happens only on the already-empty
path, under the guard that is already held (permitted: inspection reads, it
does not load or evict), so a plot with data costs nothing extra. The
"Time and measurement data size mismatch" warning at line 542 is unchanged:
Phase 6's y-name-only rule guarantees that an available y has its x.

### Results appear through ordinary invalidation only (spec 9.6)

Verified on `master`, nothing to build:

- `PlotWidget` rebuilds on `SessionModel::modelChanged`
  (`PlotWidget.cpp` line 170). Its `dependencyChanged` filter
  (`onDependencyChanged`, lines 2133-2154) only matches measurements that
  already have a graph or that equal the x variable, which would miss a
  checked plot that has no graph yet - but Phase 5's
  `publishCalculationInvalidation()` emits `modelChanged` with every
  publication, so the rebuild is always scheduled.
- `LegendPresenter` recomputes on `modelChanged` (lines 101-103); the
  logbook repaints from the row's `dataChanged`; the measure tool reads at
  interaction time.
- Consequently this phase adds **no** "show the plot when done" hookup: no
  connection from `JobQueue::jobFinished` or `PlotRequests` to `PlotWidget`,
  the legend, or the logbook. If a published result does not appear, the
  defect is in Phase 5's announcement and is fixed there as a
  `Phase 5 fixup`, not worked around here.

### What must not appear (acceptance 20, stated here so review can check)

From `sensor-fusion-clean-port`, none of the following, under any name:

- a `QProgressDialog`, `QEventLoop`, `QCoreApplication::processEvents()`, or
  any modal or nested-event-loop construct connected with a calculation
  (branch `src/calculations/sensorfusioncalculations.cpp` lines 92-100);
- `sensorFusionIsRunning()` or any "a calculation is running" global;
- the idle-scheduler pause (branch `src/idlescheduler.cpp`: the
  `if (Calculations::sensorFusionIsRunning()) { m_timer.start(100); return; }`
  block) - `src/idlescheduler.*` is not edited by this phase;
- the plot-widget rebuild guard (branch `PlotWidget.cpp`: `m_rebuildingPlot`
  with the `QScopedValueRollback` / `qScopeGuard` pair at the top of
  `updatePlot()`). `m_pendingRebuildLevel` is `master`'s own and stays;
- the thread-local re-entrancy guard;
- the branch's `ImuGnssEkf = "_IMU_GNSS_EKF"` session key. Other edits to
  `src/sessiondata.h` are not forbidden in the plan as a whole (Phases 2 and 9
  legitimately add `SessionKeys` there); this phase itself does not edit it.

## Tasks

### Task 7.1: Pure row geometry

**Purpose:** Put every number of the row layout and the hit rectangle in one function that is testable without Widgets.

**Files to create:**
- `src/ui/docks/plotselection/PlotRowLayout.h` - header only; includes `<QRect>`, `<QPoint>`, `<Qt>` only; `PlotRowMetrics`, `PlotRowGeometry`, `inline PlotRowGeometry layoutPlotRow(...)` exactly as in the design summary.

**Files to modify:**
- `src/CMakeLists.txt` - one line in `PROJECT_SOURCES` directly after the `PlotSelectionDockFeature` line (line 355): `ui/docks/plotselection/PlotRowLayout.h`. Task 7.2 adds its line in the same hunk.

**Technical Approach:**
- Build right to left from `itemRect.right() - metrics.rightMargin`: control icon (`iconSide` square, vertically centred), `spacing`, control label (`controlLabelWidth` wide, full item height; omitted with its spacing when the width is 0), then when a warning is shown a gap of `2 * spacing` (only if a control is shown), warning count, `spacing`, warning icon.
- `controlHit` and `clusterWidth` as defined under "Row layout". With neither warning nor control every rect is null and `clusterWidth == 0`.
- Mirror for `Qt::RightToLeft`: `mirrored.moveLeft(itemRect.left() + itemRect.right() - r.right())` for every non-null rect.
- A short header comment draws the row diagram of the design summary and states that the function decides geometry only - what is shown is decided by `PlotRowState`.

**Acceptance Criteria:**
- [ ] For `itemRect = QRect(20, 100, 200, 20)`, metrics `{16, 4, 4}`, control shown with label width 7, no warning, left-to-right: `controlIcon == QRect(200, 102, 16, 16)`, `controlLabel == QRect(189, 100, 7, 20)`, `controlHit == QRect(198, 100, 22, 20)`, `clusterWidth == 31`, warning rects null.
- [ ] Adding a warning with count width 7 places `warningCount` at `QRect(174, 100, 7, 20)` and `warningIcon` at `QRect(154, 102, 16, 16)`; `clusterWidth == 66`.
- [ ] A warning without a control ends at the right margin: `warningCount.right() == itemRect.right() - 4`, `controlHit.isNull()`.
- [ ] Right-to-left output is the exact mirror image of left-to-right for the same inputs; no rect extends outside `itemRect` horizontally when `itemRect.width() >= clusterWidth`.
- [ ] The header includes no Gui or Widgets header.

**Complexity:** S

---

### Task 7.2: `PlotRowDelegate` - painting

**Purpose:** Paint refresh + count, or progress + cancel, and independently the warning badge + count, leaving plain rows to the base class (spec 9.2).

**Files to create:**
- `src/ui/docks/plotselection/PlotRowDelegate.h` / `PlotRowDelegate.cpp`.

**Files to modify:**
- `src/CMakeLists.txt` - `ui/docks/plotselection/PlotRowDelegate.h    ui/docks/plotselection/PlotRowDelegate.cpp` in `PROJECT_SOURCES`, same hunk as Task 7.1.

**Technical Approach:**
- Class shape:

  ```cpp
  class PlotRowDelegate : public QStyledItemDelegate {
      Q_OBJECT
  public:
      PlotRowDelegate(PlotRequests *requests, QAbstractItemView *view);

      void paint(QPainter *, const QStyleOptionViewItem &, const QModelIndex &) const override;
      bool editorEvent(QEvent *, QAbstractItemModel *, const QStyleOptionViewItem &, const QModelIndex &) override;   // Task 7.3
      bool helpEvent(QHelpEvent *, QAbstractItemView *, const QStyleOptionViewItem &, const QModelIndex &) override;  // Task 7.3

      /// What helpEvent() shows; empty for plain rows and categories.
      QString toolTipFor(const QModelIndex &index) const;
      /// Where the row's control is in viewport coordinates; null when the
      /// row shows none. For tests and for nothing else.
      QRect controlRect(const QModelIndex &index) const;
  private:
      PlotRowState    stateFor(const QModelIndex &index) const;          // default state without requests / id
      PlotRowGeometry geometryFor(const QStyleOptionViewItem &opt, const PlotRowState &state) const;
      QPointer<PlotRequests>       m_requests;
      QPointer<QAbstractItemView>  m_view;
      QPersistentModelIndex        m_pressedIndex;                       // Task 7.3
      PlotRowState::Control        m_pressedControl = PlotRowState::Control::None;
  };
  ```
- `paint()`: early return to the base class for plain rows (design summary). Otherwise: copy the option, `initStyleOption(&opt, index)`, compute metrics and `geometryFor()`, elide `opt.text` as specified, draw the item with `style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget)` where `style = opt.widget ? opt.widget->style() : QApplication::style()` (what the base class does); then `painter->save()`, `setClipRect(opt.rect)`, `setRenderHint(QPainter::Antialiasing)`, draw the glyphs and the two texts (`opt.font`, glyph colour, `Qt::AlignVCenter | Qt::AlignRight` for the label, `Qt::AlignVCenter | Qt::AlignLeft` for the failed count), `painter->restore()`.
- `geometryFor()` measures `QString::number(state.failedCount)` and the label with `opt.fontMetrics.horizontalAdvance()` and calls `layoutPlotRow()` with `opt.rect` and `opt.direction`.
- `controlRect(index)` builds an option from the view (`opt.initFrom(m_view)`, `opt.rect = m_view->visualRect(index)`, `initStyleOption`) and returns `geometryFor(...).controlHit`.
- Include `plotrequests.h`, `plotmodel.h`, `ui/docks/plotselection/PlotRowLayout.h`; no `AppContext.h`, no KDDockWidgets header, no `mainwindow.h`.
- The delegate draws no word of its own and contains no `tr()` string.

**Acceptance Criteria:**
- [ ] With every row plain (or `requests == nullptr`), an image of the view grabbed with this delegate equals, pixel for pixel, an image grabbed with a default `QStyledItemDelegate` (Task 7.7 `plainRowsAreIdenticalToBaseDelegate`).
- [ ] For a row with missing tracks, the grabbed image differs from the plain image inside `controlRect(index)` and is identical inside the check-indicator rect.
- [ ] A row whose name does not fit is elided on the right and the cluster is fully visible (view width 120 px in the test).
- [ ] `sizeHint()` is not overridden; row height is the same for plain and non-plain rows.
- [ ] `git grep -n "tr(\|QIcon\|QPixmap\|\.svg\|QTimer" src/ui/docks/plotselection/PlotRowDelegate.*` finds nothing.

**Complexity:** M

---

### Task 7.3: `PlotRowDelegate` - hit-testing, the check gesture, tooltip, repaint

**Purpose:** Forward exactly the two gestures and cancel to `PlotRequests`, show its tooltip, and repaint a row when its state changes (spec 9.2-9.4, overview Decision 9).

**Files to modify:**
- `src/ui/docks/plotselection/PlotRowDelegate.h` / `.cpp`.

**Technical Approach:**
- `editorEvent()` exactly as numbered under "Gestures and hit-testing". Mouse position: `static_cast<QMouseEvent *>(event)->position().toPoint()` (viewport coordinates, the coordinate system of `option.rect`).
- `helpEvent()` and `toolTipFor()` as under "Tooltip".
- Constructor: store both pointers; when both are non-null connect `rowStateChanged` to a private slot `onRowStateChanged(const QString &plotId)` that returns when the view or its model is gone, finds the index with `match()` as under "Repaint", and calls `m_view->update(index)` when it is valid.
- The class comment lists what is a gesture and what is not (the three programmatic paths with their file references), and states that the delegate never connects to `dataChanged`.

**Acceptance Criteria:**
- [ ] A left click on the check indicator of an unchecked explicit-backed plot with two missing visible tracks checks it and queues two jobs; Space on the current row does the same (Task 7.7 `checkBoxClickIsGesture`, `spaceKeyIsGesture`).
- [ ] A click or Space that **unchecks** calls nothing on `PlotRequests` that creates a job; `PlotModel::setPlotEnabled()`, `togglePlot()`, and `setData()` called from code while the view is shown create no job (`programmaticCheckStartsNothingWithViewAttached`).
- [ ] A click inside `controlRect()` of a row showing refresh queues the missing tracks' jobs, does not toggle the check box, and does not change the selection; on a row showing cancel it cancels and leaves the plot checked.
- [ ] Press inside the control and release outside it, press outside and release inside, a click on the label or the warning badge, and a right click inside the control do nothing.
- [ ] A double click on refresh creates the jobs once and cancels nothing.
- [ ] `toolTipFor()` equals `rowState(id).toolTip` for a non-plain row and is empty for plain rows and categories; `helpEvent()` returns `true` for the former.
- [ ] After a row's state changes, the viewport receives a paint event without any other stimulus (`rowStateChangeRepaintsRow`).
- [ ] `git grep -n "dataChanged\|setData\|setPlotEnabled\|togglePlot\|request(" src/ui/docks/plotselection/PlotRowDelegate.cpp` finds nothing.

**Complexity:** M

---

### Task 7.4: `AppContext` and the plot-selection dock

**Purpose:** Hand the application-wide objects to the dock features and install the delegate.

**Files to modify:**
- `src/ui/docks/AppContext.h` - forward-declare `class JobQueue;` and `class PlotRequests;`; add `JobQueue* jobQueue = nullptr;` and `PlotRequests* plotRequests = nullptr;` after `measureModel`. (`jobQueue` has no consumer yet; it is what a future jobs dock would view through `JobQueue::model()`, and adding it now keeps that dock a pure view.)
- `src/ui/docks/plotselection/PlotSelectionDockFeature.cpp` - include `PlotRowDelegate.h`; after `setEditTriggers(...)` (line 24): `m_treeView->setItemDelegate(new PlotRowDelegate(ctx.plotRequests, m_treeView));`.

**Technical Approach:**
- Nothing else in the feature changes: expansion persistence, the model hookup, and `NoEditTriggers` stay. The feature keeps no pointer to `PlotRequests`. A null `ctx.plotRequests` yields a delegate that behaves as the base class.
- `DockRegistry.cpp` and `DockFeature.h` are not edited.

**Acceptance Criteria:**
- [ ] The application builds; the plot list looks and behaves as before for all existing plots.
- [ ] `PlotSelectionDockFeature.h` is unchanged.

**Complexity:** S

---

### Task 7.5: `MainWindow` - create, own, and tear down the queue and the request component

**Purpose:** Put the queue into the application with a safe construction and destruction order and an orderly close (spec 8.3; acceptance 16 wiring half, 17 application half).

**Files to modify:**
- `src/mainwindow.h` - forward-declare `class JobQueue;` and `class PlotRequests;` next to the other forward declarations (lines 23-30); add `JobQueue *m_jobQueue = nullptr;` and `PlotRequests *m_plotRequests = nullptr;` under `// Models`.
- `src/mainwindow.cpp` - four small hunks: two includes (`jobqueue.h`, `plotrequests.h`); creation after line 196 and the two `ctx.` assignments after line 206; `closeEvent()`; `~MainWindow()`.

**Technical Approach:**
- Exactly as under "Application wiring and teardown". The comment at the creation site says why here (sessions populated, calculations registered, docks not yet created) and that nothing at startup is a gesture. The comment in `~MainWindow()` explains the child-deletion-order problem in two lines.
- `closeEvent()` becomes: `shutdown()` (with the wait cursor when `!m_jobQueue->isIdle()`), `model->flushDirtySessions()`, `saveDockLayout()`, base class.
- `MainWindow::togglePlot()`, `initializePlotsMenu()`, and `applyProfile()` are **not** edited; they must never call `PlotRequests`.
- No connection is made from `JobQueue` or `PlotRequests` to `MainWindow`.

**Acceptance Criteria:**
- [ ] `m_jobQueue->shutdown()` is the first statement of `closeEvent()` that has an effect; `~MainWindow()` deletes `m_plotRequests`, then `m_jobQueue`, before `delete ui`.
- [ ] `git grep -n "m_plotRequests\|plotRequests" src/mainwindow.cpp src/profilestatebridge.cpp src/plotviewsettingsmodel.cpp` shows only the creation, the `ctx.` assignment, and the delete; in particular nothing in `togglePlot`, the Plots menu, or the profile code.
- [ ] `git grep -n "QMessageBox\|QProgressDialog\|processEvents\|QEventLoop" src/` shows no occurrence that was not on `master` before this phase.
- [ ] Starting and quitting the application with an idle queue neither crashes nor hangs, in Debug and Release; quitting through File > Exit and through the window's close button both pass through `closeEvent()`.
- [ ] With Phase 9 present: see the deferred script; without it this criterion is complete with the previous bullet and `tst_jobqueue`'s shutdown tests.

**Complexity:** S

---

### Task 7.6: Silence the "no data" warning for uncomputed values; confirm the invalidation path

**Purpose:** Spec 9.6.

**Files to modify:**
- `src/ui/docks/plot/PlotWidget.cpp` - the block at lines 531-534; one include if needed (`engine/calculationengine.h` / `engine/blockerreport.h`; `dependencykey.h` is already reachable).

**Technical Approach:**
- Implement the rule of "Warning suppression rule" with a small file-static helper, for example `bool isMerelyUncomputed(const SessionData &, const QString &sensorId, const QString &measurementId)`, returning true for `Blocked` and `NotProduced`, with a comment citing spec 9.6 and saying why the engine is asked rather than the row state.
- The helper is called only when `yData.isEmpty()`. The `continue` is unconditional, as today.
- Do not add a rebuild guard, do not touch `onDependencyChanged()`, `schedulePlotRebuild()`, or the legend: the design summary's "Results appear through ordinary invalidation only" is a statement of fact to re-verify by reading the three cited places, not work to do. If Phase 5's `publishCalculationInvalidation()` as implemented does not emit `modelChanged`, stop and report it as a Phase 5 defect.

**Acceptance Criteria:**
- [ ] `git grep -n "No data available for plot" src/` still finds exactly one site, now behind the engine check.
- [ ] A checked ordinary plot for a recording that lacks the sensor still logs the warning (unchanged behaviour).
- [ ] The diff of `PlotWidget.cpp` contains no `m_rebuildingPlot`, no `QScopedValueRollback`, no `qScopeGuard`, and touches no function other than `updatePlot()` (plus the helper and an include).
- [ ] `src/idlescheduler.*`, `src/sessiondata.h`, `src/ui/docks/legend/*`, `src/plottool/*`, and `src/ui/docks/map/*` are untouched.

**Complexity:** S

---

### Task 7.7: Tests - geometry without Widgets, the delegate with an offscreen view

**Purpose:** Prove the view's only responsibilities - geometry, plain-row identity, hit-testing, gesture detection, tooltip, repaint - automatically, because the real fusion plots may not exist when this phase is implemented and manual verification of rows is therefore deferred.

**Files to create:**
- `tests/tst_plot_row_layout.cpp` - class `PlotRowLayoutTest`; `FLYSIGHT_TEST_MAIN`; includes `ui/docks/plotselection/PlotRowLayout.h` (reachable through `flysight_core`'s public include directory, `src/`). No Widgets.
- `tests/tst_plot_row_delegate.cpp` - class `PlotRowDelegateTest`; the first and only test that links Qt Widgets.

**Files to modify:**
- `tests/CMakeLists.txt` - after Phase 6's `tst_plot_requests` lines, one additive hunk:

  ```cmake
  # Plot-list rows: pure geometry (no widgets) ...
  flysight_add_test(tst_plot_row_layout SOURCES tst_plot_row_layout.cpp)

  # ... and the delegate in an offscreen QTreeView. The only test that links
  # Qt Widgets; it compiles the delegate from the application sources
  # (pattern: tst_python_bridge). QT_QPA_PLATFORM=offscreen is already set by
  # flysight_add_test().
  option(FLYSIGHT_BUILD_WIDGET_TESTS "Build and run the tests that need Qt Widgets" ON)
  if(FLYSIGHT_BUILD_WIDGET_TESTS)
    flysight_add_test(tst_plot_row_delegate
      SOURCES tst_plot_row_delegate.cpp
              "${FLYSIGHT_SRC_DIR}/ui/docks/plotselection/PlotRowDelegate.cpp"
              "${FLYSIGHT_SRC_DIR}/ui/docks/plotselection/PlotRowDelegate.h"
      LIBS Qt${QT_VERSION_MAJOR}::Widgets Threads::Threads)
    set_tests_properties(tst_plot_row_delegate PROPERTIES LABELS "core;widgets")
  endif()
  ```
- Root `CMakeLists.txt` - declare and forward the option exactly as Phase 1 (01 Task 1.1) does for `FLYSIGHT_BUILD_FUSION_TESTS` (overview, Integration Note 8): `option(FLYSIGHT_BUILD_WIDGET_TESTS "With FLYSIGHT_BUILD_TESTS: also build the tests that need Qt Widgets" ON)` next to `FLYSIGHT_BUILD_PYTHON_TESTS` / `FLYSIGHT_BUILD_FUSION_TESTS`, with its line in the option comment block and the summary, and `-DFLYSIGHT_BUILD_WIDGET_TESTS=${FLYSIGHT_BUILD_WIDGET_TESTS}` in `_APP_CMAKE_ARGS`, so that the value given to the superbuild reaches the `FlySightViewer` external project. One additive hunk per place; no reflow.

**Technical Approach:**
- **Why a Widgets test at all.** The harness has linked no Widgets so far, and geometry is factored out precisely so that most of the view is testable without them. But the one decision the view does own - "this check was made by direct interaction" (overview Decision 9, acceptance 16 wiring half) - lives in `editorEvent()` and depends on how `QAbstractItemView` routes events to a delegate. That cannot be reached by a pure function, and it cannot be verified by hand in this phase because no shipped plot is backed by an explicit calculation until Phase 9. The delegate was therefore designed to depend on `flysight_core` and Qt Widgets only, and one offscreen test drives it with `PlotFixture`'s synthetic plots. The option exists so that a platform where the offscreen plugin misbehaves in CI can switch the test off without touching anything else (the macOS and Linux runs are unverified until a push).
- `tst_plot_row_delegate.cpp` has its own `main()`, identical to `FLYSIGHT_TEST_MAIN` (`tests/support/testmain.h`) except that it constructs a `QApplication` and then calls `QApplication::setStyle(QStyleFactory::create("Fusion"))` as `src/main.cpp` line 28 does. Keep the order: deterministic hash seed, application object, `TestEnvironment`, test object.
- Fixture per test function, in this order (Phase 6 Task 6.7 plus the view): `JobWorld`, `PlotFixture`, `SessionModel`, `JobQueue`, `PlotModel` (`setPlots(PlotFixture::plots())`), `PlotRequests`, `QTreeView` (`setModel`, `setHeaderHidden(true)`, `setEditTriggers(NoEditTriggers)`, `resize(300, 300)`, `expandAll()`, `show()`, `QVERIFY(QTest::qWaitForWindowExposed(&view))`), `PlotRowDelegate(&requests, &view)` installed with `setItemDelegate`. Destroy in reverse. Sessions `s1`, `s2` with `G_IN = 4`, shown through `PlotFixture::show()`.
- Helpers in the test: `QModelIndex indexOf(plotId)` (the same `match()` call the delegate uses); `QPoint checkBoxCentre(index)` from `view.style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, &view).center()` with an option built as `controlRect()` builds it; clicks with `QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, point)`; press and release separately with `QTest::mousePress` / `mouseRelease`; keys with `view.setCurrentIndex(index); QTest::keyClick(&view, Qt::Key_Space)`.
- "Nothing started" is asserted as in Phase 6: an empty `QSignalSpy(&queue, &JobQueue::jobQueued)` and an unchanged `queue.model()->rowCount()`. Call `requests.flush()` before reading `rowState()` or grabbing an image. Jobs held in the `Gate` are released and `FlySightTest::waitIdle(queue)` is awaited before the fixture is destroyed. No sleeps.
- `tst_plot_row_layout` functions: `controlOnly`, `controlAndWarning`, `warningOnly`, `nothingShown`, `emptyLabelOmitsItsSpacing`, `rightToLeftIsMirrorImage`, `hitRectSpansRowHeightToRightEdge` - the literals of Task 7.1's criteria.
- `tst_plot_row_delegate` functions:
  - `plainRowsAreIdenticalToBaseDelegate` - nothing checked: `view.grab().toImage()` with the delegate equals the image with a plain `QStyledItemDelegate`; repeated with `Syn/plain` checked and with a delegate constructed with `nullptr` requests.
  - `missingRowPaintsControl`, `longNameIsElidedNotTheCluster` (Task 7.2).
  - `checkBoxClickIsGesture`, `spaceKeyIsGesture`, `uncheckIsNotAGesture`.
  - `programmaticCheckStartsNothingWithViewAttached` - `setPlotEnabled`, `togglePlot`, and `setData(index, Qt::Checked, Qt::CheckStateRole)` with the view shown and the event loop spun (`QTest::qWait(0)` twice, `flush()`): no job; the row shows refresh with `controlCount() == 2`. This is the wiring half of acceptance 16: the view adds no path from a model change to a request.
  - `startupStyleRestoreStartsNothingWithViewAttached` - a `PlotModel` with `QSettings` holding `state/plots/Syn/g = true`, `setPlots()` after the view and delegate exist: no job, refresh shown.
  - `refreshClickRequests`, `cancelClickCancelsAndLeavesChecked`, `controlClickDoesNotToggleOrSelect`.
  - `pressInsideReleaseOutsideDoesNothing`, `pressOutsideReleaseInsideDoesNothing`, `clickOnLabelOrBadgeDoesNothing` (badge: `Syn/ea` with `EA_IN = -1` after its job succeeded), `rightClickDoesNothing`.
  - `doubleClickOnRefreshRequestsOnceAndCancelsNothing` (`QTest::mouseDClick`).
  - `toolTipComesFromRowState` - `toolTipFor()` for a missing row, a failed row (contains the literal reason `"Explicit A: negative input"`), a plain row, a category; `helpEvent()` with a constructed `QHelpEvent` returns `true` only for the non-plain rows.
  - `rowStateChangeRepaintsRow` - an event filter on the viewport counts `QEvent::Paint`; after `PlotFixture::show()` of a further session and `flush()`, `QTRY_VERIFY(paints > before)`.
  - `survivesRequestsDestroyedFirst` - delete `PlotRequests`, then click, press Space, request a tooltip, grab: no crash, base behaviour.

**Acceptance Criteria:**
- [ ] Both executables pass in Debug and Release on Windows under `ctest -L core`, 20 consecutive runs each, well inside the 120 s timeout.
- [ ] `tst_plot_row_layout` links no Widgets. `tst_plot_row_delegate` is the only target under `tests/` that links `Qt::Widgets`, and `-DFLYSIGHT_BUILD_WIDGET_TESTS=OFF` configures and builds without it.
- [ ] The root `CMakeLists.txt` declares `FLYSIGHT_BUILD_WIDGET_TESTS` and forwards it through `_APP_CMAKE_ARGS` to the `FlySightViewer` external project, as it does `FLYSIGHT_BUILD_FUSION_TESTS`: configuring the superbuild with `-DFLYSIGHT_BUILD_WIDGET_TESTS=OFF` yields an application build without `tst_plot_row_delegate`.
- [ ] No other test target, and neither `flysight_core` nor `flysight_test_support`, gains a Widgets dependency.
- [ ] Every function named above exists; `cleanup()` verifies the global registry as Phase 6's test does.

**Complexity:** L

---

### Task 7.8: Documentation, manual script, and traceability

**Purpose:** Record the view contract, leave Phase 10 a ready manual script, and keep the acceptance map current - all append-only.

**Files to modify:**
- `docs/CALCULATIONS.md` - append to Phase 6's section 16 (the number is fixed at 16 for Phases 6 and 7 regardless of commit order; overview, Integration Note 2) a short subsection "The plot list view and application wiring" (own hunk): the delegate paints `PlotRowState` and decides nothing; what counts as the check gesture and the three programmatic paths that do not; no keyboard surface for refresh / cancel; creation order, `shutdown()` first in `closeEvent()`, explicit delete order in `~MainWindow()`; the warning rule of spec 9.6 and where it is applied; results appear through `modelChanged` / `dependencyChanged` only. User-facing documentation of the controls is Phase 10.
- `tests/README.md` - executable count plus two (relative to whatever count is found); two rows in the test table; one line for the option `FLYSIGHT_BUILD_WIDGET_TESTS` and the label `widgets` in section 3; one sentence in section 8 saying that a Widgets test writes its own `main()` with `QApplication`; and a new section **"Manual verification: plot-driven jobs"**, numbered (the next section number) and placed after the last numbered section and before Appendix A (overview, Integration Note 3), containing the deferred script below verbatim.
- `tests/acceptance_map.txt` - append comment-only lines to the `# SFJ` block (Phase 4's convention; the block header follows Phase 2's form, 02 Task 2.7, as Phase 4's document gives it). The `# SFJ n` comment lines are converted to audited `100 + n` lines by Phase 10 (overview, Integration Note 1):

  ```
  # SFJ 16 tst_plot_row_delegate programmaticCheckStartsNothingWithViewAttached
  # SFJ 16 tst_plot_row_delegate startupStyleRestoreStartsNothingWithViewAttached
  # SFJ 16 tst_plot_row_delegate checkBoxClickIsGesture
  # SFJ 16 manual  tests/README.md "Manual verification: plot-driven jobs" steps 1-2
  # SFJ 17 manual  tests/README.md "Manual verification: plot-driven jobs" steps 8-9
  # SFJ 19 manual  tests/README.md "Manual verification: plot-driven jobs" step 5
  ```

**Acceptance Criteria:**
- [ ] `audit_cleanup` passes (the new map lines are comments).
- [ ] The manual script in `tests/README.md` is the script of this document, step for step.
- [ ] Edits to the three shared files are append-only apart from the README count and table rows.

**Complexity:** S

## Testing Requirements

### Unit Tests
- New: `tst_plot_row_layout` (no Widgets), `tst_plot_row_delegate` (Widgets, offscreen) - Task 7.7.
- Unchanged and passing: everything else, in particular `tst_plot_requests`, `tst_jobqueue`, `tst_jobmodel`, `tst_session_model_engine`, `tst_workflow`, and `audit_cleanup`.

### Integration Tests
- `tst_plot_row_delegate` is the integration of view + delegate + `PlotRequests` + `PlotModel` + `JobQueue` + `SessionModel`, driven by synthesized mouse and key events.
- `MainWindow` cannot be constructed in the harness (KDDockWidgets, WebEngine, plugins). Its wiring is verified by the grep criteria of Task 7.5, by review of the two orderings (creation, teardown), and manually.
- The application half of acceptance 17 rests on `tst_jobqueue`'s shutdown tests (Phase 5: shutdown with queued and running jobs; queue-then-model and model-then-queue destruction) plus this phase's guarantee that `shutdown()` runs first in the close path and that the queue is destroyed before the session model.

### Manual Verification

**In this phase (always possible, with or without Phase 9):**
1. `cmake --build build --config Release`; `ctest --test-dir build -C Release -L core --output-on-failure`.
2. Start the application with the usual logbook. Every existing plot row looks exactly as before (compare with a `master` build side by side, light and dark theme, 100% and 150% scaling): no glyph, no count, no tooltip, same row height, same elision.
3. Check and uncheck plots by mouse, by Space, through the Plots menu, by shortcut, and by applying a profile: behaviour as before.
4. With a visible recording that lacks a sensor (for example no IMU) and an IMU plot checked, the "No data available for plot" warning still appears in the debug output.
5. Quit through File > Exit and through the close button, in Debug and Release: no crash, no hang, no debugger output about destroyed objects or running threads.

**Deferred script (needs Phase 9's fusion plots; run here if Phase 9 is already accepted, otherwise by Phase 10 - Task 7.8 copies it into `tests/README.md`).** Use at least three recordings with IMU data, one without, and one the model rejects if available.
1. *Startup (16).* Check "Sensor fusion > Roll" with three fusable tracks visible, let it compute, quit, restart. After restart the row is checked and shows the refresh control with the count 3; no job ever starts (no progress appears, CPU idle). The debug output contains no "No data available" line for the fusion plot.
2. *Profile and menu (16).* Uncheck Roll. Apply a profile that checks fusion plots; toggle a fusion plot through the Plots menu and its shortcut: rows show refresh with counts; nothing starts.
3. *Refresh (15).* Press Roll's refresh control: the row shows "0 of 3" and the cancel control; Pitch and Yaw, if checked, show the same progress. The tooltip lists the computing track with the solver's progress text and the queued tracks. As each fit publishes, its graph appears without any further action, the label advances ("1 of 3", "2 of 3"), and finally the row is plain. The legend and any fusion logbook column fill in at the same moments.
4. *Check gesture (15).* On a fresh set of tracks, uncheck and re-check a fusion plot by clicking its check box: jobs start. Do the same with Space.
5. *Interactive during a fit (19).* While a fit runs: pan and zoom the plot, switch tools, hide and show other tracks, edit a session's description, set a marker, open Preferences. Nothing blocks; no dialog appears.
6. *Cancel (15).* Press cancel on a row with one running and two queued jobs: at once the row shows refresh with 3, the plot stays checked, other fusion rows change identically; within one solver step the CPU goes idle. Press refresh again: it recomputes.
7. *Fourth track and failure (15).* Show a fourth fusable track afterwards: the row shows refresh with 1 and nothing starts; refresh computes it. Show the recording without IMU data: it never appears in any count or tooltip. For a rejected recording the row shows the warning badge with 1, the tooltip gives the reason, no refresh is offered for it, and no message box appears.
8. *Remove and unload (17).* With one fit running and one queued, delete the queued track's session, then the running one's: no crash, no hang, nothing published for them; the remaining rows' counts fall.
9. *Quit (17).* With one fit running and two queued, close the window: a wait cursor for at most one solver step, then the application exits; no crash dialog, the process is gone from the task manager, and on restart the logbook is intact. Repeat with File > Exit.

## Notes for Implementer

### Gotchas
- Task 7.1's literals use the **exclusive** right edge `itemRect.x() + itemRect.width()` for construction, while `QRect::right()` is inclusive (`x + width - 1`). Build with `x()` / `width()`, and mirror with the formula given.
- The event-routing facts this design relies on (the view calls `editorEvent()` for press, release, double-click, and Space despite `NoEditTriggers`; a press for which the delegate returns `true` does not select the row; no release is delivered after a double-click) are from the Qt 6 sources as remembered by the documenter. Task 7.7's tests pin each of them; if one fails, adapt the delegate so that the *behavioural* criteria of Task 7.3 hold and say so in the report.
- Detect the check gesture by comparing the model's check state before and after the base call, not by the base call's return value (it returns `true` for a press inside the indicator without toggling).
- `plotCheckedByUser()` must come after the base call. `PlotRequests` re-reads the model and answers 0 for an unchecked plot, so a wrong order fails safe - but it fails.
- `option.rect` and the mouse position are both in viewport coordinates. Do not use `QCursor::pos()`.
- `m_pressedIndex` must be a `QPersistentModelIndex`: `PlotModel::setPlots()` can reset the model between press and release (a plugin registering a plot); an invalidated persistent index simply never matches.
- `QToolTip` treats text as rich text when `Qt::mightBeRichText()` says so; that function inspects the first line only, and the first line of every `PlotRowState::toolTip` is one of Phase 6's fixed headings, so session names containing `<` or `&` are shown literally. Do not convert the text to HTML (the two-space indentation would be lost).
- Painting on top of `CE_ItemViewItem` instead of shrinking `opt.rect` keeps the selection and hover background across the full row; shrinking the rect would cut the highlight short of the cluster.
- `view->update(index)` on an index scrolled out of view or inside a collapsed category is harmless.
- `JobQueue::shutdown()` blocks the main thread until the compute function returns. That is the specified behaviour (Phase 5 "Why no timeout"); do not add a timeout, a `processEvents()` loop, or a progress dialog around it.
- After `shutdown()` every gesture answers 0 and rows may show refresh controls that do nothing; this is only observable if a close is vetoed after shutdown, which nothing does today. Keep `shutdown()` after any future veto decision.
- Explicitly deleting `m_plotRequests` and `m_jobQueue` in `~MainWindow()` removes them from the children list; there is no double delete. Set both pointers to `nullptr` afterwards.
- `blockers()` asserts that no evaluation is open. `updatePlot()` runs from the event loop (timer, model signals), never from inside a calculation, so the assertion holds; do not call the helper from anywhere else.
- `src/CMakeLists.txt`: Phase 6 removed the `plotmodel` line from `PROJECT_SOURCES`; line numbers quoted here are `master`'s and shift by one or two.
- The test's `view.grab()` comparison needs both images taken at the same size, expansion state, selection, and focus state; grab twice from the same view, swapping only the delegate, and call `QApplication::processEvents()` after the swap.

### Decisions Made
- **`PlotRowDelegate` in `src/ui/docks/plotselection/`, depending on core + Widgets only**, so that an offscreen test can compile it without the application (rationale in Task 7.7).
- **Geometry is a header-only pure function** (`PlotRowLayout.h`) tested without Widgets; **interaction is tested with one Widgets-linked offscreen test**, gated by `FLYSIGHT_BUILD_WIDGET_TESTS` (default ON). Manual-only verification was rejected because the real plots may not exist when this phase lands and gesture detection is the one decision the view owns.
- **Painted glyphs, not bundled SVG/PNG icons** (no Qt Svg in the application; theme and DPI for free).
- **Control icon right-most, warning group to its left; refresh and cancel share one slot.**
- **The painted progress is the text "k of n"; no spinner, no animation timer, no painted job progress text** (tooltip only).
- **The name is elided, the cluster never; `sizeHint()` is not overridden.**
- **The control's hit rectangle is the icon column over the full row height to the right edge**; the label and the badge are inert.
- **Press and release must both be inside the control, on the same index, with the control unchanged in between.**
- **Space on the row is the check gesture; there is no keyboard, menu, or context-menu surface for refresh or cancel** (spec 9.3 "exactly two gestures", spec 12).
- **The delegate calls `PlotRequests` directly** (no intermediate signals) and owns the repaint connection; the dock feature only installs it.
- **Row lookup by `QAbstractItemModel::match()`**; `plotmodel.*` stays unedited.
- **Tooltip through `helpEvent()`** with Phase 6's ready-made text; `PlotModel` knows nothing of `PlotRequests`.
- **`JobQueue` then `PlotRequests` are created in the `MainWindow` constructor body before the docks; `shutdown()` is first in `closeEvent()`; both are deleted explicitly, requests first, in `~MainWindow()`.** A wait cursor brackets a non-idle shutdown.
- **`AppContext` gains both pointers** although only `plotRequests` has a consumer now.
- **One warning site exists and one is changed** (`PlotWidget.cpp` line 532), asking `blockers().state`; suppressed for `Blocked` and `NotProduced`.
- **No "show when done" mechanism**: publication's `modelChanged` already rebuilds the plot and the legend (verified); a gap there would be a Phase 5 fixup.
- **Acceptance map lines are comments**, manual items included, following Phases 4-6; Phase 10 reconciles the scheme.

### Open Questions
- None blocking. Whether the offscreen Widgets test runs cleanly on the macOS and Linux CI images cannot be known before a push; the option `FLYSIGHT_BUILD_WIDGET_TESTS` is the escape hatch, and turning it off in CI would be a separate, unverified CI edit decided by Michael.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria
2. All tests pass (`ctest -L core`, plus `audit_cleanup`), in Debug and Release on Windows, including 20-run loops of the two new tests
3. Code follows patterns established in reference files (`PlotSelectionDockFeature.cpp` and `LegendPresenter.cpp` for naming and comment density; `tests/CMakeLists.txt`'s `tst_python_bridge` block for a test that compiles application sources; `tests/README.md` section 8)
4. No TODOs or placeholder code remains
5. The "In this phase" manual steps 1-5 were performed and reported; the deferred script is in `tests/README.md` and was either run (Phase 9 present) or explicitly handed to Phase 10 in the report
6. Nothing listed under "What must not appear" exists; `src/plotmodel.*`, `src/plotrequests.*`, `src/jobqueue.*`, `src/jobmodel.*`, `src/engine/*`, `src/sessionmodel.*`, `src/idlescheduler.*`, `src/sessiondata.h`, `src/profilestatebridge.cpp`, and `src/plotviewsettingsmodel.cpp` are untouched
7. `flysight_core` and `flysight_test_support` still link Qt Core + Gui (+ Test) only; the delegate contains no decision about state, counts, or what to request (overview Decision 8)
