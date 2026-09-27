#include "demandsettleclock.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace FlySight {

DemandSettleClock::DemandSettleClock(int delayMs, std::function<void()> waitEnded)
    : m_delayMs(qMax(0, delayMs))
    , m_waitEnded(std::move(waitEnded))
{
    m_timer.setSingleShot(true);
    QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] { onTimeout(); });
}

void DemandSettleClock::setDelay(int milliseconds)
{
    m_delayMs = qMax(0, milliseconds);
}

void DemandSettleClock::start(const QString &sessionId)
{
    m_until.insert(sessionId, QDeadlineTimer(m_delayMs));
    arm();
}

void DemandSettleClock::endAll()
{
    m_until.clear();
    m_timer.stop();
}

void DemandSettleClock::keepOnly(const QSet<QString> &sessionIds)
{
    m_until.removeIf(
        [&sessionIds](QHash<QString, QDeadlineTimer>::iterator it) { return !sessionIds.contains(it.key()); });
    arm();
}

void DemandSettleClock::dropExpired()
{
    for (auto it = m_until.begin(); it != m_until.end();) {
        if (it->hasExpired())
            it = m_until.erase(it);
        else
            ++it;
    }
}

bool DemandSettleClock::isSettling(const QString &sessionId) const
{
    const auto it = m_until.constFind(sessionId);
    return it != m_until.constEnd() && !it->hasExpired();
}

bool DemandSettleClock::hasSettling() const
{
    return std::any_of(m_until.cbegin(), m_until.cend(),
                       [](const QDeadlineTimer &deadline) { return !deadline.hasExpired(); });
}

QDeadlineTimer DemandSettleClock::nextWaitEnd() const
{
    if (m_until.isEmpty())
        return QDeadlineTimer(QDeadlineTimer::Forever);
    return *std::min_element(m_until.cbegin(), m_until.cend(),
                             [](const QDeadlineTimer &a, const QDeadlineTimer &b) { return a < b; });
}

void DemandSettleClock::arm()
{
    const QDeadlineTimer next = nextWaitEnd();
    if (next.isForever()) {
        m_timer.stop();
        return;
    }
    const qint64 remaining = qMax<qint64>(0, next.remainingTime());
    m_timer.start(int(qMin<qint64>(remaining, std::numeric_limits<int>::max())));
}

void DemandSettleClock::onTimeout()
{
    // A coarse timer may fire a little before the earliest deadline: then no
    // wait has ended, and the clock only re-arms
    const qsizetype before = m_until.size();
    dropExpired();
    const bool ended = m_until.size() < before;
    arm();
    if (ended && m_waitEnded)
        m_waitEnded();
}

} // namespace FlySight
