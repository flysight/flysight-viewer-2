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

#include "calculations/builtincalculations.h"
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

// Key of m_cachedValues' per-session maps.
QString columnDefinitionKey(const LogbookColumn &col)
{
    return logbookColumnDefinitionKey(col);
}

// Cached value -> JSON: null = "computed, no value"; numbers stay numbers.
QJsonValue variantToJson(const QVariant &val)
{
    if (!val.isValid() || val.isNull())
        return QJsonValue::Null;
    if (val.typeId() == QMetaType::Double || val.typeId() == QMetaType::Float
        || val.typeId() == QMetaType::Int || val.typeId() == QMetaType::LongLong)
        return QJsonValue(val.toDouble());
    return QJsonValue(val.toString());
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

void LogbookManager::initialize()
{
    // Calculation records whose session file does not exist. The rule depends
    // only on the files on disk, so it runs first and identically for every
    // index branch below. A record whose csv exists but that the index does
    // not know stays: orphan adoption makes that csv a session.
    removeStrayCalculationRecords();

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

                // Cached column values are trusted only when they were computed
                // by compatible calculation code in the same calculation
                // environment. A released index has neither field (toInt() of
                // a missing or non-numeric value is 0, which is never a marker).
                const QString currentEnvironment = calculationEnvironmentFingerprint();
                const bool valid =
                    root[QStringLiteral("calculationCompatibility")].toInt() == CalculationCompatibilityVersion
                    && root[QStringLiteral("calculationEnvironment")].toString() == currentEnvironment;
                if (!valid) {
                    // Session files are not touched; the values are recomputed
                    // lazily and the index is rewritten with the current marker.
                    m_discardedOnLoad = true;
                    m_indexNeedsFlush = true;
                }

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
                    if (valid && entry.contains(QStringLiteral("values"))) {
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

                // Orphan adoption: a session file the index does not reference
                // (a new session whose CSV was committed but whose index entry
                // was not) becomes an identity stub, exactly like the entries
                // of the filename scan; the model remaps it to the real
                // SESSION_ID when it first loads the file.
                QSet<QString> knownUuids;
                for (auto it = m_sessionIdToUuid.constBegin(); it != m_sessionIdToUuid.constEnd(); ++it)
                    knownUuids.insert(it.value());
                const QStringList stems = sessionFileStems();
                for (const QString &stem : stems) {
                    if (knownUuids.contains(stem) || m_sessionIdToUuid.contains(stem))
                        continue;
                    m_sessionIdToUuid[stem] = stem;
                    m_indexNeedsFlush = true;
                }

                m_cacheEnvironment = currentEnvironment;
                m_hasIndexData = true;
                return;
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
                m_cacheEnvironment = calculationEnvironmentFingerprint();
                return;
            }
        }
    }

    // Fallback: scan filenames only (no CSV parsing) for instant startup.
    // The column worker will parse each CSV in the background and rebuild the index.
    m_scannedUuids = scanSessionFilenames();
    m_deferredScan = true;
    m_cacheEnvironment = calculationEnvironmentFingerprint();
}

// ============================================================================
// Reset
// ============================================================================

void LogbookManager::reset()
{
    // In-memory state only; no file is touched.
    m_sessionIdToUuid.clear();
    m_reservedStems.clear();    // a restart forgets unsaved sessions, like a process exit
    m_lastAccessed.clear();
    m_cachedValues.clear();
    m_scannedUuids.clear();
    m_hasIndexData = false;
    m_deferredScan = false;
    m_cacheEnvironment.clear();
    m_discardedOnLoad = false;
    m_indexNeedsFlush = false;
    m_unsavedColumns.clear();
    m_unsavedAll.clear();
    m_needsFlushBeforeSave.clear();
    m_lastSaveError.clear();
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

    // Build mapping: definition key → live column indices. Live columns that
    // share a definition share the one stored value, and each of them gets it:
    // a row that came back with fewer values than columns would be taken for
    // an uncomputed one and loaded from disk on every start.
    QMap<QString, QVector<int>> defKeyToLiveIndices;
    for (int i = 0; i < liveColumns.size(); ++i)
        defKeyToLiveIndices[columnDefinitionKey(liveColumns[i])].append(i);

    // For each session, translate definition-key-keyed values to index-keyed
    for (auto sit = m_cachedValues.constBegin(); sit != m_cachedValues.constEnd(); ++sit) {
        const QString &sessionId = sit.key();
        const QMap<QString, QJsonValue> &sessionValues = sit.value();

        QMap<int, QVariant> columnValues;
        for (auto vit = sessionValues.constBegin(); vit != sessionValues.constEnd(); ++vit) {
            auto liveIt = defKeyToLiveIndices.constFind(vit.key());
            if (liveIt == defKeyToLiveIndices.constEnd())
                continue;
            const QJsonValue &jv = vit.value();
            // Null means "computed but no value" — store invalid QVariant
            if (!jv.isDouble() && !jv.isString() && !jv.isNull())
                continue;
            const QVariant value = jsonToVariant(jv);
            for (int liveIndex : liveIt.value())
                columnValues[liveIndex] = value;
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
    for (auto it = columnValues.constBegin(); it != columnValues.constEnd(); ++it)
        converted[columnDefinitionKey(it.key())] = variantToJson(it.value());
    m_cachedValues[sessionId] = converted;
    m_indexNeedsFlush = true;
}

void LogbookManager::updateCachedValues(const QString &sessionId,
                                        const QMap<LogbookColumn, QVariant> &columnValues)
{
    if (columnValues.isEmpty())
        return;

    QMap<QString, QJsonValue> &cached = m_cachedValues[sessionId];
    for (auto it = columnValues.constBegin(); it != columnValues.constEnd(); ++it)
        cached[columnDefinitionKey(it.key())] = variantToJson(it.value());
    m_indexNeedsFlush = true;
}

// ============================================================================
// Cache validity
// ============================================================================

QString LogbookManager::cacheEnvironment() const
{
    return m_cacheEnvironment;
}

bool LogbookManager::cachedValuesDiscardedOnLoad() const
{
    return m_discardedOnLoad;
}

bool LogbookManager::indexNeedsFlush() const
{
    return m_indexNeedsFlush;
}

void LogbookManager::discardCachedValues()
{
    m_cachedValues.clear();
    m_cacheEnvironment = calculationEnvironmentFingerprint();
    m_indexNeedsFlush = true;
}

// ============================================================================
// Unsaved-column tracking
// ============================================================================

void LogbookManager::markColumnsUnsaved(const QString &sessionId, const QVector<LogbookColumn> &columns)
{
    if (columns.isEmpty())
        return;

    QSet<QString> &unsaved = m_unsavedColumns[sessionId];
    auto cachedIt = m_cachedValues.find(sessionId);
    for (const LogbookColumn &col : columns) {
        const QString key = columnDefinitionKey(col);
        if (cachedIt != m_cachedValues.end())
            cachedIt->remove(key);
        unsaved.insert(key);
    }

    // Only a session the on-disk index can know about needs the pre-save flush
    if (m_sessionIdToUuid.contains(sessionId))
        m_needsFlushBeforeSave.insert(sessionId);
    m_indexNeedsFlush = true;
}

void LogbookManager::markSessionUnsaved(const QString &sessionId)
{
    m_cachedValues.remove(sessionId);
    m_unsavedAll.insert(sessionId);

    if (m_sessionIdToUuid.contains(sessionId))
        m_needsFlushBeforeSave.insert(sessionId);
    m_indexNeedsFlush = true;
}

void LogbookManager::dropCachedValuesExcept(const QString &sessionId, const QVector<LogbookColumn> &columns)
{
    auto cachedIt = m_cachedValues.find(sessionId);
    if (cachedIt == m_cachedValues.end())
        return;

    QSet<QString> keep;
    for (const LogbookColumn &col : columns)
        keep.insert(columnDefinitionKey(col));

    for (auto it = cachedIt->begin(); it != cachedIt->end();) {
        if (keep.contains(it.key()))
            ++it;
        else
            it = cachedIt->erase(it);
    }
}

bool LogbookManager::hasUnsavedColumns(const QString &sessionId) const
{
    return m_unsavedAll.contains(sessionId) || !m_unsavedColumns.value(sessionId).isEmpty();
}

QString LogbookManager::lastSaveError() const
{
    return m_lastSaveError;
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

std::optional<SessionData> LogbookManager::loadSessionRaw(const QString &sessionId, QString *error)
{
    if (error)
        error->clear();

    if (!m_sessionIdToUuid.contains(sessionId)) {
        qWarning("LogbookManager::loadSession: unknown SESSION_ID '%s'", qPrintable(sessionId));
        if (error)
            *error = QStringLiteral("not in the logbook index");
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
        if (error)
            *error = importer.getLastError();
        return std::nullopt;
    }

    return sessionData;
}

void LogbookManager::applyLegacyBackfill(SessionData &session)
{
    // Backfill mass/area for sessions saved before per-session attributes existed
    if (!session.hasAttribute(SessionKeys::JumperMass)) {
        session.setAttribute(SessionKeys::JumperMass,
            PreferencesManager::instance().getValue(PreferenceKeys::AeroMass));
    }
    if (!session.hasAttribute(SessionKeys::PlanformArea)) {
        session.setAttribute(SessionKeys::PlanformArea,
            PreferencesManager::instance().getValue(PreferenceKeys::AeroArea));
    }

    // Backfill wind defaults for sessions saved before wind attributes existed
    if (!session.hasAttribute(SessionKeys::WindN)) {
        session.setAttribute(SessionKeys::WindN, 0.0);
    }
    if (!session.hasAttribute(SessionKeys::WindE)) {
        session.setAttribute(SessionKeys::WindE, 0.0);
    }
}

std::optional<SessionData> LogbookManager::loadSession(const QString &sessionId)
{
    std::optional<SessionData> session = loadSessionRaw(sessionId);
    if (session.has_value())
        applyLegacyBackfill(*session);
    return session;
}

// ============================================================================
// Identity entries
// ============================================================================

QString LogbookManager::reserveSessionFile(const QString &sessionId)
{
    if (sessionId.isEmpty())
        return QString();
    const auto known = m_sessionIdToUuid.constFind(sessionId);
    if (known != m_sessionIdToUuid.constEnd())
        return known.value();
    // Kept apart from m_sessionIdToUuid, which means "has a session file"
    // (flushIndex() lists it; loads and the save ordering rely on it).
    QString &stem = m_reservedStems[sessionId];
    if (stem.isEmpty())
        stem = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return stem;
}

bool LogbookManager::isIdentityEntry(const QString &sessionId) const
{
    auto it = m_sessionIdToUuid.constFind(sessionId);
    return it != m_sessionIdToUuid.constEnd() && it.value() == sessionId;
}

std::optional<QString> LogbookManager::peekSessionId(const QString &sessionId) const
{
    auto it = m_sessionIdToUuid.constFind(sessionId);
    if (it == m_sessionIdToUuid.constEnd())
        return std::nullopt;

    const QString filePath = sessionsDirectory()
        + QStringLiteral("/") + it.value() + QStringLiteral(".csv");
    return DataImporter::peekHeaderAttribute(filePath, QLatin1String(SessionKeys::SessionId));
}

// ============================================================================
// Save Session
// ============================================================================

// Save ordering (see the class comment). D = on-disk index values of the
// columns the pending change can affect, F = the session file:
//
//   crash point                                   D        F     consistent because
//   after the edit, before any write              old      old   nothing changed on disk
//   after step b (pre-save index flush)           absent   old   absent values are recomputed from F
//   after step c (session file committed)         absent   new   same
//   after a ColumnTask / SaveTask flush while
//     the row is still unsaved                    absent   old   same (flushIndex skips unsaved columns)
//   after the post-save flush                     new      new   computed from the state that was saved
//   step b fails (index not writable)             old      old   nothing changed on disk; the marks stay
//   step c fails (session file not written)       absent   old   the marks stay: every later flush still omits
//                                                                the columns, until a save succeeds
//
// A failed save is therefore the same on-disk state as a crash at that point,
// except that the process lives on: the caller (SessionModel) keeps the session
// in memory, dirty, and retries - it never treats the row as saved.
//
// Columns that were NOT marked keep their values throughout: by the static
// dependency closure they cannot depend on the change, so one value is right
// for both the old and the new file.
bool LogbookManager::saveSession(const SessionData& session)
{
    m_lastSaveError.clear();

    // a. Stored attribute only: a save never creates or consults an engine.
    const QString sessionId = session.storedAttribute(SessionKeys::SessionId).toString();
    if (sessionId.isEmpty()) {
        m_lastSaveError = QStringLiteral("The session has no SESSION_ID");
        return false;
    }

    // b. The on-disk index may still hold a value for a column this save can
    //    change: remove it first.
    if (m_needsFlushBeforeSave.contains(sessionId)) {
        if (!flushIndex()) {
            m_lastSaveError = QStringLiteral("index.json could not be written");
            qWarning("LogbookManager: session %s not saved: %s",
                     qPrintable(sessionId), qPrintable(m_lastSaveError));
            return false;
        }
    }

    // Calculation records are neither read nor written here: the session
    // file's bytes never depend on them.

    // Reuse the existing UUID, else the one reserved at import (records may
    // already be written under it), else generate a new one
    QString uuid;
    if (m_sessionIdToUuid.contains(sessionId)) {
        uuid = m_sessionIdToUuid[sessionId];
    } else if (m_reservedStems.contains(sessionId)) {
        uuid = m_reservedStems.value(sessionId);
    } else {
        uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    const QString filePath = sessionsDirectory()
        + QStringLiteral("/") + uuid + QStringLiteral(".csv");

    // c. The session file. On failure the previous file is intact and the
    //    unsaved marks stay: the index keeps omitting the affected columns.
    if (!DataExporter::exportSession(filePath, session, &m_lastSaveError)) {
        qWarning("LogbookManager: session %s not saved: %s",
                 qPrintable(sessionId), qPrintable(m_lastSaveError));
        return false;
    }

    // d. Memory and file agree again; the next flush may publish the values.
    //    A reservation becomes the index entry (on failure it stays).
    m_sessionIdToUuid[sessionId] = uuid;
    m_reservedStems.remove(sessionId);
    m_unsavedColumns.remove(sessionId);
    m_unsavedAll.remove(sessionId);
    m_indexNeedsFlush = true;

    // Set lastAccessed for newly imported sessions
    if (!m_lastAccessed.contains(sessionId)) {
        m_lastAccessed[sessionId] = static_cast<double>(
            QDateTime::currentDateTimeUtc().toSecsSinceEpoch());
    }

    return true;
}

// ============================================================================
// Scan Session Filenames (no CSV parsing)
// ============================================================================

QStringList LogbookManager::sessionFileStems() const
{
    QStringList stems;
    const QDir sessDir(sessionsDirectory());
    const QStringList csvFiles = sessDir.entryList(
        QStringList() << QStringLiteral("*.csv"),
        QDir::Files, QDir::Name);

    for (const QString &filename : csvFiles)
        stems.append(QFileInfo(filename).completeBaseName());
    return stems;
}

QStringList LogbookManager::scanSessionFilenames()
{
    const QStringList uuids = sessionFileStems();
    for (const QString &uuid : uuids)
        m_sessionIdToUuid[uuid] = uuid;  // identity mapping
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
    if (m_sessionIdToUuid.contains(newId) || m_reservedStems.contains(newId))
        return false;

    // File stems never change (the uuid moves to the new id), so calculation
    // records need no move.
    const QString uuid = m_sessionIdToUuid.take(oldId);
    m_sessionIdToUuid[newId] = uuid;

    if (m_lastAccessed.contains(oldId)) {
        m_lastAccessed[newId] = m_lastAccessed.take(oldId);
    }
    if (m_cachedValues.contains(oldId)) {
        m_cachedValues[newId] = m_cachedValues.take(oldId);
    }

    // The unsaved marks travel with the id
    if (m_unsavedColumns.contains(oldId)) {
        m_unsavedColumns[newId] = m_unsavedColumns.take(oldId);
    }
    if (m_unsavedAll.remove(oldId)) {
        m_unsavedAll.insert(newId);
    }
    if (m_needsFlushBeforeSave.remove(oldId)) {
        m_needsFlushBeforeSave.insert(newId);
    }
    m_indexNeedsFlush = true;

    return true;
}

// ============================================================================
// Remove Session
// ============================================================================

bool LogbookManager::removeSession(const QString& sessionId)
{
    if (!m_sessionIdToUuid.contains(sessionId)) {
        // Never saved: no session file, only records under the reserved stem.
        const auto reserved = m_reservedStems.constFind(sessionId);
        if (reserved == m_reservedStems.constEnd())
            return false;
        const QString stem = reserved.value();
        m_reservedStems.remove(sessionId);
        if (!removeCalculationRecordsForStem(stem)) {
            qWarning("LogbookManager: calculation records of %s not all removed; "
                     "the next start removes them", qPrintable(sessionId));
        }
        return true;
    }

    const QString uuid = m_sessionIdToUuid[sessionId];
    const QString filePath = sessionsDirectory()
        + QStringLiteral("/") + uuid + QStringLiteral(".csv");

    // The session file first: if it stays, its records stay with it.
    if (!QFile::remove(filePath)) {
        qWarning("LogbookManager: failed to remove %s", qPrintable(filePath));
        return false;
    }

    // Then its records, while the stem is still known. The session is gone
    // whatever happens here; a record left behind is a stray that the next
    // start's initialize() removes.
    if (!removeCalculationRecordsForStem(uuid)) {
        qWarning("LogbookManager: calculation records of %s not all removed; "
                 "the next start removes them", qPrintable(sessionId));
    }

    m_sessionIdToUuid.remove(sessionId);
    m_reservedStems.remove(sessionId);
    m_lastAccessed.remove(sessionId);
    m_cachedValues.remove(sessionId);
    m_unsavedColumns.remove(sessionId);
    m_unsavedAll.remove(sessionId);
    m_needsFlushBeforeSave.remove(sessionId);
    m_indexNeedsFlush = true;
    return true;
}

// ============================================================================
// Calculation records
// ============================================================================

QString LogbookManager::recordStem(const QString &sessionId) const
{
    // As loadSessionRaw(): the stem is the entry's uuid (the id itself for an
    // identity entry). A session not saved yet has the stem reserved for it.
    const auto it = m_sessionIdToUuid.constFind(sessionId);
    if (it != m_sessionIdToUuid.constEnd())
        return it.value();
    return m_reservedStems.value(sessionId);
}

QString LogbookManager::calculationRecordPath(const QString &stem, const QString &calculationId) const
{
    return sessionsDirectory() + QLatin1Char('/') + recordFileName(stem, calculationId);
}

QStringList LogbookManager::calculationRecordFileNames() const
{
    // Never a wildcard built from a stem: an identity stem is an arbitrary
    // file name and may contain '[', '*' or '?'.
    QStringList names = QDir(sessionsDirectory()).entryList(
        QStringList() << (QStringLiteral("*.") + calculationRecordExtension()),
        QDir::Files, QDir::Name);
    names.sort();
    return names;
}

QStringList LogbookManager::calculationRecordIdsForStem(const QString &stem) const
{
    QStringList ids;
    const QStringList names = calculationRecordFileNames();
    for (const QString &name : names) {
        const auto parsed = parseRecordFileName(name);
        if (parsed && parsed->first == stem)
            ids.append(parsed->second);
    }
    ids.sort();
    ids.removeDuplicates();     // the extension in two spellings on a case-sensitive file system
    return ids;
}

bool LogbookManager::removeCalculationRecordsForStem(const QString &stem)
{
    const QString dir = sessionsDirectory();
    bool allRemoved = true;
    const QStringList names = calculationRecordFileNames();
    for (const QString &name : names) {
        const auto parsed = parseRecordFileName(name);
        if (!parsed || parsed->first != stem)
            continue;
        const QString path = dir + QLatin1Char('/') + name;
        if (!QFile::remove(path)) {
            qWarning("LogbookManager: failed to remove calculation record %s", qPrintable(path));
            allRemoved = false;
        }
    }
    return allRemoved;
}

int LogbookManager::removeStrayCalculationRecords()
{
    const QStringList stemList = sessionFileStems();
    const QSet<QString> stems(stemList.cbegin(), stemList.cend());
    const QString dir = sessionsDirectory();

    int removed = 0;
    const QStringList names = calculationRecordFileNames();
    for (const QString &name : names) {
        const auto parsed = parseRecordFileName(name);
        if (parsed && stems.contains(parsed->first))
            continue;
        const QString path = dir + QLatin1Char('/') + name;
        if (QFile::remove(path))
            ++removed;
        else
            qWarning("LogbookManager: failed to remove stray calculation record %s", qPrintable(path));
    }
    return removed;
}

bool LogbookManager::writeCalculationRecord(const QString &sessionId, const CalculationRecord &record,
                                            QString *error)
{
    if (error)
        error->clear();

    const QString &calculationId = record.result.calculationId;
    const auto fail = [&](const QString &text) {
        qWarning("LogbookManager: calculation record %s of %s not written: %s",
                 qPrintable(calculationId), qPrintable(sessionId), qPrintable(text));
        if (error)
            *error = text;
        return false;
    };

    const QString stem = recordStem(sessionId);
    if (stem.isEmpty())
        return fail(QStringLiteral("not in the logbook index"));

    // Encode first: a record the format refuses never opens a file, so the
    // previous record is untouched.
    QString encodeError;
    const std::optional<QByteArray> bytes = encodeCalculationRecord(record, &encodeError);
    if (!bytes)
        return fail(encodeError);

    // As DataExporter's session write: a temporary file renamed over the
    // record at commit (no direct-write fallback), so a failure at any step
    // leaves the previous record intact. Records are not referenced from
    // index.json, so no index flush is involved. Nothing is retried.
    const QString path = calculationRecordPath(stem, calculationId);
    const auto writeError = [&path](const QSaveFile &file) {
        return QStringLiteral("Couldn't write file '%1': %2").arg(path, file.errorString());
    };

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(writeError(file));

    file.write(*bytes);
    if (file.error() != QFileDevice::NoError) {
        const QString text = writeError(file);
        file.cancelWriting();
        return fail(text);
    }

    if (!file.commit())
        return fail(writeError(file));
    return true;
}

CalculationRecordRead LogbookManager::readCalculationRecord(const QString &sessionId,
                                                            const QString &calculationId) const
{
    CalculationRecordRead read;

    const QString stem = recordStem(sessionId);
    if (stem.isEmpty()) {
        read.error = QStringLiteral("not in the logbook index");
        return read;    // Missing
    }

    QFile file(calculationRecordPath(stem, calculationId));
    if (!file.exists())
        return read;    // Missing, no error

    if (!file.open(QIODevice::ReadOnly)) {
        read.status = CalculationRecordStatus::Unreadable;
        read.error = file.errorString();
        return read;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        read.status = CalculationRecordStatus::Unreadable;
        read.error = file.errorString();
        return read;
    }

    // Deciding what is stale (and deleting it) is the caller's job.
    CalculationRecord record;
    read.status = decodeCalculationRecord(bytes, &record, &read.error);
    if (read.status != CalculationRecordStatus::Ok)
        return read;

    if (record.result.calculationId != calculationId) {
        read.status = CalculationRecordStatus::Corrupt;
        read.error = QStringLiteral("the record belongs to calculation '%1'").arg(record.result.calculationId);
        return read;
    }

    read.record = std::move(record);
    return read;
}

QStringList LogbookManager::calculationRecordIds(const QString &sessionId) const
{
    const QString stem = recordStem(sessionId);
    if (stem.isEmpty())
        return {};
    return calculationRecordIdsForStem(stem);
}

bool LogbookManager::removeCalculationRecord(const QString &sessionId, const QString &calculationId)
{
    const QString stem = recordStem(sessionId);
    if (stem.isEmpty())
        return false;

    const QString path = calculationRecordPath(stem, calculationId);
    if (!QFileInfo::exists(path))
        return true;
    if (!QFile::remove(path)) {
        qWarning("LogbookManager: failed to remove calculation record %s", qPrintable(path));
        return false;
    }
    return true;
}

bool LogbookManager::removeCalculationRecords(const QString &sessionId)
{
    const QString stem = recordStem(sessionId);
    if (stem.isEmpty())
        return false;
    return removeCalculationRecordsForStem(stem);
}

// ============================================================================
// Flush Index
// ============================================================================

bool LogbookManager::flushIndex()
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

        // Build values: map cached values (keyed by definition key) to ephemeral UUIDs.
        // Unsaved columns are omitted even when memory already holds their new
        // value: that value belongs to a session file that is not on disk yet.
        QJsonObject valuesObj;
        if (m_cachedValues.contains(sessionId) && !m_unsavedAll.contains(sessionId)) {
            const QMap<QString, QJsonValue> &cached = m_cachedValues[sessionId];
            const QSet<QString> unsaved = m_unsavedColumns.value(sessionId);
            for (auto cit = cached.constBegin(); cit != cached.constEnd(); ++cit) {
                const QString &defKey = cit.key();
                if (unsaved.contains(defKey))
                    continue;
                if (defKeyToEphemeralUuid.contains(defKey)) {
                    valuesObj[defKeyToEphemeralUuid[defKey]] = cit.value();
                }
            }
        }
        entry[QStringLiteral("values")] = valuesObj;

        sessionsObj[sessionId] = entry;
    }

    // 4. Write root object
    // The environment is the one the in-memory values were computed under,
    // NOT a freshly computed fingerprint: computing it here would bless stale
    // values after an environment change nobody told the manager about.
    QJsonObject root;
    root[QStringLiteral("calculationCompatibility")] = CalculationCompatibilityVersion;
    root[QStringLiteral("calculationEnvironment")] = m_cacheEnvironment;
    root[QStringLiteral("columns")] = columnsObj;
    root[QStringLiteral("sessions")] = sessionsObj;

    const QString filePath = logbookDirectory() + QStringLiteral("/index.json");

    // Atomic write via QSaveFile
    QSaveFile saveFile(filePath);
    if (!saveFile.open(QIODevice::WriteOnly)) {
        qWarning("LogbookManager: failed to open %s for writing", qPrintable(filePath));
        return false;
    }
    saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!saveFile.commit()) {
        qWarning("LogbookManager: failed to commit %s", qPrintable(filePath));
        return false;
    }

    // The on-disk index now holds no value for any unsaved column.
    m_needsFlushBeforeSave.clear();
    m_indexNeedsFlush = false;
    return true;
}
