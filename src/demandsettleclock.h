#ifndef DEMANDSETTLECLOCK_H
#define DEMANDSETTLECLOCK_H

#include <QDeadlineTimer>
#include <QHash>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>

namespace FlySight {

/// The settle clock of the demand layer: per-session deadlines of the
/// input-settle wait and one single-shot timer armed for the earliest;
/// answers whether a session is settling and when the next wait ends; calls
/// its owner when a wait ends. What starts a wait is the owner's decision.
///
/// It knows sessions by id only. Main thread only; not a QObject, and it emits
/// nothing: its one callback is `waitEnded`.
class DemandSettleClock
{
public:
    /// `waitEnded` is called from the timer, after the waits that have ended
    /// were forgotten: the owner schedules a pass.
    DemandSettleClock(int delayMs, std::function<void()> waitEnded);
    DemandSettleClock(const DemandSettleClock &) = delete;
    DemandSettleClock &operator=(const DemandSettleClock &) = delete;

    void setDelay(int milliseconds);            ///< clamped at 0; affects later start() calls only
    int  delay() const { return m_delayMs; }
    void start(const QString &sessionId);       ///< starts or restarts the session's wait of delay(); re-arms
    void endAll();                              ///< every wait ends now; the timer stops; waitEnded is not called
    void keepOnly(const QSet<QString> &sessionIds);   ///< forgets every other session's wait; re-arms
    void dropExpired();                         ///< forgets the waits that have ended; calls nothing

    bool isSettling(const QString &sessionId) const;  ///< its wait exists and has not ended
    bool hasSettling() const;                   ///< some session is settling
    /// When the next wait ends: the earliest deadline the clock holds (one
    /// that has ended is due now); QDeadlineTimer::Forever when it holds none.
    QDeadlineTimer nextWaitEnd() const;

private:
    void arm();                                 ///< single shot for nextWaitEnd(), or stop
    void onTimeout();                           ///< dropExpired(); arm(); m_waitEnded() if a wait ended

    QHash<QString, QDeadlineTimer> m_until;
    QTimer m_timer;                             ///< single shot
    int m_delayMs;
    std::function<void()> m_waitEnded;
};

} // namespace FlySight

#endif // DEMANDSETTLECLOCK_H
