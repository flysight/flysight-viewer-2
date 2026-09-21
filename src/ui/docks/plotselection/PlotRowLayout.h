#ifndef PLOTROWLAYOUT_H
#define PLOTROWLAYOUT_H

#include <QPoint>
#include <QRect>
#include <Qt>

// Geometry of the control cluster of a plot-list row, as a pure function of
// integers: no font, no style, no widget, so that it is testable without a GUI
// application. Left to right (mirrored as a whole in a right-to-left layout):
//
//   [check] plot name (elided) ........ [warn][failedCount]   [label][control] |
//
// The control icon (refresh or cancel) occupies the right-most slot, so the two
// appear in the same place and the cluster does not jump when the state flips.
// The warning group is to its left and present only when a warning is shown.
//
// This file decides geometry only. WHAT is shown - which control, which
// counts, whether there is a warning - is decided by PlotRowState
// (plotrequests.h), and painted by PlotRowDelegate.

namespace FlySight {

struct PlotRowMetrics {
    int iconSide = 0;       ///< side of the square glyphs
    int spacing = 0;        ///< between the elements of a group; twice that between the groups
    int rightMargin = 0;    ///< between the right-most element and the item rect's edge
};

struct PlotRowGeometry {
    QRect warningIcon, warningCount;    ///< null when no warning is shown
    QRect controlLabel, controlIcon;    ///< null when no control is shown (the label also when it is empty)
    /// What counts as "on the control": the icon's column over the full row
    /// height, extended to the item rect's edge. Null when no control is shown.
    /// The label and the warning group are not clickable.
    QRect controlHit;
    /// From the outer edge of the left-most present element to the item rect's
    /// right edge (left edge, in right-to-left). 0 when nothing is shown.
    int clusterWidth = 0;
};

/// Text widths are passed in (the caller measures them with its font metrics).
/// Rects use Qt's conventions: x() / width() are exact, right() is inclusive.
inline PlotRowGeometry layoutPlotRow(const QRect &itemRect, const PlotRowMetrics &metrics,
                                     bool showsWarning, int warningCountWidth,
                                     bool showsControl, int controlLabelWidth,
                                     Qt::LayoutDirection direction)
{
    PlotRowGeometry geometry;
    if (!showsWarning && !showsControl)
        return geometry;

    const int edge = itemRect.x() + itemRect.width();   // exclusive right edge
    const int iconTop = itemRect.y() + (itemRect.height() - metrics.iconSide) / 2;

    // Built right to left; `cursor` is the exclusive right edge of the next element
    int cursor = edge - metrics.rightMargin;
    const auto takeIcon = [&]() {
        cursor -= metrics.iconSide;
        return QRect(cursor, iconTop, metrics.iconSide, metrics.iconSide);
    };
    const auto takeText = [&](int width) {
        cursor -= width;
        return QRect(cursor, itemRect.y(), width, itemRect.height());
    };

    if (showsControl) {
        geometry.controlIcon = takeIcon();
        const int hitLeft = geometry.controlIcon.x() - metrics.spacing / 2;
        geometry.controlHit = QRect(hitLeft, itemRect.y(), edge - hitLeft, itemRect.height());
        if (controlLabelWidth > 0) {
            cursor -= metrics.spacing;
            geometry.controlLabel = takeText(controlLabelWidth);
        }
    }

    if (showsWarning) {
        if (showsControl)
            cursor -= 2 * metrics.spacing;
        if (warningCountWidth > 0) {
            geometry.warningCount = takeText(warningCountWidth);
            cursor -= metrics.spacing;
        }
        geometry.warningIcon = takeIcon();
    }

    geometry.clusterWidth = edge - cursor;

    if (direction == Qt::RightToLeft) {
        // Mirror every rect about the item rect's vertical centre line
        const auto mirror = [&itemRect](QRect &rect) {
            if (!rect.isNull())
                rect.moveLeft(itemRect.left() + itemRect.right() - rect.right());
        };
        mirror(geometry.warningIcon);
        mirror(geometry.warningCount);
        mirror(geometry.controlLabel);
        mirror(geometry.controlIcon);
        mirror(geometry.controlHit);
    }

    return geometry;
}

} // namespace FlySight

#endif // PLOTROWLAYOUT_H
