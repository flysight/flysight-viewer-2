#ifndef DATAIMPORTER_H
#define DATAIMPORTER_H

#include "sessiondata.h"
#include <QString>
#include <QTextStream>

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

    /// readFile(), then - only when it succeeded - initializeFromDevice().
    /// Clears the last error at entry.
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

    /// Applies the creation-time defaults (description, device ID, session ID,
    /// import time, wind, mass, area, fixed ground elevation).
    void initializeFromDevice(const QString& fileName, const QByteArray& fileData, SessionData& sessionData);

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

    // Helper function to extract device ID
    void extractDeviceId(const QString& fileName, SessionData& sessionData, const QString& expectedKey);
    QString findFlySightRoot(const QString& filePath);

    // Default sensor ID for FS1 files
    static constexpr char DefaultSensorId[] = "GNSS";

    static QString getDescription(const QString& fileName);
};

} // namespace FlySight

#endif // DATAIMPORTER_H
