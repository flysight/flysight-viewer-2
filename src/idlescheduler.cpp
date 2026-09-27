#include "idlescheduler.h"

#include <algorithm>

namespace FlySight {

IdleScheduler::IdleScheduler(QObject *parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(0);
    connect(&m_timer, &QTimer::timeout, this, &IdleScheduler::tick);
}

void IdleScheduler::registerTask(TaskId id, const TaskDef &def)
{
    // One entry per id: a registration under a known id replaces it
    m_tasks.removeIf([id](const Entry &entry) { return entry.id == id; });

    Entry entry{id, def};

    // Insert in priority-sorted order (ascending — lowest number = highest priority)
    auto it = std::lower_bound(m_tasks.begin(), m_tasks.end(), entry,
        [](const Entry &a, const Entry &b) {
            return a.def.priority < b.def.priority;
        });
    m_tasks.insert(it, entry);
}

void IdleScheduler::unregisterTask(TaskId id)
{
    if (m_tasks.removeIf([id](const Entry &entry) { return entry.id == id; }) == 0)
        return;

    // The next tick reports the next task with work, or goes idle
    if (m_activeTask == id) {
        m_activeTask = -1;
        wake();
    }
}

void IdleScheduler::wake()
{
    if (!m_timer.isActive())
        m_timer.start();
}

void IdleScheduler::cancel(TaskId id)
{
    for (auto &entry : m_tasks) {
        if (entry.id == id) {
            if (entry.def.onComplete)
                entry.def.onComplete(true);

            if (m_activeTask == id)
                m_activeTask = -1;

            wake();
            return;
        }
    }
}

IdleScheduler::Entry *IdleScheduler::find(TaskId id)
{
    for (auto &entry : m_tasks) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

void IdleScheduler::reportProgress(TaskId id)
{
    const Entry *entry = find(id);
    if (!entry || !entry->def.progress)
        return;
    const Progress p = entry->def.progress();
    emit progressChanged(id, p.remaining, p.total);
}

void IdleScheduler::tick()
{
    // A waiting task (one registered with canStep) that was last reported
    // active and has lost its work without a step: its final progress, then
    // its completion, before the scan below reports the next active task or
    // goes idle. It runs first so that the scan sees what onComplete left.
    // m_activeTask is cleared here, so the rule fires once per report; a
    // cancelled task was cleared by cancel(), and one that completed through
    // its step was cleared by the step path.
    if (m_activeTask >= 0) {
        if (const Entry *entry = find(m_activeTask);
            entry && entry->def.canStep && entry->def.hasWork && !entry->def.hasWork()) {
            const TaskId gone = m_activeTask;
            reportProgress(gone);
            // A slot of progressChanged may have cancelled the task (completed
            // already), removed it (never completed) or given it work again
            // (the scan below reports it active again): the rule goes on only
            // while the task is still the one reported active, and completes
            // it only while it still has no work
            if (m_activeTask == gone) {
                m_activeTask = -1;
                if (const Entry *still = find(gone);
                    still && still->def.canStep && still->def.hasWork && !still->def.hasWork()) {
                    const CompleteFn complete = still->def.onComplete;
                    if (complete)
                        complete(false);
                }
            }
        }
    }

    // The active task: the highest-priority task with work. The task stepped:
    // the highest-priority task with work that can step now. Only ids are
    // kept: a slot of the signals below, or a step, may register or remove
    // tasks, so entries are found again by id.
    TaskId activeId = -1;
    bool activeCancellable = false;
    TaskId steppedId = -1;
    for (const auto &entry : std::as_const(m_tasks)) {
        if (!entry.def.hasWork || !entry.def.hasWork())
            continue;
        if (activeId < 0) {
            activeId = entry.id;
            activeCancellable = entry.def.cancellable;
        }
        if (!entry.def.canStep || entry.def.canStep()) {
            steppedId = entry.id;
            break;
        }
    }

    // No work — go idle
    if (activeId < 0) {
        if (!m_idle) {
            m_idle = true;
            m_activeTask = -1;
            emit schedulerIdle();
        }
        return;
    }

    // Task switch detection
    bool reported = false;
    if (activeId != m_activeTask) {
        m_activeTask = activeId;
        m_idle = false;
        emit activeTaskChanged(activeId, activeCancellable);
        reportProgress(activeId);
        reported = true;
    }

    // Work that no task can step now: it waits on something outside the
    // scheduler, which wakes the scheduler when that changes. The line shows
    // the active task; the timer is not re-armed, so nothing spins.
    if (steppedId < 0) {
        if (!reported)
            reportProgress(activeId);
        return;
    }

    // Execute one step
    if (const Entry *entry = find(steppedId)) {
        const StepFn step = entry->def.step;
        if (step)
            step();
    }

    // Report the active task's progress after the step
    reportProgress(activeId);

    // Check completion of the task that stepped
    if (const Entry *entry = find(steppedId); entry && entry->def.hasWork && !entry->def.hasWork()) {
        const CompleteFn complete = entry->def.onComplete;
        if (complete)
            complete(false);
        if (m_activeTask == steppedId)
            m_activeTask = -1;
    }

    // Re-arm for next tick
    m_timer.start();
}

} // namespace FlySight
