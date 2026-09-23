#include "fusion/initializer.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

#include <QString>

#include <gtsam/geometry/Pose3.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/navigation/ImuBias.h>

#include "fusion/factorgraphfit.h"
#include "fusion/imuintegration.h"

namespace FlySight::Fusion::Detail {

using gtsam::symbol_shorthand::B;
using gtsam::symbol_shorthand::V;
using gtsam::symbol_shorthand::X;

namespace {

// A direction needs a vector at least this long (m/s^2: both are accelerations).
constexpr double kMinDirectionNorm = .1;
// cos(angle) within this of -1: the two directions are opposite.
constexpr double kAntiparallelMargin = 1e-10;

// The prefix of a segment (spec section 3.2, steps b-d): the first window is
// this long (s), centred on the anchor, and doubles until the marginal yaw
// sigma of its first pose is at most this many degrees, the window covers the
// segment, or a doubling cut the sigma by less than this fraction.
constexpr double kPrefixLength = 60;
constexpr double kYawSigmaLimitDeg = 20;
constexpr double kGrowthMinGain = .2;
// The heading offsets each prefix window is fitted from, degrees about the vertical.
constexpr double kHeadingOffsetsDeg[] = {0, 90, 180, 270};
// Two objectives within this (relative to max(1, the better one)) are equal,
// and the earlier start wins: an exactly unobservable yaw is then chosen the
// same way on every compiler instead of by rounding noise.
constexpr double kSameObjectiveRelative = 1e-9;
// A prefix fit is a start, not an answer (spec section 3.3, Budgets): one
// pass of at most this many iterations, no re-preintegration.
constexpr int kPrefixIterations = 50;
constexpr int kPrefixPasses = 1;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// A fit whose failure (FitFailure: non-finite or increasing cost) is a start
/// with infinite objective, reported as "no result". Nothing else is caught,
/// deliberately: FusionCancelled is not a std::exception and must reach
/// runPipeline(), and any other exception in a prefix or segment fit (a GTSAM
/// solver exception included) is fatal for the run, which runPipeline()
/// reports as SolverFailed; it is not a start that failed.
std::optional<FitResult> fitOrFail(const Samples &d, const InitialState &initial, const Tuning &tuning,
                                   const QString &passFormat, const Checkpoint &checkpoint)
{
    try {
        return fitFactorGraph(d, initial, tuning, passFormat, checkpoint);
    } catch (const FitFailure &) {
        return std::nullopt;
    }
}

/// Whether `candidate` is lower than `best` by more than rounding.
bool lowerObjective(double candidate, double best)
{
    return candidate < best - kSameObjectiveRelative*std::max(1., best);
}

/// The fix of `d` with the smallest speed accuracy (stored three times in
/// velocitySigma; the x component is read), the earliest on a tie.
size_t anchorFix(const Samples &d)
{
    return size_t(std::min_element(d.velocitySigma.begin(), d.velocitySigma.end(),
        [](const gtsam::Vector3 &a, const gtsam::Vector3 &b) { return a.x() < b.x(); }) - d.velocitySigma.begin());
}

/// The fixes of `segment` inside the window of nominal length `length`
/// centred on `anchorTime` and clipped to the segment: inclusive indices.
std::pair<size_t, size_t> prefixFixes(const Samples &segment, double anchorTime, double length)
{
    const std::vector<double> &t = segment.gnssTime;
    const double lo = std::max(t.front(), anchorTime-length/2);
    const double hi = std::min(t.back(), anchorTime+length/2);
    const size_t p = size_t(std::lower_bound(t.begin(), t.end(), lo)-t.begin());
    const size_t q = size_t(std::upper_bound(t.begin(), t.end(), hi)-t.begin())-1;
    return {p, q};
}

/// The chosen prefix fit's account: what the trace and the diagnostics report
/// about the start the segment fit was given.
void recordPrefix(SegmentAccount &s, const FitResult &best)
{
    s.prefixRotation = best.values.at<gtsam::Pose3>(X(0)).rotation();
    s.prefixGyroBias = best.values.at<gtsam::imuBias::ConstantBias>(B(0)).gyroscope();
    s.prefixIterations = int(best.history.size());
    s.prefixPasses = best.stopping.passes;
    s.prefixOnLimit = best.stopping.rule == StopRule::kIterationLimit;
}

/// The last finite yaw sigma before the newest entry of `sigmas`; NaN when
/// every earlier length failed or this is the first.
double previousSigma(const std::vector<double> &sigmas)
{
    for (size_t i = sigmas.size()-1; i-- > 0;) {
        if (std::isfinite(sigmas[i]))
            return sigmas[i];
    }
    return kNaN;
}

/// Spec section 3.2, steps b-d: the prefix window grown on the marginal yaw
/// sigma, each length fitted from the four heading starts. Returns the best
/// fit of the last length tried, or nothing when every start of that length
/// failed; `s` holds the account either way, and its prefix-fit fields
/// describe the returned fit only (their defaults when there is none).
std::optional<FitResult> growPrefix(const Samples &segment, size_t anchor, const gtsam::Rot3 &coarse,
                                    int index, int count, const Tuning &prefixTuning,
                                    const Checkpoint &checkpoint, SegmentAccount &s)
{
    const size_t last = segment.gnssTime.size()-1;
    const double anchorTime = segment.gnssTime[anchor];
    for (double length = kPrefixLength;; length *= 2) {
        const auto [p, q] = prefixFixes(segment, anchorTime, length);
        const bool covers = p == 0 && q == last;
        // Fewer than three fixes is not a fit; this only happens at a GNSS
        // rate far below 1 Hz, and the window cannot then cover the segment.
        if (q+1-p < 3)
            continue;
        const Samples prefix = fittedWindow(segment, segment.gnssTime[p], segment.gnssTime[q]);
        s.prefixLength = length;
        s.prefixStart = prefix.gnssTime.front();
        s.prefixEnd = prefix.gnssTime.back();

        // The coarse attitude carried from the anchor to the window's first
        // fix with zero bias (backwards; unchanged when the anchor is that fix).
        const gtsam::Rot3 first = propagateAttitude(prefix, coarse, anchorTime, prefix.gnssTime.front(),
                                                    gtsam::Vector3::Zero());
        const QString passFormat = QStringLiteral("Segment %1 of %2: prefix %3 s, pass %4, iteration %5")
            .arg(index+1).arg(count).arg(length);
        std::optional<FitResult> best;
        for (double offsetDeg : kHeadingOffsetsDeg) {
            const InitialState initial{
                attitudesCarriedForward(prefix, gtsam::Rot3::Rz(offsetDeg*kPi/180).compose(first),
                                        gtsam::Vector3::Zero()),
                gtsam::Vector3::Zero()};
            ++s.prefixFits;
            std::optional<FitResult> fit = fitOrFail(prefix, initial, prefixTuning, passFormat, checkpoint);
            // The first start that completed is the best; a later one
            // replaces it only by more than rounding.
            if (fit && (!best || lowerObjective(fit->objective, best->objective)))
                best = std::move(fit);
        }

        if (!best) {
            // All four starts failed: the yaw is not observable here; grow,
            // unless there is nothing left to grow into.
            s.prefixYawSigmaDeg.push_back(kNaN);
            s.yawSigmaDeg = kNaN;
            if (covers) {
                s.growthStop = "all_failed";
                return std::nullopt;
            }
            continue;
        }

        const double sigma = yawSigmaDeg(best->graph, best->values, X(0));
        s.prefixYawSigmaDeg.push_back(sigma);
        s.yawSigmaDeg = sigma;
        // The fit is recorded only where it is returned: a longer window
        // whose starts all fail falls back with no prefix fit, and the
        // account must not then describe this one.
        if (sigma <= kYawSigmaLimitDeg) {
            s.growthStop = "observable";
            recordPrefix(s, *best);
            return best;
        }
        if (covers) {
            s.growthStop = "covers";
            recordPrefix(s, *best);
            return best;
        }
        // A doubling that cut the sigma by less than kGrowthMinGain means the
        // segment has no motion to find; fitting more of it will not help.
        const double previous = previousSigma(s.prefixYawSigmaDeg);
        if (std::isfinite(previous) && sigma > (1-kGrowthMinGain)*previous) {
            s.growthStop = "no_gain";
            recordPrefix(s, *best);
            return best;
        }
    }
}

/// Spec section 3.2, step e, and the fallbacks of section 3.3: one segment's
/// attitudes (one per fix of `segment`, written to `rotations`) and its account.
SegmentAccount initializeSegment(const Samples &segment, int index, int count, const Tuning &tuning,
                                 const Tuning &prefixTuning, const Checkpoint &checkpoint,
                                 std::vector<gtsam::Rot3> &rotations)
{
    SegmentAccount s;
    s.index = index;
    s.start = segment.gnssTime.front();
    s.end = segment.gnssTime.back();
    const size_t anchor = anchorFix(segment);
    s.anchorTime = segment.gnssTime[anchor];
    s.anchorSacc = segment.velocitySigma[anchor].x();
    const gtsam::Rot3 coarse = coarseAttitude(segment, anchor);

    const std::optional<FitResult> prefix = growPrefix(segment, anchor, coarse, index, count,
                                                       prefixTuning, checkpoint, s);
    if (!prefix) {
        // The fallback of spec section 3.3, over this segment only: the
        // coarse attitude at the anchor carried by the gyro with zero bias.
        // The account's prefix-fit fields stay at their defaults: no prefix
        // fit was used (initializer.h).
        s.startRotation = propagateAttitude(segment, coarse, s.anchorTime, s.start, gtsam::Vector3::Zero());
        s.startGyroBias = gtsam::Vector3::Zero();
        rotations = attitudesCarriedForward(segment, s.startRotation, s.startGyroBias);
        s.gyroBias = s.startGyroBias;
        s.fallback = true;
        return s;
    }

    // The whole segment, started from the prefix fit's attitude at its first
    // fix carried backwards to the segment's first fix and forwards to its
    // last with the prefix fit's bias. Always run, even when the prefix
    // covered the segment: one path, and the segment's bias is then the bias
    // of a fit that started at its answer.
    s.startRotation = propagateAttitude(segment, s.prefixRotation, s.prefixStart, s.start, s.prefixGyroBias);
    s.startGyroBias = s.prefixGyroBias;
    const InitialState initial{attitudesCarriedForward(segment, s.startRotation, s.startGyroBias),
                               s.startGyroBias};
    const QString passFormat = QStringLiteral("Segment %1 of %2: pass %3, iteration %4").arg(index+1).arg(count);
    const std::optional<FitResult> fit = fitOrFail(segment, initial, tuning, passFormat, checkpoint);
    if (!fit) {
        // The prefix start is better information than the coarse attitude:
        // the segment's attitudes are the start the fit was given.
        rotations = initial.rotations;
        s.gyroBias = s.startGyroBias;
        s.fallback = true;
        return s;
    }
    rotations.resize(segment.gnssTime.size());
    for (size_t j = 0; j < rotations.size(); ++j)
        rotations[j] = fit->values.at<gtsam::Pose3>(X(j)).rotation();
    s.gyroBias = fit->values.at<gtsam::imuBias::ConstantBias>(B(0)).gyroscope();
    s.iterations = int(fit->history.size());
    s.converged = fit->converged;
    // A segment fit that ended on the limit is still the start (its values
    // are the best available): converged false with fallback false.
    s.segmentOnLimit = fit->stopping.rule == StopRule::kIterationLimit;
    s.fallback = false;
    return s;
}

} // namespace

gtsam::Rot3 rotationAligning(const gtsam::Vector3 &from, const gtsam::Vector3 &to)
{
    if (from.norm() < kMinDirectionNorm || to.norm() < kMinDirectionNorm)
        return gtsam::Rot3();
    gtsam::Vector3 u = from.normalized(), v = to.normalized();
    const double dot = std::clamp(u.dot(v), -1., 1.);
    if (dot < -1 + kAntiparallelMargin) {
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

std::vector<std::pair<size_t, size_t>> segmentBounds(const std::vector<double> &gnssTime,
                                                     double segmentLength, double minFinalSegment)
{
    // Fix i belongs to the piece whose half-open interval [t0 + k L, t0 + (k+1) L)
    // contains it; the fixes are walked once and k advances with them.
    std::vector<std::pair<size_t, size_t>> pieces;
    const double t0 = gnssTime.front();
    size_t k = 0;
    for (size_t i = 0; i < gnssTime.size(); ++i) {
        bool newPiece = pieces.empty();
        while (gnssTime[i] >= t0+double(k+1)*segmentLength) {
            ++k;
            newPiece = true;
        }
        if (newPiece)
            pieces.push_back({i, i});
        else
            pieces.back().second = i;
    }
    // The final piece joins the one before it when it is too short to be a
    // segment in time or in fixes (a segment must have three fixes).
    if (pieces.size() > 1) {
        const auto [first, last] = pieces.back();
        if (gnssTime[last]-gnssTime[first] < minFinalSegment || last+1-first < 3) {
            pieces.pop_back();
            pieces.back().second = last;
        }
    }
    return pieces;
}

gtsam::Rot3 coarseAttitude(const Samples &d, size_t k)
{
    // The forward difference to the next fix, or at the last fix the backward
    // one: a segment has at least three fixes, so one of the two exists.
    const bool forward = k+1 < d.gnssTime.size();
    const size_t i = forward ? k : k-1, j = forward ? k+1 : k;
    const gtsam::Vector3 a = (d.velocity[j]-d.velocity[i])/(d.gnssTime[j]-d.gnssTime[i]);
    return rotationAligning(interpolateAt(d.imuTime, d.force, d.gnssTime[k]), a-kGravity);
}

std::vector<gtsam::Rot3> attitudesCarriedForward(const Samples &d, const gtsam::Rot3 &first,
                                                 const gtsam::Vector3 &gyroBias)
{
    std::vector<gtsam::Rot3> rotations;
    rotations.reserve(d.gnssTime.size());
    gtsam::Rot3 r = first;
    for (size_t k = 0; k < d.gnssTime.size(); ++k) {
        if (k) {
            // Carry the attitude across the interval since the previous fix.
            const std::vector<double> e = integrationEdges(d, d.gnssTime[k-1], d.gnssTime[k]);
            for (size_t j = 1; j < e.size(); ++j)
                r = r.compose(gtsam::Rot3::Expmap(gyroIncrement(d, e[j-1], e[j], gyroBias)));
        }
        rotations.push_back(r);
    }
    return rotations;
}

Initialization initialize(const Samples &window, const Tuning &tuning, const Checkpoint &checkpoint)
{
    // The prefix budget: one pass of at most kPrefixIterations, and never
    // more than the tuning allows, so a test that forces the limit forces
    // the prefix fits too.
    Tuning prefixTuning = tuning;
    prefixTuning.maxIterations = std::min(tuning.maxIterations, kPrefixIterations);
    prefixTuning.maxPasses = std::min(tuning.maxPasses, kPrefixPasses);

    Initialization init;
    init.account.segmentLength = tuning.segmentLength;
    init.state.rotations.resize(window.gnssTime.size());
    const std::vector<std::pair<size_t, size_t>> bounds =
        segmentBounds(window.gnssTime, tuning.segmentLength, tuning.minFinalSegment);
    const int count = int(bounds.size());
    for (int i = 0; i < count; ++i) {
        const auto [a, b] = bounds[i];
        // The segment's fix j is the window's fix a + j; every fix of the
        // window is inside IMU coverage, so the cut keeps exactly these.
        const Samples segment = fittedWindow(window, window.gnssTime[a], window.gnssTime[b]);
        if (segment.gnssTime.size() != b-a+1)
            throw std::logic_error("Segment cut does not hold its fixes");
        std::vector<gtsam::Rot3> rotations;
        SegmentAccount s = initializeSegment(segment, i, count, tuning, prefixTuning, checkpoint, rotations);
        s.firstFix = a;
        s.lastFix = b;
        std::copy(rotations.begin(), rotations.end(), init.state.rotations.begin()+std::ptrdiff_t(a));
        init.account.segments.push_back(std::move(s));
    }
    init.state.gyroBias = init.account.segments.front().gyroBias;
    return init;
}

gtsam::Values initialValues(const Samples &d, const InitialState &initial)
{
    if (initial.rotations.size() != d.gnssTime.size() || !initial.gyroBias.allFinite())
        throw std::invalid_argument("Initial state must have one attitude per fix and a finite bias");

    gtsam::Values values;
    values.insert(B(0), gtsam::imuBias::ConstantBias(gtsam::Vector3::Zero(), initial.gyroBias));
    for (size_t k = 0; k < d.gnssTime.size(); ++k) {
        values.insert(X(k), gtsam::Pose3(initial.rotations[k], d.position[k]));
        values.insert(V(k), d.velocity[k]);
    }
    return values;
}

} // namespace FlySight::Fusion::Detail
