#ifndef LOGBOOKCELLDELEGATE_H
#define LOGBOOKCELLDELEGATE_H

#include <QPointer>
#include <QRect>
#include <QString>
#include <QStyledItemDelegate>
#include <QVariant>
#include <QVector>

class QTreeView;

namespace FlySight {

struct AttributeDefinition;
class CalculationDemand;
class SessionModel;

/// The logbook's cell delegate. It presents two things of the demand layer
/// and decides nothing: the pending cell and the row warning. It also gives a
/// Choice cell its list editor (CHOICE EDITOR).
///
/// PENDING CELL. A logbook cell has three looks:
///  - a value;
///  - empty: "unavailable" (the cached value is invalid), and the row's own
///    pending state for a record that could not be read
///    (SessionRow::pendingColumns: not cached, no value), exactly as before;
///  - a muted pending mark, three middle dots (pendingText(), which is
///    pendingMark() of demandstate.h, in the placeholder colour):
///    the cell's pair is in demand (CalculationDemand::isCellPending()), so
///    the value is being computed - "not yet", where empty means "never". A
///    cell of the row's own pending state is never in demand: its record is
///    known.
/// Pending is a presentation of demand: the model, its cached values,
/// pendingColumns, index.json and SessionModel::sort() never see it; the
/// cached value underneath stays unavailable until the record is written, so
/// sorting treats a pending cell as unavailable. A value always wins: a cell
/// the model has a value for is painted with it, whatever the demand layer
/// said in its last pass. Pending cells do not animate, and their tooltip
/// says only that the value is being computed; the status bar carries the
/// progress.
///
/// ROW WARNING. A row whose session has a current failure
/// (CalculationDemand::sessionFailures() lists a calculation) shows the
/// style's standard warning icon in its first visual cell: the cell of the
/// first section in visual order that is not hidden, asked of the tree's
/// header each time a cell is painted, never cached, so the glyph follows a
/// moved or hidden section and a rebuild of the columns without a signal of
/// its own. A row that is not loaded shows it from the record set, without a
/// load. The glyph follows the text, as a badge follows a name: the text is
/// drawn where it always is, elided into what the text rectangle leaves once
/// the glyph's room is taken from its trailing end, and the glyph sits right
/// after the drawn text, attached to what it annotates rather than pinned to
/// the cell's edge beside the next column; its side is at most one text line.
/// It takes room only while it is shown, and sizeHint() never sees it, so no
/// row height changes and no text moves. Cells over the failed calculation
/// are blank, as any cell without a value: the glyph explains them. A first
/// visual cell may carry both the pending mark and, after it, the glyph.
///
/// HOVER. Over the glyph, the session's failures, SessionFailures::text(),
/// exactly. Elsewhere in the cell, the cell's own tooltip: the pending one,
/// else the base class's (the model's).
///
/// NO GESTURE. No event of its own: a click on the glyph is a click on the
/// cell (selection, the check box) and starts or cancels nothing.
///
/// REPAINT. pendingCellsChanged(id) repaints the visible part of that column
/// of the tree's viewport, failuresChanged() the visible part of the first
/// visual column: one viewport update each, no model signal, no reset. The
/// value itself arrives through the model's own signals.
///
/// CHOICE EDITOR. Sizes are the base class's, and so is editing, except for a
/// Choice cell: a cell of an attribute column whose definition's format type
/// is AttributeFormatType::Choice. Its editor is a non-editable QComboBox, so
/// no free text can be typed, holding the definition's choices: the labels
/// in definition order, each with its token. It opens on the entry whose
/// label is the cell's display text, and on no entry when the text is no
/// label (a raw token). Committed by the base class's Enter and focus-out, it
/// writes the chosen token through SessionModel::setData(). It writes nothing
/// when closed on the entry it opened on: an unset recording shows its
/// default's label, and opening its editor and pressing Enter must not pin
/// that value by accident (a stub's display cannot tell stored from
/// calculated without a load); pinning a value equal to the default is the
/// context menu's "Set ..." action's.
///
/// The demand layer is held weakly, and its end repaints the viewport:
/// without it (never given, or destroyed first) the delegate is exactly
/// QStyledItemDelegate with the choice editor.
class LogbookCellDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    /// `view` is the parent and the tree whose columns are repainted; `demand` may be null.
    LogbookCellDelegate(SessionModel *model, CalculationDemand *demand, QTreeView *view);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override;

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override;
    void setEditorData(QWidget *editor, const QModelIndex &index) const override;
    void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override;

    /// True when the cell is painted as pending: its pair is in demand
    /// (isCellPending) and the model has no value for it.
    bool showsPending(const QModelIndex &index) const;
    /// Where the row warning is painted, in viewport coordinates, with the
    /// option the tree paints `index` with: a rect inside the cell when
    /// `index` is its row's first visual cell and the row's session has a
    /// current failure, else a null rect. Tests only.
    QRect warningRect(const QModelIndex &index) const;
    static QString pendingText();       ///< pendingMark(): three middle dots, U+00B7
    static QString pendingToolTip();    ///< tr("Pending: this value is being computed")

private slots:
    void onPendingCellsChanged(const QString &columnId);
    void onFailuresChanged();

private:
    /// The definition of the cell's attribute when the cell is a Choice cell
    /// of the model; nullptr otherwise.
    const AttributeDefinition *choiceDefinition(const QModelIndex &index) const;
    /// The cell is its row's first visual cell and the row's session has a
    /// current failure.
    bool showsWarning(const QModelIndex &index) const;
    /// The logical index of the first section in visual order that is not
    /// hidden; -1 when there is none.
    int firstVisualColumn() const;
    /// The option the cell is painted with: the base class's for `index`,
    /// with the pending mark and its muted colour when `pending`
    /// (showsPending(index), which the caller has asked already).
    QStyleOptionViewItem cellOption(const QStyleOptionViewItem &option, const QModelIndex &index,
                                    bool pending) const;
    /// Where a warning cell's text and glyph go, for an option cellOption() built.
    struct WarningLayout {
        QRect textArea;     ///< the text's rect: the text rectangle less the glyph's room at its trailing end
        QString elided;     ///< the text as drawn, elided into textArea
        QRect glyph;        ///< the glyph, one spacing after the drawn text
    };
    static WarningLayout layoutWarning(const QStyleOptionViewItem &opt);
    /// The glyph's rect for the view's `option` of `index`, clipped to the cell.
    QRect glyphRect(const QStyleOptionViewItem &option, const QModelIndex &index) const;
    /// Repaints the visible part of the column `column` of the viewport.
    void repaintColumn(int column);

    QPointer<SessionModel> m_model;
    QPointer<CalculationDemand> m_demand;
    QPointer<QTreeView> m_view;
};

} // namespace FlySight

#endif // LOGBOOKCELLDELEGATE_H
