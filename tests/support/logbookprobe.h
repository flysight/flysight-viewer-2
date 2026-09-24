#ifndef FLYSIGHTTEST_LOGBOOKPROBE_H
#define FLYSIGHTTEST_LOGBOOKPROBE_H

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QSignalSpy>
#include <QString>
#include <QStringList>

#include "logbookcolumn.h"

/// Probes of what the test logbook holds on disk (index.json, the session
/// files and the calculation records), the logbook columns the suites share,
/// and small helpers for the model-level suites. Everything goes through TestEnvironment, so it can only
/// ever look inside the temporary logbook of the running test.
namespace FlySightTest {

// ---- index.json -----------------------------------------------------------

/// index.json as it is on disk right now (an empty object when absent or malformed).
QJsonObject readIndex();
bool writeIndex(const QJsonObject &root);

/// The ephemeral id index.json uses for a column, matched by definition
/// (type, then attribute key, or sensor / measurement / marker); empty when it
/// has none.
QString indexColumnId(const QJsonObject &root, const FlySight::LogbookColumn &col);

/// The value index.json holds for (session, column): Undefined when absent.
QJsonValue indexValue(const QJsonObject &root, const QString &sessionId, const FlySight::LogbookColumn &col);
/// The same, reading index.json from disk.
QJsonValue indexValue(const QString &sessionId, const FlySight::LogbookColumn &col);

/// The "environment" index.json records for a column (the column environment
/// its values were computed under); empty when the column or its environment
/// is absent.
QString indexColumnEnvironment(const QJsonObject &root, const FlySight::LogbookColumn &col);
QString indexColumnEnvironment(const FlySight::LogbookColumn &col);     // reads index.json from disk
/// Sets the "environment" of a column; false when the index has no such column.
bool setIndexColumnEnvironment(QJsonObject &root, const FlySight::LogbookColumn &col, const QString &environment);
/// Removes the "environment" of every column: index.json as a build before
/// per-column environments wrote it (it also wrote a root
/// "calculationEnvironment", which is removed as well).
void removeColumnEnvironments(QJsonObject &root);

/// The "records" stamp index.json holds for a session: an object, or
/// Undefined when the entry has none (older index) or there is no entry.
QJsonValue indexRecordStamp(const QJsonObject &root, const QString &sessionId);
QJsonValue indexRecordStamp(const QString &sessionId);   // reads index.json from disk

// ---- session files --------------------------------------------------------

/// Path of the file of a session according to index.json on disk; empty when
/// the index has no entry (or no uuid) for the session.
QString sessionFilePath(const QString &sessionId);

/// File names (not paths) of the *.csv files in the sessions directory, sorted.
QStringList sessionCsvFiles();

/// File names (not paths) of the *.fvresult calculation record files in the
/// cache directory (TestEnvironment::cacheDir()), sorted; empty when it does not exist.
QStringList calculationRecordFiles();

/// Base name of sessionFilePath(sessionId): the stem its record files are named after; empty when there is none.
QString sessionFileStem(const QString &sessionId);

// ---- calculation records ----------------------------------------------------

/// Makes an existing file unreadable to QFile::open() while it lives, and puts
/// it back (same path, same bytes, readable) on release() or destruction.
/// A mechanism the platform does not honour changes nothing and says why in
/// skipReason(); the caller QSKIPs the row.
///
/// A directory at a record's path is never listed as a record (the listing
/// takes files only), so the result store reaches it only through the ids the
/// logbook manager knows - within a run, not after a restart (the next
/// initialize() lists names, and a directory has none it would adopt).
class UnreadableFile {
public:
    enum class Mechanism {
        Directory,             ///< every platform: the file is removed and a directory made at its path
        LockedWithoutSharing,  ///< Windows only: held open with share mode 0 (CreateFileW)
        NoReadPermission       ///< not Windows: permissions cleared; skipped when it stays readable (root)
    };
    UnreadableFile(const QString &path, Mechanism mechanism);
    ~UnreadableFile();                       // release()
    Q_DISABLE_COPY_MOVE(UnreadableFile)
    QString skipReason() const;              ///< empty when the path exists and QFile cannot open it for reading
    bool release();                          ///< idempotent; false when the file could not be put back

private:
    QString m_path;
    Mechanism m_mechanism;
    QByteArray m_bytes;
    QString m_skip;
    void *m_handle = nullptr;                ///< Windows HANDLE while locked
    bool m_applied = false;
};

/// The same record in the layout of format version 1 with its resolutions
/// section, as development builds wrote it: version field 1 and the environment
/// fingerprint string (40 '0' characters, QDataStream form pinned to Qt_6_0
/// little-endian) after the compatibility stamp, with a fresh SHA-256 trailer;
/// an empty array unless `formatTwo` holds at least 48 bytes with version
/// field 2. It only splices bytes: the codec is not used.
QByteArray asFormatOne(const QByteArray &formatTwo);

// ---- shared logbook columns -----------------------------------------------

FlySight::LogbookColumn descriptionColumn();    ///< session attribute _DESCRIPTION
FlySight::LogbookColumn exitTimeColumn();       ///< session attribute _EXIT_TIME
/// IMU/wx (rotation) at marker _M: needs IMU data, so a SENSOR merge affects it.
FlySight::LogbookColumn gyroColumn();

// ---- model-level helpers ----------------------------------------------------

/// Writes the altitude list the way the preferences page does: the QSettings
/// array first, then a bump of the version preference, which is what makes an
/// existing AltitudeMarkerManager refresh.
void writeAltitudes(const QList<int> &altitudes);

/// True when a SessionModel::dependencyChanged spy saw (sessionId, name).
bool spyHasAttribute(const QSignalSpy &spy, const QString &sessionId, const QString &key);
bool spyHasMeasurement(const QSignalSpy &spy, const QString &sessionId,
                       const QString &sensor, const QString &measurement);

} // namespace FlySightTest

#endif // FLYSIGHTTEST_LOGBOOKPROBE_H
