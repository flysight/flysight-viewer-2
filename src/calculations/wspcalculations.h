#ifndef WSPCALCULATIONS_H
#define WSPCALCULATIONS_H

namespace FlySight {

class CalculationRegistry;

namespace Calculations {

/// Register the Wingsuit Performance calculations with the calculation engine
/// (ids builtin.wsp.*). Engine registrations only.
void registerWspCalculations(CalculationRegistry &registry);

/// Register the WS-P marker group and attribute-registry entries (UI metadata).
/// Call once per process.
void registerWspMetadata();

} // namespace Calculations
} // namespace FlySight

#endif // WSPCALCULATIONS_H
