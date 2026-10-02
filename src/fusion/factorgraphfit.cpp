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
#include "fusion/temperatureimufactor.h"

namespace FlySight::Fusion::Detail {

using gtsam::symbol_shorthand::B;
using gtsam::symbol_shorthand::S;
using gtsam::symbol_shorthand::T;
using gtsam::symbol_shorthand::V;
using gtsam::symbol_shorthand::X;

namespace {

constexpr size_t kStatesPerCheckpoint = 256;

// An iteration may raise the cost by this much (rounding) before it counts as
// an increase, which is a failure.
constexpr double kCostIncreaseTolerance = 1e-6;

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

/// The IMU between fixes k-1 and k under the temperature model: the interval
/// takes the temperature of its first fix, k-1 (ImuFactor's convention that
/// the factor's bias is the bias at state i), and is preintegrated at that
/// interval's bias and at the scale of the linearization point `at`. With the
/// scale state the factor also depends on S(0), through the preintegration's
/// scale Jacobian; without it the scale is one.
void addTemperatureImuFactor(gtsam::NonlinearFactorGraph &graph, const Samples &d, size_t k,
                             const BiasLinearization &at, const GyroBiasModel &model, const Tuning &c)
{
    const double dT = temperatureAtFix(d, k-1)-model.tRef;
    const gtsam::imuBias::ConstantBias bias = intervalBias(d, k-1, at.bias, at.slope, model);
    if (model.scaleState) {
        gtsam::Matrix96 scaleJacobian;
        const gtsam::PreintegratedImuMeasurements pim =
            preintegrateImu(d, d.gnssTime[k-1], d.gnssTime[k], bias, at.scale, c.noise, ImuStepObserver(),
                            &scaleJacobian);
        graph.emplace_shared<ScaledImuFactor>(X(k-1), V(k-1), X(k), V(k), B(0), T(0), S(0), pim, dT,
                                              scaleJacobian, at.scale);
        return;
    }
    graph.emplace_shared<TemperatureImuFactor>(X(k-1), V(k-1), X(k), V(k), B(0), T(0),
                                               preintegrateImu(d, d.gnssTime[k-1], d.gnssTime[k], bias,
                                                               at.scale, c.noise), dT);
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

/// A programming error, unreachable through planFit(): the scale state
/// belongs to the full fit, which runs the temperature model.
void requireTemperatureModelForScale(const GyroBiasModel &model)
{
    if (model.scaleState && !model.temperatureLinear)
        throw std::invalid_argument("Scale state without the temperature model");
}

/// The linearization point of a graph: B(0), under the temperature model
/// T(0), and with the scale state S(0). The constant model reads no T(0) and
/// has a zero slope; without the scale state the scale is one.
BiasLinearization linearizationOf(const gtsam::Values &values, const GyroBiasModel &model)
{
    return {values.at<gtsam::imuBias::ConstantBias>(B(0)),
            model.temperatureLinear ? values.at<gtsam::Vector3>(T(0)) : gtsam::Vector3::Zero(),
            model.scaleState ? values.at<gtsam::Vector6>(S(0)) : gtsam::Vector6::Ones()};
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

/// The account of a pass that failed: the rule, the pass it happened in and
/// the mean over that pass's iterations (the failing one is already in the
/// history: its mean may be negative or NaN, which is the point of reporting
/// it).
Stopping failedPass(const Tuning &c, const char *rule, int outer, const std::vector<FitIteration> &history)
{
    Stopping stopping = thresholdsOf(c);
    stopping.rule = rule;
    stopping.passes = outer+1;
    stopping.lastPassMeanRelativeDecrease = meanRelativeDecrease(history, outer, c.slowTailWindow);
    return stopping;
}

/// One Levenberg-Marquardt pass over a fixed graph, driven by hand so that
/// every iteration is a boundary. Updates `values`; returns whether the cost
/// settled before the iteration limit. A pass may settle on a step that did
/// not move (before == after) while the damping is below its ceiling: that is
/// accepted, as it means LM found no better point at its current damping. The
/// same step at the ceiling (`lambdaUpperBound`) is not: LM has given up
/// there, every further iteration returns the same values, and the start it
/// would call converged may be far from a minimum, so it throws FitFailure
/// (`damping saturated`). A non-finite or increasing cost throws FitFailure
/// (`cost increased`). Either carries the account of the pass it happened in.
/// Each iteration's boundary text is `passFormat` with the pass and the
/// iteration filled in (fitFactorGraph()'s contract).
bool runOptimizerPass(const gtsam::NonlinearFactorGraph &graph, gtsam::Values &values,
                      const Tuning &c, int outer, const QString &passFormat,
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
        optimizer.iterate();
        const double after = optimizer.error();
        history.push_back({outer, i, before, after});
        if (!std::isfinite(after) || after > before+kCostIncreaseTolerance)
            throw FitFailure("Nonfinite or increasing optimizer cost",
                             failedPass(c, StopRule::kCostIncreased, outer, history));
        if (after == before && optimizer.lambda() >= c.lambdaUpperBound)
            throw FitFailure("Optimizer damping saturated without progress",
                             failedPass(c, StopRule::kDampingSaturated, outer, history));
        if (before-after <= c.relativeTolerance*std::max(1., before)) {
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
    if (result.biasModel.temperatureLinear)
        result.residuals.push_back({"slope_prior", 0, d.gnssTime[0], 2*graph.at(factor++)->error(values)});
    if (result.biasModel.scaleState)
        result.residuals.push_back({"scale_prior", 0, d.gnssTime[0], 2*graph.at(factor)->error(values)});
    result.positionRms = std::sqrt(result.positionRms/n);
    result.velocityRms = std::sqrt(result.velocityRms/n);

    // Dimensions 3 (position, velocity) and 9 (IMU); n >= 3 is guaranteed by
    // fittedWindow() / validateSamples(), so there are at least two IMU factors.
    result.quality.positionNrms = std::sqrt(sumPosition/(3*n));
    result.quality.velocityNrms = std::sqrt(sumVelocity/(3*n));
    result.quality.imuNrms = std::sqrt(sumImu/(9*(n-1)));
    result.quality.objectivePerState = result.objective/n;
}

} // namespace

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
    requireTemperatureModelForScale(model);
    gtsam::NonlinearFactorGraph graph;
    for (size_t k = 0; k < d.gnssTime.size(); ++k) {
        // Preintegration is the slow part of construction: a boundary per block.
        if (k%kStatesPerCheckpoint == 0)
            checkpoint(QStringLiteral("Integrating IMU factors"));
        addGnssFactors(graph, d, k);
        if (k) {
            if (model.temperatureLinear)
                addTemperatureImuFactor(graph, d, k, at, model, c);
            else
                addImuFactor(graph, d, k, at.bias, c);
        }
    }
    addBiasPrior(graph, c);
    if (model.temperatureLinear)
        addSlopePrior(graph, c);
    if (model.scaleState)
        addScalePrior(graph, c);
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
    requireTemperatureModelForScale(model);

    FitResult result;
    result.stopping = thresholdsOf(c);
    Stopping &stopping = result.stopping;
    gtsam::Values values = initialValues(d, initial);
    // The full fit starts b1 at zero (b0 is the initializer's). Under the
    // constant model T(0) is never inserted: no factor would touch it and the
    // linear system would be indeterminate.
    if (model.temperatureLinear)
        values.insert(T(0), gtsam::Vector3(gtsam::Vector3::Zero()));
    // The scale factors start at one, the datasheet's nominal sensitivity.
    if (model.scaleState)
        values.insert(S(0), gtsam::Vector6(gtsam::Vector6::Ones()));

    // The graph after a pass is rebuilt once, at the pass's fitted bias and
    // scale, and serves three purposes: the cost test, the next pass's graph,
    // and (after the last pass) the reported graph. So there are passes + 1
    // builds, each reporting "Integrating IMU factors". Every build
    // preintegrates each interval at that interval's bias and at the scale of
    // the current linearization point; between builds the scale enters the
    // IMU factors to first order, as the bias does.
    gtsam::NonlinearFactorGraph graph = buildFactorGraph(d, linearizationOf(values, model), model, c, checkpoint);
    gtsam::NonlinearFactorGraph rebuilt;
    double costRebuilt = 0;
    bool lastSettled = false;
    int lastOuter = 0;
    for (int outer = 0; outer < c.maxPasses; ++outer) {
        lastOuter = outer;
        lastSettled = runOptimizerPass(graph, values, c, outer, passFormat, checkpoint, result.history);
        stopping.passes = outer+1;

        // The cost test: re-preintegrated at the pass's fitted bias and
        // scale, the cost at the pass's values must be what the pass ended at.
        rebuilt = buildFactorGraph(d, linearizationOf(values, model), model, c, checkpoint);
        costRebuilt = rebuilt.error(values);
        const double passFinal = result.history.back().after;
        stopping.repreintegrationCostDifference = std::abs(costRebuilt-passFinal)/std::max(1., passFinal);
        if (lastSettled && stopping.repreintegrationCostDifference <= c.biasSettledTolerance) {
            stopping.rule = StopRule::kSettled;
            result.converged = true;
            break;
        }
        // A pass that hit the limit is followed by another while passes
        // remain; the slow tail is judged only on the last one.
        if (outer+1 < c.maxPasses)
            graph = std::move(rebuilt);
    }

    // The objective, the residuals and the quality are those of the graph
    // preintegrated at the fitted bias and scale, not at those the last pass
    // was linearized at: the rebuild, whose cost the test above evaluated. It
    // is also the reported graph.
    result.values = values;
    result.objective = costRebuilt;
    result.biasModel = model;
    result.gyroBiasSlope = model.temperatureLinear ? values.at<gtsam::Vector3>(T(0)) : gtsam::Vector3::Zero();
    result.scale = model.scaleState ? values.at<gtsam::Vector6>(S(0)) : gtsam::Vector6::Ones();
    collectResiduals(d, rebuilt, result);
    result.graph = std::move(rebuilt);
    stopping.lastPassMeanRelativeDecrease = meanRelativeDecrease(result.history, lastOuter, c.slowTailWindow);

    if (!result.converged) {
        if (lastSettled) {
            // The last pass settled but re-preintegrating still moved the cost.
            stopping.rule = StopRule::kBiasNotSettled;
        } else {
            // The slow tail: the last pass at its limit, judged on its last
            // window of iterations and on the misfit to the GNSS measurements
            // (which does not depend on the bias or the scale, so the
            // rebuild's values are the pass's). Strict comparisons, so that a zero bound
            // refuses deterministically.
            int n = 0;
            for (const FitIteration &h : result.history) {
                if (h.outer == lastOuter)
                    ++n;
            }
            const bool accepted = n >= c.slowTailWindow
                && stopping.lastPassMeanRelativeDecrease < c.slowTailMaxMeanRelativeDecrease
                && result.quality.positionNrms < c.slowTailMaxNrms
                && result.quality.velocityNrms < c.slowTailMaxNrms;
            stopping.rule = accepted ? StopRule::kSlowTailAccepted : StopRule::kIterationLimit;
            result.converged = accepted;
        }
    }
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
