#ifndef IDLESCHEDULER_H
#define IDLESCHEDULER_H

#include <functional>

#include <QObject>
#include <QTimer>
#include <QVector>

namespace FlySight {

struct Progress {
    int remaining;
    int total;
};

using StepFn     = std::function<void()>;
using BoolFn     = std::function<bool()>;
using ProgressFn = std::function<Progress()>;
using CompleteFn = std::function<void(bool cancelled)>;

struct TaskDef {
    int        priority;
    StepFn     step;
    BoolFn     hasWork;
    ProgressFn progress;
    CompleteFn onComplete;
    bool       cancellable = false;
    /// Whether the task can take a step right now; null means always. A task
    /// whose hasWork() is true while this is false waits on something outside
    /// the scheduler (see the class comment).
    BoolFn     canStep = nullptr;
};

using TaskId = int;

/// Runs background work in small steps from the event loop, one step per
/// tick, the highest-priority task first (a lower priority number runs
/// first). Its tasks come from the components that register them; the
/// scheduler knows nothing of what a step does.
///
/// The ACTIVE task is the highest-priority task whose hasWork() is true: a
/// change of it emits activeTaskChanged and its progress, and its progress is
/// what every tick reports. The STEP of a tick goes to the highest-priority
/// task that has work and can step (TaskDef::canStep), which may be a lower
/// one than the active task. A task may have work it cannot step right now
/// (it waits on something outside the scheduler): it is reported as active
/// with its progress, is not stepped, and the scheduler rests until woken
/// rather than spinning. schedulerIdle is emitted only when no task has work.
class IdleScheduler : public QObject
{
    Q_OBJECT
public:
    explicit IdleScheduler(QObject *parent = nullptr);

    /// Registers `def` under `id`, sorted by priority (a lower number runs
    /// first). An id that is registered already is replaced.
    void registerTask(TaskId id, const TaskDef &def);
    /// Removes the task registered under `id`; nothing when there is none. Its
    /// onComplete is not called. If it was the active task, the next tick
    /// reports the next task with work (activeTaskChanged) or goes idle
    /// (schedulerIdle).
    void unregisterTask(TaskId id);
    void wake();
    void cancel(TaskId id);

    /// A tick is due (test seam): false while the scheduler rests, idle or
    /// waiting on a task that cannot step.
    bool isTicking() const { return m_timer.isActive(); }

signals:
    void activeTaskChanged(int id, bool cancellable);
    void progressChanged(int id, int remaining, int total);
    void schedulerIdle();

private slots:
    void tick();

private:
    struct Entry {
        TaskId  id;
        TaskDef def;
    };

    /// The entry registered under `id`, or nullptr.
    Entry *find(TaskId id);
    void reportProgress(TaskId id);

    QTimer          m_timer;
    QVector<Entry>  m_tasks;
    TaskId          m_activeTask = -1;
    bool            m_idle       = true;
};

} // namespace FlySight

#endif // IDLESCHEDULER_H
