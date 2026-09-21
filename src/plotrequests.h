#ifndef PLOTREQUESTS_H
#define PLOTREQUESTS_H

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

/// Where one visible, loaded track stands for one checked plot.
enum class PlotTrackCondition {
    Available,      ///< the plot draws it
    Missing,        ///< explicit calculations stand in the way and none of them is queued or running
    Pending,        ///< at least one of them is queued or running
    Failed,         ///< an explicit calculation ran and did not produce the value
    NotApplicable   ///< unavailable for ordinary reasons (no such sensor, missing input): silently absent
};

/// One visible, loaded track for one plot. A plain value.
struct PlotTrackState {
    QString            sessionId;
    QString            sessionName;          ///< live: _DESCRIPTION, else the session id
    PlotTrackCondition condition = PlotTrackCondition::NotApplicable;
    QStringList        calculationTitles;    ///< Missing / Pending: the blockers' titles; Failed: the calculations that did not produce
    QString            reason;               ///< Failed only; never empty for a failed track
    JobId              job = 0;              ///< Pending only: the running job if this track waits on it, else its oldest queued job
    JobState           jobState = JobState::Queued;   ///< Pending only: Queued or Running
    QString            jobProgressText;      ///< Pending + Running only: the job's latest progress text

    bool operator==(const PlotTrackState &other) const
    {
        return sessionId == other.sessionId && sessionName == other.sessionName
            && condition == other.condition && calculationTitles == other.calculationTitles
            && reason == other.reason && job == other.job && jobState == other.jobState
            && jobProgressText == other.jobProgressText;
    }
    bool operator!=(const PlotTrackState &other) const { return !(*this == other); }
};

/// Everything the plot list paints for one row beyond what it paints today. A
/// plain value; the default value is "paint exactly today's row".
struct PlotRowState {
    enum class Control { None, Cancel, Refresh };

    QString plotId;                 ///< "<sensorID>/<measurementID>" == PlotModel::PlotValueIdRole
    bool    explicitBacked = false; ///< false: the row is never inspected and looks exactly as today

    int pendingCount = 0;           ///< tracks currently pending (the count that falls as jobs publish)
    int missingCount = 0;           ///< tracks currently missing
    int failedCount  = 0;           ///< tracks currently failed
    int waitingTotal = 0;           ///< tracks the row is or was waiting for in the current episode
    int waitingDone  = 0;           ///< ... of which are no longer waiting (available, failed, or not applicable)

    QString progressLabel;          ///< "<waitingDone> of <waitingTotal>"; empty unless control() == Cancel
    QString jobProgressText;        ///< progress text of the running job when this row waits on it; else empty

    QList<PlotTrackState> pending;  ///< structured tooltip data, in session-model row order
    QList<PlotTrackState> missing;
    QList<PlotTrackState> failed;
    QString toolTip;                ///< ready-made plain text; empty when isPlain()

    /// Cancel while anything is pending; else Refresh while anything is missing.
    Control control() const
    {
        if (pendingCount > 0)
            return Control::Cancel;
        return missingCount > 0 ? Control::Refresh : Control::None;
    }
    /// The warning badge (failedCount); independent of control().
    bool showsWarning() const { return failedCount > 0; }
    /// The number next to the control. Refresh: missingCount; Cancel: pendingCount; None: 0.
    int controlCount() const
    {
        switch (control()) {
        case Control::Cancel:  return pendingCount;
        case Control::Refresh: return missingCount;
        case Control::None:    break;
        }
        return 0;
    }
    /// Nothing pending, missing, or failed: paint exactly today's row.
    bool isPlain() const { return pendingCount == 0 && missingCount == 0 && failedCount == 0; }

    bool operator==(const PlotRowState &other) const
    {
        return plotId == other.plotId && explicitBacked == other.explicitBacked
            && pendingCount == other.pendingCount && missingCount == other.missingCount
            && failedCount == other.failedCount && waitingTotal == other.waitingTotal
            && waitingDone == other.waitingDone && progressLabel == other.progressLabel
            && jobProgressText == other.jobProgressText && pending == other.pending
            && missing == other.missing && failed == other.failed && toolTip == other.toolTip;
    }
    bool operator!=(const PlotRowState &other) const { return !(*this == other); }
};

/// The widget-free logic behind the plot list's rows: what each checked plot is
/// waiting for, and the only place in the application that turns a gesture into
/// job requests. The view paints rowState() and forwards clicks; it decides
/// nothing.
///
/// TRACK CONDITIONS. For every checked plot that is backed by an explicit
/// calculation, each visible, loaded track is classified from the engine's
/// blocker inspection of the plot's y name,
/// DependencyKey::measurement(sensorID, measurementID), and from the job queue:
///
///   BlockerReport::Available                               Available
///   Blocked, a blocker has a live job                      Pending
///   Blocked, every blocker was refused by the queue        NotApplicable
///   Blocked, otherwise                                     Missing
///   NotProduced                                            Failed (reason from the notes)
///   NotApplicable                                          NotApplicable
///
/// A LIVE job is a queued or running job that was not asked to cancel: a track
/// whose job is winding down after a cancel is Missing at once. There is no
/// "stale" condition - a result invalidated by an input change reports Blocked
/// again and is simply Missing. The x axis is not inspected: every time axis of
/// a sensor produced by an explicit calculation is produced by that calculation
/// or derived on demand from its outputs, so y available implies x available.
///
/// WHAT INSPECTION COSTS. CalculationEngine::blockers() is called only for
/// (checked AND explicit-backed plots) x (visible AND loaded tracks) - names
/// the plot widget reads for the same tracks anyway. A plot is EXPLICIT-BACKED
/// when any name in the static dependency closure of its y name
/// (CalculationRegistry::staticDependencies(), which looks through source
/// conversions) has a candidate with explicit policy. That is a pure, memoized
/// function of the registrations, and exact: a plot that is not explicit-backed
/// can never report a blocker, so it is never inspected, its row state is the
/// default value, and no signal is ever emitted for it. Sessions are reached
/// through SessionModel::loadedSession() under a RowStabilityGuard: nothing is
/// loaded, evicted, or touched in the LRU. Classifications are not cached
/// across passes; passes are coalesced to one per event-loop pass.
///
/// ONLY GESTURES START WORK. JobQueue::request() is called from exactly two
/// private functions: requestMissing() - shared by plotCheckedByUser() and
/// refreshPressed(), a one-shot request for the blockers of the plot's
/// currently missing tracks - and continueAfter(), which, when a job SUCCEEDED,
/// requests the next blockers of the tracks a gesture asked about (chained
/// explicit calculations), synchronously in the jobFinished slot so that the
/// queue never reports idle between the links of a chain. A gesture is an
/// explicit call from the view; it is never inferred from a model change.
/// Restoring checked plots, applying a profile, the Plots menu, showing a
/// track, loading or merging a session, an input change, a job ending
/// cancelled / superseded / failed, a registry change, rowState() and flush()
/// request nothing: the affected tracks are Missing and the refresh control
/// shows.
///
/// THE WAITING SET. Per plot, the tracks the row is waiting for in the current
/// episode, each with a `continues` flag. A gesture enters the tracks it got a
/// job for (or found pending) with continues = true; a pass enters tracks it
/// observes pending with continues = false (progress only). The set ends when
/// the plot has no pending track, on cancelPressed(), and when the plot is
/// unchecked; a hidden or removed session leaves every set at once. `continues`
/// is what authorizes continuation, and only that; it is reset when a job of
/// the track's session ends without success or is asked to cancel.
///
/// RECOMPUTATION is scheduled (zero-interval timer) by: the queue's jobsChanged,
/// jobFinished and jobCancelRequested; SessionModel::dependencyChanged for a
/// name in the static closure of a checked explicit-backed plot,
/// visibilityChanged, modelChanged, sessionLoaded, modelReset; the PlotModel's
/// check-state dataChanged and modelReset; and a registry change. jobProgress
/// updates texts only, without inspection. Unchecking a plot and hiding a track
/// also prune, synchronously, the QUEUED jobs that no checked plot needs on a
/// visible track; the running job is left to finish.
///
/// Main thread only. No member may be called from inside a calculation or an
/// engine callback (they inspect engines and call into the queue). Create the
/// component after the JobQueue and destroy it before the queue; every
/// collaborator is held weakly, and a missing one makes the component inert.
class PlotRequests : public QObject
{
    Q_OBJECT
public:
    PlotRequests(SessionModel *sessionModel, PlotModel *plotModel, JobQueue *jobQueue,
                 QObject *parent = nullptr);
    ~PlotRequests() override;       ///< removes the registry observer

    /// sensorId + "/" + measurementId. Must equal PlotModel::PlotValueIdRole.
    static QString plotId(const QString &sensorId, const QString &measurementId);
    static QString plotId(const PlotValue &plot);

    /// The last computed state. A default state (isPlain(), explicitBacked
    /// false) for an unchecked plot, a plot not backed by an explicit
    /// calculation, and an unknown id.
    PlotRowState rowState(const QString &plotId) const;

    // ---- gestures: explicit calls from the view, never inferred ---------------
    /// The user checked this plot by direct interaction with its row. Call
    /// AFTER the check state has been written to the PlotModel. Returns the
    /// number of jobs created. No-op (0) when the plot is not checked.
    int plotCheckedByUser(const QString &plotId);
    /// The user pressed the row's refresh control. The same request.
    int refreshPressed(const QString &plotId);
    /// The user pressed the row's cancel control. Returns the number of jobs
    /// cancelled or asked to cancel. The plot stays checked.
    int cancelPressed(const QString &plotId);

    /// Runs a pending recomputation now (tests; the view never needs it).
    void flush();
    bool hasPendingUpdate() const;

    /// The ready-made tooltip of a row state: sections "Computing", "Not
    /// computed", "Could not be computed", each omitted when empty. A pure
    /// function of its argument.
    static QString buildToolTip(const PlotRowState &state);

    /// Test seam: the number of recomputation passes run so far.
    int passCount() const { return m_passCount; }

signals:
    void rowStateChanged(const QString &plotId);    ///< rowState(plotId) differs from what it was
    void rowStatesChanged();                        ///< once per pass in which at least one row changed

private:
    using JobKey = QPair<QString, QString>;         // (session id, instance id)
    using LiveJobs = QHash<JobKey, JobRecord>;

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
    bool isExplicitBacked(const PlotValue &plot);
    bool syncCheckedSet();                          // true when a plot was unchecked or vanished
    void rebuildRelevantNames();
    QVector<PlotValue> inspectedPlots();            // checked AND explicit-backed, in plot-model order
    bool isInert() const;

    // Inspection and classification
    LiveJobs liveJobs() const;
    Inspections inspect(const QVector<PlotValue> &plots, const QString &onlySessionId = QString()) const;
    BlockerReport inspectLocked(const SessionData &session, const PlotValue &plot) const;
    PlotTrackState classify(const Track &track, const BlockerReport &report, const LiveJobs &live) const;
    static bool hasLiveJob(const QString &sessionId, const BlockerReport &report, const LiveJobs &live);
    static QString failureReason(const QList<UnproducedNote> &notes);

    // The pass
    void scheduleUpdate();
    void recompute();
    PlotRowState buildRowState(const QString &plotId, const QList<PlotTrackState> &tracks);
    void applyStates(const QStringList &order, const QHash<QString, PlotRowState> &states);

    // What starts work: the only two callers of JobQueue::request()
    int requestMissing(const QString &plotId);
    void continueAfter(const JobRecord &job);
    /// The blockers of `report` worth requesting: no live job, not refused.
    QList<CalculationBlocker> requestable(const QString &sessionId, const BlockerReport &report,
                                          const LiveJobs &live) const;
    /// The queue answered "there will never be a job" for this blocker: the
    /// track must not show a refresh control that can do nothing.
    void markRefused(const QString &sessionId, const CalculationBlocker &blocker);

    void stopContinuing(const QString &sessionId);
    void pruneUnwantedQueued();
    void forgetSessions(const QSet<QString> &sessionIds);
    void dropVanishedTracks();

    // Slots
    void onPlotDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles);
    void onPlotCheckStateChanged();
    void onDependencyChanged(const QString &sessionId, const DependencyKey &key);
    void onVisibilityChanged(const QSet<QString> &shown, const QSet<QString> &hidden);
    void onSessionModelReset();
    void onJobFinished(JobId id, JobState state);
    void onJobCancelRequested(JobId id);
    void onJobProgress(JobId id, const QString &text);
    void onRegistryChanged();

    QPointer<SessionModel> m_sessionModel;
    QPointer<PlotModel> m_plotModel;
    QPointer<JobQueue> m_jobQueue;

    QTimer m_updateTimer;
    int m_passCount = 0;

    QHash<QString, PlotValue> m_checked;                    // the checked plots, by plot id
    QStringList m_checkedOrder;                             // ... in plot-model order
    QHash<QString, bool> m_explicitBacked;                  // memo, by plot id
    QHash<QString, QSet<DependencyKey>> m_staticNames;      // memo, by plot id
    QSet<DependencyKey> m_relevantNames;                    // union over checked explicit-backed plots
    bool m_relevantNamesDirty = true;

    QHash<QString, PlotRowState> m_states;                  // inspected plots only
    QHash<QString, QHash<QString, bool>> m_waiting;         // plot id -> session id -> continues
    QSet<JobKey> m_refused;                                 // requests the queue answered "nothing to run"

    int m_registryObserver = -1;
};

} // namespace FlySight

#endif // PLOTREQUESTS_H
