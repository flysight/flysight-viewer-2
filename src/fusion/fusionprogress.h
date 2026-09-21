#ifndef FLYSIGHT_FUSION_FUSIONPROGRESS_H
#define FLYSIGHT_FUSION_FUSIONPROGRESS_H

#include <QString>

#include "fusion/fusion.h"

namespace FlySight::Fusion::Detail {

/// Thrown by Checkpoint when the caller asked to stop.
///
/// Deliberately not a std::exception: no `catch (const std::exception &)` in
/// the library (or in a solver callback) can mistake a cancellation for a
/// failure and swallow it. Only runPipeline() catches it.
class FusionCancelled {};

/// The kernel's one progress-and-cancel facility: a boundary of the fit.
///
/// Calling it reports `text`, then asks whether to stop, in that order. It is
/// called from sequential code only, never from inside a solver's parallel
/// region, so the exception never crosses GTSAM.
class Checkpoint {
public:
    Checkpoint() = default;
    Checkpoint(const ProgressFn &progress, const CancelFn &cancelRequested)
        : m_progress(progress), m_cancelRequested(cancelRequested) {}

    /// Reports `text`; throws FusionCancelled when cancellation was requested.
    void operator()(const QString &text) const
    {
        if (m_progress)
            m_progress(text);
        if (m_cancelRequested && m_cancelRequested())
            throw FusionCancelled();
    }

private:
    ProgressFn m_progress;
    CancelFn m_cancelRequested;
};

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_FUSIONPROGRESS_H
