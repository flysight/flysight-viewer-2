#ifndef CALCULATIONS_REGISTRATION_H
#define CALCULATIONS_REGISTRATION_H

#include <QtGlobal>

#include "../engine/calculationdescriptor.h"
#include "../engine/calculationregistry.h"

namespace FlySight {
namespace Calculations {

/// Registers one built-in calculation. A built-in descriptor is a program
/// constant, so a rejected registration (the registry has already logged the
/// reason) is a programming error.
inline void addCalculation(CalculationRegistry &registry, const CalculationDescriptor &descriptor)
{
    const bool ok = registry.registerCalculation(descriptor);
    Q_ASSERT_X(ok, "Calculations::addCalculation", qPrintable(descriptor.id));
    Q_UNUSED(ok);
}

} // namespace Calculations
} // namespace FlySight

#endif // CALCULATIONS_REGISTRATION_H
