#ifndef DEMANDSTATE_H
#define DEMANDSTATE_H

// The presentation values of the demand layer: plain values the views read
// (the track, the per-source state, its counts and its tooltip text). No
// model, executor or widget is known here.

#include <QCoreApplication>
#include <QList>
#include <QString>
#include <QStringList>

namespace FlySight {

/// Where one track stands for one demand source: a visible, loaded session for
/// a plot; any session of the logbook (a cell) for a column.
enum class DemandCondition {
    Done,           ///< the value is available (a result exists): nothing to compute
    Waiting,        ///< in demand, not running (inside the input-settle wait, chosen next, or behind other work)
    Running,        ///< the executor's running job, not asked to stop, is one of its calculations
    Failed,         ///< an input-determined failure (NotProduced), or a failure remembered this run: a job that failed, a session that could not be loaded, a result that could not be stored
    NotApplicable   ///< unavailable for ordinary reasons, or refused as not applicable: silently absent
};

/// One track of one demand source. A plain value.
struct DemandTrack {
    QString sessionId;
    QString sessionName;            ///< SessionModel::sessionDisplayName() of the row
    DemandCondition condition = DemandCondition::NotApplicable;
    QStringList calculationTitles;  ///< Waiting/Running: the blockers' titles; Failed: what did not produce / what failed
    QString reason;                 ///< Failed only; never empty
    bool jobFailure = false;        ///< Failed only: not a stored result (a job that failed, a load that failed, a record that could not be written); tried again at the next start
    QString progressText;           ///< Running only: the running job's latest progress text

    bool operator==(const DemandTrack &other) const
    {
        return sessionId == other.sessionId && sessionName == other.sessionName
            && condition == other.condition && calculationTitles == other.calculationTitles
            && reason == other.reason && jobFailure == other.jobFailure
            && progressText == other.progressText;
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
    /// Each in session-model row order; waiting tracks are counted
    /// (waitingCount), never listed.
    QList<DemandTrack> running, failed;
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
            && failed == other.failed
            && progressLabel == other.progressLabel && toolTip == other.toolTip;
    }
    bool operator!=(const DemandState &other) const { return !(*this == other); }

    /// Tracks listed per section of a tooltip at most (running, failed); a
    /// longer section ends "and N more".
    static constexpr int kToolTipListLimit = 10;
    /// Counts one track: wanted unless NotApplicable; done = Done + Failed;
    /// Running and Failed tracks are also appended to running / failed.
    void addTrack(const DemandTrack &track);
    /// Sets progressLabel ("<doneCount> of <wantedCount>" while isWorking(),
    /// else empty) and toolTip; call once every track is added.
    void finish();
    /// The ready-made tooltip of a state: "Computing: k of n done" with the
    /// running tracks, then "Could not be computed:" with the failed tracks,
    /// each omitted when empty; each list stops after kToolTipListLimit tracks
    /// with "and N more". A pure function of its argument.
    static QString buildToolTip(const DemandState &state);

    // Last: the macro ends in "private:"
    Q_DECLARE_TR_FUNCTIONS(DemandState)
};

} // namespace FlySight

#endif // DEMANDSTATE_H
