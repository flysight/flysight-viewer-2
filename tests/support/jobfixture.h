#ifndef FLYSIGHTTEST_JOBFIXTURE_H
#define FLYSIGHTTEST_JOBFIXTURE_H

#include <atomic>
#include <functional>
#include <memory>

#include <QList>
#include <QMutex>
#include <QSemaphore>
#include <QSignalSpy>
#include <QString>
#include <QStringList>

#include "jobmodel.h"
#include "jobqueue.h"
#include "sessiondata.h"

// Controllable explicit calculations on real sessions, for tests of the job
// queue, the job model, and whatever sits on top of them. A test can hold the
// queue's worker inside a compute function, observe it there, and release or
// cancel it - without GTSAM and without sleeps.
//
// The synchronization in this file (semaphores, a mutex, atomics) belongs to
// the tests. The "no locks" rule is about the library.

namespace FlySightTest {

/// Shared by every gated compute function of one JobWorld. A compute function
/// records its entry, then waits for a permit while polling the cancel flag.
struct Gate {
    QSemaphore entered;                 ///< one release per compute function that reached the gate
    QSemaphore proceed;                 ///< one permit lets one compute function through
    std::atomic<int> running{0};        ///< compute functions inside the gate right now
    std::atomic<int> maxRunning{0};     ///< the most that were ever inside at once

    /// Lets `n` compute functions through (now or when they arrive).
    void open(int n = 1) { proceed.release(n); }
    /// True once a compute function is inside (consumes one entry). Spins the
    /// event loop while it waits: a job starts from the event loop.
    bool waitEntered(int timeoutMs = 5000);
    /// The value of the input attribute of each run, in the order they entered.
    /// Tests give every session its own input value, which identifies the run.
    QList<int> startOrder() const;

    // For the compute functions
    void enter(int inputValue);
    void leave();

private:
    mutable QMutex m_mutex;
    QList<int> m_startOrder;
};

/// Registers synthetic calculations on the GLOBAL registry (the one real
/// sessions are bound to) and removes exactly those again when destroyed.
/// Construct it in init() BEFORE the SessionModel and destroy it in cleanup()
/// AFTER the JobQueue and the SessionModel are gone: a live model reacts to
/// registry changes, and a registry change during a job can mark it stale.
///
/// Registered:
///  - Synthetic::registerExplicitWorld(): expA, derivA, derivA2, expB, derivB
///    (see fakesessionstate.h for the table and the literals).
///  - Gated and misbehaving calculations. All Explicit, all titled, each with
///    one input attribute <P>_IN and one output attribute <P>_OUT = <P>_IN + 1
///    (an int):
///
///    | Id        | P | Title      | The compute function                                                  |
///    |-----------|---|------------|-----------------------------------------------------------------------|
///    | gated     | G | Gated      | enters the gate, reports "step 1", waits for a permit while checking   |
///    |           |   |            | throwIfCancelled(), reports "step 2", returns                          |
///    | stubborn  | S | Stubborn   | as gated, but when it notices cancellation it performs one more finite |
///    |           |   |            | step (sums 1e6 doubles) and then RETURNS NORMALLY with a full result   |
///    | thrower   | T | Thrower    | throws std::runtime_error("synthetic failure")                         |
///    | exhausted | X | Exhausted  | throws std::bad_alloc on its first call only, then succeeds            |
///    | deepstack | D | Deep stack | fills and sums a 16 MiB local buffer, page by page                     |
///
/// DescentFixture sessions have none of the input attributes: they are the
/// "missing input" sessions until a test adds one through
/// SessionModel::updateAttribute(id, "G_IN", 4), the application's edit path.
///
/// Real compute functions hold no state. These capture the Gate and a call
/// counter, which is what makes them controllable; both are thread-safe.
class JobWorld {
public:
    JobWorld();
    ~JobWorld();
    JobWorld(const JobWorld &) = delete;
    JobWorld &operator=(const JobWorld &) = delete;

    Gate &gate() { return *m_gate; }
    /// Every id this world registered, in registration order.
    QStringList registeredIds() const { return m_ids; }

    /// One DescentFixture::load(id) per id.
    static QList<FlySight::SessionData> sessions(const QStringList &ids);

private:
    std::shared_ptr<Gate> m_gate;
    QStringList m_ids;
};

/// Spins the event loop until the queue has nothing queued and nothing
/// running. False if that does not happen within timeoutMs.
bool waitIdle(FlySight::JobQueue &queue, int timeoutMs = 5000);

/// "Nothing started" since this object was created: no jobQueued signal and no
/// new row in the job model.
class Quiet {
public:
    explicit Quiet(FlySight::JobQueue &queue)
        : m_queue(queue), m_spy(&queue, &FlySight::JobQueue::jobQueued), m_rows(queue.model()->rowCount())
    {
    }
    bool holds() const { return m_spy.isEmpty() && m_queue.model()->rowCount() == m_rows; }

private:
    FlySight::JobQueue &m_queue;
    QSignalSpy m_spy;
    int m_rows;
};

/// Runs `action` once, on the main thread, when the first progress text of
/// `job` is delivered. The queue delivers progress in order and before the end
/// of the same job, so the job is still Running then - the way to act "during"
/// a computation that no gate can hold (a real fit).
///
/// `context` owns the connection: pass a QObject that lives in the scope of
/// whatever `action` captures by reference (a function-local `QObject scope;`),
/// so that a test that leaves early cannot be called back into dead locals.
void onFirstProgress(FlySight::JobQueue &queue, QObject *context, FlySight::JobId job,
                     std::function<void()> action);

} // namespace FlySightTest

#endif // FLYSIGHTTEST_JOBFIXTURE_H
