#include "PlotRowDelegate.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QToolTip>
#include <QtMath>

#include "plotmodel.h"

namespace FlySight {

namespace {

using Control = PlotRowState::Control;

QPen glyphPen(const QColor &color, qreal width)
{
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

qreal glyphPenWidth(const QRectF &rect)
{
    return qMax(1.0, rect.width() / 9.0);
}

/// A point of the ellipse inscribed in `rect`, at a Qt angle (degrees,
/// counter-clockwise from three o'clock).
QPointF pointOnArc(const QRectF &rect, qreal degrees)
{
    const qreal radians = qDegreesToRadians(degrees);
    return QPointF(rect.center().x() + rect.width() / 2.0 * qCos(radians),
                   rect.center().y() - rect.height() / 2.0 * qSin(radians));
}

/// Circular arrows: two arcs of 140 degrees, each ending in a filled arrowhead
/// tangent to the arc.
void drawRefreshGlyph(QPainter *painter, const QRectF &rect, const QColor &color)
{
    const qreal penWidth = glyphPenWidth(rect);
    const QRectF circle = rect.adjusted(penWidth, penWidth, -penWidth, -penWidth);
    const qreal headLength = rect.width() * 0.28;
    const qreal sweep = 140.0;

    for (const qreal start : {20.0, 200.0}) {
        QPainterPath arc;
        arc.arcMoveTo(circle, start);
        arc.arcTo(circle, start, sweep);
        painter->setPen(glyphPen(color, penWidth));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(arc);

        // The head continues the arc: its base is centred on the arc's end,
        // across the arc; its tip lies ahead, along the tangent.
        const qreal end = qDegreesToRadians(start + sweep);
        const QPointF base = pointOnArc(circle, start + sweep);
        const QPointF tangent(-qSin(end), -qCos(end));      // counter-clockwise, in screen coordinates
        const QPointF radial(qCos(end), -qSin(end));
        QPainterPath head;
        head.moveTo(base + tangent * headLength);
        head.lineTo(base + radial * (headLength * 0.6));
        head.lineTo(base - radial * (headLength * 0.6));
        head.closeSubpath();
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawPath(head);
    }
}

/// A circled x.
void drawCancelGlyph(QPainter *painter, const QRectF &rect, const QColor &color)
{
    const qreal penWidth = glyphPenWidth(rect);
    painter->setPen(glyphPen(color, penWidth));
    painter->setBrush(Qt::NoBrush);
    painter->drawEllipse(rect.adjusted(penWidth, penWidth, -penWidth, -penWidth));

    // The diagonals span the middle 40% of the rect
    const QPointF centre = rect.center();
    const qreal dx = rect.width() * 0.2;
    const qreal dy = rect.height() * 0.2;
    painter->drawLine(centre + QPointF(-dx, -dy), centre + QPointF(dx, dy));
    painter->drawLine(centre + QPointF(-dx, dy), centre + QPointF(dx, -dy));
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

bool isLeftButtonMouseEvent(const QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
        return static_cast<const QMouseEvent *>(event)->button() == Qt::LeftButton;
    default:
        return false;
    }
}

} // namespace

PlotRowDelegate::PlotRowDelegate(PlotRequests *requests, QAbstractItemView *view)
    : QStyledItemDelegate(view)
    , m_requests(requests)
    , m_view(view)
{
    if (m_requests && m_view) {
        connect(m_requests, &PlotRequests::rowStateChanged,
                this, &PlotRowDelegate::onRowStateChanged);
    }
}

// ---- State and geometry ---------------------------------------------------------

PlotRowState PlotRowDelegate::stateFor(const QModelIndex &index) const
{
    if (!m_requests)
        return PlotRowState();
    const QString id = index.data(PlotModel::PlotValueIdRole).toString();
    if (id.isEmpty())
        return PlotRowState();      // a category
    return m_requests->rowState(id);
}

PlotRowMetrics PlotRowDelegate::metricsFor(const QStyleOptionViewItem &opt)
{
    PlotRowMetrics metrics;
    metrics.iconSide = qMin(opt.rect.height() - 2, opt.fontMetrics.height());
    metrics.spacing = qMax(2, metrics.iconSide / 4);
    metrics.rightMargin = metrics.spacing;
    return metrics;
}

QString PlotRowDelegate::controlLabelFor(const PlotRowState &state)
{
    switch (state.control()) {
    case Control::Refresh: return QString::number(state.controlCount());
    case Control::Cancel:  return state.progressLabel;
    case Control::None:    break;
    }
    return QString();
}

PlotRowGeometry PlotRowDelegate::geometryFor(const QStyleOptionViewItem &opt, const PlotRowState &state) const
{
    const bool showsControl = state.control() != Control::None;
    const int labelWidth = showsControl ? opt.fontMetrics.horizontalAdvance(controlLabelFor(state)) : 0;
    const int countWidth = state.showsWarning()
        ? opt.fontMetrics.horizontalAdvance(QString::number(state.failedCount)) : 0;
    return layoutPlotRow(opt.rect, metricsFor(opt), state.showsWarning(), countWidth,
                         showsControl, labelWidth, opt.direction);
}

QRect PlotRowDelegate::controlRect(const QModelIndex &index) const
{
    if (!m_view || !index.isValid())
        return QRect();
    QStyleOptionViewItem opt;
    opt.initFrom(m_view);
    opt.rect = m_view->visualRect(index);
    initStyleOption(&opt, index);
    return geometryFor(opt, stateFor(index)).controlHit;
}

// ---- Painting -------------------------------------------------------------------

void PlotRowDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const PlotRowState state = stateFor(index);
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

    if (state.showsWarning()) {
        drawWarningGlyph(painter, QRectF(geometry.warningIcon));
        painter->setPen(color);
        painter->drawText(geometry.warningCount, Qt::AlignVCenter | Qt::AlignLeft,
                          QString::number(state.failedCount));
    }

    switch (state.control()) {
    case Control::Refresh:
        drawRefreshGlyph(painter, QRectF(geometry.controlIcon), color);
        break;
    case Control::Cancel:
        drawCancelGlyph(painter, QRectF(geometry.controlIcon), color);
        break;
    case Control::None:
        break;
    }
    if (!geometry.controlLabel.isNull()) {
        painter->setPen(color);
        painter->drawText(geometry.controlLabel, Qt::AlignVCenter | Qt::AlignRight, controlLabelFor(state));
    }

    painter->restore();
}

// ---- Gestures -------------------------------------------------------------------

bool PlotRowDelegate::editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                                  const QModelIndex &index)
{
    const QString id = index.data(PlotModel::PlotValueIdRole).toString();
    if (!m_requests || id.isEmpty())
        return QStyledItemDelegate::editorEvent(event, model, option, index);

    // 1. The control: press and release, both inside, same row, same control
    if (isLeftButtonMouseEvent(event)) {
        const PlotRowState state = m_requests->rowState(id);
        bool inside = false;
        if (state.control() != Control::None) {
            QStyleOptionViewItem opt = option;
            initStyleOption(&opt, index);
            const QPoint position = static_cast<const QMouseEvent *>(event)->position().toPoint();
            inside = geometryFor(opt, state).controlHit.contains(position);
        }

        // Whatever this event is, it ends a press that was waiting
        const bool wasArmed = m_pressedIndex.isValid() && m_pressedIndex == index;
        const Control pressedControl = m_pressedControl;
        m_pressedIndex = QPersistentModelIndex();
        m_pressedControl = Control::None;

        switch (event->type()) {
        case QEvent::MouseButtonPress:
            if (inside) {
                m_pressedIndex = QPersistentModelIndex(index);
                m_pressedControl = state.control();
                return true;        // like a press on the check box: the row is not selected
            }
            break;
        case QEvent::MouseButtonDblClick:
            if (inside)
                return true;        // the press is forgotten; no release follows a double click
            break;
        case QEvent::MouseButtonRelease:
            if (wasArmed) {
                if (inside && state.control() == pressedControl) {
                    if (pressedControl == Control::Refresh)
                        m_requests->refreshPressed(id);
                    else if (pressedControl == Control::Cancel)
                        m_requests->cancelPressed(id);
                }
                // A press on the control released elsewhere in the row does
                // nothing at all, not even a check toggle
                return true;
            }
            break;
        default:
            break;
        }
    } else if (event->type() == QEvent::MouseButtonPress) {
        // Any other press forgets a press that was waiting
        m_pressedIndex = QPersistentModelIndex();
        m_pressedControl = Control::None;
    }

    // 2. The check gesture: the base class writes the check state, inside this
    // call, only for a click on the check box and for Space / Select on the
    // current row. Its return value says nothing about a toggle (a press on
    // the indicator is consumed without one), so the model is compared.
    const bool wasChecked = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
    const bool handled = QStyledItemDelegate::editorEvent(event, model, option, index);
    const bool isChecked = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
    if (!wasChecked && isChecked && m_requests)
        m_requests->plotCheckedByUser(id);
    return handled;
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
        // Plain text as PlotRequests built it: the whole row shows it
        const QString text = toolTipFor(index);
        if (!text.isEmpty()) {
            QToolTip::showText(event->globalPos(), text, view->viewport(), option.rect);
            return true;
        }
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

// ---- Repaint --------------------------------------------------------------------

void PlotRowDelegate::onRowStateChanged(const QString &plotId)
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
