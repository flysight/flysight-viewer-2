#include "logbookprobe.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
#include <QSettings>
#include <QtEndian>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

QJsonValue indexRecordStamp(const QJsonObject &root, const QString &sessionId)
{
    const QJsonObject sessions = root[QStringLiteral("sessions")].toObject();
    if (!sessions.contains(sessionId))
        return QJsonValue(QJsonValue::Undefined);
    return sessions[sessionId].toObject().value(QStringLiteral("records"));
}

QJsonValue indexRecordStamp(const QString &sessionId)
{
    return indexRecordStamp(readIndex(), sessionId);
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
    return QDir(TestEnvironment::instance().cacheDir())
        .entryList({QStringLiteral("*.fvresult")}, QDir::Files, QDir::Name);
}

QString sessionFileStem(const QString &sessionId)
{
    const QString path = sessionFilePath(sessionId);
    return path.isEmpty() ? QString() : QFileInfo(path).completeBaseName();
}

// ---- calculation records ----------------------------------------------------

UnreadableFile::UnreadableFile(const QString &path, Mechanism mechanism)
    : m_path(path), m_mechanism(mechanism)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_skip = QStringLiteral("the file cannot be read before the test: %1").arg(file.errorString());
        return;
    }
    m_bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        m_skip = QStringLiteral("the file cannot be read before the test: %1").arg(file.errorString());
        return;
    }
    file.close();

    switch (mechanism) {
    case Mechanism::Directory:
        if (!QFile::remove(path)) {
            m_skip = QStringLiteral("the file could not be removed");
            return;
        }
        m_applied = true;       // from here on release() puts the bytes back
        if (!QDir().mkdir(path)) {
            release();
            m_skip = QStringLiteral("no directory could be made at the path");
            return;
        }
        break;

    case Mechanism::LockedWithoutSharing:
#ifdef Q_OS_WIN
    {
        const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
                                          GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                          nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            m_skip = QStringLiteral("CreateFileW failed (error %1)").arg(qulonglong(GetLastError()));
            return;
        }
        m_handle = handle;
        m_applied = true;
        break;
    }
#else
        m_skip = QStringLiteral("Windows only");
        return;
#endif

    case Mechanism::NoReadPermission:
#ifdef Q_OS_WIN
        m_skip = QStringLiteral("not on Windows");
        return;
#else
        if (!QFile::setPermissions(path, QFileDevice::Permissions())) {
            m_skip = QStringLiteral("the permissions could not be changed");
            return;
        }
        m_applied = true;
        break;
#endif
    }

    // The mechanism must actually make the path unreadable here.
    if (!QFileInfo::exists(path)) {
        release();
        m_skip = QStringLiteral("nothing at the path");
        return;
    }
    QFile probe(path);
    if (probe.open(QIODevice::ReadOnly)) {
        probe.close();
        release();
        m_skip = QStringLiteral("the file stays readable here (running as root?)");
    }
}

UnreadableFile::~UnreadableFile()
{
    release();
}

QString UnreadableFile::skipReason() const
{
    return m_skip;
}

bool UnreadableFile::release()
{
    if (!m_applied)
        return true;
    m_applied = false;

    switch (m_mechanism) {
    case Mechanism::Directory:
        // No directory when mkdir failed: only the bytes go back.
        if (QFileInfo(m_path).isDir() && !QDir().rmdir(m_path))
            return false;
        return writeFile(m_path, m_bytes);

    case Mechanism::LockedWithoutSharing:
#ifdef Q_OS_WIN
        if (m_handle) {
            const bool closed = CloseHandle(static_cast<HANDLE>(m_handle)) != 0;
            m_handle = nullptr;
            return closed;
        }
#endif
        return true;

    case Mechanism::NoReadPermission:
        return QFile::setPermissions(m_path, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                 | QFileDevice::ReadUser | QFileDevice::WriteUser);
    }
    return false;
}

QByteArray asFormatOne(const QByteArray &formatTwo)
{
    constexpr int kMagicSize = 8;
    constexpr int kChecksumSize = 32;
    if (formatTwo.size() < kMagicSize + 4 + 4 + kChecksumSize
        || qFromLittleEndian<quint32>(formatTwo.constData() + kMagicSize) != 2u)
        return QByteArray();

    QByteArray environment;
    {
        QDataStream stream(&environment, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_0);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream << QString(40, QLatin1Char('0'));
    }

    QByteArray one = formatTwo.first(kMagicSize);
    char version[4];
    qToLittleEndian<quint32>(1u, version);
    one.append(version, 4);
    one.append(formatTwo.mid(kMagicSize + 4, 4));           // the compatibility stamp
    one.append(environment);
    one.append(formatTwo.mid(16, formatTwo.size() - 16 - kChecksumSize));
    one.append(QCryptographicHash::hash(one, QCryptographicHash::Sha256));
    return one;
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
