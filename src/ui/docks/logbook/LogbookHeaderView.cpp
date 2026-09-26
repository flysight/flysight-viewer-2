#include "LogbookHeaderView.h"

#include <QEvent>
#include <QHelpEvent>
#include <QPainter>
#include <QStringList>
#include <QStyle>
#include <QStyleOptionHeaderV2>

#include "calculationdemand.h"
#include "sessionmodel.h"
#include "ui/docks/DemandIndicator.h"
#include "ui/docks/DemandIndicatorView.h"

namespace FlySight {

LogbookHeaderView::LogbookHeaderView(SessionModel *model, CalculationDemand *demand, WorkingAnimation *clock,
                                     QWidget *parent)
    : QHeaderView(Qt::Horizontal, parent)
    , m_model(model)
    , m_demand(demand)
    , m_clock(clock)
{
    // What QTreeView gives its own header; QTreeView::setHeader() adds the rest
    setSectionsMovable(true);
    setStretchLastSection(true);
    setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    if (m_demand) {
        connect(m_demand, &CalculationDemand::columnStateChanged,
                this, &LogbookHeaderView::onColumnStateChanged);
        // Without the demand layer every section is plain at once
        repaintWhenDemandDestroyed(m_demand, viewport());
        // The clock follows the demand layer where it was made (followDemand());
        // this header only repaints its own working sections on its frames
        if (m_clock)
            connect(m_clock, &WorkingAnimation::frameAdvanced,
                    this, &LogbookHeaderView::onAnimationFrame);
    }
}

// ---- Mapping --------------------------------------------------------------------

// Computed on every call: sorting and rebuilding the columns both reset the
// model, so a cached mapping would go stale
QString LogbookHeaderView::columnIdOf(int logicalIndex) const
{
    if (!m_model || model() != m_model.data() || logicalIndex < 0 || logicalIndex >= m_model->columnCount())
        return QString();
    return CalculationDemand::columnId(m_model->column(logicalIndex));
}

DemandState LogbookHeaderView::stateFor(int logicalIndex) const
{
    if (!m_demand)
        return DemandState();
    const QString id = columnIdOf(logicalIndex);
    if (id.isEmpty())
        return DemandState();
    return m_demand->columnState(id);
}

QString LogbookHeaderView::toolTipForSection(int logicalIndex) const
{
    return stateFor(logicalIndex).toolTip;
}

// ---- Layout ---------------------------------------------------------------------

// As QHeaderView::paintSection() builds it. True when the section's brushes
// differ from the header's, so that the brush origin must move to the section.
bool LogbookHeaderView::initSectionOption(QStyleOptionHeaderV2 &opt, int logicalIndex, const QRect &rect) const
{
    initStyleOption(&opt);
    const QBrush buttonBefore = opt.palette.brush(QPalette::Button);
    const QBrush windowBefore = opt.palette.brush(QPalette::Window);
    initStyleOptionForIndex(&opt, logicalIndex);
    opt.rect = rect;
    return buttonBefore != opt.palette.brush(QPalette::Button) || windowBefore != opt.palette.brush(QPalette::Window);
}

QRect LogbookHeaderView::layoutGlyph(QStyleOptionHeaderV2 &opt) const
{
    // SE_HeaderLabel already excludes the sort arrow when the section shows one
    const QRect label = style()->subElementRect(QStyle::SE_HeaderLabel, &opt, this);
    const GlyphMetrics metrics = glyphMetrics(opt.fontMetrics, label.height());
    if (metrics.side <= 0 || label.width() < metrics.side)
        return QRect();

    // The text is centred: a symmetric reserve keeps it centred and clear of
    // the glyph. CE_HeaderLabel would elide "name\n(unit)" as one string
    // against one width, so each line is elided here and the style elides
    // nothing more.
    const int textWidth = qMax(0, label.width() - 2 * metrics.reserve());
    QStringList lines = opt.text.split(QLatin1Char('\n'));
    int block = 0;
    for (QString &line : lines) {
        line = opt.fontMetrics.elidedText(line, Qt::ElideRight, textWidth);
        block = qMax(block, opt.fontMetrics.horizontalAdvance(line));
    }
    opt.text = lines.join(QLatin1Char('\n'));
    opt.textElideMode = Qt::ElideNone;

    const int y = label.y() + (label.height() - metrics.side) / 2;
    int x = 0;
    if (opt.direction == Qt::RightToLeft) {
        x = label.center().x() - (block + 1) / 2 - metrics.spacing - metrics.side;
        x = qMax(x, label.left());
    } else {
        x = label.center().x() + (block + 1) / 2 + metrics.spacing;
        x = qMin(x, label.right() + 1 - metrics.side);
    }
    return QRect(x, y, metrics.side, metrics.side);
}

QRect LogbookHeaderView::sectionViewportRect(int logicalIndex) const
{
    // As QHeaderView::paintEvent() places a section
    const int rtlOffset = isRightToLeft() ? 1 : 0;
    return QRect(sectionViewportPosition(logicalIndex) + rtlOffset, 0, sectionSize(logicalIndex),
                 viewport()->height());
}

QRect LogbookHeaderView::indicatorRect(int logicalIndex) const
{
    if (logicalIndex < 0 || logicalIndex >= count() || isSectionHidden(logicalIndex))
        return QRect();
    if (columnIdOf(logicalIndex).isEmpty() || stateFor(logicalIndex).isPlain())
        return QRect();
    const QRect rect = sectionViewportRect(logicalIndex);
    if (!rect.isValid())
        return QRect();
    QStyleOptionHeaderV2 opt;
    initSectionOption(opt, logicalIndex, rect);
    return layoutGlyph(opt);
}

QSize LogbookHeaderView::sectionSizeFromContents(int logicalIndex) const
{
    QSize size = QHeaderView::sectionSizeFromContents(logicalIndex);
    if (m_demand) {
        const QString id = columnIdOf(logicalIndex);
        if (!id.isEmpty() && m_demand->columnState(id).requested) {
            // Room for the glyph on both sides of the centred text
            size.rwidth() += 2 * glyphMetrics(fontMetrics(), fontMetrics().height()).reserve();
        }
    }
    return size;
}

// ---- Painting -------------------------------------------------------------------

void LogbookHeaderView::paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const
{
    const DemandState state = stateFor(logicalIndex);
    if (state.isPlain() || !rect.isValid()) {
        // Exactly today's section
        QHeaderView::paintSection(painter, rect, logicalIndex);
        return;
    }

    QStyleOptionHeaderV2 opt;
    const QPointF oldBrushOrigin = painter->brushOrigin();
    if (initSectionOption(opt, logicalIndex, rect))
        painter->setBrushOrigin(opt.rect.topLeft());
    const QRect glyph = layoutGlyph(opt);
    style()->drawControl(QStyle::CE_Header, &opt, painter, this);
    painter->setBrushOrigin(oldBrushOrigin);

    if (glyph.isNull())
        return;
    painter->save();
    painter->setClipRect(rect);
    painter->setRenderHint(QPainter::Antialiasing);
    drawDemandGlyph(painter, QRectF(glyph), state, glyphColor(opt), m_clock);
    painter->restore();
}

// ---- Hover ----------------------------------------------------------------------

bool LogbookHeaderView::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *helpEvent = static_cast<QHelpEvent *>(event);
        const int logical = logicalIndexAt(helpEvent->pos());
        // The whole section shows the state's tooltip, which hides when the
        // pointer leaves the section; a plain section keeps the base's
        if (showIndicatorToolTip(helpEvent, stateFor(logical), viewport(), sectionViewportRect(logical)))
            return true;
    }
    return QHeaderView::viewportEvent(event);
}

// ---- Repaint --------------------------------------------------------------------

void LogbookHeaderView::onColumnStateChanged(const QString &columnId)
{
    for (int logical = 0; logical < count(); ++logical) {
        if (!isSectionHidden(logical) && columnIdOf(logical) == columnId)
            updateSection(logical);
    }
}

// Only the working sections show the arc: only they are repainted
void LogbookHeaderView::onAnimationFrame()
{
    if (!m_demand)
        return;
    const QStringList working = m_demand->workingColumnIds();
    if (working.isEmpty())
        return;
    for (int logical = 0; logical < count(); ++logical) {
        if (!isSectionHidden(logical) && working.contains(columnIdOf(logical)))
            updateSection(logical);
    }
}

} // namespace FlySight
