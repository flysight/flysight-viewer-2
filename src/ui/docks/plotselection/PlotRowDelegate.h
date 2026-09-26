#ifndef PLOTROWDELEGATE_H
#define PLOTROWDELEGATE_H

#include <QPointer>
#include <QRect>
#include <QString>
#include <QStyledItemDelegate>

#include "demandstate.h"
#include "ui/docks/DemandIndicator.h"
#include "ui/docks/plotselection/PlotRowLayout.h"

class QAbstractItemView;
class QPainter;

namespace FlySight {

class CalculationDemand;

/// The plot list's row delegate: it paints what CalculationDemand reports for
/// a row and decides nothing - no state, no count, no word of text, and nothing
/// about what to compute. Every number and every string it shows comes from
/// DemandState.
///
/// PAINTING. Right-aligned in a plot row (PlotRowLayout.h), one glyph
/// (DemandIndicatorView.h, drawn with DemandIndicator.h's glyphs): the working
/// indicator, an arc that turns about once a second, while any of the plot's
/// demand is waiting or running; once the work is finished and some sessions
/// could not be computed, the warning badge instead. There is no label and no
/// count: the hover carries the numbers. The plot's name is elided to make
/// room; the glyph never is. A row whose state isPlain() - every plot that is
/// not over a requested calculation, every unchecked row, every row with
/// nothing waiting, running or failed, and every category - is painted by the
/// unmodified base class. The row height never changes: sizeHint() is not
/// overridden. The glyph is drawn with QPainter in the row's text colour, so
/// it follows the theme, the selection and the screen's scale without any
/// bundled image.
///
/// NO GESTURES. The delegate handles no event of its own: every click and key
/// is the base class's. Checking a row is the base class's write to PlotModel,
/// which the demand layer observes like every other check change (the Plots
/// menu, a profile, the start-up restore). Nothing in the row is clickable beyond what
/// QStyledItemDelegate makes clickable.
///
/// TOOLTIP. helpEvent() shows DemandState::toolTip over the whole row
/// (showIndicatorToolTip()).
///
/// REPAINT. plotStateChanged(plotId) repaints that row. While any plot is
/// working, each frame of the application's working-indicator clock (given at
/// construction; the application makes it follow the demand layer) repaints
/// the working rows (workingPlotIds()). Without a clock the arc is drawn at
/// rest and nothing repaints by itself.
///
/// The component and the clock are held weakly: without the component (never
/// given, or destroyed first) the delegate behaves exactly as
/// QStyledItemDelegate.
class PlotRowDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    /// `clock` is the application's working-indicator clock (may be null: the
    /// arc is drawn at rest); `view` is the parent, and the view whose rows are
    /// repainted.
    PlotRowDelegate(CalculationDemand *demand, WorkingAnimation *clock, QAbstractItemView *view);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override;

    /// What helpEvent() shows; empty for plain rows and categories.
    QString toolTipFor(const QModelIndex &index) const;
    /// Where the row's one glyph is painted, in viewport coordinates; null for
    /// plain rows. For tests and for nothing else.
    QRect indicatorRect(const QModelIndex &index) const;

    /// The clock it was given (null when none, or once it is destroyed). For
    /// tests and for nothing else.
    WorkingAnimation *animation() const { return m_clock.data(); }

private slots:
    void onPlotStateChanged(const QString &plotId);
    void onAnimationFrame();

private:
    /// The row of a plot id (PlotModel::PlotValueIdRole); invalid when none.
    QModelIndex indexForPlot(const QString &plotId) const;
    /// The default ("plain") state without a component or for a category.
    DemandState stateFor(const QModelIndex &index) const;
    static PlotRowMetrics metricsFor(const QStyleOptionViewItem &opt);
    /// `opt` must have been through initStyleOption().
    PlotRowGeometry geometryFor(const QStyleOptionViewItem &opt, const DemandState &state) const;

    QPointer<CalculationDemand> m_demand;
    QPointer<QAbstractItemView> m_view;
    QPointer<WorkingAnimation> m_clock;     // the application's; may be null
};

} // namespace FlySight

#endif // PLOTROWDELEGATE_H
