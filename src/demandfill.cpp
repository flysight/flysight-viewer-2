#include "demandfill.h"

#include <tuple>
#include <utility>

#include "idlescheduler.h"
#include "sessionmodel.h"

namespace FlySight {

DemandFill::DemandFill(SessionModel *sessionModel, Hooks hooks)
    : m_model(sessionModel)
    , m_hooks(std::move(hooks))
{
}

DemandFill::~DemandFill()
{
    detach();
}

void DemandFill::registerTask()
{
    if (!m_model || m_registered)
        return;
    m_model->scheduler().registerTask(SessionModel::ColumnFillTask, TaskDef{
        /*priority*/    5,          // below save (1), load (2), bulk edit (3), column work (4)
        /*step*/        [this] { step(); },
        /*hasWork*/     [this] { return hasWork(); },
        /*progress*/    [this] { return Progress{m_remaining, m_highWater}; },
        /*onComplete*/  [this](bool) {
                            // The next fill starts its own count. A cancel()
                            // changes nothing while sessions remain.
                            if (m_remaining == 0)
                                m_highWater = 0;
                        },
        /*cancellable*/ false,      // what is wanted changes only by disabling the column
        /*canStep*/     [this] { return canLoad(); }});
    m_registered = true;
}

void DemandFill::detach()
{
    // The task's functions capture this fill
    if (m_model && m_registered)
        m_model->scheduler().unregisterTask(SessionModel::ColumnFillTask);
    m_registered = false;

    if (m_model) {
        for (const QString &held : std::as_const(m_held))
            m_model->unpinSession(held);
    }
    m_held.clear();
}

// O(1): the scheduler asks on every tick. The fill has work while a session
// has a pending cell. A fill ends with a job, not with a step: the pass that
// finds the last pending cell resolved wakes the scheduler (update()), which
// reports the fill's final progress and completes it.
bool DemandFill::hasWork() const
{
    return m_hooks.enabled && m_hooks.enabled() && m_remaining > 0;
}

bool DemandFill::canLoad() const
{
    return hasWork() && m_held.size() < kMaxHeldSessions && !m_candidates.isEmpty();
}

void DemandFill::update(const QSet<QString> &pendingSessions, const QStringList &loadCandidates)
{
    const auto snapshot = [this] {
        return std::make_tuple(hasWork(), m_remaining, m_highWater, m_candidates, m_held.size());
    };
    const auto before = snapshot();

    // A hold ends when its row is gone or not loaded any more, or when the
    // session has no pending cell left. Outside any guard: unpinning only
    // queues an eviction pass.
    if (!m_model) {
        m_held.clear();
    } else if (!m_held.isEmpty()) {
        QStringList kept;
        for (const QString &held : std::as_const(m_held)) {
            const int row = m_model->getSessionRow(held);
            const bool release = row < 0 || !std::as_const(*m_model).rowAt(row).isLoaded()
                || !pendingSessions.contains(held);
            if (release)
                m_model->unpinSession(held);
            else
                kept.append(held);
        }
        m_held = kept;
    }

    m_candidates = loadCandidates;

    // Remaining = sessions with a pending cell; the total is the fill's
    // high-water mark, reset when the scheduler completes the fill (the task's
    // onComplete). The scheduler completes only the task it last reported
    // active, so a fill that lost its work behind another task keeps its
    // mark: a fill that starts while no session had a pending cell starts its
    // own count.
    const int remaining = int(pendingSessions.size());
    if (m_remaining == 0 && remaining > 0)
        m_highWater = remaining;
    else
        m_highWater = qMax(m_highWater, remaining);
    m_remaining = remaining;

    // The scheduler reports the active task's progress on its tick even when
    // nothing can step, and steps the load again when it can
    if (m_model && snapshot() != before)
        m_model->scheduler().wake();
}

// One hidden load: the session-id correction happens here, before any pair of
// the session is offered. Every hook may run a pass that calls update()
// re-entrantly; the step returns right after each.
void DemandFill::step()
{
    m_hooks.runPendingPass();       // the candidates as demand is now
    if (!canLoad())
        return;

    const QString id = m_candidates.takeFirst();
    // The column worker or resolveIdentityStubs() may have remapped the id
    // silently, or a slot may have loaded or shown the row: the pass knows
    // what to do now
    const int row = m_model->getSessionRow(id);
    if (row < 0) {
        m_hooks.runPass();
        return;
    }
    {
        const SessionRow &sr = std::as_const(*m_model).rowAt(row);
        if ((sr.isLoaded() && !sr.loadFailed) || sr.visible) {
            m_hooks.runPass();
            return;
        }
    }

    const QString held = m_model->loadPinnedSession(id);
    if (held.isEmpty()) {
        // Not held; the owner remembers the failure, so the session is not
        // loaded again in this run
        m_hooks.loadFailed(id);
        return;
    }

    m_held.append(held);
    m_hooks.loaded(id, held);
}

} // namespace FlySight
