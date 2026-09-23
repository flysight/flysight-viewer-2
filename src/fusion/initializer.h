#ifndef FLYSIGHT_FUSION_INITIALIZER_H
#define FLYSIGHT_FUSION_INITIALIZER_H

#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtsam/geometry/Rot3.h>
#include <gtsam/nonlinear/Values.h>

#include "fusion/fusionprogress.h"
#include "fusion/fusionsamples.h"

// Internal to the fusion library: where the optimizer starts. The fitted
// window is cut into segments; in each, the coarse attitude at the fix with
// the smallest speed accuracy (measured force aligned with GNSS acceleration
// minus gravity) seeds prefix fits from four heading offsets, the prefix grows
// until the yaw of its first pose is observable, and one fit of the whole
// segment gives every fix of it an attitude near its answer. The full fit
// starts from those attitudes, the first segment's gyro bias and a zero
// accelerometer bias. No attitude is propagated further than a segment.

namespace FlySight::Fusion::Detail {

/// Where the optimizer starts: one attitude per fix of the window it is for
/// (body to NED), and the gyro bias. The accelerometer bias always starts
/// at zero; positions and velocities are the GNSS measurements.
struct InitialState {
    std::vector<gtsam::Rot3> rotations;
    gtsam::Vector3 gyroBias = gtsam::Vector3::Zero();   ///< rad/s
};

/// What the initializer did in one segment. Times are seconds since the epoch.
struct SegmentAccount {
    int index = 0;                             ///< zero-based
    size_t firstFix = 0, lastFix = 0;          ///< inclusive indices into the fitted window
    double start = 0, end = 0;                 ///< the segment's first and last fix
    double anchorTime = 0, anchorSacc = 0;     ///< the smallest-sAcc fix and its sAcc, m/s
    double prefixLength = 0;                   ///< the nominal prefix length reached, s (60, 120, ...)
    double prefixStart = 0, prefixEnd = 0;     ///< the last prefix window's first and last fix
    std::vector<double> prefixYawSigmaDeg;     ///< the chosen start's yaw sigma per length tried; NaN when every start failed
    double yawSigmaDeg = std::numeric_limits<double>::quiet_NaN();   ///< the last entry; NaN when the segment fell back with no prefix fit
    int prefixFits = 0;                        ///< prefix fits attempted, all lengths and starts, failed ones included
    int prefixIterations = 0;                  ///< the chosen prefix fit's iterations (0 without one)
    int prefixPasses = 0;                      ///< its passes (1 by construction; 0 without one)
    bool prefixOnLimit = false;                ///< the chosen prefix fit ended on its budget (used anyway)
    bool segmentOnLimit = false;               ///< the segment fit ended on the iteration limit (used anyway)
    std::string growthStop;                    ///< why the prefix stopped growing: observable, covers, no_gain, all_failed
    int iterations = 0;                        ///< the segment fit's; 0 for a fallback
    bool converged = false;                    ///< the segment fit's; false for a fallback
    bool fallback = false;                     ///< the segment's attitudes are its start state, not a fit
    gtsam::Rot3 prefixRotation;                ///< the best prefix fit's attitude at prefixStart (identity without one)
    gtsam::Vector3 prefixGyroBias = gtsam::Vector3::Zero();   ///< its fitted gyro bias (zero without one)
    gtsam::Rot3 startRotation;                 ///< the segment fit's start attitude at `start`
    gtsam::Vector3 startGyroBias = gtsam::Vector3::Zero();    ///< the segment fit's start gyro bias
    gtsam::Vector3 gyroBias = gtsam::Vector3::Zero();         ///< the segment's fitted gyro bias (startGyroBias for a fallback)
};

struct InitializerAccount {
    double segmentLength = 0;                  ///< Tuning::segmentLength
    std::vector<SegmentAccount> segments;
};

/// The initializer's result: the full fit's start and the account of how it was chosen.
struct Initialization {
    InitialState state;
    InitializerAccount account;
};

/// The rotation that takes direction `from` to direction `to` along the
/// shortest arc. Identity when either vector is too short to have a direction.
gtsam::Rot3 rotationAligning(const gtsam::Vector3 &from, const gtsam::Vector3 &to);

/// The segments of a window whose fixes are at `gnssTime`, as inclusive index
/// pairs: consecutive pieces of `segmentLength` from the first fix, the final
/// piece merged into the one before it when it is shorter than
/// `minFinalSegment` or holds fewer than three fixes. Every fix is in exactly
/// one piece; a window shorter than one segment is one piece.
std::vector<std::pair<size_t, size_t>> segmentBounds(const std::vector<double> &gnssTime,
                                                     double segmentLength, double minFinalSegment);

/// The coarse attitude at fix `k` of `d`: specific force is acceleration
/// minus gravity, so aligning the measured force at the fix with (GNSS
/// acceleration - gravity) gives a usable roll and pitch and no yaw. The GNSS
/// acceleration is the forward difference to the next fix, or the backward
/// one at the last fix. Gyro bias zero is implied.
gtsam::Rot3 coarseAttitude(const Samples &d, size_t k);

/// One attitude per fix of `d`: `first` at the first fix, then carried across
/// each interval by the gyro corrected by `gyroBias`.
std::vector<gtsam::Rot3> attitudesCarriedForward(const Samples &d, const gtsam::Rot3 &first,
                                                 const gtsam::Vector3 &gyroBias);

/// The segmented initializer on the validated fitted window (spec section 3).
/// Its prefix and segment fits report through `checkpoint` like the full fit,
/// with texts naming the segment, and throw FusionCancelled when asked to
/// stop; a fit that fails is a start with infinite objective, never an error.
Initialization initialize(const Samples &window, const Tuning &tuning,
                          const Checkpoint &checkpoint = Checkpoint());

/// The optimizer's initial values for `window`: the shared bias B(0) with a
/// zero accelerometer part, and per fix the pose X(k) (the given attitude,
/// the measured position) and the measured velocity V(k). Throws unless
/// `initial` has one attitude per fix and a finite bias.
gtsam::Values initialValues(const Samples &window, const InitialState &initial);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_INITIALIZER_H
