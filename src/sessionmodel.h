#ifndef SESSIONMODEL_H
#define SESSIONMODEL_H

#include <functional>
#include <optional>

#include <QAbstractTableModel>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QVector>
#include "calculationresultstore.h"
#include "engine/calculationregistry.h"
#include "idlescheduler.h"
#include "logbookcolumn.h"
#include "parsedfile.h"
#include "sessiondata.h"

namespace FlySight {

struct SessionRow {
    QString sessionId;
    QMap<int, QVariant> cachedValues;  // column index -> cached value from index
    std::optional<SessionData> session; // std::nullopt = stub, has_value = loaded
    bool visible = false;  // all sessions default to not visible
    bool dirty = false;    // true when in-memory data has not been persisted
    // The last attempt to save this row failed. The row stays dirty (its
    // in-memory state is the only copy of the edit), the idle saver skips it so
    // that a persistent failure cannot spin the scheduler, and the LRU never
    // evicts it. Cleared by a successful save; a new edit clears it too, so the
    // idle saver tries again; flushDirtySessions() (shutdown) always retries.
    bool saveFailed = false;
    // The session file could not be loaded and `session` is an empty
    // placeholder (sessionRef() must return a reference). A placeholder is
    // never attached, never saved, and never merged into; eviction resets the
    // row to a stub so that a later access retries the load.
    bool loadFailed = false;
    // Explicit-backed columns of a row that is not loaded, left uncached
    // because the column worker could not vouch for a record they depend on
    // (it was skipped: see CACHED COLUMN VALUES), or because the session file
    // could not be loaded (a failed-load placeholder, a stub whose temporary
    // load failed). The worker passes them over; the next load of the session
    // fills them. Never holds an index that cachedValues holds. Empty for a
    // loaded row (except a failed-load placeholder).
    QSet<int> pendingColumns;

    bool isLoaded() const { return session.has_value(); }
};

/// What SessionModel::mergeSessions did with one input file.
struct MergeResult {
    enum class Outcome {
        Created,    ///< no session had the file's SESSION_ID: a new session exists
        Merged,     ///< the existing session changed
        Unchanged,  ///< the file holds nothing the session lacks: nothing happened at all
        Failed      ///< see error; the existing session (if any) is exactly as it was
    };
    QString filePath;      ///< ParsedFile::filePath
    QString sessionId;     ///< match id ("" when the input had none)
    Outcome outcome = Outcome::Failed;
    QString error;         ///< non-empty iff Failed; the complete reason, without the hint
    QString hint;          ///< what the user can do about it (MergePlan::hint); empty for most failures
    bool ok() const { return outcome != Outcome::Failed; }
    /// The error followed by its hint: what to show for one failure on its own.
    QString errorWithHint() const { return hint.isEmpty() ? error : error + QLatin1Char(' ') + hint; }
};

/// The logbook table: one row per session, loaded or stub.
///
/// CACHED COLUMN VALUES (SessionRow::cachedValues, mirrored in LogbookManager
/// for index.json). Loaded rows are displayed live from the session; the cache
/// exists to feed index.json and the stub the row becomes when it is evicted.
/// Invariant: a value that is present is the column's value for the row's
/// current in-memory (for a stub: on-disk) state; a value that may no longer
/// hold is REMOVED, never left stale. "Missing" is what the idle column worker
/// looks for, and every maintenance site computes only missing columns
/// (fillMissingColumns) - an edit of _DESCRIPTION does not re-run the time
/// fit for the other columns.
/// A column that depends on an explicitly requested calculation
/// (logbookColumnExplicitCalculations() is not empty) is computed like any
/// other for a LOADED row: from the engine, which holds the stored result
/// restored at load or the one just published, and reads unavailable when
/// the calculation is not requested. While such a column is enabled, the
/// demand layer (calculationdemand.h) has the requested calculations it
/// needs computed for every session, loading unloaded ones as hidden
/// sessions. The column worker knows nothing of it: a record written by a
/// job drops the values over it, and the paths below compute them again.
/// Its value is valid only with the
/// session's record set. LogbookManager stamps it in index.json and drops
/// it whenever a record of one of those calculations is written or deleted
/// (calculationRecordsChanged). The row then loses it at once and gets it
/// back on the next event-loop pass (refreshRecordColumns, which emits
/// nothing: loaded cells are live). For a row that is NOT loaded the value is
/// the same function of the session's valid stored results: without a record
/// it is cached as unavailable, with no load. With one, the column worker
/// restores the session's stored results into its temporary copy of the
/// session (restoreForColumnWorker(): the checks of a load, a stale record
/// deleted, an unreadable one skipped) and computes the value from that
/// engine, exactly as for a loaded row. A value over a record the copy could
/// not vouch for (skipped) stays PENDING (SessionRow::pendingColumns: not
/// cached, shown empty) until the session is loaded. The worker's copy counts
/// as a load for reading and never for writing: it has no explicit-result
/// listener, so it writes and rewrites no record, and it never requests,
/// prepares or runs a requested calculation. The bulk edit's temporary load
/// reads no record: it leaves such values missing for the worker.
///
/// RULE for every code path that mutates a row's PERSISTENT state
/// (SessionData::setAttribute / removeAttribute / mergeSourceData /
/// setSourceMeasurement, or replacing the row's session): before the next
/// return to the event loop it must call
///   invalidateColumns(row, changedKeys)   - changedKeys = the stored names it
///                                           wrote (attribute(k), measurement(s, n)), or
///   invalidateAllColumns(row)             - new or replaced session,
/// and schedule a save. These remove the affected cached values and mark them
/// unsaved in LogbookManager, which is what keeps index.json from ever holding
/// a column value that disagrees with the session file on disk (see
/// LogbookManager, SAVE ORDERING). This includes sessions that are loaded only
/// temporarily for the mutation (bulk edit on a stub). Which columns a key can
/// affect comes from CalculationRegistry::staticDependencies(), so it is
/// correct for cold engines and unloaded rows alike.
///
/// `mergeSessions` is the only import path. It never assigns an incoming file
/// to an existing row; an existing row's session changes only through
/// `SessionMerge::apply`.
///
/// Changes of the calculation ENVIRONMENT (a calculation registered or
/// unregistered, a declared preference changed) are not persistent changes:
/// they discard, in ALL rows, loaded or not, the cached values of exactly the
/// columns whose environment (logbookColumnEnvironment(): what the column's
/// static dependency closure can observe of the registrations and
/// preferences) changed, and the column worker recomputes them (an
/// explicit-backed column of an unloaded row from the session's stored
/// results, as above); the other columns keep their values. Nothing is
/// marked dirty or unsaved and no session file is rewritten.
///
/// ROW STABILITY. A reference to a row, or to a loaded row's session, is valid
/// only until the next operation that appends, erases, reorders or resets rows,
/// or that loads, replaces or evicts a row's session. A reader that needs such
/// references to stay valid holds a RowStabilityGuard (stableRows()) for as
/// long as it uses them: while a guard is alive every such operation asserts in
/// debug builds (in release builds the guard only counts). Release the guard
/// before anything that can emit or mutate; do not hold one across a return to
/// the event loop. forEachLoadedSession() is the guarded way to read several
/// sessions by id; loadedSession() looks one up for a reader that already
/// holds a guard.
///
/// PINNED SESSIONS. Hidden, unfocused loaded rows live in an LRU list and are
/// evicted (saved, turned back into stubs) beyond the cache capacity. The
/// executor pins the session of its running job and of its chosen next job.
/// The demand layer pins each session it loads for column demand
/// (loadPinnedSession()) until that session has no column demand left. Pins
/// are counted per session id. A pinned loaded row is passed over by eviction
/// exactly like a row whose save failed, so the cache may exceed its capacity by the number
/// of pinned rows until unpinSession() schedules the pass that brings it back.
/// A pin prevents eviction and NOTHING else: removeSessions(), a merge, and a
/// repopulation of the model still remove or change a pinned session. The model
/// knows pinned ids only; it knows nothing about jobs.
///
/// STORED RESULTS. The result of an explicit calculation that the engine
/// installs with status Ok (a job's publish or a synchronous request) is
/// written to the logbook's cache/ folder by CalculationResultStore, through
/// the explicit-result listener that attachSession() installs. That includes a
/// session not saved yet: mergeSessions() reserves its file stem at import
/// (LogbookManager::reserveSessionFile). A result that an input change, or a
/// registry change made while the application runs, drops deletes its record.
/// Every path that installs a session into a row (sessionRef(), the unloaded
/// branch of mergeSessions(), the promotion of a bulk edit's temporary
/// session) restores the session's valid records into its engine before the
/// row is published (before sessionLoaded, dataChanged or any plot pass) and
/// deletes the stale ones; a record that cannot be read is skipped (kept for
/// the next load, its column values never cached meanwhile). Restoring is not
/// requesting: it starts nothing. The column worker's temporary copy of an
/// unloaded session is restored the same way when a missing column needs a
/// record (see CACHED COLUMN VALUES); it is never installed into the row, and
/// no sessionLoaded is emitted for it. The bulk edit's temporary load never
/// reads a record. Eviction, unloading, a registration removed as
/// teardown, the model's destruction and removeSessions() never delete one
/// (LogbookManager::removeSession does, with the session file).
class SessionModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum CustomRoles {
        IsHoveredRole = Qt::UserRole + 100
    };

    /// The ids of the idle scheduler's tasks (the one namespace of them). The
    /// model registers tasks 0-3 in its constructor; a task registered by
    /// another component takes its id from this enum.
    enum WorkerTask {
        SaveTask       = 0, // priority 1 (highest)
        LoadTask       = 1, // priority 2
        BulkEditTask   = 2, // priority 3
        ColumnTask     = 3, // priority 4
        ColumnFillTask = 4  // priority 5 (lowest): registered by the demand layer
                            // (calculationdemand.h), not by the model - the column
                            // fill, whose steps are hidden loads
    };

    SessionModel(QObject *parent = nullptr);
    ~SessionModel() override;       ///< removes the registry observer

    IdleScheduler& scheduler() { return m_scheduler; }

    /// See ROW STABILITY in the class comment. Guards nest.
    class RowStabilityGuard
    {
    public:
        explicit RowStabilityGuard(const SessionModel &model) : m_model(model) { ++m_model.m_rowStabilityDepth; }
        ~RowStabilityGuard() { --m_model.m_rowStabilityDepth; }
        RowStabilityGuard(const RowStabilityGuard &) = delete;
        RowStabilityGuard &operator=(const RowStabilityGuard &) = delete;
    private:
        const SessionModel &m_model;
    };
    RowStabilityGuard stableRows() const { return RowStabilityGuard(*this); }
    int rowStabilityDepth() const { return m_rowStabilityDepth; }   ///< live guards; 0 outside guarded reads

    /// Calls `fn` with the live session of each id that names a loaded row, in
    /// the order given; ids without a row, and rows that are not loaded, are
    /// skipped. A plain read: it never loads or evicts a session and does not
    /// count as a use for the LRU. Each id is resolved to its row at the moment
    /// it is visited, under a RowStabilityGuard, so `fn` must only read; the
    /// reference it receives is not valid after it returns.
    void forEachLoadedSession(const QStringList &sessionIds,
                              const std::function<void(const SessionData &)> &fn) const;

    /// The live session of the loaded row with this id, or nullptr when the id
    /// is empty, has no row, or its row is not loaded. A plain read like
    /// forEachLoadedSession(): nothing is loaded, evicted or touched in the
    /// LRU. The pointer is a session reference in the sense of ROW STABILITY:
    /// call this, and use the result, only while holding a RowStabilityGuard.
    const SessionData *loadedSession(const QString &sessionId) const;

    /// Work done maintaining cached column values. A test seam: the temporary
    /// sessions (and engines) behind stub rows are gone by the time a test
    /// could inspect their run counters.
    struct ColumnWorkStats {
        int valuesComputed = 0;     ///< column values computed or settled (unavailable) by column maintenance; pending columns are not counted
        int sessionsLoaded = 0;     ///< stub rows loaded temporarily (column worker, bulk edit)
        int calculationRuns = 0;    ///< calculation runs those computations caused
    };
    const ColumnWorkStats &columnWorkStats() const { return m_columnWorkStats; }
    void resetColumnWorkStats() { m_columnWorkStats = ColumnWorkStats(); }

    /// Work done by the stored-result store (see STORED RESULTS). A test seam.
    const CalculationResultStore::Stats &storedResultStats() const { return m_resultStore.stats(); }
    void resetStoredResultStats() { m_resultStore.resetStats(); }

    // Data management
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    Qt::ItemFlags flags(const QModelIndex &index) const override;

    /// The import path: new session versus merge, attribute conflicts, the
    /// measurement merge, and its effects. One result per input, in input order.
    /// A file whose match id has no row CREATES a session (import-time defaults
    /// are applied here and only here); any other file MERGES into the existing
    /// session, loaded or not, as a validated all-or-nothing operation on
    /// source data and stored attributes (SessionMerge). An unloaded session is
    /// loaded for the merge; a failed load fails the file. A merge that changes
    /// something goes through the normal invalidation / column / dirty / save
    /// path and leaves the session loaded; a file that changes nothing
    /// (Unchanged) or fails mutates nothing, emits nothing, and saves nothing.
    /// A later file of the batch sees the effects of the earlier ones.
    QList<MergeResult> mergeSessions(const QList<ParsedFile> &files);
    /// For tests and tools: each session is adopted as is
    /// (ParsedFile::fromSession - no import-time defaults).
    QList<MergeResult> mergeSessions(const QList<SessionData> &sessions);

    /// Gives rows that are still known by their file stem (deferred filename
    /// scan, orphan adoption) their real SESSION_ID through a header-only
    /// read, so that an import can match them. Unreadable files and refused
    /// remaps leave the row as it is. Emits nothing.
    void resolveIdentityStubs();

    bool removeSessions(const QList<QString> &sessionIds);

    void setRowsVisibility(const QMap<int, bool>& rowVisibility);

    const SessionRow& rowAt(int row) const;
    SessionRow& rowAt(int row);
    SessionData &sessionRef(int row);
    const LogbookColumn& column(int col) const { return m_columns[col]; }

    // Populate model from cached index data (stubs only, no CSV parsing)
    void populateFromIndex(const QMap<QString, QMap<int, QVariant>> &cachedValues,
                           const QMap<QString, double> &lastAccessed);

    // Populate model from UUID filenames only (no cached values, no CSV parsing)
    void populateFromUuids(const QStringList &uuids);

    // Hovered session management
    QString hoveredSessionId() const;
    void setHoveredSessionId(const QString& sessionId);

    // Focused session management
    QString focusedSessionId() const;
    void setFocusedSessionId(const QString& sessionId);

    int getSessionRow(const QString& sessionId) const;

    bool updateAttribute(const QString &sessionId,
                         const QString &attributeKey,
                         const QVariant &newValue);

    bool removeAttribute(const QString &sessionId,
                         const QString &attributeKey);

    // Enable sorting
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    // Flush dirty sessions to disk (public so MainWindow can call on shutdown)
    void flushDirtySessions();

    /// Delivers, now, the invalidations that registry and preference changes
    /// caused in loaded sessions (dataChanged + dependencyChanged per session)
    /// and, when such a change is pending, the calculation-environment check
    /// that discards, in every row, the cached values of the logbook columns
    /// whose environment changed. Normally this
    /// runs by itself on the next event-loop pass; tests (and shutdown) call
    /// it directly.
    void flushPendingInvalidations();

    /// See PINNED SESSIONS in the class comment. Counted: every pinSession()
    /// needs one unpinSession(). An empty id is ignored; unpinning an id that
    /// is not pinned warns and changes nothing. When the last pin of an id is
    /// released one eviction pass is scheduled for the next event-loop pass;
    /// unpinSession() itself never evicts, so it is safe to call from a slot.
    void pinSession(const QString &sessionId);
    void unpinSession(const QString &sessionId);
    bool isSessionPinned(const QString &sessionId) const;

    /// Loads the session of the row with this id the way showing it would -
    /// sessionRef(): the session-id correction, the engine attached, its
    /// stored results restored, sessionLoaded, the LRU and its eviction pass -
    /// without making the row visible, and pins it (pinSession()) under the
    /// id the row has after the load, before anything can evict it. Returns
    /// that id; the caller releases the pin with unpinSession(<returned id>).
    /// A row that is already loaded is pinned without a load.
    /// Returns an empty string and pins nothing when no row has `sessionId`,
    /// or when the session file cannot be loaded (the row is then a
    /// failed-load placeholder, as after any failed load; eviction turns it
    /// back into a stub). Emits what sessionRef() emits. Must not be called
    /// while a RowStabilityGuard is held.
    QString loadPinnedSession(const QString &sessionId);

    /// Publishes, now, names that a session's engine reported as invalidated
    /// outside a model edit: the `invalidated` set of an asynchronous
    /// prepare() / publish() (the job queue). Emits one publishing dataChanged
    /// for the row, dependencyChanged per key, and modelChanged. Nothing is
    /// emitted for an empty set, an id without a row, or a row that is not
    /// loaded. A published calculation result is not a persistent change:
    /// publishing invalidates no cached column by itself; the record written
    /// at the install drops the explicit-backed values of the session through
    /// LogbookManager::calculationRecordsChanged. Nothing is marked dirty,
    /// nothing is saved (the record of a stored result is written by the
    /// engine's listener at the install itself, see STORED RESULTS).
    /// Must not be called while a RowStabilityGuard is held (it emits).
    void publishCalculationInvalidation(const QString &sessionId, const QSet<DependencyKey> &keys);

signals:
    void modelChanged();
    void sessionLoaded(const QString &sessionId);
    void hoveredSessionChanged(const QString& sessionId);
    void focusedSessionChanged(const QString& sessionId);
    void dependencyChanged(const QString &sessionId, const DependencyKey &key);
    void visibilityChanged(QSet<QString> shown, QSet<QString> hidden);

public:
    void startColumnWorker();
    void cancelColumnWorker();
    void cancelLoader();
    void startBulkEdit(const QList<int> &rows, int columnIndex, const QVariant &value);
    void cancelBulkEdit();

private:
    // Declared before m_rows so that it outlives every row's engine.
    CalculationResultStore m_resultStore;

    QVector<SessionRow> m_rows;
    QVector<LogbookColumn> m_columns;

    // Live RowStabilityGuards. Every operation that can invalidate a reference
    // into m_rows, or to a row's session, calls assertRowsMutable() first.
    mutable int m_rowStabilityDepth = 0;
    void assertRowsMutable(const char *operation) const
    {
        Q_ASSERT_X(m_rowStabilityDepth == 0, operation,
                   "rows or sessions would change while a RowStabilityGuard is held");
        Q_UNUSED(operation);
    }
    QString m_hoveredSessionId;
    QString m_focusedSessionId;

    // LRU cache for non-visible loaded sessions
    QList<QString> m_lruList;  // front = most recently used, back = eviction candidate
    int m_cacheCapacity = 50;
    void lruTouch(const QString &sessionId);
    void lruRemove(const QString &sessionId);
    void lruInsert(const QString &sessionId);
    void evictIfNeeded(const QString &keep = QString());   // keep: never evicted by this pass
    bool evictSession(const QString &sessionId);   // false: the row must stay loaded (its save failed, or it is pinned)
    QHash<QString, int> m_pinnedSessions;           // session id -> pin count (> 0); see PINNED SESSIONS
    bool m_evictionPassQueued = false;              // unpinSession() queued an evictIfNeeded()
    bool saveLoadedRow(SessionRow &sr);             // the one place a loaded row is saved; see the .cpp

    // Invalidation. Edits made through the model return their invalidated
    // names to the caller, which publishes them at once. Invalidation that
    // originates elsewhere (a calculation registered or unregistered, a
    // declared preference changed) reaches a loaded session through its
    // engine's listener; it is queued per session and published once per
    // event-loop pass, because one user action can cause many registry changes.
    void attachSession(SessionRow &sr);
    /// Restores the stored results of a row whose session has just been installed
    /// (see STORED RESULTS). Returns the names the restore invalidated.
    QSet<DependencyKey> restoreStoredResults(SessionRow &sr);
    void queueInvalidation(const QString &sessionId, const QSet<DependencyKey> &keys);
    void publishInvalidation(int row, const QSet<DependencyKey> &keys);
    QHash<QString, QSet<DependencyKey>> m_pendingInvalidations;
    bool m_invalidationFlushQueued = false;

    // Cached column values: per-column refresh (see the class comment).
    QVector<StaticDependencies> m_columnDependencies;       // parallel to m_columns
    QVector<QStringList> m_columnExplicitCalculations;      // parallel to m_columns: E(c)
    void rebuildColumnDependencies();                       // union of staticDependencies() over logbookColumnNames(col)
    bool isExplicitBacked(int column) const;
    /// True when some column of the row is neither cached nor pending: the one
    /// "row needs column work" test (a pending column must not make the
    /// column worker spin).
    bool needsColumnWork(const SessionRow &row) const;
    void invalidateColumns(int row, const QSet<DependencyKey> &changedKeys);   // persistent change
    void invalidateAllColumns(int row);                                         // new / replaced session
    enum class ColumnSource {
        LoadedRow,      ///< the row's own session: its engine holds the restored / published results
        TemporaryLoad,  ///< the bulk edit's temporary load: no stored result was restored
        WorkerCopy      ///< the column worker's temporary copy: restoreForColumnWorker() ran on it
    };
    /// Computes ONLY the missing indices (neither cached nor pending). For a
    /// TemporaryLoad, and for a failed-load placeholder, the explicit-backed
    /// ones are settled first (settleExplicitColumns), and a TemporaryLoad
    /// leaves those with a record missing for the column worker. For a
    /// WorkerCopy, a missing column over a record the copy could not vouch for
    /// (LogbookManager::unconfirmedCalculationRecords(): skipped, or a removal
    /// that failed) goes pending instead of being computed.
    void fillMissingColumns(int row, const SessionData &session, ColumnSource source);
    /// What settleExplicitColumns() does with a column over a known record.
    enum class RecordColumns {
        LeaveMissing,   ///< for the column worker, which restores the record into its copy
        Pend            ///< the session file cannot be loaded: wait for the next load
    };
    /// For each missing explicit-backed column of a row whose engine holds no
    /// stored result (a stub, a temporary load, a failed-load placeholder):
    /// cached as unavailable (row and LogbookManager::updateCachedValues) when
    /// LogbookManager knows no record of the session for any calculation the
    /// column depends on; otherwise left missing or made pending, as
    /// `recordColumns` says. Loads nothing and reads no record: the record set
    /// comes from knownCalculationRecords(). "No record, so unavailable" holds
    /// only while an Explicit calculation's outputs have no other candidate
    /// (docs/CALCULATIONS.md section 8).
    void settleExplicitColumns(int row, RecordColumns recordColumns);
    /// The column worker's read of a stub's stored results: when some missing
    /// column of the row depends on a calculation LogbookManager knows a
    /// record of, restores the session's records into `copy`'s engine through
    /// the result store's restore of a load (the same checks: a stale
    /// record deleted, an unreadable one skipped) and returns true; otherwise
    /// (and always for a row that is loaded) reads nothing and returns false.
    /// `copy` is the worker's temporary copy
    /// of the row's session: it has no listener, so the restore writes no
    /// record, and nothing is requested, prepared or run. Called once per
    /// worker step, only from processNextDirtyColumn().
    bool restoreForColumnWorker(int row, SessionData &copy);
    ColumnWorkStats m_columnWorkStats;

    /// A record of (sessionId, calculationId) changed (LogbookManager has
    /// dropped its copies). Removes the row's cached and pending values of every
    /// column over that calculation; a loaded row is recomputed on the next
    /// event-loop pass (queueRecordColumnRefresh), a stub is filled by the
    /// column worker. Runs inside record methods, which the result store calls
    /// from an engine listener and from a restore (a load, or the column
    /// worker's copy): it only drops state and defers.
    void onCalculationRecordsChanged(const QString &sessionId, const QString &calculationId);
    QSet<QString> m_recordColumnRefresh;        // loaded rows whose explicit-backed values are to be recomputed
    bool m_recordColumnRefreshQueued = false;
    void queueRecordColumnRefresh(const QString &sessionId);   // coalesced, QueuedConnection
    /// Recomputes the missing columns of the queued loaded rows from their
    /// engines, emits nothing (loaded cells are live), and flushes the index
    /// once when it computed something.
    void refreshRecordColumns();

    // Calculation-environment changes: coalesced with the invalidation flush.
    // The check discards the columns whose environment changed
    // (LogbookManager::checkColumnEnvironments()) in every row.
    void queueEnvironmentCheck();
    void checkCalculationEnvironment();
    /// True when the static dependency closure of some column reads `key`.
    bool columnsReadPreference(const QString &key) const;
    /// Runs a pending environment check now. Called first by every path that
    /// stores column values (fillMissingColumns, settleExplicitColumns,
    /// restoreForColumnWorker), so
    /// that a value enters the cache only under the environment of its column
    /// it was computed in; rebuildColumns() does the check itself.
    void applyPendingEnvironmentCheck();
    bool m_environmentCheckPending = false;
    int m_registryObserver = -1;

    // Idle scheduler (replaces per-worker QTimers)
    IdleScheduler m_scheduler;

    // Deferred logbook persistence (saver worker)
    int m_saveHighWater = 0;   // total dirty sessions in current save wave
    int m_saveRemaining = 0;   // dirty sessions remaining in current wave
    void scheduleSave(const QString &sessionId);
    void saveNextSession();

    // Background visibility loader (loads stub sessions made visible)
    int m_loadHighWater = 0;
    int m_loadRemaining = 0;
    QList<QString> m_loadQueue;       // session IDs of stubs to load
    QSet<QString> m_loadedDuringBatch; // sessions loaded so far (emitted on completion/cancel)
    static constexpr int kSyncLoadThreshold = 3;
    void loadNextVisibleSession();

    // Dirty column worker (computes missing cached column values in background)
    int m_columnWorkerHighWater = 0;
    int m_columnWorkerRemaining = 0;
    void processNextDirtyColumn();

    // Bulk edit worker (edits + saves one session per tick). Items wait across
    // returns to the event loop, during which rows can be sorted, added,
    // removed or reset and the columns rebuilt, so an item names its target by
    // session id and attribute key, never by row or column index; the row is
    // looked up when the item is processed. setRowSessionId() keeps the queued
    // ids current when a row's id changes.
    struct BulkEditItem {
        QString sessionId;
        QString attributeKey;
        QVariant value;
    };
    int m_bulkEditHighWater = 0;
    int m_bulkEditRemaining = 0;
    int m_bulkEditSkipped = 0;   // items of the current batch whose session was gone
    QList<BulkEditItem> m_bulkEditQueue;
    /// The one place a row's id changes after the row was created (a row known
    /// by its file stem learns its real SESSION_ID): remaps the logbook entry
    /// and, if that succeeds, the row and every queued bulk edit item that
    /// names it. False (nothing changed) when the logbook refuses the remap.
    bool setRowSessionId(SessionRow &sr, const QString &realId);
    void finishBulkEdit();
    void processNextBulkEdit();

    // Formatting helpers for data()
    QVariant formatAttributeValue(const SessionData &session, const LogbookColumn &col) const;
    QVariant formatMeasurementValue(const SessionData &session, const LogbookColumn &col) const;
    QVariant formatDeltaValue(const SessionData &session, const LogbookColumn &col) const;
    QVariant formatRawValue(const QVariant &rawValue, const LogbookColumn &col) const;
    QString columnUnitLabel(const LogbookColumn &col) const;

    // Extract the raw values of the given columns (indices into m_columns) from
    // a loaded session. What a column stores is part of cache validity: see
    // CalculationCompatibilityVersion before changing it.
    QMap<LogbookColumn, QVariant> computeColumnValues(const SessionData &session,
                                                      const QVector<int> &columnIndices) const;

private slots:
    void rebuildColumns();
};

} // namespace FlySight

#endif // SESSIONMODEL_H
