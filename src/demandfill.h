#ifndef DEMANDFILL_H
#define DEMANDFILL_H

#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

#include "jobqueue.h"

namespace FlySight {

class SessionModel;

/// The column fill of the demand layer. Given, after each pass, the set of
/// sessions with a pending column cell and the load candidates in order, the
/// fill keeps at most kMaxHeldSessions hidden sessions loaded and pinned for
/// column demand, releases a hold when its session has no pending cell or no
/// loaded row, loads the next candidate when a hold is free and the scheduler
/// steps it, reports progress as the sessions remaining of the high-water
/// mark, loads nothing more once the executor is shut down (the owner's
/// `enabled` hook; the next update(), when a pass runs, releases every hold)
/// and holds nothing once the component goes (detach()).
///
/// It is the idle scheduler's lowest-priority task
/// (SessionModel::ColumnFillTask, priority 5), registered by registerTask(). It
/// has work while some session has a pending cell, and reports the number of
/// sessions that have one out of the fill's high-water mark, so the logbook's
/// progress line shows the fill for its whole duration; the scheduler completes
/// the fill when it has no pending cell left, reporting its final progress (a
/// fill that starts while no session had a pending cell starts its own count).
/// Its steps are loads: it can step while fewer than kMaxHeldSessions sessions
/// are held and a candidate waits, taking the candidates in the order given;
/// otherwise the scheduler rests until update() wakes it. A load is
/// SessionModel::loadPinnedSession(): the real load (the session-id correction
/// included, so every pair offered carries the row's corrected id) and a pin,
/// the HOLD, from the load until the session has no pending cell left (so a
/// chain of requested calculations keeps it). Sessions that were already loaded
/// are never held; the executor pins them while their job is chosen or running.
/// A released session stays in the pool of hidden sessions until ordinary
/// eviction. Not cancellable: a cancel would be undone at the next tick. Cases:
///  - a pool capacity (LogbookCacheSize) below the bound: the pool exceeds it
///    by at most the holds, which the eviction pass after a release evicts;
///    capacity 0 works;
///  - a column disabled mid-load: the next pass reports no pending cell for
///    the held session and the fill releases it;
///  - a held session shown: nothing changes for the hold; released, the
///    visible row stays loaded;
///  - the logbook closed or repopulated, or the session removed: a model
///    reset, then the release (the row is gone or not loaded);
///  - not applicable once loaded: no pending cell, released;
///  - a load that fails: not held; the owner's `loadFailed` hook remembers it
///    so that it is not a candidate again this run;
///  - the executor shut down: no work (the owner's `enabled` hook), nothing
///    more is loaded, and every hold is released at the next update(). The
///    shutdown ends the executor's jobs and each end runs the owner's pass;
///    with no job to end no pass may follow, and the holds last until the
///    next pass or detach().
/// Saves, visible loads, bulk edits and column work have a higher priority,
/// so a load never reads a file that a bulk edit is about to rewrite.
///
/// The fill exposes nothing of the walk: its interface takes and returns
/// session ids only, and it never calls the executor. The owner's hooks are
/// its only callers back. Main thread only; not a QObject.
class DemandFill
{
public:
    /// Sessions held loaded for column demand at most: the running job's and
    /// the chosen next job's (the executor's bound plus one).
    static constexpr int kMaxHeldSessions = JobQueue::kMaxRunningJobs + 1;

    /// What the fill asks of the component that owns it. Each hook is called
    /// on the main thread, outside any row stability guard.
    struct Hooks {
        std::function<bool()> enabled;              ///< work may be done: the owner is not inert and the executor is not shut down
        std::function<void()> runPendingPass;       ///< before a load: run the owner's pending pass, if any
        std::function<void()> runPass;              ///< run a pass now (the candidate went, was loaded or shown meanwhile)
        std::function<void(const QString &sessionId)> loadFailed;               ///< loadPinnedSession() failed; not held
        std::function<void(const QString &requestedId, const QString &heldId)> loaded; ///< held under heldId (the corrected id)
    };

    DemandFill(SessionModel *sessionModel, Hooks hooks);
    DemandFill(const DemandFill &) = delete;
    DemandFill &operator=(const DemandFill &) = delete;
    ~DemandFill();                  ///< detach()

    void registerTask();            ///< SessionModel::ColumnFillTask; call once
    /// Unregisters the task (when registered and the model lives) and releases
    /// every hold. Idempotent; nothing is called back.
    void detach();

    /// After each pass: releases every hold whose row is gone or not loaded,
    /// or whose session is not in `pendingSessions` (every hold when the model
    /// is gone or `enabled` is false); takes `loadCandidates` (row order, at
    /// most kMaxHeldSessions); remaining = pendingSessions.size(), and the
    /// total is the high-water mark, started afresh when remaining rises from
    /// 0; wakes the scheduler when hasWork(), the counts, the candidates or the
    /// number of holds changed.
    void update(const QSet<QString> &pendingSessions, const QStringList &loadCandidates);

    QStringList heldSessionIds() const { return m_held; }   ///< in load order
    bool hasWork() const;           ///< O(1): enabled() and remaining > 0
    bool canLoad() const;           ///< hasWork(), a hold is free and a candidate waits
    void step();                    ///< the task's step: at most one hidden load

private:
    QPointer<SessionModel> m_model;
    Hooks m_hooks;
    QStringList m_held;             // sessions pinned by loadPinnedSession(), in load order
    QStringList m_candidates;       // the next loads, in row order
    int m_remaining = 0;            // sessions with a pending cell
    int m_highWater = 0;            // the fill's total; 0 between fills
    bool m_registered = false;
};

} // namespace FlySight

#endif // DEMANDFILL_H
