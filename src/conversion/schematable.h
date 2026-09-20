#ifndef FLYSIGHT_CONVERSION_SCHEMATABLE_H
#define FLYSIGHT_CONVERSION_SCHEMATABLE_H

#include <optional>

#include <QList>
#include <QString>
#include <QVariant>

// The one authority on recorded data schemas: which SCHEMA_VER values exist,
// what each one corrects, and what counts as a valid recorded value. Shared by
// the conversion layer (which applies the corrections at read time) and the
// importer (which rejects files declaring anything else).
//
// Only the recorded SCHEMA_VER attribute selects a schema. Nothing here looks
// at the firmware version, a file name, a date, or the data itself.
//
// A future schema version adds rows to the table in schematable.cpp and, if it
// needs a new kind of step, a step in the conversion layer. Nothing else.

namespace FlySight {
namespace Schema {

/// The header attribute that declares a file's schema version.
constexpr char AttributeKey[] = "SCHEMA_VER";

/// What an ABSENT attribute means. Interpreted by the conversion layer only;
/// it is never written back as an attribute.
constexpr int ImpliedVersion = 1;

/// Every schema version Viewer understands, ascending.
QList<int> supportedVersions();

/// Recorded value -> version. nullopt when the value is malformed or names an
/// unsupported version. Never returns ImpliedVersion for "absent": a caller
/// that has no attribute does not call this.
std::optional<int> parseVersion(const QVariant &recorded);

/// "Unsupported SCHEMA_VER '<text>' (supported: 1, 2)"
QString unsupportedMessage(const QVariant &recorded);

/// True iff any supported version has a correction for this measurement.
bool isSchemaDependent(const QString &sensor, const QString &name);

/// Multiplicative correction for (version, measurement); 1.0 when the table
/// has no row.
double correctionScale(int version, const QString &sensor, const QString &name);

} // namespace Schema
} // namespace FlySight

#endif // FLYSIGHT_CONVERSION_SCHEMATABLE_H
