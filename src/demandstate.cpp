#include "demandstate.h"

#include <QStringList>

namespace FlySight {

QString pendingMark()
{
    return QString(3, QChar(0x00B7));
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
