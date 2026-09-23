#ifndef FLYSIGHTTEST_FUSIONTRACE_H
#define FLYSIGHTTEST_FUSIONTRACE_H

#include <cmath>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include "fusion/fusionpipeline.h"

// The one authority for the golden `trace` object of a success fixture
// (tests/data/fusion/<fixture>.json): fusion_golden_capture writes it with
// traceJson(), and tst_fusion_kernel::fitTraceMatchesGolden reads it back
// through the same function, so the two cannot drift. A change to
// PipelineTrace changes this function and re-captures the goldens
// (tests/README.md, section 11).
//
// Header-only on purpose: PipelineTrace carries gtsam::Rot3 (reached through
// fusion/fusionpipeline.h; no GTSAM header is included here), so this must not
// be compiled into flysight_fusion_test_support, which stays free of GTSAM
// headers. The two targets that include it, tst_fusion_kernel and
// fusion_golden_capture, both link gtsam.

namespace FlySightTest {

/// The trace in the shape the goldens hold it: the initializer's account (the
/// segment length and, per segment, its bounds, anchor, prefix window, yaw
/// sigma, fit counts, flags, start attitude and biases), the full fit's
/// convergence flag and the cost before and after every optimizer iteration
/// as [pass, iteration, before, after] rows. The per-length yaw sigmas are
/// not written: the kernel tests read them from PipelineTrace.
inline QJsonObject traceJson(const FlySight::Fusion::Detail::PipelineTrace &trace)
{
    const auto toJsonArray = [](const gtsam::Vector3 &v) { return QJsonArray{v.x(), v.y(), v.z()}; };
    const auto numberOrNull = [](double value) {
        return std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
    };
    QJsonArray segments;
    for (const FlySight::Fusion::Detail::SegmentAccount &s : trace.initializer.segments) {
        const auto q = s.startRotation.toQuaternion();
        segments.append(QJsonObject{
            {"index", s.index},
            {"start_s", s.start},
            {"end_s", s.end},
            {"anchor_s", s.anchorTime},
            {"anchor_sacc_m_s", s.anchorSacc},
            {"prefix_length_s", s.prefixLength},
            {"prefix_start_s", s.prefixStart},
            {"prefix_end_s", s.prefixEnd},
            {"yaw_sigma_deg", numberOrNull(s.yawSigmaDeg)},
            {"prefix_fits", s.prefixFits},
            {"prefix_iterations", s.prefixIterations},
            {"prefix_passes", s.prefixPasses},
            {"prefix_on_limit", s.prefixOnLimit},
            {"segment_on_limit", s.segmentOnLimit},
            {"growth_stop", QString::fromStdString(s.growthStop)},
            {"iterations", s.iterations},
            {"converged", s.converged},
            {"fallback", s.fallback},
            {"start_quaternion_xyzw", QJsonArray{q.x(), q.y(), q.z(), q.w()}},
            {"start_gyro_bias_rad_s", toJsonArray(s.startGyroBias)},
            {"gyro_bias_rad_s", toJsonArray(s.gyroBias)}});
    }
    QJsonArray history;
    for (const FlySight::Fusion::Detail::FitIteration &h : trace.history)
        history.append(QJsonArray{h.outer, h.iteration, h.before, h.after});
    return {{"initializer", QJsonObject{{"segment_length_s", trace.initializer.segmentLength},
                                        {"segments", segments}}},
            {"converged", trace.converged},
            {"history", history}};
}

} // namespace FlySightTest

#endif // FLYSIGHTTEST_FUSIONTRACE_H
