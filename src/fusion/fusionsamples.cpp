#include "fusion/fusionsamples.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <utility>

#include <QString>

#include "fusion/samplestatistics.h"

namespace FlySight::Fusion::Detail {

const gtsam::Vector3 kGravity(0, 0, 9.80665);

namespace {

// Rejection reasons that more than one check reports. The whole recording
// and the fitted window are checked for the same defects in different words;
// the words are part of the reference's behavior and stay as they are.
const char kWindowLengthsMessage[] = "Mismatched input array lengths";
const char kWindowNonfiniteMessage[] = "Nonfinite input";
const char kRecordingLengthsMessage[] = "Mismatched fusion input array lengths";
const char kRecordingNonfiniteMessage[] = "Nonfinite fusion input";

/// Each field must have `count` entries (checked first) and only finite ones.
void requireFiniteArrays(size_t count, std::initializer_list<const Vectors *> fields,
                         const char *lengthMessage, const char *finiteMessage)
{
    for (const Vectors *field : fields) {
        if (field->size() != count)
            throw std::invalid_argument(lengthMessage);
        for (const gtsam::Vector3 &value : *field) {
            if (!value.allFinite())
                throw std::invalid_argument(finiteMessage);
        }
    }
}

/// A GNSS sigma weights a factor by its inverse: zero or negative is meaningless.
void requirePositiveSigmas(const Samples &samples)
{
    for (const Vectors *field : { &samples.positionSigma, &samples.velocitySigma }) {
        for (const gtsam::Vector3 &sigma : *field) {
            if (sigma.minCoeff() <= 0)
                throw std::invalid_argument("GNSS sigmas must be positive");
        }
    }
}

void requireValidTuning(const Tuning &tuning)
{
    for (double value : { tuning.accDensity, tuning.gyroDensity, tuning.accBiasSigma,
                          tuning.gyroBiasSigma, tuning.maxGap, tuning.segmentLength,
                          tuning.minFinalSegment }) {
        if (!std::isfinite(value) || value <= 0)
            throw std::invalid_argument("Invalid fusion configuration");
    }
    // The per-step slopes may be zero (the term is then exactly absent) but
    // not negative or non-finite.
    for (double value : { tuning.accStepSlope, tuning.gyroStepSlope }) {
        if (!std::isfinite(value) || value < 0)
            throw std::invalid_argument("Invalid fusion configuration");
    }
    // The tolerances and bounds need only be finite: a negative value is a
    // legal "never" forcing for a test (the settle test, the cost test and the
    // slow-tail bounds are then never satisfied), and it is the only way to
    // force a pass to run to maxIterations, since a rejected LM step is an
    // exact no-op that a zero tolerance would count as settled.
    for (double value : { tuning.relativeTolerance, tuning.biasSettledTolerance,
                          tuning.slowTailMaxMeanRelativeDecrease, tuning.slowTailMaxNrms }) {
        if (!std::isfinite(value))
            throw std::invalid_argument("Invalid fusion configuration");
    }
    if (tuning.maxIterations < 1 || tuning.maxPasses < 1 || tuning.slowTailWindow < 1)
        throw std::invalid_argument("Invalid iteration limit");
}

/// Every IMU factor integrates between two fixes, so every fix must lie inside
/// IMU coverage. The caller trims (fittedWindow); this does not.
void requireGnssInsideImuCoverage(const Samples &samples)
{
    if (samples.gnssTime.front() < samples.imuTime.front()
        || samples.gnssTime.back() > samples.imuTime.back())
        throw std::invalid_argument("GNSS extends beyond IMU coverage; trim explicitly");
}

/// GNSS outside IMU coverage can be trimmed away, but a hole in the IMU data
/// between two fixes cannot: integrating across it would invent motion. A gap
/// that touches the GNSS span is therefore fatal.
void requireNoImuGapInsideGnssSpan(const Samples &samples, double maxGap)
{
    const std::vector<double> &imuTime = samples.imuTime;
    for (size_t i = 1; i < imuTime.size(); ++i) {
        if (imuTime[i] - imuTime[i - 1] > maxGap
            && imuTime[i - 1] < samples.gnssTime.back()
            && imuTime[i] > samples.gnssTime.front()) {
            // QString::number, not std::to_string: same digits, but not
            // subject to the C locale Qt installs on Unix.
            const QString message = QStringLiteral("IMU gap at ")
                + QString::number(imuTime[i - 1], 'f', 6)
                + QStringLiteral(" s; fusion unavailable across missing data");
            throw std::invalid_argument(message.toStdString());
        }
    }
}

/// The GNSS part of a window: fixes with start <= t <= end that IMU data covers.
Samples gnssInsideWindowAndCoverage(const Samples &recording, double start, double end)
{
    Samples window;
    for (size_t k = 0; k < recording.gnssTime.size(); ++k) {
        const double t = recording.gnssTime[k];
        if (t >= start && t <= end
            && t >= recording.imuTime.front() && t <= recording.imuTime.back()) {
            window.gnssTime.push_back(t);
            window.position.push_back(recording.position[k]);
            window.velocity.push_back(recording.velocity[k]);
            window.positionSigma.push_back(recording.positionSigma[k]);
            window.velocitySigma.push_back(recording.velocitySigma[k]);
        }
    }
    return window;
}

/// Index range [first, second) of the IMU samples needed to integrate from
/// `firstFix` to `lastFix`: one sample before the first fix (so it can be
/// interpolated) to one past the last, clamped to the recording.
std::pair<size_t, size_t> imuRangeCovering(const std::vector<double> &imuTime,
                                           double firstFix, double lastFix)
{
    size_t lo = std::lower_bound(imuTime.begin(), imuTime.end(), firstFix) - imuTime.begin();
    if (lo)
        --lo;
    const size_t hi = std::min(imuTime.size(),
        size_t(std::lower_bound(imuTime.begin(), imuTime.end(), lastFix) - imuTime.begin() + 1));
    return { lo, hi };
}

} // namespace

void requireIncreasingFiniteTimes(const std::vector<double> &times)
{
    if (times.size() < 2)
        throw std::invalid_argument("At least two timestamps required");
    for (size_t i = 0; i < times.size(); ++i) {
        if (!std::isfinite(times[i]) || (i && times[i] <= times[i - 1]))
            throw std::invalid_argument("Timestamps must be finite and strictly increasing");
    }
}

double medianInterval(const std::vector<double> &times)
{
    requireIncreasingFiniteTimes(times);
    std::vector<double> intervals;
    intervals.reserve(times.size() - 1);
    for (size_t i = 1; i < times.size(); ++i)
        intervals.push_back(times[i] - times[i - 1]);
    return quantile(intervals, .5);
}

void validateSamples(const Samples &samples, const Tuning &tuning)
{
    requireIncreasingFiniteTimes(samples.imuTime);
    requireIncreasingFiniteTimes(samples.gnssTime);
    requireFiniteArrays(samples.imuTime.size(), { &samples.force, &samples.gyro },
                        kWindowLengthsMessage, kWindowNonfiniteMessage);
    requireFiniteArrays(samples.gnssTime.size(),
                        { &samples.position, &samples.velocity,
                          &samples.positionSigma, &samples.velocitySigma },
                        kWindowLengthsMessage, kWindowNonfiniteMessage);
    requirePositiveSigmas(samples);
    requireValidTuning(tuning);
    requireGnssInsideImuCoverage(samples);
    requireNoImuGapInsideGnssSpan(samples, tuning.maxGap);
}

Samples fittedWindow(const Samples &recording, double start, double end)
{
    Samples window = gnssInsideWindowAndCoverage(recording, start, end);
    if (window.gnssTime.size() < 3)
        throw std::invalid_argument("Fewer than three GNSS fixes in IMU coverage");

    const auto [lo, hi] = imuRangeCovering(recording.imuTime,
                                           window.gnssTime.front(), window.gnssTime.back());
    window.imuTime.assign(recording.imuTime.begin() + lo, recording.imuTime.begin() + hi);
    window.force.assign(recording.force.begin() + lo, recording.force.begin() + hi);
    window.gyro.assign(recording.gyro.begin() + lo, recording.gyro.begin() + hi);
    return window;
}

void requireUsableRecording(const Samples &recording, double epoch, double usableStart)
{
    // The input adapter enforces this contract too, but the pipeline must not
    // depend on that: fittedWindow() indexes these arrays and the initializer
    // scans samples outside the fitted interval.
    if (recording.gnssTime.size() < 3 || recording.imuTime.size() < 2)
        throw std::invalid_argument(
            "Sensor fusion needs at least three GNSS fixes and two IMU samples");
    if (!std::isfinite(epoch) || !std::isfinite(usableStart))
        throw std::invalid_argument("Nonfinite fusion epoch or usable start");
    requireFiniteArrays(recording.imuTime.size(), { &recording.force, &recording.gyro },
                        kRecordingLengthsMessage, kRecordingNonfiniteMessage);
    requireFiniteArrays(recording.gnssTime.size(),
                        { &recording.position, &recording.velocity,
                          &recording.positionSigma, &recording.velocitySigma },
                        kRecordingLengthsMessage, kRecordingNonfiniteMessage);
    requirePositiveSigmas(recording);
}

void requireNoGnssOutage(const Samples &window, double limit)
{
    // A large GNSS outage makes fusion unavailable even if the IMU happened to
    // continue: segments are never joined.
    for (size_t k = 1; k < window.gnssTime.size(); ++k) {
        if (window.gnssTime[k] - window.gnssTime[k - 1] > limit)
            throw std::invalid_argument("GNSS gap: fusion unavailable for a disconnected session");
    }
}

} // namespace FlySight::Fusion::Detail
