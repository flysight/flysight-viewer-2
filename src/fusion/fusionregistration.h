#ifndef FLYSIGHT_FUSION_FUSIONREGISTRATION_H
#define FLYSIGHT_FUSION_FUSIONREGISTRATION_H

#include "engine/calculationregistry.h"

namespace FlySight::Fusion {

/// Identity of the explicit "Sensor fusion" calculation, for code that
/// requests it by id.
inline constexpr char FitCalculationId[] = "builtin.fusion.fit";

/// Register sensor fusion with the calculation engine:
///
///  - builtin.fusion.fit (explicit, title "Sensor fusion"): the batch GNSS/IMU
///    fit of fusion.h as one calculation with eighteen outputs published
///    together: the measurements Fusion/_time, north, east, down, velN, velE,
///    velD, accN, accE, accD, roll, pitch, yaw, qx, qy, qz, qw and the
///    attribute _FUSION_DIAGNOSTICS. A recording the model rejects and a
///    solver failure are results: the measurements are unavailable, the
///    diagnostics attribute and the result's reason say why.
///  - builtin.fusion.accH (on demand): Fusion/accH, the horizontal magnitude
///    of accN and accE.
///  - builtin.fusion.systemTime (on demand): Fusion/_system_time, the inverse
///    time fit of Fusion/_time.
///
/// The only function of this library the application and the tests call;
/// flysight_core never references it. The application calls it directly after
/// Calculations::registerBuiltInCalculations().
void registerFusionCalculations(CalculationRegistry &registry = CalculationRegistry::instance());

} // namespace FlySight::Fusion

#endif // FLYSIGHT_FUSION_FUSIONREGISTRATION_H
