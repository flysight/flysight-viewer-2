#ifndef FLYSIGHTTEST_FUSIONTRACE_H
#define FLYSIGHTTEST_FUSIONTRACE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "fusion/fusionpipeline.h"

// The one authority for the golden `trace` object of a success fixture
// (tests/data/fusion/<fixture>.json): fusion_golden_capture writes it with
// traceJson(), and tst_fusion_kernel::fitTraceMatchesGolden reads it back
// through the same function, so the two cannot drift. A later phase that
// changes PipelineTrace changes this function and re-captures the goldens
// (tests/README.md, section 11).
//
// Header-only on purpose: PipelineTrace carries gtsam::Rot3 (reached through
// fusion/fusionpipeline.h; no GTSAM header is included here), so this must not
// be compiled into flysight_fusion_test_support, which stays free of GTSAM
// headers. The two targets that include it, tst_fusion_kernel and
// fusion_golden_capture, both link gtsam.

namespace FlySightTest {

/// The trace in the shape the goldens hold it: the initializer's result, the
/// convergence flag and the cost before and after every optimizer iteration
/// as [pass, iteration, before, after] rows.
inline QJsonObject traceJson(const FlySight::Fusion::Detail::PipelineTrace &trace)
{
    const auto toJsonArray = [](const gtsam::Vector3 &v) { return QJsonArray{v.x(), v.y(), v.z()}; };
    const auto q = trace.attitude.rotation.toQuaternion();
    QJsonArray history;
    for (const FlySight::Fusion::Detail::FitIteration &h : trace.history)
        history.append(QJsonArray{h.outer, h.iteration, h.before, h.after});
    return {{"method", QString::fromStdString(trace.attitude.method)},
            {"interval_s", QJsonArray{trace.attitude.intervalStart, trace.attitude.intervalEnd}},
            {"anchor_time_s", trace.attitude.anchorTime},
            {"gyro_bias_rad_s", toJsonArray(trace.attitude.gyroBias)},
            {"start_quaternion_xyzw", QJsonArray{q.x(), q.y(), q.z(), q.w()}},
            {"converged", trace.converged},
            {"history", history}};
}

} // namespace FlySightTest

#endif // FLYSIGHTTEST_FUSIONTRACE_H
