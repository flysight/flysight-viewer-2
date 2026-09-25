#include "LogbookHeaderView.h"

#include <QEvent>
#include <QHelpEvent>
#include <QPainter>
#include <QStringList>
#include <QStyle>
#include <QStyleOptionHeaderV2>
#include <QToolTip>

#include "sessionmodel.h"
#include "ui/docks/DemandIndicator.h"

namespace FlySight {

namespace {

/// The header text's colour (CE_HeaderLabel draws with ButtonText), in the
/// colour group the section is painted in.
QColor glyphColor(const QStyleOptionHeader &opt)
{
    QPalette::ColorGroup group = QPalette::Normal;
    if (!(opt.state & QStyle::State_Enabled))
        group = QPalette::Disabled;
    else if (!(opt.state & QStyle::State_Active))
        group = QPalette::Inactive;
    return opt.palette.color(group, QPalette::ButtonText);
}

/// The glyph's side and the room it takes beside the text, for one text line
/// of `lineHeight` in a label `labelHeight` tall: the plot rows' rule.
struct GlyphMetrics {
    int side = 0;
    int spacing = 0;
    int reserve = 0;        // side + spacing
};

GlyphMetrics glyphMetrics(int labelHeight, int lineHeight)
{
    GlyphMetrics metrics;
    metrics.side = qMin(labelHeight, lineHeight);
    metrics.spacing = qMax(2, metrics.side / 4);
    metrics.reserve = metrics.side + metrics.spacing;
    return metrics;
}

} // namespace

LogbookHeaderView::LogbookHeaderView(SessionModel *model, CalculationDemand *demand, QWidget *parent)
    : QHeaderView(Qt::Horizontal, parent)
    , m_model(model)
    , m_demand(demand)
    , m_animation(new WorkingAnimation(this))
{
    // What QTreeView gives its own header; QTreeView::setHeader() adds the rest
    setSectionsMovable(true);
    setStretchLastSection(true);
    setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    if (m_demand) {
        connect(m_demand, &CalculationDemand::columnStateChanged,
                this, &LogbookHeaderView::onColumnStateChanged);
        connect(m_demand, &CalculationDemand::statesChanged,
                this, &LogbookHeaderView::syncAnimation);
        connect(m_animation, &WorkingAnimation::frameAdvanced,
                this, &LogbookHeaderView::onAnimationFrame);
        // Without the demand layer every section is plain at once
        connect(m_demand, &QObject::destroyed, this, [this] {
            syncAnimation();
            viewport()->update();
        });
    }
    // Columns may already be working when the view is built
    syncAnimation();
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
    const GlyphMetrics metrics = glyphMetrics(label.height(), opt.fontMetrics.height());
    if (metrics.side <= 0 || label.width() < metrics.side)
        return QRect();

    // The text is centred: a symmetric reserve keeps it centred and clear of
    // the glyph. CE_HeaderLabel would elide "name\n(unit)" as one string
    // against one width, so each line is elided here and the style elides
    // nothing more.
    const int textWidth = qMax(0, label.width() - 2 * metrics.reserve);
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
            const int line = fontMetrics().height();
            size.rwidth() += 2 * glyphMetrics(line, line).reserve;
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
    if (state.isWorking())
        drawWorkingGlyph(painter, QRectF(glyph), glyphColor(opt), m_animation->angle());
    else
        drawWarningGlyph(painter, QRectF(glyph));       // showsWarning(): the badge replaces the indicator
    painter->restore();
}

// ---- Hover ----------------------------------------------------------------------

bool LogbookHeaderView::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *helpEvent = static_cast<QHelpEvent *>(event);
        const int logical = logicalIndexAt(helpEvent->pos());
        const QString text = toolTipForSection(logical);
        if (!text.isEmpty()) {
            // Plain text as CalculationDemand built it: the whole section shows
            // it, and it hides when the pointer leaves the section
            QToolTip::showText(helpEvent->globalPos(), text, viewport(), sectionViewportRect(logical));
            return true;
        }
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

void LogbookHeaderView::syncAnimation()
{
    m_animation->setActive(m_demand && !m_demand->workingColumnIds().isEmpty());
}

// Only the working sections show the arc: only they are repainted
void LogbookHeaderView::onAnimationFrame()
{
    if (!m_demand) {
        syncAnimation();
        return;
    }
    const QStringList working = m_demand->workingColumnIds();
    if (working.isEmpty())
        return;
    for (int logical = 0; logical < count(); ++logical) {
        if (!isSectionHidden(logical) && working.contains(columnIdOf(logical)))
            updateSection(logical);
    }
}

} // namespace FlySight
