#include "fusion/fusion.h"

#include <algorithm>
#include <exception>
#include <new>

#include "fusion/factorgraphfit.h"
#include "fusion/fusionoutput.h"
#include "fusion/fusionpipeline.h"
#include "fusion/initializer.h"
#include "fusion/inputadapter.h"
#include "fusion/trajectoryreconstruction.h"

namespace FlySight::Fusion {

namespace Detail {

namespace {

// A GNSS interval above max(this many seconds, this many median intervals) is
// an outage.
constexpr double kGnssOutageSeconds = 2.;
constexpr double kGnssOutageMedians = 5;

/// Everything decided before the fit starts.
struct FitPlan {
    PreparedInput prepared;
    Tuning tuning;
    Samples window;             ///< what the graph covers
};

/// Stage 1: is this a recording the model can use, and what does the fit
/// cover? Throws the rejection reason. A few single passes over the
/// recording; nothing is reported and nothing is asked: the first boundary
/// of a run is "Starting fit".
FitPlan planFit(const Channels &channels, const Tuning &baseTuning)
{
    FitPlan plan;
    plan.prepared = prepareInput(channels);
    const Samples &full = plan.prepared.recording;
    requireUsableRecording(full, plan.prepared.epoch, plan.prepared.usableStart);

    plan.tuning = baseTuning;
    plan.tuning.maxGap = kImuGapMedians*medianInterval(full.imuTime);
    plan.window = fittedWindow(full, plan.prepared.usableStart, full.gnssTime.back());
    validateSamples(plan.window, plan.tuning);
    requireNoGnssOutage(plan.window, std::max(kGnssOutageSeconds, kGnssOutageMedians*medianInterval(full.gnssTime)));
    return plan;
}

/// A Rejected or SolverFailed result: the reason, its diagnostics (with the
/// stopping account and the quality when the fit got far enough to have
/// them), no channels.
Result withoutChannels(Outcome outcome, const QString &reason, const Stopping *stopping = nullptr,
                       const Quality *quality = nullptr)
{
    Result result;
    result.outcome = outcome;
    result.reason = reason;
    result.diagnosticsJson = toCompactJson(failureDiagnostics(result.reason, stopping, quality));
    return result;
}

/// Stage 2: the initializer's fits, the full fit and everything after it. A
/// full fit that completed its passes without converging is a SolverFailed
/// result naming the rule that ended it; anything else that fails throws the
/// failure reason.
Result fitAndAssemble(const FitPlan &plan, const Checkpoint &checkpoint, PipelineTrace *trace)
{
    checkpoint(QStringLiteral("Starting fit"));
    const Initialization init = initialize(plan.window, plan.tuning, checkpoint);
    // Before the full fit: a cancelled or failed full fit keeps the account.
    if (trace)
        trace->initializer = init.account;
    // The fit still refreshes preintegration as the fitted biases change.
    const FitResult fit = fitFactorGraph(plan.window, init.state, plan.tuning,
                                         QString::fromLatin1(kFullFitPassFormat), checkpoint);
    if (trace) {
        trace->history = fit.history;
        trace->converged = fit.converged;
        trace->stopping = fit.stopping;
    }
    if (!fit.converged) {
        return withoutChannels(Outcome::SolverFailed,
            QStringLiteral("Batch fusion did not converge (%1); sensor fusion unavailable")
                .arg(QString::fromStdString(fit.stopping.rule)),
            &fit.stopping, &fit.quality);
    }

    const DenseTrajectory dense = reconstructTrajectory(plan.window, fit);
    Result result;
    result.outcome = Outcome::Succeeded;
    fillOutputChannels(dense, plan.prepared.epoch, result);
    result.diagnosticsJson = toCompactJson(
        successDiagnostics(plan.prepared, init.account, fit, plan.window, dense, plan.tuning));
    return result;
}

} // namespace

Result runPipeline(const Channels &channels, const Tuning &baseTuning,
                   const Checkpoint &checkpoint, PipelineTrace *trace)
{
    // Classification is by stage, not by exception type: the solver may throw
    // std::invalid_argument from inside a solve. Memory exhaustion is not a
    // function of the inputs and is never turned into a result. Anything that
    // is not a std::exception is not caught either: neither this library nor
    // the solver throws one, so it would be a defect, and the caller (the
    // engine) reports it as one instead of caching it as a property of the
    // recording.
    FitPlan plan;
    try {
        plan = planFit(channels, baseTuning);
    } catch (const FusionCancelled &) {
        return Result();
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::exception &e) {
        return withoutChannels(Outcome::Rejected, QString::fromUtf8(e.what()));
    }

    try {
        return fitAndAssemble(plan, checkpoint, trace);
    } catch (const FusionCancelled &) {
        return Result();
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const FitFailure &e) {
        // Before the generic handler: it is a std::runtime_error. A pass
        // raised the cost, so there is a stopping account but no rebuild and
        // therefore no quality.
        if (trace)
            trace->stopping = e.stopping;
        return withoutChannels(Outcome::SolverFailed, QString::fromUtf8(e.what()), &e.stopping);
    } catch (const std::exception &e) {
        return withoutChannels(Outcome::SolverFailed, QString::fromUtf8(e.what()));
    }
}

} // namespace Detail

Result run(const Channels &channels, const ProgressFn &progress, const CancelFn &cancelRequested)
{
    const Detail::Checkpoint checkpoint{progress, cancelRequested};
    return Detail::runPipeline(channels, Detail::Tuning{}, checkpoint);
}

} // namespace FlySight::Fusion
