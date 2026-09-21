#ifndef FLYSIGHT_FUSION_FACTORGRAPHFIT_H
#define FLYSIGHT_FUSION_FACTORGRAPHFIT_H

#include <string>
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

struct FitResult {
    gtsam::Values values;
    bool converged = false;
    double objective = 0;                        ///< graph error at `values`, preintegrated at the fitted bias
    double heading = 0;                          ///< the initial heading offset, deg
    double positionRms = 0, velocityRms = 0;     ///< fitted state vs GNSS measurement, vector RMS
    std::vector<FitIteration> history;
    std::vector<FactorResidual> residuals;       ///< in factor order; the bias prior last
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
/// at most five passes, until a pass settles without moving the bias.
/// Non-convergence is reported in the result, not thrown.
FitResult fitFactorGraph(const Samples &samples, double headingDeg, const Tuning &tuning,
                         const InitialAttitude &attitude,
                         const Checkpoint &checkpoint = Checkpoint());

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_FACTORGRAPHFIT_H
