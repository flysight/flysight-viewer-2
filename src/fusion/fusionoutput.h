#ifndef FLYSIGHT_FUSION_FUSIONOUTPUT_H
#define FLYSIGHT_FUSION_FUSIONOUTPUT_H

#include <QJsonObject>
#include <QString>

#include "fusion/factorgraphfit.h"
#include "fusion/fusion.h"
#include "fusion/initializer.h"
#include "fusion/inputadapter.h"
#include "fusion/trajectoryreconstruction.h"

// Internal to the fusion library: from the fitted trajectory to what run()
// returns, i.e. the seventeen channels and the diagnostics JSON.

namespace FlySight::Fusion::Detail {

/// Fills the seventeen arrays of `result` from the IMU-rate reconstruction
/// `trajectory`: its time with `epoch` added back (UTC), its corrected
/// position, velocity and published acceleration, and its corrected attitude
/// as roll / pitch / yaw in degrees, unwrapped over the whole fit with the
/// rule the GNSS course uses, and as a quaternion xyzw.
void fillOutputChannels(const ImuRateTrajectory &trajectory, double epoch, Result &result);

/// The diagnostics of a converged fit: input audit, the initializer's
/// account, objective, biases, per-factor residuals, the model constants of
/// `tuning`, the statements of what the outputs mean, and the account of the
/// reconstruction from `trajectory`: its sample count and its four summaries
/// (the largest attitude and velocity mismatch at a fix, the largest step
/// correction and when it occurred).
QJsonObject successDiagnostics(const PreparedInput &prepared, const InitializerAccount &account,
                               const FitResult &fit, const Samples &window,
                               const ImuRateTrajectory &trajectory, const Tuning &tuning);

/// The diagnostics of a rejected recording or a failed fit: the algorithm and
/// the reason, plus the stopping account when the fit ran a pass (`stopping`
/// given) and the quality of the reported fit when it completed its passes
/// (`quality` given). A rejection has neither.
QJsonObject failureDiagnostics(const QString &reason, const Stopping *stopping = nullptr,
                               const Quality *quality = nullptr);

QString toCompactJson(const QJsonObject &object);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_FUSIONOUTPUT_H
