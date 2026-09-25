// LogbookView.h
#ifndef LOGBOOKVIEW_H
#define LOGBOOKVIEW_H

#include <QToolButton>
#include <QWidget>
#include <QTreeView>
#include <QProgressBar>
#include "sessionmodel.h"

namespace FlySight {

class CalculationDemand;

/// The logbook table: the session model in a tree with a header that shows
/// each column's working indicator or warning badge and its hover detail
/// (LogbookHeaderView), cells that read pending while their value is being
/// computed (LogbookCellDelegate), and the progress line of the idle
/// scheduler's tasks. The demand layer may be null: then header and cells are
/// plain.
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
    void cancelRequested(int taskId);

public slots:
    void selectSessions(const QList<QString> &sessionIds);
    void onActiveTaskChanged(int id, bool cancellable);
    void onProgressChanged(int id, int remaining, int total);
    void onSchedulerIdle();

private slots:
    void onContextMenuRequested(const QPoint &pos);

protected:
    QSize minimumSizeHint() const override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    QTreeView *treeView;
    SessionModel *model;
    CalculationDemand *m_demand;
    QProgressBar *m_progressBar;
    QToolButton *m_cancelButton;
    int m_activeTaskId = -1;

    void setupView();
};

} // namespace FlySight

#endif // LOGBOOKVIEW_H
