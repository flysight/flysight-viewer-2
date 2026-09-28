// LogbookView.h
#ifndef LOGBOOKVIEW_H
#define LOGBOOKVIEW_H

#include <QWidget>
#include <QTreeView>
#include "sessionmodel.h"

namespace FlySight {

class CalculationDemand;
class WorkingAnimation;

/// The logbook table: the session model in a tree with a header that shows
/// each column's working indicator or warning badge and its hover detail
/// (LogbookHeaderView), and cells that read pending while their value is
/// being computed (LogbookCellDelegate). The demand layer and the working-indicator
/// clock may be null: then header and cells are plain (without a clock, the
/// header's arc does not turn). Background work is shown in the main window's
/// status bar, not here.
class LogbookView : public QWidget
{
    Q_OBJECT
public:
    LogbookView(SessionModel *model, CalculationDemand *demand, WorkingAnimation *clock,
                QWidget *parent = nullptr);
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

    void setupView(CalculationDemand *demand, WorkingAnimation *clock);
};

} // namespace FlySight

#endif // LOGBOOKVIEW_H
