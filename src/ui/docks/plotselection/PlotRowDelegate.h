#ifndef PLOTROWDELEGATE_H
#define PLOTROWDELEGATE_H

#include <QPersistentModelIndex>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QStyledItemDelegate>

#include "plotrequests.h"
#include "ui/docks/plotselection/PlotRowLayout.h"

class QAbstractItemView;

namespace FlySight {

/// The plot list's row delegate: it paints what PlotRequests reports for a row
/// and forwards the user's clicks to it. It decides nothing - no state, no
/// count, no word of text, and nothing about what to compute. Every number and
/// every string it shows comes from PlotRowState.
///
/// PAINTING. Right-aligned in a plot row (PlotRowLayout.h): the refresh control
/// with the missing count, or the progress label ("k of n") with the cancel
/// control, and independently the warning badge with the failed count. The
/// plot's name is elided to make room; the cluster never is. A row whose state
/// isPlain() - every plot that is not backed by an explicit calculation, every
/// unchecked row, every row with nothing pending, missing or failed, and every
/// category - is painted and handled by the unmodified base class. The row
/// height never changes: sizeHint() is not overridden. The glyphs are drawn
/// with QPainter in the row's text colour, so they follow the theme, the
/// selection and the screen's scale without any bundled image.
///
/// GESTURES. editorEvent() is the only entry point, and it makes exactly three
/// kinds of call:
///   - PlotRequests::plotCheckedByUser(), when the base class turned the row's
///     check state to Checked INSIDE this call: a left click on the check box,
///     or Space / Select on the current row. That is "checked by direct
///     interaction with the row", detected by comparing the model's check
///     state before and after the base call, so the model already holds
///     Checked when the component hears of it.
///   - PlotRequests::refreshPressed() / cancelPressed(), for a left-button
///     press AND release inside the control's hit rectangle, on the same row,
///     with the same control showing at both moments. A double click forgets
///     the press, so it can never land on the cancel control that replaced the
///     refresh control it pressed.
/// Unchecking is not a gesture. There is no keyboard, menu or context-menu
/// surface for refresh and cancel.
///
/// WHAT IS NOT A GESTURE never reaches this class, by construction: the Plots
/// menu and its shortcuts (MainWindow::togglePlot, src/mainwindow.cpp),
/// applying a profile (applyProfile(), src/profilestatebridge.cpp), and the
/// restore of checked plots from the settings (PlotModel::setPlots(),
/// src/plotmodel.cpp) all write the model directly. The delegate never
/// connects to the model's dataChanged and never infers a gesture from a model
/// change.
///
/// TOOLTIP. helpEvent() shows PlotRowState::toolTip over the whole row.
///
/// REPAINT. PlotRequests::rowStateChanged(plotId) updates that row of the view.
/// There is no animation and no timer.
///
/// The component is held weakly: without it (never given, or destroyed first)
/// the delegate behaves exactly as QStyledItemDelegate.
class PlotRowDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    /// `view` is the parent, and the view whose rows are repainted.
    PlotRowDelegate(PlotRequests *requests, QAbstractItemView *view);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override;

    /// What helpEvent() shows; empty for plain rows and categories.
    QString toolTipFor(const QModelIndex &index) const;
    /// Where the row's control is in viewport coordinates; null when the row
    /// shows none. For tests and for nothing else.
    QRect controlRect(const QModelIndex &index) const;

private slots:
    void onRowStateChanged(const QString &plotId);

private:
    /// The default ("plain") state without a component or for a category.
    PlotRowState stateFor(const QModelIndex &index) const;
    static PlotRowMetrics metricsFor(const QStyleOptionViewItem &opt);
    /// `opt` must have been through initStyleOption().
    PlotRowGeometry geometryFor(const QStyleOptionViewItem &opt, const PlotRowState &state) const;
    /// The text next to the control: the count for refresh, "k of n" for cancel.
    static QString controlLabelFor(const PlotRowState &state);

    QPointer<PlotRequests> m_requests;
    QPointer<QAbstractItemView> m_view;

    // A left-button press inside the control, waiting for its release
    QPersistentModelIndex m_pressedIndex;
    PlotRowState::Control m_pressedControl = PlotRowState::Control::None;
};

} // namespace FlySight

#endif // PLOTROWDELEGATE_H
