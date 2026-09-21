#include "fusion/factorgraphfit.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <gtsam/geometry/Pose3.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/navigation/GPSFactor.h>
#include <gtsam/navigation/ImuFactor.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/slam/PriorFactor.h>

#include "fusion/imuintegration.h"

namespace FlySight::Fusion::Detail {

using gtsam::symbol_shorthand::B;
using gtsam::symbol_shorthand::V;
using gtsam::symbol_shorthand::X;

namespace {

constexpr int kMaxBiasPasses = 5;
constexpr size_t kStatesPerCheckpoint = 256;

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
    graph.emplace_shared<gtsam::ImuFactor>(X(k-1), V(k-1), X(k), V(k), B(0), preintegrateImu(d, d.gnssTime[k-1], d.gnssTime[k], bias, c));
}

/// A weak zero-mean prior on the shared bias: sensor biases are small, and
/// without it a short or gentle recording leaves them unobservable.
void addBiasPrior(gtsam::NonlinearFactorGraph &graph, const Tuning &c)
{
    gtsam::Vector6 sig;
    sig << c.accBiasSigma, c.accBiasSigma, c.accBiasSigma, c.gyroBiasSigma, c.gyroBiasSigma, c.gyroBiasSigma;
    graph.emplace_shared<gtsam::PriorFactor<gtsam::imuBias::ConstantBias>>(B(0), gtsam::imuBias::ConstantBias(), gtsam::noiseModel::Diagonal::Sigmas(sig));
}

/// One Levenberg-Marquardt pass over a fixed graph, driven by hand so that
/// every iteration is a boundary. Updates `values`; returns whether the cost
/// settled before the iteration limit. A pass may settle on a step that did
/// not move (before == after): that is accepted, as it means LM found no
/// better point at its current damping.
bool runOptimizerPass(const gtsam::NonlinearFactorGraph &graph, gtsam::Values &values,
                      const Tuning &c, int outer, const Checkpoint &checkpoint,
                      std::vector<FitIteration> &history)
{
    gtsam::LevenbergMarquardtParams params;
    params.setLinearSolverType("MULTIFRONTAL_QR");
    params.maxIterations = c.maxIterations;
    gtsam::LevenbergMarquardtOptimizer optimizer(graph, values, params);

    bool settled = false;
    for (int i = 0; i < c.maxIterations; ++i) {
        checkpoint(QStringLiteral("Pass %1, iteration %2").arg(outer+1).arg(i+1));
        const double before = optimizer.error();
        optimizer.iterate();
        const double after = optimizer.error();
        history.push_back({outer, i, before, after});
        if (!std::isfinite(after) || after > before+1e-6)
            throw std::runtime_error("Nonfinite or increasing optimizer cost");
        if (before-after <= c.relativeTolerance*std::max(1., before)) {
            settled = true;
            break;
        }
    }
    values = optimizer.values();
    return settled;
}

/// The bias moved so little that re-preintegrating would change nothing.
bool biasSettled(const gtsam::Vector6 &shift)
{
    return shift.head<3>().norm() < 1e-5 && shift.tail<3>().norm() < 1e-6;
}

/// Per-factor residuals and the RMS misfit to the GNSS measurements. The
/// factor index walks the insertion order of buildFactorGraph().
void collectResiduals(const Samples &d, const gtsam::NonlinearFactorGraph &graph, FitResult &result)
{
    const gtsam::Values &values = result.values;
    size_t factor = 0;
    for (size_t k = 0; k < d.gnssTime.size(); ++k) {
        result.residuals.push_back({"position", k, d.gnssTime[k], 2*graph.at(factor++)->error(values)});
        result.residuals.push_back({"velocity", k, d.gnssTime[k], 2*graph.at(factor++)->error(values)});
        if (k)
            result.residuals.push_back({"imu", k, d.gnssTime[k], 2*graph.at(factor++)->error(values)});
        result.positionRms += (values.at<gtsam::Pose3>(X(k)).translation()-d.position[k]).squaredNorm();
        result.velocityRms += (values.at<gtsam::Vector3>(V(k))-d.velocity[k]).squaredNorm();
    }
    result.residuals.push_back({"bias_prior", 0, d.gnssTime[0], 2*graph.at(factor)->error(values)});
    result.positionRms = std::sqrt(result.positionRms/d.gnssTime.size());
    result.velocityRms = std::sqrt(result.velocityRms/d.gnssTime.size());
}

} // namespace

gtsam::NonlinearFactorGraph buildFactorGraph(const Samples &d, const gtsam::imuBias::ConstantBias &bias,
                                             const Tuning &c, const Checkpoint &checkpoint)
{
    gtsam::NonlinearFactorGraph graph;
    for (size_t k = 0; k < d.gnssTime.size(); ++k) {
        // Preintegration is the slow part of construction: a boundary per block.
        if (k%kStatesPerCheckpoint == 0)
            checkpoint(QStringLiteral("Integrating IMU factors"));
        addGnssFactors(graph, d, k);
        if (k)
            addImuFactor(graph, d, k, bias, c);
    }
    addBiasPrior(graph, c);
    return graph;
}

FitResult fitFactorGraph(const Samples &d, double headingDeg, const Tuning &c,
                         const InitialAttitude &attitude, const Checkpoint &checkpoint)
{
    // Validated by the caller already; kept because this is where the arrays
    // are indexed, and it costs nothing next to the fit.
    validateSamples(d, c);

    FitResult result;
    result.heading = headingDeg;
    gtsam::Values values = initialValues(d, headingDeg, attitude);
    for (int outer = 0; outer < kMaxBiasPasses; ++outer) {
        const auto bias = values.at<gtsam::imuBias::ConstantBias>(B(0));
        const auto graph = buildFactorGraph(d, bias, c, checkpoint);
        const bool settled = runOptimizerPass(graph, values, c, outer, checkpoint, result.history);
        gtsam::Vector6 shift = values.at<gtsam::imuBias::ConstantBias>(B(0)).vector()-bias.vector();
        if (settled && biasSettled(shift)) {
            result.converged = true;
            break;
        }
    }

    // The objective and the residuals are those of the graph preintegrated at
    // the fitted bias, not at the bias the last pass was linearized at.
    const auto graph = buildFactorGraph(d, values.at<gtsam::imuBias::ConstantBias>(B(0)), c, checkpoint);
    result.values = values;
    result.objective = graph.error(values);
    collectResiduals(d, graph, result);
    return result;
}

} // namespace FlySight::Fusion::Detail
