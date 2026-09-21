#ifndef FLYSIGHTTEST_SOLVERPROBE_H
#define FLYSIGHTTEST_SOLVERPROBE_H

// A small, self-contained exercise of the solver dependencies (GTSAM and,
// through it, its bundled Eigen, METIS and oneTBB). Header-only and free of
// Qt so that the same code serves the QtTest smoke test (tst_solver_smoke)
// and the plain executable that is run inside an installed application tree
// (solver_deploy_probe).
//
// It answers "does the exported gtsam target compile, link, load and solve?".
// It is not a numerical fixture and uses nothing from the fusion model.

#include <cmath>
#include <cstddef>
#include <exception>
#include <sstream>
#include <string>

#include <gtsam/geometry/Pose3.h>
#include <gtsam/inference/Ordering.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/slam/PriorFactor.h>

#if defined(_MSC_VER)
#  define FLYSIGHTTEST_NOINLINE __declspec(noinline)
#else
#  define FLYSIGHTTEST_NOINLINE __attribute__((noinline))
#endif

namespace FlySightTest {

struct SolverProbeResult {
    bool ok = false;
    double error = 0.0;      ///< Final graph error (0.5 * sum of squared whitened residuals)
    std::string detail;      ///< One line describing the outcome, for failure messages
};

/// Builds a pose chain whose measurements agree exactly, starts the optimizer
/// away from the answer, and checks that it comes back.
///
/// The graph is a prior on the first pose, a chain of identical relative-pose
/// factors, and a prior on the last pose that agrees with the chain, so the
/// solution is known analytically and the final error is zero up to rounding.
inline SolverProbeResult runSolverProbe()
{
    SolverProbeResult result;

    try {
        constexpr int kSteps = 10;

        const gtsam::Pose3 first;
        const gtsam::Pose3 step(gtsam::Rot3::RzRyRx(0.01, -0.02, 0.10),
                                gtsam::Point3(1.0, 0.2, -0.1));

        // Rotation sigmas first (radians), then translation (metres).
        gtsam::Vector6 sigmas;
        sigmas << 0.01, 0.01, 0.01, 0.05, 0.05, 0.05;
        const auto noise = gtsam::noiseModel::Diagonal::Sigmas(sigmas);

        // Analytic answer, and an initial estimate pushed away from it by a
        // different, deterministic amount at every key.
        gtsam::Values truth;
        gtsam::Values initial;
        gtsam::Pose3 pose = first;
        for (int k = 0; k <= kSteps; ++k) {
            truth.insert(gtsam::Key(k), pose);

            gtsam::Vector6 offset;
            offset << 0.02, -0.03, 0.04, 0.10, -0.15, 0.05;
            offset *= (k % 2 == 0 ? 1.0 : -1.0) * (1.0 + 0.1 * k);
            initial.insert(gtsam::Key(k), pose.retract(offset));

            pose = pose.compose(step);
        }
        const gtsam::Pose3 last = truth.at<gtsam::Pose3>(gtsam::Key(kSteps));

        gtsam::NonlinearFactorGraph graph;
        graph.addPrior(gtsam::Key(0), first, noise);
        for (int k = 0; k < kSteps; ++k)
            graph.emplace_shared<gtsam::BetweenFactor<gtsam::Pose3>>(
                gtsam::Key(k), gtsam::Key(k + 1), step, noise);
        graph.emplace_shared<gtsam::PriorFactor<gtsam::Pose3>>(gtsam::Key(kSteps), last, noise);

        // The default ordering is COLAMD; ask METIS for one as well so that
        // metis-gtsam is called, not merely loaded.
        const gtsam::Ordering metisOrdering = gtsam::Ordering::Metis(graph);
        if (metisOrdering.size() != std::size_t(kSteps + 1)) {
            result.detail = "METIS ordering has the wrong number of keys";
            return result;
        }

        gtsam::LevenbergMarquardtOptimizer optimizer(graph, initial);
        const gtsam::Values solution = optimizer.optimize();

        result.error = graph.error(solution);
        const double translationError =
            (solution.at<gtsam::Pose3>(gtsam::Key(kSteps)).translation() - last.translation()).norm();

        result.ok = std::isfinite(result.error) && result.error < 1e-9
                    && translationError < 1e-6;

        std::ostringstream text;
        text << "initial error=" << graph.error(initial)
             << ", final error=" << result.error
             << ", last-pose translation error=" << translationError
             << ", iterations=" << optimizer.iterations();
        result.detail = text.str();
    } catch (const std::exception &e) {
        result.ok = false;
        result.detail = std::string("exception: ") + e.what();
    }

    return result;
}

namespace detail {

constexpr std::size_t kStackFrameBytes = 64 * 1024;

/// One frame of consumeStack(). The buffer is volatile and written end to
/// end so every page of it is really touched, and it is read again after the
/// recursive call so the call is not a tail call and the frame must stay
/// alive underneath it.
FLYSIGHTTEST_NOINLINE inline std::size_t consumeStackFrame(std::size_t remaining, unsigned &checksum)
{
    volatile unsigned char frame[kStackFrameBytes];
    for (std::size_t i = 0; i < kStackFrameBytes; ++i)
        frame[i] = static_cast<unsigned char>(i + remaining);

    std::size_t consumed = kStackFrameBytes;
    if (remaining > kStackFrameBytes)
        consumed += consumeStackFrame(remaining - kStackFrameBytes, checksum);

    checksum += static_cast<unsigned>(frame[0]) + static_cast<unsigned>(frame[kStackFrameBytes - 1]);
    return consumed;
}

} // namespace detail

/// Uses at least @p bytes of the calling thread's stack and returns the
/// number of bytes used. Crashes with a stack overflow if the thread's stack
/// is smaller than that, which is the point: it is how a test proves that its
/// main thread got the 64 MiB stack of flysight_solver_stack().
inline std::size_t consumeStack(std::size_t bytes)
{
    unsigned checksum = 0;
    const std::size_t consumed = detail::consumeStackFrame(bytes, checksum);

    // Every frame contributes its first and last byte after the frames below
    // it have returned. Recomputing the sum here makes the result depend on
    // each frame having kept its contents; a mismatch reports zero bytes.
    unsigned expected = 0;
    for (std::size_t remaining = bytes;; remaining -= detail::kStackFrameBytes) {
        expected += static_cast<unsigned char>(remaining);
        expected += static_cast<unsigned char>(detail::kStackFrameBytes - 1 + remaining);
        if (remaining <= detail::kStackFrameBytes)
            break;
    }
    return checksum == expected ? consumed : 0;
}

} // namespace FlySightTest

#endif // FLYSIGHTTEST_SOLVERPROBE_H
