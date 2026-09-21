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
/// Do not bump for pure additions / removals / renames of registrations - the
/// environment fingerprint below already covers those. Never reuse a value;
/// never set it to 0 (0 is what an index without the field reads as). Not
/// related to SCHEMA_VER, which describes recorded data, not this program.
///
/// History: 1 - first marker; invalidates every released index.json, whose
/// gyro-derived columns were computed without the legacy-gyro schema correction.
/// 2 - centered time fit: _TIME_FIT_A/B, and with them every non-GNSS _time,
/// changed at high device uptime.
constexpr int CalculationCompatibilityVersion = 2;

/// The second half of cache validity (index.json field
/// "calculationEnvironment"): which calculations are registered, the order in
/// which candidates for one name are tried, and the values of the preferences
/// the calculations declare as inputs.
///
/// SHA-1 (lower-case hex, 40 characters) over registry.candidateOrder() and
/// the preferences, in this order (each list is a label line followed by one
/// "#<id>\n" line per id):
///   "attribute:<key>\n" / "measurement:<sensor>/<name>\n" + candidate ids,
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
/// Not covered: a registration whose id is unchanged but whose code changed
/// (for the built-ins that is what CalculationCompatibilityVersion is for).
QString calculationEnvironmentFingerprint(const CalculationRegistry &registry = CalculationRegistry::instance());

/// WS-P / SP marker groups and AttributeRegistry entries (UI metadata that
/// accompanies the built-in calculations). Call once per process.
void registerBuiltInCalculationMetadata();

} // namespace FlySight

#endif // BUILTINCALCULATIONS_H
