#include "fusion/initializer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <gtsam/geometry/Pose3.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/navigation/ImuBias.h>

#include "fusion/imuintegration.h"
#include "fusion/stationarywindow.h"

namespace FlySight::Fusion::Detail {

using gtsam::symbol_shorthand::B;
using gtsam::symbol_shorthand::V;
using gtsam::symbol_shorthand::X;

namespace {

// Candidate stationary windows: this long, starting on this grid (seconds
// since the epoch of the recording).
constexpr double kWindowLength = 30;
constexpr double kWindowGrid = 5;

/// The quietest accepted window of the recording; `accepted` is false when
/// there is none. Among equally quiet windows the earliest wins.
StationaryWindow bestStationaryWindow(const Samples &d)
{
    StationaryWindow best;
    const double first = std::max({0., std::ceil(d.imuTime.front()/kWindowGrid)*kWindowGrid});
    const double last = std::min({d.imuTime.back(), d.gnssTime.back()});
    // The start is accumulated, not computed from an index: the window
    // boundaries are part of the numerical behavior.
    for (double s = first; s+kWindowLength <= last+1e-6; s += kWindowGrid) {
        const StationaryWindow c = assessStationaryWindow(d, s, s+kWindowLength);
        if (c.accepted && (!best.accepted || c.score < best.score))
            best = c;
    }
    return best;
}

/// Gravity measured over a stationary window gives roll and pitch, and the
/// mean gyro reading there is the gyro bias. The attitude is anchored at the
/// fix nearest mid-window and carried by the gyro to the start of the graph
/// (usually backwards).
InitialAttitude attitudeFromStationaryWindow(const Samples &d, const StationaryWindow &window,
                                             double graphStart)
{
    InitialAttitude init;
    init.startTime = graphStart;
    const double middle = (window.start+window.end)/2;
    const auto anchor = std::min_element(d.gnssTime.begin(), d.gnssTime.end(),
        [&](double a, double b) { return std::abs(a-middle) < std::abs(b-middle); });
    init.anchorTime = *anchor;
    init.intervalStart = window.start;
    init.intervalEnd = window.end;
    init.gyroBias = window.gyroMean;
    init.rotation = propagateAttitude(d, rotationAligning(window.forceMean, -kGravity),
                                      *anchor, graphStart, init.gyroBias);
    init.method = "stationary initialization only; heading unknown";
    return init;
}

/// Without a stationary window: specific force is acceleration minus gravity,
/// so aligning the measured force at the start with (GNSS acceleration -
/// gravity) gives a usable roll and pitch. The gyro bias starts at zero.
InitialAttitude coarseAttitude(const Samples &d, double graphStart)
{
    InitialAttitude init;
    init.startTime = graphStart;
    const size_t k = std::lower_bound(d.gnssTime.begin(), d.gnssTime.end(), graphStart) - d.gnssTime.begin();
    if (k+1 >= d.gnssTime.size())
        throw std::invalid_argument("No GNSS initializer interval");
    const gtsam::Vector3 a = (d.velocity[k+1]-d.velocity[k])/(d.gnssTime[k+1]-d.gnssTime[k]);
    init.rotation = rotationAligning(interpolateAt(d.imuTime, d.force, graphStart), a-kGravity);
    return init;
}

} // namespace

gtsam::Rot3 rotationAligning(const gtsam::Vector3 &from, const gtsam::Vector3 &to)
{
    if (from.norm() < .1 || to.norm() < .1)
        return gtsam::Rot3();
    gtsam::Vector3 u = from.normalized(), v = to.normalized();
    const double dot = std::clamp(u.dot(v), -1., 1.);
    if (dot < -1 + 1e-10) {
        // Antiparallel: every perpendicular axis is a shortest arc. Turn half
        // way round the one built from the smallest component of `from`.
        Eigen::Index index;
        u.cwiseAbs().minCoeff(&index);
        return gtsam::Rot3::Expmap(kPi*u.cross(gtsam::Vector3::Unit(index)).normalized());
    }
    // Half-angle quaternion (1 + cos, sin * axis), normalized.
    const gtsam::Vector3 c = u.cross(v);
    Eigen::Quaterniond q(1+dot, c.x(), c.y(), c.z());
    return gtsam::Rot3(q.normalized());
}

InitialAttitude initialAttitude(const Samples &fullRecording, double graphStart)
{
    const StationaryWindow best = bestStationaryWindow(fullRecording);
    if (best.accepted)
        return attitudeFromStationaryWindow(fullRecording, best, graphStart);
    return coarseAttitude(fullRecording, graphStart);
}

gtsam::Values initialValues(const Samples &d, double headingDeg, const InitialAttitude &init)
{
    if (std::abs(init.startTime-d.gnssTime.front()) > 1e-9 || !init.gyroBias.allFinite())
        throw std::invalid_argument("Initialization must be propagated to graph start with finite bias");

    // The heading offset is the constant zero, but its composition stays: the
    // model is "initial attitude after a heading rotation".
    gtsam::Rot3 r = gtsam::Rot3::Rz(headingDeg*kPi/180).compose(init.rotation);
    gtsam::Values values;
    values.insert(B(0), gtsam::imuBias::ConstantBias(gtsam::Vector3::Zero(), init.gyroBias));
    for (size_t k = 0; k < d.gnssTime.size(); ++k) {
        if (k) {
            // Carry the attitude across the interval since the previous fix.
            const std::vector<double> e = integrationEdges(d, d.gnssTime[k-1], d.gnssTime[k]);
            for (size_t j = 1; j < e.size(); ++j)
                r = r.compose(gtsam::Rot3::Expmap(gyroIncrement(d, e[j-1], e[j], init.gyroBias)));
        }
        values.insert(X(k), gtsam::Pose3(r, d.position[k]));
        values.insert(V(k), d.velocity[k]);
    }
    return values;
}

} // namespace FlySight::Fusion::Detail
