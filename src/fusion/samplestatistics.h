#ifndef FLYSIGHT_FUSION_SAMPLESTATISTICS_H
#define FLYSIGHT_FUSION_SAMPLESTATISTICS_H

#include <vector>

#include "fusion/fusionsamples.h"

// Internal to the fusion library: the few statistics the validation and the
// stationary-window test are built from. Summation order is part of the
// model's numerical behavior; do not replace these by library reductions.

namespace FlySight::Fusion::Detail {

/// The q-quantile (0 <= q <= 1) of `values` by linear interpolation between
/// order statistics. Takes a copy because it sorts. Throws when empty.
double quantile(std::vector<double> values, double q);

/// Per-component mean. When `trimmed`, each component is sorted and the
/// lowest and highest tenth (rounded down) are left out, which makes the mean
/// robust against a few outliers.
gtsam::Vector3 componentMean(const Vectors &values, bool trimmed = false);

/// Per-component population standard deviation about the untrimmed mean.
gtsam::Vector3 componentStddev(const Vectors &values);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_SAMPLESTATISTICS_H
