#ifndef CALCULATIONDEMAND_H
#define CALCULATIONDEMAND_H

#include <QDeadlineTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "dependencykey.h"
#include "engine/blockerreport.h"
#include "jobmodel.h"
#include "plotregistry.h"

namespace FlySight {

class JobQueue;
class PlotModel;
class SessionData;
class SessionModel;

/// Where one track (a visible, loaded session) stands for one demand source (a plot; phase 2: a column).
enum class DemandCondition {
    Done,           ///< the value is available (a result exists): nothing to compute
    Waiting,        ///< in demand, not running (inside the input-settle wait, chosen next, or behind other work)
    Running,        ///< the executor's running job, not asked to stop, is one of its calculations
    Failed,         ///< an input-determined failure (NotProduced) or a job-level failure remembered this run
    NotApplicable   ///< unavailable for ordinary reasons, or refused as not applicable: silently absent
};

/// One track of one demand source. A plain value.
struct DemandTrack {
    QString sessionId;
    QString sessionName;            ///< live _DESCRIPTION, else the session id
    DemandCondition condition = DemandCondition::NotApplicable;
    QStringList calculationTitles;  ///< Waiting/Running: the blockers' titles; Failed: what did not produce / what failed
    QString reason;                 ///< Failed only; never empty
    bool jobFailure = false;        ///< Failed only: the job failed (not stored; retried at the next start)
    bool settling = false;          ///< Waiting only: the session is inside its input-settle wait
    JobId job = 0;                  ///< Running: the running job; Waiting: the chosen next job if it is this track's, else 0
    QString progressText;           ///< Running only: the running job's latest progress text

    bool operator==(const DemandTrack &other) const
    {
        return sessionId == other.sessionId && sessionName == other.sessionName
            && condition == other.condition && calculationTitles == other.calculationTitles
            && reason == other.reason && jobFailure == other.jobFailure && settling == other.settling
            && job == other.job && progressText == other.progressText;
    }
    bool operator!=(const DemandTrack &other) const { return !(*this == other); }
};

/// Everything a plot row (phase 2: a column header) presents. The default value is "plain".
struct DemandState {
    QString sourceId;               ///< plot id "<sensorID>/<measurementID>" (== PlotModel::PlotValueIdRole)
    bool requested = false;         ///< the source's value depends on a requested calculation; false: never inspected
    int wantedCount = 0;            ///< tracks that are not NotApplicable
    int doneCount = 0;              ///< ... of which nothing is left to compute: Done + Failed
    int waitingCount = 0;
    int runningCount = 0;           ///< 0 or 1 (JobQueue::kMaxRunningJobs)
    int failedCount = 0;            ///< input-determined + job-level
    QList<DemandTrack> running, waiting, failed;   ///< each in session-model row order
    QString progressLabel;          ///< "<doneCount> of <wantedCount>" while isWorking(); else empty
    QString toolTip;                ///< buildToolTip(*this); empty when isPlain()

    bool isWorking() const { return waitingCount + runningCount > 0; }
    bool showsWarning() const { return !isWorking() && failedCount > 0; }   ///< spec 10: the badge replaces the indicator
    bool isPlain() const { return !isWorking() && failedCount == 0; }

    bool operator==(const DemandState &other) const
    {
        return sourceId == other.sourceId && requested == other.requested
            && wantedCount == other.wantedCount && doneCount == other.doneCount
            && waitingCount == other.waitingCount && runningCount == other.runningCount
            && failedCount == other.failedCount && running == other.running
            && waiting == other.waiting && failed == other.failed
            && progressLabel == other.progressLabel && toolTip == other.toolTip;
    }
    bool operator!=(const DemandState &other) const { return !(*this == other); }
};

/// The demand layer: the widget-free component that derives what requested
/// calculations are wanted from what the user has switched on, keeps the
/// executor's chosen next job equal to its current choice, and publishes the
/// per-plot state that the plot list presents. The view paints plotState(); it
/// decides nothing and calls nothing here.
///
/// WHAT IS WANTED. Every checked plot whose value is a requested output (the
/// plot is REQUESTED), for every track: a session that is visible, loaded, and
/// not a failed-load placeholder, in session-model row order. A pair (session,
/// requested calculation) is wanted while the engine's blocker inspection of
/// the plot's y name, DependencyKey::measurement(sensorID, measurementID),
/// names it: only a missing result is wanted. A result counts whether it was
/// published in this run or restored from storage (SessionModel restores it
/// before the row is published), and whether it is a success or an
/// input-determined failure. Nothing about how the state arose matters: a
/// click, the Plots menu, applying a profile and the start-up restore of
/// checked plots create the same demand. At start-up every session is hidden,
/// so the first pass has no track and offers nothing.
///
/// TRACK CONDITIONS. Each track is classified from its BlockerReport, the
/// executor's running and chosen next jobs, the memory and the settle set:
///
///   BlockerReport               executor / memory / settle set          condition
///   Available                                                           Done
///   NotApplicable                                                       NotApplicable
///   NotProduced                                                         Failed (reason from the notes)
///   Blocked   a blocker is the running job, not asked to stop           Running
///   Blocked   otherwise, a blocker has a remembered job failure         Failed (job failure)
///   Blocked   otherwise, every blocker is remembered not applicable     NotApplicable
///   Blocked   otherwise                                                 Waiting (settling inside
///                                                                       the input-settle wait)
///
/// A Blocked report's notes are ignored: Blocked wins. There is no "stale"
/// condition - a result invalidated by an input change reports Blocked again.
/// The x axis is not inspected: every time axis of a sensor produced by an
/// explicit calculation is produced by that calculation or derived on demand
/// from its outputs, so y available implies x available. Debug builds check
/// that for every track classified Done (a warning when a time axis of the
/// plot's sensor is Blocked); release builds do not. Every count is a function
/// of the current tracks: wanted = every track that is not NotApplicable, done
/// = Done + Failed.
///
/// WHAT INSPECTION COSTS. CalculationEngine::blockers() is called only for
/// (checked AND requested plots) x (tracks) - names the plot widget reads for
/// the same tracks anyway. A plot is REQUESTED when
/// CalculationRegistry::dependsOnExplicit() says so for its y name: any name
/// in the static dependency closure (which looks through source conversions)
/// has a candidate with explicit policy. That is a pure, memoized function of
/// the registrations, and exact: a plot that is not requested can never report
/// a blocker, so it is never inspected, its state is the default value, and no
/// signal is ever emitted for it. Sessions are reached through
/// SessionModel::loadedSession() under a RowStabilityGuard: nothing is loaded,
/// evicted, or touched in the LRU. Classifications are not cached across
/// passes; passes are coalesced to one per event-loop pass.
///
/// THE CHOICE. Every pass lists the candidates in priority order: (a) the pairs
/// of the focused session, when it is a track; (b) the pairs of the other
/// tracks in row order. Within a session, plot-model order, then the report's
/// blocker order (upstream first). A pair is not a candidate while it is
/// remembered, while it is the running job not asked to stop, or while its
/// session is settling. The first candidate the executor accepts is its chosen
/// next job (an equal chosen next job is kept as it is); one it refuses as not
/// applicable (missing input, nothing to do, unknown calculation) is
/// remembered and the next is tried. With no candidate accepted, the chosen
/// next job this component offered is withdrawn; one it did not offer is left
/// alone. The running job is never stopped here: it finishes, and its result
/// is published and stored.
///
/// THE INPUT-SETTLE WAIT. A change of a name in the static closure of a checked
/// requested plot (SessionModel::dependencyChanged), other than the
/// publication of a job's own result (JobQueue::publishingJob()), starts or
/// restarts the session's wait of kInputSettleMs. The session's pairs are
/// Waiting (settling) from the first change and are offered only once the
/// session's inputs have been still for the whole wait, so a burst of edits
/// runs one job. Showing, hiding, checking, applying a profile and loading
/// start no wait.
///
/// MEMORY. Per pair, for this run only: a job that ended Failed (the worker
/// could not start, out of memory; never stored) with its reason, and an offer
/// the executor refused as not applicable. A remembered pair is not offered
/// again. The memory of a session is cleared by its input change; all memory
/// by a registry change; a reset of the session model forgets the ids that no
/// longer have a row (a sort resets the model too). Input-determined failures
/// are results (stored) and need no memory. Nothing is persisted: a new
/// component (the next start of the application) tries again.
///
/// WHEN A PASS RUNS. Scheduled (a zero-interval timer) by: a check change of
/// the PlotModel and its reset; SessionModel::visibilityChanged, sessionLoaded,
/// modelChanged, focusedSessionChanged, modelReset and a relevant
/// dependencyChanged; the executor's jobStarted and jobCancelRequested; a
/// registry change; the end of a settle wait. At once: when a plot is
/// unchecked or a session hidden while a chosen next job exists (so that it is
/// dropped before it can start), and in jobFinished, which the executor emits
/// before it decides between idle() and the next start - so a chain of
/// requested calculations continues without an idle period between its links.
/// jobProgress updates texts only, without inspection.
///
/// THE ONLY CALLER. This is the only product caller of JobQueue::offer() and
/// JobQueue::withdrawChosenNext(). Nothing calls back into it: it observes
/// PlotModel, SessionModel, the registry and the executor's signals.
///
/// LIFETIME. Main thread only. No member may be called from inside a
/// calculation or an engine callback (they inspect engines and offer to the
/// executor). Create the component after the executor and destroy it before
/// the executor; every collaborator is held weakly, and a missing one makes
/// the component inert: every state is the default, nothing is offered.
class CalculationDemand : public QObject
{
    Q_OBJECT
public:
    static constexpr int kInputSettleMs = 1000;      ///< input-settle wait

    CalculationDemand(SessionModel *sessionModel, PlotModel *plotModel, JobQueue *executor,
                      QObject *parent = nullptr);
    ~CalculationDemand() override;                   ///< removes the registry observer

    /// sensorId + "/" + measurementId. Must equal PlotModel::PlotValueIdRole.
    static QString plotId(const QString &sensorId, const QString &measurementId);
    static QString plotId(const PlotValue &plot);

    /// The last computed state. The default state (isPlain(), requested false)
    /// for an unchecked plot, a plot that is not requested, and an unknown id.
    DemandState plotState(const QString &plotId) const;

    /// The ready-made tooltip of a state: "Computing: k of n done" with the
    /// running tracks, then "Could not be computed:" with the failed tracks,
    /// each omitted when empty. A pure function of its argument.
    static QString buildToolTip(const DemandState &state);

    /// True when a plot value that was just read as empty is absent only
    /// because a requested calculation has not produced it: it is waiting to be
    /// computed (BlockerReport::State::Blocked; the row shows it working) or
    /// rejected by a requested calculation (NotProduced; the warning badge).
    /// That is an ordinary, supported state, and the plot widget does not warn
    /// "No data available" about it. False for NotApplicable (a missing input,
    /// an unknown sensor: the warning stays) and for Available.
    ///
    /// The engine is asked, not plotState(): that may be one event-loop pass
    /// behind. Inspection never runs an explicit calculation, and it reads -
    /// it neither loads nor evicts - so the call is allowed under a row
    /// stability guard. Like every engine read: main thread, not from inside a
    /// calculation.
    static bool isMerelyUncomputed(const SessionData &session, const QString &sensorId,
                                   const QString &measurementId);

    // ---- test seams ------------------------------------------------------------
    void flush();                                    ///< runs a pending pass now
    bool hasPendingUpdate() const;
    int  passCount() const { return m_passCount; }
    /// Affects later input changes only.
    void setInputSettleDelay(int milliseconds);
    int  inputSettleDelay() const { return m_settleDelayMs; }
    void endInputSettleWaits();                      ///< every session's wait ends now; schedules a pass
    bool isSettling(const QString &sessionId) const;
    bool hasSettlingSessions() const;

signals:
    void plotStateChanged(const QString &plotId);   ///< plotState(plotId) differs from what it was
    void statesChanged();                           ///< once per pass (or progress update) that changed a state

private:
    using PairKey = QPair<QString, QString>;        // (session id, instance id)

    /// What this run remembers of a pair.
    struct Memory {
        enum class Kind { NotApplicable, JobFailed } kind = Kind::NotApplicable;
        QString reason;                             ///< JobFailed: "<title>: <JobRecord::reason>"
    };
    /// One pair the executor may be offered.
    struct Candidate {
        QString sessionId;
        CalculationBlocker calculation;
    };

    struct Track {
        QString sessionId;
        QString sessionName;
    };
    /// What one guarded inspection returns for one (plot, track): plain values.
    struct Inspection {
        Track track;
        BlockerReport report;
    };
    using Inspections = QHash<QString, QList<Inspection>>;      // by plot id, tracks in row order

    // Which plots matter
    bool isRequested(const PlotValue &plot);
    bool syncCheckedSet();                          // true when a plot was unchecked or vanished
    void rebuildRelevantNames();
    QVector<PlotValue> inspectedPlots();            // checked AND requested, in plot-model order
    bool isInert() const;

    // Inspection and classification
    Inspections inspect(const QVector<PlotValue> &plots) const;
    /// Call under a RowStabilityGuard (inspect() holds it).
    BlockerReport inspectUnderGuard(const SessionData &session, const PlotValue &plot) const;
    DemandTrack classify(const Track &track, const BlockerReport &report,
                         const JobRecord &running, const JobRecord &chosen) const;
    static QString failureReason(const QList<UnproducedNote> &notes);

    // The choice
    QList<Candidate> plotCandidates(const Inspections &inspections, const QString &focusedId) const;
    void offerChoice(const QList<Candidate> &candidates);
    void withdrawOwnOffer();

    // The pass
    void scheduleUpdate();
    void recompute();
    DemandState buildState(const QString &sourceId, const QList<DemandTrack> &tracks) const;
    void applyStates(const QStringList &order, const QHash<QString, DemandState> &states);

    // The input-settle wait
    void dropExpiredSettles();
    void armSettleTimer();
    void onSettleTimeout();

    // Slots
    void onPlotDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles);
    void onPlotCheckStateChanged();
    void onDependencyChanged(const QString &sessionId, const DependencyKey &key);
    void onVisibilityChanged(const QSet<QString> &shown, const QSet<QString> &hidden);
    void onSessionModelReset();
    void onJobFinished(JobId id, JobState state);
    void onJobProgress(JobId id, const QString &text);
    void onRegistryChanged();

    QPointer<SessionModel> m_sessionModel;
    QPointer<PlotModel> m_plotModel;
    QPointer<JobQueue> m_queue;

    QTimer m_updateTimer;
    int m_passCount = 0;

    QHash<QString, PlotValue> m_checked;                    // the checked plots, by plot id
    QStringList m_checkedOrder;                             // ... in plot-model order
    QHash<QString, bool> m_requested;                       // memo, by plot id
    QHash<QString, QSet<DependencyKey>> m_staticNames;      // memo, by plot id
    QSet<DependencyKey> m_relevantNames;                    // union over checked requested plots
    bool m_relevantNamesDirty = true;

    QHash<QString, DemandState> m_states;                   // inspected plots only
    QHash<PairKey, Memory> m_memory;                        // this run's job failures and refusals

    QHash<QString, QDeadlineTimer> m_settleUntil;           // sessions inside their input-settle wait
    QTimer m_settleTimer;                                   // single shot, armed for the earliest deadline
    int m_settleDelayMs = kInputSettleMs;

    JobId m_offeredJob = 0;         // the last offer the executor accepted from this component
    bool m_reconciling = false;     // inside a pass: the executor's signals only schedule

    int m_registryObserver = -1;
};

} // namespace FlySight

#endif // CALCULATIONDEMAND_H
