#ifndef FLYSIGHT_FUSION_FUSIONPIPELINE_H
#define FLYSIGHT_FUSION_FUSIONPIPELINE_H

#include <vector>

#include "fusion/factorgraphfit.h"
#include "fusion/fitcovariance.h"
#include "fusion/fusion.h"
#include "fusion/fusionprogress.h"
#include "fusion/fusionsamples.h"
#include "fusion/initializer.h"
#include "fusion/inputadapter.h"

// Internal to the fusion library: run() with its seams exposed, for the
// kernel tests. Nothing outside src/fusion/ and tests/tst_fusion_kernel.cpp
// includes this.

namespace FlySight::Fusion::Detail {

/// What a run went through, beyond what the diagnostics carry. Filled as far
/// as the run got: `initializer` once the initializer returned, before the
/// full fit (so a full fit that is cancelled or fails keeps the account);
/// `history`, `converged` and `stopping` are the full fit's (the segment fits'
/// histories are not traced; their iteration counts are in the account).
struct PipelineTrace {
    InitializerAccount initializer;
    std::vector<FitIteration> history;
    bool converged = false;
    Stopping stopping;              ///< filled whenever a pass ran, including for a FitFailure
};

/// The whole fit: adapter, checks, window, initializer, fit, covariance step,
/// reconstruction, channels and diagnostics, and the only place that catches. Anything thrown
/// before the fit starts is Outcome::Rejected; anything from the fit onward is
/// Outcome::SolverFailed; std::bad_alloc propagates. The initializer's prefix
/// and segment fits are part of the fit stage: they run after "Starting fit".
///
/// `baseTuning` is Tuning{} in production (maxGap and the noise are always
/// replaced by the values derived from the recording). It is a parameter only
/// so that a test can force a stopping rule, e.g. non-convergence with
/// maxIterations = 1 and a negative relativeTolerance (no pass can settle), or
/// damping saturation with a lower lambdaUpperBound.
Result runPipeline(const Channels &channels, const Tuning &baseTuning,
                   const Checkpoint &checkpoint, PipelineTrace *trace = nullptr);

/// What runPipeline() does with a converged `fit` of `window` once its
/// covariance step has run: the reconstruction at every IMU sample, which
/// composes `covariance` when it was computed, the widening, the channels and
/// the success diagnostics. runPipeline() passes fitCovariance() of `fit`; a
/// test passes one that failed, to see that nothing but the accuracy changes.
/// No checkpoint.
Result assembleSuccess(const PreparedInput &prepared, const InitializerAccount &account, const Samples &window,
                       const FitResult &fit, const Tuning &tuning, const FitCovariance &covariance);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_FUSIONPIPELINE_H
