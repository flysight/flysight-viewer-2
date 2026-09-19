#ifndef SIMPLIFICATIONCALCULATIONS_H
#define SIMPLIFICATIONCALCULATIONS_H

namespace FlySight {

class CalculationRegistry;

namespace Calculations {

/// Register the simplified track (builtin.simplified.track: one calculation,
/// four measurement outputs) with the calculation engine.
void registerSimplificationCalculations(CalculationRegistry &registry);

} // namespace Calculations
} // namespace FlySight

#endif // SIMPLIFICATIONCALCULATIONS_H
