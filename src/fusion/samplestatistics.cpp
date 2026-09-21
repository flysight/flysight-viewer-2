#include "fusion/samplestatistics.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace FlySight::Fusion::Detail {

double quantile(std::vector<double> values, double q)
{
    if (values.empty())
        throw std::invalid_argument("Empty statistic");
    std::sort(values.begin(), values.end());

    // Fractional rank, then interpolate between the two neighbouring order
    // statistics (the upper one clamped at the maximum).
    const double index = q * (values.size() - 1);
    const size_t i = size_t(index), j = std::min(i + 1, values.size() - 1);
    return values[i] + (values[j] - values[i]) * (index - i);
}

gtsam::Vector3 componentMean(const Vectors &values, bool trimmed)
{
    gtsam::Vector3 result;
    for (int j = 0; j < 3; ++j) {
        std::vector<double> component;
        component.reserve(values.size());
        for (const auto &value : values)
            component.push_back(value[j]);
        if (trimmed)
            std::sort(component.begin(), component.end());

        // Samples dropped at EACH end; zero when not trimming.
        const size_t n = trimmed ? size_t(.1 * component.size()) : 0;
        result[j] = std::accumulate(component.begin() + n, component.end() - n, 0.)
                    / (component.size() - 2*n);
    }
    return result;
}

gtsam::Vector3 componentStddev(const Vectors &values)
{
    const gtsam::Vector3 m = componentMean(values);
    gtsam::Vector3 v = gtsam::Vector3::Zero();
    for (const auto &x : values)
        v += (x-m).cwiseAbs2();
    return (v / double(values.size())).cwiseSqrt();
}

} // namespace FlySight::Fusion::Detail
