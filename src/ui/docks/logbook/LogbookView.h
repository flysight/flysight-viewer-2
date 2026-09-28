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
/// bar, not here.
class LogbookView : public QWidget
{
    Q_OBJECT
public:
    LogbookView(SessionModel *model, CalculationDemand *demand, QWidget *parent = nullptr);
    QList<QModelIndex> selectedRows() const;

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
