#ifndef FLYSIGHTTEST_LOGBOOKPROBE_H
#define FLYSIGHTTEST_LOGBOOKPROBE_H

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QSignalSpy>
#include <QString>
#include <QStringList>

#include "logbookcolumn.h"

/// Probes of what the test logbook holds on disk (index.json and the session
/// files), the logbook columns the suites share, and small helpers for the
/// model-level suites. Everything goes through TestEnvironment, so it can only
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

// ---- session files --------------------------------------------------------

/// Path of the file of a session according to index.json on disk; empty when
/// the index has no entry (or no uuid) for the session.
QString sessionFilePath(const QString &sessionId);

/// File names (not paths) of the *.csv files in the sessions directory, sorted.
QStringList sessionCsvFiles();

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
