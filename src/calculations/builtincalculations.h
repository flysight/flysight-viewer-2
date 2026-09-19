#ifndef BUILTINCALCULATIONS_H
#define BUILTINCALCULATIONS_H

#include "../engine/calculationregistry.h"

namespace FlySight {

/// Registers every built-in calculation with the calculation engine, in a
/// fixed order: attributes, GNSS, IMU, MAG, time, simplified track, WS-P, SP,
/// and last the synthesized-interpolation family. Registration order is the
/// order in which competing candidates for one output are tried.
///
/// Engine registrations only: safe to call on any number of private registries.
void registerBuiltInCalculations(CalculationRegistry &registry = CalculationRegistry::instance());

/// WS-P / SP marker groups and AttributeRegistry entries (UI metadata that
/// accompanies the built-in calculations). Call once per process.
void registerBuiltInCalculationMetadata();

} // namespace FlySight

#endif // BUILTINCALCULATIONS_H
