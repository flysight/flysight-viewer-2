#include "logbookmanager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QDateTime>

#include "dataimporter.h"
#include "dataexporter.h"
#include "logbookcolumn.h"
#include "preferences/preferencesmanager.h"
#include "preferences/preferencekeys.h"

using namespace FlySight;

// ============================================================================
// Column serialization helpers (file-local)
// ============================================================================

namespace {

QString columnTypeToString(ColumnType type)
{
    switch (type) {
    case ColumnType::SessionAttribute:    return QStringLiteral("SessionAttribute");
    case ColumnType::MeasurementAtMarker: return QStringLiteral("MeasurementAtMarker");
    case ColumnType::Delta:               return QStringLiteral("Delta");
    }
    return QStringLiteral("SessionAttribute");
}

ColumnType columnTypeFromString(const QString &s)
{
    if (s == QStringLiteral("MeasurementAtMarker")) return ColumnType::MeasurementAtMarker;
    if (s == QStringLiteral("Delta"))               return ColumnType::Delta;
    return ColumnType::SessionAttribute;
}

QJsonObject columnToJson(const LogbookColumn &col)
{
    QJsonObject obj;
    obj[QStringLiteral("type")] = columnTypeToString(col.type);

    switch (col.type) {
    case ColumnType::SessionAttribute:
        obj[QStringLiteral("attributeKey")] = col.attributeKey;
        break;
    case ColumnType::MeasurementAtMarker:
        obj[QStringLiteral("sensorID")] = col.sensorID;
        obj[QStringLiteral("measurementID")] = col.measurementID;
        obj[QStringLiteral("measurementType")] = col.measurementType;
        obj[QStringLiteral("markerAttributeKey")] = col.markerAttributeKey;
        break;
    case ColumnType::Delta:
        obj[QStringLiteral("sensorID")] = col.sensorID;
        obj[QStringLiteral("measurementID")] = col.measurementID;
        obj[QStringLiteral("measurementType")] = col.measurementType;
        obj[QStringLiteral("markerAttributeKey")] = col.markerAttributeKey;
        obj[QStringLiteral("marker2AttributeKey")] = col.marker2AttributeKey;
        break;
    }

    return obj;
}

LogbookColumn columnFromJson(const QJsonObject &obj)
{
    LogbookColumn col;
    col.type = columnTypeFromString(obj[QStringLiteral("type")].toString());
    col.attributeKey = obj[QStringLiteral("attributeKey")].toString();
    col.sensorID = obj[QStringLiteral("sensorID")].toString();
    col.measurementID = obj[QStringLiteral("measurementID")].toString();
    col.measurementType = obj[QStringLiteral("measurementType")].toString();
    col.markerAttributeKey = obj[QStringLiteral("markerAttributeKey")].toString();
    col.marker2AttributeKey = obj[QStringLiteral("marker2AttributeKey")].toString();
    col.enabled = true;  // not persisted in index
    return col;
}

// Build a deterministic string key from column definition fields.
// Used for m_cachedValues internal storage.
QString columnDefinitionKey(const LogbookColumn &col)
{
    switch (col.type) {
    case ColumnType::SessionAttribute:
        return QStringLiteral("SessionAttribute|") + col.attributeKey;
    case ColumnType::MeasurementAtMarker:
        return QStringLiteral("MeasurementAtMarker|")
               + col.sensorID + QStringLiteral("|")
               + col.measurementID + QStringLiteral("|")
               + col.measurementType + QStringLiteral("|")
               + col.markerAttributeKey;
    case ColumnType::Delta:
        return QStringLiteral("Delta|")
               + col.sensorID + QStringLiteral("|")
               + col.measurementID + QStringLiteral("|")
               + col.measurementType + QStringLiteral("|")
               + col.markerAttributeKey + QStringLiteral("|")
               + col.marker2AttributeKey;
    }
    return QString();
}

} // anonymous namespace

// ============================================================================
// Singleton
// ============================================================================

LogbookManager& LogbookManager::instance()
{
    static LogbookManager manager;
    return manager;
}

LogbookManager::LogbookManager()
    : QObject(nullptr)
{
}

// ============================================================================
// Directory
// ============================================================================

QString LogbookManager::logbookDirectory() const
{
    const QString logbookFolder = PreferencesManager::instance()
        .getValue(PreferenceKeys::GeneralLogbookFolder).toString();
    return logbookFolder + QStringLiteral("/FlySight Viewer/logbook");
}

QString LogbookManager::sessionsDirectory() const
{
    const QString dir = logbookDirectory() + QStringLiteral("/sessions");
    QDir().mkpath(dir);
    return dir;
}

// ============================================================================
// Initialize
// ============================================================================

QList<SessionData> LogbookManager::initialize()
{
    // Attempt to read index.json
    const QString indexPath = logbookDirectory() + QStringLiteral("/index.json");
    QFile indexFile(indexPath);
    if (indexFile.exists() && indexFile.open(QIODevice::ReadOnly)) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(indexFile.readAll(), &parseError);
        indexFile.close();

        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonObject root = doc.object();

            if (root.contains(QStringLiteral("columns"))) {
                // --- New extended format ---
                const QJsonObject columnsObj = root[QStringLiteral("columns")].toObject();
                const QJsonObject sessionsObj = root[QStringLiteral("sessions")].toObject();

                // Build ephemeral UUID → definition key mapping
                QMap<QString, QString> uuidToDefKey;
                for (auto it = columnsObj.constBegin(); it != columnsObj.constEnd(); ++it) {
                    const LogbookColumn col = columnFromJson(it.value().toObject());
                    uuidToDefKey[it.key()] = columnDefinitionKey(col);
                }

                // Parse sessions
                for (auto it = sessionsObj.constBegin(); it != sessionsObj.constEnd(); ++it) {
                    const QString sessionId = it.key();
                    const QJsonObject entry = it.value().toObject();
                    const QString uuid = entry[QStringLiteral("uuid")].toString();
                    if (!uuid.isEmpty()) {
                        m_sessionIdToUuid[sessionId] = uuid;
                    }

                    // lastAccessed
                    if (entry.contains(QStringLiteral("lastAccessed"))) {
                        m_lastAccessed[sessionId] = entry[QStringLiteral("lastAccessed")].toDouble();
                    }

                    // values — translate UUID-keyed entries to definition-key-keyed
                    if (entry.contains(QStringLiteral("values"))) {
                        const QJsonObject valuesObj = entry[QStringLiteral("values")].toObject();
                        QMap<QString, QJsonValue> sessionValues;
                        for (auto vit = valuesObj.constBegin(); vit != valuesObj.constEnd(); ++vit) {
                            const QString defKey = uuidToDefKey.value(vit.key());
                            if (!defKey.isEmpty())
                                sessionValues[defKey] = vit.value();
                        }
                        if (!sessionValues.isEmpty())
                            m_cachedValues[sessionId] = sessionValues;
                    }
                }

                m_hasIndexData = true;
                return {};
            } else {
                // --- Legacy flat format ---
                for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
                    const QString sessionId = it.key();
                    const QJsonObject entry = it.value().toObject();
                    const QString uuid = entry[QStringLiteral("uuid")].toString();
                    if (!uuid.isEmpty()) {
                        m_sessionIdToUuid[sessionId] = uuid;
                    }
                }
                return {};
            }
        }
    }

    // Fallback: scan filenames only (no CSV parsing) for instant startup.
    // The column worker will parse each CSV in the background and rebuild the index.
    m_scannedUuids = scanSessionFilenames();
    m_deferredScan = true;
    return {};
}

// ============================================================================
// Reset
// ============================================================================

void LogbookManager::reset()
{
    // In-memory state only; no file is touched.
    m_sessionIdToUuid.clear();
    m_lastAccessed.clear();
    m_cachedValues.clear();
    m_scannedUuids.clear();
    m_hasIndexData = false;
    m_deferredScan = false;
}

// ============================================================================
// Index data accessors
// ============================================================================

bool LogbookManager::hasIndexData() const
{
    return m_hasIndexData;
}

bool LogbookManager::hasDeferredScan() const
{
    return m_deferredScan;
}

const QStringList &LogbookManager::scannedUuids() const
{
    return m_scannedUuids;
}

const QMap<QString, double>& LogbookManager::lastAccessedMap() const
{
    return m_lastAccessed;
}

QMap<QString, QMap<int, QVariant>> LogbookManager::cachedColumnValues(
    const QVector<LogbookColumn> &liveColumns) const
{
    QMap<QString, QMap<int, QVariant>> result;

    // Build mapping: definition key → live column index
    QMap<QString, int> defKeyToLiveIndex;
    for (int i = 0; i < liveColumns.size(); ++i)
        defKeyToLiveIndex[columnDefinitionKey(liveColumns[i])] = i;

    // For each session, translate definition-key-keyed values to index-keyed
    for (auto sit = m_cachedValues.constBegin(); sit != m_cachedValues.constEnd(); ++sit) {
        const QString &sessionId = sit.key();
        const QMap<QString, QJsonValue> &sessionValues = sit.value();

        QMap<int, QVariant> columnValues;
        for (auto vit = sessionValues.constBegin(); vit != sessionValues.constEnd(); ++vit) {
            auto liveIt = defKeyToLiveIndex.constFind(vit.key());
            if (liveIt != defKeyToLiveIndex.constEnd()) {
                const QJsonValue &jv = vit.value();
                if (jv.isDouble()) {
                    columnValues[liveIt.value()] = QVariant(jv.toDouble());
                } else if (jv.isString()) {
                    columnValues[liveIt.value()] = QVariant(jv.toString());
                } else if (jv.isNull()) {
                    // Null means "computed but no value" — store invalid QVariant
                    columnValues[liveIt.value()] = QVariant();
                }
            }
        }
        result[sessionId] = columnValues;
    }

    // Ensure ALL known sessions appear in the result so that
    // populateFromIndex() creates stub rows for every session,
    // even when no cached column values matched the current columns.
    for (auto it = m_sessionIdToUuid.constBegin(); it != m_sessionIdToUuid.constEnd(); ++it) {
        if (!result.contains(it.key())) {
            result[it.key()] = QMap<int, QVariant>();
        }
    }

    return result;
}

// ============================================================================
// setCachedValues
// ============================================================================

void LogbookManager::setCachedValues(const QString &sessionId,
                                     const QMap<LogbookColumn, QVariant> &columnValues)
{
    QMap<QString, QJsonValue> converted;
    for (auto it = columnValues.constBegin(); it != columnValues.constEnd(); ++it) {
        const QString key = columnDefinitionKey(it.key());
        const QVariant &val = it.value();
        if (!val.isValid() || val.isNull()) {
            converted[key] = QJsonValue::Null;
        } else if (val.typeId() == QMetaType::Double || val.typeId() == QMetaType::Float
                   || val.typeId() == QMetaType::Int || val.typeId() == QMetaType::LongLong) {
            converted[key] = QJsonValue(val.toDouble());
        } else {
            converted[key] = QJsonValue(val.toString());
        }
    }
    m_cachedValues[sessionId] = converted;
}

const QMap<QString, QJsonValue> &LogbookManager::cachedValuesForSession(const QString &sessionId) const
{
    static const QMap<QString, QJsonValue> empty;
    auto it = m_cachedValues.constFind(sessionId);
    return (it != m_cachedValues.constEnd()) ? it.value() : empty;
}

QString LogbookManager::columnDefKey(const LogbookColumn &col)
{
    return columnDefinitionKey(col);
}

QVariant LogbookManager::jsonToVariant(const QJsonValue &jv)
{
    if (jv.isDouble())
        return QVariant(jv.toDouble());
    if (jv.isString())
        return QVariant(jv.toString());
    return QVariant();
}

// ============================================================================
// setLastAccessed
// ============================================================================

void LogbookManager::setLastAccessed(const QString &sessionId, double timestamp)
{
    m_lastAccessed[sessionId] = timestamp;
}

// ============================================================================
// loadSession
// ============================================================================

std::optional<SessionData> LogbookManager::loadSession(const QString &sessionId)
{
    if (!m_sessionIdToUuid.contains(sessionId)) {
        qWarning("LogbookManager::loadSession: unknown SESSION_ID '%s'", qPrintable(sessionId));
        return std::nullopt;
    }

    const QString uuid = m_sessionIdToUuid[sessionId];
    const QString filePath = sessionsDirectory()
        + QStringLiteral("/") + uuid + QStringLiteral(".csv");

    DataImporter importer;
    SessionData sessionData;
    if (!importer.readFile(filePath, sessionData)) {
        qWarning("LogbookManager::loadSession: failed to read '%s': %s",
                 qPrintable(filePath), qPrintable(importer.getLastError()));
        return std::nullopt;
    }

    // Backfill mass/area for sessions saved before per-session attributes existed
    if (!sessionData.hasAttribute(SessionKeys::JumperMass)) {
        sessionData.setAttribute(SessionKeys::JumperMass,
            PreferencesManager::instance().getValue(PreferenceKeys::AeroMass));
    }
    if (!sessionData.hasAttribute(SessionKeys::PlanformArea)) {
        sessionData.setAttribute(SessionKeys::PlanformArea,
            PreferencesManager::instance().getValue(PreferenceKeys::AeroArea));
    }

    // Backfill wind defaults for sessions saved before wind attributes existed
    if (!sessionData.hasAttribute(SessionKeys::WindN)) {
        sessionData.setAttribute(SessionKeys::WindN, 0.0);
    }
    if (!sessionData.hasAttribute(SessionKeys::WindE)) {
        sessionData.setAttribute(SessionKeys::WindE, 0.0);
    }

    return sessionData;
}

// ============================================================================
// Save Session
// ============================================================================

bool LogbookManager::saveSession(const SessionData& session)
{
    const QString sessionId = session.getAttribute(SessionKeys::SessionId).toString();
    if (sessionId.isEmpty()) {
        return false;
    }

    // Reuse existing UUID or generate a new one
    QString uuid;
    if (m_sessionIdToUuid.contains(sessionId)) {
        uuid = m_sessionIdToUuid[sessionId];
    } else {
        uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    const QString filePath = sessionsDirectory()
        + QStringLiteral("/") + uuid + QStringLiteral(".csv");

    if (!DataExporter::exportSession(filePath, session)) {
        return false;
    }

    m_sessionIdToUuid[sessionId] = uuid;

    // Set lastAccessed for newly imported sessions
    if (!m_lastAccessed.contains(sessionId)) {
        m_lastAccessed[sessionId] = static_cast<double>(
            QDateTime::currentDateTimeUtc().toSecsSinceEpoch());
    }

    return true;
}

// ============================================================================
// Load All Sessions
// ============================================================================

QList<SessionData> LogbookManager::loadAllSessions()
{
    return scanSessionFiles();
}

// ============================================================================
// Scan Session Files
// ============================================================================

QList<SessionData> LogbookManager::scanSessionFiles()
{
    QList<SessionData> result;
    const QString dir = sessionsDirectory();
    const QDir sessDir(dir);
    const QStringList csvFiles = sessDir.entryList(
        QStringList() << QStringLiteral("*.csv"),
        QDir::Files, QDir::Name);

    for (const QString& filename : csvFiles) {
        const QString filePath = sessDir.absoluteFilePath(filename);
        DataImporter importer;
        SessionData sessionData;
        if (importer.readFile(filePath, sessionData)) {
            const QString sessionId = sessionData.getAttribute(SessionKeys::SessionId).toString();
            const QString uuid = QFileInfo(filename).completeBaseName();
            if (!sessionId.isEmpty()) {
                m_sessionIdToUuid[sessionId] = uuid;
            }
            result.append(sessionData);
        }
    }

    return result;
}

// ============================================================================
// Scan Session Filenames (no CSV parsing)
// ============================================================================

QStringList LogbookManager::scanSessionFilenames()
{
    QStringList uuids;
    const QString dir = sessionsDirectory();
    const QDir sessDir(dir);
    const QStringList csvFiles = sessDir.entryList(
        QStringList() << QStringLiteral("*.csv"),
        QDir::Files, QDir::Name);

    for (const QString &filename : csvFiles) {
        const QString uuid = QFileInfo(filename).completeBaseName();
        m_sessionIdToUuid[uuid] = uuid;  // identity mapping
        uuids.append(uuid);
    }
    return uuids;
}

// ============================================================================
// Remap Session ID
// ============================================================================

bool LogbookManager::remapSessionId(const QString &oldId, const QString &newId)
{
    if (!m_sessionIdToUuid.contains(oldId))
        return false;
    if (oldId == newId)
        return true;
    if (m_sessionIdToUuid.contains(newId))
        return false;

    const QString uuid = m_sessionIdToUuid.take(oldId);
    m_sessionIdToUuid[newId] = uuid;

    if (m_lastAccessed.contains(oldId)) {
        m_lastAccessed[newId] = m_lastAccessed.take(oldId);
    }
    if (m_cachedValues.contains(oldId)) {
        m_cachedValues[newId] = m_cachedValues.take(oldId);
    }

    return true;
}

// ============================================================================
// Remove Session
// ============================================================================

bool LogbookManager::removeSession(const QString& sessionId)
{
    if (!m_sessionIdToUuid.contains(sessionId)) {
        return false;
    }

    const QString uuid = m_sessionIdToUuid[sessionId];
    const QString filePath = sessionsDirectory()
        + QStringLiteral("/") + uuid + QStringLiteral(".csv");

    if (!QFile::remove(filePath)) {
        qWarning("LogbookManager: failed to remove %s", qPrintable(filePath));
        return false;
    }

    m_sessionIdToUuid.remove(sessionId);
    m_lastAccessed.remove(sessionId);
    m_cachedValues.remove(sessionId);
    return true;
}

// ============================================================================
// Flush Index
// ============================================================================

void LogbookManager::flushIndex()
{
    // 1. Get current enabled columns
    const QVector<LogbookColumn> enabledCols = LogbookColumnStore::instance().enabledColumns();

    // 2. Generate ephemeral UUIDs for each enabled column, build "columns" object
    QJsonObject columnsObj;
    // Mapping: columnDefinitionKey -> ephemeral UUID (for linking to values)
    QMap<QString, QString> defKeyToEphemeralUuid;
    for (const LogbookColumn &col : enabledCols) {
        const QString ephemeralUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString defKey = columnDefinitionKey(col);
        defKeyToEphemeralUuid[defKey] = ephemeralUuid;
        columnsObj[ephemeralUuid] = columnToJson(col);
    }

    // 3. Build "sessions" object
    QJsonObject sessionsObj;
    for (auto it = m_sessionIdToUuid.constBegin(); it != m_sessionIdToUuid.constEnd(); ++it) {
        const QString &sessionId = it.key();

        QJsonObject entry;
        entry[QStringLiteral("uuid")] = it.value();
        entry[QStringLiteral("lastAccessed")] = m_lastAccessed.value(sessionId, 0.0);

        // Build values: map cached values (keyed by definition key) to ephemeral UUIDs
        QJsonObject valuesObj;
        if (m_cachedValues.contains(sessionId)) {
            const QMap<QString, QJsonValue> &cached = m_cachedValues[sessionId];
            for (auto cit = cached.constBegin(); cit != cached.constEnd(); ++cit) {
                const QString &defKey = cit.key();
                if (defKeyToEphemeralUuid.contains(defKey)) {
                    valuesObj[defKeyToEphemeralUuid[defKey]] = cit.value();
                }
            }
        }
        entry[QStringLiteral("values")] = valuesObj;

        sessionsObj[sessionId] = entry;
    }

    // 4. Write root object
    QJsonObject root;
    root[QStringLiteral("columns")] = columnsObj;
    root[QStringLiteral("sessions")] = sessionsObj;

    const QString filePath = logbookDirectory() + QStringLiteral("/index.json");

    // Atomic write via QSaveFile
    QSaveFile saveFile(filePath);
    if (!saveFile.open(QIODevice::WriteOnly)) {
        qWarning("LogbookManager: failed to open %s for writing", qPrintable(filePath));
        return;
    }
    saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!saveFile.commit()) {
        qWarning("LogbookManager: failed to commit %s", qPrintable(filePath));
    }
}
