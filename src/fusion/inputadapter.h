#ifndef FLYSIGHT_FUSION_INPUTADAPTER_H
#define FLYSIGHT_FUSION_INPUTADAPTER_H

#include <QJsonObject>

#include "fusion/fusion.h"
#include "fusion/fusionsamples.h"

// Internal to the fusion library: the boundary between the plain channels
// of the public API and the samples the numerical stages work on.

namespace FlySight::Fusion::Detail {

/// A recording ready for the pipeline.
struct PreparedInput {
    Samples recording;        ///< the whole recording; times relative to `epoch`, gyro in rad/s
    double epoch = 0;         ///< UTC s of the first GNSS fix; output time = epoch + sample time
    double usableStart = 0;   ///< s since the epoch of the local-origin fix: where the fit may begin
    QJsonObject audit;        ///< what was read, for the diagnostics
};

/// Validates `channels` and converts them. Subtracting a common epoch keeps
/// the full precision of the timestamps in the arithmetic that follows (UTC
/// seconds leave a double only about 0.2 microseconds of resolution).
/// Throws with the rejection reason; the order of the checks decides which
/// reason a recording with several defects reports.
PreparedInput prepareInput(const Channels &channels);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_INPUTADAPTER_H
