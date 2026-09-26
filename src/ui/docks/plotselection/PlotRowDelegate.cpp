#include "PlotRowDelegate.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QHelpEvent>
#include <QPainter>
#include <QStyle>
#include <QToolTip>

#include "plotmodel.h"
#include "ui/docks/DemandIndicator.h"

namespace FlySight {

namespace {

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
    , m_animation(new WorkingAnimation(this))
{
    if (m_demand && m_view) {
        connect(m_demand, &CalculationDemand::plotStateChanged,
                this, &PlotRowDelegate::onPlotStateChanged);
        // The clock follows whether any plot is working
        connect(m_demand, &CalculationDemand::statesChanged,
                this, &PlotRowDelegate::syncAnimation);
        // Without the demand layer every row is plain at once
        connect(m_demand, &QObject::destroyed, this, [this] {
            syncAnimation();
            if (m_view)
                m_view->viewport()->update();
        });
        connect(m_animation, &WorkingAnimation::frameAdvanced,
                this, &PlotRowDelegate::onAnimationFrame);
    }
    // States may already be working when the view is built
    syncAnimation();
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

// The badge and the indicator are exclusive (DemandState::showsWarning(); see
// docs/COMPUTED_PLOTS.md section 2): while working, failures appear in the
// tooltip only.
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
        drawWorkingGlyph(painter, QRectF(geometry.indicatorIcon), color, m_animation->angle());
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

void PlotRowDelegate::syncAnimation()
{
    m_animation->setActive(m_demand && m_view && !m_demand->workingPlotIds().isEmpty());
}

// Only the working rows show the arc: only they are repainted
void PlotRowDelegate::onAnimationFrame()
{
    if (!m_demand) {
        syncAnimation();
        return;
    }
    const QStringList working = m_demand->workingPlotIds();
    for (const QString &plotId : working) {
        const QModelIndex index = indexForPlot(plotId);
        if (index.isValid())
            m_view->update(index);
    }
}

} // namespace FlySight
