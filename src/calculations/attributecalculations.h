#ifndef ATTRIBUTECALCULATIONS_H
#define ATTRIBUTECALCULATIONS_H

namespace FlySight {

class CalculationRegistry;

namespace Calculations {

/// Register the session-wide attribute calculations with the calculation
/// engine (ids builtin.attr.*).
void registerAttributeCalculations(CalculationRegistry &registry);

} // namespace Calculations
} // namespace FlySight

#endif // ATTRIBUTECALCULATIONS_H
