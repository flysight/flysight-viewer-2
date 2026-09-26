#ifndef DEMANDINDICATORVIEW_H
#define DEMANDINDICATORVIEW_H

// The views' half of the one indicator (DemandIndicator.h is the core). It
// paints a DemandState's one glyph, takes the glyph's colour from the style
// option, shows the state's tooltip, repaints when the demand layer goes, and
// makes the application's one clock follow the demand layer. It uses Qt
// Widgets and the demand layer's read interface, and it decides nothing: what
// is shown is DemandState's (demandstate.h).

#include <QtGlobal>

class QColor;
class QHelpEvent;
class QPainter;
class QRect;
class QRectF;
class QStyleOptionHeader;
class QStyleOptionViewItem;
class QWidget;

namespace FlySight {

class CalculationDemand;
struct DemandState;
class WorkingAnimation;

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
/// The two are exclusive: while working, failures appear in the tooltip only.
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

} // namespace FlySight

#endif // DEMANDINDICATORVIEW_H
