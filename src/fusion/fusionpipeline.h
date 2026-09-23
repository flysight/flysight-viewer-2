#ifndef FLYSIGHT_FUSION_FUSIONPIPELINE_H
#define FLYSIGHT_FUSION_FUSIONPIPELINE_H

#include <vector>

#include "fusion/factorgraphfit.h"
#include "fusion/fusion.h"
#include "fusion/fusionprogress.h"
#include "fusion/fusionsamples.h"
#include "fusion/initializer.h"

// Internal to the fusion library: run() with its seams exposed, for the
// kernel tests. Nothing outside src/fusion/ and tests/tst_fusion_kernel.cpp
// includes this.

namespace FlySight::Fusion::Detail {

/// What a run went through, beyond what the diagnostics carry. Filled as far
/// as the run got.
struct PipelineTrace {
    InitialAttitude attitude;
    std::vector<FitIteration> history;
    bool converged = false;
    Stopping stopping;              ///< filled whenever a pass ran, including for a FitFailure
};

/// The whole fit: adapter, checks, window, initializer, fit, reconstruction,
/// channels and diagnostics, and the only place that catches. Anything thrown
/// before the fit starts is Outcome::Rejected; anything from the fit onward is
/// Outcome::SolverFailed; std::bad_alloc propagates.
///
/// `baseTuning` is Tuning{} in production (maxGap is always replaced by the
/// value derived from the recording). It is a parameter only so that a test
/// can force a stopping rule, e.g. non-convergence with maxIterations = 1 and
/// a negative relativeTolerance (no pass can settle).
Result runPipeline(const Channels &channels, const Tuning &baseTuning,
                   const Checkpoint &checkpoint, PipelineTrace *trace = nullptr);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_FUSIONPIPELINE_H
