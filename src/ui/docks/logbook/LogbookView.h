// LogbookView.h
#ifndef LOGBOOKVIEW_H
#define LOGBOOKVIEW_H

#include <QWidget>
#include <QTreeView>
#include "sessionmodel.h"

namespace FlySight {

class CalculationDemand;

/// The logbook table: the session model in a tree with the tree's own
/// header, whose cells present the demand layer (LogbookCellDelegate): a
/// row whose recording could not be computed carries one warning glyph in
/// its first visual cell, with the failures in its hover, and a cell whose
/// value is being computed reads pending. The demand layer may be null: then
/// the cells are plain. Background work is shown in the main window's status
/// bar, not here. Editable attribute columns are set for the selected
/// sessions from the context menu ("Set ..."), and a Choice attribute is
/// offered as a list, as its cells are when edited in place.
class LogbookView : public QWidget
{
    Q_OBJECT
public:
    LogbookView(SessionModel *model, CalculationDemand *demand, QWidget *parent = nullptr);
    QList<QModelIndex> selectedRows() const;

    /// What the context menu's "Set <column>..." action does once chosen: asks
    /// for the value and bulk-edits the attribute of these sessions. A Choice
    /// attribute is asked with a list (QInputDialog::getItem(), not editable:
    /// the delegate's LogbookCellDelegate::choiceEntries(), opening on
    /// "Default", which removes the stored attribute); any other with a text
    /// prompt. `columnLabel` titles the dialog. The dialog runs a nested event
    /// loop, so the column and the sessions are resolved again after it
    /// closes, and a session or column gone meanwhile is skipped. The context
    /// menu calls it with what it captured; tests call it directly.
    void askAndSetAttribute(const QString &attributeKey, const QString &columnLabel,
                            const QStringList &sessionIds);

signals:
    void showSelectedRequested();
    void hideSelectedRequested();
    void hideOthersRequested();
    void deleteRequested();
    void focusSessionRequested(int row);
    void currentSessionChanged(const QString& sessionId);

public slots:
    void selectSessions(const QList<QString> &sessionIds);

private slots:
    void onContextMenuRequested(const QPoint &pos);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    QTreeView *treeView;
    SessionModel *model;

    void setupView(CalculationDemand *demand);
};

} // namespace FlySight

#endif // LOGBOOKVIEW_H
