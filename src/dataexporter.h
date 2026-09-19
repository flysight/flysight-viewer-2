#ifndef DATAEXPORTER_H
#define DATAEXPORTER_H

#include <optional>

#include <QByteArray>
#include <QString>

#include "sessiondata.h"

namespace FlySight {

/// Writes a session in FlySight 2 CSV form. The file is a function of the
/// SOURCE layer and the STORED attribute map and of nothing else: the exporter
/// reads only sourceData() and stored attributes, and never calls
/// getAttribute / getMeasurement / effectiveUnit / calculationEngine(). A save
/// therefore never computes, never creates an engine, and is the same whether
/// the session's calculation cache is warm or cold.
///
/// What is written:
///  - header attributes exactly as stored - unknown keys included, SCHEMA_VER
///    if and only if it is stored, never a default - and every "_" attribute.
///    Values take the text forms of CsvFormat::formatAttributeValue (strings
///    verbatim, doubles in shortest-round-trip form, ...);
///  - per sensor a $COL and a $UNIT line (labels and unit text verbatim);
///  - samples through CsvFormat::formatDouble: the shortest decimal text that
///    the importer parses back to the same bits; negative zero as "-0";
///    non-finite samples as "nan", "inf", "-inf" - never as zero, never
///    dropped. Time columns are written as the numeric seconds they are stored
///    as (as every released version has done).
///  Output order (known attributes / sensors / columns first, then map order)
///  is deterministic and part of the byte-stability guarantee: save -> load ->
///  save yields identical bytes. UTF-8, no BOM, "\n" line endings.
///
/// What cannot be written:
///  - a line break inside an attribute value is replaced by one space
///    (CsvFormat::singleLine); SessionModel already does this at its text-edit
///    entry points, the exporter is the last line of defence;
///  - an attribute whose key contains ',' or a line break, or whose value is
///    invalid / of an unconvertible type, is SKIPPED with a warning. Nothing
///    the importer or the UI produces is like that, and it is not worth
///    failing the save of a whole session for;
///  - a sensor name, column label or unit containing ',' or a line break, and
///    a RAGGED sensor (columns of unequal length), make the save FAIL. The row
///    format cannot express unequal lengths; padding would make the reloaded
///    source differ from the saved one and truncating would lose data.
///    All-empty sensors (header-only recordings) are valid;
///  - a stored SCHEMA_VER that Schema::parseVersion rejects makes the save
///    FAIL. Never writes a file that DataImporter would reject.
///  Validation happens before the target file is opened; on any failure nothing
///  is written and a previous file at the path is left intact (QSaveFile).
class DataExporter {
public:
    DataExporter() = default;

    /// On failure returns false and *error (if given) names the reason:
    ///   "Sensor '<s>': name/column/unit text cannot be written ('<text>')"
    ///   "Sensor '<s>' has columns of unequal length (<c1>: <n1>, <c2>: <n2>)"
    ///   "Unsupported SCHEMA_VER '<text>' (supported: 1, 2)"
    ///   "Couldn't write file '<path>': <reason>"
    static bool exportSession(const QString &filePath, const SessionData &sessionData,
                              QString *error = nullptr);

    /// The same bytes into memory (whole file; meant for tests). nullopt on a
    /// validation failure, with *error set as above.
    static std::optional<QByteArray> toBytes(const SessionData &sessionData, QString *error = nullptr);
};

} // namespace FlySight

#endif // DATAEXPORTER_H
