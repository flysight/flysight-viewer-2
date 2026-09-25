#ifndef FLYSIGHT_FUSION_SOLVERTHREADS_H
#define FLYSIGHT_FUSION_SOLVERTHREADS_H

#include <QtGlobal>

#include <functional>
#include <limits>

namespace FlySight::Fusion {

/// Runs `work` on the calling thread so that the solver's helper threads - the
/// oneTBB worker threads that GTSAM's parallel elimination runs on - take the
/// calling thread's priority while they help it, and give it back when they
/// leave. Exceptions from `work` propagate unchanged.
///
/// Why: the executor runs a fit on a below-normal thread, but oneTBB's workers
/// run at normal priority. Under load that is a priority inversion: the fit's
/// own helpers spin at normal priority and starve the below-normal thread that
/// drives the fit, so it stalls; and the heavy parallel part of the fit would
/// not run below normal at all. On the synchronous path (a normal-priority
/// caller) the helpers stay at normal priority.
///
/// The fit's arithmetic does not change: the same work runs in an arena of the
/// default concurrency, entered from the same thread.
void runWithSolverThreadsAtCallerPriority(const std::function<void()> &work);

/// What the helper threads of the fits run by runWithSolverThreadsAtCallerPriority()
/// did since the last resetSolverThreadRecord(), for the tests. Priorities are
/// native: THREAD_PRIORITY_* on Windows, the scheduling priority elsewhere.
/// Read and reset only while no fit runs.
struct SolverThreadRecord {
    /// Times a oneTBB worker thread joined a fit's work
    qint64 entries = 0;
    /// Of those, times it then ran at a priority other than that of the
    /// thread that runs the fit
    qint64 atOtherPriority = 0;
    /// The highest priority a helper ran at (lowest int when entries == 0)
    int highestPriority = std::numeric_limits<int>::min();
};
SolverThreadRecord solverThreadRecord();
void resetSolverThreadRecord();

} // namespace FlySight::Fusion

#endif // FLYSIGHT_FUSION_SOLVERTHREADS_H
