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

/// Fills the seventeen arrays of `result` from `dense`: UTC time restored by
/// adding `epoch`, roll / pitch / yaw in degrees and unwrapped over the whole
/// fit with the rule the GNSS course uses, quaternion xyzw.
void fillOutputChannels(const DenseTrajectory &dense, double epoch, Result &result);

/// The diagnostics of a converged fit: input audit, the initializer's
/// account, objective, biases, per-factor residuals, the model constants of
/// `tuning`, and the statements of what the outputs mean.
QJsonObject successDiagnostics(const PreparedInput &prepared, const InitializerAccount &account,
                               const FitResult &fit, const Samples &window,
                               const DenseTrajectory &dense, const Tuning &tuning);

/// The diagnostics of a rejected recording or a failed fit: the algorithm and
/// the reason, plus the stopping account when the fit ran a pass (`stopping`
/// given) and the quality of the reported fit when it completed its passes
/// (`quality` given). A rejection has neither.
QJsonObject failureDiagnostics(const QString &reason, const Stopping *stopping = nullptr,
                               const Quality *quality = nullptr);

QString toCompactJson(const QJsonObject &object);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_FUSIONOUTPUT_H
