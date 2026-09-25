#ifndef PLOTROWDELEGATE_H
#define PLOTROWDELEGATE_H

#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QStyledItemDelegate>

#include "calculationdemand.h"
#include "ui/docks/plotselection/PlotRowLayout.h"

class QAbstractItemView;
class QColor;
class QPainter;

namespace FlySight {

/// The plot list's row delegate: it paints what CalculationDemand reports for
/// a row and decides nothing - no state, no count, no word of text, and nothing
/// about what to compute. Every number and every string it shows comes from
/// DemandState.
///
/// PAINTING. Right-aligned in a plot row (PlotRowLayout.h): a working indicator
/// with the progress label ("k of n") while any of the plot's demand is
/// waiting or running; once the work is finished and some sessions could not
/// be computed, the warning badge with the failed count instead. The two are
/// never shown together. The plot's name is elided to make room; the cluster
/// never is. A row whose state isPlain() - every plot that is not over a
/// requested calculation, every unchecked row, every row with nothing waiting,
/// running or failed, and every category - is painted by the unmodified base
/// class. The row height never changes: sizeHint() is not overridden. The
/// glyphs are drawn with QPainter in the row's text colour, so they follow the
/// theme, the selection and the screen's scale without any bundled image.
///
/// NO GESTURES. The delegate handles no event of its own: editorEvent() is the
/// base class's. Checking a row is the base class's write to PlotModel, which
/// the demand layer observes like every other check change (the Plots menu, a
/// profile, the start-up restore). Nothing in the row is clickable beyond what
/// QStyledItemDelegate makes clickable.
///
/// TOOLTIP. helpEvent() shows DemandState::toolTip over the whole row.
///
/// REPAINT. CalculationDemand::plotStateChanged(plotId) updates that row of the
/// view. There is no timer.
///
/// The component is held weakly: without it (never given, or destroyed first)
/// the delegate behaves exactly as QStyledItemDelegate.
class PlotRowDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    /// `view` is the parent, and the view whose rows are repainted.
    PlotRowDelegate(CalculationDemand *demand, QAbstractItemView *view);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override;

    /// What helpEvent() shows; empty for plain rows and categories.
    QString toolTipFor(const QModelIndex &index) const;
    /// The union of the painted cluster's rects (indicator and label, or badge
    /// and count) in viewport coordinates; null for plain rows. For tests and
    /// for nothing else.
    QRect clusterRect(const QModelIndex &index) const;

    /// An open arc of 270 degrees in `color`: the working indicator, turned by
    /// `rotationDegrees` (static in this version: 0).
    static void drawWorkingGlyph(QPainter *painter, const QRectF &rect, const QColor &color,
                                 qreal rotationDegrees);

private slots:
    void onPlotStateChanged(const QString &plotId);

private:
    /// The default ("plain") state without a component or for a category.
    DemandState stateFor(const QModelIndex &index) const;
    static PlotRowMetrics metricsFor(const QStyleOptionViewItem &opt);
    /// `opt` must have been through initStyleOption().
    PlotRowGeometry geometryFor(const QStyleOptionViewItem &opt, const DemandState &state) const;

    QPointer<CalculationDemand> m_demand;
    QPointer<QAbstractItemView> m_view;
};

} // namespace FlySight

#endif // PLOTROWDELEGATE_H
