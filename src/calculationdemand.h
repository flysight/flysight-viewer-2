#ifndef CALCULATIONDEMAND_H
#define CALCULATIONDEMAND_H

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

#include <array>
#include <memory>

#include "dependencykey.h"
#include "demandstate.h"
#include "engine/blockerreport.h"
#include "jobmodel.h"
#include "jobqueue.h"
#include "logbookcolumn.h"
#include "plotregistry.h"

namespace FlySight {

class DemandFill;
class DemandSettleClock;
class PlotModel;
class SessionData;
class SessionModel;
struct SessionRow;

/// The demand layer: the widget-free component that derives what requested
/// calculations are wanted from what the user has switched on, keeps the
/// executor's chosen next job equal to its current choice, has sessions that
/// are not loaded loaded for column demand, and publishes the progress of the
/// computations, the failures of each recording and the pending cells, which
/// the views present. The views read the values that PRESENTATION lists; they
/// decide nothing and call nothing else here. It also still publishes a
/// per-plot and per-column state, which no view reads.
///
/// WHAT IS WANTED. (a) Plot demand: every checked plot whose value is a
/// requested output (the plot is REQUESTED), for every track: a session that
/// is visible, loaded, and not a failed-load placeholder, in session-model row
/// order. (b) Column demand: every enabled logbook column whose value depends
/// on a requested calculation (the column is REQUESTED:
/// SessionModel::columnRequestedCalculations() is not empty), for every
/// session row of the logbook, loaded or not; one track per cell. A pair
/// (session, requested calculation) is wanted while it has no result. A
/// result counts whether it was published in this run or restored from
/// storage (SessionModel restores it before the row is published), and
/// whether it is a success or an input-determined failure. Nothing about how
/// the state arose matters: a click, the Plots menu, applying a profile, the
/// start-up restore of checked plots and enabling a column create the same
/// demand. At start-up every session is hidden, so plots have no track;
/// enabled requested columns create demand at once.
///
/// WHERE A RESULT IS LOOKED UP. Every source (a checked requested plot, an
/// enabled requested column) has its names: the plot's y name,
/// DependencyKey::measurement(sensorID, measurementID), or the column's
/// logbookColumnNames() (one, two for a Delta); and its storable requested
/// calculations: for a plot CalculationRegistry::explicitDependencies() of its
/// y name, for a column SessionModel::columnRequestedCalculations(), each
/// without the explicit family instances, which are never stored. For a loaded
/// session: the engine's blocker inspection of the source's names, combined
/// (any NotApplicable, else any NotProduced, else any Blocked, else
/// Available); a plot's single name combines to itself. For a session that is
/// not loaded (and a failed-load placeholder, whose engine holds no stored
/// result), a track of columns only, in this order:
///  1. no storable calculation - not applicable (a column over explicit
///     family instances alone);
///  2. a storable calculation remembered failed (see MEMORY: a job-level
///     failure, a failed load, a write that failed, an unstored result) -
///     failed with the remembered reasons;
///  3. every storable calculation remembered not applicable for the column
///     (refused, or this column's verdict) - not applicable;
///  4. every storable calculation with a record in the logbook manager's
///     record set (LogbookManager::knownCalculationRecords()) - done, or
///     failed with the first reason the index recorded for those records
///     (LogbookManager::calculationRecordReason()); a record is never opened,
///     and a record with a reason is a failed result after a restart as
///     before it;
///  5. a failed-load placeholder - failed ("The session file could not be
///     loaded"), remembered for the source's storable calculations as a
///     failed load;
///  6. otherwise waiting.
/// Rules 2 and 3 read the memory only; the manager is asked in rule 4 alone.
/// A known record counts until the column worker's restore finds it stale and
/// deletes it; that record change moves the cell into demand.
///
/// TRACK CONDITIONS. Each track of a loaded session is classified from its
/// combined BlockerReport, the executor's running job and the memory:
///
///   BlockerReport               executor / memory                         condition
///   Available                   no storable calculation remembered failed  Done
///   Available                   a storable calculation remembered failed   Failed (a result that could not be stored)
///   NotApplicable                                                          NotApplicable
///   NotProduced                                                            Failed (reason from the notes)
///   Blocked   a blocker is the running job, not asked to stop              Running
///   Blocked   otherwise, a blocker has a remembered failure                Failed
///   Blocked   otherwise, every blocker is remembered refused               NotApplicable
///   Blocked   otherwise                                                    Waiting
///
/// A Blocked report's notes are ignored: Blocked wins. There is no "stale"
/// condition - a result invalidated by an input change reports Blocked again.
/// The x axis of a plot is not inspected: every time axis of a sensor produced
/// by an explicit calculation is produced by that calculation or derived on
/// demand from its outputs, so y available implies x available. Debug builds
/// check that for every plot report that is Available (a warning when a time
/// axis of the plot's sensor is Blocked); release builds do not. Every count is
/// a function of the current tracks: wanted = every track that is not
/// NotApplicable, done = Done + Failed. A cell is PENDING while it is Waiting
/// or Running.
///
/// WHAT A PASS COSTS. CalculationEngine::blockers() is called only for the
/// tracks of the sources - a plot's tracks are visible loaded sessions, names
/// the plot widget reads for the same tracks anyway - and only when a loaded
/// session's report memo lacks the source. Every source's combined report is
/// memoized per loaded session and dropped by every event after which it may
/// differ: any dependency change of the session (an input change, a
/// publication, a bulk edit), its load, a record change of it, a job's end, a
/// reset of the session model and a registry change; the walk drops the memo
/// of a row that is not loaded. The memo is keyed per session, not per name:
/// after A publishes, the blocker of B's output may change from A to B although
/// B's output is not re-announced, and A's publication drops B's report with
/// the rest of the session's. A plot is REQUESTED when
/// CalculationRegistry::dependsOnExplicit() says so for its y name: any name
/// in the static dependency closure (which looks through source conversions)
/// has a candidate with explicit policy. That is a pure, memoized function of
/// the registrations, and exact: a plot that is not requested can never report
/// a blocker, so it is never inspected, its state is the default value, and no
/// signal is ever emitted for it. The same holds for a column that is not
/// requested. With sources, a pass costs O(rows x sources) hash lookups, and
/// one manager lookup per unloaded session between changes of its records;
/// with none, rows are not even walked. Sessions are read in place under one
/// RowStabilityGuard per pass (SessionModel::loadedSession(),
/// SessionModel::rowAt()): nothing is loaded, evicted, or touched in the LRU
/// by a pass. Passes are coalesced to one per event-loop pass.
///
/// THE CHOICE. The walk files the candidates by tier as it goes: (a) the plot
/// pairs of the focused session, when it is a plot track; (b) the plot pairs of
/// the other tracks in row order; (c) the column pairs of every loaded session
/// (not a placeholder): first the visible ones in row order, then the hidden
/// ones (the pool, and the sessions the column fill has loaded) in row order.
/// Within a session, source order (plot-model order, then column order), then
/// the report's blocker order (upstream first). Every blocker of a Blocked
/// report is filed, whatever the track's condition. A pair is listed once, in
/// its first tier. A pair is not a candidate while a failure or a refusal is
/// remembered for it, while it is the running job not asked to stop, or while
/// its session is settling. A session that is not loaded is never offered: it
/// enters tier (c) once the fill has loaded it. Each candidate is offered in
/// turn: the first the executor creates is the chosen next job, and one it
/// answers AlreadyActive for as the chosen next job is kept as it is; one it
/// refuses as not applicable (missing input, nothing to do, unknown
/// calculation) is remembered as refused, a pass is scheduled (every track was
/// classified in the walk, before the offers), and the next is tried. Blocked
/// is not expected: the candidates list upstream first. With no candidate
/// accepted, the chosen next job is withdrawn: this component is the only
/// offerer, so it is always its own. The running job is never stopped here: it
/// finishes, and its result is published and stored.
///
/// HIDDEN LOADS. The column fill (demandfill.h) loads sessions that are not
/// loaded for column demand. The pass gives it the sessions with a pending
/// cell and the load candidates, which are waiting, hidden, unloaded, not
/// settling sessions in row order up to DemandFill::kMaxHeldSessions.
///
/// THE INPUT-SETTLE WAIT. A change of a name in the static closure of a checked
/// requested plot or of a requested column (SessionModel::dependencyChanged),
/// other than the publication of a job's own result (JobQueue::publishingJob()),
/// starts or restarts the session's wait of kInputSettleMs. The session's pairs
/// are Waiting from the first change and are offered only once the session's
/// inputs have been still for the whole wait, so a burst of edits runs one
/// job. Showing, hiding, checking, enabling, applying a profile and loading
/// start no wait. The clock (demandsettleclock.h) holds the deadlines.
///
/// MEMORY. One memory of this run, keyed by pair (session id, requested
/// calculation instance id); there is no other memory of a verdict, and a
/// track's verdict is always derived: for a loaded session from the engine and
/// the memory, for one that is not loaded from the memory and the record set.
/// A pair is remembered in one of two kinds:
///  - Not applicable, not shown: the executor refused an offer of the pair
///    (missing input, nothing to do, unknown calculation); or a loaded
///    column's value was found not applicable, and every storable calculation
///    of that column is remembered so FOR THAT COLUMN ONLY. The engine reports
///    a name not applicable whenever one input is missing, even when the
///    requested calculation behind it can run (a column over a marker the
///    session lacks, beside a column over the same calculation's output), so
///    the column's verdict is read only by rule 3 of that column for a session
///    that is not loaded: it never makes another source's track not applicable
///    and never keeps a pair from being offered. Plots remember no verdict:
///    they have no track that is not loaded.
///  - Failed, with its reason, shown as a failure (PRESENTATION): a job that ended
///    Failed (the worker could not start, out of memory); a load of the
///    session that failed ("The session file could not be loaded", the fill's
///    load or a failed-load placeholder); a record that the result store could
///    not write (SessionModel::calculationRecordWriteFailed); a result the
///    engine holds that is never stored (a NotProduced note of a storable
///    calculation whose status is not Ok, a computation that threw), which
///    would otherwise run again after every eviction. The reason remembered
///    is the why alone, never the calculation's title and never empty; a
///    failed track's text is built from it in one place (failedTrack():
///    "<title>: <why>", the why alone for a failed load, which is the
///    session's failure), and a failure entry (PRESENTATION) carries it beside
///    the title.
/// Failures and refusals keep a pair from being offered, and they are what a
/// loaded session's classification reads; the column verdict is neither. A
/// failure replaces whatever was remembered; a refusal replaces a column
/// verdict. Cleared: for a session, by a relevant dependency change of it (a
/// bulk edit publishes one), and its failed-load facts by a load of it that
/// succeeds; for a pair, by a record change of the pair (a later successful
/// write clears a failed one); everything by a registry change; a reset of the
/// session model forgets the sessions that no longer have a row (a sort resets
/// the model and forgets nothing else). Nothing else clears it: no display
/// change of the session model is observed, so the column worker's processing
/// of a stub clears nothing, and multi-row changes (the unit system, the
/// environment check) clear nothing. Input-determined failures that are stored
/// are results and need no memory. Every storable calculation of a source is
/// assumed to have a record once its track is done (the registrations give
/// every output of a requested calculation one candidate). Nothing is
/// persisted: a new component (the next start of the application) tries
/// again.
///
/// WHEN A PASS RUNS. Scheduled (a zero-interval timer) by: a check change of
/// the PlotModel and its reset; SessionModel::visibilityChanged, sessionLoaded,
/// modelChanged, focusedSessionChanged, modelReset (which a column change
/// causes), a relevant dependencyChanged (a bulk edit publishes one) and
/// calculationRecordWriteFailed; LogbookManager::calculationRecordsChanged (a
/// record written or removed, or a changed reason the index learned); the
/// executor's jobStarted and jobCancelRequested; a registry change; the end of
/// a settle wait; the column fill's load; an offer the executor refused as not
/// applicable. At once: when a plot is unchecked or a session hidden while a
/// chosen next job exists (so that it is dropped before it can start), in
/// jobFinished, which the executor emits before it schedules the next start -
/// so a chain of requested calculations continues without an idle period
/// between its links - and before the fill's load when a pass is pending.
/// jobProgress updates texts only, without inspection.
///
/// PRESENTATION. The views read three values, each a direct projection of
/// what the one walk classifies and the pair memory holds, with an
/// announcement of its own: the status bar reads progress() and failures(),
/// and the logbook's cells read isCellPending() and sessionFailures(). No view
/// reads the per-plot and per-column state (plotState(), columnState(),
/// workingPlotIds(), workingColumnIds(), and their signals plotStateChanged(),
/// columnStateChanged() and statesChanged()).
///  - progress() (DemandProgress): the sessions with a Waiting or Running
///    track in any source, plot and column tracks alike, each session once;
///    the high-water mark of that count since it was last 0; and the display
///    name and latest progress text of the executor's running job not asked
///    to stop (both empty when there is none, and in a pass with no source,
///    which walks no row). progressChanged() when it differs, after a pass or
///    on a progress text of the job the value describes (without a pass).
///  - failures() (SessionFailures per session): every session with a Failed
///    track in some source, in session-model row order, with its display name
///    and each failed pair once (the first source that fails it wins), with
///    the calculation's title, the reason and whether the next start tries it
///    again. The entries are made by the same call that makes the track Failed,
///    so a track and its entries never disagree; a session that leaves demand
///    leaves the list, and returns with it, the memory unchanged.
///    sessionFailures() answers for one session from an index, without a
///    scan. failuresChanged() when the list differs, order included.
///  - the pending cells (isCellPending()): pendingCellsChanged(columnId) for
///    each requested column whose set differs after a pass, and for each
///    column no longer requested that had pending cells.
/// Every value is stored before anything is announced: a slot of any signal
/// reads every query. When the component is inert, progress is the default
/// value and there are no failures. The views never run a pass (flush() is
/// a test seam), never offer, and never write. "Pending" exists only here and
/// in the view that paints it: never in SessionModel, the cached column values
/// or the logbook index. The values, the per-source state's tooltip and its
/// limit (DemandState::kToolTipListLimit tracks per section, then how many more
/// there are; the state's own lists stay complete) and the one text form of a
/// recording's failures and of the capped list (SessionFailures::text(),
/// SessionFailures::listText()) are in demandstate.h.
///
/// THE ONLY CALLER. This is the only product caller of JobQueue::offer() and
/// JobQueue::withdrawChosenNext(). Nothing calls back into it: it observes
/// PlotModel, SessionModel (its column set, column knowledge and display names
/// included), the logbook manager's record changes, the registry and the
/// executor's signals.
///
/// PARTS. The presentation values (demandstate.h) are what the views read. The
/// fill (demandfill.h) holds sessions loaded for column demand and runs the
/// scheduler task. The settle clock (demandsettleclock.h) answers whether a
/// session is settling and when the next wait ends. This file is the
/// reconciler: the walk, the classification, the memory, the choice and the
/// pass. The walk reads the rows under one guard, asks the clock and reads a
/// copy of the holds; it returns every state, candidate and learned fact as
/// plain values. It calls neither the executor nor the fill, it never loads,
/// pins or emits, and it writes nothing but its memos. After it, the pass
/// applies the learned facts to the memory, gives the fill the pending
/// sessions and the load candidates, offers, and announces.
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

    /// Ids of the plots / logbook columns whose state isWorking(), in no
    /// particular order; empty when nothing is working. No view reads them
    /// (statesChanged() says when to ask again).
    QStringList workingPlotIds() const;
    QStringList workingColumnIds() const;

    /// The computations' progress as the last pass (or the running job's
    /// latest progress text) left it; the default value when inert.
    DemandProgress progress() const;
    /// Every session with a current failure, in session-model row order; each
    /// session once, each pair once. Empty when inert.
    QList<SessionFailures> failures() const;
    /// That session's element of failures(); for a session without a failure,
    /// one with the given id, an empty name and no calculations. O(1).
    SessionFailures sessionFailures(const QString &sessionId) const;

    /// True when a plot value that was just read as empty is absent only
    /// because a requested calculation has not produced it: it is waiting to be
    /// computed (BlockerReport::State::Blocked; the status bar counts it among
    /// the computations) or rejected by a requested calculation (NotProduced; a
    /// failure of the recording, which the status bar and the logbook row show).
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
    int  inputSettleDelay() const;
    void endInputSettleWaits();                      ///< every session's wait ends now; schedules a pass
    bool isSettling(const QString &sessionId) const;
    bool hasSettlingSessions() const;
    QStringList heldSessionIds() const;              ///< sessions pinned by the load step, in load order
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
    /// progress() differs from what it was: after a pass, or on a progress
    /// text of the job it describes. Every value is stored before it is emitted.
    void progressChanged();
    /// failures() differs from what it was (order included), after a pass.
    /// Every value is stored before it is emitted.
    void failuresChanged();
    /// The set of pending cells of the column differs after a pass: a
    /// requested column whose set changed, or a column no longer requested
    /// that had pending cells. Every value is stored before it is emitted.
    void pendingCellsChanged(const QString &columnId);

private:
    using PairKey = QPair<QString, QString>;        // (session id, instance id)

    /// One source of demand (see WHAT IS WANTED): a checked requested plot or an enabled
    /// requested logbook column. Rebuilt at the start of every pass.
    struct Source {
        enum class Kind {
            Plot,       ///< tracks: visible, loaded rows that are not failed-load placeholders; pairs in tiers (a)/(b)
            Column      ///< tracks: every row, loaded or not; pairs of loaded rows in tier (c)
        };
        Kind kind = Kind::Column;
        QString id;                     ///< plotId() or columnId()
        QList<DependencyKey> names;     ///< what inspection reads: {y name} or logbookColumnNames()
        QStringList storable;           ///< requested calculations without explicit family instances ('#'), in the authority's order
        QStringList storableTitles;     ///< parallel to storable: the registry's titles (the id when a title is empty)
        QString sensorId;               ///< Plot only: the sensor whose time axes the debug check reads
    };

    /// What this run remembers of one pair (see MEMORY). Plain data; built by
    /// the factories below, so that kind and origin always match.
    struct PairMemory {
        enum class Kind {
            NotApplicable,  ///< there is nothing to run for it: not shown
            Failed          ///< shown as a failure, with `reason`
        };
        enum class Origin {
            Refused,        ///< NotApplicable: the executor refused an offer of the pair
            ColumnVerdict,  ///< NotApplicable: `columns` found the loaded session not applicable
            Result,         ///< Failed: the engine held a result of the pair that is never stored (status not Ok)
            Job,            ///< Failed: the pair's job ended Failed
            Load,           ///< Failed: the session file could not be loaded
            Write           ///< Failed: the result's record could not be written
        };
        Kind kind = Kind::NotApplicable;
        Origin origin = Origin::Refused;
        QString reason;             ///< Failed only: the why, without the title (failedTrack() adds it); never empty
        QSet<QString> columns;      ///< ColumnVerdict only: the ids of the columns that found it so

        static PairMemory refused() { return PairMemory(); }
        static PairMemory columnVerdict(const QString &columnId)
        {
            PairMemory fact;
            fact.origin = Origin::ColumnVerdict;
            fact.columns.insert(columnId);
            return fact;
        }
        /// `origin` is Result, Job, Load or Write.
        static PairMemory failed(Origin origin, const QString &reason)
        {
            Q_ASSERT(origin != Origin::Refused && origin != Origin::ColumnVerdict);
            PairMemory fact;
            fact.kind = Kind::Failed;
            fact.origin = origin;
            fact.reason = reason;
            return fact;
        }
    };
    /// A fact the walk learned, applied after the walk (the walk writes memos only).
    struct LearnedFact {
        QString sessionId;
        QString calculationId;
        PairMemory fact;
    };
    /// What forgetSession() forgets of a session.
    enum class Forget {
        Everything,     ///< every fact of the session
        LoadFailures    ///< its failed-load facts only
    };

    /// One pair the executor may be offered.
    struct Candidate {
        QString sessionId;
        CalculationBlocker calculation;
    };

    /// What the one walk returns: plain values, built under one guard.
    struct Walk {
        enum Tier { FocusedPlots, OtherPlots, VisibleColumns, HiddenColumns, TierCount };
        std::array<QList<Candidate>, TierCount> tiers;  ///< each in row order; deduplicated by the pass
        QVector<DemandState> states;                    ///< parallel to m_sources: counts and listed tracks, not finished
        QVector<QSet<QString>> pendingCells;            ///< parallel to m_sources; empty for plots
        QSet<QString> pendingSessions;                  ///< sessions with a pending column cell
        QStringList loadCandidates;                     ///< at most DemandFill::kMaxHeldSessions, row order
        QList<LearnedFact> learned;                     ///< applied by the pass after the walk
        /// count, sessionName and progressText; the pass keeps highWater
        DemandProgress progress;
        JobId progressJob = 0;                          ///< the running job progress describes; 0 when none
        QList<SessionFailures> failures;                ///< row order; each pair of a row once
    };

    // Which plots and columns matter
    bool isRequested(const PlotValue &plot);
    bool syncCheckedSet();                          // true when a plot was unchecked or vanished
    /// In the static closure of a checked requested plot, or of a requested
    /// column (read from the session model).
    bool isRelevantName(const DependencyKey &key);
    /// Rebuilds m_sources: the checked requested plots in plot-model order,
    /// then the requested enabled columns in the session model's column order.
    void syncSources();
    /// Fills storable and storableTitles from `calculations`.
    static void setStorable(Source &source, const QStringList &calculations);
    bool isInert() const;

    // Inspection and classification
    /// The source's names inspected and combined for a loaded session. Call
    /// under the walk's RowStabilityGuard.
    static BlockerReport combinedReport(const SessionData &session, const Source &source);
    /// A loaded row that is not a placeholder (see TRACK CONDITIONS).
    /// `running` is the executor's running record (a default record when
    /// there is none). Facts learned go to `learned`; a Failed track appends
    /// one entry per failed pair to `failures`.
    DemandTrack classifyLoaded(const QString &sessionId, const Source &source, const BlockerReport &report,
                               const JobRecord &running, QList<LearnedFact> *learned,
                               QList<FailedCalculation> *failures) const;
    /// A column track whose row is not loaded or is a failed-load placeholder
    /// (see WHERE A RESULT IS LOOKED UP). Call under the walk's guard. Facts
    /// learned go to `learned`; a Failed track appends one entry per failed
    /// pair to `failures`.
    DemandTrack classifyUnloaded(const SessionRow &sr, const Source &source, QList<LearnedFact> *learned,
                                 QList<FailedCalculation> *failures);
    /// A Failed track from remembered failures: `calculationIds`, `titles` and
    /// `facts` are parallel. The one place a track's text is built from a
    /// remembered why: "<title>: <why>" per fact, the why alone for a failed
    /// load; the distinct texts joined by "; " in order. jobFailure is true
    /// unless every fact's origin is Result. Appends one entry per fact to
    /// `failures`, each tried again at the next start (nothing remembered is
    /// stored).
    static DemandTrack failedTrack(const QString &sessionId, const QStringList &calculationIds,
                                   const QStringList &titles, const QList<const PairMemory *> &facts,
                                   QList<FailedCalculation> *failures);
    /// The why of a failure: `detail`, or the default when it is empty (a
    /// failed computation's, or that of a calculation that gave no result).
    static QString failureDetail(const QString &detail, bool computationFailed);
    /// failureDetail() of one note.
    static QString noteDetail(const UnproducedNote &note);
    /// "<title>: <detail>" of one note, with a default detail.
    static QString noteReason(const UnproducedNote &note);
    static QString failureReason(const QList<UnproducedNote> &notes);
    static QString loadFailureReason();
    /// The manager's record set of a session, memoized with its reasons.
    const QSet<QString> &recordSet(const QString &sessionId);
    /// Drop the session's report memos / every report memo; both count in
    /// m_reportDrops.
    void dropReports(const QString &sessionId);
    void dropAllReports();

    // The pair memory
    /// What is remembered of the pair; null when nothing. O(1).
    const PairMemory *remembered(const QString &sessionId, const QString &calculationId) const;
    /// A failure or a refusal: the pair is not offered, and a loaded
    /// session's classification reads it.
    static bool blocksOffer(const PairMemory *memory);
    /// Refused, or found not applicable by the column `columnId`.
    static bool notApplicableFor(const PairMemory *memory, const QString &columnId);
    /// The one writer: a failure replaces anything; a refusal replaces a
    /// column verdict; a column verdict unites into one and is kept out by
    /// anything else. Never schedules.
    void remember(const QString &sessionId, const QString &calculationId, const PairMemory &fact);
    bool forgetSession(const QString &sessionId, Forget which);    // true when something was forgotten
    bool forgetPair(const QString &sessionId, const QString &calculationId);

    /// The fill's hooks: a load that failed is remembered failed for every
    /// requested column; a load under a corrected id drops what was known
    /// under the old one.
    void onFillLoadFailed(const QString &sessionId);
    void onFillLoaded(const QString &requestedId, const QString &heldId);

    // The walk and the choice
    /// The one guarded read of the component (see PARTS). `running` is the
    /// executor's running record before the offers; `held` is a copy of the
    /// fill's holds; `focusedId` the session model's focused session.
    Walk walkRows(const JobRecord &running, const QSet<QString> &held, const QString &focusedId);
    void offerChoice(const QList<Candidate> &candidates);
    void withdrawChoice();

    // The pass
    void scheduleUpdate();
    void recompute();
    void applyStates(const QStringList &order, const QHash<QString, DemandState> &states,
                     const QStringList &columnOrder, const QHash<QString, DemandState> &columnStates,
                     const QHash<QString, QSet<QString>> &pendingCells, const DemandProgress &progress,
                     JobId progressJob, const QList<SessionFailures> &failures);

    // Slots
    void onPlotDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles);
    void onPlotCheckStateChanged();
    void onDependencyChanged(const QString &sessionId, const DependencyKey &key);
    void onVisibilityChanged(const QSet<QString> &shown, const QSet<QString> &hidden);
    void onSessionModelAboutToBeReset();
    void onSessionModelReset();
    void onCalculationRecordsChanged(const QString &sessionId, const QString &calculationId);
    void onCalculationRecordWriteFailed(const QString &sessionId, const QString &calculationId,
                                        const QString &reason);
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

    // The sources as the last pass found them (a per-pass copy)
    QVector<Source> m_sources;                              // plots in plot-model order, then columns in SessionModel column order
    // memo: a loaded session's combined report per source id. A plot id and a
    // column id never collide (a column id starts with its type and '|').
    QHash<QString, QHash<QString, BlockerReport>> m_reports;
    // Counts every drop of report memos (a removal or a clear): a report whose
    // memos were dropped while it was computed is not memoized
    quint64 m_reportDrops = 0;
    /// Session id -> requested calculation instance id -> what this run remembers.
    /// Keyed by pair; nested so that a session's facts are found in O(1).
    QHash<QString, QHash<QString, PairMemory>> m_memory;

    QHash<QString, DemandState> m_states;                   // requested checked plots only
    QHash<QString, DemandState> m_columnStates;             // requested columns only, by column id
    QHash<QString, QSet<QString>> m_pendingCells;           // column id -> sessions whose cell is pending
    bool m_hasPendingCells = false;                         // some set of m_pendingCells is not empty
    DemandProgress m_progress;
    JobId m_progressJob = 0;                                // the running job m_progress describes; 0 when none
    QList<SessionFailures> m_failures;                      // row order
    QHash<QString, qsizetype> m_failureIndex;               // session id -> its index in m_failures
    QHash<QString, QSet<QString>> m_recordSets;             // memo: knownCalculationRecords(), rows not loaded
    QHash<QString, QHash<QString, QString>> m_recordReasons; // memo beside it: calculationRecordReason(), non-empty
    int m_recordSetLookups = 0;

    // The parts: the input-settle wait's deadlines and the column fill
    std::unique_ptr<DemandSettleClock> m_settle;
    std::unique_ptr<DemandFill> m_fill;

    bool m_reconciling = false;     // inside a pass: the executor's signals only schedule

    int m_registryObserver = -1;
};

} // namespace FlySight

#endif // CALCULATIONDEMAND_H
