#ifndef LOGBOOKMANAGER_H
#define LOGBOOKMANAGER_H

#include <optional>

#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVector>

#include "logbookcolumn.h"
#include "sessiondata.h"

namespace FlySight {

class LogbookManager : public QObject {
    Q_OBJECT

public:
    static LogbookManager& instance();

    // Creates sessions directory if missing, loads index.json or scans *.csv files.
    // Returns parsed sessions if fallback CSV scan was used, empty list otherwise.
    QList<SessionData> initialize();

    // Drops all in-memory index state so the next initialize() re-reads the current
    // logbook folder. Used by tests to simulate an application restart.
    void reset();

    // Writes a session to disk as a UUID-based .csv file; returns true on success
    bool saveSession(const SessionData& session);

    // Reads all *.csv files from the sessions directory, returns parsed sessions
    QList<SessionData> loadAllSessions();

    // Deletes the .csv file for the given SESSION_ID; returns true on success
    bool removeSession(const QString& sessionId);

    // Writes index.json mapping SESSION_IDs to UUIDs
    void flushIndex();

    // Stores cached column values for a session, to be written on next flushIndex().
    // columnValues maps a LogbookColumn (matched by definition) to its QVariant value.
    void setCachedValues(const QString &sessionId,
                         const QMap<LogbookColumn, QVariant> &columnValues);

    // Loads and parses the CSV for a single session. Returns std::nullopt on failure.
    std::optional<SessionData> loadSession(const QString &sessionId);

    // Updates the lastAccessed timestamp for a session (persisted on next flushIndex())
    void setLastAccessed(const QString &sessionId, double timestamp);

    // --- Accessors for index data (populated after initialize()) ---

    // Returns true if the index contained column definitions (new format)
    bool hasIndexData() const;

    // Returns true if a filename-only scan was used (no CSV parsing)
    bool hasDeferredScan() const;

    // Returns UUIDs found by the deferred filename scan
    const QStringList &scannedUuids() const;

    // Atomically replaces a temporary UUID-based session ID with the real SESSION_ID.
    // Updates m_sessionIdToUuid, m_lastAccessed, and m_cachedValues.
    // Returns false if oldId not found or newId already exists.
    bool remapSessionId(const QString &oldId, const QString &newId);

    // Returns the lastAccessed map (SESSION_ID -> epoch seconds)
    const QMap<QString, double>& lastAccessedMap() const;

    // Matches index columns to liveColumns by definition and returns
    // SESSION_ID -> { liveColumnIndex -> QVariant value }
    QMap<QString, QMap<int, QVariant>> cachedColumnValues(
        const QVector<LogbookColumn> &liveColumns) const;

    // Returns the raw cached values map for a single session (defKey -> QJsonValue)
    const QMap<QString, QJsonValue> &cachedValuesForSession(const QString &sessionId) const;

    // Returns the definition key for a LogbookColumn
    static QString columnDefKey(const LogbookColumn &col);

    // Converts a QJsonValue to QVariant (double, string, or invalid for null)
    static QVariant jsonToVariant(const QJsonValue &jv);

private:
    LogbookManager();
    Q_DISABLE_COPY(LogbookManager)

    // Returns the full path to the logbook directory
    QString logbookDirectory() const;

    // Returns the full path to the sessions directory
    QString sessionsDirectory() const;

    // Scans *.csv files in the sessions directory, parses each, and populates m_sessionIdToUuid
    QList<SessionData> scanSessionFiles();

    // Scans *.csv filenames only (no parsing); populates m_sessionIdToUuid with identity mappings
    QStringList scanSessionFilenames();

    // Maps SESSION_ID strings to UUID filename stems (without extension)
    QMap<QString, QString> m_sessionIdToUuid;

    // Maps SESSION_ID -> epoch seconds (last accessed time)
    QMap<QString, double> m_lastAccessed;

    // Maps SESSION_ID -> { columnDefinitionKey -> QJsonValue }
    // Populated on index load and by setCachedValues(); persisted by flushIndex()
    QMap<QString, QMap<QString, QJsonValue>> m_cachedValues;

    bool m_hasIndexData = false;
    bool m_deferredScan = false;
    QStringList m_scannedUuids;
};

} // namespace FlySight

#endif // LOGBOOKMANAGER_H
