#ifndef LOGBOOKMANAGER_H
#define LOGBOOKMANAGER_H

#include <optional>

#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

#include "logbookcolumn.h"
#include "sessiondata.h"

namespace FlySight {

/// The logbook on disk: one CSV per session under sessions/, and index.json,
/// which maps SESSION_IDs to file names and caches the logbook column values
/// of every session so that the logbook can be shown without parsing a CSV.
///
/// index.json root: "calculationCompatibility" (integer marker,
/// FlySight::CalculationCompatibilityVersion), "calculationEnvironment"
/// (calculationEnvironmentFingerprint() the cached values were computed under),
/// "columns", "sessions" (per id: "uuid", "lastAccessed", "values").
///
/// CACHE VALIDITY is decided here and nowhere else. initialize() keeps the
/// cached "values" only when both the marker and the environment recorded in
/// the index equal the current ones; otherwise every cached value is dropped
/// (uuid and lastAccessed are kept, session files are never touched or even
/// opened) and the model's idle column worker recomputes them lazily.
///
/// SAVE ORDERING. index.json and a session file are separate atomic writes.
/// The invariant kept by construction is: index.json on disk never holds a
/// column value that disagrees with the session file on disk. A persistent
/// change marks the columns it can affect UNSAVED (markColumnsUnsaved /
/// markSessionUnsaved): "the value of this column for the in-memory session
/// may differ from its value for the session file on disk". flushIndex()
/// omits unsaved columns even when a new value is already cached in memory;
/// saveSession() first flushes the index if the on-disk index may still hold
/// a value for a marked column, then writes the CSV, then clears the marks, so
/// that the next flush publishes the new values. A save that FAILS clears
/// nothing: the marks stay, so every later flush keeps omitting the affected
/// columns (whatever is cached for them in memory), and the previous session
/// file is intact. SessionModel then keeps the row loaded and dirty
/// (SessionRow::saveFailed) and retries at the next edit or at shutdown; the
/// marks are cleared by the save that finally succeeds. See saveSession() in
/// the .cpp for the crash analysis.
class LogbookManager : public QObject {
    Q_OBJECT

public:
    static LogbookManager& instance();

    // Creates sessions directory if missing, loads index.json or scans *.csv
    // file names. No session file is parsed: every session comes up as a stub.
    void initialize();

    // Drops all in-memory index state so the next initialize() re-reads the current
    // logbook folder. Used by tests to simulate an application restart.
    void reset();

    // Writes a session to disk as a UUID-based .csv file; returns true on success.
    // The only caller of DataExporter::exportSession in the application, so no
    // session file is written without the pre-save index flush described above.
    // On failure (see lastSaveError()) the previous file is intact and the
    // session's unsaved marks stay in place.
    bool saveSession(const SessionData& session);

    // Reason of the last failed saveSession(); empty after a successful one.
    QString lastSaveError() const;

    // Deletes the .csv file for the given SESSION_ID; returns true on success
    bool removeSession(const QString& sessionId);

    // Writes index.json (atomically): the marker, cacheEnvironment(), the column
    // definitions, and per session uuid / lastAccessed / cached values except
    // those of unsaved columns. Returns true when the file was committed.
    bool flushIndex();

    // True when the in-memory index differs from what flushIndex() last wrote
    // (or from what initialize() read).
    bool indexNeedsFlush() const;

    // Stores cached column values for a session, to be written on next flushIndex().
    // columnValues maps a LogbookColumn (matched by definition) to its QVariant value.
    // REPLACES everything cached for the session; updateCachedValues() MERGES.
    void setCachedValues(const QString &sessionId,
                         const QMap<LogbookColumn, QVariant> &columnValues);
    void updateCachedValues(const QString &sessionId,
                            const QMap<LogbookColumn, QVariant> &columnValues);

    // --- Cache validity (calculation-compatibility marker + environment) ---

    // The environment fingerprint the in-memory cached values are valid for.
    // flushIndex() writes THIS, never a freshly computed fingerprint: if nobody
    // told the manager about an environment change (discardCachedValues()), the
    // next start sees the mismatch and discards.
    QString cacheEnvironment() const;

    // True when initialize() found a missing / different marker or environment
    // and therefore dropped every cached value.
    bool cachedValuesDiscardedOnLoad() const;

    // Drops the cached values of every session and adopts the current
    // environment fingerprint. Unsaved marks are unaffected: they concern
    // persistence, not calculation semantics.
    void discardCachedValues();

    // --- Unsaved-column tracking (see SAVE ORDERING above) ---

    // Removes the cached values of these columns and keeps them out of
    // index.json until the session has been saved.
    void markColumnsUnsaved(const QString &sessionId, const QVector<LogbookColumn> &columns);
    // The same for all columns, present and future (new or replaced session).
    void markSessionUnsaved(const QString &sessionId);
    bool hasUnsavedColumns(const QString &sessionId) const;
    // Forgets the values cached for this session under any column that is not
    // in `columns`. Called with the enabled columns when the session changes:
    // values kept for disabled columns cannot be checked against the change
    // and must not come back stale when such a column is enabled again.
    void dropCachedValuesExcept(const QString &sessionId, const QVector<LogbookColumn> &columns);

    // Loads and parses the CSV for a single session: the file's contents and
    // nothing else (no backfill, no Viewer-generated value). Returns
    // std::nullopt on failure, with the reason in *error: "not in the logbook
    // index" for an unknown id, otherwise the importer's error text ("Couldn't
    // read file", "Unknown file format", "Missing $DATA section", ...).
    // This is the load a merge uses: merge decisions are made on what is on disk.
    std::optional<SessionData> loadSessionRaw(const QString &sessionId, QString *error = nullptr);

    // Compatibility shim for session files written by Viewer versions that
    // predate `_JUMPER_MASS` / `_PLANFORM_AREA` / `_WIND_N` / `_WIND_E`. It is
    // not an import default: it never runs on an incoming file, never runs
    // before a merge has been decided, only fills keys that are absent, and
    // does not mark the session dirty (pinned by
    // `tst_persistence_roundtrip::releasedLogbookBackfillIsAdditive`).
    static void applyLegacyBackfill(SessionData &session);

    // loadSessionRaw() + applyLegacyBackfill(): the ordinary load.
    // Returns std::nullopt on failure.
    std::optional<SessionData> loadSession(const QString &sessionId);

    // True when the entry maps a file stem to itself: a session known only by
    // its file name (deferred filename scan, orphan adoption), whose real
    // SESSION_ID has not been read yet.
    bool isIdentityEntry(const QString &sessionId) const;

    // Header-only read of the SESSION_ID recorded in the entry's CSV. nullopt
    // when the id is unknown, the file is unreadable, or it records none.
    std::optional<QString> peekSessionId(const QString &sessionId) const;

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
    // Updates m_sessionIdToUuid, m_lastAccessed, m_cachedValues, and the unsaved marks.
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

    // Scans *.csv filenames only (no parsing); populates m_sessionIdToUuid with identity mappings
    QStringList scanSessionFilenames();

    // File stems of sessions/*.csv, sorted by name. Touches no state.
    QStringList sessionFileStems() const;

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

    // Cache validity
    QString m_cacheEnvironment;         // fingerprint m_cachedValues is valid for
    bool m_discardedOnLoad = false;
    bool m_indexNeedsFlush = false;

    // Unsaved-column tracking
    QMap<QString, QSet<QString>> m_unsavedColumns;  // SESSION_ID -> column definition keys
    QSet<QString> m_unsavedAll;                     // SESSION_IDs with every column unsaved
    QSet<QString> m_needsFlushBeforeSave;           // the on-disk index may hold a marked value

    QString m_lastSaveError;
};

} // namespace FlySight

#endif // LOGBOOKMANAGER_H
