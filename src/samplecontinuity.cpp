#include "samplecontinuity.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace FlySight {
namespace SampleContinuity {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// The longest interval that is not a hole, in nominal intervals.
constexpr double kHoleFactor = 1.5;

bool finiteAndStrictlyIncreasing(TimeAxis axis)
{
    for (std::size_t i = 0; i < axis.count; ++i) {
        if (!std::isfinite(axis.times[i]) || (i && axis.times[i] <= axis.times[i - 1]))
            return false;
    }
    return true;
}

} // namespace

double nominalInterval(TimeAxis axis)
{
    if (axis.count < 3 || !finiteAndStrictlyIncreasing(axis))
        return kNaN;

    std::vector<double> intervals;
    intervals.reserve(axis.count - 1);
    for (std::size_t i = 1; i < axis.count; ++i)
        intervals.push_back(axis.times[i] - axis.times[i - 1]);

    // The median as the order statistics at fractional rank (m - 1) / 2,
    // interpolated: the same bits as sorting and reading the rank, which is
    // what the fusion kernel's thresholds were captured with. Selecting the
    // lower order statistic leaves every larger difference after it, so the
    // upper one is the least of those.
    const double index = .5 * (intervals.size() - 1);
    const std::size_t i = std::size_t(index), j = std::min(i + 1, intervals.size() - 1);
    std::nth_element(intervals.begin(), intervals.begin() + i, intervals.end());
    const double lower = intervals[i];
    const double upper = j == i ? lower : *std::min_element(intervals.begin() + j, intervals.end());
    return lower + (upper - lower) * (index - i);
}

double holeThreshold(TimeAxis axis)
{
    return kHoleFactor * nominalInterval(axis);
}

bool isHole(double interval, double threshold)
{
    return interval > threshold;
}

bool isHoleBefore(TimeAxis axis, std::size_t i, double threshold)
{
    if (i == 0 || i >= axis.count)
        return false;
    return isHole(axis.times[i] - axis.times[i - 1], threshold);
}

std::vector<Run> runs(TimeAxis axis)
{
    std::vector<Run> result;
    if (axis.count == 0)
        return result;

    const double threshold = holeThreshold(axis);
    std::size_t begin = 0;
    for (std::size_t i = 1; i < axis.count; ++i) {
        if (isHoleBefore(axis, i, threshold)) {
            result.push_back({begin, i});
            begin = i;
        }
    }
    result.push_back({begin, axis.count});
    return result;
}

} // namespace SampleContinuity
} // namespace FlySight
