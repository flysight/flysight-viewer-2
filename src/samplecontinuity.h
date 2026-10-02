#ifndef FLYSIGHT_SAMPLECONTINUITY_H
#define FLYSIGHT_SAMPLECONTINUITY_H

#include <cstddef>
#include <vector>

// The one authority on where a sensor's samples are continuous: when the
// interval between two successive samples is a hole, across which nothing is
// drawn, read, interpolated or differenced. Every reader asks it (the plots,
// the point reads, the map, the interpolation family, the derivative helper)
// and so does the sensor fusion kernel for its IMU gap rule, which is why it
// lives in flysight_model, which the kernel and the core both link.
//
// A sensor's time axis is its _time column as the session holds it. Its
// nominal interval is the median of the successive differences of the whole
// axis: the order statistic at fractional rank (m - 1) / 2 of the m
// differences, or the mean of the two around it when that rank falls halfway
// between them. An interval between two successive samples is a hole when it
// is strictly greater than 1.5 times the nominal interval. 1.5 is halfway
// between the nominal interval and the shortest interval that can hold a
// missing sample. The two samples around a hole are ordinary samples: a hole
// is the interval between them, nothing more.
//
// An axis with fewer than three samples has no nominal interval and no holes.
// So has an axis that is not finite and strictly increasing throughout, which
// a session does not exclude: on it every reader behaves as if the recording
// were continuous. Nothing here throws or logs; the kernel validates its axes
// before it asks.
//
// The nominal interval is a selection over the whole axis. A caller that
// judges many intervals of one axis (a graph build, a derivative, a cursor
// event that reads several series of one sensor) asks for the threshold once
// and passes it to isHoleBefore(). Nothing is stored: holes are recomputed
// from the samples when they are needed.
//
// Pure: the standard library only, no state. Safe to use from any thread.

namespace FlySight {
namespace SampleContinuity {

/// A time axis as contiguous seconds, read in place: a QVector<double> or a
/// std::vector<double> converts to it without a copy.
struct TimeAxis {
    const double *times = nullptr;
    std::size_t count = 0;

    template <class Container>
    TimeAxis(const Container &container)
        : times(container.data()), count(std::size_t(container.size())) {}
};

/// A run of connected samples: the half-open index range [begin, end). The
/// runs of an axis partition its samples, in order; two successive runs meet
/// at a hole.
struct Run {
    std::size_t begin = 0;
    std::size_t end = 0;
};

/// The nominal interval of `axis`, in seconds; NaN when it has none (fewer
/// than three samples, or not finite and strictly increasing).
double nominalInterval(TimeAxis axis);

/// The longest interval of `axis` that is not a hole, in seconds: 1.5 times
/// its nominal interval; NaN when it has none.
double holeThreshold(TimeAxis axis);

/// True when the interval between samples `i - 1` and `i` of `axis` is a hole
/// against `threshold` (holeThreshold() of the axis): strictly greater. Never
/// true against a NaN threshold; false for i == 0 and for i past the end.
bool isHoleBefore(TimeAxis axis, std::size_t i, double threshold);

/// The runs of connected samples of `axis`, in order; one run of every sample
/// for an axis without holes, none for an empty axis.
std::vector<Run> runs(TimeAxis axis);

} // namespace SampleContinuity
} // namespace FlySight

#endif // FLYSIGHT_SAMPLECONTINUITY_H
