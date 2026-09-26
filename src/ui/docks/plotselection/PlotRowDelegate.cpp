#include "PlotRowDelegate.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QHelpEvent>
#include <QPainter>
#include <QStyle>

#include "calculationdemand.h"
#include "plotmodel.h"
#include "ui/docks/DemandIndicator.h"
#include "ui/docks/DemandIndicatorView.h"

namespace FlySight {

PlotRowDelegate::PlotRowDelegate(CalculationDemand *demand, WorkingAnimation *clock, QAbstractItemView *view)
    : QStyledItemDelegate(view)
    , m_demand(demand)
    , m_view(view)
    , m_clock(clock)
{
    if (m_demand && m_view) {
        connect(m_demand, &CalculationDemand::plotStateChanged,
                this, &PlotRowDelegate::onPlotStateChanged);
        // Without the demand layer every row is plain at once
        repaintWhenDemandDestroyed(m_demand, m_view->viewport());
        // The clock follows the demand layer where it was made (followDemand());
        // this delegate only repaints its own working rows on its frames
        if (m_clock)
            connect(m_clock, &WorkingAnimation::frameAdvanced,
                    this, &PlotRowDelegate::onAnimationFrame);
    }
}

// ---- State and geometry ---------------------------------------------------------

DemandState PlotRowDelegate::stateFor(const QModelIndex &index) const
{
    if (!m_demand)
        return DemandState();
    const QString id = index.data(PlotModel::PlotValueIdRole).toString();
    if (id.isEmpty())
        return DemandState();       // a category
    return m_demand->plotState(id);
}

PlotRowMetrics PlotRowDelegate::metricsFor(const QStyleOptionViewItem &opt)
{
    const GlyphMetrics glyph = glyphMetrics(opt.fontMetrics, opt.rect.height() - 2);
    PlotRowMetrics metrics;
    metrics.iconSide = glyph.side;
    metrics.spacing = glyph.spacing;
    metrics.rightMargin = glyph.spacing;
    return metrics;
}

PlotRowGeometry PlotRowDelegate::geometryFor(const QStyleOptionViewItem &opt, const DemandState &state) const
{
    return layoutPlotRow(opt.rect, metricsFor(opt), !state.isPlain(), opt.direction);
}

QRect PlotRowDelegate::indicatorRect(const QModelIndex &index) const
{
    if (!m_view || !index.isValid())
        return QRect();
    const DemandState state = stateFor(index);
    if (state.isPlain())
        return QRect();
    QStyleOptionViewItem opt;
    opt.initFrom(m_view);
    opt.rect = m_view->visualRect(index);
    initStyleOption(&opt, index);
    return geometryFor(opt, state).glyph;
}

// ---- Painting -------------------------------------------------------------------

void PlotRowDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const DemandState state = stateFor(index);
    if (state.isPlain()) {
        // Exactly today's row
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QWidget *widget = opt.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();

    const PlotRowMetrics metrics = metricsFor(opt);
    const PlotRowGeometry geometry = geometryFor(opt, state);

    // The name gives way, never the glyph. The style still paints the whole
    // item over the full rect - background, selection, hover, focus, check
    // box, text - so the highlight runs under the glyph.
    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
    const int textMargin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, widget) + 1;
    const int available = textRect.width() - (geometry.reservedWidth + metrics.spacing) - 2 * textMargin;
    opt.text = opt.fontMetrics.elidedText(opt.text, Qt::ElideRight, qMax(0, available));
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    painter->save();
    painter->setClipRect(opt.rect);
    painter->setRenderHint(QPainter::Antialiasing);
    drawDemandGlyph(painter, QRectF(geometry.glyph), state, glyphColor(opt), m_clock);
    painter->restore();
}

// ---- Tooltip --------------------------------------------------------------------

QString PlotRowDelegate::toolTipFor(const QModelIndex &index) const
{
    return stateFor(index).toolTip;
}

bool PlotRowDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                                const QModelIndex &index)
{
    // The whole row shows the state's tooltip; a plain row keeps the base's
    if (event && event->type() == QEvent::ToolTip && view
        && showIndicatorToolTip(event, stateFor(index), view->viewport(), option.rect))
        return true;
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

// ---- Repaint --------------------------------------------------------------------

QModelIndex PlotRowDelegate::indexForPlot(const QString &plotId) const
{
    if (!m_view || !m_view->model())
        return QModelIndex();
    const QAbstractItemModel *model = m_view->model();
    if (model->rowCount() == 0)
        return QModelIndex();
    const QModelIndexList found = model->match(model->index(0, 0), PlotModel::PlotValueIdRole, plotId, 1,
                                               Qt::MatchExactly | Qt::MatchRecursive);
    return found.isEmpty() ? QModelIndex() : found.first();
}

void PlotRowDelegate::onPlotStateChanged(const QString &plotId)
{
    const QModelIndex index = indexForPlot(plotId);
    if (index.isValid())
        m_view->update(index);      // harmless for a row that is scrolled out or collapsed
}

// Only the working rows show the arc: only they are repainted. With only a
// column working there is nothing here to repaint.
void PlotRowDelegate::onAnimationFrame()
{
    if (!m_demand || !m_view)
        return;
    const QStringList working = m_demand->workingPlotIds();
    for (const QString &plotId : working) {
        const QModelIndex index = indexForPlot(plotId);
        if (index.isValid())
            m_view->update(index);
    }
}

} // namespace FlySight
