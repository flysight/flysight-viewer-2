# Phase 3: Working indicator, hover detail and pending cells

## Overview

This phase presents what the demand layer (`CalculationDemand`) already knows after Phases 1 and 2. It adds:

- an animated **working indicator** at the right of a plot row's name and of a logbook column header's text while any of its demand is waiting or running;
- the **warning badge**, which replaces the indicator once work is finished and some sessions could not be computed;
- **hover detail** on plot rows and column headers, built from the same `DemandState::toolTip` so both say the same thing;
- **pending logbook cells**, painted distinctly from "unavailable" and from the row's unreadable-record pending state.

All of it is view-side. `SessionModel::data`, `cachedValues`, `pendingColumns`, `index.json` and sorting never see "pending". The demand layer stays widget-free and gains only read-only presentation queries.

## Dependencies

- **Depends on:** Phase 1 (Executor and plot demand), Phase 2 (Column demand).
- **Blocks:** Phase 4 (Documentation, acceptance map and audit).
- **Assumptions:** everything `01-executor-and-plot-demand.md` and `02-column-demand.md` describe exists. In particular:
  - **`CalculationDemand`** (`src/calculationdemand.{h,cpp}`) has:
    - the types `DemandCondition`, `DemandTrack` and `DemandState` (`isWorking()`, `showsWarning()`, `isPlain()`, `toolTip`, `progressLabel`, the counts, and the `running` / `waiting` / `failed` lists; a column state leaves `waiting` empty);
    - `plotState(plotId)`, `columnState(columnId)`, static `columnId(const LogbookColumn &)`, `isCellPending(sessionId, columnId)` and `isCellPending(int row, int column)`;
    - static `buildToolTip(const DemandState &)`;
    - the signals `plotStateChanged(plotId)`, `columnStateChanged(columnId)` (per column, before `statesChanged()`) and `statesChanged()` (once per pass that changed a state);
    - the test seams `flush`, `endInputSettleWaits`, `heldSessionIds`, `hasFillWork` and `runLoadStep`.
  - **`AppContext::calculationDemand`** (`src/ui/docks/AppContext.h`) is set by `MainWindow`, which deletes the demand layer before the executor and before the docks.
  - **`PlotRowDelegate`**, as Phase 1 leaves it:
    - constructed as `PlotRowDelegate(CalculationDemand *, QAbstractItemView *)`;
    - no `editorEvent`;
    - `stateFor()` returns a `DemandState`;
    - `clusterRect(index)` is a test seam;
    - a static working glyph `drawWorkingGlyph(QPainter*, const QRectF&, const QColor&, qreal rotationDegrees)` (a 270° arc, painted at rotation 0), `drawWarningGlyph`, `glyphPen` and `glyphPenWidth` in the anonymous namespace of `PlotRowDelegate.cpp`;
    - `onPlotStateChanged(plotId)` repaints one row, found through `PlotModel::PlotValueIdRole`.
  - **`PlotRowLayout.h`** uses `indicatorIcon`, `progressLabel`, `warningIcon`, `warningCount` and `clusterWidth`, and has no `controlHit`.
  - **`SessionModel::ColumnFillTask`** exists, and `LogbookView::onProgressChanged` labels it "Computing columns: %v / %m" (the same label as the cheap column pass, on purpose). The task is not cancellable, is active for the whole fill, and steps only when it can load.
  - **Tests:**
    - `tests/tst_calculation_demand.cpp` with its fixture and helpers (`row`, `settle`, `spin`, `restartDemand`, `stored`, `enableColumns`, `makeStubs`, `section`, `colId`, `col`);
    - `tests/tst_plot_row_delegate.cpp` with `makeWorkingRow()` and the Phase 1 function names;
    - `PlotFixture` with 8 plots, `waitDemandIdle`, and `FlySightTest::attributeColumn(key)` in `logbookprobe.h`.
  - **Build and test:**
    - Build only `build-phase1/` with `cmake --build build-phase1 --config Release`.
    - This phase adds a test source file, so reconfigure the inner project once with `cmake build-phase1/FlySightViewer-build` after creating it.
    - Test with `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`.
    - **Never build `build/`.**

---

## Tasks

### Task 3.1: Shared demand glyphs and the animation clock (`DemandIndicator`)

**Purpose:** Paint the working indicator and the warning badge in one place, and provide the clock that turns the indicator. Both the plot rows' delegate and the logbook's header view use it.

**Files to create:**
- `src/ui/docks/DemandIndicator.h`: declarations and file comment.
- `src/ui/docks/DemandIndicator.cpp`: implementation.

**Files to modify:**
- `src/ui/docks/plotselection/PlotRowDelegate.cpp`: remove `glyphPen`, `glyphPenWidth`, `pointOnArc` (if Phase 1 left it) and the two glyph functions from the anonymous namespace; include `ui/docks/DemandIndicator.h`.
- `src/CMakeLists.txt` (`PROJECT_SOURCES`, next to `ui/docks/AppContext.h`, `:412`): add `ui/docks/DemandIndicator.h    ui/docks/DemandIndicator.cpp`.

**Technical Approach:**

1. **Header contents.** Namespace `FlySight`. Include `<QObject>`, `<QTimer>`, `<QtGlobal>`, and forward-declare `QPainter`, `QRectF` and `QColor`. **No widget header, no `QStyleOption`, no demand-layer type**: this file is QtCore + QtGui only.

   ```cpp
   /// The working indicator: an open arc of 270 degrees in `color`, with the pen
   /// width of the badge, turned clockwise by `rotationDegrees` about the rect's
   /// centre. Rotation 0 is exactly the static glyph of phase 1. Leaves the
   /// painter's state as it found it.
   void drawWorkingGlyph(QPainter *painter, const QRectF &rect, const QColor &color, qreal rotationDegrees);
   /// The warning badge: a filled amber triangle with a dark exclamation mark. The
   /// colours are fixed, so it reads on light, dark and highlight backgrounds alike.
   /// Leaves the painter's state as it found it.
   void drawWarningGlyph(QPainter *painter, const QRectF &rect);

   /// The clock of the working indicator: one per view. It ticks only while the
   /// view has something working (setActive(true)), and it restarts at frame 0.
   class WorkingAnimation : public QObject {
       Q_OBJECT
   public:
       static constexpr int kFrameIntervalMs = 80;   ///< 12.5 frames per second
       static constexpr int kFramesPerTurn = 12;     ///< 30 degrees per frame: one turn in 0.96 s
       explicit WorkingAnimation(QObject *parent = nullptr);
       void setActive(bool active);   ///< false also resets the frame to 0
       bool isActive() const;         ///< something is working
       bool isTicking() const;        ///< the timer runs: active and not frozen
       int  frame() const;            ///< 0 .. kFramesPerTurn-1
       qreal angle() const;           ///< frame() * 360 / kFramesPerTurn
       // test seams
       void setFrozen(bool frozen);   ///< a frozen clock never ticks; isActive() is unaffected
       void advance();                ///< one frame now (wraps), and frameAdvanced()
   signals:
       void frameAdvanced();
   private:
       QTimer m_timer;                // interval kFrameIntervalMs, Qt::CoarseTimer
       int m_frame = 0;
       bool m_active = false, m_frozen = false;
   };
   ```

2. **Moving the glyphs.**
   - Move Phase 1's `glyphPen`, `glyphPenWidth` and `drawWarningGlyph` into `DemandIndicator.cpp`, byte for byte, in an anonymous namespace. `drawWarningGlyph` is the exception: it is public.
   - Move `drawWorkingGlyph` with its rotation-0 geometry unchanged. Implement the rotation as `painter->save(); painter->translate(rect.center()); painter->rotate(rotationDegrees); painter->translate(-rect.center());`, then draw exactly as Phase 1 does, then `painter->restore()`. `QPainter::rotate` turns clockwise on screen for positive angles, so frame 0 stays pixel-identical to Phase 1's glyph whatever start angle Phase 1 chose.
   - Wrap `drawWarningGlyph` in `save()`/`restore()` too.
   - Delete `pointOnArc` if nothing uses it any more. It was the refresh glyph's helper.

3. **`WorkingAnimation` semantics:**
   - `setActive(true)` starts the timer unless it is frozen or already running.
   - `setActive(false)` stops the timer and sets `m_frame = 0`. It emits nothing.
   - The timer's timeout and `advance()` do `m_frame = (m_frame + 1) % kFramesPerTurn; emit frameAdvanced();`.
   - `setFrozen(true)` stops the timer. `setFrozen(false)` starts it again if the clock is active.
   - The class never repaints anything itself: its owner connects `frameAdvanced` to its own repaint.

4. **File comment** (top of the header): "Painting shared by the plot list's rows and the logbook's column headers: the working indicator and the warning badge that present the demand layer's state (calculationdemand.h), and the clock that turns the indicator while something is working and stops when nothing is. QtCore and QtGui only." Do not name the executor's class, a thread class or `processEvents` anywhere. The audit rules `no jobs window, no view of the queue` (`src/ui`), `one place creates a thread` and `no nested event loop` match comments too.

**Acceptance Criteria:**
- [ ] `DemandIndicator.h/.cpp` include no QtWidgets header (`git grep -nE "QtWidgets|#include <Q(Widget|Style|Application|AbstractItemView)" src/ui/docks/DemandIndicator.*` is empty).
- [ ] `PlotRowDelegate.cpp` no longer defines `drawWorkingGlyph`, `drawWarningGlyph`, `glyphPen` or `glyphPenWidth`.
- [ ] A frozen clock never emits `frameAdvanced` by itself. An active, unfrozen clock emits it about every 80 ms. `setActive(false)` stops it and resets `frame()` to 0. `advance()` wraps after 12 frames. All of this is tested in Task 3.7 (`workingAnimationClock`).
- [ ] Every Phase 1 pixel test of `tst_plot_row_delegate` still passes: frame 0 equals the static glyph.

**Complexity:** M

---

### Task 3.2: Demand layer: presentation queries and a bounded hover list

**Purpose:** Give the views the few read-only answers they need to run their clocks. Keep hover detail readable when a column has many failures.

**Files to modify:**
- `src/calculationdemand.h` / `.cpp`
- `tests/tst_calculation_demand.cpp`

**Technical Approach:**

1. **Queries** (public, `const`, and no pass is run):

   ```cpp
   /// Ids of the plots / logbook columns whose state isWorking(), in no particular
   /// order; empty when nothing is working. What the views' working-indicator
   /// clocks follow (statesChanged() says when to ask again).
   QStringList workingPlotIds() const;
   QStringList workingColumnIds() const;
   ```

   Each iterates the stored per-plot or per-column states that `applyStates` writes, and returns the keys whose state `isWorking()`. Both are O(number of states) and emit nothing. With a missing collaborator (inert), both are empty.

2. **Bounded lists in `buildToolTip`**. This refines Phase 1 Task 1.2 item 6.
   - Add `static constexpr int kToolTipListLimit = 10;`.
   - The running and failed sections each list at most `kToolTipListLimit` tracks, in list order.
   - When a list has more, a final line of that section reads `"  " + tr("and %n more", nullptr, remaining)`, e.g. `"  and 2 more"`.
   - Every other line and word is unchanged, so Phase 1's and Phase 2's tooltip assertions still hold.
   - The state's own lists stay complete. Only the text is bounded.

3. **Class comment.** Add a section **PRESENTATION**:
   - The views read `plotState`, `columnState`, `isCellPending`, `workingPlotIds` and `workingColumnIds`, and repaint on `plotStateChanged`, `columnStateChanged` and `statesChanged`.
   - They never run a pass (`flush` is a test seam), never offer, and never write.
   - "Pending" exists only here and in the view that paints it: never in `SessionModel`, the cached values or the logbook index.

   Name no widget class.

4. **Tests** (`tst_calculation_demand.cpp`, new functions):

   | Function | What it proves |
   |---|---|
   | `workingIdsFollowStates` | No plot or column → both lists empty. `g` checked, `s1`, `s2` visible, gate held → `workingPlotIds() == {"Syn/g"}`, `workingColumnIds()` empty. `enableColumns({G_OUT})` + `flush()` → `workingColumnIds() == {colId("G_OUT")}`. Unchecking `g` + `flush()` → `workingPlotIds()` empty at once. After the gate is opened and `waitDemandIdle` → both empty. A `statesChanged` spy saw the transition to empty. |
   | `toolTipListsAtMostTenFailures` | Pure `buildToolTip` on hand-built states. 12 failed tracks "Jump 1".."Jump 12" (reason "r"), not working → `"Could not be computed:"`, then 10 lines `"  Jump k - r"` for k = 1..10, then `"  and 2 more"`. Exactly 10 failures → no "more" line. Working with 12 failures → the "Computing" line first, then the capped failure section. |

**Acceptance Criteria:**
- [ ] `workingPlotIds()` and `workingColumnIds()` exist, are `const`, and return exactly the ids whose state `isWorking()`.
- [ ] `buildToolTip` never lists more than `kToolTipListLimit` tracks per section, and ends a longer section with the "and N more" line. Every Phase 1 and Phase 2 tooltip test passes unchanged.
- [ ] The two new functions pass.

**Complexity:** S

---

### Task 3.3: Plot rows: the animated working indicator and hover detail

**Purpose:** Turn Phase 1's static indicator into the animated one. Run the row delegate's clock only while some plot is working, and keep hover detail and the badge as Phase 1 defined them.

**Files to modify:**
- `src/ui/docks/plotselection/PlotRowDelegate.h` / `.cpp`
- `src/ui/docks/plotselection/PlotSelectionDockFeature.cpp`: comment only.

**Technical Approach:**

1. **Members:**
   - `WorkingAnimation *m_animation`, a child of the delegate, always created. Without a demand layer it stays inactive.
   - Public test seam: `WorkingAnimation *animation() const { return m_animation; }`.
2. **Constructor.** If `m_demand && m_view`, connect:
   - `CalculationDemand::statesChanged` → `syncAnimation()`;
   - `QObject::destroyed` of the demand layer → a slot that calls `syncAnimation()` and `m_view->viewport()->update()`, so every row becomes plain at once;
   - `m_animation->frameAdvanced` → `onAnimationFrame()`.

   Then call `syncAnimation()` once, because states may already be working when the view is built.
3. **`syncAnimation()`** (private slot): `m_animation->setActive(m_demand && m_view && !m_demand->workingPlotIds().isEmpty())`.
4. **`onAnimationFrame()`** (private slot):
   - if `!m_demand`, call `syncAnimation()` and return;
   - otherwise, for each id in `m_demand->workingPlotIds()`, `m_view->update(indexForPlot(id))`.

   Factor `QModelIndex indexForPlot(const QString &plotId) const` out of `onPlotStateChanged`, which is Phase 1's `model->match(..., PlotValueIdRole, ..., MatchRecursive)` lookup. Only working rows repaint, about 12 times a second.
5. **`paint()`:** `drawWorkingGlyph(painter, QRectF(geometry.indicatorIcon), color, m_animation->angle())`. Everything else is Phase 1's:
   - the name is elided and never the cluster;
   - the "k of n" label is shown while working;
   - the badge with the failed count is shown when `showsWarning()`;
   - plain rows go to the base class.
6. **Hover.** `toolTipFor()` / `helpEvent()` are unchanged: `DemandState::toolTip` over the whole row, indicator included. The bounded list (Task 3.2) applies automatically.
7. **Class comment** (Phase 1's text, amended):
   - PAINTING: "... a working indicator, an arc that turns about once a second, and 'k of n' ...".
   - REPAINT: "`plotStateChanged(plotId)` repaints that row. While any plot is working (`workingPlotIds()`), a `WorkingAnimation` repaints the working rows 12.5 times a second. It stops, and nothing is repainted, as soon as nothing is working (`statesChanged()`). Without a demand layer the clock never runs."
   - Remove "no timer in this phase".
8. **`PlotSelectionDockFeature.cpp:27-29` comment:** "Rows of plots over requested calculations show an animated working indicator and 'k of n' while their demand is computed, and a warning badge for sessions that could not be computed; hovering a row shows the detail (null demand layer: plain rows)".

**Acceptance Criteria:**
- [ ] The row's clock is active exactly while `workingPlotIds()` is not empty, and inactive without a demand layer or after it is destroyed.
- [ ] Each frame repaints only the working rows. Nothing repaints once nothing is working (Task 3.7).
- [ ] Hover detail over the row, including over the indicator, equals `plotState(id).toolTip`.
- [ ] `PlotRowDelegate` still overrides neither `editorEvent` nor any mouse handler, and `src/` still has no refresh or cancel entry point.

**Complexity:** M

---

### Task 3.4: The logbook's column header view (`LogbookHeaderView`)

**Purpose:** Draw the same working indicator and warning badge at the right of a logbook column header's text while that column has demand, and show hover detail on the header. Sorting, resizing, moving and hiding sections keep working as they do today.

**Files to create:**
- `src/ui/docks/logbook/LogbookHeaderView.h`
- `src/ui/docks/logbook/LogbookHeaderView.cpp`

**Files to modify:**
- `src/CMakeLists.txt` (`PROJECT_SOURCES`, next to `LogbookView`, `:420`): add `ui/docks/logbook/LogbookHeaderView.h    ui/docks/logbook/LogbookHeaderView.cpp`.

**Technical Approach:**

1. **Class** (namespace `FlySight`):

   ```cpp
   class LogbookHeaderView : public QHeaderView {
       Q_OBJECT
   public:
       /// A horizontal header for the logbook's tree, configured as QTreeView's own
       /// (movable sections, last section stretched, AlignLeft|AlignVCenter default
       /// alignment). `model` is the session model the tree shows; `demand` may be
       /// null (then the header is exactly QHeaderView).
       LogbookHeaderView(SessionModel *model, CalculationDemand *demand, QWidget *parent = nullptr);

       /// What hovering the section shows: columnState(<its column id>).toolTip;
       /// empty for a plain section, out of range, or without a demand layer.
       QString toolTipForSection(int logicalIndex) const;
       /// Where the section's indicator or badge is painted, in viewport
       /// coordinates; null when none is (plain, hidden, or too narrow). Tests only.
       QRect indicatorRect(int logicalIndex) const;
       WorkingAnimation *animation() const;          ///< tests only

   protected:
       void paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const override;
       QSize sectionSizeFromContents(int logicalIndex) const override;
       bool viewportEvent(QEvent *event) override;

   private slots:
       void onColumnStateChanged(const QString &columnId);
       void syncAnimation();
       void onAnimationFrame();

   private:
       QString columnIdOf(int logicalIndex) const;   ///< empty when out of range or not our model
       DemandState stateFor(int logicalIndex) const; ///< DemandState() when columnIdOf is empty or no demand
       /// Elides opt.text for the glyph's room and returns the glyph's rect (null
       /// when the label is narrower than the glyph). `opt` is fully initialized.
       QRect layoutGlyph(QStyleOptionHeaderV2 &opt) const;
       QRect sectionViewportRect(int logicalIndex) const;

       QPointer<SessionModel> m_model;
       QPointer<CalculationDemand> m_demand;
       WorkingAnimation *m_animation;                // child; never null
   };
   ```

2. **Mapping a section to a logbook column.** The tree shows `SessionModel` directly: there is no proxy, and sorting is `SessionModel::sort()`. A header **logical** index is therefore a `SessionModel` column index.
   - `columnIdOf(l)` returns `CalculationDemand::columnId(m_model->column(l))` when `m_model && model() == m_model && 0 <= l < m_model->columnCount()`, else `{}`.
   - It is computed **on every call** and never cached. `rebuildColumns()` (column editor, profiles) and `sort()` both reset the model, and a cached mapping would go stale.
   - `paintSection` receives logical indices, and tooltips use `logicalIndexAt(pos)`, so moving sections (drag and drop) needs nothing more.
   - Hidden sections are never painted, and `indicatorRect` returns null for them (`isSectionHidden(l)`).
   - The fixed columns are ordinary `SessionModel` columns here. Column 0's check box is the tree's and is not in the header.
3. **`paintSection(painter, rect, l)`:**
   1. `const DemandState state = stateFor(l);` If `state.isPlain()` or `!rect.isValid()`, call `QHeaderView::paintSection(painter, rect, l)` and return. A plain section is the base class's, pixel for pixel.
   2. Otherwise build the option exactly as `QHeaderView::paintSection` does (Qt 6.9 `qheaderview.cpp:3088-3116`):
      - a `QStyleOptionHeaderV2 opt`;
      - save the brush origin, then `initStyleOption(&opt)`;
      - the brush comparison;
      - `initStyleOptionForIndex(&opt, l)`, then `opt.rect = rect`;
      - set the brush origin when the brushes differ.
   3. `const QRect glyph = layoutGlyph(opt);`, then `style()->drawControl(QStyle::CE_Header, &opt, painter, this);` and restore the brush origin.
   4. If `!glyph.isNull()`:
      - `painter->save()`, `setClipRect(rect)`, `setRenderHint(QPainter::Antialiasing)`;
      - if `state.isWorking()`: `drawWorkingGlyph(painter, QRectF(glyph), color, m_animation->angle())`;
      - else (`showsWarning()`): `drawWarningGlyph(painter, QRectF(glyph))`;
      - `painter->restore()`.

      The two are never both drawn.
   5. `color` is `opt.palette.color(group, QPalette::ButtonText)`, the header text's role, where `group` is `Disabled` without `State_Enabled`, `Inactive` without `State_Active`, else `Normal`. This is the rule of the plot delegate's `glyphColor`. It follows light and dark palettes.
4. **`layoutGlyph(opt)`.** The glyph sits at the right of the text, never over the sort arrow, and the text gives way:
   - `label = style()->subElementRect(QStyle::SE_HeaderLabel, &opt, this)`. In Fusion (`QCommonStyle`, `qcommonstyle.cpp:2631-2646`) this rect already **excludes the sort arrow** when the section shows one (`SE_HeaderArrow` is at the section's right). A glyph inside `label` therefore never overlaps the arrow.
   - `side = qMin(label.height(), opt.fontMetrics.height())` (one text line tall), `spacing = qMax(2, side / 4)` (the plot rows' rule), `reserve = side + spacing`.
   - If `label.width() < side`, return `{}`: no glyph, and the text is untouched. The tooltip still works.
   - **Text.** `SessionModel::headerData` returns `Qt::AlignCenter` (`sessionmodel.cpp:440`) and a two-line text "name\n(unit)".
     - Reserve room symmetrically, so the centred text stays centred and clear of the glyph: `textWidth = qMax(0, label.width() - 2 * reserve)`.
     - Elide **each line** separately (`opt.text.split('\n')`, `opt.fontMetrics.elidedText(line, Qt::ElideRight, textWidth)`), rejoin with `'\n'`, and set `opt.textElideMode = Qt::ElideNone`. `CE_HeaderLabel` would otherwise elide the whole two-line string against one width (`qcommonstyle.cpp:1676-1679`).
     - `block` = the widest elided line's `horizontalAdvance`.
   - **Glyph rect** (left-to-right):
     - `x = label.center().x() + (block + 1) / 2 + spacing`, clamped to `label.right() + 1 - side`;
     - `y = label.y() + (label.height() - side) / 2`;
     - `QRect(x, y, side, side)`.
   - **Right-to-left** (`opt.direction == Qt::RightToLeft`): the mirror, left of the text: `x = label.center().x() - (block + 1) / 2 - spacing - side`, clamped to `label.left()`.
5. **`indicatorRect(l)`:** `{}` when the section is hidden, `stateFor(l).isPlain()`, or `columnIdOf(l)` is empty. Otherwise build the option as in `paintSection` with `opt.rect = sectionViewportRect(l)` (`QRect(sectionViewportPosition(l), 0, sectionSize(l), viewport()->height())`) and return `layoutGlyph(opt)`.
6. **`sectionSizeFromContents(l)`** (used by double-clicking a section handle and by `ResizeToContents`): the base size, plus `2 * reserve` in width when `columnState(columnIdOf(l)).requested`. A double-click then fits the text beside the glyph. Compute `reserve` from `fontMetrics()` as in item 4.
7. **Hover detail** (`viewportEvent`). On `QEvent::ToolTip`:
   - `logical = logicalIndexAt(helpEvent->pos())`, `text = toolTipForSection(logical)`;
   - if `text` is not empty: `QToolTip::showText(helpEvent->globalPos(), text, viewport(), sectionViewportRect(logical))` and `return true`. The whole section, glyph included, shows it, and it hides when the pointer leaves the section.
   - Every other event, and an empty text, goes to `QHeaderView::viewportEvent(event)`, which keeps any `ToolTipRole` behaviour.

   The header overrides **no mouse handler**: a click anywhere in a section, glyph included, sorts exactly as today.
8. **Repaint.** The constructor connects the following when `m_demand` is set:
   - `columnStateChanged(id)` → `onColumnStateChanged(id)`: for each logical `l` in `0..count()-1` with `columnIdOf(l) == id` and not hidden, call `updateSection(l)`, the protected slot of `QHeaderView`. There is no model signal and no reset.
   - `statesChanged` → `syncAnimation()`: `m_animation->setActive(m_demand && !m_demand->workingColumnIds().isEmpty())`.
   - `m_animation->frameAdvanced` → `onAnimationFrame()`: if `!m_demand`, `syncAnimation()` and return; else `updateSection(l)` for every visible section whose id is in `workingColumnIds()`.
   - `destroyed` of the demand layer → `syncAnimation()` + `viewport()->update()`.

   Call `syncAnimation()` once at the end of the constructor.
9. **Constructor defaults**: exactly what `QTreeViewPrivate::initialize()` gives its own header (Qt 6.9 `qtreeview.cpp:3118-3121`):
   - `QHeaderView(Qt::Horizontal, parent)`;
   - `setSectionsMovable(true)`;
   - `setStretchLastSection(true)`;
   - `setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter)`.

   `QTreeView::setHeader()` adds `setFirstSectionMovable(false)` and the sort settings.
10. **Class comment** (header file): what it paints and why (spec §10), mapping (logical index = `SessionModel` column; id = `CalculationDemand::columnId`; computed per call), layout (glyph right of centred text inside `SE_HeaderLabel`, which excludes the sort arrow; per-line elision), hover, repaint and clock, "no gesture: clicks are the base class's", "without a demand layer it is exactly `QHeaderView`". Do not name the executor's class.

**Acceptance Criteria:**
- [ ] A plain section paints exactly as `QHeaderView` does (Task 3.8 `plainHeaderAndCellsAreIdenticalToBase`).
- [ ] A working section paints the arc, and a finished section with failures paints the badge, inside `SE_HeaderLabel` at the right of the text. The glyph never intersects the `SE_HeaderArrow` rect of a sorted section.
- [ ] Moved, hidden and reordered columns keep their indicator on the right column.
- [ ] Hovering any point of a non-plain section shows `columnState(id).toolTip`.
- [ ] A click on the glyph sorts like a click elsewhere in the section, and starts or cancels no job.
- [ ] The header's clock is active exactly while `workingColumnIds()` is not empty.

**Complexity:** L

---

### Task 3.5: Pending cells (`LogbookCellDelegate`)

**Purpose:** Let a logbook cell whose pair is in demand read "pending" (not yet), distinct from an empty "unavailable" cell (never) and from the row's unreadable-record pending state. The cached value, the index and sorting are untouched.

**Files to create:**
- `src/ui/docks/logbook/LogbookCellDelegate.h`
- `src/ui/docks/logbook/LogbookCellDelegate.cpp`

**Files to modify:**
- `src/CMakeLists.txt` (`PROJECT_SOURCES`): add `ui/docks/logbook/LogbookCellDelegate.h    ui/docks/logbook/LogbookCellDelegate.cpp`.

**Technical Approach:**

1. **Class:**

   ```cpp
   class LogbookCellDelegate : public QStyledItemDelegate {
       Q_OBJECT
   public:
       /// `view` is the parent and the tree whose columns are repainted; `demand` may be null.
       LogbookCellDelegate(SessionModel *model, CalculationDemand *demand, QTreeView *view);
       void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
       bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                      const QModelIndex &index) override;
       /// True when the cell is painted as pending: its pair is in demand
       /// (isCellPending) and the model has no value for it.
       bool showsPending(const QModelIndex &index) const;
       static QString pendingText();       ///< "…" (a horizontal ellipsis)
       static QString pendingToolTip();    ///< tr("Pending: this value is being computed")
   private slots:
       void onColumnStateChanged(const QString &columnId);
   private:
       QPointer<SessionModel> m_model;
       QPointer<CalculationDemand> m_demand;
       QPointer<QTreeView> m_view;
   };
   ```

2. **`showsPending(index)`** is `m_demand && m_model && index.isValid() && index.model() == m_model && m_demand->isCellPending(index.row(), index.column()) && index.data(Qt::DisplayRole).toString().isEmpty()`.
   - **A value always wins.** If the model has a value (the record was just written and the loaded row reads it live), the value is shown, whatever the demand layer said in its last pass.
   - `isCellPending(row, column)` maps the row to its session id on every call (Phase 2), so a sort or a column rebuild can never make it answer for the wrong cell.
   - It is O(1) apart from building the column's definition key. It is called only for painted cells.
3. **`paint`:**
   - If `!showsPending(index)`: `QStyledItemDelegate::paint(...)` and return. Every non-pending cell is the base delegate's, pixel for pixel.
   - Otherwise:
     - `QStyleOptionViewItem opt = option; initStyleOption(&opt, index);`
     - `opt.text = pendingText(); opt.features |= QStyleOptionViewItem::HasDisplay;`
     - Set the muted colour for all groups:
       - `opt.palette.setColor(QPalette::Text, opt.palette.color(group, QPalette::PlaceholderText))`, with `group` chosen as in Task 3.4 item 3.5;
       - `QColor selected = opt.palette.color(group, QPalette::HighlightedText); selected.setAlphaF(0.6);`, then `opt.palette.setColor(QPalette::HighlightedText, selected)`.
     - The alignment is the cell's own (`opt.displayAlignment` from the model; none is set, so left), where the value will appear.
     - Draw with `style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget)`, where `widget = opt.widget` and `style = widget ? widget->style() : QApplication::style()`, as `PlotRowDelegate::paint` does. Background, selection, hover and focus stay the style's.
4. **The three looks:**
   - a **value**;
   - **empty**: "unavailable" (cached invalid) and the unreadable-record pending state (`SessionRow::pendingColumns`, not cached, `data()` invalid), exactly as today;
   - a **muted "…"**: in demand.

   A cell in `pendingColumns` is never demand-pending. It is a stub over a known record, and Phase 2 classifies a known record as `Done`. So the two pending states never meet in one cell.
5. **`helpEvent`:** on `QEvent::ToolTip`, if `showsPending(index)`: `QToolTip::showText(event->globalPos(), pendingToolTip(), view->viewport(), option.rect)` and `return true`. Otherwise use the base class.
6. **Repaint** (`onColumnStateChanged(id)`): connected in the constructor when `m_demand && m_view`. For each column `c` in `0..m_model->columnCount()-1` with `CalculationDemand::columnId(m_model->column(c)) == id` and `!m_view->isColumnHidden(c)`, call `m_view->viewport()->update(QRect(m_view->columnViewportPosition(c), 0, m_view->columnWidth(c), m_view->viewport()->height()))`.
   - This is **one repaint of the visible part of one column per pass that changed it**: no `dataChanged`, no reset, no layout change.
   - The value itself appears through the model's own signals. A publication emits the row's `dataChanged` (`SessionModel::publishInvalidation`, `sessionmodel.cpp:1712-1717`), and a stub's values arrive when the demand layer has loaded it.
   - Also connect `destroyed` of the demand layer → `m_view->viewport()->update()`.
7. **Pending cells are static** (no animation). The column header's indicator carries the motion. Animating possibly thousands of cells would repaint the whole table 12 times a second.
8. **Sizes and editing:** `sizeHint`, `createEditor`, `setEditorData` and `setModelData` are not overridden. Row heights stay uniform, and editing is the base delegate's. A pending cell's column is never editable: requested outputs are not editable attributes.
9. **Class comment:** the three looks above; "pending is a presentation of demand: the model, its cached values, `pendingColumns`, `index.json` and `SessionModel::sort()` never see it; the cached value underneath stays unavailable until the record is written, so sorting treats a pending cell as unavailable"; value wins; repaint; "without a demand layer it is exactly `QStyledItemDelegate`".

**Acceptance Criteria:**
- [ ] With no requested column enabled, or without a demand layer, every cell paints exactly as with `QStyledItemDelegate`.
- [ ] A cell is painted pending exactly when `isCellPending(row, column)` is true and the model has no value. Its pixels differ from the empty unavailable cell's.
- [ ] `SessionModel::data`, `SessionRow::cachedValues`, `SessionRow::pendingColumns` and `index.json` never contain a pending marker. No product code outside `LogbookCellDelegate` uses `pendingText()`.
- [ ] `SessionModel::sort` and `SessionModel::data` are unchanged (`git diff` of both functions is empty).
- [ ] A `columnStateChanged` repaints only that column's visible rect of the tree viewport, and causes no model signal.

**Complexity:** M

---

### Task 3.6: Wiring, comments, and the "no refresh, no cancel" check

**Purpose:** Install the header view and the cell delegate in the logbook, pass the demand layer through, and confirm that no refresh or cancel for requested calculations exists anywhere.

**Files to modify:**
- `src/ui/docks/logbook/LogbookView.h` / `.cpp`
- `src/ui/docks/logbook/LogbookDockFeature.cpp`
- `src/ui/docks/AppContext.h`: comment only.
- `src/mainwindow.cpp`: comment only.

**Technical Approach:**

1. **`LogbookView`:**
   - Forward-declare `class CalculationDemand;`.
   - The constructor becomes `LogbookView(SessionModel *model, CalculationDemand *demand, QWidget *parent = nullptr)`, with a member `CalculationDemand *m_demand` initialized before `setupView()` runs.
   - `setupView()` (`:70`). Order matters:
     1. `treeView->setHeader(new LogbookHeaderView(model, m_demand, treeView));`, **before** `setModel`, so the tree hands the header its model and sort settings (`QTreeView::setHeader`, `qtreeview.cpp:274-306`);
     2. `treeView->setModel(model);`
     3. `treeView->setItemDelegate(new LogbookCellDelegate(model, m_demand, treeView));`
     4. the existing lines unchanged: default section size, fixed two-line height from `header()->font()`, mouse tracking, selection, edit triggers, uniform row heights, the viewport event filter, and `setSortingEnabled(true)` last.
   - Add a class comment to `LogbookView.h`: "The logbook table: the session model in a tree with a header that shows each column's working indicator or warning badge and its hover detail (LogbookHeaderView), cells that read pending while their value is being computed (LogbookCellDelegate), and the progress line of the idle scheduler's tasks. The demand layer may be null: then header and cells are plain."
   - The progress-line slots and labels (`:266-307`) are **unchanged**. The progress line's form is spec §10's last bullet. Phase 2's `ColumnFillTask` label is already there, and its cancel button stays hidden because the task is not cancellable.
2. **`LogbookDockFeature.cpp:16`:** `new LogbookView(ctx.sessionModel, ctx.calculationDemand, m_dock)`, with the comment "the demand layer supplies the column headers' indicators and the pending cells (null: plain)". The scheduler wiring (`:36-49`) is unchanged.
3. **`AppContext.h`:** the `calculationDemand` comment becomes `// what the plot list's rows and the logbook's column headers and cells present (may be null: all plain)`.
4. **`mainwindow.cpp` destructor comment** (Phase 1's): add "The plot list's delegate and the logbook's header and cell delegate hold the demand layer weakly and turn plain once it is gone."
5. **Check that no refresh or cancel remains** (spec §10, first bullet). Run the searches below and confirm every hit is on the allowed list. Record the result in the phase report; no file changes are expected.
   - `git grep -nEi "refresh|recompute" -- src/ui src/mainwindow.cpp src/mainwindow.h src/mainwindow.ui src/calculationdemand.*`. Allowed hits: `altitudemarker` `refresh()` calls in `mainwindow.cpp`, and none in `src/ui`.
   - `git grep -nE "[Cc]ancel" -- src/ui`. Allowed hits: the logbook progress line's scheduler cancel button (`LogbookView.*`, `LogbookDockFeature.cpp`) and `VideoWidget`'s `cancelPendingSeek`.
   - `git grep -nE "QAction|QShortcut|addAction" -- src/ui/docks/plotselection src/ui/docks/logbook`. Allowed hits: `LogbookView::onContextMenuRequested` (show, hide, hide others, set attribute, delete) only.
   - The Plots menu in `mainwindow.cpp` toggles plots (`togglePlot`), and nothing computes or cancels a calculation.

   The audit's `gestures` rules (Phase 1) enforce the rest.

**Acceptance Criteria:**
- [ ] The application builds (`build-phase1`). The logbook shows its header and cells through the new classes, sorting by a header click still works, sections can still be dragged, and the last section still stretches.
- [ ] `LogbookView` is constructed with the demand layer. With `nullptr` it behaves exactly as before this phase.
- [ ] The searches of item 5 show only allowed hits.
- [ ] `ctest -L audit` passes with no rule edited. The new `src/ui` files name neither the executor's class nor `JobModel`, `QThread`, `QEventLoop` or `processEvents`.

**Complexity:** S

---

### Task 3.7: Plot row widget tests (`tst_plot_row_delegate`)

**Purpose:** Prove the plot rows' animated indicator, its clock, the badge and hover detail against the demand layer's state, without disturbing Phase 1's pixel tests.

**Files to modify:**
- `tests/tst_plot_row_delegate.cpp`
- `tests/CMakeLists.txt`: the `tst_plot_row_delegate` SOURCES gain `"${FLYSIGHT_SRC_DIR}/ui/docks/DemandIndicator.cpp"` and `"${FLYSIGHT_SRC_DIR}/ui/docks/DemandIndicator.h"`.

**Technical Approach:**

1. **`buildUi()`:** right after creating the delegate, `m_delegate->animation()->setFrozen(true);`, with the comment "pixel comparisons need a still indicator (frame 0, the static glyph); the animation tests unfreeze it". Every Phase 1 function keeps its name and its assertions. The acceptance map cites them.
2. **New functions:**

   | Function | What it proves |
   |---|---|
   | `workingAnimationClock` | A standalone `WorkingAnimation`:<br>• Inactive and not ticking at first. `setActive(true)` → active, ticking, `frame() == 0`. A `frameAdvanced` spy reaches ≥ 2 within 1000 ms (`QTRY_VERIFY`).<br>• `setActive(false)` → not ticking, `frame() == 0`, and the spy count is unchanged over `QTest::qWait(4 * kFrameIntervalMs)`.<br>• Frozen and active → `isActive()` and `!isTicking()`. `advance()` → `frame() == 1`, `angle() == 30.0`, one signal. 11 more `advance()` → `frame() == 0`. |
   | `workingIndicatorAnimatesOnlyWhileWorking` | Setup: `makeWorkingRow()`, then `animation()->isActive()`.<br>• Grab the viewport (frame 0) and `advance()` once. The pixels inside `clusterRect(index)` differ, the check box and the "0 of 2" label pixels are identical, and a `PaintCounter` on the viewport counts a paint.<br>• Unfreeze → `isTicking()`. Open the gate (2) and `waitDemandIdle`: the row is plain, `!isActive()`, `!isTicking()`, and the paint count is unchanged over `QTest::qWait(4 * WorkingAnimation::kFrameIntervalMs)` (no idle repaint).<br>• Check `Syn/g2`, whose demand is already done → after `spin()`, `!isActive()`. |
   | `badgeReplacesIndicatorOnceFinished` | Setup: `EA_IN = -1` on `s1` and `4` on `s2`; check `ea`; `waitDemandIdle`.<br>• `showsWarning()`, `!isActive()`.<br>• Inside `clusterRect`, at least one pixel equals the badge's amber `#E69F00`, and the glyph slot differs from a working row's frame-0 arc (compare against `makeWorkingRow()`'s grab of `Syn/g`).<br>• Tooltip: `"Could not be computed:\n  Jump 1 - Explicit A: negative input"`. |
   | `hoverDetailFollowsDemandState` | Working: `toolTipFor(g) == row("Syn/g").toolTip == "Computing: 0 of 2 done\n  Jump 1 - Gated: step 1"`. A `QHelpEvent` at the centre of `clusterRect` (the indicator) shows the same text (`QToolTip::text()`). Open the gate once, `gate().waitEntered()`, `flush()` → `"Computing: 1 of 2 done\n  Jump 2 - Gated: step 1"`. |

3. **Extend `survivesDemandDestroyedFirst`** (Phase 1's name) with `QVERIFY(!m_delegate->animation()->isActive())` after `m_demand.reset()`.
4. The header comment gains a line: "the working indicator's clock (DemandIndicator.h) runs only while a plot is working".

**Acceptance Criteria:**
- [ ] The four new functions pass, and every Phase 1 function passes unchanged, with the clock frozen at frame 0.
- [ ] The clock is shown to stop, with no repaint, once nothing is working.

**Complexity:** M

---

### Task 3.8: Logbook widget tests (`tst_logbook_indicators`, new)

**Purpose:** Prove the column header's indicator, badge, hover and clock, and the pending cells, in a real `LogbookView` over a real `SessionModel`, executor and demand layer. This covers the logbook half of spec §13's two presentation rows.

**Files to create:**
- `tests/tst_logbook_indicators.cpp`: class `LogbookIndicatorsTest`, with its own `main()` (a `QApplication` with the Fusion style, as `tst_plot_row_delegate.cpp:929-946` does).

**Files to modify:**
- `tests/CMakeLists.txt`. Inside the `if(FLYSIGHT_BUILD_WIDGET_TESTS)` block, after `tst_plot_row_delegate`:

  ```cmake
  # ... and the logbook's column headers (working indicator, badge, hover) and
  # pending cells in a real LogbookView, compiled from the application sources.
  flysight_add_test(tst_logbook_indicators
    SOURCES tst_logbook_indicators.cpp
            "${FLYSIGHT_SRC_DIR}/ui/docks/logbook/LogbookView.cpp"          "${FLYSIGHT_SRC_DIR}/ui/docks/logbook/LogbookView.h"
            "${FLYSIGHT_SRC_DIR}/ui/docks/logbook/LogbookHeaderView.cpp"    "${FLYSIGHT_SRC_DIR}/ui/docks/logbook/LogbookHeaderView.h"
            "${FLYSIGHT_SRC_DIR}/ui/docks/logbook/LogbookCellDelegate.cpp"  "${FLYSIGHT_SRC_DIR}/ui/docks/logbook/LogbookCellDelegate.h"
            "${FLYSIGHT_SRC_DIR}/ui/docks/DemandIndicator.cpp"              "${FLYSIGHT_SRC_DIR}/ui/docks/DemandIndicator.h"
    LIBS Qt${QT_VERSION_MAJOR}::Widgets)
  set_tests_properties(tst_logbook_indicators PROPERTIES LABELS "core;widgets")
  ```

  Rewrite the file's header comment (`:9-17`): the exceptions now include "`tst_plot_row_delegate` and `tst_logbook_indicators`: the plot list's row delegate, and the logbook view with its header view and cell delegate, the only tests that link Qt Widgets (`FLYSIGHT_BUILD_WIDGET_TESTS`)". Rewrite the comment above the widget block the same way. Reconfigure the inner project once.

**Technical Approach:**

**Fixture:**
- **`initTestCase`:**
  - `TestEnvironment::instance().registerBuiltIns()`;
  - `registerPreference(LogbookColumnsVersion, 0)`;
  - `LogbookColumnStore::instance().setColumns({descriptionColumn()})`.
- **`init`:**
  1. `useFreshLogbook()`, `resetPreferencesToDefaults()`, `LogbookManager::instance().initialize()`; record the registry ids.
  2. Create `JobWorld`, then `PlotFixture`, then `SessionModel`.
  3. `mergeSessions(JobWorld::sessions({"s1","s2","s3","s4"}))`. Name them "Jump 1".."Jump 4" (`_DESCRIPTION`). `G_IN` is 1, 2 and 4 on `s1`, `s2` and `s4`; `s3` has none, so it is not applicable.
  4. `flushPendingInvalidations()`; `QVERIFY(waitForIdle(*m_model))`. The sessions are loaded, hidden, saved and indexed.
  5. The executor; a `PlotModel` with `PlotFixture::plots()`, none checked; the demand layer.
  6. The UI: a top-level `QWidget m_window` with a `QHBoxLayout` holding:
     - the `LogbookView(m_model, m_demand)`, fixed width 460;
     - a **reference** `QTreeView`, fixed width 460, configured as `LogbookView::setupView` configures its tree but with QTreeView's own header: `setModel`, `setRootIsDecorated(false)`, `header()->setDefaultSectionSize(100)`, the same fixed header height, `setUniformRowHeights(true)`, `setSortingEnabled(true)`.

     Both share the model and live in one window, so they paint under the same active state. Height 320; `QTest::qWaitForWindowExposed`.
  7. Freeze the header's clock (`header()->animation()->setFrozen(true)`), then `spin()`.
- **`cleanup`:**
  1. `gate().open(16)`; `waitDemandIdle`; `m_queue->shutdown()`;
  2. `QToolTip::hideText()`; destroy `m_window` (the views) **before** the demand layer;
  3. then the demand layer, executor, model, `PlotFixture` and `JobWorld`;
  4. `setColumns({descriptionColumn()})`, `LogbookCacheSize` 50;
  5. the registry checks of `tst_plot_row_delegate::cleanup`.
- **Helpers:**
  - `tree()` = `m_logbook->findChild<QTreeView *>()`;
  - `header()` = `qobject_cast<LogbookHeaderView *>(tree()->header())`;
  - `cells()` = `qobject_cast<LogbookCellDelegate *>(tree()->itemDelegate())`;
  - `enableColumns(list)` = `setColumns(description + list)`, the profile / editor path;
  - `section(key)`, `colId(key)`, `col(key)` = `flush()` + `columnState`;
  - `cell(id, key)` = `m_model->index(getSessionRow(id), section(key))`;
  - `stored(id, calc)`;
  - `grabHeader()` / `grabReferenceHeader()`;
  - `grabCellsWithBaseDelegate()`: swap in a `QStyledItemDelegate` and restore, as `tst_plot_row_delegate` does;
  - `sectionRect(l)`;
  - `spin()` = `PlotFixture::spin(m_demand)` + `QApplication::processEvents()`;
  - `PaintCounter`, and a `RegionRecorder` (an event filter uniting `QPaintEvent::rect()`).
- **Columns:** `G_OUT` over `gated` (`G_OUT = G_IN + 1`; values 2, 3 and 5), and `EA1` over `expA` (rejects `EA_IN < 0` with "negative input").

**Functions:**

| Function | What it proves |
|---|---|
| `plainHeaderAndCellsAreIdenticalToBase` | Columns: description and `attributeColumn("G_IN")`, neither requested:<br>• `grabHeader() == grabReferenceHeader()`;<br>• the tree viewport equals the base delegate's;<br>• `indicatorRect(l)` null and `toolTipForSection(l)` empty for every section;<br>• the clock is inactive;<br>• `sectionSizeHint(l)` equals the reference header's. |
| `workingColumnShowsIndicatorRightOfText` | `enableColumns({G_OUT})` with the gate held; `gate().waitEntered()`, `flush()`.<br>• `r = indicatorRect(section(G_OUT))` is not null. It lies inside `sectionRect`, `r.left() > sectionRect.center().x()`, and it is vertically inside the header.<br>• Pixels in `r` differ from the reference header's; every other section is pixel-identical to the reference.<br>• The header height is unchanged.<br>• `sectionSizeHint(G_OUT) > reference.sectionSizeHint(G_OUT)`. |
| `indicatorAnimatesOnlyWhileWorking` | Working → `animation()->isActive()`. Grab; `advance()`; the pixels in `indicatorRect` differ and a `PaintCounter` on the header viewport counts a paint. Unfreeze → `isTicking()`. Open the gate (3); `waitDemandIdle` + `waitForIdle(model)` → `!isActive()`, `!isTicking()`, `indicatorRect` null, and the header's paint count is unchanged over `QTest::qWait(4 * kFrameIntervalMs)`. |
| `badgeReplacesIndicatorWhenFinished` | `EA_IN` is -1 on `s1` and 4 on `s2` and `s4`; `enableColumns({EA1})`; `waitDemandIdle`.<br>• `col(EA1).showsWarning()`, and `indicatorRect` is not null.<br>• An amber `#E69F00` pixel is inside it, and the clock is inactive.<br>• A click on it creates no job (`Quiet`). |
| `failedLoadSessionShowsBadgeNotPending` | Presentation half of Phase 2's `visibleFailedLoadIsSettledAsFailed`. Delete `s2`'s session file while `s2` is a stub (`LogbookCacheSize` 0, then `waitForIdle`); `PlotFixture::show(*m_model, {"s2"})` → a visible failed-load placeholder. `enableColumns({G_OUT})`, gate open; `waitDemandIdle` + `waitForIdle(model)`.<br>• `showsPending(cell(s2))` is false, and `s2`'s cell pixels equal the base delegate's (empty, no "…").<br>• The `G_OUT` header shows the badge (an amber `#E69F00` pixel inside `indicatorRect`), not the arc; the header clock is inactive.<br>• `toolTipForSection` contains "Could not be computed:" and "The session file could not be loaded".<br>• Over `spin()` ×3 the header's paint count is unchanged. |
| `headerToolTipFollowsDemandState` | Working, gate held:<br>• `toolTipForSection == col(G_OUT).toolTip == CalculationDemand::buildToolTip(col(G_OUT))`, which starts `"Computing: 0 of 3 done"` and contains `"  Jump 1 - Gated: step 1"`;<br>• a `QHelpEvent` sent to `header()->viewport()`, once at the indicator's centre and once at the section's left edge + 4 px, shows it (`QToolTip::text()`).<br>Open one: `"Computing: 1 of 3 done"`, `"Jump 2 - Gated: step 1"`.<br>Failed (`EA1` as above): `"Could not be computed:\n  Jump 1 - Explicit A: negative input"`.<br>Over the description section: `QToolTip::hideText()` first; the event is not accepted and no tooltip shows. |
| `indicatorFollowsColumnWhenMovedHiddenOrReordered` | Columns: description, `G_IN`, `G_OUT`, gate held.<br>• `header()->moveSection(visualIndex(G_OUT), 0)` → `indicatorRect(section(G_OUT))` lies inside that section's new `sectionRect`; the moved-away sections have none.<br>• `hideSection` → null, and nothing is painted for it. `showSection` → back.<br>• `enableColumns({G_OUT, G_IN})` (a reset, new logical indices) → the indicator is on the new `section(G_OUT)`, and `toolTipForSection` of `G_IN`'s section is empty. |
| `indicatorClearsSortArrowAndNarrowSections` | `tree()->sortByColumn(section(G_OUT), Qt::AscendingOrder)`. Build a `QStyleOptionHeader` for the section (rect = `sectionRect`, `State_Horizontal`, `sortIndicator = SortUp`) and ask the style for `SE_HeaderArrow`: `indicatorRect` does not intersect it.<br>`resizeSection` to 60 → still inside and still clear of the arrow. To 12 → null, with no crash when painted. |
| `clickOnIndicatorIsAClickOnTheSection` | Finished column (gate opened, idle). `QTest::mouseClick` on `header()->viewport()` at the indicator's centre → `sortIndicatorSection() == section(G_OUT)`; a second click flips `sortIndicatorOrder()`, as for a click elsewhere in the section. `Quiet` holds. No `jobCancelRequested`. |
| `pendingCellsAreDistinctFromUnavailable` | `G_OUT` enabled, gate held: `s1` running, `s2` and `s4` waiting.<br>• `showsPending` is true for `s1`, `s2`, `s4` and false for `s3` and for every description cell.<br>• Against the base delegate, `s2`'s cell pixels differ, and `s3`'s cell and every description cell are identical.<br>• For `s1`–`s4`: `data(DisplayRole)` and `data(ToolTipRole)` are invalid; `rowAt(r).cachedValues.value(section)` is invalid; `!pendingColumns.contains(section)`; `indexValue(id, G_OUT)` is null or undefined.<br>• `readIndex()` serialized contains no `"…"`.<br>• A `QHelpEvent` over `s2`'s cell shows `pendingToolTip()`; over `s3`'s, nothing. |
| `pendingCellBecomesValueWhenRecordIsWritten_data/()` | Rows "loaded" (the fixture) and "stub" (`LogbookCacheSize` 0, then `waitForIdle`: every row a stub). |
| `pendingCellBecomesValueWhenRecordIsWritten` | The scheduler is wired to the view as `LogbookDockFeature.cpp:36-49` does, and a slot notes `m_cancelButton` (`findChild<QToolButton *>()`) visibility at every `activeTaskChanged(ColumnFillTask, …)`. `enableColumns({G_OUT})`, gate held.<br>Open one at a time. After `s1`'s job:<br>• `QTRY_VERIFY(!showsPending(cell(s1)))`, `cell(s1).data() == "2"`, `stored("s1","gated")`;<br>• after `waitForIdle`, `indexValue("s1", G_OUT).toDouble() == 2`;<br>• `s2` is still pending.<br>At the end:<br>• no cell pending; values 2, 3, empty and 5;<br>• `index.json` holds those values and never held `"…"`;<br>• the "stub" row saw at least one `ColumnFillTask` activation and the cancel button hidden at every one ("no cancel for requested calculations");<br>• the "loaded" row saw no `modelAboutToBeReset` during the fill. |
| `sortingTreatsPendingAsUnavailable` | `s1` computed (value 2); `s2` running; `s4` waiting; `s3` unavailable.<br>• `sortByColumn(G_OUT, Ascending)` → row 0 is `s1`, and `s2`, `s3`, `s4` follow in some order. `showsPending` holds exactly for `getSessionRow("s2")` and `getSessionRow("s4")`.<br>• `Descending` → row 0 is still `s1`, because missing values sort to the bottom both ways (`sessionmodel.cpp:2452-2455`).<br>• The index values are unchanged by sorting. |
| `unreadableRecordPendingIsNotDemandPending_data/()` | Rows "locked without sharing" and "no read permission" (`UnreadableFile::Mechanism`). |
| `unreadableRecordPendingIsNotDemandPending` | `G_OUT` enabled and filled.<br>Set-up, as `tst_result_columns::workerSkipsUnreadableRecord` (`:700-723`): destroy the UI and the demand layer; `resetModel`; remove `s1`'s `G_OUT` value from `index.json`; make the record unreadable (`QSKIP` on `skipReason()`); `restart()`, i.e. reopen, `populateFromIndex`, a new executor, demand layer and UI.<br>After `startColumnWorker` + `waitForIdle`:<br>• `rowAt(s1).pendingColumns` contains the section, the demand layer does not (`!isCellPending`), and `showsPending` is false;<br>• the cell's pixels equal the base delegate's (empty, as today);<br>• `col(G_OUT).isWorking()` is false and no job is offered for `s1` (`Quiet`). |
| `columnStateChangeRepaintsOnlyThatColumn` | `G_OUT` enabled (idle). `RegionRecorder` on the tree and header viewports; spies on the model's `dataChanged`, `modelReset`, `layoutChanged` and `headerDataChanged`. `emit m_demand->columnStateChanged(colId("G_OUT"))`:<br>• `QTRY_VERIFY` the recorded tree region is inside the column's viewport rect (`columnViewportPosition`, `columnWidth`), and the header region is inside `sectionRect`;<br>• no model signal fires;<br>• an unknown id is harmless. |
| `survivesDemandDestroyedFirst` | Gate held, column working: `m_demand.reset()`. Then the header equals the reference, every cell equals the base delegate, `indicatorRect` is null, `toolTipForSection` is empty, the clock is inactive, a sort by `G_OUT` works, and help events show nothing. |

**Acceptance Criteria:**
- [ ] `tst_logbook_indicators` is registered under the widget option with the labels `core;widgets`, and all 15 functions pass.
- [ ] Spec §13 "working indicator and hover detail reflect waiting, running, done and failed counts on ... column headers; no refresh or cancel control exists" is covered by:
  - `workingColumnShowsIndicatorRightOfText`
  - `indicatorAnimatesOnlyWhileWorking`
  - `badgeReplacesIndicatorWhenFinished`
  - `failedLoadSessionShowsBadgeNotPending`
  - `headerToolTipFollowsDemandState`
  - `clickOnIndicatorIsAClickOnTheSection`
  - `pendingCellBecomesValueWhenRecordIsWritten`, stub row: no cancel button
- [ ] Spec §13 "a pending column cell is distinguishable from an unavailable one, is not written to the logbook index, and becomes the value when the record is written" is covered by:
  - `pendingCellsAreDistinctFromUnavailable`
  - `pendingCellBecomesValueWhenRecordIsWritten`
  - `sortingTreatsPendingAsUnavailable`
  - `unreadableRecordPendingIsNotDemandPending`

**Complexity:** L

---

## Testing Requirements

### Unit Tests
- **New:**
  - `tests/tst_logbook_indicators.cpp`: 15 functions (plus two `_data`) (Task 3.8);
  - `tst_plot_row_delegate`: `workingAnimationClock`, `workingIndicatorAnimatesOnlyWhileWorking`, `badgeReplacesIndicatorOnceFinished` and `hoverDetailFollowsDemandState` (Task 3.7);
  - `tst_calculation_demand`: `workingIdsFollowStates` and `toolTipListsAtMostTenFailures` (Task 3.2).
- **Changed:**
  - `tst_plot_row_delegate`: `buildUi` freezes the clock, and `survivesDemandDestroyedFirst` also checks the clock;
  - `tests/CMakeLists.txt`: DemandIndicator sources for the delegate test, the new registration, and the header comments.
- **Unchanged, and must pass:** every other suite, in particular `tst_calculation_demand`'s Phase 1 and Phase 2 functions (the tooltip strings are unchanged below the cap), `tst_plot_row_layout`, `tst_column_cache`, `tst_result_columns`, `tst_logbook_index`, `tst_fusion_rows` and `tst_fusion_store`.

### Integration Tests
- The full `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure` (core, fusion, exact, python, audit, and the widget label): all green.
- `audit_cleanup` passes with no rule edited, and the acceptance map is untouched. Phase 4 adds items 501+ and may cite the functions named above.

### Manual Verification
1. **M-P3-1, plot row.** Show a session with IMU data and check "roll". The row shows a turning arc (about one turn per second) and "0 of 1" at the right of the name. Hovering the row or the arc shows "Computing: 0 of 1 done" and "<name> - Sensor fusion: <progress>". When the fit ends, the arc stops and the row is plain.
2. **M-P3-2, column header.** Enable a "roll @ exit" logbook column on a logbook with stubs. Its header shows the turning arc at the right of the (centred) name, and its hover shows the done / wanted count and the running session.
   - Sort by the column: the arc sits left of the sort arrow.
   - Drag the column elsewhere: the arc moves with it.
   - Narrow the column: the name elides and the arc stays.
   - Double-click the section handle: the section widens to fit name and arc.
3. **M-P3-3, pending cells.** During the fill, cells of sessions still to compute show a grey "…". Sessions without IMU data are blank. Values replace the "…" row by row. Sorting by the column puts "…" and blank cells together at the bottom. `index.json` in the logbook folder contains no "…".
4. **M-P3-4, badge.** With a session whose fit is rejected, the column header shows the amber badge once the fill is done, and its hover lists "<name> - <reason>". The plot row shows the badge and the count, as in Phase 1.
5. **M-P3-5, themes.** In Windows dark mode and light mode, the arc (header text colour), the badge and the "…" (placeholder colour) are all readable, including on a selected row.
6. **M-P3-6, no controls, no idle work.**
   - Nowhere (plot rows, headers, context menus, the Plots menu, the progress line during a column fill, which reads "Computing columns: k / n" without a cancel button) is there a refresh or cancel for a calculation.
   - Clicking the arc only selects the row or sorts the column.
   - With nothing computing, a process monitor shows the application idle (no 12.5 Hz repaint).

## Notes for Implementer

### Gotchas
- **Audit text rules match comments in `src/ui`.**
  - `no jobs window, no view of the queue` allows `[Jj]ob[Qq]ueue|JobModel` only in `AppContext.h`. Write "the executor", never its class name, in `DemandIndicator.*`, `LogbookHeaderView.*`, `LogbookCellDelegate.*`, `LogbookView.*` and `PlotRowDelegate.*`.
  - `QThread`, `QEventLoop` and `processEvents` must not appear anywhere in `src`. Tests may use `QApplication::processEvents()`.
- **Set the header before the model.** `setHeader()` after `setModel()` would still work, but the header must also exist before `setSortingEnabled(true)` and before `header()->setFixedHeight`. `setupView` must create it first. A header built with `new QHeaderView(Qt::Horizontal)` alone would lose QTreeView's movable sections, last-section stretch and default alignment: set all three in the constructor.
- **Logical, never visual.** `paintSection`, `updateSection`, `sectionViewportPosition`, `logicalIndexAt` and `isSectionHidden` all take or give logical indices, and `SessionModel` column index = logical index. `visualIndex` is only for tests that move sections.
- **Never cache a section → column id mapping.** `SessionModel::sort()` and `rebuildColumns()` reset the model. Compute `columnIdOf(l)` per call; it is a short string build.
- **`CE_HeaderLabel` elides the whole "name\n(unit)" string against one width.** For non-plain sections, elide per line yourself and set `textElideMode = Qt::ElideNone`. Plain sections go through `QHeaderView::paintSection` untouched, so today's look is kept exactly.
- **Freeze the clocks in pixel tests.** An unfrozen clock can tick between two grabs and make comparisons flaky. Frame 0 is the static glyph that Phase 1's tests expect.
- **Views only read.** No `flush()`, `runLoadStep()` or other test seam from `src/ui`. Painting must not emit or mutate. `plotState`, `columnState`, `isCellPending` and `working*Ids` are pure reads.
- **Value wins.** A loaded row whose record was just written reads its value live (the publication's `dataChanged`) before the demand layer's next pass drops the cell from pending. `showsPending` checks the model's value first, so the cell never shows "…" over a value.
- **`isCellPending(int row, int column)` is in `SessionModel` coordinates.** The logbook has no proxy model. If one is ever added, map through it; do not add one in this phase.
- **Tooltips return `true`** from `viewportEvent` / `helpEvent` once shown. Otherwise the base class shows the model's `ToolTipRole` (none) and hides ours. `QToolTip` treats text that looks like HTML as rich text. The texts here are plain session names and reasons, as in Phase 1.
- **After the executor's shutdown** (`closeEvent`), demand states stay waiting until `MainWindow` deletes the demand layer a moment later. The clocks tick until then, which is harmless. The views turn plain on the demand layer's `destroyed`.
- **A hidden dock** still receives frame repaints while something works. `update()` of a hidden widget is a no-op, and the timer costs nothing measurable. Do not add visibility tracking.
- **Colours.**
  - Plot rows use `Text` / `HighlightedText`, as Phase 1 does.
  - Headers use `ButtonText`, the role `CE_HeaderLabel` draws with (`qcommonstyle.cpp:1681-1682`).
  - Pending cells use `PlaceholderText`, and `HighlightedText` at 60 % alpha on a selected row.
  - The badge is fixed amber.

  Never hard-code a colour for text-like marks.
- `QStyleOptionHeaderV2` and `initStyleOptionForIndex` are Qt 6 API (`qheaderview.h:210`). The project uses Qt 6.9.3.

### Decisions Made
- **Shared painting lives in `src/ui/docks/DemandIndicator.{h,cpp}`:** `drawWorkingGlyph`, `drawWarningGlyph` and `WorkingAnimation`, moved out of `PlotRowDelegate.cpp`. They are QtCore and QtGui only, so both views use them and the demand layer stays widget-free. The file is not added to the `widget-free-core` audit list, because no existing rule breaks. Phase 4 may add it (recommended) to the new `demand` group.
- **Animation:** a 270° arc turned clockwise by 30° every 80 ms, one turn in 0.96 s. There is one `WorkingAnimation` per view: the plot delegate's follows `workingPlotIds()`, and the header's follows `workingColumnIds()`. It is active only while its list is non-empty. Stopping resets it to frame 0, which is Phase 1's static glyph. Each frame repaints only the working rows or sections. A `setFrozen` / `advance` seam makes tests deterministic.
- **Refinement of Phase 1/2: `workingPlotIds()` / `workingColumnIds()`** on `CalculationDemand`. They are read-only and have no signal (`statesChanged` says when to ask). A view cannot enumerate plot or column states itself.
- **Refinement of Phase 1: tooltip lists are capped at 10 tracks per section** (`kToolTipListLimit`), with a final "and N more" line. A column over thousands of sessions could otherwise produce a tooltip taller than the screen. The cap lives in the demand layer, so plot rows and headers still say the same thing.
- **A failed-load session needs no demand-layer change here.** Phase 2 (Task 2.2 rule 4.6, test `visibleFailedLoadIsSettledAsFailed`) settles every failed-load placeholder, visible or hidden, as `Failed` ("The session file could not be loaded"), so it is never pending and never keeps a column working. Phase 3 only checks the presentation: the header's badge and a cell without "…" (`failedLoadSessionShowsBadgeNotPending`).
- **Header visuals:**
  - glyph only: the arc while working, the amber badge after failures;
  - no "k of n" and no failed count in the header, whose two-line, 100 px sections have no room. The hover carries the numbers;
  - the glyph sits immediately right of the centred text, inside `SE_HeaderLabel`, which excludes the sort arrow;
  - the text keeps a symmetric reserve and is elided per line;
  - glyph side: one text line;
  - `sectionSizeFromContents` adds the reserve for requested columns.
- **Hover:**
  - the whole header section (glyph included) shows `DemandState::toolTip`, like the whole plot row;
  - pending cells get their own fixed tooltip, "Pending: this value is being computed";
  - the per-session detail lives in the header's hover, not in thousands of cells.
- **Pending cells:**
  - a static, muted `"…"` (U+2026, `PlaceholderText`), drawn by a view-side `LogbookCellDelegate` that asks `CalculationDemand::isCellPending(row, column)`;
  - a model value always wins;
  - the unreadable-record pending state keeps today's empty look, so the three looks are value, empty and "…";
  - repaint is one column rect per `columnStateChanged`, with no model signal.
- **`LogbookView`'s constructor takes the demand layer** (`LogbookView(SessionModel*, CalculationDemand*, QWidget*)`). There is one caller, `LogbookDockFeature`, and `nullptr` gives today's view.
- **No mouse handling** in the header view or the cell delegate. The glyphs are not controls (spec §10: no refresh, no cancel).
- Accessibility: tooltips carry the detail, and the model's `DisplayRole` stays empty for pending cells. Screen readers read "empty", as for "unavailable". Exposing pending to accessibility would require the model to know it, which the overview forbids.

### Open Questions
- None blocking. **For Phase 4**, when writing the docs and the `demand` audit group:
  - (a) `docs/COMPUTED_PLOTS.md` should describe the arc, "k of n", the badge and the hover as built here;
  - (b) `docs/DATA_SCHEMA.md` §11 should say that a pending cell is a view-side presentation, not a cached state;
  - (c) consider audit rules that `src/ui` calls only the demand layer's read API (no `flush(`, `runLoadStep(`, `endInputSettleWaits(`), and that `DemandIndicator.*` stays widget-free;
  - (d) `tests/README.md`'s test table and the "only test that links Qt Widgets" wording need `tst_logbook_indicators`.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria.
2. All tests pass under `build-phase1`: core, fusion, exact, python, audit and widgets.
3. Code follows the patterns of the reference files:
   - `PlotRowDelegate`'s weak demand pointer, base-class fallback and pixel-identical plain rows;
   - `QHeaderView::paintSection`'s option set-up;
   - the demand layer's read-only queries.
4. No TODOs or placeholder code remain. No product code outside the two logbook view classes and `PlotRowDelegate` paints demand state. `SessionModel`, `index.json` and `sort()` carry no trace of "pending".
