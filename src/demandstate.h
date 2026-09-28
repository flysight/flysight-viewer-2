#ifndef DEMANDSTATE_H
#define DEMANDSTATE_H

// The presentation values of the demand layer: the three plain values the
// views read (the progress of the computations, one failed calculation, and
// the failures of one recording) and the one text form of a recording's
// failures and of a list of recordings. No model, executor or widget is known
// here.

#include <QCoreApplication>
#include <QList>
#include <QString>

namespace FlySight {

/// The computations as one piece of work: how many recordings are still to
/// compute, out of how many since there were none, and what the executor is
/// computing now. The default value is "nothing to compute".
struct DemandProgress {
    int count = 0;                  ///< sessions with a Waiting or Running track in any source, each once
    int highWater = 0;              ///< the largest count since count was last 0; 0 while count is 0
    QString sessionName;            ///< the display name of the running job's recording; empty when no job runs
    QString progressText;           ///< that job's latest progress text; empty when none

    bool operator==(const DemandProgress &other) const
    {
        return count == other.count && highWater == other.highWater
            && sessionName == other.sessionName && progressText == other.progressText;
    }
    bool operator!=(const DemandProgress &other) const { return !(*this == other); }
};

/// One current failure of one pair (a recording and a requested calculation).
struct FailedCalculation {
    QString calculationId;          ///< the requested calculation's instance id
    QString title;                  ///< the registry's title; the id when the title is empty
    QString reason;                 ///< why it failed, without the title; never empty
    bool retriedAtNextStart = false; ///< not stored (a failed job, load or write, an unstored result): the next start tries again

    bool operator==(const FailedCalculation &other) const
    {
        return calculationId == other.calculationId && title == other.title
            && reason == other.reason && retriedAtNextStart == other.retriedAtNextStart;
    }
    bool operator!=(const FailedCalculation &other) const { return !(*this == other); }
};

/// The current failures of one recording, each pair once, and the one text
/// form in which a recording's failures and a list of recordings are shown.
/// A value with no calculations is "no failure".
struct SessionFailures {
    QString sessionId;
    QString sessionName;            ///< SessionModel::sessionDisplayName() of the row
    QList<FailedCalculation> calculations;

    /// Recordings that listText() lists at most.
    static constexpr int kListLimit = 10;

    bool operator==(const SessionFailures &other) const
    {
        return sessionId == other.sessionId && sessionName == other.sessionName
            && calculations == other.calculations;
    }
    bool operator!=(const SessionFailures &other) const { return !(*this == other); }

    /// One line per failed calculation, in order: "<title>: <reason>", ending
    /// " (tried again at the next start)" when retriedAtNextStart; the lines
    /// joined by "\n". Empty with no calculations.
    QString text() const;
    /// For each of the first kListLimit recordings, its name on one line and
    /// each line of its text() indented by two spaces; then, when more are
    /// given, "and N more" at column 0. No header. Empty for an empty list. A
    /// pure function of its argument.
    static QString listText(const QList<SessionFailures> &failures);

    // Last: the macro ends in "private:"
    Q_DECLARE_TR_FUNCTIONS(SessionFailures)
};

} // namespace FlySight

#endif // DEMANDSTATE_H
