#ifndef FLYSIGHT_FUSION_FUSIONSAMPLES_H
#define FLYSIGHT_FUSION_FUSIONSAMPLES_H

#include <vector>

#include <gtsam/base/Vector.h>

// Internal to the fusion library: the recording as the numerical stages see
// it, the tuning constants, and every rule that decides whether a recording
// can be fitted. A rule that is violated throws with the text that becomes the
// rejection reason; fusion.cpp is the only place that catches.

namespace FlySight::Fusion::Detail {

/// Three-vectors per sample. The aligned allocator is what GTSAM's fixed-size
/// Eigen types require inside standard containers.
using Vectors = std::vector<gtsam::Vector3, Eigen::aligned_allocator<gtsam::Vector3>>;

/// A recording, or a window of one. Times are seconds since the recording's
/// epoch (its first GNSS fix), strictly increasing. NED frame.
struct Samples {
    std::vector<double> imuTime, gnssTime;
    Vectors force;            ///< specific force, m/s^2, per IMU sample
    Vectors gyro;             ///< angular rate, rad/s, per IMU sample
    /// IMU temperature, degC, per IMU sample. The public boundary always
    /// supplies it; hand-built samples of the stock path (tests) may leave it
    /// empty.
    std::vector<double> temperature;
    Vectors position;         ///< m, per GNSS fix
    Vectors velocity;         ///< m/s, per GNSS fix
    Vectors positionSigma;    ///< m (hAcc, hAcc, vAcc), per GNSS fix
    Vectors velocitySigma;    ///< m/s (sAcc three times), per GNSS fix
};

/// The model's tuning. The defaults are the model; only maxGap is derived
/// from the recording (1.6 median IMU intervals), and only a test changes
/// anything else. The four stopping fields and relativeTolerance may be set
/// to a negative value by a test, which makes the corresponding test
/// impossible to satisfy ("never settles", "never accepted"); production
/// never does. The initializer's prefix fits run with maxIterations and
/// maxPasses replaced by their own budget (initializer.cpp).
constexpr double kPi = 3.14159265358979323846;

struct Tuning {
    double accDensity = .015, gyroDensity = .001;   ///< IMU noise densities
    double accBiasSigma = .3, gyroBiasSigma = .03;  ///< prior on the shared biases
    // The gyro bias of the full fit is b(t) = b0 + b1 (T(t) - T_ref), T the
    // IMU temperature; b0's prior is gyroBiasSigma and b1's is this.
    double gyroBiasSlopeSigma = .010 * kPi / 180;  ///< prior on b1, the gyro bias change per degC of IMU temperature, rad/s/degC (spec: 0.010 deg/s/degC)
    // Per integration step of length dt (s), a white-noise term is added in
    // quadrature to the density: sigma_w = gyroStepSlope x dt x |delta omega|
    // (radians; |delta omega| the norm of the change of the interpolated rate
    // across the step, rad/s) and sigma_a = accStepSlope x dt x |delta f| (m/s;
    // |delta f| the change of the specific force, m/s^2). The step's covariance
    // is (density^2 + sigma^2 x dt) I. Zero disables the term and gives exactly
    // the density covariance.
    double accStepSlope = .40, gyroStepSlope = .026; ///< per-step noise slopes, s
    double maxGap = .025;                           ///< longest IMU interval the fit integrates across, s
    double relativeTolerance = 1e-8;                ///< cost decrease at which a pass has settled
    int maxIterations = 100;                        ///< per bias pass
    double biasSettledTolerance = 1e-6;             ///< a settled pass has converged when re-preintegrating at its bias changes the cost by at most this, relative to max(1, cost)
    int slowTailWindow = 20;                        ///< iterations at the end of a final pass at the limit over which the slow tail is judged
    double slowTailMaxMeanRelativeDecrease = 1e-4;  ///< slow tail: mean (before - after) / max(1, before) over the window must be below this
    double slowTailMaxNrms = 2;                     ///< slow tail: position and velocity normalized RMS must both be below this
    // The segmented initializer (spec section 3.2, step 1): the fitted window
    // is cut into segments of segmentLength, and a final piece shorter than
    // minFinalSegment joins the segment before it. A test may shorten both to
    // keep a multi-segment recording small.
    double segmentLength = 600;                     ///< the initializer cuts the fitted window into segments this long, s
    double minFinalSegment = 120;                   ///< a final piece shorter than this is merged into the segment before it, s
    int maxPasses = 5;                              ///< re-preintegration passes of one fit: the full fit's and a segment fit's five; a prefix fit's one
};

/// Gravity in the NED frame, m/s^2.
extern const gtsam::Vector3 kGravity;

/// An IMU interval above this many median intervals is missing data.
constexpr double kImuGapMedians = 1.6;

/// Throws unless `times` has at least two entries, all finite and strictly
/// increasing. Every later stage searches these arrays by bisection.
void requireIncreasingFiniteTimes(const std::vector<double> &times);

/// The median of the successive differences of `times` (validated first).
double medianInterval(const std::vector<double> &times);

/// Everything the fit assumes about the samples it is given, checked in a
/// fixed order because the first violation is the reported reason.
void validateSamples(const Samples &samples, const Tuning &tuning);

/// The part of `recording` the graph covers: the GNSS fixes in [start, end]
/// that lie inside IMU coverage, and the IMU samples from one before the first
/// kept fix to one past the last. Throws when fewer than three fixes remain.
Samples fittedWindow(const Samples &recording, double start, double end);

/// The checks made on a whole prepared recording before anything indexes it.
void requireUsableRecording(const Samples &recording, double epoch, double usableStart);

/// Throws when two successive fixes of `window` are more than `limit` seconds
/// apart: a disconnected recording is not joined across the outage.
void requireNoGnssOutage(const Samples &window, double limit);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_FUSIONSAMPLES_H
