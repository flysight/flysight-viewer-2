#include "fusion/fusion.h"

#include <exception>
#include <new>
#include <stdexcept>

#include <QString>

#include "fusion/factorgraphfit.h"
#include "fusion/fitcovariance.h"
#include "fusion/fusionoutput.h"
#include "fusion/fusionpipeline.h"
#include "fusion/initializer.h"
#include "fusion/inputadapter.h"
#include "fusion/trajectoryreconstruction.h"
#include "samplecontinuity.h"

namespace FlySight::Fusion {

namespace Detail {

namespace {

// The longest interval between successive fixes the fit bridges, s, hole or
// not: the limit is the factor's span. An interval is spanned by one
// preintegrated IMU factor, into which the bias and the scale enter to first
// order within a pass, and the settle test rebuilds the graph at the fitted
// values; over one long factor that correction is poor enough that the passes
// stop contracting. Measured on the synthetic long-hole recording
// (docs/SENSOR_FUSION.md section 6): every hole up to 40 s settles, 50 s on
// the fifth pass, 52 to 58 s cycle, 60 s only on the twelfth of thirty. The
// limit is twice the longest hole of the reference corpus.
constexpr double kLongestBridgedIntervalSeconds = 30;

/// Everything decided before the fit starts.
struct FitPlan {
    PreparedInput prepared;
    Tuning tuning;
    Samples window;             ///< what the graph covers
    GyroBiasModel biasModel;    ///< the full fit's model: the gyro bias temperature-linear about the window's mean temperature, with the scale factors
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
    // The IMU gap rule is the application's continuity rule on the whole
    // recording's IMU axis: an interval longer than its hole threshold is
    // missing data, for the window checks and the attitude propagation alike.
    // It is the one disconnection. A hole in the fixes with the IMU running
    // through it is bridged by the one IMU factor that spans the interval,
    // whose covariance prices the hole, up to the longest the fit bridges.
    // prepareInput() guaranteed finite, strictly increasing axes and
    // requireUsableRecording() three samples of each, so both are finite.
    const double imuInterval = SampleContinuity::nominalInterval(full.imuTime);
    plan.tuning.maxGap = SampleContinuity::holeThreshold(full.imuTime);
    plan.window = fittedWindow(full, plan.prepared.usableStart, full.gnssTime.back());
    validateSamples(plan.window, plan.tuning);
    // Every interval of the fitted window, whether or not the continuity
    // rule calls it a hole (the holes it finds serve the diagnostics).
    const std::vector<double> &fixes = plan.window.gnssTime;
    for (size_t k = 1; k < fixes.size(); ++k) {
        const double interval = fixes[k]-fixes[k-1];
        if (interval > kLongestBridgedIntervalSeconds) {
            // QString::number, as the IMU gap's reason: not subject to the C
            // locale Qt installs on Unix. Two decimals, so that an interval a
            // hair over the limit does not read as the limit.
            const QString message = QStringLiteral("GNSS fixes ") + QString::number(interval, 'f', 2)
                + QStringLiteral(" s apart; fusion bridges at most ")
                + QString::number(kLongestBridgedIntervalSeconds) + QStringLiteral(" s");
            throw std::invalid_argument(message.toStdString());
        }
    }
    // The configuration's checks come after every check of the recording
    // itself, so that a recording's own defect is the reason it reports: the
    // datasheet entry, then the lattice of every reading, then the rates.
    const ImuNoise noise = imuNoise(channels.imuConfiguration);
    requireReadingsOnLattice(channels);
    requireStatedRates(imuInterval, channels.imuConfiguration);
    plan.tuning.noise = noise;
    // T_ref is a property of the fitted window (the mean of its IMU samples'
    // temperature), decided before the fit starts. The temperature model
    // carries the scale factors: the initializer's fits keep the stock model.
    plan.biasModel = gyroBiasModelFor(plan.window);
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
    // The fit still refreshes preintegration as the fitted biases and scale
    // factors change. The full fit alone runs the temperature model with the
    // scale state; the initializer's fits above took the stock model by
    // default.
    const FitResult fit = fitFactorGraph(plan.window, init.state, plan.tuning,
                                         QString::fromLatin1(kFullFitPassFormat), checkpoint, plan.biasModel);
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

    // The covariance of the converged fit, then the fitted states and their
    // accuracy at every IMU sample. Neither is a boundary: one factorization
    // and one pass over the samples after the last iteration, a few seconds on
    // the longest recording, so they run to their end once the fit has
    // converged. A covariance that cannot be computed is part of the success.
    return assembleSuccess(plan.prepared, init.account, plan.window, fit, plan.tuning,
                           fitCovariance(fit, plan.window.gnssTime.size()));
}

} // namespace

Result assembleSuccess(const PreparedInput &prepared, const InitializerAccount &account, const Samples &window,
                       const FitResult &fit, const Tuning &tuning, const FitCovariance &covariance)
{
    const ImuRateTrajectory trajectory = reconstructAtImuRate(window, fit, tuning, &covariance);
    // The widening of every sample, when there is an accuracy to widen.
    std::vector<double> widenings;
    if (!trajectory.headingAcc.empty()) {
        for (const double factor : wideningFactors(window.gnssTime, fit.residuals, trajectory.time))
            widenings.push_back(widening(factor));
    }
    Result result;
    result.outcome = Outcome::Succeeded;
    fillOutputChannels(trajectory, widenings, prepared.epoch, result);
    result.diagnosticsJson = toCompactJson(
        successDiagnostics(prepared, account, fit, window, trajectory, tuning, covariance, widenings, result));
    return result;
}

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
