#ifndef BUILTINCALCULATIONS_H
#define BUILTINCALCULATIONS_H

#include <QString>

#include "../engine/calculationregistry.h"

namespace FlySight {

/// Registers every built-in calculation with the calculation engine, in a
/// fixed order: the conversion layer (source -> effective values), attributes,
/// GNSS, IMU, MAG, time, local coordinates, simplified track, WS-P, SP, and
/// last the synthesized-interpolation family. Registration order is the order
/// in which competing candidates for one output are tried.
///
/// Engine registrations only: safe to call on any number of private registries.
void registerBuiltInCalculations(CalculationRegistry &registry = CalculationRegistry::instance());

/// The calculation-compatibility marker of the logbook column cache
/// (index.json field "calculationCompatibility"). An index whose marker is
/// missing or different has ALL its cached column values discarded at startup;
/// they are recomputed lazily from the session files, which are never touched.
///
/// Bump whenever a code change can alter the value that ANY existing session
/// yields for ANY logbook column: a built-in calculation's arithmetic, inputs,
/// or candidate order; the schema table or the unit-normalization table
/// (src/conversion, src/units/unitconversion.h); the interpolation family;
/// SessionModel::computeColumnValues (what a column stores, or its unit).
/// Do not bump for pure additions / removals / renames of registrations, or
/// for a changed result version - the environment fingerprint below already
/// covers those. Never reuse a value;
/// never set it to 0 (0 is what an index without the field reads as). Not
/// related to SCHEMA_VER, which describes recorded data, not this program.
///
/// Bump it, or the result version of the calculation concerned
/// (CalculationDescriptor::resultVersion), whenever a change can alter what a
/// requested calculation, or anything it reads, produces. A stored result of an
/// explicit calculation is used only while this marker and the result version it
/// was stored with equal the current ones, and every name it looked up still
/// resolves to the same provider with the same result version. Bumping a result
/// version drops the stored results of that calculation and of every requested
/// calculation whose lookups went through it; bumping this marker drops every
/// stored result and every cached column value.
///
/// History: 1 - first marker; invalidates every released index.json, whose
/// gyro-derived columns were computed without the legacy-gyro schema correction.
/// 2 - centered time fit: _TIME_FIT_A/B, and with them every non-GNSS _time,
/// changed at high device uptime.
constexpr int CalculationCompatibilityVersion = 2;

/// The second half of cache validity (index.json field
/// "calculationEnvironment"): which calculations are registered, the order in
/// which candidates for one name are tried, the result versions they declare,
/// and the values of the preferences the calculations declare as inputs.
///
/// SHA-1 (lower-case hex, 40 characters) over registry.candidateOrder() and
/// the preferences, in this order (each list is a label line followed by one
/// id line per id):
///   "attribute:<key>\n" / "measurement:<sensor>/<name>\n" + candidate ids,
///        each with its result version,
///        for every output name a plain calculation declares          (sorted by name)
///   "families\n" + family ids                                         (registration order)
///   "conversions\n" + source-conversion family ids                    (registration order)
///   "pref:<key>=<text>\n"  for every key in registry.declaredPreferenceKeys()  (sorted),
///        text = CsvFormat::formatAttributeValue(provider value).value_or(QString())
/// The relative order of registrations that can never be candidates for the
/// same name does not enter: registering the same calculations in another
/// order gives the same fingerprint unless that changes which one is tried
/// first for some name.
/// A registry without a preference provider contributes empty texts.
///
/// Each id line is "#<id>\n", or "#<id>#<version>\n" when the registration
/// declares a result version (backslash and line feed escaped as "\\" and
/// "\n"); family and conversion ids never carry one. A result version is
/// declared by every Python plug-in registration (the plug-in code identity,
/// plugincodeidentity.h) and by the sensor fusion fit (its algorithm string);
/// built-ins declare none.
///
/// Not covered: a registration whose code changed while its id and result
/// version did not; for the built-ins, that is what
/// CalculationCompatibilityVersion is for.
QString calculationEnvironmentFingerprint(const CalculationRegistry &registry = CalculationRegistry::instance());

/// WS-P / SP marker groups and AttributeRegistry entries (UI metadata that
/// accompanies the built-in calculations). Call once per process.
void registerBuiltInCalculationMetadata();

} // namespace FlySight

#endif // BUILTINCALCULATIONS_H
