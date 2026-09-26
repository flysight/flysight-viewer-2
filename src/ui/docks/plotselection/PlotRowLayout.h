#ifndef PLOTROWLAYOUT_H
#define PLOTROWLAYOUT_H

#include <QPoint>
#include <QRect>
#include <Qt>

// Geometry of a plot-list row's one glyph, as a pure function of integers: no
// font, no style, no widget, so that it is testable without a GUI
// application. Left to right (mirrored as a whole in a right-to-left layout):
//
//   [check] plot name (elided) ... [glyph] |
//
// The glyph is the working indicator or the warning badge. There is no label
// and no count beside it, and nothing in it is clickable.
//
// This file decides geometry only. WHICH glyph is shown is decided by
// DemandState (demandstate.h), and it is painted by DemandIndicatorView.h's
// drawDemandGlyph.

namespace FlySight {

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
/// a right-to-left layout. Rects use Qt's conventions: x() / width() are exact,
/// right() is inclusive.
inline PlotRowGeometry layoutPlotRow(const QRect &itemRect, const PlotRowMetrics &metrics,
                                     bool showsGlyph, Qt::LayoutDirection direction)
{
    PlotRowGeometry geometry;
    if (!showsGlyph)
        return geometry;

    const int edge = itemRect.x() + itemRect.width();   // exclusive right edge
    const int iconTop = itemRect.y() + (itemRect.height() - metrics.iconSide) / 2;
    geometry.glyph = QRect(edge - metrics.rightMargin - metrics.iconSide, iconTop,
                           metrics.iconSide, metrics.iconSide);
    geometry.reservedWidth = metrics.rightMargin + metrics.iconSide;

    // Mirrored about the item rect's vertical centre line
    if (direction == Qt::RightToLeft)
        geometry.glyph.moveLeft(itemRect.left() + itemRect.right() - geometry.glyph.right());

    return geometry;
}

} // namespace FlySight

#endif // PLOTROWLAYOUT_H
