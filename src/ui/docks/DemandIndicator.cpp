#include "DemandIndicator.h"

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>

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

} // namespace

void drawWorkingGlyph(QPainter *painter, const QRectF &rect, const QColor &color, qreal rotationDegrees)
{
    painter->save();
    // The turn is the painter's: QPainter::rotate() turns clockwise on screen
    // for positive angles, and rotation 0 leaves the arc exactly as at rest
    painter->translate(rect.center());
    painter->rotate(rotationDegrees);
    painter->translate(-rect.center());

    const qreal penWidth = glyphPenWidth(rect);
    const QRectF circle = rect.adjusted(penWidth, penWidth, -penWidth, -penWidth);
    painter->setPen(glyphPen(color, penWidth));
    painter->setBrush(Qt::NoBrush);
    // Qt angles: sixteenths of a degree, counter-clockwise from three o'clock.
    // The gap of 90 degrees starts at twelve o'clock.
    painter->drawArc(circle, 90 * 16, -270 * 16);

    painter->restore();
}

void drawWarningGlyph(QPainter *painter, const QRectF &rect)
{
    painter->save();

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

    painter->restore();
}

// ---- WorkingAnimation -----------------------------------------------------------

WorkingAnimation::WorkingAnimation(QObject *parent)
    : QObject(parent)
{
    m_timer.setInterval(kFrameIntervalMs);
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &WorkingAnimation::advance);
}

void WorkingAnimation::setActive(bool active)
{
    m_active = active;
    if (!active) {
        // At rest the indicator is the arc at rotation 0; nothing is announced,
        // since nothing that is working is left to repaint
        m_timer.stop();
        m_frame = 0;
        return;
    }
    if (!m_frozen && !m_timer.isActive())
        m_timer.start();
}

bool WorkingAnimation::isTicking() const
{
    return m_timer.isActive();
}

qreal WorkingAnimation::angle() const
{
    return m_frame * 360.0 / kFramesPerTurn;
}

void WorkingAnimation::setFrozen(bool frozen)
{
    m_frozen = frozen;
    if (frozen)
        m_timer.stop();
    else if (m_active && !m_timer.isActive())
        m_timer.start();
}

void WorkingAnimation::advance()
{
    m_frame = (m_frame + 1) % kFramesPerTurn;
    emit frameAdvanced();
}

} // namespace FlySight
