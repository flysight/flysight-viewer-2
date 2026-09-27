# Phase 4: What the user sees

## Overview

This phase makes the two working indicators identical and gives them one
clock (spec §12, §13 "The views"). A plot row and a logbook column header now
show the same single glyph: the turning arc while any of the source's demand
is waiting or running, the warning badge once the work is finished with
failures, and nothing else. The plot row loses its "k of n" label and the
count beside the badge, and the hover carries the numbers.

One `WorkingAnimation` is created in `MainWindow` beside the demand layer.
`followDemand()` makes it follow the demand layer, and it reaches the views
through `AppContext`. The views stop creating clocks of their own.

The glyph plumbing is written once, and no boundary moves (spec §1):

- **`DemandIndicator`** (QtCore + QtGui, below the demand layer, as the audit
  keeps it) holds the glyphs, their side and spacing for a text line, and
  the clock's mechanics.
- **A new `DemandIndicatorView`** holds the widget-bound, demand-aware part:
  the glyph's colour for a style option, the painting of one state's glyph,
  the tooltip display, the repaint when the demand layer is destroyed, and
  the clock's following of the demand layer.

The logbook's progress line gets its own text for the fill: "Computing
results: k / n".

## Dependencies

- **Depends on:** Phase 3 (One walk and one pair memory), and through it
  Phases 1-2.
- **Blocks:** Phase 5 (Documentation, acceptance map and audit).
- **Assumptions:**
  - Phases 1-3 are committed and tagged
    (`plan/calculation-refinements/phase-3-done`). The full suite and
    `audit_cleanup` pass there.
  - As Phase 2 leaves the code:
    - The presentation values are in `src/demandstate.{h,cpp}`:
      `DemandCondition`; `DemandTrack` without `settling` or `job`; and
      `DemandState` with `addTrack()`, `finish()`, `static buildToolTip()`,
      `kToolTipListLimit`, no `waiting` list, and `Q_DECLARE_TR_FUNCTIONS`
      as the last line of the struct body.
    - `PlotRowDelegate.h` and `LogbookHeaderView.h` include `demandstate.h`
      and forward-declare `CalculationDemand`. Their `.cpp` files include
      `calculationdemand.h`.
    - The audit defines `DEMAND_LAYER` / `DEMAND_FILES`. The rule "nothing
      below the demand layer includes it" matches
      `#include [\"<](\.\./)*${DEMAND_FILES}\.h` and still lists
      `"src/ui/docks/DemandIndicator.*"` and `PlotRowLayout.h` in its
      pathspec.
  - Phase 3 leaves the demand layer's read interface unchanged:
    - `plotState`, `columnState`, both `isCellPending`, `workingPlotIds`,
      `workingColumnIds`, `columnId`;
    - the signals `plotStateChanged`, `columnStateChanged` and
      `statesChanged`.

    Phase 3 also makes a record write that failed into a Failed track, with
    the store's reason, on plot rows and column headers, whether the session
    is loaded or not. This phase changes no demand-layer decision.
  - Line numbers are those of `ec6bd43`, and Phases 1-3 have shifted some of
    them. Find every passage by its name or its quoted text.

## Tasks

### Task 4.1: Shared glyph plumbing: the core in `DemandIndicator`, the views' part in `DemandIndicatorView`

**Purpose:** Write once what both views compute today: the glyph's colour,
its size and spacing, the painting of the one glyph, the tooltip display, the
repaint when the demand layer is destroyed, and the clock's following of the
demand layer. No boundary moves (spec §1), so the plumbing is split in two:
- **`DemandIndicator` (unchanged boundary).** The part that needs neither
  widgets nor the demand layer stays here: the glyphs, the glyph metrics and
  the clock's mechanics. It stays QtCore + QtGui and below the demand layer,
  as the audit keeps it.
- **`DemandIndicatorView` (new).** The part that needs widgets or the demand
  layer goes into this file, and both views use it.

**Files to modify:**
- `src/ui/docks/DemandIndicator.h`, `.cpp`: add `GlyphMetrics` /
  `glyphMetrics`, and rewrite the file comment and the `WorkingAnimation`
  class comment. Nothing else changes: no other API change, no widget
  header, and no demand-layer type or header.
- `src/CMakeLists.txt` (~420, the UI source list): after
  `ui/docks/DemandIndicator.h    ui/docks/DemandIndicator.cpp`, add
  `ui/docks/DemandIndicatorView.h    ui/docks/DemandIndicatorView.cpp`.

**Files to create:**
- `src/ui/docks/DemandIndicatorView.h`, `src/ui/docks/DemandIndicatorView.cpp`:
  `glyphColor` (two overloads), `drawDemandGlyph`, `showIndicatorToolTip`,
  `repaintWhenDemandDestroyed` and `followDemand`.

**Technical Approach:**

Today the same plumbing exists twice.

`PlotRowDelegate.cpp` (anonymous namespace, ~16-24):
```cpp
QColor glyphColor(const QStyleOptionViewItem &opt)
{
    QPalette::ColorGroup group = (opt.state & QStyle::State_Enabled) ? QPalette::Normal : QPalette::Disabled;
    if (group == QPalette::Normal && !(opt.state & QStyle::State_Active))
        group = QPalette::Inactive;
    const bool selected = opt.state & QStyle::State_Selected;
    return opt.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text);
}
```

`LogbookHeaderView.cpp` (~20-45) has the same colour-group rule with
`QPalette::ButtonText`, and the metrics:
```cpp
GlyphMetrics glyphMetrics(int labelHeight, int lineHeight)
{
    GlyphMetrics metrics;
    metrics.side = qMin(labelHeight, lineHeight);
    metrics.spacing = qMax(2, metrics.side / 4);
    metrics.reserve = metrics.side + metrics.spacing;
    return metrics;
}
```

`PlotRowDelegate::metricsFor` (~66-73) repeats the metrics rule
(`qMin(opt.rect.height() - 2, opt.fontMetrics.height())`, `qMax(2, side / 4)`).
Each view connects `&QObject::destroyed` to its own repaint, calls
`QToolTip::showText` itself, chooses between `drawWorkingGlyph` and
`drawWarningGlyph` itself, and drives its own clock from `statesChanged`
(`syncAnimation()`).

**1. `DemandIndicator.h` / `.cpp` (the core).** Forward-declare
`QFontMetrics` in the header, and include `<QFontMetrics>` (QtGui) in the
`.cpp`. Add:
```cpp
/// The glyph's side and the room it takes beside text, for one text line of
/// `metrics` in `room` pixels of height: side = min(room, line height),
/// spacing = max(2, side / 4). The plot rows and the column headers use it alike.
struct GlyphMetrics {
    int side = 0;
    int spacing = 0;
    int reserve() const { return side + spacing; }   ///< side and the spacing beside it
};
GlyphMetrics glyphMetrics(const QFontMetrics &metrics, int room);
```
The body is `side = qMin(room, metrics.height()); spacing = qMax(2, side / 4);`.

`WorkingAnimation` keeps its whole API unchanged: `kFrameIntervalMs`,
`kFramesPerTurn`, `setActive`, `isActive`, `isTicking`, `frame`, `angle`,
`setFrozen`, `advance` and `frameAdvanced`. It gains no member and still
knows nothing of the demand layer.

Comments:
- **File comment** (lines 4-7). Keep "QtCore and QtGui only" and the
  existing first sentence, and add: "the glyph's size beside a line of text
  (glyphMetrics); no demand-layer type: what a view shows, its colour and
  hover, and how the one clock follows the demand layer are
  DemandIndicatorView.h's".
- **`WorkingAnimation` class comment** ("The clock of the working indicator:
  one per view …") becomes: "The clock of the working indicator: one per
  application, created beside the demand layer and handed to the views that
  show the indicator. It ticks only while something is working
  (setActive(true); DemandIndicatorView.h's followDemand() decides that),
  and it restarts at frame 0. It never repaints anything itself: each view
  connects frameAdvanced() to its own repaint of what is working, and reads
  angle() when it paints."
- Say "the demand layer" in these comments, never `CalculationDemand`. The
  audit rule "only the application and its views know the demand layer"
  matches comments too.

**2. `DemandIndicatorView.h` (new, the views' part).** Namespace `FlySight`.
Include `<QtGlobal>` only, and forward-declare `QColor`, `QHelpEvent`,
`QPainter`, `QRect`, `QRectF`, `QStyleOptionHeader`, `QStyleOptionViewItem`
and `QWidget`. Inside the namespace, forward-declare
`class CalculationDemand;`, `struct DemandState;` and
`class WorkingAnimation;`. `DemandState` is declared `struct` in
`demandstate.h`, and MSVC warns (C4099) on a mismatch. The file has no
`Q_OBJECT`, so AUTOMOC needs nothing.

The file comment reads: "The views' half of the one indicator
(DemandIndicator.h is the core). It paints a DemandState's one glyph, takes
the glyph's colour from the style option, shows the state's tooltip,
repaints when the demand layer goes, and makes the application's one clock
follow the demand layer. It uses Qt Widgets and the demand layer's read
interface, and it decides nothing: what is shown is DemandState's
(demandstate.h)."

```cpp
/// The colour the working indicator is drawn in: the text colour of what the
/// style paints, in the colour group it paints it in (Disabled when not
/// enabled, else Inactive when the window is not active, else Normal).
/// An item view's row: HighlightedText when selected, else Text.
QColor glyphColor(const QStyleOptionViewItem &option);
/// A header section: ButtonText (CE_HeaderLabel's colour).
QColor glyphColor(const QStyleOptionHeader &option);

/// The one indicator of a state in `rect`: the working indicator in `color`
/// at the clock's angle while state.isWorking() (at rest, rotation 0, without
/// a clock), else the warning badge while state.showsWarning(), else nothing.
/// Leaves the painter's state as it found it.
void drawDemandGlyph(QPainter *painter, const QRectF &rect, const DemandState &state,
                     const QColor &color, const WorkingAnimation *clock);

/// Hover detail: shows state.toolTip (DemandState::buildToolTip) at the
/// event's global position over `area` of `widget`, where it hides as soon
/// as the pointer leaves `area`, and returns true. Returns false and shows
/// nothing for a null event or a plain state (empty toolTip): the caller
/// then hands the event to its base class, which keeps any model tooltip.
bool showIndicatorToolTip(const QHelpEvent *event, const DemandState &state, QWidget *widget, const QRect &area);

/// When `demand` is destroyed, `widget` is repainted: from then on every
/// state it shows is plain. `widget` is the connection's context, so nothing
/// happens once it is gone. Nothing for a null `demand` or `widget`.
void repaintWhenDemandDestroyed(CalculationDemand *demand, QWidget *widget);

/// Makes `clock` follow `demand` (held weakly): active exactly while
/// workingPlotIds() or workingColumnIds() is not empty, asked again on every
/// statesChanged(); inactive (frame 0) once `demand` is destroyed, and at
/// once for a null `demand`. `clock` is the connections' context. Call once
/// per clock: the application calls it beside the demand layer, and tests
/// call it for the clock they give the views.
void followDemand(WorkingAnimation *clock, CalculationDemand *demand);
```

**3. `DemandIndicatorView.cpp`.** Include `<QHelpEvent>`, `<QPainter>`,
`<QPalette>`, `<QPointer>`, `<QStyle>`, `<QStyleOptionHeader>`,
`<QStyleOptionViewItem>`, `<QToolTip>`, `<QWidget>`, `"calculationdemand.h"`,
`"demandstate.h"` and `"ui/docks/DemandIndicator.h"`. The bodies move from
`PlotRowDelegate.cpp` and `LogbookHeaderView.cpp`:
- **Colour.** One anonymous-namespace helper,
  `QPalette::ColorGroup colorGroup(const QStyleOption &)`, holds the group
  rule (the header's form: Disabled, else Inactive, else Normal, which equals
  the plot row's form). The two `glyphColor` overloads call it and pick the
  role. The results must be pixel-identical to today's.
- **`drawDemandGlyph`:**
  - `if (state.isWorking())`: `drawWorkingGlyph(painter, rect, color, clock ? clock->angle() : 0.0)`.
  - `else if (state.showsWarning())`: `drawWarningGlyph(painter, rect)`.
  - Antialiasing and clipping stay the caller's, as today.
- **`showIndicatorToolTip`:**
  `if (!event || !widget || state.toolTip.isEmpty()) return false; QToolTip::showText(event->globalPos(), state.toolTip, widget, area); return true;`.
- **`repaintWhenDemandDestroyed`:**
  `if (demand && widget) QObject::connect(demand, &QObject::destroyed, widget, [widget] { widget->update(); });`.
- **`followDemand`:**
  ```cpp
  if (!clock)
      return;
  const QPointer<CalculationDemand> weak(demand);
  const auto sync = [clock, weak] {
      clock->setActive(weak && (!weak->workingPlotIds().isEmpty() || !weak->workingColumnIds().isEmpty()));
  };
  if (demand) {
      QObject::connect(demand, &CalculationDemand::statesChanged, clock, sync);
      QObject::connect(demand, &QObject::destroyed, clock, sync);
  }
  sync();
  ```
  During `destroyed` the `QPointer` is already null, so the clock stops at
  frame 0. Both connections have `clock` as their context, so they end with
  the clock.

**Acceptance Criteria:**
- [ ] `DemandIndicator.*` stays inside its boundary. It includes no widget header and no demand-layer header, and it names no `CalculationDemand`. The audit rules "the logic components see no widget" and "nothing below the demand layer includes it" pass for it unedited.
- [ ] `DemandIndicator.h` adds only `GlyphMetrics` / `glyphMetrics`. `WorkingAnimation`'s API and timing are unchanged, and `workingAnimationClock` passes unchanged in what it asserts.
- [ ] `DemandIndicatorView.h` declares both `glyphColor` overloads, `drawDemandGlyph`, `showIndicatorToolTip`, `repaintWhenDemandDestroyed` and `followDemand`, and includes no header of `src/`.
- [ ] A clock passed to `followDemand` is active exactly while `workingPlotIds()` or `workingColumnIds()` is not empty. It is inactive at frame 0 once the demand layer is destroyed, and it is inactive after `followDemand(clock, nullptr)`.
- [ ] `glyphMetrics` and `glyphColor` return what the removed per-view helpers returned for the same inputs (test `sharedGlyphPlumbing`).

**Complexity:** M

---

### Task 4.2: One clock per application, handed to the views through `AppContext`

**Purpose:** Create the one clock beside the demand layer, with the same
lifetime discipline, and hand it to the views the way the demand layer is
handed to them (spec §12 "One clock").

**Files to modify:**
- `src/ui/docks/AppContext.h`: forward-declare `WorkingAnimation`, add the field.
- `src/mainwindow.h`: forward declaration and member.
- `src/mainwindow.cpp`: construction, `AppContext` filling, destruction.
- `src/ui/docks/plotselection/PlotSelectionDockFeature.cpp`: pass the clock to the delegate, and update the comment.
- `src/ui/docks/logbook/LogbookDockFeature.cpp`: pass the clock to `LogbookView`, and update the comment.

**Technical Approach:**

1. **`AppContext.h`.** Add `class WorkingAnimation;` beside
   `class CalculationDemand;`. After `calculationDemand` add:
   ```cpp
   WorkingAnimation* workingClock = nullptr;  // the working indicator's one clock (DemandIndicator.h); made to follow the demand layer (followDemand, DemandIndicatorView.h) (may be null: the indicator does not turn)
   ```
   The file comment "All pointers are non-owning; lifetime is managed by
   MainWindow" already covers it.
2. **`mainwindow.h`.** Add `class WorkingAnimation;` to the forward
   declarations. Add a member after `m_calculationDemand`:
   `WorkingAnimation *m_workingClock = nullptr;  // the working indicator's one clock: follows the demand layer; destroyed right after it`.
3. **`mainwindow.cpp`, construction.** Add `#include "ui/docks/DemandIndicator.h"`
   and `#include "ui/docks/DemandIndicatorView.h"`. Right after
   `m_calculationDemand = new CalculationDemand(model, m_plotModel, m_jobQueue, this);`
   (~215) add:
   ```cpp
   m_workingClock = new WorkingAnimation(this);
   followDemand(m_workingClock, m_calculationDemand);
   ```
   Add one sentence to the comment above (~205-213): "The working
   indicator's one clock follows the demand layer, so the plot list's rows
   and the logbook's column headers turn in step." In the `AppContext`
   block, add `ctx.workingClock = m_workingClock;` after
   `ctx.calculationDemand = m_calculationDemand;`.
4. **`mainwindow.cpp`, destruction** (`~MainWindow`, ~342-355). After
   `delete m_calculationDemand; m_calculationDemand = nullptr;`, add
   `delete m_workingClock; m_workingClock = nullptr;`, before the executor.
   Extend the comment:

   > The clock follows the demand layer and stops when it goes; it is deleted
   > next, and the views hold it weakly, as they hold the demand layer.

   The order is deterministic. The demand layer's `destroyed` stops the clock
   and repaints the views, the clock is deleted, then the executor, then
   Qt's children (docks included). The views' `QPointer`s make a later paint
   safe.
5. **`PlotSelectionDockFeature.cpp`** (~76-80). Use
   `new PlotRowDelegate(ctx.calculationDemand, ctx.workingClock, m_treeView)`.
   The comment becomes: "Rows of plots over requested calculations show one
   glyph: the turning working indicator while their demand is computed, or a
   warning badge for sessions that could not be computed. Hovering a row
   shows the counts and the detail. The indicator turns with the
   application's one clock (null demand layer: plain rows)."
6. **`LogbookDockFeature.cpp`** (~16-17). Use
   `new LogbookView(ctx.sessionModel, ctx.calculationDemand, ctx.workingClock, m_dock)`.
   The comment gains "and the one clock that turns the headers' indicators".

**Acceptance Criteria:**
- [ ] `grep -rn "new WorkingAnimation\|make_unique<WorkingAnimation>" src` finds exactly one line, in `src/mainwindow.cpp`.
- [ ] `AppContext` has `workingClock`. Both dock features pass it, and neither dock feature nor any view creates a `WorkingAnimation`.
- [ ] `~MainWindow` deletes the clock right after the demand layer and before the executor.

**Complexity:** S

---

### Task 4.3: The plot row shows one glyph and nothing else

**Purpose:** Remove the "k of n" label and the failed count. The row layout
keeps room for one glyph and nothing more. The delegate uses the shared
plumbing and the clock it is given (spec §12 "One indicator").

**Files to modify:**
- `src/ui/docks/plotselection/PlotRowLayout.h`: rewrite to one glyph.
- `src/ui/docks/plotselection/PlotRowDelegate.h`, `.cpp`: constructor, members, painting, tooltip, repaint, class comment.

**Technical Approach:**

**1. `PlotRowLayout.h`.** Today `layoutPlotRow(itemRect, metrics,
showsWarning, warningCountWidth, showsIndicator, progressLabelWidth,
direction)` lays out `warningIcon`, `warningCount`, `progressLabel` and
`indicatorIcon`. Replace it with:
```cpp
struct PlotRowMetrics {
    int iconSide = 0;       ///< side of the square glyph
    int spacing = 0;        ///< between the (elided) name and the glyph
    int rightMargin = 0;    ///< between the glyph and the item rect's edge
};

struct PlotRowGeometry {
    QRect glyph;            ///< null when nothing is shown
    /// From the glyph's outer edge to the item rect's right edge (left edge,
    /// in right-to-left): what the name gives way to. 0 when nothing is shown.
    int reservedWidth = 0;
};

/// One square glyph, right-aligned, vertically centred; mirrored as a whole in
/// a right-to-left layout. Rects use Qt's conventions.
inline PlotRowGeometry layoutPlotRow(const QRect &itemRect, const PlotRowMetrics &metrics,
                                     bool showsGlyph, Qt::LayoutDirection direction);
```
The body keeps today's arithmetic for the indicator alone:
- `edge = itemRect.x() + itemRect.width()`;
- `iconTop = itemRect.y() + (itemRect.height() - metrics.iconSide) / 2`;
- `glyph = QRect(edge - metrics.rightMargin - metrics.iconSide, iconTop, iconSide, iconSide)`;
- `reservedWidth = metrics.rightMargin + metrics.iconSide`;
- in right-to-left, `glyph.moveLeft(itemRect.left() + itemRect.right() - glyph.right())`.

The file comment's diagram becomes
`[check] plot name (elided) ... [glyph] |`. The text says that the glyph is
the working indicator or the warning badge (which one is DemandState's,
demandstate.h, and it is painted by `DemandIndicatorView.h`'s `drawDemandGlyph`),
that there is no label and no count, and that nothing in it is clickable.

**2. `PlotRowDelegate.h`.**
- Constructor: `PlotRowDelegate(CalculationDemand *demand, WorkingAnimation *clock, QAbstractItemView *view);`
  with the doc: "`clock` is the application's working-indicator clock (may
  be null: the arc is drawn at rest); `view` is the parent, and the view
  whose rows are repainted".
- `clusterRect(index)` becomes
  `QRect indicatorRect(const QModelIndex &index) const;`: "Where the row's
  one glyph is painted, in viewport coordinates; null for plain rows. For
  tests and for nothing else." The name matches `LogbookHeaderView::indicatorRect`.
- `animation()` becomes `WorkingAnimation *animation() const;`: "The clock
  it was given (null when none, or once it is destroyed). For tests and for
  nothing else." Define it in the `.cpp` or keep it inline. The header
  already includes `DemandIndicator.h`, so `QPointer::data()` sees the
  complete type.
- Members: `QPointer<WorkingAnimation> m_clock;` replaces
  `WorkingAnimation *m_animation;`. Delete the `syncAnimation()` slot. Keep
  `onPlotStateChanged` and `onAnimationFrame`.
- Class comment:
  - **PAINTING**: "Right-aligned in a plot row (PlotRowLayout.h), one glyph
    (DemandIndicatorView.h, drawn with DemandIndicator.h's glyphs): the working indicator, an arc that turns about
    once a second, while any of the plot's demand is waiting or running;
    once the work is finished and some sessions could not be computed, the
    warning badge instead. There is no label and no count: the hover carries
    the numbers." Keep the plain-row, elision, row-height and colour
    sentences, adjusted to "the glyph".
  - **TOOLTIP**: unchanged in substance, using `showIndicatorToolTip()`.
  - **REPAINT**: "plotStateChanged(plotId) repaints that row. While any plot
    is working, each frame of the application's working-indicator clock
    (given at construction; the application makes it follow the demand
    layer) repaints the working rows (workingPlotIds()). Without a clock the arc is drawn at
    rest and nothing repaints by itself."

**3. `PlotRowDelegate.cpp`.**
- Add `#include "ui/docks/DemandIndicatorView.h"`. `glyphMetrics` comes from
  `DemandIndicator.h`, which the header already includes.
- Delete the anonymous-namespace `glyphColor`.
- **Constructor.**
  ```cpp
  : QStyledItemDelegate(view), m_demand(demand), m_view(view), m_clock(clock)
  {
      if (m_demand && m_view) {
          connect(m_demand, &CalculationDemand::plotStateChanged, this, &PlotRowDelegate::onPlotStateChanged);
          // Without the demand layer every row is plain at once
          repaintWhenDemandDestroyed(m_demand, m_view->viewport());
          if (m_clock)
              connect(m_clock, &WorkingAnimation::frameAdvanced, this, &PlotRowDelegate::onAnimationFrame);
      }
  }
  ```
  There is no `statesChanged` connection and no `syncAnimation()`: the
  clock was made to follow the demand layer by `followDemand()` where it was
  created.
- **`metricsFor(opt)`**:
  `const GlyphMetrics glyph = glyphMetrics(opt.fontMetrics, opt.rect.height() - 2);`
  then `metrics.iconSide = glyph.side; metrics.spacing = glyph.spacing; metrics.rightMargin = glyph.spacing;`.
- **`geometryFor(opt, state)`**:
  `return layoutPlotRow(opt.rect, metricsFor(opt), !state.isPlain(), opt.direction);`.
  Delete the label and count widths and the comment about exclusivity. That
  comment moves to `drawDemandGlyph`'s doc in substance.
- **`indicatorRect(index)`**: as `clusterRect` is today, returning
  `geometryFor(opt, state).glyph`.
- **`paint`**: keep the plain branch, the elision
  (`available = textRect.width() - (geometry.reservedWidth + metrics.spacing) - 2 * textMargin`),
  and `drawControl`. Then
  `painter->save(); painter->setClipRect(opt.rect); painter->setRenderHint(QPainter::Antialiasing); drawDemandGlyph(painter, QRectF(geometry.glyph), state, glyphColor(opt), m_clock); painter->restore();`.
  Delete `setFont`, `setPen` and both `drawText` calls.
- **`helpEvent`**:
  `if (event && event->type() == QEvent::ToolTip && view && showIndicatorToolTip(event, stateFor(index), view->viewport(), option.rect)) return true; return QStyledItemDelegate::helpEvent(event, view, option, index);`.
  `toolTipFor()` is unchanged (`stateFor(index).toolTip`).
- **`onAnimationFrame`**: `if (!m_demand || !m_view) return;`, then the loop
  over `workingPlotIds()` as today.
- Remove `#include <QToolTip>` and `<QHelpEvent>` if nothing else needs them.

**4. The hover carries both numbers.** No tooltip text changes. Both views
show `DemandState::toolTip`, built by the one `DemandState::buildToolTip()`
(Phase 2). While working it starts "Computing: k of n done", which holds the
label's k and n. Once finished with failures it lists "Could not be
computed:" with each track and reason, and "and N more" beyond ten. The
header does the same today (`headerToolTipFollowsDemandState`). The plot
row's `toolTipFor()` and the header's `toolTipForSection()` both return
`stateFor(...).toolTip`, which is verified identical to
`DemandState::buildToolTip(state)`.

**Acceptance Criteria:**
- [ ] `PlotRowLayout.h` has no `warningIcon`, `warningCount`, `progressLabel` or `indicatorIcon`. `layoutPlotRow` takes `(itemRect, metrics, showsGlyph, direction)` and returns one `glyph` and `reservedWidth`.
- [ ] `PlotRowDelegate.cpp` contains no `drawText`, no `failedCount`, no `progressLabel`, no `QPalette::ColorGroup`, no `QToolTip::`, no `&QObject::destroyed` and no `qMax(2`.
- [ ] A working row and a badged row are pixel-identical to the base delegate everywhere outside `indicatorRect(index)`, for a name that fits (`workingRowPaintsIndicator`, `badgeReplacesIndicatorOnceFinished`).
- [ ] The row's glyph is square, with side `min(row height - 2, line height)`, at `rightMargin = max(2, side / 4)` from the row's right edge.
- [ ] `toolTipFor(index) == plotState(id).toolTip == DemandState::buildToolTip(plotState(id))` for working, failed and plain rows.

**Complexity:** M

---

### Task 4.4: The header and the cells use the shared plumbing and the given clock

**Purpose:** The logbook header stops computing the colour, the metrics, the
tooltip display and the destroyed repaint, and stops owning a clock.
`LogbookView` hands the clock on (spec §12 last paragraph, "One clock").

**Files to modify:**
- `src/ui/docks/logbook/LogbookHeaderView.h`, `.cpp`.
- `src/ui/docks/logbook/LogbookView.h`, `.cpp`: constructor and `setupView`.
- `src/ui/docks/logbook/LogbookCellDelegate.cpp`: the destroyed repaint only.

**Technical Approach:**

**1. `LogbookHeaderView.h`.**
- Constructor:
  `LogbookHeaderView(SessionModel *model, CalculationDemand *demand, WorkingAnimation *clock, QWidget *parent = nullptr);`
  Doc: "... `demand` may be null (then the header is exactly QHeaderView);
  `clock` is the application's working-indicator clock (may be null: the arc
  is drawn at rest)".
- `animation()`: "the clock it was given; tests only". It returns
  `m_clock.data()`, which needs the complete type. Either include
  `"ui/docks/DemandIndicator.h"` in the header (replacing the forward
  declaration `class WorkingAnimation;`) or define `animation()` in the
  `.cpp`.
- Members: `QPointer<WorkingAnimation> m_clock;` replaces
  `WorkingAnimation *m_animation;`. Delete the `syncAnimation()` slot.
- Class comment REPAINT: "... while any column is working, each frame of the
  application's working-indicator clock (given at construction; the
  application makes it follow the demand layer) repaints the working
  sections (workingColumnIds())."
  Keep the rest.

**2. `LogbookHeaderView.cpp`.**
- Add `#include "ui/docks/DemandIndicatorView.h"`. It keeps
  `"ui/docks/DemandIndicator.h"` for `glyphMetrics` and `WorkingAnimation`.
- Delete the anonymous namespace: `glyphColor(const QStyleOptionHeader &)`,
  `struct GlyphMetrics` and `glyphMetrics(int, int)`.
- **Constructor.** Keep the three configuration lines. Then:
  ```cpp
  if (m_demand) {
      connect(m_demand, &CalculationDemand::columnStateChanged, this, &LogbookHeaderView::onColumnStateChanged);
      repaintWhenDemandDestroyed(m_demand, viewport());
      if (m_clock)
          connect(m_clock, &WorkingAnimation::frameAdvanced, this, &LogbookHeaderView::onAnimationFrame);
  }
  ```
  There is no `statesChanged` connection and no `syncAnimation()`.
- **`layoutGlyph`**:
  `const GlyphMetrics metrics = glyphMetrics(opt.fontMetrics, label.height());`.
  Replace `metrics.reserve` with `metrics.reserve()`. The layout is
  otherwise unchanged.
- **`sectionSizeFromContents`**:
  `size.rwidth() += 2 * glyphMetrics(fontMetrics(), fontMetrics().height()).reserve();`.
- **`paintSection`**: replace the `if (state.isWorking()) drawWorkingGlyph(...) else drawWarningGlyph(...)`
  pair with
  `drawDemandGlyph(painter, QRectF(glyph), state, glyphColor(opt), m_clock);`.
  Keep the save, clip, antialiasing and restore around it.
- **`viewportEvent`**: on `QEvent::ToolTip`, compute `logical` as today and
  `if (showIndicatorToolTip(helpEvent, stateFor(logical), viewport(), sectionViewportRect(logical))) return true;`.
  Everything else goes to `QHeaderView::viewportEvent(event)`.
  `toolTipForSection()` is unchanged.
- **`onAnimationFrame`**: `if (!m_demand) return;`, then as today.
- Remove `#include <QToolTip>` if it is unused.

**3. `LogbookView`.**
- Header: forward-declare `class WorkingAnimation;`. The constructor becomes
  `LogbookView(SessionModel *model, CalculationDemand *demand, WorkingAnimation *clock, QWidget *parent = nullptr);`,
  and `setupView(CalculationDemand *demand, WorkingAnimation *clock)`.
  Class comment: "... The demand layer and the working-indicator clock may
  be null: then header and cells are plain (without a clock, the header's
  arc does not turn)."
- `.cpp`: `treeView->setHeader(new LogbookHeaderView(model, demand, clock, treeView));`.
  The comment above `setupView` becomes "The demand layer and the clock are
  handed on, not kept: the header and the delegate hold them weakly."

**4. `LogbookCellDelegate.cpp`** (constructor, ~20-28). Replace the
`connect(m_demand, &QObject::destroyed, this, [this] { if (m_view) m_view->viewport()->update(); });`
with `repaintWhenDemandDestroyed(m_demand, m_view->viewport());` inside the
existing `if (m_demand && m_view)`. Add `#include "ui/docks/DemandIndicatorView.h"`.
Nothing else in the cell delegate changes (pending cells, their colour and
their tooltip are the cell's own presentation, not the glyph's).

**Acceptance Criteria:**
- [ ] `LogbookHeaderView.cpp` contains no `QPalette::ColorGroup`, no `QToolTip::`, no `&QObject::destroyed`, no `qMax(2`, no `drawWorkingGlyph` and no `drawWarningGlyph`. `LogbookCellDelegate.cpp` contains no `&QObject::destroyed`.
- [ ] Header pixels are unchanged: `workingColumnShowsIndicatorRightOfText`, `indicatorClearsSortArrowAndNarrowSections`, `badgeReplacesIndicatorWhenFinished`, `plainHeaderAndCellsAreIdenticalToBase` and `survivesDemandDestroyedFirst` pass unchanged in what they assert.
- [ ] `header()->animation()` is the clock the view was given.

**Complexity:** M

---

### Task 4.5: `DemandState::progressLabel` goes

**Purpose:** The plot row's label was the only reader of `progressLabel`.
What no product code uses is removed (spec §2). The facts it carried are the
counts, which stay.

**Files to modify:**
- `src/demandstate.h`, `src/demandstate.cpp`.
- `tests/tst_calculation_demand.cpp`, `tests/tst_fusion_rows.cpp`, `tests/tst_plot_row_delegate.cpp`: the assertions that read it.

**Technical Approach:**
1. In `demandstate.h`:
   - Delete `QString progressLabel;` and its comment.
   - Delete `&& progressLabel == other.progressLabel` from `operator==`.
     `progressLabel` was a function of `doneCount`, `wantedCount` and
     `isWorking()`, which are all compared, so no announcement changes.
   - The `finish()` doc becomes "Sets toolTip (buildToolTip(*this)); call
     once every track is added."
2. In `demandstate.cpp`, `finish()` keeps only
   `toolTip = buildToolTip(*this);`.
3. Tests. Mechanical, the same fact through the counts. Locate each by text
   (`grep -rn progressLabel tests`). At `ec6bd43` there are about 30
   assertions: `tst_calculation_demand` ~747, 763, 789, 798, 805, 856,
   1368, 1724, 1848-1849, 1877, 3436, 3460; `tst_fusion_rows` ~274, 350,
   412, 435, 459, 469, 515; `tst_plot_row_delegate` ~328, 558-559, 627,
   660, 762, 826, 973. Phases 1-3 may have added more.

   | Today | Replacement |
   |---|---|
   | `QCOMPARE(x.progressLabel, QStringLiteral("k of n"));` | `QCOMPARE(x.doneCount, k); QCOMPARE(x.wantedCount, n);` (`x` is working at that point, as the label was non-empty) |
   | `QVERIFY(x.progressLabel.isEmpty());` | `QVERIFY(!x.isWorking());` |
   | `QCOMPARE(a.progressLabel, b.progressLabel);` | `QCOMPARE(a.doneCount, b.doneCount); QCOMPARE(a.wantedCount, b.wantedCount);` |
   | `x.progressLabel != QStringLiteral("0 of 2")` (a condition, `makeWorkingRow`) | `x.doneCount != 0 \|\| x.wantedCount != 2` |

**Acceptance Criteria:**
- [ ] `grep -rn "progressLabel" src tests` finds nothing.
- [ ] `tst_calculation_demand`, `tst_fusion_rows` and `tst_plot_row_delegate` pass, asserting the same counts. The signal-count tests (`changeSignalsAreMinimal`, `workingIdsFollowStates`, `plotStateChangeRepaintsRow`, …) pass unchanged.

**Complexity:** M

---

### Task 4.6: The fill's own progress text

**Purpose:** The logbook's progress line says "Computing results: k / n"
while the fill is the active task, distinct from the column worker's
"Computing columns: k / n" (spec §12, "The fill's own progress text").

**Files to modify:**
- `src/ui/docks/logbook/LogbookView.cpp`: the label switch in
  `onActiveTaskChanged` (~274-310 at ec6bd43, ending in
  `m_progressBar->setFormat(label);`). Locate it by that text; the label is
  not set in `onProgressChanged`.

**Technical Approach:**

Today (LogbookView.cpp ~362-367):
```cpp
    case SessionModel::ColumnTask:
    case SessionModel::ColumnFillTask:
        // The same text on purpose: to the user a column fills in the
        // background the same way whether its values are cheap or requested
        label = tr("Computing columns: %v / %m");
        break;
```
Split it:
```cpp
    case SessionModel::ColumnTask:
        label = tr("Computing columns: %v / %m");
        break;
    case SessionModel::ColumnFillTask:
        // The demand layer's fill has a total of its own (the sessions still
        // to compute), so it has a text of its own: the line never shows two
        // totals under one label when the fill follows a column pass
        label = tr("Computing results: %v / %m");
        break;
```
Nothing else changes:
- The cancel button follows `cancellable` (the fill is registered not
  cancellable).
- The active task comes from the scheduler, whose priorities (save 1, load 2,
  bulk edit 3, column work 4, fill 5) are untouched.
- The line hides on `schedulerIdle`.

The fill's final `(0, n)` report reaches the line through Phase 1's
completion rule.

**Acceptance Criteria:**
- [ ] With `ColumnFillTask` active, the progress bar's `format()` is `"Computing results: %v / %m"` and its `text()` for `(remaining 2, total 3)` is `"Computing results: 1 / 3"`. The cancel button is hidden.
- [ ] With `ColumnTask`, `SaveTask`, `LoadTask` or `BulkEditTask` active, the text is as before ("Computing columns", "Saving sessions", "Loading sessions", "Updating sessions").
- [ ] In a real fill over unloaded sessions, every progress report of `ColumnFillTask` is shown under "Computing results", and every report of `ColumnTask` under "Computing columns".

**Complexity:** S

---

### Task 4.7: Tests

**Purpose:** Prove one glyph with no label or count, one clock for both
views, the fill's text, the shared plumbing, and the failed write in the
hover (spec §14 "The user interface"). Keep every existing assertion's fact.

**Files to modify:**
- `tests/tst_plot_row_layout.cpp`: rewrite to the one-glyph layout.
- `tests/tst_plot_row_delegate.cpp`: the fixture's clock, the edits listed below, one new test.
- `tests/tst_logbook_indicators.cpp`: the fixture's clock, a plot-list helper, three new tests.
- `tests/CMakeLists.txt`: both widget tests compile `DemandIndicatorView.*`, and `tst_logbook_indicators` also compiles the plot row delegate (Task 4.8).

**Technical Approach:** See "Testing Requirements". Every edit and every new
test is listed there.

**Acceptance Criteria:**
- [ ] The new test functions are declared in `private slots:` and pass: `tst_plot_row_delegate::sharedGlyphPlumbing`, `tst_logbook_indicators::plotRowsAndHeaderTurnOnOneClock`, `fillProgressLineHasItsOwnText` and `failedWriteIsListedInTheHover`.
- [ ] No test function cited in `tests/acceptance_map.txt` is renamed. `tst_plot_row_layout::indicatorAndWarning` is removed and its map line with it (Task 4.8).
- [ ] The full suite passes sequentially.

**Complexity:** L

---

### Task 4.8: Build lists, the audit and the acceptance map

**Purpose:** Build the new file and the widget tests with their sources. Keep
`audit_cleanup` and its traceability check green. The audit rules that must
name the new file are updated here, and new rules are Phase 5's.

**Files to modify:**
- `src/CMakeLists.txt`: the UI source list (Task 4.1).
- `tests/CMakeLists.txt`: the sources of both widget tests and the comments (~155-185).
- `tests/audit/cleanup_audit.cmake`: two rules.
- `tests/acceptance_map.txt`: one line.

**Technical Approach:**

1. **`tests/CMakeLists.txt`.**
   - `tst_plot_row_delegate`: add
     `"${FLYSIGHT_SRC_DIR}/ui/docks/DemandIndicatorView.cpp" "${FLYSIGHT_SRC_DIR}/ui/docks/DemandIndicatorView.h"`
     after the `DemandIndicator` pair.
   - `tst_logbook_indicators`: add the same pair, and also
     ```
     "${FLYSIGHT_SRC_DIR}/ui/docks/plotselection/PlotRowDelegate.cpp" "${FLYSIGHT_SRC_DIR}/ui/docks/plotselection/PlotRowDelegate.h"
     "${FLYSIGHT_SRC_DIR}/ui/docks/plotselection/PlotRowLayout.h"
     ```
   - Update the comments. Both tests compile "the shared indicator
     (DemandIndicator.*, DemandIndicatorView.*)". Also: "…
     `tst_logbook_indicators`: the logbook's column headers, pending cells
     and progress line in a real LogbookView, beside a plot list with the
     row delegate, on one working-indicator clock, compiled from the
     application sources".
   - `DemandIndicatorView.cpp` includes `calculationdemand.h` and
     `demandstate.h`, which come from `flysight_core`, which both tests
     link.
   - `cmake/SolverDependencies.cmake` needs no change, because no new test
     target is added.
2. **Audit.** `DemandIndicator.*` stays where the audit puts it, and the
   three rules that name it are not edited for it:
   - "the logic components see no widget" (`widget-free-core`, ~427-430);
   - "nothing below the demand layer includes it" (`demand`, ~705-708);
   - "the demand views handle no event of their own" (`demand`, ~717-721).

   The pathspec glob `"src/ui/docks/DemandIndicator.*"` does not match
   `DemandIndicatorView.*`, because the character after "DemandIndicator"
   must be a literal ".". So the new file falls outside the first two rules
   without an exclusion. It must be named in two rules:

   | Rule (group) | Change |
   |---|---|
   | "only the application and its views know the demand layer" (`demand`, ~700-702, as Phase 2 left it) | Add `\|^src/ui/docks/DemandIndicatorView\\.(cpp\|h)$` to the allowed-file regex. The comment above gains "the shared indicator's view half (DemandIndicatorView.*) is one of its views; DemandIndicator.* is not". |
   | "the demand views handle no event of their own" (`demand`, ~717-721) | Add `"src/ui/docks/DemandIndicatorView.*"` to the pathspec, beside `"src/ui/docks/DemandIndicator.*"`. |

   Item 542's citation of `widget-free-core` is unaffected. Plant a hit once
   for each point below, then remove it:
   - `#include <QWidget>` in `src/ui/docks/DemandIndicator.h` still trips
     "the logic components see no widget";
   - `#include "calculationdemand.h"` in `src/ui/docks/DemandIndicator.cpp`
     still trips "nothing below the demand layer includes it";
   - a comment `// mousePressEvent` in `src/ui/docks/DemandIndicatorView.cpp`
     trips "the demand views handle no event of their own";
   - `CalculationDemand` in a comment of `src/ui/docks/DemandIndicator.h`
     trips "only the application and its views know the demand layer",
     while `DemandIndicatorView.cpp`'s own uses do not.
3. **Acceptance map.** Delete the line `562 tst_plot_row_layout indicatorAndWarning`.
   Item 562 keeps `tst_plot_row_layout indicatorOnly` (retargeted to the one
   glyph) and its other lines. No other cited function is renamed or
   removed. The new functions are cited by Phase 5.

**Acceptance Criteria:**
- [ ] `cmake build-phase1/FlySightViewer-build` reconfigures, and `cmake --build build-phase1 --config Release` builds the application (with `DemandIndicatorView.cpp`) and every test.
- [ ] `audit_cleanup` passes, including the traceability check. Each planted hit trips its rule. The widget-free-core rule and "nothing below the demand layer includes it" are textually unchanged.
- [ ] `tests/acceptance_map.txt` differs from Phase 3's only by the removed `indicatorAndWarning` line.

**Complexity:** S

## Testing Requirements

### Unit Tests

**`tests/tst_plot_row_layout.cpp`** (pure geometry, `kItem(20, 100, 200, 20)`, `kMetrics{16, 4, 4}`):
- `rectsOf()` becomes the one `glyph`. Delete `indicatorAndWarning`,
  `warningOnly` and `emptyLabelOmitsItsSpacing`, both the declarations and
  the bodies.
- `indicatorOnly` (kept, cited by item 562):
  `layoutPlotRow(kItem, kMetrics, true, Qt::LeftToRight)`, so `glyph == QRect(200, 102, 16, 16)` and `reservedWidth == 20`.
- `nothingShown`: `showsGlyph` false, in both directions: `glyph.isNull()` and `reservedWidth == 0`.
- `rightToLeftIsMirrorImage_data` / `rightToLeftIsMirrorImage`: the only
  column is `item`. The rows are "row" `kItem`, "wide" `QRect(0, 0, 301, 23)`,
  "odd origin" `QRect(-7, 3, 97, 17)` and "exact fit"
  `QRect(20, 100, 20, 20)`. The assertions are the same, on the glyph: the
  mirror image both ways, equal `reservedWidth`, and inside the item when it
  fits.
- The file comment says "the geometry of a plot-list row's one glyph".

**`tests/tst_plot_row_delegate.cpp`:**
- **Fixture.**
  - Add `#include "demandstate.h"` if it is not already reachable,
    `#include "ui/docks/DemandIndicatorView.h"`, and a member
    `std::unique_ptr<WorkingAnimation> m_clock;` declared after `m_demand`.
  - In `buildUi`, right after `m_demand` is made:
    `m_clock = std::make_unique<WorkingAnimation>(); followDemand(m_clock.get(), m_demand.get());`.
    Then `m_delegate = new PlotRowDelegate(m_demand.get(), m_clock.get(), m_view.get());`.
    Keep `m_delegate->animation()->setFrozen(true);`.
  - In `destroyUi`, reset `m_clock` after `m_demand`.
  - The file comment's "the working indicator's clock (DemandIndicator.h)"
    becomes "the application's working-indicator clock, made to follow the
    demand layer by followDemand() (DemandIndicatorView.h)".
- **Helpers.**
  - Delete `indicatorSlot`, `labelRect` and `badgeSlot`. Use
    `m_delegate->indicatorRect(index)`.
  - Add
    `QRect besideGlyph(const QModelIndex &index) const { const QRect row = m_view->visualRect(index); const QRect glyph = m_delegate->indicatorRect(index); return QRect(row.left(), row.top(), glyph.left() - row.left(), row.height()); }`
    ("the row left of its glyph: the check box and the name").
  - `glyphSide()` stays.
  - Replace every `clusterRect(` with `indicatorRect(`.
- **Edits by function.** Replace `progressLabel` everywhere as in Task 4.5.
  - `plainRowsAreIdenticalToBaseDelegate`: `PlotRowDelegate inert(nullptr, nullptr, m_view.get());`.
  - `workingRowPaintsIndicator`:
    - The comment "label and indicator" becomes "the indicator".
    - Add `QCOMPARE(m_delegate->indicatorRect(index).size(), QSize(glyphSide(index), glyphSide(index)));`.
    - **No label:** `QCOMPARE(cut(with, besideGlyph(index)), cut(base, besideGlyph(index)));`.
      The name "g" fits, so the row left of the glyph is the base
      delegate's.
    - The right margin is the base delegate's too:
      `const QRect margin(indicatorRect.right() + 1, rowRect.top(), rowRect.right() - indicatorRect.right(), rowRect.height()); QCOMPARE(cut(with, margin), cut(base, margin));`.
  - `longNameIsElidedNotTheCluster`: the comments "(\"0 of 2\" and the
    indicator)" and "The band over the label and the indicator" become "the
    glyph". The assertions stay, on `indicatorRect`.
  - `makeWorkingRow`: the doc "The row is working, "0 of 2"" becomes "The
    row is working: 0 of 2 done", and the condition is from Task 4.5.
  - `workingIndicatorAnimatesOnlyWhileWorking`: replace
    `QCOMPARE(cut(frame1, labelRect(index)), cut(frame0, labelRect(index))); // "0 of 2"`
    with `QCOMPARE(cut(frame1, besideGlyph(index)), cut(frame0, besideGlyph(index)));`.
    Use `indicatorRect(index)` for the frame difference. The rest is
    unchanged: `!clock->isActive()` after the work ends now comes from
    `followDemand()`.
  - `badgeReplacesIndicatorOnceFinished`:
    - `badgeSlot` / `indicatorSlot` become `indicatorRect`.
    - **No count:** `QCOMPARE(cut(badged, besideGlyph(badgeRow)), cut(grabViewportWithBaseDelegate(), besideGlyph(badgeRow)));`.
      Grab the base right after `badged`, before `makeWorkingRow()`.
  - `clickOnClusterIsAClickOnTheRow`: `clusterRect` becomes `indicatorRect`.
    The points stay the same (centre, left, right, top right, row's right
    edge). The comment "over the working indicator and its label, and over
    the warning badge and its count" becomes "over the working indicator
    and over the warning badge".
  - `hoverDetailFollowsDemandState`: `indicatorSlot(index).center()` becomes
    `m_delegate->indicatorRect(index).center()`.
  - `survivesDemandDestroyedFirst`: unchanged in substance.
    `!m_delegate->animation()->isActive()` now proves that the clock
    stopped when the demand layer went. Add `QVERIFY(m_delegate->animation() == m_clock.get());`.
  - `workingAnimationClock`: add at the end
    `WorkingAnimation follower; followDemand(&follower, nullptr); QVERIFY(!follower.isActive()); QVERIFY(!follower.isTicking());`.
    Also `followDemand(nullptr, nullptr);` does nothing and does not crash.
- **New: `sharedGlyphPlumbing`.** Declare it after `workingAnimationClock`.
  Add `#include <QStyleOptionHeader>`. It covers both halves: `glyphMetrics`
  from `DemandIndicator.h`, and `glyphColor` and `showIndicatorToolTip` from
  `DemandIndicatorView.h`.
  1. **Metrics.** `const QFontMetrics fm = m_view->fontMetrics();`. Then
     `glyphMetrics(fm, 1000).side == fm.height()`,
     `glyphMetrics(fm, 7).side == 7`, `glyphMetrics(fm, 7).spacing == 2`,
     `glyphMetrics(fm, 40).spacing == qMax(2, qMin(40, fm.height()) / 4)`,
     and `reserve() == side + spacing`.
  2. **Colours.** Use a `QPalette p` in which every (group, role) of Text,
     HighlightedText and ButtonText for Normal, Inactive and Disabled has a
     distinct colour.
     - `QStyleOptionViewItem v; v.palette = p;` with
       `v.state = State_Enabled | State_Active` gives `p.color(Normal, Text)`.
     - Adding `State_Selected` gives `HighlightedText` in Normal.
     - `State_Enabled` alone (inactive window) gives Inactive.
     - No `State_Enabled` gives Disabled, with or without `State_Active`.
     - `QStyleOptionHeader h; h.palette = p;` gives `ButtonText` in the same
       three groups, and never `HighlightedText`.
  3. **Tooltip.**
     - `DemandState plain;`: `QVERIFY(!showIndicatorToolTip(&event, plain, m_view->viewport(), rect))`
       and no tooltip visible (hide first, as the tests' `hideToolTip()`
       does).
     - `DemandState s; s.waitingCount = 1; s.toolTip = QStringLiteral("Computing: 0 of 1 done");`:
       it returns true, and `QToolTip::text() == s.toolTip`.
     - A null event returns false.
     - `QToolTip::hideText()` at the end.

**`tests/tst_logbook_indicators.cpp`:**
- **Fixture.**
  - Add the member `std::unique_ptr<WorkingAnimation> m_clock;` after
    `m_demand`.
  - In `buildServices`, after `m_demand`:
    `m_clock = std::make_unique<WorkingAnimation>(); followDemand(m_clock.get(), m_demand.get());`.
  - In `buildUi`:
    `m_logbook = new LogbookView(m_model.get(), m_demand.get(), m_clock.get(), m_window.get());`.
    Keep `header()->animation()->setFrozen(true);`: it freezes `m_clock`.
  - In `cleanup`, `m_clock.reset()` right after `m_demand.reset()`.
  - Rewrite the file comment's "the clock that runs only while a column
    works" as "the application's one working-indicator clock, shared with
    the plot list's rows", and add "the progress line's texts".
  - Add the includes `<QProgressBar>`, `<QDir>`, `"calculationrecord.h"`,
    `"ui/docks/DemandIndicatorView.h"` and
    `"ui/docks/plotselection/PlotRowDelegate.h"`.
- **Helper** (private):
  `[[nodiscard]] std::unique_ptr<QTreeView> makePlotList(PlotRowDelegate **delegate)`
  configures a `QTreeView` over `m_plots` as `PlotSelectionDockFeature` does:
  - `setHeaderHidden(true)` and `NoEditTriggers`;
  - `new PlotRowDelegate(m_demand.get(), m_clock.get(), tree)`;
  - `expandAll()`, `resize(300, 300)` and `show()`;
  - returns null unless `QTest::qWaitForWindowExposed`.

  Add also `QModelIndex plotIndex(const char *plotId) const`, the
  `PlotValueIdRole` lookup of `tst_plot_row_delegate::indexOf`.
- **New: `plotRowsAndHeaderTurnOnOneClock`.** Declare it after
  `indicatorAnimatesOnlyWhileWorking`. This is the spec §14 test "the two
  views' clocks are one".
  1. `PlotRowDelegate *rows = nullptr; auto plotList = makePlotList(&rows); QVERIFY(plotList);`.
  2. `PlotFixture::show(*m_model, {"s1"}); m_plots->setPlotEnabled(QStringLiteral("Syn"), QStringLiteral("g"), true); enableColumns({QStringLiteral("G_OUT")});`.
     Then `QVERIFY(gate().waitEntered());` for the one job (s1, gated), which
     both sources share.
  3. `QTRY_VERIFY(m_demand->plotState("Syn/g").isWorking() && col("G_OUT").isWorking()); spin();`.
  4. **One clock.**
     - `QCOMPARE(rows->animation(), m_clock.get()); QCOMPARE(header()->animation(), m_clock.get());`.
     - `QVERIFY(m_window->findChildren<WorkingAnimation *>().isEmpty()); QVERIFY(plotList->findChildren<WorkingAnimation *>().isEmpty());`:
       neither view made its own.
     - `QVERIFY(m_clock->isActive()); QVERIFY(!m_clock->isTicking());`, since
       it is frozen by `buildUi`.
  5. **One frame turns both.**
     - Grab `plotList->viewport()` and the header, then put a `PaintCounter`
       on the Syn/g row (`visualRect` widened to the viewport's width) and
       one on `sectionRect(section("G_OUT"))`.
     - `m_clock->advance();`, then `QTRY_VERIFY` that both counters rose.
     - Both glyphs changed: `cut(after, rows->indicatorRect(plotIndex("Syn/g")))`
       differs from before, and the same for the header's
       `indicatorRect(section("G_OUT"))`.
     - `QCOMPARE(m_clock->angle(), 30.0)`: both painted this one angle.
  6. **Each view repaints only its own working items.**
     - `gate().open(1)` makes s1 done, and s2 runs:
       `QVERIFY(gate().waitEntered()); QTRY_VERIFY(!m_demand->plotState("Syn/g").isWorking()); spin();`.
     - The plot row is plain and the column still works, so the clock stays
       active.
     - Put new counters on the row and the section, then
       `m_clock->advance(); spin();`. The section counter rose. The row
       counter is 0.
  7. **Stops when nothing works.**
     - `gate().open(16); QVERIFY(waitDemandIdle()); QVERIFY(waitForIdle(*m_model)); spin();`.
     - Then `!m_clock->isActive()` and `m_clock->frame() == 0`.
     - Destroy `plotList` before returning.
- **New: `fillProgressLineHasItsOwnText`.** Declare it after
  `pendingCellBecomesValueWhenRecordIsWritten`.
  1. **Direct.** Take `auto *bar = m_logbook->findChild<QProgressBar *>();`
     and `auto *cancel = m_logbook->findChild<QToolButton *>();`. For each
     `(id, cancellable, expected)` of the following, call
     `m_logbook->onActiveTaskChanged(id, cancellable); m_logbook->onProgressChanged(id, 2, 3);`,
     then check `bar->text() == expected` and
     `cancel->isVisibleTo(m_logbook) == cancellable`:
     - `(SaveTask, false, "Saving sessions: 1 / 3")`
     - `(LoadTask, true, "Loading sessions: 1 / 3")`
     - `(BulkEditTask, false, "Updating sessions: 1 / 3")`
     - `(ColumnTask, true, "Computing columns: 1 / 3")`
     - `(ColumnFillTask, false, "Computing results: 1 / 3")`

     Also check `bar->format() == "Computing results: %v / %m"` for the
     fill. A report for another id than the active one changes nothing.
     `onSchedulerIdle()` hides the bar.
  2. **Live fill.**
     - Set `PreferenceKeys::LogbookCacheSize` to 0 and `waitForIdle(*m_model)`,
       so every session is a stub (as in the "stub" row of
       `pendingCellBecomesValueWhenRecordIsWritten`).
     - Wire the scheduler to the view as `LogbookDockFeature` does. Then
       connect a recorder to `IdleScheduler::progressChanged`, which runs
       after the view's slot and appends `(id, bar->format())` when
       `id == ColumnFillTask || id == ColumnTask`.
     - `gate().open(16); enableColumns({QStringLiteral("G_OUT")}); QVERIFY(waitDemandIdle()); QVERIFY(waitForIdle(*m_model));`.
     - At least one `ColumnFillTask` entry was recorded. Every
       `ColumnFillTask` entry has `"Computing results: %v / %m"`, and every
       `ColumnTask` entry has `"Computing columns: %v / %m"`.
     - `QTRY_VERIFY(!bar->isVisible())` at the end.
- **New: `failedWriteIsListedInTheHover`.** Declare it after
  `failedLoadSessionShowsBadgeNotPending`. This is spec §12 "A stored result
  that could not be written" (presentation half; the demand side is Phase
  3's).
  1. Provoke the failure as Phase 1 describes (a directory at the record's
     path):
     `const QString path = TestEnvironment::instance().cacheDir() + QLatin1Char('/') + recordFileName(sessionFileStem(QStringLiteral("s1")), QStringLiteral("gated")); QVERIFY(QDir().mkpath(path)); const auto removeDirectory = qScopeGuard([path] { QDir(path).removeRecursively(); });`.
  2. `PlotRowDelegate *rows = nullptr; auto plotList = makePlotList(&rows); QVERIFY(plotList);`.
  3. `PlotFixture::show(*m_model, {"s1"});`, check `Syn/g`, and
     `enableColumns({QStringLiteral("G_OUT")}); gate().open(16);`.
  4. `{ WarningCapture warnings; QVERIFY(waitDemandIdle()); QVERIFY(waitForIdle(*m_model)); } spin();`.
     The store logs "not written".
  5. **The plot row.**
     - `const DemandState plot = m_demand->plotState("Syn/g");` gives
       `showsWarning()` and `failedCount == 1`.
     - `plot.toolTip.startsWith("Could not be computed:")`.
     - One line of the tooltip starts with `"  Jump 1 - "` and contains
       `"Couldn't write file"`.
     - `rows->toolTipFor(plotIndex("Syn/g")) == plot.toolTip`.
     - The row's `indicatorRect` has an amber `kAmber` pixel.
     - A `QHelpEvent` through `rows->helpEvent(...)` at the glyph's centre
       shows `plot.toolTip` (`QToolTip::text()`).
  6. **The column header.**
     - `const DemandState column = col("G_OUT");` gives `showsWarning()`,
       `failedCount == 1` and `wantedCount == 3`, since s2 and s4 were
       written.
     - The tooltip holds the same "Jump 1 - …Couldn't write file…" line as
       the plot row's.
     - `header()->toolTipForSection(section("G_OUT")) == column.toolTip`.
     - `headerHelp(indicatorRect.center())` shows it.
     - There is an amber pixel in the section's `indicatorRect`.
  7. Destroy `plotList` before returning.

  Assert on the substrings. Phase 3 decides whether the reason is prefixed
  with the calculation's title.
- **Existing functions**: no edit beyond the fixture. `header()->animation()`
  now returns the shared clock, which `followDemand()` makes follow the
  demand layer, so
  `plainHeaderAndCellsAreIdenticalToBase`, `indicatorAnimatesOnlyWhileWorking`,
  `badgeReplacesIndicatorWhenFinished`, `failedLoadSessionShowsBadgeNotPending`
  and `survivesDemandDestroyedFirst` assert the same facts.

**`tests/tst_calculation_demand.cpp`, `tests/tst_fusion_rows.cpp`:** only the `progressLabel` replacements of Task 4.5.

### Integration Tests
- The full suite, sequentially:
  `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`.
  The widget tests (`tst_plot_row_delegate`, `tst_logbook_indicators`) and
  the load-sensitive fusion tests (`tst_fusion_rows`, `tst_fusion_store`,
  `tst_fusion_jobs`, `tst_fusion_session`) must pass without `-j`.
- `audit_cleanup`, including its traceability check.

### Manual Verification
1. Build `build-phase1` and run the application.
2. Check a fusion plot with two visible tracks without stored fits. The row
   shows only the turning arc at its right end, with no "0 of 2". Hovering
   the row or the arc shows "Computing: 0 of 2 done" and the running track
   with its step.
3. Also add a logbook column over a fusion value. The header's arc and the
   row's arc turn in step. Watch both for a few seconds: they point the same
   way at every moment.
4. When the fits end, the row goes plain. With one track that cannot be
   fitted, the row shows only the warning triangle, with no number. The
   hover lists the track with its reason.
5. Hide every track and enable the column on a logbook with unloaded
   recordings. The progress line shows "Computing results: k / n" while the
   fill loads and fits, and "Computing columns: k / n" while cheap columns
   are computed. It never shows one label with two totals. There is no
   cancel button for the fill.
6. Make a record unwritable: a folder with the record file's name in the
   logbook's `cache/` folder. Show the track and let it fit. The row and the
   column header show the triangle, and the hover lists the recording with a
   "Couldn't write file" reason.
7. Close the application while a fit runs. It closes without a crash
   (demand layer, then clock, then executor).

## Notes for Implementer

### Build and test (from the overview's Decisions & Constraints)
- Build **only** `build-phase1/`: `cmake --build build-phase1 --config Release`. **Never build `build/`.**
- This phase **adds a source file**, `src/ui/docks/DemandIndicatorView.{h,cpp}` in `src/CMakeLists.txt`. It also adds sources to both widget test targets: both compile `DemandIndicatorView.*`, and `tst_logbook_indicators` also compiles `PlotRowDelegate.*` and `PlotRowLayout.h`. Reconfigure the inner project once, after editing both CMake lists: `cmake build-phase1/FlySightViewer-build`. Then build.
- Test with `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`, **sequentially, never `-j`**, with no stray `ctest` or `tst_*` process running (executor-driven fusion tests are load-sensitive). One target: `-R tst_logbook_indicators`. The audit: `-R audit_cleanup`.
- No new test target links fusion, so `_FLYSIGHT_GTSAM_REACHERS` in `cmake/SolverDependencies.cmake` is unchanged.
- Commits are the orchestrator's. Never run a git command that changes repository state.

### Gotchas
- **Incomplete types behind `QPointer`.** `QPointer<T>::data()` casts from `QObject *` and needs `T` complete. A header that only forward-declares `WorkingAnimation` or `CalculationDemand` may hold the `QPointer` member but must not dereference it inline. Include `DemandIndicator.h` in `LogbookHeaderView.h`, or define `animation()` in the `.cpp`. `followDemand` keeps its `QPointer<CalculationDemand>` inside `DemandIndicatorView.cpp`, where the type is complete.
- **Keep the core's boundary.** `DemandIndicator.{h,cpp}` must not include `demandstate.h` or `calculationdemand.h`, must not include a widget header (`QStyleOption*`, `QToolTip`, `QWidget`, …), and must not name `CalculationDemand`, even in a comment (write "the demand layer"). Anything that reads a `DemandState`, a style option or the demand layer goes in `DemandIndicatorView`. That is why `drawDemandGlyph` lives there, not beside the glyphs.
- **`struct DemandState`** is forward-declared as `struct`, not `class` (MSVC C4099 treats the mismatch as a warning, and warnings-as-errors builds may fail).
- **`followDemand()` drives the clock; the views never call `setActive`.** Delete every `syncAnimation()`. A view that still connected `statesChanged` to its own clock logic would fight the shared clock. That includes stopping it because *its* ids are empty while the other view's are not.
- **A frame is cheap for the view with nothing working.** When only a column works, the plot delegate's `onAnimationFrame` runs, finds `workingPlotIds()` empty and repaints nothing. Keep the early return and do not repaint the whole viewport.
- **Destroyed order.** During `QObject::destroyed` the demand layer's `QPointer`s are already null. `followDemand()`'s slot therefore sees null and stops the clock. `repaintWhenDemandDestroyed` only schedules an update, which paints plain rows because `stateFor()` checks `m_demand`.
- **Pixel identity.** The colour-group rule must stay exactly as today (Disabled when not enabled, else Inactive when not active, else Normal). Selection picks `HighlightedText` for rows only. The header's `ButtonText` never depends on selection. The existing pixel tests compare against the base classes and the reference header.
- **The elision budget** uses `reservedWidth + spacing`, as today's `clusterWidth + spacing`. For the old indicator-only row without a label, `clusterWidth` was also `rightMargin + iconSide`, so the elision of a working row's name only gains the label's room.
- **Audit words in comments.**
  - Do not write `JobQueue` or `JobModel` anywhere in `src/ui` except `AppContext.h`.
  - Do not write `processEvents`, `QEventLoop`, `QThread`, "cancel control" or "cancel icon" in `src`.
  - Do not write a mouse or key handler's name in `DemandIndicator.*`, `DemandIndicatorView.*`, `PlotRowDelegate.*` or `LogbookHeaderView.*` (write "no event of its own").
  - `CalculationDemand` may appear in `DemandIndicatorView.*` (allowed by Task 4.8) and never in `DemandIndicator.*`.
- **`tst_logbook_indicators` window.** The plot list for the new tests is a separate top-level widget. Wait for its exposure, and destroy it before the test returns, so `cleanup()` (views before the demand layer) holds.
- **The failed-write test** needs saved, indexed sessions for `sessionFileStem()`, which `init()` provides. It needs the directory removed afterwards (`qScopeGuard`). Wrap the waits in `WarningCapture`, as `tst_result_store` does, because the store logs the failure.
- **`QProgressBar::text()`** is empty when minimum and maximum are both 0. Use non-zero totals in the direct part of `fillProgressLineHasItsOwnText`.

### Decisions Made
- **The plumbing is split so that no boundary moves (spec §1).**
  - **`DemandIndicator.{h,cpp}` keeps its place.** It stays QtCore + QtGui,
    in the widget-free core and below the demand layer: it names no demand
    type and includes no demand-layer header. It keeps the glyph drawing
    (`drawWorkingGlyph`, `drawWarningGlyph`) and `WorkingAnimation`'s clock
    mechanics, and gains the glyph metrics (`GlyphMetrics` /
    `glyphMetrics`, QtGui font metrics).
  - **`src/ui/docks/DemandIndicatorView.{h,cpp}` is new.** It holds the
    widget-bound, demand-aware part, and both views use it: `glyphColor` for
    a row and for a header section, `drawDemandGlyph`,
    `showIndicatorToolTip`, `repaintWhenDemandDestroyed` and `followDemand`.
    It is a view of the demand layer.
  - The audit names the new file in "only the application and its views know
    the demand layer" and "the demand views handle no event of their own".
    The rules that keep `DemandIndicator.*` widget-free and below the demand
    layer are unchanged.
  - `drawDemandGlyph` lives in the view half because it reads a
    `DemandState`, and `demandstate.h` is a demand-layer header that the core
    may not include.
- **The clock is made to follow the demand layer by a free function,
  `followDemand(WorkingAnimation *clock, CalculationDemand *demand)`**
  (`DemandIndicatorView.h`), not by a member of the core class. The clock is
  active while `workingPlotIds()` or `workingColumnIds()` is not empty. It
  re-reads them on `statesChanged()` and on `destroyed`, with the clock as
  the connections' context. `MainWindow` calls it once, and the tests call it
  for their own clock. The views decide nothing about the clock: they repaint
  their own working items on `frameAdvanced` and read `angle()` when
  painting. `setActive` stays public, for `followDemand` and the standalone
  clock test.
- **The views receive the clock as a constructor argument**
  (`PlotRowDelegate(demand, clock, view)`,
  `LogbookView(model, demand, clock, parent)`,
  `LogbookHeaderView(model, demand, clock, parent)`) and hold it in a
  `QPointer`. A null clock means "at rest". Tests create a clock and call
  `followDemand()` themselves, so no test needs `MainWindow`. `animation()`
  keeps its name and returns the given clock, so `setFrozen` / `advance` in
  existing tests keep working unchanged.
- **The clock is deleted explicitly in `~MainWindow`**, right after the
  demand layer and before the executor, and the views also hold it weakly.
  The order is deterministic, and a late paint is safe.
- **The plot delegate's `clusterRect()` is renamed `indicatorRect()`**,
  matching the header's. `PlotRowGeometry` is `{glyph, reservedWidth}`, and
  `layoutPlotRow` takes one `showsGlyph` flag.
- **`DemandState::progressLabel` is removed** (spec §2: nothing uses it once
  the label goes). The tests assert `doneCount` / `wantedCount` instead.
  `operator==` loses a derived field, so no announcement changes.
- **No tooltip text changes.** "Both numbers" in the hover are the
  "Computing: k of n done" line that both views already show from the one
  builder. The failures are listed by name, as for the header today.
- **The tooltip helper takes the `DemandState` and returns false for a plain
  state**, so the caller falls back to its base class. That keeps a model's
  `ToolTipRole` and today's behaviour ("the event is not accepted" over a
  plain section). It does not call `QToolTip::hideText()`: `showText` with
  an area already hides the tooltip when the pointer leaves.
- **The cell delegate uses `repaintWhenDemandDestroyed` too**, so the
  destroyed repaint exists once. Its pending colour and tooltip are the
  cell's own and stay.
- **The one-clock test lives in `tst_logbook_indicators`**, which now also
  compiles the plot row delegate. It is the only binary with a demand layer,
  a plot model, a logbook view and widgets. The failed-write hover test is
  there too, covering both views on saved, indexed sessions.

### Open Questions
- None.

### Hand-off to Phase 5
- **Passages this phase makes false:**
  - `docs/COMPUTED_PLOTS.md`:
    - §2 table: "A turning arc with "k of n"" and "A warning triangle with
      a number" become a turning arc and a warning triangle alone. The hover
      gives the numbers.
    - §3 (~71): "the count on the row rises" becomes "the numbers in the
      row's hover rise".
    - §4 (~87): "Computing columns: k / n" for the fill becomes "Computing
      results: k / n".
    - §7: a result that could not be stored is listed with its reason (with
      Phase 3).
    - §8: the arcs of the plot list and the logbook turn in step.
  - `docs/CALCULATIONS.md`:
    - ~1247 (the `progressLabel` row);
    - ~1518-1521 (the fill's progress label "Computing columns … under the
      same label as the cheap column pass");
    - ~1588 ("k of n");
    - 16.10 ~1636-1646: plot rows paint `progressLabel` and `failedCount`,
      and "the name is elided, never the cluster";
    - 16.10 ~1648-1653: "The shared glyphs … `WorkingAnimation`, one clock
      per view, ticking … only while its view has something working". Keep
      "`src/ui/docks/DemandIndicator.h` (Qt Core and Gui only)": it stays
      true. Rewrite the clock sentence as "one clock per application,
      created in `MainWindow` beside the demand layer, handed to the views
      through `AppContext::workingClock`, and made to follow the demand
      layer by `followDemand()`". Add a sentence on
      `src/ui/docks/DemandIndicatorView.h`: the views' half (the glyph's
      colour for a style option, the one glyph of a `DemandState`, the
      hover, the repaint when the demand layer goes, and `followDemand`),
      which uses Qt Widgets and is a view of the demand layer;
    - "The progress line is unchanged in form" (~1678).
  - `README.md` source tree (~374): add `DemandIndicatorView.*` beside
    `DemandIndicator.*`.
  - `tests/README.md`:
    - ~92 (`tst_plot_row_layout`: indicator and warning, warning only, an
      empty label);
    - ~93 (`tst_plot_row_delegate`: "the working indicator and "k of n"
      painted", "the cluster", "the indicator's clock"; it now also
      compiles `DemandIndicatorView`);
    - the `tst_logbook_indicators` row (now also the plot row delegate, one
      clock, progress texts, the failed write);
    - ~1072 (item 562 cites `indicatorAndWarning`);
    - the `demand` group's description, where the no-event rule and the
      views' list now name `DemandIndicatorView.*`;
    - M4 ~1793 ("the arc and the "k of n" of a working row");
    - M17 ~1877 and M24 ~1905 ("Computing columns" for the fill becomes
      "Computing results").

    The `widget-free-core` description (~1144-1146) stays true. M2's remark
    on "Computing columns" for cheap columns stays true.
  - Acceptance items 535-539 and 557 as amended: one glyph, no label or
    count, one clock, the fill's text.
- **New test functions to cite:**
  - `tst_plot_row_delegate::sharedGlyphPlumbing`;
  - `tst_logbook_indicators::plotRowsAndHeaderTurnOnOneClock`,
    `fillProgressLineHasItsOwnText`, `failedWriteIsListedInTheHover`;
  - `tst_plot_row_layout::indicatorOnly` (retargeted).
- **Suggested audit rules (Phase 5's):**
  - one clock: `expect_count("one working-indicator clock" "new WorkingAnimation\\(" 1 src)`
    plus `expect_only(... "^src/mainwindow\\.cpp$" src)`;
  - the clock follows the demand layer in one place:
    `expect_only("the clock follows the demand layer in one place" "followDemand\\(" "^src/ui/docks/DemandIndicatorView\\.(cpp|h)$|^src/mainwindow\\.cpp$" src)`;
  - the views compute no glyph plumbing:
    `expect_none("the views share the glyph plumbing" "QPalette::ColorGroup|QToolTip::|&QObject::destroyed" "src/ui/docks/plotselection/PlotRowDelegate.*" "src/ui/docks/logbook/LogbookHeaderView.*")`;
  - no label or count on a plot row:
    `expect_none(... "progressLabel|warningCount|drawText" "src/ui/docks/plotselection/*" src/demandstate.h)`.
- **Removed names for the `demand` group:** `DemandState::progressLabel`,
  `PlotRowGeometry::warningIcon/warningCount/progressLabel/indicatorIcon`,
  `PlotRowDelegate::clusterRect`, `syncAnimation`.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria.
2. All tests pass: the full `ctest` run on `build-phase1`, sequential, including `audit_cleanup` and its traceability check.
3. The code follows the patterns of the reference files:
   - the views read the demand layer and decide nothing;
   - they hold collaborators weakly (`QPointer`);
   - one owner per fact: the glyph plumbing in `DemandIndicator` (core) and `DemandIndicatorView` (views), the clock in `MainWindow`, the tooltip text in `DemandState::buildToolTip`.
4. No TODOs or placeholder code remain. No comment in `src/` or `tests/` names a removed member (`progressLabel`, `clusterRect`, `warningCount`, `syncAnimation`), and no comment says "one clock per view". `DemandIndicator` still says "QtCore and QtGui only", and that is still true.
