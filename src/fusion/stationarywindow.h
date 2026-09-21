#ifndef FLYSIGHT_FUSION_STATIONARYWINDOW_H
#define FLYSIGHT_FUSION_STATIONARYWINDOW_H

#include <string>
#include <vector>

#include "fusion/fusionsamples.h"

// Internal to the fusion library: the test that decides whether the sensor
// was at rest (or translating steadily) over a time window, which is what
// lets gravity alone fix roll and pitch and the mean gyro reading be taken as
// the gyro bias.

namespace FlySight::Fusion::Detail {

/// The verdict on one candidate window [start, end).
struct StationaryWindow {
    bool accepted = false;
    double start = 0, end = 0;
    double score = 0;                       ///< lower is quieter (norm of the force std)
    size_t imuCount = 0, gnssCount = 0;     ///< samples inside the window
    gtsam::Vector3 forceMean = gtsam::Vector3::Zero();   ///< trimmed mean specific force, m/s^2
    gtsam::Vector3 gyroMean = gtsam::Vector3::Zero();    ///< trimmed mean rate, rad/s
    std::vector<std::string> rejected;      ///< names of the gates that failed, in gate order
};

/// Assesses the samples of `samples` with start <= t < end. Every gate is
/// evaluated (not only the first that fails), so `rejected` says everything
/// that is wrong with the window; only too little data ends the test early.
/// The samples are found by bisection: both time axes must be strictly
/// increasing (requireIncreasingFiniteTimes).
///
/// `imuGap` is the longest IMU interval that is not missing data,
/// imuGapLimit(samples). It belongs to the recording, not to the window, so a
/// caller that assesses many windows computes it once.
StationaryWindow assessStationaryWindow(const Samples &samples, double start, double end,
                                        double imuGap);

/// The same, for a single window.
StationaryWindow assessStationaryWindow(const Samples &samples, double start, double end);

/// kImuGapMedians median IMU intervals of `samples`, in seconds. Sorts every
/// IMU interval of the recording.
double imuGapLimit(const Samples &samples);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_STATIONARYWINDOW_H
