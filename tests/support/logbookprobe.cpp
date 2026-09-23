#include "logbookprobe.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSettings>

#include "dependencykey.h"
#include "fixturebuilder.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "testenvironment.h"

using namespace FlySight;

namespace FlySightTest {

QJsonObject readIndex()
{
    return QJsonDocument::fromJson(readFileBytes(TestEnvironment::instance().indexPath())).object();
}

bool writeIndex(const QJsonObject &root)
{
    return writeFile(TestEnvironment::instance().indexPath(), QJsonDocument(root).toJson());
}

QString indexColumnId(const QJsonObject &root, const LogbookColumn &col)
{
    const QJsonObject columns = root[QStringLiteral("columns")].toObject();
    for (auto it = columns.constBegin(); it != columns.constEnd(); ++it) {
        const QJsonObject def = it.value().toObject();
        const bool match = col.type == ColumnType::SessionAttribute
            ? def[QStringLiteral("type")].toString() == QLatin1String("SessionAttribute")
                  && def[QStringLiteral("attributeKey")].toString() == col.attributeKey
            : def[QStringLiteral("type")].toString() == QLatin1String("MeasurementAtMarker")
                  && def[QStringLiteral("sensorID")].toString() == col.sensorID
                  && def[QStringLiteral("measurementID")].toString() == col.measurementID
                  && def[QStringLiteral("markerAttributeKey")].toString() == col.markerAttributeKey;
        if (match)
            return it.key();
    }
    return QString();
}

QJsonValue indexValue(const QJsonObject &root, const QString &sessionId, const LogbookColumn &col)
{
    const QString columnId = indexColumnId(root, col);
    if (columnId.isEmpty())
        return QJsonValue(QJsonValue::Undefined);
    return root[QStringLiteral("sessions")].toObject()[sessionId].toObject()
               [QStringLiteral("values")].toObject().value(columnId);
}

QJsonValue indexValue(const QString &sessionId, const LogbookColumn &col)
{
    return indexValue(readIndex(), sessionId, col);
}

QString sessionFilePath(const QString &sessionId)
{
    const QString uuid = readIndex()[QStringLiteral("sessions")].toObject()[sessionId].toObject()
                             [QStringLiteral("uuid")].toString();
    if (uuid.isEmpty())
        return QString();
    return TestEnvironment::instance().sessionsDir() + QLatin1Char('/') + uuid + QStringLiteral(".csv");
}

QStringList sessionCsvFiles()
{
    return QDir(TestEnvironment::instance().sessionsDir())
        .entryList({QStringLiteral("*.csv")}, QDir::Files, QDir::Name);
}

QStringList calculationRecordFiles()
{
    return QDir(TestEnvironment::instance().sessionsDir())
        .entryList({QStringLiteral("*.fvresult")}, QDir::Files, QDir::Name);
}

QString sessionFileStem(const QString &sessionId)
{
    const QString path = sessionFilePath(sessionId);
    return path.isEmpty() ? QString() : QFileInfo(path).completeBaseName();
}

LogbookColumn descriptionColumn()
{
    LogbookColumn col;
    col.type = ColumnType::SessionAttribute;
    col.attributeKey = QStringLiteral("_DESCRIPTION");
    return col;
}

LogbookColumn exitTimeColumn()
{
    LogbookColumn col;
    col.type = ColumnType::SessionAttribute;
    col.attributeKey = QStringLiteral("_EXIT_TIME");
    return col;
}

LogbookColumn gyroColumn()
{
    LogbookColumn col;
    col.type = ColumnType::MeasurementAtMarker;
    col.sensorID = QStringLiteral("IMU");
    col.measurementID = QStringLiteral("wx");
    col.measurementType = QStringLiteral("rotation");
    col.markerAttributeKey = QStringLiteral("_M");
    return col;
}

void writeAltitudes(const QList<int> &altitudes)
{
    {
        QSettings settings;
        settings.beginWriteArray(QStringLiteral("altitudeMarkers"), altitudes.size());
        for (int i = 0; i < altitudes.size(); ++i) {
            settings.setArrayIndex(i);
            settings.setValue(QStringLiteral("value"), altitudes.at(i));
        }
        settings.endArray();
    }

    PreferencesManager &prefs = PreferencesManager::instance();
    if (prefs.hasPreference(PreferenceKeys::AltitudeMarkersVersion)) {
        const int version = prefs.getValue(PreferenceKeys::AltitudeMarkersVersion).toInt();
        prefs.setValue(PreferenceKeys::AltitudeMarkersVersion, version + 1);
    }
}

bool spyHasAttribute(const QSignalSpy &spy, const QString &sessionId, const QString &key)
{
    for (const QList<QVariant> &args : spy) {
        const DependencyKey name = args.at(1).value<DependencyKey>();
        if (args.at(0).toString() == sessionId && name == DependencyKey::attribute(key))
            return true;
    }
    return false;
}

bool spyHasMeasurement(const QSignalSpy &spy, const QString &sessionId,
                       const QString &sensor, const QString &measurement)
{
    for (const QList<QVariant> &args : spy) {
        const DependencyKey name = args.at(1).value<DependencyKey>();
        if (args.at(0).toString() == sessionId && name == DependencyKey::measurement(sensor, measurement))
            return true;
    }
    return false;
}

} // namespace FlySightTest
