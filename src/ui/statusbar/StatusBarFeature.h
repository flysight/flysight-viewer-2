#ifndef STATUSBARFEATURE_H
#define STATUSBARFEATURE_H

#include <QObject>
#include <QPointer>
#include <QString>

class QLabel;
class QProgressBar;
class QStatusBar;
class QToolButton;
class QWidget;

namespace FlySight {

class CalculationDemand;
class IdleScheduler;
struct AppContext;

/// The main window's status bar: the one place background work is shown. It
/// presents the idle scheduler's tasks and the demand layer's progress and
/// failures, and decides nothing: what it shows follows from the values it
/// reads and one rule.
///
/// It adds two things to the bar, once each: the activity area at the left,
/// in the bar's normal area, where an application says what it is doing, and
/// the warning at the right, as a permanent widget, where state indicators
/// sit. Nothing in the application shows a temporary message, so the normal
/// area is never hidden under one.
///  - the ACTIVITY AREA: a label "<label>: <done> / <total>", a compact
///    progress bar beside it showing the same, and a cancel button after the
///    bar. Its items are the scheduler's
///    save, load, bulk edit and column tasks, each under a label of its own,
///    and the computations, while the demand layer's progress counts a
///    session (done = high-water mark - count, out of the high-water mark).
///    The fill is not an item: its loads serve the computations. The shown
///    item is the active task when it is an item, otherwise the computations,
///    otherwise nothing, and then the area is empty. Its hover lists every item in progress, the shown one first,
///    and under the computations the recording being computed and its step.
///    The cancel button shows exactly while the shown item is a task the
///    scheduler reported cancellable, and asks the scheduler to cancel it.
///  - the WARNING: the style's warning icon and the number of recordings the
///    demand layer lists as failed, with SessionFailures::listText() as its
///    hover; shown exactly while that list is not empty. It is not a control.
/// Widgets take width only while they are shown: nothing is reserved for a
/// hidden cancel button or a hidden warning. The bar's height never changes,
/// because the activity container is always shown and keeps the height of
/// the tallest thing the bar can show.
///
/// The scheduler has no query for its active task: the component follows
/// activeTaskChanged, progressChanged (the last report for the active task's
/// id; a report for another id changes nothing) and schedulerIdle from its
/// construction, so it must exist before the scheduler's first tick. A
/// component made later shows no task until the next activeTaskChanged.
///
/// The demand layer is held weakly and may be null. It is read at
/// construction and again on its progressChanged and failuresChanged; once it
/// is destroyed (the main window destroys it before its children), the
/// computations and the warning are gone at once and tasks are still shown.
/// Every connection has one of the component's widgets as its context, and
/// the component deletes its widgets when it goes, so nothing runs once
/// either the component or the bar is gone.
class StatusBarFeature : public QObject
{
    Q_OBJECT
public:
    explicit StatusBarFeature(const AppContext &ctx, QStatusBar *statusBar, QObject *parent = nullptr);
    ~StatusBarFeature() override;

private:
    /// The label of a scheduler task that is an item; empty for the fill and
    /// for any other id.
    static QString taskLabel(int taskId);
    /// "<label>: <done> / <total>"
    static QString countText(const QString &label, int done, int total);

    void onActiveTaskChanged(int taskId, bool cancellable);
    void onTaskProgress(int taskId, int remaining, int total);
    void onSchedulerIdle();
    void cancelShownTask();
    /// The activity area from the active task and the demand layer's progress.
    void showActivity();
    /// The warning from the demand layer's failures.
    void showWarning();

    QPointer<IdleScheduler> m_scheduler;
    QPointer<CalculationDemand> m_demand;

    // The active task as the scheduler last reported it; -1 when idle
    int m_taskId = -1;
    bool m_taskCancellable = false;
    int m_taskRemaining = 0;            // the last report for m_taskId
    int m_taskTotal = 0;

    // Children of the bar: the two containers, and the widgets inside them
    QPointer<QWidget> m_activity;
    QPointer<QWidget> m_warning;
    QLabel *m_activityLabel = nullptr;
    QProgressBar *m_activityBar = nullptr;
    QToolButton *m_cancelButton = nullptr;
    QLabel *m_warningText = nullptr;
};

} // namespace FlySight

#endif // STATUSBARFEATURE_H
