#ifndef FLYSIGHT_FUSION_FUSIONSAMPLES_H
#define FLYSIGHT_FUSION_FUSIONSAMPLES_H

#include <vector>

#include <gtsam/base/Vector.h>

#include "fusion/fusion.h"
#include "fusion/sensornoise.h"

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

/// The model's tuning. The defaults are the model; only maxGap and noise are
/// derived from the recording (the hole threshold of its IMU axis,
/// samplecontinuity.h; the datasheet's noise at the recording's
/// configuration), and only a test changes anything else.
/// The four stopping fields and relativeTolerance may be set to a negative
/// value by a test, which makes the corresponding test impossible to satisfy
/// ("never settles", "never accepted"); production never does. The
/// initializer's prefix fits cap maxIterations and maxPasses at their own
/// budget (initializer.cpp): the smaller of the tuning's limit and 50
/// iterations, and of the tuning's limit and 1 pass; they keep the noise.
struct Tuning {
    /// The IMU's noise: derived from the recording's configuration by
    /// planFit(), as maxGap is; NaN until then (preintegrateImu() refuses it).
    ImuNoise noise;
    // The bias priors cover the datasheet's typical offsets (Table 2):
    // LA_TyOff +/-20 mg (0.2 m/s^2) and G_TyOff +/-1 deg/s (0.017 rad/s).
    double accBiasSigma = .3, gyroBiasSigma = .03;  ///< prior on the shared biases
    // The gyro bias of the full fit is b(t) = b0 + b1 (T(t) - T_ref), T the
    // IMU temperature; b0's prior is gyroBiasSigma and b1's is this.
    double gyroBiasSlopeSigma = .010 * kPi / 180;  ///< prior on b1, the gyro bias change per degC of IMU temperature, rad/s/degC (Table 2, G_OffDr: 0.010 deg/s/degC)
    double maxGap = .025;                           ///< longest IMU interval the fit integrates across, s: planFit() sets it to the hole threshold of the recording's IMU axis (SampleContinuity::holeThreshold)
    // The ceiling of Levenberg-Marquardt's damping. GTSAM's default, 1e5, is
    // below the damping a resting recording needs under the datasheet's
    // densities (the IMU blocks of the Hessian are about 1e9): every iteration
    // at that ceiling returns the same values. Only a test changes it.
    double lambdaUpperBound = 1e12;
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

/// Throws unless `times` has at least two entries, all finite and strictly
/// increasing. Every later stage searches these arrays by bisection.
void requireIncreasingFiniteTimes(const std::vector<double> &times);

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

/// Throws unless every reading of `channels` (all of them, inside the fitted
/// window or not, as the kernel received them) shows the stated range of its
/// sensor: the coarsest range whose lattice the three axes fit
/// (rangeShownByReadings) must be the configured one. The accelerometer is
/// checked first. The configuration must have a datasheet entry.
void requireReadingsOnLattice(const Channels &channels);

/// Throws unless the recording's median IMU interval (the nominal interval of
/// its IMU axis, SampleContinuity::nominalInterval) is within 10 % of the
/// interval, 1 / rate, of each stated rate: ACCEL_ODR_HZ first, then
/// GYRO_ODR_HZ.
void requireStatedRates(double medianImuInterval, const ImuConfiguration &configuration);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_FUSIONSAMPLES_H
