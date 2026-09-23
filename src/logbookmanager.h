#ifndef LOGBOOKMANAGER_H
#define LOGBOOKMANAGER_H

#include <optional>

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include "calculationrecord.h"
#include "logbookcolumn.h"
#include "sessiondata.h"

namespace FlySight {

/// What LogbookManager::readCalculationRecord() found.
struct CalculationRecordRead {
    CalculationRecordStatus status = CalculationRecordStatus::Missing;
    std::optional<CalculationRecord> record;   ///< set only for Ok
    QString error;                             ///< empty for Ok and for a plain Missing
};

/// The logbook on disk: one CSV per session under sessions/ (beside it, the
/// session's calculation record files), and index.json, which maps SESSION_IDs
/// to file names and caches the logbook column values of every session so
/// that the logbook can be shown without parsing a CSV.
///
/// CALCULATION RECORDS. sessions/ also holds
/// <stem>.<encoded calculation id>.fvresult files (calculationrecord.h), at
/// most one per (session, requested calculation): the stored result of an
/// explicit calculation. They are keyed by the session's file stem (which
/// never changes), are referenced only by the per-session record stamp (see
/// RECORD STAMPS), and are never listed as sessions (they do not end in .csv). They are written only through
/// writeCalculationRecord() (QSaveFile, like a session file), and removed with
/// their session (removeSession), by the stray pass of initialize() when their
/// session file does not exist, and by explicit removal. A session
/// not saved yet may have records under the stem reserved for it
/// (reserveSessionFile()); if it is never saved they are strays and the next
/// initialize() removes them. The manager only stores and reports them; the
/// caller decides validity.
///
/// index.json root: "calculationCompatibility" (integer marker,
/// FlySight::CalculationCompatibilityVersion), "calculationEnvironment"
/// (calculationEnvironmentFingerprint() the cached values were computed under),
/// "columns", "sessions" (per id: "uuid", "lastAccessed", "values", and
/// "records", the record stamp: calculation id -> current result version of
/// each known and confirmed record of the session, "" when the descriptor
/// declares none; always written, {} when there is none).
///
/// CACHE VALIDITY is decided here and nowhere else. initialize() keeps the
/// cached "values" only when both the marker and the environment recorded in
/// the index equal the current ones; otherwise every cached value is dropped
/// (uuid and lastAccessed are kept, session files are never touched or even
/// opened) and the model's idle column worker recomputes them lazily. A kept
/// value of a column over explicit calculations E
/// (logbookColumnExplicitCalculations()) must also agree with the record
/// files: with a "records" stamp, every e in E is in the stamp exactly when
/// the session has a record of e, with the stamp's version equal to e's
/// current result version; without one (an entry written by an older build,
/// which cached such values as unavailable), the session has no record of any
/// e in E. A value that fails is dropped.
///
/// RECORD STAMPS. The manager knows which calculations have a record file per
/// session (knownCalculationRecords(): one names-only listing at initialize(),
/// then its own writes and removals; no record is opened). Ids whose record
/// may disagree with the loaded session's engine (a failed write or removal,
/// an environment change while loaded) are UNCONFIRMED: flushIndex() leaves
/// them out of the stamp and omits the values over them. Writing or removing
/// a record drops the session's cached values over that calculation and emits
/// calculationRecordsChanged(). Ordering rule: a write whose calculation
/// index.json on disk lists as present under a value flushes the index first;
/// a removal never needs to (the start-up check sees the present -> absent
/// flip). See the crash table above writeCalculationRecord() in the .cpp.
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

    // Creates sessions directory if missing, removes stray calculation records
    // (those whose session file does not exist; names only, nothing is
    // opened), then loads index.json or scans *.csv file names. No session
    // file is parsed: every session comes up as a stub.
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

    // Deletes the .csv file and the calculation records of the given
    // SESSION_ID; returns true when the .csv file was removed. The .csv goes
    // first: if it cannot be removed, nothing is (false). A record that cannot
    // be removed afterwards is warned about and left to the next start's stray
    // pass (still true). A session that only has a reserved stem
    // (reserveSessionFile(), never saved) has no .csv: its records are
    // removed, the reservation is dropped, and the result is true. False for
    // an id that is neither in the index nor reserved.
    bool removeSession(const QString& sessionId);

    // --- Calculation records ---

    // Writes sessions/<stem>.<encoded id>.fvresult atomically (QSaveFile),
    // replacing any previous record for (session, record.result's calculation id).
    // On failure *error is set and the previous record (if any) is intact. The
    // record is encoded before any file is opened, so a record the format
    // refuses never touches the disk. Drops the session's cached values over
    // the calculation, flushes index.json first when the ordering rule asks
    // for it (RECORD STAMPS; if that flush fails the record is not written),
    // and emits calculationRecordsChanged() unless the session is unknown.
    bool writeCalculationRecord(const QString &sessionId, const CalculationRecord &record,
                                QString *error = nullptr);

    // Reads and decodes one record. Missing when the session is unknown or has
    // no such file; Unreadable when the file cannot be opened / read; the codec's
    // status otherwise; Corrupt when the record names a different calculation id.
    // Never deletes anything.
    CalculationRecordRead readCalculationRecord(const QString &sessionId, const QString &calculationId) const;

    // Calculation ids of the session's record files, sorted. Names only: no
    // record is opened. Empty for an unknown session.
    QStringList calculationRecordIds(const QString &sessionId) const;

    // Deletes one record / every record of the session. True when no such file
    // remains (absent counts as success); false for an unknown session or when a
    // file could not be removed (warned). Drops the session's cached values
    // over the calculation and emits calculationRecordsChanged(), except for an
    // unknown session and for an absent record that was neither known nor
    // unconfirmed (nothing changed). Never flushes index.json.
    bool removeCalculationRecord(const QString &sessionId, const QString &calculationId);
    bool removeCalculationRecords(const QString &sessionId);

    // --- Record stamps of cached column values (see RECORD STAMPS) ---

    // Calculation ids with a record file for the session, as known from the
    // names-only listing at initialize() and this manager's own writes and
    // removals. No record is opened. Files changed behind the application's
    // back are seen at the next initialize().
    QSet<QString> knownCalculationRecords(const QString &sessionId) const;
    // Ids whose record may disagree with the loaded session's engine. Values
    // that depend on them are never written to index.json.
    QSet<QString> unconfirmedCalculationRecords(const QString &sessionId) const;
    // Marks every known record of the session unconfirmed. SessionModel calls
    // it for each loaded row on a calculation-environment change, because a
    // registry change drops explicit results from memory and keeps their records.
    void markCalculationRecordsUnconfirmed(const QString &sessionId);
    // The session's in-memory results are being discarded (eviction): drops
    // the cached values that depend on its unconfirmed records, forgets the
    // marks, and returns the ids. From now on the records on disk are the truth.
    QStringList discardUnconfirmedCalculationRecords(const QString &sessionId);

    // Writes index.json (atomically): the marker, cacheEnvironment(), the column
    // definitions, and per session uuid / lastAccessed / cached values (except
    // those of unsaved columns and those over unconfirmed records) / the
    // record stamp. Returns true when the file was committed.
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

    // Gives a session that has no session file yet the file stem its first
    // save will use, so that files kept beside the session file (calculation
    // records) can be written before that save. Returns the stem: the
    // session's existing one if it has a file, else a reserved fresh uuid
    // (idempotent). A reservation is never listed in index.json, is not a
    // session of the scan, and is forgotten by reset(). Empty id: returns "".
    QString reserveSessionFile(const QString &sessionId);

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
    // Returns false if oldId not found or newId already exists (in the index or
    // as a reservation).
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

signals:
    // A record of (sessionId, calculationId) was written, removed, or a write or
    // removal of it failed. The cached values of the session that depend on the
    // calculation have already been dropped here. Emitted synchronously, from
    // inside the record method (so possibly from an engine listener): a receiver
    // must only drop state and defer work. Not emitted by removeSession(), the
    // stray pass, or initialize().
    void calculationRecordsChanged(const QString &sessionId, const QString &calculationId);

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

    // --- Calculation records (keyed by file stem: the stray pass and
    //     removeSession have a stem, not an id) ---

    // The file stem the records of SESSION_ID are named after: the index's
    // uuid, else the stem reserved for a session not saved yet; empty when
    // it is neither ("not in the logbook index").
    QString recordStem(const QString &sessionId) const;

    QString calculationRecordPath(const QString &stem, const QString &calculationId) const;
    // Names of the *.fvresult files in sessions/ (QDir::Files), sorted.
    QStringList calculationRecordFileNames() const;
    // Calculation ids of the record files whose parsed stem is exactly `stem`, sorted.
    QStringList calculationRecordIdsForStem(const QString &stem) const;
    // Removes every record file whose parsed stem is exactly `stem`; false when
    // one could not be removed (warned).
    bool removeCalculationRecordsForStem(const QString &stem);

    // Removes every record file whose name does not parse or whose stem has no
    // session file (*.csv) on disk. Names only: no record and no session file
    // is opened, and only *.fvresult files are deleted. Returns the number of
    // files removed.
    int removeStrayCalculationRecords();

    // --- Record stamps ---

    // Fills m_knownRecords from the names of the record files: every (stem, id)
    // whose stem is the file of a session of m_sessionIdToUuid. Names only.
    void adoptCalculationRecordSet();
    // Applies the start-up validity rule (class comment, CACHE VALIDITY) to the
    // cached values of explicit-backed columns and sets m_recordBackedOnDisk.
    // `columnsByDefKey`: the index's columns; `stamps`: per session, the
    // "records" object of the entries that have one.
    void validateRecordStamps(const QMap<QString, LogbookColumn> &columnsByDefKey,
                              const QMap<QString, QJsonObject> &stamps);
    // Removes from the session's cached values every value whose column depends
    // on one of `calculationIds`, and every value of a column that is not
    // enabled. True (and m_indexNeedsFlush set) when something was removed.
    // Marks nothing unsaved: this is cache validity, not save ordering.
    bool dropRecordDependentValues(const QString &sessionId, const QStringList &calculationIds);
    // removeCalculationRecord() for a resolved stem.
    bool removeCalculationRecordOfStem(const QString &sessionId, const QString &stem,
                                       const QString &calculationId);

    // Maps SESSION_ID strings to UUID filename stems (without extension).
    // Means "has a session file": flushIndex() lists every entry.
    QMap<QString, QString> m_sessionIdToUuid;

    QMap<QString, QString> m_reservedStems; // SESSION_ID -> uuid of a session not saved yet

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

    // Record stamps (see RECORD STAMPS), keyed by SESSION_ID like
    // m_cachedValues (a reserved session by its id as well)
    QMap<QString, QSet<QString>> m_knownRecords;        // SESSION_ID -> calculation ids with a record file
    QMap<QString, QSet<QString>> m_unconfirmedRecords;  // SESSION_ID -> ids whose record may disagree with the loaded engine
    QMap<QString, QSet<QString>> m_recordBackedOnDisk;  // SESSION_ID -> ids index.json on disk lists as present
                                                        //   AND on which some value it holds for the session depends
};

} // namespace FlySight

#endif // LOGBOOKMANAGER_H
