#ifndef LOGBOOKCELLDELEGATE_H
#define LOGBOOKCELLDELEGATE_H

#include <QPointer>
#include <QString>
#include <QStyledItemDelegate>

class QTreeView;

namespace FlySight {

class CalculationDemand;
class SessionModel;

/// The logbook's cell delegate. A logbook cell has three looks:
///  - a value;
///  - empty: "unavailable" (the cached value is invalid), and the row's own
///    pending state for a record that could not be read
///    (SessionRow::pendingColumns: not cached, no value), exactly as before;
///  - a muted ellipsis, U+2026 (pendingText(), in the placeholder colour):
///    the cell's pair is in demand (CalculationDemand::isCellPending()), so
///    the value is being computed - "not yet", where empty means "never". A
///    cell of the row's own pending state is never in demand: its record is
///    known.
///
/// Pending is a presentation of demand: the model, its cached values,
/// pendingColumns, index.json and SessionModel::sort() never see it; the
/// cached value underneath stays unavailable until the record is written, so
/// sorting treats a pending cell as unavailable. A value always wins: a cell
/// the model has a value for is painted with it, whatever the demand layer
/// said in its last pass. Pending cells do not animate (the column header's
/// indicator carries the motion), and their tooltip says only that the value
/// is being computed; the column header's hover has the detail.
///
/// REPAINT. columnStateChanged(id) repaints the visible part of that column
/// of the tree's viewport, once per pass that changed it: no model signal, no
/// reset. The value itself arrives through the model's own signals.
///
/// Sizes and editing are the base class's. The demand layer is held weakly:
/// without it (never given, or destroyed first) the delegate is exactly
/// QStyledItemDelegate.
class LogbookCellDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    /// `view` is the parent and the tree whose columns are repainted; `demand` may be null.
    LogbookCellDelegate(SessionModel *model, CalculationDemand *demand, QTreeView *view);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override;

    /// True when the cell is painted as pending: its pair is in demand
    /// (isCellPending) and the model has no value for it.
    bool showsPending(const QModelIndex &index) const;
    static QString pendingText();       ///< U+2026, a horizontal ellipsis
    static QString pendingToolTip();    ///< tr("Pending: this value is being computed")

private slots:
    void onColumnStateChanged(const QString &columnId);

private:
    QPointer<SessionModel> m_model;
    QPointer<CalculationDemand> m_demand;
    QPointer<QTreeView> m_view;
};

} // namespace FlySight

#endif // LOGBOOKCELLDELEGATE_H
