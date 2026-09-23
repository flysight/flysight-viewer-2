#ifndef FLYSIGHT_FUSION_FACTORGRAPHFIT_H
#define FLYSIGHT_FUSION_FACTORGRAPHFIT_H

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtsam/navigation/ImuBias.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/Values.h>

#include "fusion/fusionprogress.h"
#include "fusion/fusionsamples.h"
#include "fusion/initializer.h"

// Internal to the fusion library: the batch fit. One state (pose X(k),
// velocity V(k)) per GNSS fix, one accelerometer-and-gyro bias B(0) shared by
// the whole recording. Each fix contributes a position and a velocity factor;
// successive states are tied by the preintegrated IMU between them; a weak
// prior keeps the bias near zero. Heading is not constrained by any factor of
// its own: it is observable only through motion.

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
}

/// How the fit ended: the rule, the measurements the rules were judged on,
/// and the thresholds in force (copied from Tuning, so the account is
/// complete on its own). A measurement that could not be taken is NaN; the
/// diagnostics write it as null.
struct Stopping {
    std::string rule;                            ///< one of StopRule; empty until a pass has run
    int passes = 0;                              ///< bias passes run, 1..5; the pass a failure happened in counts
    /// Mean of (before - after) / max(1, before) over the last
    /// min(slowTailWindow, n) iterations of the last pass, n its iteration count.
    double lastPassMeanRelativeDecrease = std::numeric_limits<double>::quiet_NaN();
    /// |cost of the graph rebuilt at the last pass's fitted bias, evaluated at
    /// that pass's values - the pass's final cost| / max(1, the pass's final
    /// cost). NaN when no pass completed.
    double repreintegrationCostDifference = std::numeric_limits<double>::quiet_NaN();
    double biasSettledTolerance = 0;
    int slowTailWindow = 0;
    double slowTailMaxMeanRelativeDecrease = 0;
    double slowTailMaxNrms = 0;
};

/// The quality of the reported fit. The normalized RMS of a factor kind is
/// sqrt(sum over its factors of the squared whitened error / (number of
/// factors x factor dimension)), dimensions 3 (position, velocity) and 9
/// (IMU); objectivePerState is the objective divided by the number of states.
struct Quality { double imuNrms = 0, positionNrms = 0, velocityNrms = 0, objectivePerState = 0; };

struct FitResult {
    gtsam::Values values;
    bool converged = false;                      ///< true for `settled` and `slow tail accepted`
    double objective = 0;                        ///< graph error at `values`, preintegrated at the fitted bias
    double heading = 0;                          ///< the initial heading offset, deg
    double positionRms = 0, velocityRms = 0;     ///< fitted state vs GNSS measurement, vector RMS
    std::vector<FitIteration> history;
    std::vector<FactorResidual> residuals;       ///< in factor order; the bias prior last
    Stopping stopping;
    Quality quality;
};

/// Thrown by fitFactorGraph() when a pass makes the cost non-finite or raises
/// it, the one failure the fit cannot continue from. A std::runtime_error so
/// that a caller treating every failure alike (runPipeline(), an initializer's
/// "infinite-objective start") needs no special case, and carrying the
/// stopping account so the diagnostics can say `cost increased`.
class FitFailure : public std::runtime_error {
public:
    FitFailure(const std::string &what, Stopping stopping)
        : std::runtime_error(what), stopping(std::move(stopping)) {}
    Stopping stopping;
};

/// The factor graph of `samples`, with the IMU preintegrated at `bias`.
/// Reports "Integrating IMU factors" through `checkpoint` every 256 states.
///
/// Factor order is part of the numerical behavior (it fixes the elimination
/// ordering): per state the position factor, the velocity factor, then for
/// every state but the first the IMU factor; the bias prior last.
gtsam::NonlinearFactorGraph buildFactorGraph(const Samples &samples,
                                             const gtsam::imuBias::ConstantBias &bias,
                                             const Tuning &tuning,
                                             const Checkpoint &checkpoint = Checkpoint());

/// Fits `samples`. Preintegration is linearized at a fixed bias, so the fit
/// alternates: optimize, re-preintegrate at the new bias, optimize again, for
/// at most five passes. A settled pass has converged when the graph rebuilt at
/// its bias changes the cost by at most `biasSettledTolerance` (relative to
/// max(1, cost)); a fifth pass that reaches its iteration limit is accepted as
/// a slow tail when its last `slowTailWindow` iterations lowered the cost by
/// less than `slowTailMaxMeanRelativeDecrease` per iteration on average and
/// the position and velocity normalized RMS are both below `slowTailMaxNrms`.
/// Otherwise the result says which rule ended the fit and `converged` is
/// false. A non-finite or increasing cost throws FitFailure. The reported
/// objective, residuals and quality are those of the graph rebuilt at the
/// fitted bias.
FitResult fitFactorGraph(const Samples &samples, double headingDeg, const Tuning &tuning,
                         const InitialAttitude &attitude,
                         const Checkpoint &checkpoint = Checkpoint());

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_FACTORGRAPHFIT_H
