#ifndef INTERPOLATIONCALCULATIONS_H
#define INTERPOLATIONCALCULATIONS_H

namespace FlySight {

class CalculationRegistry;

namespace Calculations {

/// Register synthesized interpolation as the calculation family
/// "builtin.interpolation".
///
/// An attribute named "{timeAttr}:{sensor}/{timeVector}/{dataVector}" (see
/// SessionData::interpolationKey) is the value of sensor/dataVector linearly
/// interpolated at the time held by the attribute timeAttr, using
/// sensor/timeVector as the time axis. Each distinct name is one instance
/// ("builtin.interpolation#<name>") with its own result and dependencies.
void registerInterpolationFamily(CalculationRegistry &registry);

} // namespace Calculations
} // namespace FlySight

#endif // INTERPOLATIONCALCULATIONS_H
