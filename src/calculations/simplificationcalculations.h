#ifndef SIMPLIFICATIONCALCULATIONS_H
#define SIMPLIFICATIONCALCULATIONS_H

namespace FlySight {

class CalculationRegistry;

namespace Calculations {

/// Register the simplified track (builtin.simplified.track: one calculation,
/// seven measurement outputs at the same retained sample indices; horizontal
/// Ramer-Douglas-Peucker, 0.5 m, on the shared Local/north and Local/east)
/// with the calculation engine.
void registerSimplificationCalculations(CalculationRegistry &registry);

} // namespace Calculations
} // namespace FlySight

#endif // SIMPLIFICATIONCALCULATIONS_H
