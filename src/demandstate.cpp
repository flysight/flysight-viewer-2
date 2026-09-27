#include "demandstate.h"

namespace FlySight {

void DemandState::addTrack(const DemandTrack &track)
{
    switch (track.condition) {
    case DemandCondition::Done:
        ++wantedCount;
        ++doneCount;
        break;
    case DemandCondition::Waiting:
        ++wantedCount;
        ++waitingCount;
        break;
    case DemandCondition::Running:
        ++wantedCount;
        ++runningCount;
        running.append(track);
        break;
    case DemandCondition::Failed:
        ++wantedCount;
        ++doneCount;
        ++failedCount;
        failed.append(track);
        break;
    case DemandCondition::NotApplicable:
        break;      // silently absent
    }
}

void DemandState::finish()
{
    toolTip = buildToolTip(*this);
}

QString DemandState::buildToolTip(const DemandState &state)
{
    const QString indent = QStringLiteral("  ");
    QStringList lines;

    // A column over thousands of sessions must not make a tooltip taller than
    // the screen: each list stops at the limit and says how many it left out
    const auto moreLine = [&indent](qsizetype listed) {
        const int remaining = int(listed) - kToolTipListLimit;
        return indent + tr("and %n more", nullptr, remaining);
    };

    if (state.isWorking()) {
        lines.append(tr("Computing: %1 of %2 done").arg(state.doneCount).arg(state.wantedCount));
        for (const DemandTrack &track : state.running.mid(0, kToolTipListLimit)) {
            const QString progress = track.progressText.isEmpty() ? tr("running") : track.progressText;
            lines.append(indent + tr("%1 - %2: %3").arg(track.sessionName,
                                                         track.calculationTitles.join(QStringLiteral(", ")),
                                                         progress));
        }
        if (state.running.size() > kToolTipListLimit)
            lines.append(moreLine(state.running.size()));
    }
    if (state.failedCount > 0) {
        lines.append(tr("Could not be computed:"));
        for (const DemandTrack &track : state.failed.mid(0, kToolTipListLimit))
            lines.append(indent + tr("%1 - %2").arg(track.sessionName, track.reason));
        if (state.failed.size() > kToolTipListLimit)
            lines.append(moreLine(state.failed.size()));
    }
    return lines.join(QLatin1Char('\n'));
}

QString SessionFailures::text() const
{
    QStringList lines;
    for (const FailedCalculation &calculation : calculations) {
        lines.append(calculation.retriedAtNextStart
                         ? tr("%1: %2 (tried again at the next start)").arg(calculation.title, calculation.reason)
                         : tr("%1: %2").arg(calculation.title, calculation.reason));
    }
    return lines.join(QLatin1Char('\n'));
}

// The status bar's hover over thousands of recordings must not be taller than
// the screen: the list stops at the limit and says how many it left out
QString SessionFailures::listText(const QList<SessionFailures> &failures)
{
    const QString indent = QStringLiteral("  ");
    QStringList lines;
    for (const SessionFailures &session : failures.mid(0, kListLimit)) {
        lines.append(session.sessionName);
        const QString text = session.text();
        if (text.isEmpty())
            continue;
        for (const QString &line : text.split(QLatin1Char('\n')))
            lines.append(indent + line);
    }
    if (failures.size() > kListLimit)
        lines.append(tr("and %n more", nullptr, int(failures.size()) - kListLimit));
    return lines.join(QLatin1Char('\n'));
}

} // namespace FlySight
