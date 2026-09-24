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
/// missing or different has ALL its cached column values discarded at startup,
/// whatever the column environments (below) say; they are recomputed lazily from the session files, which are never touched.
///
/// Bump whenever a code change can alter the value that ANY existing session
/// yields for ANY logbook column: a built-in calculation's arithmetic, inputs,
/// or candidate order; the schema table or the unit-normalization table
/// (src/conversion, src/units/unitconversion.h); the interpolation family;
/// SessionModel::computeColumnValues (what a column stores, or its unit).
/// Do not bump for pure additions / removals / renames of registrations, or
/// for a changed result version - the column environment digest below
/// already covers those. Never reuse a value; never set it to 0 (0 is what an index
/// without the field reads as). Not related to SCHEMA_VER, which describes
/// recorded data, not this program.
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

/// The second half of cache validity, one per logbook column (index.json field
/// "environment" of each column definition, logbookColumnEnvironment()): the
/// part of the registrations and preferences that the value of `names` can
/// observe. A cached column value is kept exactly while the digest of its
/// column is unchanged, so a registry or preference change discards only the
/// values of the columns it can affect.
///
/// The closure: C = the union of CalculationRegistry::staticDependencies(n)
/// .names over `names` (every name reachable through the declared inputs of
/// EVERY candidate and, for a measurement, of every source conversion - not
/// only of the ones that would win -, `names` included), and P = the union of
/// the matching .preferences (family instances included).
///
/// SHA-1 (lower-case hex, 40 characters) over, for every name of C (sorted):
///   "attribute:<key>\n" or "measurement:<sensor>/<name>\n", then
///   one id line per registry.candidatesFor(name), in the order they are tried
///        (the plain calculations declaring it and the family instances
///        accepting it), then, for a measurement only,
///   "conversions\n" + one id line per sourceConversionsFor(sensor, name)
///        while any source conversion is registered, else "no conversions\n"
///        (whether one is registered decides between the passthrough and the
///        conversion layer for a measurement with source data);
/// then "pref:<key>=<text>\n" for every key of P (sorted), text =
/// CsvFormat::formatAttributeValue(provider value).value_or(QString()); a
/// registry without a preference provider contributes empty texts.
/// Each id line is "#<instance id>\n", or "#<instance id>#<version>\n" when the
/// registration declares a result version. A result version is declared by
/// every Python plug-in registration (the plug-in code identity,
/// plugincodeidentity.h) and by the sensor fusion fit (its algorithm string);
/// built-ins declare none. Keys, ids, versions and preference texts have
/// backslash and line feed escaped as "\\" and "\n"; a sensor and a
/// measurement name also have '/' escaped as "\/".
///
/// Why this is enough (docs/CALCULATIONS.md section 17): resolving a name
/// reads session state (a stored attribute, source data) or tries exactly the
/// candidates listed for it, in that order; a candidate is a pure function of
/// its declared inputs, which are names of C, preferences of P, or the source
/// layer. So while the session, the code and the digest are unchanged, a
/// fresh evaluation of `names` gives what it gave before. The relative order
/// of registrations that are never candidates for the same name does not
/// enter: a calculation registered at run time (appended) hashes the same as
/// after the next start, where it may be registered between others.
///
/// Not covered: a registration whose code changed while its id and result
/// version did not; for the built-ins, that is what
/// CalculationCompatibilityVersion is for.
QString calculationEnvironmentDigest(const QList<DependencyKey> &names,
                                     const CalculationRegistry &registry = CalculationRegistry::instance());

/// WS-P / SP marker groups and AttributeRegistry entries (UI metadata that
/// accompanies the built-in calculations). Call once per process.
void registerBuiltInCalculationMetadata();

} // namespace FlySight

#endif // BUILTINCALCULATIONS_H
