#ifndef DATAIMPORTER_H
#define DATAIMPORTER_H

#include "parsedfile.h"
#include "sessiondata.h"
#include <QString>
#include <QTextStream>
#include <optional>

namespace FlySight {

/// Reads FlySight 1 ("time,lat,lon,hMSL,...") and FlySight 2 ("$FLYS") files.
///
/// A file is parsed into a private staging structure, validated, and only then
/// published to the session through SessionData's public API. What is published
/// is exactly what the file says: sample values as parsed, unit text verbatim,
/// header attributes verbatim (unknown keys included). Nothing is scaled,
/// converted, relabelled, defaulted, or stamped - in particular SCHEMA_VER is
/// never inserted or rewritten. Schema correction and unit normalization happen
/// at read time, in the conversion layer.
///
/// Parsing is separate from session creation. parseFile() yields a ParsedFile:
/// what the file says plus a match id. Viewer's import-time defaults
/// (description, import time, wind, mass, area, fixed ground elevation, and the
/// synthesized SESSION_ID / DEVICE_ID) are written by applyCreationDefaults()
/// and by nothing else; SessionModel::mergeSessions calls it only when a file
/// creates a new session, never for a merge.
///
/// Numeric fields: `nan`, `inf`, `-inf` are non-finite samples (written by
/// DataExporter for non-finite source values); every other field is parsed
/// with QStringView::toDouble (correctly rounded). Timestamps ending in `Z`
/// become seconds since the epoch with millisecond precision. Numeric fields
/// go through CsvFormat::parseDouble, the inverse of the exporter's
/// CsvFormat::formatDouble, so a saved sample reloads with identical bits.
/// $VAR values are not parsed at all: they stay QString, verbatim.
///
/// Error policy:
///  - header problems are structural errors and reject the file (see readFile);
///  - an unsupported or malformed SCHEMA_VER rejects the file;
///  - malformed data rows (truncated last line after power loss, a glitched
///    row, a row for an undeclared sensor) are skipped and summarized in one
///    warning per file;
///  - a file without data rows is valid.
class DataImporter {
public:
    DataImporter() = default;

    /// Parses a file into `out`: recorded header attributes and source data in
    /// out.data, the path, and the match id (the recorded SESSION_ID, else the
    /// MD5 of the file bytes as lower-case hex). Nothing is written into
    /// out.data that the file did not contain - not the hash, not DEVICE_ID,
    /// not any `_` key. This is what the application imports with.
    /// On failure returns false with getLastError() set (see readFile) and
    /// leaves `out` equivalent to a default-constructed ParsedFile.
    bool parseFile(const QString& fileName, ParsedFile& out);

    /// The ONLY writer of import-time defaults. `session` is the new session,
    /// already holding a copy of file.data's state. Each default is written
    /// only when the key is not already stored (a Viewer-saved file imported
    /// as a new session keeps its own `_` values), in this order: SESSION_ID
    /// (the synthesized match id, when none was recorded), DEVICE_ID (from
    /// FLYSIGHT.TXT above file.filePath, else SessionKeys::DeviceIdUnknown;
    /// skipped entirely when file.filePath is empty), _DESCRIPTION,
    /// _IMPORT_TIME, _WIND_N / _WIND_E, _JUMPER_MASS / _PLANFORM_AREA, and
    /// _GROUND_ELEV when the ground reference mode is "Fixed".
    static void applyCreationDefaults(const ParsedFile& file, SessionData& session);

    /// Convenience for tests and tools: parse + create. The application never
    /// calls this; it imports through `SessionImport::importFiles`, where the
    /// model decides between creation and merge.
    /// Clears the last error at entry; on failure sessionData is untouched.
    bool importFile(const QString& fileName, SessionData& sessionData);

    /// Parses a file into sessionData without device-specific initialization.
    ///
    /// Clears the last error at entry. On failure it returns false with a
    /// non-empty getLastError() and publishes nothing: sessionData (and
    /// *fileData) are left exactly as they were passed in. On success it
    /// publishes one setAttribute() per header attribute, in file order, then
    /// the staged source data through one mergeSourceData(). Callers pass a
    /// fresh SessionData; merging into an existing session is the model's job.
    ///
    /// Errors: "Couldn't read file", "Empty file", "Unknown file format",
    /// Schema::unsupportedMessage(), "Missing $DATA section", and structural
    /// errors of the form "Line <n>: <reason>" (1-based line numbers).
    bool readFile(const QString& fileName, SessionData& sessionData, QByteArray* fileData = nullptr);

    /// Header-only read: the first value of `$VAR,<key>,...` before `$DATA`
    /// (same grammar as readFile: the value is the verbatim remainder after the
    /// second comma). No data row is read and no SessionData is built. nullopt
    /// when the file cannot be opened or does not declare the key.
    static std::optional<QString> peekHeaderAttribute(const QString& fileName, const QString& key);

    /// Text of the last import error; empty after a successful call.
    QString getLastError() const;

private:
    // Enums and type definitions
    enum class FS_FileType { FS1, FS2 };
    enum class FS_Section { HEADER, DATA };

    // Staging structures (defined in dataimporter.cpp)
    struct StagedSensor;
    struct StagedFile;

    // Last import error
    QString m_lastError;

    // Parsing into the staging structure. The bool helpers return false after
    // setting m_lastError; none of them touches a SessionData.
    bool importSimple(QTextStream& in, StagedFile& staged, const QString &sensorName);
    bool importFS2(QTextStream& in, StagedFile& staged);
    bool importHeaderRow(const QString& line, int lineNumber, StagedFile& staged, FS_Section& section);
    void importDataRow(const QString& line, StagedFile& staged, const QString& fixedSensor);
    bool structuralError(int lineNumber, const QString& reason);

    // Publication: the only step that touches the session
    static void publish(const StagedFile& staged, SessionData& sessionData);

    // DEVICE_ID from the FLYSIGHT.TXT found above the file; "" when there is none
    static QString extractDeviceId(const QString& fileName);
    static QString findFlySightRoot(const QString& filePath);

    // Default sensor ID for FS1 files
    static constexpr char DefaultSensorId[] = "GNSS";

    static QString getDescription(const QString& fileName);
};

} // namespace FlySight

#endif // DATAIMPORTER_H
