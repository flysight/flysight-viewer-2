#ifndef FLYSIGHT_FUSION_FACTORGRAPHFIT_H
#define FLYSIGHT_FUSION_FACTORGRAPHFIT_H

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <QString>

#include <gtsam/inference/Key.h>
#include <gtsam/navigation/ImuBias.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/Values.h>

#include "fusion/fusionprogress.h"
#include "fusion/fusionsamples.h"
#include "fusion/initializer.h"

// Internal to the fusion library: the batch fit. One state (pose X(k),
// velocity V(k)) per GNSS fix, one accelerometer-and-gyro bias B(0) shared by
// the whole recording and, in the full fit, one slope T(0) that makes the gyro
// bias linear in the IMU temperature (temperatureimufactor.h) and the six
// scale factors S(0) by which the readings are divided (scaledimufactor.h);
// the initializer's fits keep the constant bias and unit scale. Each fix
// contributes a position and a velocity factor; successive states are tied by
// the preintegrated IMU between them; weak priors keep the bias near zero, the
// slope near zero and the scale factors near one. Heading is not constrained
// by any factor of its own: it is observable only through motion.

namespace FlySight::Fusion::Detail {

/// Cost before and after one optimizer iteration. Both indices are zero-based.
struct FitIteration { int outer, iteration; double before, after; };

/// One factor's share of the objective: twice its error, i.e. its squared
/// whitened residual.
struct FactorResidual { std::string kind; size_t node; double time, squaredWhitenedError; };

/// The rule that ended a fit, as the diagnostics report it.
namespace StopRule {
constexpr char kSettled[] = "settled";
constexpr char kSlowTailAccepted[] = "slow tail accepted";
constexpr char kIterationLimit[] = "iteration limit";
constexpr char kBiasNotSettled[] = "bias not settled";
constexpr char kCostIncreased[] = "cost increased";
constexpr char kDampingSaturated[] = "damping saturated";
}

/// How the fit ended: the rule, the measurements the rules were judged on,
/// and the thresholds in force (copied from Tuning, so the account is
/// complete on its own). A measurement that could not be taken is NaN; the
/// diagnostics write it as null.
struct Stopping {
    std::string rule;                            ///< one of StopRule; empty until a pass has run
    int passes = 0;                              ///< bias passes run, 1..maxPasses; the pass a failure happened in counts
    /// Mean of (before - after) / max(1, before) over the last
    /// min(slowTailWindow, n) iterations of the last pass, n its iteration count.
    double lastPassMeanRelativeDecrease = std::numeric_limits<double>::quiet_NaN();
    /// |cost of the graph rebuilt at the last pass's fitted bias and scale,
    /// evaluated at that pass's values - the pass's final cost| / max(1, the
    /// pass's final cost). NaN when no pass completed.
    double repreintegrationCostDifference = std::numeric_limits<double>::quiet_NaN();
    double biasSettledTolerance = 0;
    double lambdaUpperBound = 0;                 ///< the ceiling of the optimizer's damping
    int slowTailWindow = 0;
    double slowTailMaxMeanRelativeDecrease = 0;
    double slowTailMaxNrms = 0;
};

/// The quality of the reported fit. The normalized RMS of a factor kind is
/// sqrt(sum over its factors of the squared whitened error / (number of
/// factors x factor dimension)), dimensions 3 (position, velocity) and 9
/// (IMU); objectivePerState is the objective divided by the number of states.
struct Quality { double imuNrms = 0, positionNrms = 0, velocityNrms = 0, objectivePerState = 0; };

/// How a fit models the gyro bias and, beside it, whether it carries the
/// scale state. The full fit uses the temperature model (the public boundary
/// always supplies a temperature) with the scale state on (planFit() sets
/// it); the initializer's prefix and segment fits use the constant model with
/// it off. A fit without the scale state is the same fit with the readings
/// taken at unit scale, as the constant model is the temperature model at a
/// zero slope.
struct GyroBiasModel {
    bool temperatureLinear = false;   ///< true: b(t) = b0 + b1 (T(t) - tRef) with T(0); false: constant bias, stock ImuFactor, no T(0)
    double tRef = 0;                  ///< degC; the mean IMU temperature of the fitted window; meaningful only when temperatureLinear
    /// true: the six scale factors S(0) are variables of the graph, with their
    /// prior, and every IMU factor depends on them; false: no S(0), unit
    /// scale. Requires temperatureLinear: the state belongs to the full fit only.
    bool scaleState = false;
};

/// The temperature model for `window`, tRef its plain mean temperature (index
/// order), the scale state off. Throws std::invalid_argument("Temperature model without a temperature series") on an empty series.
GyroBiasModel gyroBiasModelFor(const Samples &window);

/// Where a graph is linearized: the constant bias, with the temperature
/// model the slope (zero under the constant model), and with the scale state
/// the scale factors (ones without it).
struct BiasLinearization {
    gtsam::imuBias::ConstantBias bias;
    gtsam::Vector3 slope = gtsam::Vector3::Zero();
    gtsam::Vector6 scale = gtsam::Vector6::Ones();
};

/// The bias the model assigns to the interval that starts at fix `k`: `bias`
/// unchanged under the constant model; with the temperature model its gyro
/// part shifted by slope * (T_k - tRef), T_k the temperature at fix k.
gtsam::imuBias::ConstantBias intervalBias(const Samples &d, size_t k, const gtsam::imuBias::ConstantBias &bias,
                                          const gtsam::Vector3 &slope, const GyroBiasModel &model);

struct FitResult {
    gtsam::Values values;
    bool converged = false;                      ///< true for `settled` and `slow tail accepted`
    double objective = 0;                        ///< graph error at `values`, preintegrated at the fitted bias and scale
    double positionRms = 0, velocityRms = 0;     ///< fitted state vs GNSS measurement, vector RMS
    std::vector<FitIteration> history;
    std::vector<FactorResidual> residuals;       ///< in factor order; the bias prior, then (temperature model) the slope prior, then (scale state) the scale prior, last
    Stopping stopping;
    Quality quality;
    gtsam::NonlinearFactorGraph graph;           ///< the graph the objective, residuals and quality were evaluated on:
                                                 ///< rebuilt at the fitted bias and scale, so linearized at `values` with every
                                                 ///< IMU factor's S - s^ zero; the initializer takes the marginal yaw sigma from it
    gtsam::Vector3 gyroBiasSlope = gtsam::Vector3::Zero();   ///< the fitted b1, rad/s per degC; zero under the constant model
    gtsam::Vector6 scale = gtsam::Vector6::Ones();           ///< the fitted S(0), accelerometer x, y, z then gyro; ones without the scale state
    GyroBiasModel biasModel;                     ///< the model this fit used (tRef for the diagnostics and the reconstruction)
};

/// The full fit's iteration text; the initializer's fits pass the segment texts.
inline constexpr char kFullFitPassFormat[] = "Pass %1, iteration %2";

/// Thrown by fitFactorGraph() when a pass makes the cost non-finite or raises
/// it, or leaves it unchanged with the optimizer's damping at its ceiling: the
/// two failures the fit cannot continue from. A std::runtime_error so that a
/// caller treating every failure alike (runPipeline(), an initializer's
/// "infinite-objective start") needs no special case, and carrying the
/// stopping account so the diagnostics can say `cost increased` or `damping
/// saturated`.
class FitFailure : public std::runtime_error {
public:
    FitFailure(const std::string &what, Stopping stopping)
        : std::runtime_error(what), stopping(std::move(stopping)) {}
    Stopping stopping;
};

/// The stock factor graph of `samples` (constant bias), with the IMU
/// preintegrated at `bias`: the six-argument form under the constant model.
/// Reports "Integrating IMU factors" through `checkpoint` every 256 states.
gtsam::NonlinearFactorGraph buildFactorGraph(const Samples &samples,
                                             const gtsam::imuBias::ConstantBias &bias,
                                             const Tuning &tuning,
                                             const Checkpoint &checkpoint = Checkpoint());

/// The factor graph of `samples` under `model`, every IMU factor preintegrated
/// at its interval's bias (intervalBias of `at`) and at `at.scale`. Factor
/// order is part of the numerical behavior (it fixes the elimination
/// ordering): per state the position factor, the velocity factor, then for
/// every state but the first the IMU factor; the bias prior; with the
/// temperature model the slope prior; and with the scale state the scale prior
/// last: mean one, sigmas the sensitivity tolerances of tuning.noise
/// (accelerometer x, y, z, then gyro). The IMU factor is stock under the
/// constant model, the temperature factor (temperatureimufactor.h) under the
/// temperature model, and with the scale state the factor of
/// scaledimufactor.h, given the preintegration's scale Jacobian. Throws
/// std::invalid_argument("Scale state without the temperature model") for a
/// scale state under the constant model.
gtsam::NonlinearFactorGraph buildFactorGraph(const Samples &samples, const BiasLinearization &at,
                                             const GyroBiasModel &model, const Tuning &tuning,
                                             const Checkpoint &checkpoint = Checkpoint());

/// Fits `samples` from `initial`. Preintegration is linearized at a fixed
/// bias and scale, so the fit alternates: optimize, re-preintegrate at the
/// new bias and scale, optimize again, for at most `maxPasses` passes. A
/// settled pass has converged when the graph rebuilt at its bias and scale changes the cost by at most
/// `biasSettledTolerance` (relative to max(1, cost)); a last pass that reaches
/// its iteration limit is accepted as a slow tail when its last
/// `slowTailWindow` iterations lowered the cost by less than
/// `slowTailMaxMeanRelativeDecrease` per iteration on average and the position
/// and velocity normalized RMS are both below `slowTailMaxNrms`. Otherwise the
/// result says which rule ended the fit and `converged` is false. A non-finite
/// or increasing cost throws FitFailure, and so does an iteration that leaves
/// the cost unchanged while the damping is at `lambdaUpperBound`: the
/// optimizer is stuck there, and that is never a convergence. The reported
/// objective, residuals, quality and `graph` are those of the graph rebuilt at
/// the fitted bias and scale.
///
/// Every iteration is reported through `checkpoint` as `passFormat` with its
/// two remaining QString::arg placeholders filled: the lower-numbered one with
/// the one-based pass, the other with the one-based iteration (QString::arg
/// fills the lowest-numbered placeholder left, so a caller formats the fixed
/// parts of its text first). The default reproduces the full fit's texts.
///
/// Under the temperature model (`model.temperatureLinear`) the graph carries
/// T(0), started at zero; every interval is preintegrated and evaluated at
/// its own bias in every build, and the fitted slope is returned in
/// `gyroBiasSlope`. With the scale state (`model.scaleState`) it also
/// carries S(0), started at ones; every build preintegrates at the scale of
/// the current values, and the fitted factors are returned in `scale`. The
/// default is the constant model without the scale state: the initializer's
/// prefix and segment fits are stock. A scale state under the constant model
/// throws std::invalid_argument, a programming error.
FitResult fitFactorGraph(const Samples &samples, const InitialState &initial, const Tuning &tuning,
                         const QString &passFormat = QString::fromLatin1(kFullFitPassFormat),
                         const Checkpoint &checkpoint = Checkpoint(),
                         const GyroBiasModel &model = GyroBiasModel());

/// The marginal standard deviation, in degrees, of the rotation of pose `key`
/// about the navigation vertical, from `graph` linearized at `values`
/// (QR factorization). Capped at 180 degrees: a rotation about the vertical is
/// an angle on a circle, and an indeterminate system or a non-finite
/// covariance means the yaw is simply undetermined, which is what 180 says.
double yawSigmaDeg(const gtsam::NonlinearFactorGraph &graph, const gtsam::Values &values, gtsam::Key key);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_FACTORGRAPHFIT_H
