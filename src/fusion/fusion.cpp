#include "fusion/fusion.h"

#include <algorithm>
#include <exception>
#include <new>
#include <stdexcept>

#include "fusion/factorgraphfit.h"
#include "fusion/fusionoutput.h"
#include "fusion/fusionpipeline.h"
#include "fusion/initializer.h"
#include "fusion/inputadapter.h"
#include "fusion/trajectoryreconstruction.h"

namespace FlySight::Fusion {

namespace Detail {

namespace {

// One initial heading offset; heading remains free during optimization.
constexpr double kInitialHeadingDeg = 0.;

// A GNSS interval above max(this many seconds, this many median intervals) is
// an outage.
constexpr double kGnssOutageSeconds = 2.;
constexpr double kGnssOutageMedians = 5;

/// Everything decided before the fit starts.
struct FitPlan {
    PreparedInput prepared;
    Tuning tuning;
    Samples window;             ///< what the graph covers
    InitialAttitude attitude;
};

/// Stage 1: is this a recording the model can use, and where does the fit
/// start? Throws the rejection reason.
FitPlan planFit(const Channels &channels, const Tuning &baseTuning)
{
    FitPlan plan;
    plan.prepared = prepareInput(channels);
    const Samples &full = plan.prepared.recording;
    requireUsableRecording(full, plan.prepared.epoch, plan.prepared.usableStart);
    medianInterval(full.gnssTime);

    plan.tuning = baseTuning;
    plan.tuning.maxGap = kImuGapMedians*medianInterval(full.imuTime);
    plan.window = fittedWindow(full, plan.prepared.usableStart, full.gnssTime.back());
    validateSamples(plan.window, plan.tuning);
    requireNoGnssOutage(plan.window, std::max(kGnssOutageSeconds, kGnssOutageMedians*medianInterval(full.gnssTime)));

    // The initializer looks at the full recording, not only at the window.
    plan.attitude = initialAttitude(full, plan.window.gnssTime.front());
    return plan;
}

/// Stage 2: the fit and everything after it. Throws the failure reason.
Result fitAndAssemble(const FitPlan &plan, const Checkpoint &checkpoint, PipelineTrace *trace)
{
    checkpoint(QStringLiteral("Starting fit"));
    // The fit still refreshes preintegration as the fitted biases change.
    const FitResult fit = fitFactorGraph(plan.window, kInitialHeadingDeg, plan.tuning,
                                         plan.attitude, checkpoint);
    if (trace) {
        trace->history = fit.history;
        trace->converged = fit.converged;
    }
    if (!fit.converged)
        throw std::runtime_error("Batch fusion did not converge; sensor fusion unavailable");

    const DenseTrajectory dense = reconstructTrajectory(plan.window, fit);
    Result result;
    result.outcome = Outcome::Succeeded;
    channelsFrom(dense, plan.prepared.epoch, result);
    result.diagnosticsJson = toCompactJson(
        successDiagnostics(plan.prepared, plan.attitude, fit, plan.window, dense));
    return result;
}

/// A Rejected or SolverFailed result: the reason, its diagnostics, no channels.
Result withoutChannels(Outcome outcome, const char *what)
{
    Result result;
    result.outcome = outcome;
    result.reason = QString::fromUtf8(what);
    result.diagnosticsJson = toCompactJson(failureDiagnostics(result.reason));
    return result;
}

} // namespace

Result runPipeline(const Channels &channels, const Tuning &baseTuning,
                   const Checkpoint &checkpoint, PipelineTrace *trace)
{
    // Classification is by stage, not by exception type: the solver may throw
    // std::invalid_argument from inside a solve. Memory exhaustion is not a
    // function of the inputs and is never turned into a result.
    FitPlan plan;
    try {
        plan = planFit(channels, baseTuning);
    } catch (const FusionCancelled &) {
        return Result();
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::exception &e) {
        return withoutChannels(Outcome::Rejected, e.what());
    }
    if (trace)
        trace->attitude = plan.attitude;

    try {
        return fitAndAssemble(plan, checkpoint, trace);
    } catch (const FusionCancelled &) {
        return Result();
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::exception &e) {
        return withoutChannels(Outcome::SolverFailed, e.what());
    }
}

} // namespace Detail

Result run(const Channels &channels, const ProgressFn &progress, const CancelFn &cancelRequested)
{
    const Detail::Checkpoint checkpoint{progress, cancelRequested};
    return Detail::runPipeline(channels, Detail::Tuning{}, checkpoint);
}

} // namespace FlySight::Fusion
