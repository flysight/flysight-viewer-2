#ifndef GNSSCALCULATIONS_H
#define GNSSCALCULATIONS_H

namespace FlySight {

class CalculationRegistry;

namespace Calculations {

/// Register the GNSS-based measurements with the calculation engine (ids
/// builtin.gnss.<measurement>).
void registerGnssCalculations(CalculationRegistry &registry);

} // namespace Calculations
} // namespace FlySight

#endif // GNSSCALCULATIONS_H
