#ifndef FLYSIGHT_FUSION_FACTORGRAPHFIT_H
#define FLYSIGHT_FUSION_FACTORGRAPHFIT_H

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <QString>

#include <gtsam/inference/Key.h>
#include <gtsam/linear/GaussianFactorGraph.h>
#include <gtsam/navigation/ImuBias.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/Values.h>

#include "fusion/fusionprogress.h"
#include "fusion/fusionsamples.h"
#include "fusion/initializer.h"

// Internal to the fusion library: the batch fit. One state (pose X(k),
// velocity V(k)) per GNSS fix, one accelerometer-and-gyro bias B(0) shared by
// the whole recording and, in the full fit, one slope T(0) that makes the gyro
// bias linear in the IMU temperature and the six scale factors S(0) by which
// the readings are divided, both entering through the full fit's one IMU
// factor (scaledimufactor.h); the initializer's fits keep the constant bias
// and unit scale with the stock factor. Each fix
// contributes a position and a velocity factor; successive states are tied by
// the preintegrated IMU between them; weak priors keep the bias near zero, the
// slope near zero and the scale factors near one. Heading is not constrained
// by any factor of its own: it is observable only through motion.

namespace FlySight::Fusion::Detail {

/// Cost before and after one optimizer iteration. Both indices are zero-based;
/// `outer`, the pass, counts on across the two stages of the full fit, so the
/// released stage's first pass is one more than the held stage's last.
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
constexpr char kDiverged[] = "diverged";
}

/// How a stage of the fit ended: the rule, the measurements the rules were
/// judged on, and the thresholds in force (copied from Tuning, so the account
/// is complete on its own). A measurement that could not be taken is NaN; the
/// diagnostics write it as null.
struct Stopping {
    std::string rule;                            ///< one of StopRule; empty until a pass has run
    int passes = 0;                              ///< the stage's own bias passes, 1..its budget (maxPasses, or the release budget of Tuning for the released stage); the pass a failure happened in counts
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
    double divergenceMaxImuNrms = 0;
    Tuning::ScaleRange divergenceScaleRange{0, 0};
};

/// The account of one stage of the full fit: the rule it ended under, its own
/// passes and iterations, and the objective of its last rebuilt graph (NaN
/// when the stage ended by a FitFailure, which is thrown inside a pass before
/// its rebuild).
struct StageAccount {
    std::string rule;
    int passes = 0;
    int iterations = 0;
    double objective = std::numeric_limits<double>::quiet_NaN();
};

/// The account of the scale factors' release: the held stage's; the released
/// stage's when it ran at least one iteration; whether the released stage is
/// the fit; and, when it was discarded, the rule it ended under (empty
/// otherwise, and when it was kept). Filled by the full fit; the
/// initializer's fits leave it default.
struct ScaleRelease {
    StageAccount held;
    std::optional<StageAccount> released;
    bool kept = false;
    std::string reason;
};

/// The quality of the reported fit. The normalized RMS of a factor kind is
/// sqrt(sum over its factors of the squared whitened error / (number of
/// factors x factor dimension)), dimensions 3 (position, velocity) and 9
/// (IMU); objectivePerState is the objective divided by the number of states.
struct Quality { double imuNrms = 0, positionNrms = 0, velocityNrms = 0, objectivePerState = 0; };

/// Which of the two fits a graph is. The full fit uses the temperature model
/// (the public boundary always supplies a temperature), and with it the scale
/// factors: the gyro bias linear in the temperature and the six scale factors
/// are variables of the graph, with their priors, and every IMU factor is the
/// scaled one. The initializer's prefix and segment fits use the constant
/// model: one constant bias, the readings at unit scale, the stock ImuFactor.
struct GyroBiasModel {
    bool temperatureLinear = false;   ///< true: b(t) = b0 + b1 (T(t) - tRef) with T(0), and S(0); false: constant bias, unit scale, stock ImuFactor, neither T(0) nor S(0)
    double tRef = 0;                  ///< degC; the mean IMU temperature of the fitted window; meaningful only when temperatureLinear
};

/// The temperature model for `window`, tRef its plain mean temperature (index
/// order). Throws std::invalid_argument("Temperature model without a temperature series") on an empty series.
GyroBiasModel gyroBiasModelFor(const Samples &window);

/// Where a graph is linearized: the constant bias and, under the temperature
/// model, the slope and the scale factors (zero and ones under the constant
/// model).
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

/// What a fit returns. For the full fit every member but `history` and
/// `scaleRelease` is the reported stage's: the released stage when it was
/// kept, the held stage otherwise.
struct FitResult {
    gtsam::Values values;
    bool converged = false;                      ///< true for `settled` and `slow tail accepted`
    double objective = 0;                        ///< graph error at `values`, preintegrated at the fitted bias and scale
    double positionRms = 0, velocityRms = 0;     ///< fitted state vs GNSS measurement, vector RMS
    std::vector<FitIteration> history;           ///< every iteration of the fit, of both stages of the full fit, kept or discarded
    std::vector<FactorResidual> residuals;       ///< in factor order; the bias prior, then (temperature model) the slope prior and the scale prior, last
    Stopping stopping;
    Quality quality;
    gtsam::NonlinearFactorGraph graph;           ///< the graph the objective, residuals and quality were evaluated on:
                                                 ///< rebuilt at the fitted bias and scale, so linearized at `values` with every
                                                 ///< IMU factor's S - s^ zero; the initializer takes the marginal yaw sigma from it
    gtsam::Vector3 gyroBiasSlope = gtsam::Vector3::Zero();   ///< the fitted b1, rad/s per degC; zero under the constant model
    gtsam::Vector6 scale = gtsam::Vector6::Ones();           ///< the fitted S(0), accelerometer x, y, z then gyro; ones under the constant model
    GyroBiasModel biasModel;                     ///< the model this fit used (tRef for the diagnostics and the reconstruction)
    ScaleRelease scaleRelease;                   ///< the full fit's account of its two stages; default under the constant model
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
/// every state but the first the IMU factor; the bias prior; and with the
/// temperature model the slope prior and, last, the scale prior: mean one,
/// sigmas the sensitivity tolerances of tuning.noise (accelerometer x, y, z,
/// then gyro). The IMU factor is stock under the constant model and, under the
/// temperature model, the scaled factor of scaledimufactor.h, given the
/// preintegration's scale Jacobian.
gtsam::NonlinearFactorGraph buildFactorGraph(const Samples &samples, const BiasLinearization &at,
                                             const GyroBiasModel &model, const Tuning &tuning,
                                             const Checkpoint &checkpoint = Checkpoint());

/// Fits `samples` from `initial`. Preintegration is linearized at a fixed
/// bias and scale, so the fit alternates: optimize, re-preintegrate at the
/// new bias and scale, optimize again, for at most `maxPasses` passes (the
/// tuning's release budget in the released stage of the full fit, below). A
/// settled pass has converged when the graph rebuilt at its bias and scale changes the cost by at most
/// `biasSettledTolerance` (relative to max(1, cost)); a last pass that reaches
/// its iteration limit is accepted as a slow tail when its last
/// `slowTailWindow` iterations lowered the cost by less than
/// `slowTailMaxMeanRelativeDecrease` per iteration on average and the
/// position, velocity and IMU normalized RMS are all below `slowTailMaxNrms`.
/// Otherwise the result says which rule ended the fit and `converged` is
/// false. A non-finite or increasing cost throws FitFailure, and so does an iteration that leaves
/// the cost unchanged while the damping is at `lambdaUpperBound` with the
/// linearization still predicting a decrease above the settling threshold
/// (predictedDecrease()): the optimizer is stuck short of a minimum, and that
/// is never a convergence. Stalled at the ceiling where it predicts none, the
/// pass has settled (under a test's negative `relativeTolerance`, which no
/// pass can satisfy, the stall is instead a no-op iteration, and the
/// prediction is judged against a rounding floor). The reported objective, residuals, quality and `graph`
/// are those of the graph rebuilt at the fitted bias and scale.
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
/// `gyroBiasSlope`; it also carries S(0), started at ones, every build
/// preintegrates at the scale of the current values, and the fitted factors
/// are returned in `scale`. The default is the constant model: the
/// initializer's prefix and segment fits are stock, one sequence of passes.
///
/// The full fit (the temperature model) runs that sequence of passes twice,
/// on the same graph with only the scale prior's sigma different. The held
/// stage, from `initial`, has the sensitivity tolerances divided by a thousand
/// (the factors held at one) and `maxPasses` passes. Only when it converged
/// (`settled` or `slow tail accepted`) is `Releasing the scale factors`
/// reported through `checkpoint`, and the released stage runs from the held
/// stage's values (every state, the bias, the slope and the factors) with the
/// tolerances themselves and the tuning's release budget of passes. A
/// released stage that converges is the fit. One that ends under any other rule, or throws
/// FitFailure (from a pass or from the release boundary), is discarded: the
/// held stage is the fit, converged. A held stage that does not converge is
/// the fit, not converged, and a FitFailure in it propagates. Passes are
/// numbered on across the stages in the texts and in `history`, which holds
/// both stages' iterations; `stopping` is the reported stage's own;
/// `scaleRelease` accounts for both.
///
/// In either stage of the full fit, after every pass's rebuild and before its
/// cost test, the pass has diverged unless the IMU normalized RMS of the
/// rebuilt graph at the pass's values is strictly below
/// `divergenceMaxImuNrms` and each of the six factors lies strictly inside
/// `divergenceScaleRange`; a pass that diverged ends its stage at once under
/// `diverged`, not converged, with the rebuild's objective, residuals and
/// quality (strict comparisons: a zero bound or an empty interval refuses
/// deterministically).
FitResult fitFactorGraph(const Samples &samples, const InitialState &initial, const Tuning &tuning,
                         const QString &passFormat = QString::fromLatin1(kFullFitPassFormat),
                         const Checkpoint &checkpoint = Checkpoint(),
                         const GyroBiasModel &model = GyroBiasModel());

/// What Levenberg-Marquardt would gain from `linear` with no damping: the
/// linearized cost at zero minus at its Gauss-Newton step (QR). At a minimum
/// it is the rounding of the cost's sum; infinity when the undamped system is
/// indeterminate, which no damping resolves. fitFactorGraph() judges a pass
/// stalled at the damping ceiling by it; a test reads it on a hand-built graph.
double predictedDecrease(const gtsam::GaussianFactorGraph &linear);

/// The cap of every attitude sigma the kernel reports, degrees: the heading
/// check's below and the published heading and tilt accuracies
/// (fitcovariance.h). An angle on a circle, so nothing above it says more, and
/// it reads "undetermined".
constexpr double kYawSigmaCapDeg = 180;

/// A navigation-frame attitude variance, rad^2, as the sigma the kernel
/// reports: its square root in degrees, capped at kYawSigmaCapDeg. A variance
/// that is not finite, or is negative, says the angle is undetermined and
/// reads as the cap. The one conversion of yawSigmaDeg() and the published
/// heading and tilt (attitudeAccuracy(), fitcovariance.h).
inline double cappedSigmaDeg(double variance)
{
    if (!std::isfinite(variance) || variance < 0)
        return kYawSigmaCapDeg;
    return std::min(std::sqrt(variance)*180/kPi, kYawSigmaCapDeg);
}

/// The marginal standard deviation, in degrees, of the rotation of pose `key`
/// about the navigation vertical, from `graph` linearized at `values`
/// (QR factorization). Capped at kYawSigmaCapDeg: a rotation about the vertical is
/// an angle on a circle, and an indeterminate system or a non-finite
/// covariance means the yaw is simply undetermined, which is what 180 says.
double yawSigmaDeg(const gtsam::NonlinearFactorGraph &graph, const gtsam::Values &values, gtsam::Key key);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_FACTORGRAPHFIT_H
