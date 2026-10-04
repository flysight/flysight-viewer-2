#include "LogbookCellDelegate.h"

#include <QApplication>
#include <QComboBox>
#include <QHeaderView>
#include <QHelpEvent>
#include <QIcon>
#include <QPainter>
#include <QStyle>
#include <QToolTip>
#include <QTreeView>

#include "attributeregistry.h"
#include "calculationdemand.h"
#include "demandstate.h"
#include "sessionmodel.h"

namespace FlySight {

namespace {

// The entry of the choice editor it opened on (setEditorData()), -1 for none
constexpr char kOpeningEntry[] = "openingEntry";

} // namespace

LogbookCellDelegate::LogbookCellDelegate(SessionModel *model, CalculationDemand *demand, QTreeView *view)
    : QStyledItemDelegate(view)
    , m_model(model)
    , m_demand(demand)
    , m_view(view)
{
    if (m_demand && m_view) {
        connect(m_demand, &CalculationDemand::pendingCellsChanged,
                this, &LogbookCellDelegate::onPendingCellsChanged);
        connect(m_demand, &CalculationDemand::failuresChanged,
                this, &LogbookCellDelegate::onFailuresChanged);
        // Without the demand layer no cell is pending and no row warns any
        // more. The viewport is the context, so nothing happens once it is gone.
        QWidget *viewport = m_view->viewport();
        connect(m_demand, &QObject::destroyed, viewport, [viewport] { viewport->update(); });
    }
}

QString LogbookCellDelegate::pendingText()
{
    return pendingMark();
}

QString LogbookCellDelegate::pendingToolTip()
{
    return tr("Pending: this value is being computed");
}

QString LogbookCellDelegate::excludedText()
{
    return tr("excluded");
}

QString LogbookCellDelegate::excludedToolTip()
{
    return tr("Not computed: background computation is switched off for this recording");
}

// A value always wins: a loaded row reads a just-written record live before
// the demand layer's next pass drops the cell from pending. The value is
// checked first: most cells have one, and then the demand layer is not asked.
bool LogbookCellDelegate::showsPending(const QModelIndex &index) const
{
    return m_demand && m_model && index.isValid() && index.model() == m_model.data()
        && index.data(Qt::DisplayRole).toString().isEmpty()
        && m_demand->isCellPending(index.row(), index.column());
}

// As for pending, a value always wins and is checked first
bool LogbookCellDelegate::showsExcluded(const QModelIndex &index) const
{
    return m_demand && m_model && index.isValid() && index.model() == m_model.data()
        && index.data(Qt::DisplayRole).toString().isEmpty()
        && m_demand->isCellExcluded(index.row(), index.column());
}

// The demand layer never files a cell as both, so the order only saves a question
LogbookCellDelegate::Placeholder LogbookCellDelegate::placeholder(const QModelIndex &index) const
{
    if (showsPending(index))
        return Placeholder::Pending;
    if (showsExcluded(index))
        return Placeholder::Excluded;
    return Placeholder::None;
}

// The column is checked first: only one cell of a row can carry the glyph,
// and then the demand layer is asked once per row
bool LogbookCellDelegate::showsWarning(const QModelIndex &index) const
{
    return m_demand && m_model && m_view && index.isValid() && index.model() == m_model.data()
        && index.column() == firstVisualColumn()
        && !m_demand->sessionFailures(m_model->rowAt(index.row()).sessionId).calculations.isEmpty();
}

// Asked of the header every time: moving, hiding and rebuilding the columns
// all repaint the tree, so the answer is current without a signal
int LogbookCellDelegate::firstVisualColumn() const
{
    if (!m_view)
        return -1;
    const QHeaderView *header = m_view->header();
    for (int visual = 0; visual < header->count(); ++visual) {
        const int logical = header->logicalIndex(visual);
        if (logical >= 0 && !header->isSectionHidden(logical))
            return logical;
    }
    return -1;
}

// The option the cell is painted with: the base class's, and for a pending or
// an excluded cell its placeholder text in the muted colour
QStyleOptionViewItem LogbookCellDelegate::cellOption(const QStyleOptionViewItem &option,
                                                     const QModelIndex &index, Placeholder placeholder) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    if (placeholder != Placeholder::None) {
        opt.text = placeholder == Placeholder::Pending ? pendingText() : excludedText();
        opt.features |= QStyleOptionViewItem::HasDisplay;

        // Muted in every colour group: the placeholder colour, and the
        // selection's text colour at 60 % on a selected row. The alignment is
        // the cell's own, where the value will appear.
        QPalette::ColorGroup group = QPalette::Normal;
        if (!(opt.state & QStyle::State_Enabled))
            group = QPalette::Disabled;
        else if (!(opt.state & QStyle::State_Active))
            group = QPalette::Inactive;
        opt.palette.setColor(QPalette::Text, opt.palette.color(group, QPalette::PlaceholderText));
        QColor selected = opt.palette.color(group, QPalette::HighlightedText);
        selected.setAlphaF(0.6f);
        opt.palette.setColor(QPalette::HighlightedText, selected);
    }
    return opt;
}

// The glyph follows the text, as a badge follows a name: the text is drawn
// in its own place, elided into what the text rectangle leaves once the
// glyph's room is taken from its trailing end, and the glyph sits right after
// the drawn text, so it is attached to what it annotates and never pinned to
// the cell's edge next to the neighbouring column. At most one text line
// tall, so it fits the row that sizeHint() gave without it.
LogbookCellDelegate::WarningLayout LogbookCellDelegate::layoutWarning(const QStyleOptionViewItem &opt)
{
    const QWidget *widget = opt.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
    const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, widget) + 1;
    const QRect inner = textRect.adjusted(margin, 0, -margin, 0);
    const int side = qMin(opt.decorationSize.height(), opt.fontMetrics.height());
    const int spacing = qMax(2, side / 4);
    const bool rightToLeft = opt.direction == Qt::RightToLeft;

    WarningLayout layout;
    layout.textArea = inner;
    if (rightToLeft)
        layout.textArea.setLeft(qMin(inner.right() + 1, inner.left() + side + spacing));
    else
        layout.textArea.setRight(qMax(inner.left() - 1, inner.right() - side - spacing));
    layout.elided = opt.fontMetrics.elidedText(opt.text, Qt::ElideRight, layout.textArea.width());
    const QRect drawn = style->itemTextRect(opt.fontMetrics, layout.textArea,
                                            int(opt.displayAlignment | Qt::TextSingleLine),
                                            opt.state & QStyle::State_Enabled, layout.elided);
    const int top = inner.center().y() - side / 2;
    if (rightToLeft)
        layout.glyph = QRect(drawn.left() - spacing - side, top, side, side);
    else
        layout.glyph = QRect(drawn.right() + 1 + spacing, top, side, side);
    return layout;
}

QRect LogbookCellDelegate::glyphRect(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    return layoutWarning(cellOption(option, index, placeholder(index))).glyph & option.rect;
}

// The option QAbstractItemView::initViewItemOption() gives the cell, as far
// as it places the decoration (a delegate cannot call it): the tree's font,
// direction and icon size, the default alignments, and the cell's rect,
// which is QTreeView::drawRow()'s for a tree without root decoration.
QRect LogbookCellDelegate::warningRect(const QModelIndex &index) const
{
    if (!showsWarning(index))
        return QRect();
    QStyleOptionViewItem opt;
    opt.initFrom(m_view);
    opt.widget = m_view;
    opt.font = m_view->font();
    if (m_view->iconSize().isValid()) {
        opt.decorationSize = m_view->iconSize();
    } else {
        const int extent = m_view->style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, m_view);
        opt.decorationSize = QSize(extent, extent);
    }
    opt.decorationPosition = QStyleOptionViewItem::Left;
    opt.decorationAlignment = Qt::AlignCenter;
    opt.displayAlignment = Qt::AlignLeft | Qt::AlignVCenter;
    opt.rect = m_view->visualRect(index);
    return glyphRect(opt, index);
}

void LogbookCellDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const Placeholder mark = placeholder(index);
    const bool warning = showsWarning(index);
    if (mark == Placeholder::None && !warning) {
        // Exactly today's cell
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    const QStyleOptionViewItem opt = cellOption(option, index, mark);
    const QWidget *widget = opt.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    if (!warning) {
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
        return;
    }

    // Everything but the text (the background, the selection, the check box,
    // the focus frame), then the text in its own place, then the glyph after it
    const WarningLayout layout = layoutWarning(opt);
    QStyleOptionViewItem rest = opt;
    rest.text.clear();
    style->drawControl(QStyle::CE_ItemViewItem, &rest, painter, widget);

    QPalette::ColorGroup group = QPalette::Normal;
    if (!(opt.state & QStyle::State_Enabled))
        group = QPalette::Disabled;
    else if (!(opt.state & QStyle::State_Active))
        group = QPalette::Inactive;
    QPalette palette = opt.palette;
    palette.setCurrentColorGroup(group);
    const bool selected = opt.state & QStyle::State_Selected;

    painter->save();
    painter->setClipRect(opt.rect);
    painter->setFont(opt.font);
    style->drawItemText(painter, layout.textArea, int(opt.displayAlignment | Qt::TextSingleLine), palette,
                        opt.state & QStyle::State_Enabled, layout.elided,
                        selected ? QPalette::HighlightedText : QPalette::Text);
    // The style's warning icon, in the selection's icon mode on a selected row
    style->standardIcon(QStyle::SP_MessageBoxWarning, &opt, widget)
        .paint(painter, layout.glyph, Qt::AlignCenter, selected ? QIcon::Selected : QIcon::Normal);
    painter->restore();
}

// The glyph's rect shows the failures, so that the rest of a pending or an
// excluded first cell keeps its own tooltip
bool LogbookCellDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                                    const QModelIndex &index)
{
    if (event && event->type() == QEvent::ToolTip && view) {
        if (showsWarning(index)) {
            const QRect glyph = glyphRect(option, index);
            if (glyph.contains(event->pos())) {
                const QString text = m_demand->sessionFailures(m_model->rowAt(index.row()).sessionId).text();
                QToolTip::showText(event->globalPos(), text, view->viewport(), glyph);
                return true;
            }
        }
        if (showsPending(index)) {
            QToolTip::showText(event->globalPos(), pendingToolTip(), view->viewport(), option.rect);
            return true;
        }
        if (showsExcluded(index)) {
            QToolTip::showText(event->globalPos(), excludedToolTip(), view->viewport(), option.rect);
            return true;
        }
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

// ---- The choice editor -------------------------------------------------------

const AttributeDefinition *LogbookCellDelegate::choiceDefinition(const QModelIndex &index) const
{
    if (!m_model || !index.isValid() || index.model() != m_model.data())
        return nullptr;
    const LogbookColumn &column = m_model->column(index.column());
    if (column.type != ColumnType::SessionAttribute)
        return nullptr;
    const AttributeDefinition *definition = AttributeRegistry::instance().findByKey(column.attributeKey);
    return definition && definition->formatType == AttributeFormatType::Choice ? definition : nullptr;
}

QWidget *LogbookCellDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                                           const QModelIndex &index) const
{
    const AttributeDefinition *definition = choiceDefinition(index);
    if (!definition)
        return QStyledItemDelegate::createEditor(parent, option, index);

    auto *editor = new QComboBox(parent);
    editor->setEditable(false);     // a value outside the list cannot be typed
    for (const AttributeChoice &choice : definition->choices)
        editor->addItem(choice.label, choice.token);
    return editor;
}

// Opens on the entry whose label the cell shows; a raw token (a hand-edited
// file) opens on no entry, and every entry is then a change.
void LogbookCellDelegate::setEditorData(QWidget *editor, const QModelIndex &index) const
{
    auto *combo = qobject_cast<QComboBox *>(editor);
    if (!combo || !choiceDefinition(index)) {
        QStyledItemDelegate::setEditorData(editor, index);
        return;
    }
    const QString text = index.data(Qt::DisplayRole).toString();
    int opening = -1;
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemText(i) == text) {
            opening = i;
            break;
        }
    }
    combo->setCurrentIndex(opening);
    combo->setProperty(kOpeningEntry, opening);
}

// Nothing is written when the editor closes on the entry it opened on: see
// CHOICE EDITOR in the class comment
void LogbookCellDelegate::setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const
{
    auto *combo = qobject_cast<QComboBox *>(editor);
    if (!combo || !choiceDefinition(index)) {
        QStyledItemDelegate::setModelData(editor, model, index);
        return;
    }
    const int chosen = combo->currentIndex();
    const QVariant opening = combo->property(kOpeningEntry);
    if (chosen < 0 || (opening.isValid() && chosen == opening.toInt()))
        return;
    model->setData(index, combo->itemData(chosen), Qt::EditRole);
}

void LogbookCellDelegate::repaintColumn(int column)
{
    QWidget *viewport = m_view->viewport();
    viewport->update(QRect(m_view->columnViewportPosition(column), 0, m_view->columnWidth(column),
                           viewport->height()));
}

// One repaint of the visible part of one column per pass that changed it
void LogbookCellDelegate::onPendingCellsChanged(const QString &columnId)
{
    if (!m_view || !m_model)
        return;
    for (int column = 0; column < m_model->columnCount(); ++column) {
        if (!m_view->isColumnHidden(column) && CalculationDemand::columnId(m_model->column(column)) == columnId)
            repaintColumn(column);
    }
}

// The signal names no session: the whole first visual column is repainted
// rather than keeping the previous list to find the rows that changed
void LogbookCellDelegate::onFailuresChanged()
{
    if (!m_view)
        return;
    const int column = firstVisualColumn();
    if (column >= 0)
        repaintColumn(column);
}

} // namespace FlySight
