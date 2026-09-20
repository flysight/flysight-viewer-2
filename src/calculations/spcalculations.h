#ifndef SPCALCULATIONS_H
#define SPCALCULATIONS_H

namespace FlySight {

class CalculationRegistry;

namespace Calculations {

/// Register the Speed Skydiving calculations with the calculation engine
/// (ids builtin.sp.*). Engine registrations only.
void registerSpCalculations(CalculationRegistry &registry);

/// Register the SP marker group and attribute-registry entries (UI metadata).
/// Call once per process.
void registerSpMetadata();

} // namespace Calculations
} // namespace FlySight

#endif // SPCALCULATIONS_H
