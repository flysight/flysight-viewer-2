#include "LogbookCellDelegate.h"

#include <QApplication>
#include <QHelpEvent>
#include <QStyle>
#include <QToolTip>
#include <QTreeView>

#include "calculationdemand.h"
#include "sessionmodel.h"

namespace FlySight {

LogbookCellDelegate::LogbookCellDelegate(SessionModel *model, CalculationDemand *demand, QTreeView *view)
    : QStyledItemDelegate(view)
    , m_model(model)
    , m_demand(demand)
    , m_view(view)
{
    if (m_demand && m_view) {
        connect(m_demand, &CalculationDemand::columnStateChanged,
                this, &LogbookCellDelegate::onColumnStateChanged);
        // Without the demand layer no cell is pending any more
        connect(m_demand, &QObject::destroyed, this, [this] {
            if (m_view)
                m_view->viewport()->update();
        });
    }
}

QString LogbookCellDelegate::pendingText()
{
    return QString(QChar(0x2026));
}

QString LogbookCellDelegate::pendingToolTip()
{
    return tr("Pending: this value is being computed");
}

// A value always wins: a loaded row reads a just-written record live before
// the demand layer's next pass drops the cell from pending
bool LogbookCellDelegate::showsPending(const QModelIndex &index) const
{
    return m_demand && m_model && index.isValid() && index.model() == m_model.data()
        && m_demand->isCellPending(index.row(), index.column())
        && index.data(Qt::DisplayRole).toString().isEmpty();
}

void LogbookCellDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (!showsPending(index)) {
        // Exactly today's cell
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text = pendingText();
    opt.features |= QStyleOptionViewItem::HasDisplay;

    // Muted in every colour group: the placeholder colour, and the selection's
    // text colour at 60 % on a selected row. The alignment is the cell's own,
    // where the value will appear.
    QPalette::ColorGroup group = QPalette::Normal;
    if (!(opt.state & QStyle::State_Enabled))
        group = QPalette::Disabled;
    else if (!(opt.state & QStyle::State_Active))
        group = QPalette::Inactive;
    opt.palette.setColor(QPalette::Text, opt.palette.color(group, QPalette::PlaceholderText));
    QColor selected = opt.palette.color(group, QPalette::HighlightedText);
    selected.setAlphaF(0.6f);
    opt.palette.setColor(QPalette::HighlightedText, selected);

    const QWidget *widget = opt.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
}

bool LogbookCellDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                                    const QModelIndex &index)
{
    if (event && event->type() == QEvent::ToolTip && view && showsPending(index)) {
        QToolTip::showText(event->globalPos(), pendingToolTip(), view->viewport(), option.rect);
        return true;
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

// One repaint of the visible part of one column per pass that changed it
void LogbookCellDelegate::onColumnStateChanged(const QString &columnId)
{
    if (!m_view || !m_model)
        return;
    QWidget *viewport = m_view->viewport();
    for (int column = 0; column < m_model->columnCount(); ++column) {
        if (m_view->isColumnHidden(column) || CalculationDemand::columnId(m_model->column(column)) != columnId)
            continue;
        viewport->update(QRect(m_view->columnViewportPosition(column), 0, m_view->columnWidth(column),
                               viewport->height()));
    }
}

} // namespace FlySight
