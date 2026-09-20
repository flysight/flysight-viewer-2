#ifndef FLYSIGHT_CONVERSION_SOURCECONVERSION_H
#define FLYSIGHT_CONVERSION_SOURCECONVERSION_H

#include "../engine/calculationregistry.h"

namespace FlySight {
namespace Calculations {

/// The conversion layer: the one fixed, built-in transformation from a source
/// measurement (samples and unit text as recorded) to the effective value that
/// ordinary reads return under the same name. Per measurement, in this order:
///
///  1. schema correction, selected by the recorded SCHEMA_VER attribute and the
///     measurement's sensor and name (Schema::, src/conversion/schematable.h);
///  2. unit normalization, selected by the recorded unit text
///     (UnitConversion::, src/units/unitconversion.h).
///
/// The result carries the normalized unit label. A conversion that changes no
/// value hands the source vector on unchanged, so the effective measurement
/// shares the source buffer; any other conversion costs exactly one buffer
/// while it is cached.
///
/// Registers, in this order, through CalculationRegistry::registerSourceConversion:
///
///  - "builtin.conversion.schema": matches only schema-dependent measurements
///    and declares SCHEMA_VER as an input, so it runs only when the session
///    records a schema version;
///  - "builtin.conversion.default": matches every measurement and applies the
///    correction of Schema::ImpliedVersion. This is the only place where an
///    absent SCHEMA_VER is interpreted.
///
/// All declared inputs are required, which is why "with or without SCHEMA_VER"
/// takes two registrations. Measurements that are not schema-dependent have
/// only the second candidate and therefore no dependency on SCHEMA_VER.
///
/// Engine registrations only: safe to call on any number of private registries.
/// The layer is not extensible by plugins and is not configured per session.
void registerSourceConversions(CalculationRegistry &registry);

} // namespace Calculations
} // namespace FlySight

#endif // FLYSIGHT_CONVERSION_SOURCECONVERSION_H
