#include "fusion/solverthreads.h"

#include <algorithm>
#include <optional>

#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_scheduler_observer.h>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <pthread.h>
#  include <sched.h>
#endif

// The solver's helper threads at the priority of the thread that runs the fit.
//
// The executor runs a fit on its worker thread at below-normal priority (the
// user interface stays responsive and the machine usable). GTSAM parallelizes
// the fit's elimination on oneTBB's worker threads, which oneTBB creates at
// normal priority. The worker that drives the fit waits on those helpers, and
// between bursts of work they spin looking for more: under load, the normal-
// priority helpers starve the below-normal thread they are waiting for
// (priority inversion), and the fit stalls. The cure is not to raise the
// worker but to lower the helpers: each fit runs in its own oneTBB arena whose
// observer gives every worker thread that joins it the priority of the thread
// that runs the fit, and gives back the thread's own priority when it leaves.
//
// Within FlySight only the fusion kernel uses oneTBB, through GTSAM
// (flysight_fusion is the only library that links GTSAM, and nothing links
// oneTBB but GTSAM; GTSAM's task groups and parallel loops run in the arena of
// the calling thread and create none of their own). That matters in one case:
// a helper still in the arena when the fit returns leaves it after the
// observer is gone and keeps the fit's priority. The next oneTBB work it does
// is then another fit's, whose observer sets its priority again on entry.

using namespace FlySight;

namespace {

// ---- the platform's thread priority ------------------------------------------

#ifdef Q_OS_WIN

using NativePriority = int;     // THREAD_PRIORITY_*

std::optional<NativePriority> currentPriority()
{
    const int priority = GetThreadPriority(GetCurrentThread());
    if (priority == THREAD_PRIORITY_ERROR_RETURN)
        return std::nullopt;
    return priority;
}

void setPriority(const NativePriority &priority)
{
    // Best effort: a thread whose priority cannot be changed keeps its own
    SetThreadPriority(GetCurrentThread(), priority);
}

int level(const NativePriority &priority)
{
    return priority;
}

bool samePriority(const NativePriority &a, const NativePriority &b)
{
    return a == b;
}

#else

// The scheduling policy and parameters, as Qt sets a thread's priority. Where ordinary
// threads have a single priority (SCHED_OTHER on Linux), both the worker's
// below-normal priority and this copy of it change nothing.
struct NativePriority {
    int policy = SCHED_OTHER;
    sched_param param{};
};

std::optional<NativePriority> currentPriority()
{
    NativePriority priority;
    if (pthread_getschedparam(pthread_self(), &priority.policy, &priority.param) != 0)
        return std::nullopt;
    return priority;
}

void setPriority(const NativePriority &priority)
{
    // Best effort: lowering needs no privilege; a thread whose priority
    // cannot be changed keeps its own
    pthread_setschedparam(pthread_self(), priority.policy, &priority.param);
}

int level(const NativePriority &priority)
{
    return priority.param.sched_priority;
}

bool samePriority(const NativePriority &a, const NativePriority &b)
{
    return a.policy == b.policy && a.param.sched_priority == b.param.sched_priority;
}

#endif

// ---- the record, for the tests -----------------------------------------------

// One record per thread that ever helped a fit: a helper writes only its own,
// so the helpers share nothing. solverThreadRecord() combines them while no
// fit runs (the end of a fit's observer waits for its callbacks to finish).
using Records = tbb::enumerable_thread_specific<Fusion::SolverThreadRecord>;

Records &records()
{
    static Records perThread;
    return perThread;
}

// ---- the observer ------------------------------------------------------------

class CallerPriorityObserver final : public tbb::task_scheduler_observer {
public:
    CallerPriorityObserver(tbb::task_arena &arena, const NativePriority &callerPriority)
        : tbb::task_scheduler_observer(arena)
        , m_callerPriority(callerPriority)
    {
        observe(true);
    }

    // Deactivating waits for callbacks in progress on other threads
    ~CallerPriorityObserver() override { observe(false); }

    // The thread that runs the fit enters too (isWorker false): it keeps its own
    void on_scheduler_entry(bool isWorker) override
    {
        if (!isWorker)
            return;
        std::optional<NativePriority> &ownPriority = m_ownPriority.local();
        if (!ownPriority.has_value())
            ownPriority = currentPriority();
        setPriority(m_callerPriority);

        // What the helper runs at now, as the system reports it
        Fusion::SolverThreadRecord &record = records().local();
        ++record.entries;
        const std::optional<NativePriority> now = currentPriority();
        if (!now.has_value() || !samePriority(*now, m_callerPriority))
            ++record.atOtherPriority;
        if (now.has_value())
            record.highestPriority = std::max(record.highestPriority, level(*now));
    }

    void on_scheduler_exit(bool isWorker) override
    {
        if (!isWorker)
            return;
        std::optional<NativePriority> &ownPriority = m_ownPriority.local();
        if (!ownPriority.has_value())
            return;
        setPriority(*ownPriority);
        ownPriority.reset();
    }

private:
    const NativePriority m_callerPriority;
    // Per helper: the priority it had before it joined this fit, while it is in
    // the fit's arena
    tbb::enumerable_thread_specific<std::optional<NativePriority>> m_ownPriority;
};

} // namespace

void Fusion::runWithSolverThreadsAtCallerPriority(const std::function<void()> &work)
{
    const std::optional<NativePriority> callerPriority = currentPriority();
    if (!callerPriority.has_value()) {
        work();
        return;
    }

    // Default concurrency, one slot for this thread: the fit runs here, on this
    // thread's stack, exactly as it would outside the arena. The observer is
    // active before any worker can join, and gone before the arena is.
    tbb::task_arena arena;
    CallerPriorityObserver observer(arena, *callerPriority);
    arena.execute([&work] { work(); });
}

Fusion::SolverThreadRecord Fusion::solverThreadRecord()
{
    SolverThreadRecord total;
    for (const SolverThreadRecord &record : records()) {
        total.entries += record.entries;
        total.atOtherPriority += record.atOtherPriority;
        total.highestPriority = std::max(total.highestPriority, record.highestPriority);
    }
    return total;
}

void Fusion::resetSolverThreadRecord()
{
    for (SolverThreadRecord &record : records())
        record = SolverThreadRecord();
}
