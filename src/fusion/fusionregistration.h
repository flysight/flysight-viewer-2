#ifndef FLYSIGHT_FUSION_FUSIONREGISTRATION_H
#define FLYSIGHT_FUSION_FUSIONREGISTRATION_H

#include <functional>

#include <QList>
#include <QString>
#include <QVariant>
#include <QVector>

#include "engine/calculationregistry.h"
#include "fusion/fusion.h"

namespace FlySight::Fusion {

/// Identity of the explicit "Sensor fusion" calculation, for code that
/// requests it by id.
inline constexpr char FitCalculationId[] = "builtin.fusion.fit";

/// The twenty-six declared inputs of builtin.fusion.fit: the eighteen
/// measurements, in the order the kernel's Channels take them, then the four
/// origin attributes (_LOCAL_ORIGIN_INDEX, _LAT, _LON, _HMSL), then the four
/// IMU configuration attributes (ACCEL_FS_G, GYRO_FS_DEG_S, ACCEL_ODR_HZ,
/// GYRO_ODR_HZ; sensorconfiguration.h). One table serves the declaration,
/// the hand-over and the tooling (fusion_runner --dump-inputs).
QList<CalcInput> fitInputs();

/// Effective values, as any reader serves them.
using MeasurementReader = std::function<QVector<double>(const QString &sensor, const QString &name)>;
using AttributeReader   = std::function<QVariant(const QString &key)>;

/// The kernel's input assembled from effective values: every measurement of
/// fitInputs() into its Channels member, the origin attributes (an origin
/// index that is not a number becomes -1, the kernel's "outside the GNSS
/// samples"), and the configuration attributes into
/// Channels::imuConfiguration (a value that is not a number stays NaN).
/// Field-by-field copies of implicitly shared vectors. The
/// registered calculation and fusion_runner both call this, so the two cannot
/// drift apart.
Channels channelsFrom(const MeasurementReader &measurement, const AttributeReader &attribute);

/// The twenty-one measurement outputs of the fit in publication order, each
/// named as it is published under the Fusion sensor (_time, north, ..., qw,
/// then headingAcc, tiltAcc, accHAcc, accDAcc), with the array of `result`
/// that holds it (empty arrays unless Succeeded; the four accuracies also
/// empty for a success whose covariance failed).
struct FitOutputChannel {
    QString name;
    QVector<double> samples;
};
QList<FitOutputChannel> fitOutputChannels(const Result &result);

/// Register sensor fusion with the calculation engine:
///
///  - builtin.default.ACCEL_FS_G, builtin.default.GYRO_FS_DEG_S,
///    builtin.default.ACCEL_ODR_HZ and builtin.default.GYRO_ODR_HZ
///    (Calculations::addConstantDefault), in that order: the constant defaults
///    of the four configuration inputs of the fit, as text, from
///    SensorConfiguration::defaultValue (sensorconfiguration.h). A recording
///    that states a key reads its own value.
///  - builtin.fusion.fit (explicit, title "Sensor fusion"): the batch GNSS/IMU
///    fit of fusion.h as one calculation of the twenty-six inputs of
///    fitInputs(), with twenty-two outputs published
///    together: the measurements Fusion/_time, north, east, down, velN, velE,
///    velD, accN, accE, accD, roll, pitch, yaw, qx, qy, qz, qw, the
///    accuracies headingAcc, tiltAcc (deg), accHAcc, accDAcc (m/s^2), and the
///    attribute _FUSION_DIAGNOSTICS. A recording the model rejects and a
///    solver failure are results: the measurements are unavailable, the
///    diagnostics attribute and the result's reason say why. A success whose
///    covariance could not be computed publishes the seventeen and leaves the
///    four accuracies unavailable; its diagnostics say why.
///  - builtin.fusion.accH (on demand): Fusion/accH, the horizontal magnitude
///    of accN and accE.
///  - builtin.fusion.systemTime (on demand): Fusion/_system_time, the inverse
///    time fit of Fusion/_time.
///  - builtin.fusion.z (on demand): Fusion/z, the elevation above the ground,
///    _LOCAL_ORIGIN_HMSL less down less _GROUND_ELEV, as GNSS/z is hMSL less
///    _GROUND_ELEV; unavailable when either attribute is not a number.
///  - builtin.fusion.accAlongTrack (on demand): Fusion/accAlongTrack, the
///    GNSS along-track acceleration (calculations/trackhelper.h) of accN,
///    accE, accD against the wind-corrected velN, velE, velD.
///  - builtin.fusion.accCrossTrack (on demand): Fusion/accCrossTrack, the
///    GNSS cross-track acceleration of the same inputs.
///  - builtin.default._ORIENTATION (Calculations::addConstantDefault): the
///    constant default of the orientation attribute _ORIENTATION, forward +y,
///    up +z (Orientation::defaultOrientation(), fusion/orientation.h). The
///    attribute's definition (category "Session", "Orientation", a Choice of
///    the 24 orientations) is registered with it, once per process.
///  - builtin.fusion.attitude (on demand): Fusion/bodyHeading, bodyPitch and
///    bodyRoll, published together, in degrees: the aircraft Euler angles of
///    the body frame the orientation attribute defines, from the quaternion
///    qx..qw. Heading is the forward axis clockwise from north, unwrapped by
///    the one unwrap rule and not referenced to the course reference (a
///    compass heading, where GNSS/course is relative to its reference), pitch
///    the forward axis above the horizontal in [-90, 90], roll about the
///    forward axis, right side down positive, in (-180, 180]. Its inputs are
///    the quaternion and _ORIENTATION only. Unavailable for a stored
///    orientation that is not one of the 24.
///
/// The derived values' inputs exist only once the fit has published, so they
/// appear with it and never start it. Vertical acceleration is Fusion/accD
/// itself (positive down, like GNSS/accD) and has no calculation of its own.
///
/// The application's one entry point into this library: it calls this directly
/// after Calculations::registerBuiltInCalculations(), and it also registers the
/// orientation attribute's definition in the AttributeRegistry. The three
/// helpers above are what the tests' tooling (fusion_runner) uses to feed the
/// kernel exactly what the registered calculation feeds it. flysight_core
/// never references any of them.
void registerFusionCalculations(CalculationRegistry &registry = CalculationRegistry::instance());

} // namespace FlySight::Fusion

#endif // FLYSIGHT_FUSION_FUSIONREGISTRATION_H
