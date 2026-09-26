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
#include "jobqueue.h"
#include "logbookcolumn.h"
#include "plotregistry.h"

namespace FlySight {

class PlotModel;
class SessionData;
class SessionModel;
struct SessionRow;

/// Where one track stands for one demand source: a visible, loaded session for
/// a plot; any session of the logbook (a cell) for a column.
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
    QString sessionName;            ///< a loaded row's live _DESCRIPTION; for a row that is not loaded (or
                                    ///< failed to load) the logbook's cached description; else the session id
    DemandCondition condition = DemandCondition::NotApplicable;
    QStringList calculationTitles;  ///< Waiting/Running: the blockers' titles; Failed: what did not produce / what failed
    QString reason;                 ///< Failed only; never empty
    bool jobFailure = false;        ///< Failed only: the job failed (not stored; retried at the next start)
    bool settling = false;          ///< Waiting only: the session is inside its input-settle wait
    /// Running: the running job. Waiting: the chosen next job if it is this
    /// track's, else 0 - for a plot track the chosen next job after the
    /// pass's offers; a column track is classified before them, so it names
    /// the chosen next job as the pass found it (column states do not list
    /// waiting tracks).
    JobId job = 0;
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

/// Everything a plot row or a column header presents. The default value is "plain".
struct DemandState {
    QString sourceId;               ///< plot id "<sensorID>/<measurementID>" (== PlotModel::PlotValueIdRole),
                                    ///< or a column id (CalculationDemand::columnId())
    bool requested = false;         ///< the source's value depends on a requested calculation; false: never inspected
    int wantedCount = 0;            ///< tracks that are not NotApplicable
    int doneCount = 0;              ///< ... of which nothing is left to compute: Done + Failed
    int waitingCount = 0;
    int runningCount = 0;           ///< 0 or 1 (JobQueue::kMaxRunningJobs)
    int failedCount = 0;            ///< input-determined + job-level
    QList<DemandTrack> running, failed;            ///< each in session-model row order
    /// Each in session-model row order. Plot states only; a column state
    /// counts its waiting tracks without listing them.
    QList<DemandTrack> waiting;
    QString progressLabel;          ///< "<doneCount> of <wantedCount>" while isWorking(); else empty
    QString toolTip;                ///< buildToolTip(*this); empty when isPlain()

    bool isWorking() const { return waitingCount + runningCount > 0; }
    /// The warning badge replaces the working indicator: shown only once nothing is working.
    bool showsWarning() const { return !isWorking() && failedCount > 0; }
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
/// executor's chosen next job equal to its current choice, has sessions that
/// are not loaded loaded for column demand, and publishes the per-plot and
/// per-column state that the plot list and the logbook present. The views
/// paint plotState(), columnState() and isCellPending(); they decide nothing
/// and call nothing else here.
///
/// WHAT IS WANTED. (a) Plot demand: every checked plot whose value is a
/// requested output (the plot is REQUESTED), for every track: a session that
/// is visible, loaded, and not a failed-load placeholder, in session-model row
/// order. (b) Column demand: every enabled logbook column whose value depends
/// on a requested calculation (the column is REQUESTED:
/// logbookColumnExplicitCalculations() is not empty), for every session row of
/// the logbook, loaded or not; one track per cell. A pair (session, requested
/// calculation) is wanted while it has no result. A result counts whether it
/// was published in this run or restored from storage (SessionModel restores
/// it before the row is published), and whether it is a success or an
/// input-determined failure. Nothing about how the state arose matters: a
/// click, the Plots menu, applying a profile, the start-up restore of checked
/// plots and enabling a column create the same demand. At start-up every
/// session is hidden, so plots have no track; enabled requested columns create
/// demand at once.
///
/// WHERE A RESULT IS LOOKED UP. For a loaded session: the engine's blocker
/// inspection of the plot's y name, DependencyKey::measurement(sensorID,
/// measurementID), or of the column's names (logbookColumnNames(): one, two
/// for a Delta, combined: any NotApplicable, else any NotProduced, else any
/// Blocked, else Available). For a session that is not loaded (and a
/// failed-load placeholder, whose engine holds no stored result): the logbook
/// manager's record set (LogbookManager::knownCalculationRecords()) and the
/// reasons the index recorded for those records
/// (LogbookManager::calculationRecordReason()); a record is never opened. A
/// cell has a result only when every calculation it needs has a record
/// (explicit family instances are never stored, so a column over them alone
/// is not applicable to a session that is not loaded); a record with a reason
/// is a failed result with that reason, after a restart as before it. A known
/// record counts until the column worker's restore finds it stale and deletes
/// it; that record change moves the cell into demand. In order, for a session
/// that is not loaded: no storable calculation - not applicable; a
/// settlement (below) - its verdict; every record known - done, or failed
/// with the first recorded reason; a remembered job failure - failed; every
/// calculation remembered not applicable - not applicable; a failed-load
/// placeholder - settled failed ("The session file could not be loaded");
/// otherwise waiting.
///
/// TRACK CONDITIONS. Each track of a loaded session is classified from its
/// BlockerReport, the executor's running and chosen next jobs, the memory and
/// the settle set:
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
/// The x axis of a plot is not inspected: every time axis of a sensor produced
/// by an explicit calculation is produced by that calculation or derived on
/// demand from its outputs, so y available implies x available. Debug builds
/// check that for every plot track classified Done (a warning when a time axis
/// of the plot's sensor is Blocked); release builds do not. Every count is a
/// function of the current tracks: wanted = every track that is not
/// NotApplicable, done = Done + Failed. A cell is PENDING while it is Waiting
/// or Running.
///
/// SETTLEMENTS. Per cell (session, column), for this run only: the last final
/// verdict (done, failed, not applicable) of a loaded session's cell, recorded
/// in every pass and erased when the cell is no longer final, and the failure
/// of a load ("The session file could not be loaded", a job-level failure).
/// For a session that is not loaded a settlement comes before the record set.
/// Without it, a session loaded for column demand whose cell ended without a
/// record (not applicable, an exception, a failed record write, a job-level
/// failure, a failed load) would be loaded again after its eviction, without
/// end. Cleared: for a session, by its relevant input change, and by a
/// single-row display change while it is not loaded (a bulk edit, the column
/// worker); for a cell, by a record change of one of the column's
/// calculations; for a column, when it leaves the enabled requested set;
/// everything by a registry change; a reset of the session model forgets the
/// ids that no longer have a row (a sort resets the model too). Multi-row
/// changes (the unit system, the environment check) clear nothing.
///
/// WHAT A PASS COSTS. CalculationEngine::blockers() is called only for
/// (checked AND requested plots) x (tracks) - names the plot widget reads for
/// the same tracks anyway - and for the requested columns of loaded sessions
/// whose memo was dropped (an input change or publication, a load, a record
/// change, a job's end, a single-row display change). A plot is REQUESTED when
/// CalculationRegistry::dependsOnExplicit() says so for its y name: any name
/// in the static dependency closure (which looks through source conversions)
/// has a candidate with explicit policy. That is a pure, memoized function of
/// the registrations, and exact: a plot that is not requested can never report
/// a blocker, so it is never inspected, its state is the default value, and no
/// signal is ever emitted for it. The same holds for a column that is not
/// requested. With requested columns, a pass costs O(rows) plus O(rows x
/// requested columns) hash lookups, and one manager lookup per unloaded
/// session between changes of its records; with none, rows are not even
/// walked. Sessions are read in place under one RowStabilityGuard per pass
/// (SessionModel::loadedSession(), SessionModel::rowAt()): nothing is loaded,
/// evicted, or touched in the LRU by a pass. Plot classifications are not
/// cached across passes; passes are coalesced to one per event-loop pass.
///
/// THE CHOICE. Every pass lists the candidates in priority order: (a) the pairs
/// of the focused session, when it is a plot track; (b) the plot pairs of the
/// other tracks in row order; (c) the column pairs of every loaded session
/// (not a placeholder): first the visible ones in row order, then the hidden
/// ones (the pool, and the sessions the column fill has loaded) in row order.
/// Within a session, plot-model (column) order, then the report's blocker
/// order (upstream first). A pair is listed once, in its first tier. A pair is
/// not a candidate while it is remembered, while it is the running job not
/// asked to stop, or while its session is settling. A session that is not
/// loaded is never offered: it enters tier (c) once the fill has loaded it.
/// The first candidate the executor accepts is its chosen next job (an equal
/// chosen next job is kept as it is); one it refuses as not applicable
/// (missing input, nothing to do, unknown calculation) is remembered, a pass
/// is scheduled (the column cells were classified before the offers), and the
/// next is tried. With no candidate accepted, the chosen next job this
/// component offered is withdrawn; one it did not offer is left alone. The
/// running job is never stopped here: it finishes, and its result is
/// published and stored.
///
/// HIDDEN LOADS. The column fill is the idle scheduler's lowest-priority task
/// (SessionModel::ColumnFillTask, priority 5), registered here. It has work
/// while some session has a pending column cell, and reports the sessions
/// that have one of the fill's high-water mark, so the logbook's progress line
/// shows the fill for its whole duration; one last step, which loads nothing,
/// reports the fill complete. Its other steps are loads: it can step
/// while fewer than kMaxHeldSessions sessions are held and a session that is
/// not loaded, not visible (a visible stub is the visible loader's) and not
/// settling waits, taking them in row order; otherwise the scheduler rests
/// until a pass wakes it. A load is SessionModel::loadPinnedSession(): the
/// real load (the session-id correction included, so every pair offered
/// carries the row's corrected id) and a pin, the HOLD, from the load until
/// the session has no pending cell left (so a chain of requested calculations
/// keeps it). Sessions that were already loaded are never held; the executor
/// pins them while their job is chosen or running. A released session stays
/// in the pool of hidden sessions until ordinary eviction. Not cancellable: a
/// cancel would be undone at the next tick. Cases:
///  - a pool capacity (LogbookCacheSize) below the bound: the pool exceeds it
///    by at most the holds, which the eviction pass after a release evicts;
///    capacity 0 works;
///  - a column disabled mid-load: the next pass finds no demand for the held
///    session and releases it; a chosen next job of it is withdrawn; a running
///    job finishes and is stored;
///  - a held session shown: nothing changes for the hold; plot demand may move
///    it into tier (b); released, the visible row stays loaded;
///  - the logbook closed or repopulated, or the session removed: a model reset,
///    then the release;
///  - not applicable once loaded: settled, released without a job (the column
///    worker had cached the value as unavailable, and it stays so);
///  - a load that fails: settled failed, not held, not loaded again this run;
///  - the executor shut down: no work, nothing more is loaded.
/// Saves, visible loads, bulk edits and column work have a higher priority,
/// so a load never reads a file that a bulk edit is about to rewrite.
///
/// THE INPUT-SETTLE WAIT. A change of a name in the static closure of a checked
/// requested plot or of a requested column (SessionModel::dependencyChanged),
/// other than the publication of a job's own result (JobQueue::publishingJob()),
/// starts or restarts the session's wait of kInputSettleMs. The session's pairs
/// are Waiting (settling) from the first change and are offered only once the
/// session's inputs have been still for the whole wait, so a burst of edits
/// runs one job. Showing, hiding, checking, enabling, applying a profile and
/// loading start no wait.
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
/// modelChanged, focusedSessionChanged, modelReset (which a column change
/// causes), a relevant dependencyChanged, and a single-row display change of a
/// session that is not loaded when it changed what this component knows of
/// it; LogbookManager::calculationRecordsChanged; the executor's jobStarted
/// and jobCancelRequested; a registry change; the end of a settle wait; the
/// column fill's load; an offer the executor refused as not applicable. At
/// once: when a plot is unchecked or a session hidden while a chosen next job
/// exists (so that it is dropped before it can start), in jobFinished, which
/// the executor emits before it decides between idle() and the next start -
/// so a chain of requested calculations continues without an idle period
/// between its links - and before the fill's load when a pass is pending.
/// jobProgress updates texts only, without inspection.
///
/// PRESENTATION. The views read plotState(), columnState(), isCellPending(),
/// workingPlotIds() and workingColumnIds(), and repaint on plotStateChanged(),
/// columnStateChanged() and statesChanged(). They never run a pass (flush() is
/// a test seam), never offer, and never write. "Pending" exists only here and
/// in the view that paints it: never in SessionModel, the cached column values
/// or the logbook index. A tooltip lists at most kToolTipListLimit tracks per
/// section, then how many more there are; the state's own lists stay complete.
///
/// THE ONLY CALLER. This is the only product caller of JobQueue::offer() and
/// JobQueue::withdrawChosenNext(). Nothing calls back into it: it observes
/// PlotModel, SessionModel (its column set included), the logbook manager's
/// record changes, the registry and the executor's signals.
///
/// LIFETIME. Main thread only. No member may be called from inside a
/// calculation or an engine callback (they inspect engines and offer to the
/// executor). Create the component after the executor and destroy it before
/// the executor; it registers the load step with the session model's
/// scheduler, so destroy it before the session model as well. Every
/// collaborator is held weakly, and a missing one makes the component inert:
/// every state is the default, nothing is offered or loaded, and no session is
/// held.
class CalculationDemand : public QObject
{
    Q_OBJECT
public:
    static constexpr int kInputSettleMs = 1000;      ///< input-settle wait
    /// Sessions the demand layer holds loaded for column demand at most:
    /// the running job's and the chosen next job's (the executor's bound
    /// plus one; see HIDDEN LOADS).
    static constexpr int kMaxHeldSessions = JobQueue::kMaxRunningJobs + 1;
    /// Tracks listed per section of a tooltip at most (running, failed); a
    /// longer section ends "and N more".
    static constexpr int kToolTipListLimit = 10;

    CalculationDemand(SessionModel *sessionModel, PlotModel *plotModel, JobQueue *executor,
                      QObject *parent = nullptr);
    /// Removes the registry observer, unregisters the column fill and releases
    /// every hold.
    ~CalculationDemand() override;

    /// sensorId + "/" + measurementId. Must equal PlotModel::PlotValueIdRole.
    static QString plotId(const QString &sensorId, const QString &measurementId);
    static QString plotId(const PlotValue &plot);
    /// A logbook column's id: logbookColumnDefinitionKey(column). Unique
    /// among the columns (LogbookColumnStore collapses equal definitions).
    static QString columnId(const LogbookColumn &column);

    /// The last computed state. The default state (isPlain(), requested false)
    /// for an unchecked plot, a plot that is not requested, and an unknown id.
    DemandState plotState(const QString &plotId) const;
    /// Default for a column that is not enabled, not requested or unknown.
    DemandState columnState(const QString &columnId) const;
    /// True while the requested calculations the cell needs are waiting or
    /// running (the cell is in demand). Never true for a column that is not
    /// requested.
    bool isCellPending(const QString &sessionId, const QString &columnId) const;
    /// The same by SessionModel row and column index; false out of range.
    bool isCellPending(int row, int column) const;

    /// Ids of the plots / logbook columns whose state isWorking(), in no particular
    /// order; empty when nothing is working. What the views' working-indicator
    /// clocks follow (statesChanged() says when to ask again).
    QStringList workingPlotIds() const;
    QStringList workingColumnIds() const;

    /// The ready-made tooltip of a state: "Computing: k of n done" with the
    /// running tracks, then "Could not be computed:" with the failed tracks,
    /// each omitted when empty; each list stops after kToolTipListLimit tracks
    /// with "and N more". A pure function of its argument.
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
    QStringList heldSessionIds() const { return m_held; }   ///< sessions pinned by the load step, in load order
    bool hasFillWork() const;                        ///< the column fill's hasWork
    bool canLoad() const;                            ///< the column fill can load a session now
    void runLoadStep();                              ///< the load step's step, callable directly
    int  recordSetLookups() const { return m_recordSetLookups; }   ///< LogbookManager::knownCalculationRecords() calls so far

signals:
    void plotStateChanged(const QString &plotId);   ///< plotState(plotId) differs from what it was
    /// columnState(columnId) or the column's set of pending cells differs
    /// from what it was. Emitted per column, before statesChanged().
    void columnStateChanged(const QString &columnId);
    void statesChanged();                           ///< once per pass (or progress update) that changed a state

private:
    using PairKey = QPair<QString, QString>;        // (session id, instance id)
    using CellKey = QPair<QString, QString>;        // (session id, column id)

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

    /// One requested enabled logbook column.
    struct ColumnInfo {
        QString id;                          // columnId()
        QList<DependencyKey> names;          // logbookColumnNames(): what inspection reads
        QStringList calculations;            // E(c) = logbookColumnExplicitCalculations(col, registry)
        QStringList storable;                // E(c) without explicit family instances (ids containing '#')
        QStringList storableTitles;          // parallel to storable: the registry's titles
    };
    /// The last final verdict of a cell (see SETTLEMENTS).
    struct Settlement {
        DemandCondition condition = DemandCondition::Done;
        QStringList titles;
        QString reason;
        bool jobFailure = false;
    };
    /// What one column walk returns: plain values, built under one guard.
    struct ColumnWalk {
        QList<Candidate> visibleCandidates;         // tier (c), visible sessions, row order
        QList<Candidate> hiddenCandidates;          // tier (c), hidden loaded sessions, row order
        QStringList loadCandidates;                 // at most kMaxHeldSessions, row order
        QSet<QString> pendingSessions;              // sessions with a pending cell
        QVector<DemandState> states;                // parallel to m_columns (counts and listed tracks)
        QVector<QSet<QString>> pendingCells;        // parallel to m_columns
    };

    // Which plots and columns matter
    bool isRequested(const PlotValue &plot);
    bool syncCheckedSet();                          // true when a plot was unchecked or vanished
    void rebuildRelevantNames();
    QVector<PlotValue> inspectedPlots();            // checked AND requested, in plot-model order
    void syncColumns();
    bool isInert() const;

    // Inspection and classification
    Inspections inspect(const QVector<PlotValue> &plots) const;
    /// Call under a RowStabilityGuard (inspect() holds it).
    BlockerReport inspectUnderGuard(const SessionData &session, const PlotValue &plot) const;
    DemandTrack classify(const Track &track, const BlockerReport &report,
                         const JobRecord &running, const JobRecord &chosen) const;
    static QString failureReason(const QList<UnproducedNote> &notes);

    // Column demand
    /// The one guarded read of the column demand. Writes this component's
    /// memos and settlements only.
    ColumnWalk walkColumns(const JobRecord &running, const JobRecord &chosen);
    /// The combined report of each requested column for a loaded session.
    /// Call under a RowStabilityGuard.
    static QVector<BlockerReport> columnReports(const SessionData &session, const QVector<ColumnInfo> &columns);
    /// The rules for a session that is not loaded (see WHERE A RESULT IS
    /// LOOKED UP). Call under a RowStabilityGuard.
    DemandTrack classifyUnloaded(const SessionRow &sr, const ColumnInfo &column);
    /// The settlement of every cell of a session whose load failed.
    static Settlement loadFailedSettlement();
    /// The manager's record set of a session, memoized with its reasons.
    const QSet<QString> &recordSet(const QString &sessionId);
    QString rowDisplayName(const SessionRow &sr) const;
    bool eraseSettlements(const QString &sessionId);    // true when one was erased
    void releaseHolds(const QSet<QString> &pendingSessions);
    void releaseAllHolds();
    void registerFillTask();
    bool isFillEnding() const;          // the fill's last step, which only reports its end

    // The choice
    QList<Candidate> plotCandidates(const Inspections &inspections, const QString &focusedId) const;
    void offerChoice(const QList<Candidate> &candidates);
    void withdrawOwnOffer();

    // The pass
    void scheduleUpdate();
    void recompute();
    DemandState buildState(const QString &sourceId, const QList<DemandTrack> &tracks) const;
    /// Adds one track to a state's counts and lists (waiting tracks listed only when `listWaiting`).
    static void addTrack(DemandState &state, const DemandTrack &track, bool listWaiting);
    /// Fills in the progress label and the tooltip of a state whose tracks are all added.
    static void finishState(DemandState &state);
    void applyStates(const QStringList &order, const QHash<QString, DemandState> &states,
                     const QStringList &columnOrder, const QHash<QString, DemandState> &columnStates,
                     const QHash<QString, QSet<QString>> &pendingCells);

    // The input-settle wait
    void dropExpiredSettles();
    void armSettleTimer();
    void onSettleTimeout();

    // Slots
    void onPlotDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles);
    void onPlotCheckStateChanged();
    void onDependencyChanged(const QString &sessionId, const DependencyKey &key);
    void onVisibilityChanged(const QSet<QString> &shown, const QSet<QString> &hidden);
    void onSessionModelAboutToBeReset();
    void onSessionModelReset();
    void onSessionDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles);
    void onCalculationRecordsChanged(const QString &sessionId, const QString &calculationId);
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
    QSet<DependencyKey> m_relevantNames;                    // union over checked requested plots and requested columns
    bool m_relevantNamesDirty = true;

    QHash<QString, DemandState> m_states;                   // inspected plots only
    QHash<PairKey, Memory> m_memory;                        // this run's job failures and refusals

    // Column demand
    QVector<ColumnInfo> m_columns;                          // requested enabled columns, in SessionModel column order
    bool m_columnsDirty = true;
    QHash<QString, DemandState> m_columnStates;             // requested columns only, by column id
    QHash<QString, QSet<QString>> m_pendingCells;           // column id -> sessions whose cell is pending
    bool m_hasPendingCells = false;                         // some set of m_pendingCells is not empty
    QHash<CellKey, Settlement> m_settled;                   // see SETTLEMENTS
    QHash<QString, QSet<QString>> m_recordSets;             // memo: knownCalculationRecords(), rows not loaded
    QHash<QString, QHash<QString, QString>> m_recordReasons; // memo beside it: calculationRecordReason(), non-empty
    int m_recordSetLookups = 0;
    QHash<QString, QVector<BlockerReport>> m_columnReports; // memo: loaded sessions, parallel to m_columns

    // The column fill
    QStringList m_held;                 // sessions this component pinned (loadPinnedSession), in load order
    QStringList m_loadCandidates;       // sessions the next load may take, in row order
    int m_fillRemaining = 0;            // sessions with a pending cell
    int m_fillHighWater = 0;            // the fill's total; 0 between fills
    bool m_fillEnding = false;          // no pending cell left: one step reports the fill complete
    bool m_fillTaskRegistered = false;

    QHash<QString, QDeadlineTimer> m_settleUntil;           // sessions inside their input-settle wait
    QTimer m_settleTimer;                                   // single shot, armed for the earliest deadline
    int m_settleDelayMs = kInputSettleMs;

    JobId m_offeredJob = 0;         // the last offer the executor accepted from this component
    bool m_reconciling = false;     // inside a pass: the executor's signals only schedule

    int m_registryObserver = -1;
};

} // namespace FlySight

#endif // CALCULATIONDEMAND_H
