#ifndef MAGCALCULATIONS_H
#define MAGCALCULATIONS_H

namespace FlySight {

class CalculationRegistry;

namespace Calculations {

/// Register the MAG-based measurements with the calculation engine (ids
/// builtin.mag.<measurement>).
void registerMagCalculations(CalculationRegistry &registry);

} // namespace Calculations
} // namespace FlySight

#endif // MAGCALCULATIONS_H
