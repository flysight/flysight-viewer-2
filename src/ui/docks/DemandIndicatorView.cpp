#include "DemandIndicatorView.h"

#include <QHelpEvent>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QStyle>
#include <QStyleOptionHeader>
#include <QStyleOptionViewItem>
#include <QToolTip>
#include <QWidget>

#include "calculationdemand.h"
#include "demandstate.h"
#include "ui/docks/DemandIndicator.h"

namespace FlySight {

namespace {

/// The colour group the style paints the option's text in, as
/// QStyledItemDelegate and CE_HeaderLabel choose it.
QPalette::ColorGroup colorGroup(const QStyleOption &option)
{
    if (!(option.state & QStyle::State_Enabled))
        return QPalette::Disabled;
    if (!(option.state & QStyle::State_Active))
        return QPalette::Inactive;
    return QPalette::Normal;
}

} // namespace

QColor glyphColor(const QStyleOptionViewItem &option)
{
    const bool selected = option.state & QStyle::State_Selected;
    return option.palette.color(colorGroup(option), selected ? QPalette::HighlightedText : QPalette::Text);
}

QColor glyphColor(const QStyleOptionHeader &option)
{
    return option.palette.color(colorGroup(option), QPalette::ButtonText);
}

void drawDemandGlyph(QPainter *painter, const QRectF &rect, const DemandState &state,
                     const QColor &color, const WorkingAnimation *clock)
{
    if (state.isWorking())
        drawWorkingGlyph(painter, rect, color, clock ? clock->angle() : 0.0);
    else if (state.showsWarning())
        drawWarningGlyph(painter, rect);
}

bool showIndicatorToolTip(const QHelpEvent *event, const DemandState &state, QWidget *widget, const QRect &area)
{
    if (!event || !widget || state.toolTip.isEmpty())
        return false;
    // Plain text as DemandState built it, over the whole area
    QToolTip::showText(event->globalPos(), state.toolTip, widget, area);
    return true;
}

void repaintWhenDemandDestroyed(CalculationDemand *demand, QWidget *widget)
{
    if (demand && widget)
        QObject::connect(demand, &QObject::destroyed, widget, [widget] { widget->update(); });
}

void followDemand(WorkingAnimation *clock, CalculationDemand *demand)
{
    if (!clock)
        return;
    // During destroyed() the pointer is already null: the clock stops at frame 0
    const QPointer<CalculationDemand> weak(demand);
    const auto sync = [clock, weak] {
        clock->setActive(weak && (!weak->workingPlotIds().isEmpty() || !weak->workingColumnIds().isEmpty()));
    };
    if (demand) {
        QObject::connect(demand, &CalculationDemand::statesChanged, clock, sync);
        QObject::connect(demand, &QObject::destroyed, clock, sync);
    }
    sync();
}

} // namespace FlySight
