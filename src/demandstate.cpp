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
    progressLabel = isWorking() ? tr("%1 of %2").arg(doneCount).arg(wantedCount) : QString();
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

} // namespace FlySight
