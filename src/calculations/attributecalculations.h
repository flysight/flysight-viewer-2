#ifndef ATTRIBUTECALCULATIONS_H
#define ATTRIBUTECALCULATIONS_H

#include <QString>
#include <QVariant>

#include "registration.h"

namespace FlySight {
namespace Calculations {

/// Register the session-wide attribute calculations with the calculation
/// engine (ids builtin.attr.*, and the constant defaults
/// builtin.default._WIND_N and builtin.default._WIND_E, and that of the
/// Compute attribute).
void registerAttributeCalculations(CalculationRegistry &registry);

/// Registers the constant default of an attribute: the calculation
/// `builtin.default.<attributeKey>`, with no inputs and the attribute as its
/// one output, whose value is `value`. A constant default is a calculation
/// with no inputs, so the engine runs it once and caches it; a stored
/// attribute of the same key wins over it, even an invalid or empty one.
/// Returning a session to the default therefore means removing the stored
/// attribute, never storing a blank. Every constant default in the
/// application is registered here, so searching for this name finds them
/// all. Defined inline so that a library which reaches the engine but not
/// the calculations library (the fusion registration) can call it.
inline void addConstantDefault(CalculationRegistry &registry,
                               const QString &attributeKey, const QVariant &value)
{
    CalculationDescriptor d;
    d.id = QStringLiteral("builtin.default.") + attributeKey;
    d.outputs = { DependencyKey::attribute(attributeKey) };
    d.compute = [attributeKey, value](const EvaluationContext &) -> CalculationResult {
        return CalculationResult().setAttribute(attributeKey, value);
    };
    addCalculation(registry, d);
}

} // namespace Calculations
} // namespace FlySight

#endif // ATTRIBUTECALCULATIONS_H
