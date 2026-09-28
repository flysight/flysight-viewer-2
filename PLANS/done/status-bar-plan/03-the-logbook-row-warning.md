# Phase 3: The logbook row warning; the per-source indicators go

## Purpose

Spec §8 and the view half of §9. A recording with a current failure shows
one warning glyph on its logbook row, whose hover is that recording's
failures. The per-source presentation goes from every view: the plot rows'
and column headers' glyph, hover and reserved room, the working-indicator
clock, and the shared glyph painting with its hover helper.

It is one phase because the row glyph is the last new home of a fact the
header badge showed (failures reached the status bar in phase 2), so the
badge can go in the same change without leaving a failure shown nowhere; and
because the header, the plot rows and the clock share one plumbing
(`DemandIndicator*`), which goes whole. After this phase no product code reads
the per-source surface, which phase 4 then removes from the demand layer.

## Dependencies

- **Depends on phase 2**, and through it on phase 1. It may assume, as those
  documents state them:
  - `CalculationDemand::sessionFailures(const QString &sessionId) const`,
    `failuresChanged()`, `pendingCellsChanged(const QString &columnId)`;
    `SessionFailures::text()` and `calculations`; the pending signal fires for
    exactly the columns whose pending set changed, and every value is stored
    before any signal;
  - `StatusBarFeature` in `src/ui/statusbar/`, drawing the warning with the
    style's `QStyle::SP_MessageBoxWarning` (overview decision 1);
  - `LogbookView` without a progress line, cancel button, scheduler slot or
    `minimumSizeHint`; `LogbookDockFeature` without the scheduler; the audit
    rules phase 2 changed or added; `tst_status_bar` and its functions
    (phase 2's suggested names are used below; use the names the tree holds).
- **Blocks phase 4**, which removes `plotState`, `columnState`,
  `workingPlotIds`, `workingColumnIds` and their signals once nothing in the
  product reads them.
- **Code at the start.** `LogbookView(SessionModel *, CalculationDemand *,
  WorkingAnimation *clock, QWidget *)`; `setupView()` installs
  `new LogbookHeaderView(model, demand, clock, treeView)` and the cell
  delegate; `PlotSelectionDockFeature` installs
  `new PlotRowDelegate(ctx.calculationDemand, ctx.workingClock, m_treeView)`;
  `MainWindow` creates `m_workingClock = new WorkingAnimation(this);` and
  calls `followDemand(m_workingClock, m_calculationDemand);`.

## What changes

### Settled by reading the code (overview decision 8)

- **`LogbookHeaderView` is deleted.** Without the demand layer's state every
  section is `isPlain()` and `paintSection()` calls the base class; the
  constructor sets only what `QTreeView` gives its own header (movable
  sections, last section stretched, `AlignLeft | AlignVCenter`); the
  per-line elision, `sectionSizeFromContents()` reserve and `viewportEvent()`
  hover exist only for the glyph. The tree keeps its own `QHeaderView`;
  `setupView()`'s fixed two-line header height stays.
- **`PlotRowDelegate` and `PlotRowLayout.h` are deleted.** A plain row is
  `QStyledItemDelegate::paint()` and the base `helpEvent()`; the delegate
  handles no event of its own. The plot list keeps the tree's default
  delegate.
- **`DemandIndicator.{h,cpp}` and `DemandIndicatorView.{h,cpp}` are deleted**:
  after the above, nothing uses `WorkingAnimation`, the two glyphs,
  `glyphMetrics`, `glyphColor`, `drawDemandGlyph` or `showIndicatorToolTip`,
  and the one remaining use of `repaintWhenDemandDestroyed` (the cell
  delegate) becomes the delegate's own connection. One drawing of the
  warning glyph remains: the style's standard icon, in the status bar and
  the logbook row.

### The row glyph (`src/ui/docks/logbook/LogbookCellDelegate.{h,cpp}`)

The delegate presents two things from the demand layer: the pending cell,
unchanged, and the row warning.

- **Where.** The glyph belongs to the row's first *visual* cell: the cell of
  the first section in visual order that is not hidden
  (`header()->logicalIndex(v)` for v from 0, skipping `isSectionHidden`),
  computed at paint time from the tree's header, never cached. The logbook
  has movable and hidable sections and no fixed column; because the delegate
  asks the header each time it paints, the glyph follows a move, a hide and a
  rebuild of the columns without a signal of its own (the tree repaints on
  each).
- **When.** `m_demand` lives, the index belongs to the model, and
  `sessionFailures(rowAt(row).sessionId).calculations` is not empty. A row
  that is not loaded needs no load: its failures come from the record set
  (phase 1, rule 4). A cell without the glyph and without pending text is
  painted by the unmodified base class, as today.
- **How.** The glyph is the style's
  `standardIcon(QStyle::SP_MessageBoxWarning, ...)`, placed as the item's
  decoration (`HasDecoration`) on a copy of the option in `paint()` only, so
  the style lays it out at the leading edge of the text rectangle (after the
  cell's check box when the first visual cell is logical column 0) and elides
  the text into what is left. The glyph thus takes room in the first cell's
  text rectangle only when it is shown. Its side is at most one text line
  (the option's font height), so it fits the row as painted.
- **Row height never changes.** `sizeHint()` stays the base class's and never
  sees the glyph; the tree keeps `setUniformRowHeights(true)`.
- **Cells over the failed calculation are blank**: a Failed track is not
  pending, so such a cell is the base class's empty cell, as today. Nothing
  is added for it.
- **Pending cells unchanged**: text, colour, alignment and tooltip. A first
  visual cell may carry both the glyph and "…".
- **Hover.** In `helpEvent()`, a tooltip event inside the glyph's rect (the
  decoration rect the style computes for that option, clipped to the cell)
  shows `sessionFailures(id).text()` exactly, over that rect, and is
  accepted. Anywhere else in the cell the cell's own tooltip applies: the
  pending tooltip for a pending cell, else the base class's (the model's
  tooltip, none today). So a pending first cell shows the pending tooltip
  beside the glyph and the failures over it.
- **No gesture.** No event handler is added; a click on the glyph is a click
  on the cell (selection, check box) and starts or cancels nothing.
- **Repaint.**
  - `pendingCellsChanged(columnId)` repaints the visible part of that column,
    as `onColumnStateChanged` does today (it is that slot, moved).
  - `failuresChanged()` repaints the visible part of the first visual column
    (one viewport update of that strip; no model signal, no reset).
  - The delegate connects the demand layer's `&QObject::destroyed` to a
    viewport update, with the viewport as context: from then on it paints
    exactly as `QStyledItemDelegate`.
  - It no longer connects `columnStateChanged`, and includes nothing of
    `ui/docks/DemandIndicator*`.
- **Test seam.** `QRect warningRect(const QModelIndex &index) const`: the
  glyph's rect in viewport coordinates when `index` is its row's first visual
  cell and the row has a current failure, else a null rect; `const`, and for
  tests (product code may share the computation with `paint()` and
  `helpEvent()`). The class comment states the contract above and stops
  naming the header's indicator or hover.

### The logbook view and its dock feature

- `LogbookView`'s constructor becomes
  `LogbookView(SessionModel *model, CalculationDemand *demand, QWidget *parent = nullptr);`
  and `setupView(CalculationDemand *demand)`: no clock, no `LogbookHeaderView`
  include or installation. The class comment names the row warning and the
  pending cells.
- `LogbookDockFeature.cpp`: `new LogbookView(ctx.sessionModel, ctx.calculationDemand, m_dock)`,
  and its comment says what the demand layer supplies (the row warning and
  the pending cells).

### The plot list

`PlotSelectionDockFeature.cpp` loses `#include "PlotRowDelegate.h"`, the
`setItemDelegate(new PlotRowDelegate(...))` line and its comment. The plot
list presents nothing of the demand layer (spec §12).

### The clock and the context

- `AppContext.h`: remove `workingClock` and `class WorkingAnimation;`; the
  `calculationDemand` comment names its readers: the logbook's cells and rows
  and the status bar (may be null).
- `mainwindow.{h,cpp}`: remove the two `DemandIndicator*` includes,
  `m_workingClock`, its creation, `followDemand(...)`, `ctx.workingClock`,
  and its deletion in `~MainWindow`. Rewrite the creation comment and the
  destructor comment: the demand layer, then the executor; the logbook's cell
  delegate and the status bar hold the demand layer weakly.
- `src/calculationdemand.h`: comments only. The head paragraph, the
  PRESENTATION paragraph, the `workingPlotIds()` comment (which names
  `followDemand()` and `DemandIndicatorView.h`) and `isMerelyUncomputed()`'s
  parenthetical ("the row shows it working", "the warning badge") are made
  true: the logbook's cells read `isCellPending()` and `sessionFailures()`,
  the status bar reads `progress()` and `failures()`, and no view reads the
  per-source values. No declaration changes; the surface is phase 4's.

### Build

- `src/CMakeLists.txt`: remove `ui/docks/DemandIndicator.*`,
  `ui/docks/DemandIndicatorView.*`, `ui/docks/plotselection/PlotRowLayout.h`,
  `ui/docks/plotselection/PlotRowDelegate.*` and
  `ui/docks/logbook/LogbookHeaderView.*`.
- `tests/CMakeLists.txt`: remove the `tst_plot_row_layout` registration and
  its comment and the `tst_plot_row_delegate` block. `tst_logbook_indicators`
  compiles `LogbookView.*` and `LogbookCellDelegate.*` only. Rewrite the head
  comment and the widget block's comment: the tests that link Qt Widgets are
  `tst_logbook_indicators` and `tst_status_bar` (phase 2), and nothing
  compiles a shared indicator. `tst_status_bar` must reference nothing
  removed here; if its fixture made a clock, the clock goes.

### Must not change

The demand layer's code and every signal and query of it (phase 4); the
pending cell's look, tooltip and repaint scope; the status bar; the tree's
selection, editing, sorting, hover filter and context menu; the header's
fixed two-line height.

## Interfaces

**Consumed** from phase 1: `sessionFailures()`, `SessionFailures::text()`,
`SessionFailures::calculations`, `failuresChanged()`,
`pendingCellsChanged(const QString &)`, `isCellPending(int, int)`. From
phase 2: the audit rules as it leaves them; `tst_status_bar`'s functions (cited
in the map below).

**Provided** (the overview's "Phase 3 provides", as a contract):

- `LogbookCellDelegate` paints the row glyph from `sessionFailures()` and
  repaints on `pendingCellsChanged` and `failuresChanged`; it no longer
  connects `columnStateChanged`.
- No product code calls `plotState`, `columnState`, `workingPlotIds` or
  `workingColumnIds`, or connects `plotStateChanged`, `columnStateChanged` or
  `statesChanged`. `AppContext` has no `workingClock`. `WorkingAnimation`,
  `followDemand`, `DemandIndicator.*` and `DemandIndicatorView.*` do not
  exist.
- Also: `LogbookHeaderView`, `PlotRowDelegate`, `PlotRowLayout.h`,
  `tst_plot_row_delegate` and `tst_plot_row_layout` do not exist;
  `LogbookView(SessionModel *, CalculationDemand *, QWidget * = nullptr)`;
  the seam `LogbookCellDelegate::warningRect()`; the audit rule "the views
  read no per-source state" over `src/ui` and `src/mainwindow.*`, which phase
  4 widens to the whole tree.

## Acceptance criteria

1. **One glyph at the first visual cell** (§8). A row whose session has a
   current failure shows exactly one glyph, the style's
   `SP_MessageBoxWarning`, inside its first visual cell, at the leading edge
   of the text rectangle; no other cell of the row shows one. This holds for
   a loaded row and for a row that is not loaded, which stays unloaded.
2. **It follows the column order** (§8). Moving another section to the front,
   hiding the first section, sorting and rebuilding the columns put the glyph
   in the new first visual cell, and only there.
3. **Hover** (§8). A tooltip over the glyph shows exactly
   `sessionFailures(id).text()`, naming each failed calculation with its
   reason and "(tried again at the next start)" where it applies. Elsewhere
   in the cell the cell's own tooltip shows (the pending tooltip, or none).
4. **Blank and pending cells** (§8). Cells over the failed calculation paint
   exactly as the base delegate's; pending cells are unchanged.
5. **No room without a failure; no height change** (§8). A row without a
   failure paints exactly as the base delegate; row heights and the cells'
   `sizeHint()` equal the reference tree's with and without failures.
6. **It follows the failures** (§7, §8). The glyph appears when a failure
   becomes current and goes when it clears (a successful retry after an input
   change; disabling the last source). A `failuresChanged` repaints only the
   first visual column; `pendingCellsChanged(id)` only that column; no model
   signal is emitted.
7. **No gesture** (§8, §11). A click on the glyph selects as a click
   elsewhere in the cell does, and no job is offered or cancelled.
8. **Headers and plot rows show nothing** (§9, §11). The logbook's header is
   the tree's own `QHeaderView`; while a requested column works and after it
   has failed, the header paints as the reference tree's and reserves no room
   (`sectionSizeHint` equal). The plot list has no delegate of its own.
9. **No clock** (§9). While a column works, neither the header nor the cells
   repaint by themselves; no `WorkingAnimation` exists in `src` or `tests`.
10. **No per-source reads; no scheduler** (§9, §12, §13). The audit rules
    below hold; `LogbookView` has no `QProgressBar` or `QToolButton` child.
11. **Demand layer destroyed first** (§12). The cells then paint as the base
    delegate, no glyph or failure tooltip remains, and sorting works.
12. **Green.** The suite and `audit_cleanup` pass in `build-agent/`, Release,
    sequentially.

## Tests

### `tst_logbook_indicators` (not renamed)

It still proves the logbook's indicators, now the row warning and the
pending cell; a rename would re-point about forty map lines for nothing.
Remove the plot list (`makePlotList`, `plotIndex`), the clock (`m_clock`,
`followDemand`, `animation()->setFrozen`), the `LogbookHeaderView` cast
(`header()` is `tree()->header()`), the includes of removed files and the
amber helper. Rewrite the head comment. Names below are suggestions.

- **Removed**: `workingColumnShowsIndicatorRightOfText`,
  `indicatorAnimatesOnlyWhileWorking`, `plotRowsAndHeaderTurnOnOneClock`,
  `badgeReplacesIndicatorWhenFinished`, `headerToolTipFollowsDemandState`,
  `indicatorClearsSortArrowAndNarrowSections` (§9 removes their subjects).
- **Kept, amended**:
  - `plainHeaderAndCellsAreIdenticalToBase`: with no failure, a working and a
    finished requested column; header identical to the reference, cells to
    the base delegate outside pending cells (criteria 5, 8).
  - `failedWriteIsListedInTheHover`: the row's hover of s1 contains "Gated:
    Couldn't write file" and the retry suffix; the plot-row half goes
    (criterion 3).
  - `survivesDemandDestroyedFirst`: with a glyph and pending cells shown
    (criterion 11).
  - `pendingCellsAreDistinctFromUnavailable`,
    `pendingCellBecomesValueWhenRecordIsWritten`,
    `sortingTreatsPendingAsUnavailable`,
    `unreadableRecordPendingIsNotDemandPending`: setup only (no clock).
- **Renamed and rewritten**:
  - `failedLoadSessionShowsBadgeNotPending` →
    `failedLoadSessionShowsRowWarningNotPending`: s2's row has the glyph,
    hover "The session file could not be loaded", its G_OUT cell not pending.
  - `clickOnIndicatorIsAClickOnTheSection` →
    `clickOnRowWarningIsAClickOnTheCell` (criterion 7), under `Quiet`.
  - `indicatorFollowsColumnWhenMovedHiddenOrReordered` →
    `rowWarningFollowsTheFirstVisualColumn` (criterion 2).
  - `columnStateChangeRepaintsOnlyThatColumn` →
    `pendingCellsChangeRepaintsOnlyThatColumn`: emitting
    `pendingCellsChanged(G_OUT)` repaints only that column, emitting
    `failuresChanged()` only the first visual column; no model signal
    (criterion 6). That nothing connects `columnStateChanged` is the audit's.
- **New**:
  - `rowWarningAtLeftOfFirstCell` (criteria 1, 4, 5): `makeBadgedColumn()`
    loaded, then with `LogbookCacheSize` 0 and stubs (s1 stays unloaded);
    glyph in s1's first visual cell only, inside the cell, left of where the
    text starts, pixels differ from the base delegate there; s1's EA1 cell
    and the other rows equal the base delegate; row heights equal.
  - `rowWarningHoverIsTheSessionsFailures` (criterion 3): s1 failing two
    calculations (EA1 and T_OUT); the hover over `warningRect` equals
    `sessionFailures("s1").text()`; with a pending first cell the rest of the
    cell shows `pendingToolTip()`; a row without failure accepts nothing.
  - `rowWarningFollowsFailures` (criterion 6): appears when the job fails,
    goes after an input change whose retry succeeds, and goes when the column
    is disabled.
  - `headerIsPlainAndNothingAnimates` (criteria 8, 9, 10): header's
    `metaObject()` is `QHeaderView`'s; grabs and `sectionSizeHint` equal the
    reference while working and after a failure; with a column working,
    `PaintCounter`s on the header and the tree viewport count no paint over
    400 ms; the view has no `QProgressBar` or `QToolButton`.

### Removed executables

`tst_plot_row_delegate` and `tst_plot_row_layout`, with their sources and
registrations. Their check-through-the-model tests are covered without the
view by `tst_calculation_demand::programmaticCheckCreatesDemand`, which
writes `setData(CheckStateRole)`, what the check box and Space write.

### Audit (`tests/audit/cleanup_audit.cmake`)

Changed:

- `widget-free-core`: drop `src/ui/docks/plotselection/PlotRowLayout.h` and
  `"src/ui/docks/DemandIndicator.*"` from the pathspec.
- "only the application and its views know the demand layer": the allowed
  regex loses `DemandIndicatorView`, `PlotRowDelegate`,
  `PlotSelectionDockFeature\\.cpp` and `LogbookHeaderView`; it keeps the
  demand files, `mainwindow`, `AppContext.h`, `LogbookView`,
  `LogbookCellDelegate`, `LogbookDockFeature.cpp`, `PlotWidget.cpp` and
  `StatusBarFeature`. Update the Allow comment.
- "nothing below the demand layer includes it": drop the two dead pathspecs
  and the comment's "shared glyphs and the row layout".
- "the demand views handle no event of their own": pathspec
  `LogbookCellDelegate.*` and `StatusBarFeature.*` only.
- "the views learn of the demand layer's end in one place" becomes "each
  view that holds the demand layer learns of its end itself":
  `&QObject::destroyed` only in `LogbookCellDelegate.cpp` and
  `StatusBarFeature.cpp`, in `src/ui`.
- "the views keep no label, no cluster and no clock logic of their own"
  becomes "no view keeps a label, a cluster or a clock": pattern
  `progressLabel|clusterRect|syncAnimation|WorkingAnimation|followDemand|workingClock|frameAdvanced`,
  `src tests ":!tests/README.md"`. It replaces the three clock rules.

Removed: the two "one working-indicator clock" rules, "the clock follows the
demand layer in one place", "the plot rows and the headers share the glyph
plumbing" and "the plot row shows one glyph" (subjects gone; their names are
kept out by the rules below). Update the file's head comment ("one
working-indicator clock turns every view").

Added (plant a hit once to prove each):

- `expect_none("the views read no per-source state", "[.>](plotState|columnState|workingPlotIds|workingColumnIds)\\(|::(plotStateChanged|columnStateChanged|statesChanged)", src/ui "src/mainwindow.*")`;
- `expect_none("the per-source indicators and their plumbing stay gone", "DemandIndicator|drawWarningGlyph|drawWorkingGlyph|drawDemandGlyph|[Gg]lyphMetrics|glyphColor|showIndicatorToolTip|repaintWhenDemandDestroyed|PlotRowDelegate|PlotRowLayout|layoutPlotRow|LogbookHeaderView|indicatorRect|toolTipForSection", src tests ":!tests/README.md")`;
- `expect_only("one drawing of the warning glyph: the style's", "SP_MessageBoxWarning", "^src/ui/statusbar/StatusBarFeature\\.cpp$|^src/ui/docks/logbook/LogbookCellDelegate\\.cpp$", src)`;
- `expect_none("the plot list presents nothing of the demand layer", "[Cc]alculation[Dd]emand|demandstate|DemandState|setItemDelegate", src/ui/docks/plotselection)`.

`docs/`, the root `README.md` (its source tree names `DemandIndicator.*`,
`PlotRow*`, `LogbookHeaderView.*`) and `tests/README.md` beyond the rows
below are not searched by these rules; phase 5 rewrites them and may extend
"the documents describe the refined demand layer" with these names.

### Acceptance map (`tests/acceptance_map.txt`)

Every line names a function the tree has. Lines of a removed test whose item
keeps an equal line are removed; others are re-pointed. The map as phase 2
leaves it:

| Lines today | After this phase |
|---|---|
| 116, 514, 556 `tst_plot_row_delegate` (all), 518 `uncheckByClickDropsWaitingWork` | removed (the items keep their `tst_calculation_demand` lines) |
| 517 `checkBoxClickChecksThroughTheModel`, `spaceKeyChecksThroughTheModel` | one line `517 tst_calculation_demand programmaticCheckCreatesDemand` |
| 526, 537, 557, 633 `failedLoadSessionShowsBadgeNotPending` | `failedLoadSessionShowsRowWarningNotPending` |
| 527, 534, 557, 649 `clickOnIndicatorIsAClickOnTheSection` | `clickOnRowWarningIsAClickOnTheCell` |
| 527, 534, 543, 649, 650 `tst_plot_row_delegate` (all) | removed |
| 535 every `tst_plot_row_*` line and `workingColumnShowsIndicatorRightOfText`, `indicatorAnimatesOnlyWhileWorking`, `indicatorFollowsColumnWhenMovedHiddenOrReordered`, `indicatorClearsSortArrowAndNarrowSections`, `plotRowsAndHeaderTurnOnOneClock` | `535 tst_logbook_indicators headerIsPlainAndNothingAnimates` and `535 audit demand` |
| 536 plot-row lines; `headerToolTipFollowsDemandState` | `536 tst_logbook_indicators rowWarningHoverIsTheSessionsFailures`, `536 tst_status_bar taskShownOverComputationsAndHoverListsBoth` |
| 537 `tst_plot_row_delegate badgeReplacesIndicatorOnceFinished`; `badgeReplacesIndicatorWhenFinished` | `537 tst_logbook_indicators rowWarningAtLeftOfFirstCell`, `537 tst_status_bar warningBesideComputationsThenAlone` |
| 538 `columnStateChangeRepaintsOnlyThatColumn` | `pendingCellsChangeRepaintsOnlyThatColumn` |
| 557 plot-row lines | removed; `workingColumnShowsIndicatorRightOfText` → `headerIsPlainAndNothingAnimates`, `badgeReplacesIndicatorWhenFinished` → `rowWarningAtLeftOfFirstCell`, `headerToolTipFollowsDemandState` → `rowWarningHoverIsTheSessionsFailures` |
| 562 `tst_plot_row_layout indicatorOnly` | removed |
| 644 `tst_plot_row_*` lines; `workingColumnShowsIndicatorRightOfText`; `badgeReplacesIndicatorWhenFinished` | removed; `headerIsPlainAndNothingAnimates`; `rowWarningAtLeftOfFirstCell` |
| 645 plot-row lines; `headerToolTipFollowsDemandState` | removed; `645 tst_status_bar taskShownOverComputationsAndHoverListsBoth` |
| 646 `plotRowsAndHeaderTurnOnOneClock`; `indicatorAnimatesOnlyWhileWorking`; plot-row lines | `headerIsPlainAndNothingAnimates`; removed; removed |
| 650 `indicatorClearsSortArrowAndNarrowSections` | removed |
| 660 plot-row and layout lines; `plotRowsAndHeaderTurnOnOneClock` | removed; `headerIsPlainAndNothingAnimates` |

All other lines stay. No item loses its last test or audit line (checked:
116, 514, 517, 518, 527, 534, 543, 556, 562, 644, 646, 649, 650, 660 keep one).
Restating the items "(as amended)" and the new items from 701 are phase 5's.

### README (`tests/README.md`)

- Remove the `tst_plot_row_layout` and `tst_plot_row_delegate` rows; rewrite
  the `tst_logbook_indicators` row for the functions above (row warning,
  hover, first visual column, repaint scope, plain header, nothing animates,
  pending cells, destroyed demand layer).
- Section 1 counts: 48 executables; `ctest -N` 49 entries, or 56 with the
  exact runs. The sentences naming the Widgets tests (sections 1 and 3, the
  `FLYSIGHT_BUILD_WIDGET_TESTS` row, section 8, the `tst_status_bar` row) name
  `tst_logbook_indicators` and `tst_status_bar`.
- Sections 9, 10, 12 and the appendices are phase 5's.

## Decisions

- **The glyph is the item's decoration**, set in `paint()` only: the style
  places it, reserves its room in the text rectangle only when shown, handles
  selection and direction, and `sizeHint()` never sees it, so the row height
  cannot change.
- **The first visual cell is asked of the header at paint time**, so no
  signal or cache is needed to follow a move or a hide.
- **Hover is the glyph's rect only**, so the first cell's own tooltip (the
  pending one) keeps working beside it.
- **`failuresChanged` repaints the first visual column**, not the rows that
  changed: the signal carries no id, and keeping the previous set to diff
  would be state for no visible gain.
- **The cell delegate watches the demand layer's end itself**: it is the one
  logbook view left that holds it, and the helper had one user.
- **The plot list's check tests go with their executable**; the setData path
  they exercised is `tst_calculation_demand`'s.

Ready with caveats: the `tst_status_bar` function names cited in the map are
phase 2's suggestions and must be replaced by the names the tree holds; the
root `README.md` and `docs/` name removed files until phase 5.
