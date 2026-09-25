#include "PlotRowDelegate.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QHelpEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QToolTip>
#include <QtMath>

#include "plotmodel.h"

namespace FlySight {

namespace {

QPen glyphPen(const QColor &color, qreal width)
{
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

qreal glyphPenWidth(const QRectF &rect)
{
    return qMax(1.0, rect.width() / 9.0);
}

/// A filled amber triangle with a dark exclamation mark. The colours are fixed:
/// readable on light, dark and highlight backgrounds alike.
void drawWarningGlyph(QPainter *painter, const QRectF &rect)
{
    const QColor amber(0xE6, 0x9F, 0x00);
    const QColor mark(0x20, 0x20, 0x20);
    const qreal penWidth = glyphPenWidth(rect);
    const QRectF inner = rect.adjusted(penWidth, penWidth, -penWidth, -penWidth);

    QPainterPath triangle;
    triangle.moveTo(inner.center().x(), inner.top());
    triangle.lineTo(inner.right(), inner.bottom());
    triangle.lineTo(inner.left(), inner.bottom());
    triangle.closeSubpath();
    painter->setPen(glyphPen(amber, penWidth));
    painter->setBrush(amber);
    painter->drawPath(triangle);

    const qreal x = rect.center().x();
    painter->setPen(glyphPen(mark, penWidth));
    painter->drawLine(QPointF(x, rect.top() + rect.height() * 0.40), QPointF(x, rect.top() + rect.height() * 0.62));
    painter->setPen(Qt::NoPen);
    painter->setBrush(mark);
    painter->drawEllipse(QPointF(x, rect.top() + rect.height() * 0.80), penWidth * 0.6, penWidth * 0.6);
}

/// The row's text colour, chosen as QStyledItemDelegate chooses it.
QColor glyphColor(const QStyleOptionViewItem &opt)
{
    QPalette::ColorGroup group = (opt.state & QStyle::State_Enabled) ? QPalette::Normal : QPalette::Disabled;
    if (group == QPalette::Normal && !(opt.state & QStyle::State_Active))
        group = QPalette::Inactive;
    const bool selected = opt.state & QStyle::State_Selected;
    return opt.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text);
}

} // namespace

PlotRowDelegate::PlotRowDelegate(CalculationDemand *demand, QAbstractItemView *view)
    : QStyledItemDelegate(view)
    , m_demand(demand)
    , m_view(view)
{
    if (m_demand && m_view) {
        connect(m_demand, &CalculationDemand::plotStateChanged,
                this, &PlotRowDelegate::onPlotStateChanged);
    }
}

void PlotRowDelegate::drawWorkingGlyph(QPainter *painter, const QRectF &rect, const QColor &color,
                                       qreal rotationDegrees)
{
    const qreal penWidth = glyphPenWidth(rect);
    const QRectF circle = rect.adjusted(penWidth, penWidth, -penWidth, -penWidth);
    painter->setPen(glyphPen(color, penWidth));
    painter->setBrush(Qt::NoBrush);
    // Qt angles: sixteenths of a degree, counter-clockwise from three o'clock.
    // The gap of 90 degrees starts at twelve o'clock, turned clockwise by the
    // rotation.
    const int start = qRound((90.0 - rotationDegrees) * 16.0);
    painter->drawArc(circle, start, -270 * 16);
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
    PlotRowMetrics metrics;
    metrics.iconSide = qMin(opt.rect.height() - 2, opt.fontMetrics.height());
    metrics.spacing = qMax(2, metrics.iconSide / 4);
    metrics.rightMargin = metrics.spacing;
    return metrics;
}

// The badge and the indicator are exclusive (spec section 10): while working,
// failures appear in the tooltip only.
PlotRowGeometry PlotRowDelegate::geometryFor(const QStyleOptionViewItem &opt, const DemandState &state) const
{
    const bool showsIndicator = state.isWorking();
    const bool showsWarning = state.showsWarning();
    const int labelWidth = showsIndicator ? opt.fontMetrics.horizontalAdvance(state.progressLabel) : 0;
    const int countWidth = showsWarning
        ? opt.fontMetrics.horizontalAdvance(QString::number(state.failedCount)) : 0;
    return layoutPlotRow(opt.rect, metricsFor(opt), showsWarning, countWidth,
                         showsIndicator, labelWidth, opt.direction);
}

QRect PlotRowDelegate::clusterRect(const QModelIndex &index) const
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
    const PlotRowGeometry geometry = geometryFor(opt, state);
    QRect cluster;
    for (const QRect &rect : {geometry.warningIcon, geometry.warningCount,
                              geometry.progressLabel, geometry.indicatorIcon}) {
        if (!rect.isNull())
            cluster = cluster.united(rect);
    }
    return cluster;
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

    // The name gives way, never the cluster. The style still paints the whole
    // item over the full rect - background, selection, hover, focus, check
    // box, text - so the highlight runs under the cluster.
    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
    const int textMargin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, widget) + 1;
    const int available = textRect.width() - (geometry.clusterWidth + metrics.spacing) - 2 * textMargin;
    opt.text = opt.fontMetrics.elidedText(opt.text, Qt::ElideRight, qMax(0, available));
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    painter->save();
    painter->setClipRect(opt.rect);
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setFont(opt.font);

    const QColor color = glyphColor(opt);

    if (state.isWorking()) {
        drawWorkingGlyph(painter, QRectF(geometry.indicatorIcon), color, 0.0);
        if (!geometry.progressLabel.isNull()) {
            painter->setPen(color);
            painter->drawText(geometry.progressLabel, Qt::AlignVCenter | Qt::AlignRight, state.progressLabel);
        }
    } else if (state.showsWarning()) {
        drawWarningGlyph(painter, QRectF(geometry.warningIcon));
        painter->setPen(color);
        painter->drawText(geometry.warningCount, Qt::AlignVCenter | Qt::AlignLeft,
                          QString::number(state.failedCount));
    }

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
    if (event && event->type() == QEvent::ToolTip && view) {
        // Plain text as CalculationDemand built it: the whole row shows it
        const QString text = toolTipFor(index);
        if (!text.isEmpty()) {
            QToolTip::showText(event->globalPos(), text, view->viewport(), option.rect);
            return true;
        }
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

// ---- Repaint --------------------------------------------------------------------

void PlotRowDelegate::onPlotStateChanged(const QString &plotId)
{
    if (!m_view || !m_view->model())
        return;
    const QAbstractItemModel *model = m_view->model();
    if (model->rowCount() == 0)
        return;

    const QModelIndexList found = model->match(model->index(0, 0), PlotModel::PlotValueIdRole, plotId, 1,
                                               Qt::MatchExactly | Qt::MatchRecursive);
    if (!found.isEmpty() && found.first().isValid())
        m_view->update(found.first());      // harmless for a row that is scrolled out or collapsed
}

} // namespace FlySight
