#ifndef FLYSIGHT_FUSION_FUSIONOUTPUT_H
#define FLYSIGHT_FUSION_FUSIONOUTPUT_H

#include <QJsonObject>
#include <QString>

#include <vector>

#include "fusion/factorgraphfit.h"
#include "fusion/fitcovariance.h"
#include "fusion/fusion.h"
#include "fusion/initializer.h"
#include "fusion/inputadapter.h"
#include "fusion/trajectoryreconstruction.h"

// Internal to the fusion library: from the fitted trajectory to what run()
// returns, i.e. the twenty-one channels (the seventeen of the state, and the
// four accuracies when the covariance was computed) and the diagnostics JSON.

namespace FlySight::Fusion::Detail {

/// Fills the arrays of `result` from the IMU-rate reconstruction
/// `trajectory`: its time with `epoch` added back (UTC), its corrected
/// position, velocity and published acceleration, and its corrected attitude
/// as roll / pitch / yaw in degrees, unwrapped over the whole fit with the
/// rule the GNSS course uses, and as a quaternion xyzw. When the trajectory
/// carries the accuracies, also the four accuracy arrays, each sample's
/// multiplied by its widening in `widenings` (aligned with the time):
/// headingAcc = min(180, w x heading), tiltAcc = min(180, w x tilt), accHAcc
/// and accDAcc times w, uncapped. Otherwise those four stay empty.
void fillOutputChannels(const ImuRateTrajectory &trajectory, const std::vector<double> &widenings, double epoch,
                        Result &result);

/// The diagnostics of a converged fit: input audit (with the holes of the
/// fitted window's GNSS axis, `gnss_holes`), the configuration the fit
/// ran under and the noise the datasheet gives for it (both from
/// `tuning.noise`), the initializer's account, objective, biases, the scale
/// factors and their sigmas from `covariance` (null when it was not
/// computed), per-factor residuals, the statements of what the outputs mean,
/// the stopping account with its thresholds and the quality of the reported
/// stage, the account of the scale factors' release (`scale_release`, from
/// `fit.scaleRelease`: the held stage, the released stage, whether it was
/// kept and why not), the account of the
/// reconstruction from `trajectory` (its sample count and its four summaries:
/// the largest attitude and velocity mismatch at a fix, the largest step
/// correction and when it occurred), and the account of the accuracy
/// (`accuracy`): whether `covariance` was computed and why not, the heading
/// prior and the widening's half-width, and, when computed, the largest of
/// `widenings`, how many exceed one, and how many samples of the published
/// `result.headingAcc` are at the cap.
QJsonObject successDiagnostics(const PreparedInput &prepared, const InitializerAccount &account,
                               const FitResult &fit, const Samples &window,
                               const ImuRateTrajectory &trajectory, const Tuning &tuning,
                               const FitCovariance &covariance, const std::vector<double> &widenings,
                               const Result &result);

/// The diagnostics of a rejected recording or a failed fit: the algorithm and
/// the reason, plus the stopping account when the fit ran a pass (`stopping`
/// given: its rule says `cost increased` or `damping saturated` for a pass that
/// failed) and the quality of the reported fit when it completed its passes
/// (`quality` given). A rejection has neither, and no failure carries the
/// account of the scale factors' release.
QJsonObject failureDiagnostics(const QString &reason, const Stopping *stopping = nullptr,
                               const Quality *quality = nullptr);

QString toCompactJson(const QJsonObject &object);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_FUSIONOUTPUT_H
