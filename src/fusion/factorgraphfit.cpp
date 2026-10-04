#include "fusion/factorgraphfit.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <gtsam/geometry/Pose3.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/linear/linearExceptions.h>
#include <gtsam/navigation/GPSFactor.h>
#include <gtsam/navigation/ImuFactor.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/Marginals.h>
#include <gtsam/slam/PriorFactor.h>

#include "fusion/imuintegration.h"
#include "fusion/scaledimufactor.h"

namespace FlySight::Fusion::Detail {

using gtsam::symbol_shorthand::B;
using gtsam::symbol_shorthand::S;
using gtsam::symbol_shorthand::T;
using gtsam::symbol_shorthand::V;
using gtsam::symbol_shorthand::X;

namespace {

constexpr size_t kStatesPerCheckpoint = 256;

// The held stage of the full fit divides the sensitivity tolerances, the scale
// prior's sigmas, by this: a thousand times tighter than the datasheet's holds
// every factor at one to well within anything the data could move it.
constexpr double kHeldScalePriorDivisor = 1000;

// An iteration may raise the cost by this much (rounding) before it counts as
// an increase, which is a failure.
constexpr double kCostIncreaseTolerance = 1e-6;
/// The floor, relative to max(1, cost), under which a predicted decrease at
/// a stall is rounding: the sum of a thousand squared residuals carries about
/// 1e-13 of itself, and the settling threshold (1e-8) is above this on every
/// production tuning, so the floor decides only under a test's negative
/// tolerance (runOptimizerPass()).
constexpr double kStallFloor = 1e-12;

/// The GNSS measurement of state k: position, then velocity.
void addGnssFactors(gtsam::NonlinearFactorGraph &graph, const Samples &d, size_t k)
{
    graph.emplace_shared<gtsam::GPSFactor>(X(k), d.position[k], gtsam::noiseModel::Diagonal::Sigmas(d.positionSigma[k]));
    graph.emplace_shared<gtsam::PriorFactor<gtsam::Vector3>>(V(k), d.velocity[k], gtsam::noiseModel::Diagonal::Sigmas(d.velocitySigma[k]));
}

/// The IMU between fixes k-1 and k, preintegrated at `bias`.
void addImuFactor(gtsam::NonlinearFactorGraph &graph, const Samples &d, size_t k,
                  const gtsam::imuBias::ConstantBias &bias, const Tuning &c)
{
    graph.emplace_shared<gtsam::ImuFactor>(X(k-1), V(k-1), X(k), V(k), B(0),
                                           preintegrateImu(d, d.gnssTime[k-1], d.gnssTime[k], bias,
                                                           gtsam::Vector6::Ones(), c.noise));
}

/// The IMU between fixes k-1 and k under the temperature model, the scaled
/// factor: the interval takes the temperature of its first fix, k-1
/// (ImuFactor's convention that the factor's bias is the bias at state i), is
/// preintegrated at that interval's bias and at the scale of the
/// linearization point `at`, and depends on S(0) through the
/// preintegration's scale Jacobian.
void addScaledImuFactor(gtsam::NonlinearFactorGraph &graph, const Samples &d, size_t k,
                        const BiasLinearization &at, const GyroBiasModel &model, const Tuning &c)
{
    const double dT = temperatureAtFix(d, k-1)-model.tRef;
    const gtsam::imuBias::ConstantBias bias = intervalBias(d, k-1, at.bias, at.slope, model);
    gtsam::Matrix96 scaleJacobian;
    const gtsam::PreintegratedImuMeasurements pim =
        preintegrateImu(d, d.gnssTime[k-1], d.gnssTime[k], bias, at.scale, c.noise, ImuStepObserver(),
                        &scaleJacobian);
    graph.emplace_shared<ScaledImuFactor>(X(k-1), V(k-1), X(k), V(k), B(0), T(0), S(0), pim, dT,
                                          scaleJacobian, at.scale);
}

/// A weak zero-mean prior on the shared bias: sensor biases are small, and
/// without it a short or gentle recording leaves them unobservable.
void addBiasPrior(gtsam::NonlinearFactorGraph &graph, const Tuning &c)
{
    gtsam::Vector6 sig;
    sig << c.accBiasSigma, c.accBiasSigma, c.accBiasSigma, c.gyroBiasSigma, c.gyroBiasSigma, c.gyroBiasSigma;
    graph.emplace_shared<gtsam::PriorFactor<gtsam::imuBias::ConstantBias>>(B(0), gtsam::imuBias::ConstantBias(), gtsam::noiseModel::Diagonal::Sigmas(sig));
}

/// A zero-mean prior on the slope T(0) at the datasheet's typical drift per
/// degC: a recording whose temperature does not change leaves the slope here.
void addSlopePrior(gtsam::NonlinearFactorGraph &graph, const Tuning &c)
{
    graph.emplace_shared<gtsam::PriorFactor<gtsam::Vector3>>(T(0), gtsam::Vector3::Zero(),
        gtsam::noiseModel::Diagonal::Sigmas(gtsam::Vector3::Constant(c.gyroBiasSlopeSigma)));
}

/// A zero-mean prior on the scale factors' departure from one, at the
/// datasheet's sensitivity tolerance of each sensor: an axis whose reading
/// the data do not constrain stays here. S(0) is a vector space, so the prior
/// on the factors with mean one is the prior on the departure.
void addScalePrior(gtsam::NonlinearFactorGraph &graph, const Tuning &c)
{
    const double accelerometer = c.noise.accelerometer.sensitivityTolerance;
    const double gyroscope = c.noise.gyroscope.sensitivityTolerance;
    gtsam::Vector6 sigmas;
    sigmas << accelerometer, accelerometer, accelerometer, gyroscope, gyroscope, gyroscope;
    graph.emplace_shared<gtsam::PriorFactor<gtsam::Vector6>>(S(0), gtsam::Vector6(gtsam::Vector6::Ones()),
                                                             gtsam::noiseModel::Diagonal::Sigmas(sigmas));
}

/// The linearization point of a graph: B(0) and, under the temperature model,
/// T(0) and S(0). The constant model reads neither: its slope is zero and its
/// scale one.
BiasLinearization linearizationOf(const gtsam::Values &values, const GyroBiasModel &model)
{
    return {values.at<gtsam::imuBias::ConstantBias>(B(0)),
            model.temperatureLinear ? values.at<gtsam::Vector3>(T(0)) : gtsam::Vector3::Zero(),
            model.temperatureLinear ? values.at<gtsam::Vector6>(S(0)) : gtsam::Vector6::Ones()};
}

/// The stopping account with only the thresholds filled, copied from the
/// tuning so that the account is complete on its own.
Stopping thresholdsOf(const Tuning &c)
{
    Stopping stopping;
    stopping.biasSettledTolerance = c.biasSettledTolerance;
    stopping.lambdaUpperBound = c.lambdaUpperBound;
    stopping.slowTailWindow = c.slowTailWindow;
    stopping.slowTailMaxMeanRelativeDecrease = c.slowTailMaxMeanRelativeDecrease;
    stopping.slowTailMaxNrms = c.slowTailMaxNrms;
    stopping.divergenceMaxImuNrms = c.divergenceMaxImuNrms;
    stopping.divergenceScaleRange = c.divergenceScaleRange;
    return stopping;
}

/// The mean of (before - after) / max(1, before) over the last min(window, n)
/// iterations of pass `outer` in `history`, n its iteration count. NaN when
/// the pass recorded nothing (a pass that ran has n >= 1).
double meanRelativeDecrease(const std::vector<FitIteration> &history, int outer, int window)
{
    std::vector<double> decreases;
    for (const FitIteration &h : history) {
        if (h.outer == outer)
            decreases.push_back((h.before-h.after)/std::max(1., h.before));
    }
    if (decreases.empty())
        return std::numeric_limits<double>::quiet_NaN();
    const size_t n = std::min(decreases.size(), size_t(std::max(window, 1)));
    double sum = 0;
    for (size_t i = decreases.size()-n; i < decreases.size(); ++i)
        sum += decreases[i];
    return sum/n;
}

/// The account of a pass that failed: the rule, the pass it happened in,
/// counted within its stage (whose first pass is `firstOuter`), and the mean
/// over that pass's iterations (the failing one is already in the history: its
/// mean may be negative or NaN, which is the point of reporting it).
Stopping failedPass(const Tuning &c, const char *rule, int outer, int firstOuter,
                    const std::vector<FitIteration> &history)
{
    Stopping stopping = thresholdsOf(c);
    stopping.rule = rule;
    stopping.passes = outer-firstOuter+1;
    stopping.lastPassMeanRelativeDecrease = meanRelativeDecrease(history, outer, c.slowTailWindow);
    return stopping;
}

/// One Levenberg-Marquardt pass over a fixed graph, driven by hand so that
/// every iteration is a boundary. Updates `values`; returns whether the cost
/// settled before the iteration limit. A pass may settle on a step that did
/// not move (before == after) while the damping is below its ceiling: that is
/// accepted, as it means LM found no better point at its current damping. The
/// same step at the ceiling (`lambdaUpperBound`) is judged by what the
/// linearization still predicts (predictedDecrease()): above the settling
/// threshold, LM has given up short of a minimum, every further iteration
/// returns the same values, and the pass throws FitFailure (`damping
/// saturated`); at or below it the pass has settled, because at a minimum the
/// library judges each trial step by the sign of a rounding-level linearized
/// change, and a run of negative signs raises the damping to the ceiling
/// without the cost ever being evaluated. Under a negative `relativeTolerance`
/// (a test forcing a pass that never settles) the same stall is the no-op
/// iteration that pass expects, as the library's stop below the ceiling
/// already gives, and the prediction is judged against the rounding floor. A non-finite or increasing cost
/// throws FitFailure (`cost increased`). Either carries the account of the
/// pass it happened in, counted within its stage (whose first pass is
/// `firstOuter`).
/// Each iteration's boundary text is `passFormat` with the pass and the
/// iteration filled in (fitFactorGraph()'s contract).
bool runOptimizerPass(const gtsam::NonlinearFactorGraph &graph, gtsam::Values &values,
                      const Tuning &c, int outer, int firstOuter, const QString &passFormat,
                      const Checkpoint &checkpoint, std::vector<FitIteration> &history)
{
    gtsam::LevenbergMarquardtParams params;
    params.setLinearSolverType("MULTIFRONTAL_QR");
    params.maxIterations = c.maxIterations;
    params.lambdaUpperBound = c.lambdaUpperBound;
    gtsam::LevenbergMarquardtOptimizer optimizer(graph, values, params);

    bool settled = false;
    for (int i = 0; i < c.maxIterations; ++i) {
        checkpoint(passFormat.arg(outer+1).arg(i+1));
        const double before = optimizer.error();
        const gtsam::GaussianFactorGraph::shared_ptr linear = optimizer.iterate();
        const double after = optimizer.error();
        history.push_back({outer, i, before, after});
        if (!std::isfinite(after) || after > before+kCostIncreaseTolerance)
            throw FitFailure("Nonfinite or increasing optimizer cost",
                             failedPass(c, StopRule::kCostIncreased, outer, firstOuter, history));
        const double threshold = c.relativeTolerance*std::max(1., before);
        if (after == before && optimizer.lambda() >= c.lambdaUpperBound) {
            if (predictedDecrease(*linear) > std::max(threshold, kStallFloor*std::max(1., before)))
                throw FitFailure("Optimizer damping saturated without progress",
                                 failedPass(c, StopRule::kDampingSaturated, outer, firstOuter, history));
            if (threshold < 0)
                continue;
            settled = true;
            break;
        }
        if (before-after <= threshold) {
            settled = true;
            break;
        }
    }
    values = optimizer.values();
    return settled;
}

/// Per-factor residuals, the RMS misfit to the GNSS measurements, and the
/// quality metrics: the normalized RMS of each factor kind and the objective
/// per state (result.objective is set before this is called). The factor
/// index walks the insertion order of buildFactorGraph(); no prior
/// contributes to a normalized RMS, and the temperature and scaled factors'
/// kind is `imu` like the stock one's (the same dimension, 9).
void collectResiduals(const Samples &d, const gtsam::NonlinearFactorGraph &graph, FitResult &result)
{
    const gtsam::Values &values = result.values;
    const size_t n = d.gnssTime.size();
    double sumPosition = 0, sumVelocity = 0, sumImu = 0;
    size_t factor = 0;
    for (size_t k = 0; k < n; ++k) {
        result.residuals.push_back({"position", k, d.gnssTime[k], 2*graph.at(factor++)->error(values)});
        sumPosition += result.residuals.back().squaredWhitenedError;
        result.residuals.push_back({"velocity", k, d.gnssTime[k], 2*graph.at(factor++)->error(values)});
        sumVelocity += result.residuals.back().squaredWhitenedError;
        if (k) {
            result.residuals.push_back({"imu", k, d.gnssTime[k], 2*graph.at(factor++)->error(values)});
            sumImu += result.residuals.back().squaredWhitenedError;
        }
        result.positionRms += (values.at<gtsam::Pose3>(X(k)).translation()-d.position[k]).squaredNorm();
        result.velocityRms += (values.at<gtsam::Vector3>(V(k))-d.velocity[k]).squaredNorm();
    }
    result.residuals.push_back({"bias_prior", 0, d.gnssTime[0], 2*graph.at(factor++)->error(values)});
    if (result.biasModel.temperatureLinear) {
        result.residuals.push_back({"slope_prior", 0, d.gnssTime[0], 2*graph.at(factor++)->error(values)});
        result.residuals.push_back({"scale_prior", 0, d.gnssTime[0], 2*graph.at(factor)->error(values)});
    }
    result.positionRms = std::sqrt(result.positionRms/n);
    result.velocityRms = std::sqrt(result.velocityRms/n);

    // Dimensions 3 (position, velocity) and 9 (IMU); n >= 3 is guaranteed by
    // fittedWindow() / validateSamples(), so there are at least two IMU factors.
    result.quality.positionNrms = std::sqrt(sumPosition/(3*n));
    result.quality.velocityNrms = std::sqrt(sumVelocity/(3*n));
    result.quality.imuNrms = std::sqrt(sumImu/(9*(n-1)));
    result.quality.objectivePerState = result.objective/n;
}

/// `values` as reported on `graph`, whose cost at them is `objective`: the
/// fitted slope and scale, the residuals and the quality.
FitResult evaluatedAt(const Samples &d, const gtsam::NonlinearFactorGraph &graph, const gtsam::Values &values,
                      double objective, const GyroBiasModel &model)
{
    FitResult result;
    result.values = values;
    result.objective = objective;
    result.biasModel = model;
    result.gyroBiasSlope = model.temperatureLinear ? values.at<gtsam::Vector3>(T(0)) : gtsam::Vector3::Zero();
    result.scale = model.temperatureLinear ? values.at<gtsam::Vector6>(S(0)) : gtsam::Vector6::Ones();
    collectResiduals(d, graph, result);
    return result;
}

/// Whether the pass evaluated in `pass` has left the model: its IMU
/// normalized RMS not strictly below the bound, or a factor not strictly
/// inside the range. Strict comparisons, so that a zero bound or an empty
/// interval refuses deterministically, and a NaN misfit is a divergence.
bool hasDiverged(const FitResult &pass, const Tuning &c)
{
    bool inside = pass.quality.imuNrms < c.divergenceMaxImuNrms;
    for (int i = 0; i < 6; ++i)
        inside = inside && c.divergenceScaleRange.lower < pass.scale(i) && pass.scale(i) < c.divergenceScaleRange.upper;
    return !inside;
}

/// One sequence of passes from `values`: the whole of a fit under the
/// constant model, one stage of the full fit under the temperature model (the
/// only one judged for divergence). At most `passBudget` passes, the first
/// numbered `firstOuter` in the texts and in `history`, to which every
/// iteration is appended. The result's `stopping` counts this stage's passes;
/// its `history` is left empty for the caller.
FitResult runPasses(const Samples &d, gtsam::Values values, const Tuning &c, const GyroBiasModel &model,
                    int passBudget, int firstOuter, const QString &passFormat, const Checkpoint &checkpoint,
                    std::vector<FitIteration> &history)
{
    Stopping stopping = thresholdsOf(c);

    // The graph after a pass is rebuilt once, at the pass's fitted bias and
    // scale, and serves three purposes: the cost test, the next pass's graph,
    // and (after the last pass) the reported graph. So there are passes + 1
    // builds, each reporting "Integrating IMU factors". Every build
    // preintegrates each interval at that interval's bias and at the scale of
    // the current linearization point; between builds the scale enters the
    // IMU factors to first order, as the bias does.
    gtsam::NonlinearFactorGraph graph = buildFactorGraph(d, linearizationOf(values, model), model, c, checkpoint);
    gtsam::NonlinearFactorGraph rebuilt;
    FitResult result;
    bool lastSettled = false, settled = false, diverged = false;
    int lastOuter = firstOuter;
    for (int pass = 0; pass < passBudget; ++pass) {
        const int outer = firstOuter+pass;
        lastOuter = outer;
        lastSettled = runOptimizerPass(graph, values, c, outer, firstOuter, passFormat, checkpoint, history);
        stopping.passes = pass+1;

        // The objective, the residuals and the quality are those of the graph
        // preintegrated at the pass's fitted bias and scale, not at those the
        // pass was linearized at: the rebuild. After the last pass it is also
        // the reported graph.
        rebuilt = buildFactorGraph(d, linearizationOf(values, model), model, c, checkpoint);
        const double costRebuilt = rebuilt.error(values);
        result = evaluatedAt(d, rebuilt, values, costRebuilt, model);
        const double passFinal = history.back().after;
        stopping.repreintegrationCostDifference = std::abs(costRebuilt-passFinal)/std::max(1., passFinal);

        // Divergence first: a pass that has left the model ends its stage
        // whatever the cost test would say.
        if (model.temperatureLinear && hasDiverged(result, c)) {
            diverged = true;
            break;
        }
        // The cost test: re-preintegrated at the pass's fitted bias and
        // scale, the cost at the pass's values must be what the pass ended at.
        if (lastSettled && stopping.repreintegrationCostDifference <= c.biasSettledTolerance) {
            settled = true;
            break;
        }
        // A pass that hit the limit is followed by another while passes
        // remain; the slow tail is judged only on the last one.
        if (pass+1 < passBudget)
            graph = std::move(rebuilt);
    }

    result.graph = std::move(rebuilt);
    result.stopping = stopping;
    result.stopping.lastPassMeanRelativeDecrease = meanRelativeDecrease(history, lastOuter, c.slowTailWindow);
    if (diverged) {
        result.stopping.rule = StopRule::kDiverged;
    } else if (settled) {
        result.stopping.rule = StopRule::kSettled;
        result.converged = true;
    } else if (lastSettled) {
        // The last pass settled but re-preintegrating still moved the cost.
        result.stopping.rule = StopRule::kBiasNotSettled;
    } else {
        // The slow tail: the last pass at its limit, judged on its last
        // window of iterations and on the misfit of every factor kind in
        // the rebuild: the GNSS measurements' (which do not depend on the
        // bias or the scale, so they are the pass's) and the IMU
        // factors', so that a pass that satisfies the fixes while it
        // ignores the IMU is never accepted. Strict comparisons, so that
        // a zero bound refuses deterministically.
        int n = 0;
        for (const FitIteration &h : history) {
            if (h.outer == lastOuter)
                ++n;
        }
        const bool accepted = n >= c.slowTailWindow
            && result.stopping.lastPassMeanRelativeDecrease < c.slowTailMaxMeanRelativeDecrease
            && result.quality.positionNrms < c.slowTailMaxNrms
            && result.quality.velocityNrms < c.slowTailMaxNrms
            && result.quality.imuNrms < c.slowTailMaxNrms;
        result.stopping.rule = accepted ? StopRule::kSlowTailAccepted : StopRule::kIterationLimit;
        result.converged = accepted;
    }
    return result;
}

/// `c` with the scale prior of the held stage: the sensitivity tolerances
/// divided by kHeldScalePriorDivisor. Nothing else of the tuning is read
/// differently, and the caller's tuning (whose noise the diagnostics report)
/// is not touched.
Tuning withHeldScalePrior(Tuning c)
{
    c.noise.accelerometer.sensitivityTolerance /= kHeldScalePriorDivisor;
    c.noise.gyroscope.sensitivityTolerance /= kHeldScalePriorDivisor;
    return c;
}

/// The account of a stage that returned, with `iterations` its own.
StageAccount accountOf(const FitResult &stage, size_t iterations)
{
    return {stage.stopping.rule, stage.stopping.passes, int(iterations), stage.objective};
}

} // namespace

double predictedDecrease(const gtsam::GaussianFactorGraph &linear)
{
    gtsam::VectorValues step;
    try {
        step = linear.optimize(gtsam::EliminateQR);
    } catch (const gtsam::IndeterminantLinearSystemException &) {
        return std::numeric_limits<double>::infinity();
    }
    return linear.error(gtsam::VectorValues::Zero(step))-linear.error(step);
}

GyroBiasModel gyroBiasModelFor(const Samples &window)
{
    if (window.temperature.empty())
        throw std::invalid_argument("Temperature model without a temperature series");
    // A plain sum in index order: the same value on every IEEE platform, and
    // exactly the value itself for a constant series.
    double sum = 0;
    for (double t : window.temperature)
        sum += t;
    GyroBiasModel model;
    model.temperatureLinear = true;
    model.tRef = sum/double(window.temperature.size());
    return model;
}

gtsam::imuBias::ConstantBias intervalBias(const Samples &d, size_t k, const gtsam::imuBias::ConstantBias &bias,
                                          const gtsam::Vector3 &slope, const GyroBiasModel &model)
{
    // The constant branch performs no arithmetic: the stock path is bit for
    // bit what it was.
    if (!model.temperatureLinear)
        return bias;
    return gtsam::imuBias::ConstantBias(bias.accelerometer(),
                                        bias.gyroscope() + slope*(temperatureAtFix(d, k)-model.tRef));
}

gtsam::NonlinearFactorGraph buildFactorGraph(const Samples &d, const gtsam::imuBias::ConstantBias &bias,
                                             const Tuning &c, const Checkpoint &checkpoint)
{
    return buildFactorGraph(d, BiasLinearization{bias}, GyroBiasModel{}, c, checkpoint);
}

gtsam::NonlinearFactorGraph buildFactorGraph(const Samples &d, const BiasLinearization &at,
                                             const GyroBiasModel &model, const Tuning &c,
                                             const Checkpoint &checkpoint)
{
    gtsam::NonlinearFactorGraph graph;
    for (size_t k = 0; k < d.gnssTime.size(); ++k) {
        // Preintegration is the slow part of construction: a boundary per block.
        if (k%kStatesPerCheckpoint == 0)
            checkpoint(QStringLiteral("Integrating IMU factors"));
        addGnssFactors(graph, d, k);
        if (k) {
            if (model.temperatureLinear)
                addScaledImuFactor(graph, d, k, at, model, c);
            else
                addImuFactor(graph, d, k, at.bias, c);
        }
    }
    addBiasPrior(graph, c);
    if (model.temperatureLinear) {
        addSlopePrior(graph, c);
        addScalePrior(graph, c);
    }
    return graph;
}

FitResult fitFactorGraph(const Samples &d, const InitialState &initial, const Tuning &c,
                         const QString &passFormat, const Checkpoint &checkpoint, const GyroBiasModel &model)
{
    // Validated by the caller already; kept because this is where the arrays
    // are indexed, and it costs nothing next to the fit.
    validateSamples(d, c);
    // A programming error, unreachable through planFit(); stated so that the
    // contract is checkable.
    if (model.temperatureLinear && d.temperature.empty())
        throw std::invalid_argument("Temperature model without a temperature series");

    gtsam::Values values = initialValues(d, initial);
    std::vector<FitIteration> history;
    if (!model.temperatureLinear) {
        FitResult result = runPasses(d, std::move(values), c, model, c.maxPasses, 0, passFormat, checkpoint,
                                     history);
        result.history = std::move(history);
        return result;
    }

    // The full fit starts b1 at zero (b0 is the initializer's) and the scale
    // factors at one, the datasheet's nominal sensitivity. Under the constant
    // model neither T(0) nor S(0) is inserted: no factor would touch them and
    // the linear system would be indeterminate.
    values.insert(T(0), gtsam::Vector3(gtsam::Vector3::Zero()));
    values.insert(S(0), gtsam::Vector6(gtsam::Vector6::Ones()));

    // The held stage: a global multiplicative parameter is not fitted from a
    // trajectory that has not settled, so the factors are held at one until
    // the fit has converged. One that does not converge is the fit, as it
    // stands; a FitFailure propagates.
    FitResult held = runPasses(d, std::move(values), withHeldScalePrior(c), model, c.maxPasses, 0, passFormat,
                               checkpoint, history);
    const size_t heldIterations = history.size();
    ScaleRelease release;
    release.held = accountOf(held, heldIterations);
    if (!held.converged) {
        held.scaleRelease = std::move(release);
        held.history = std::move(history);
        return held;
    }

    // The released stage: a refinement from the held solution, judged like
    // any sequence of passes. Its failures of every kind are the fallback, a
    // failure thrown at its boundary or in a pass among them; a cancellation
    // is not a failure and is not caught.
    std::optional<FitResult> released;
    try {
        checkpoint(QStringLiteral("Releasing the scale factors"));
        released = runPasses(d, held.values, c, model, c.releasePasses, held.stopping.passes, passFormat, checkpoint,
                             history);
        release.released = accountOf(*released, history.size()-heldIterations);
    } catch (const FitFailure &e) {
        // Thrown inside a pass, before its rebuild: no objective. A failure at
        // the boundary ran no iteration, and there is no released account.
        if (history.size() > heldIterations) {
            release.released = StageAccount{e.stopping.rule, e.stopping.passes, int(history.size()-heldIterations),
                                            std::numeric_limits<double>::quiet_NaN()};
        }
        release.reason = e.stopping.rule;
    }

    release.kept = released && released->converged;
    if (released && !release.kept)
        release.reason = released->stopping.rule;
    FitResult result = release.kept ? std::move(*released) : std::move(held);
    result.scaleRelease = std::move(release);
    result.history = std::move(history);
    return result;
}

double yawSigmaDeg(const gtsam::NonlinearFactorGraph &graph, const gtsam::Values &values, gtsam::Key key)
{
    try {
        gtsam::Marginals marginals(graph, values, gtsam::Marginals::QR);
        const gtsam::Matrix cov = marginals.marginalCovariance(key);
        // The Pose3 tangent is [rotation; translation] with the rotation
        // expressed in the body frame (retract is a right perturbation), so
        // the rotation block is rotated into the navigation frame before its
        // vertical (D) element means "about the vertical".
        const gtsam::Matrix3 body = cov.block<3, 3>(0, 0);
        const gtsam::Matrix3 R = values.at<gtsam::Pose3>(key).rotation().matrix();
        const gtsam::Matrix3 navigation = R*body*R.transpose();
        return cappedSigmaDeg(navigation(2, 2));
    } catch (const gtsam::IndeterminantLinearSystemException &) {
        // A rank-deficient system: the yaw is undetermined.
        return kYawSigmaCapDeg;
    }
}

} // namespace FlySight::Fusion::Detail
