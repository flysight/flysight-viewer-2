#ifndef SESSIONMODEL_H
#define SESSIONMODEL_H

#include <optional>

#include <QAbstractTableModel>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QVector>
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
    // The session file could not be loaded and `session` is an empty
    // placeholder (sessionRef() must return a reference). A placeholder is
    // never attached, never saved, and never merged into; eviction resets the
    // row to a stub so that a later access retries the load.
    bool loadFailed = false;

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
    QString error;         ///< non-empty iff Failed
    bool ok() const { return outcome != Outcome::Failed; }
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
/// they discard the cached values of ALL rows, loaded or not, and the column
/// worker recomputes them; nothing is marked dirty or unsaved and no session
/// file is rewritten.
class SessionModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum CustomRoles {
        IsHoveredRole = Qt::UserRole + 100
    };

    enum WorkerTask {
        SaveTask     = 0,   // priority 1 (highest)
        LoadTask     = 1,   // priority 2
        BulkEditTask = 2,   // priority 3
        ColumnTask   = 3    // priority 4 (lowest)
    };

    SessionModel(QObject *parent = nullptr);
    ~SessionModel() override;       ///< removes the registry observer

    IdleScheduler& scheduler() { return m_scheduler; }

    /// Work done maintaining cached column values. A test seam: the temporary
    /// sessions (and engines) behind stub rows are gone by the time a test
    /// could inspect their run counters.
    struct ColumnWorkStats {
        int valuesComputed = 0;     ///< column values computed by fillMissingColumns
        int sessionsLoaded = 0;     ///< stub rows loaded temporarily (column worker, bulk edit)
        int calculationRuns = 0;    ///< calculation runs those computations caused
    };
    const ColumnWorkStats &columnWorkStats() const { return m_columnWorkStats; }
    void resetColumnWorkStats() { m_columnWorkStats = ColumnWorkStats(); }

    // Data management
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    Qt::ItemFlags flags(const QModelIndex &index) const override;

    /// The import path (spec 6.2-6.5). One result per input, in input order.
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
    /// that discards the cached logbook columns of every row. Normally this
    /// runs by itself on the next event-loop pass; tests (and shutdown) call
    /// it directly.
    void flushPendingInvalidations();

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
    QVector<SessionRow> m_rows;
    QVector<LogbookColumn> m_columns;
    QString m_hoveredSessionId;
    QString m_focusedSessionId;

    // LRU cache for non-visible loaded sessions
    QList<QString> m_lruList;  // front = most recently used, back = eviction candidate
    int m_cacheCapacity = 50;
    void lruTouch(const QString &sessionId);
    void lruRemove(const QString &sessionId);
    void lruInsert(const QString &sessionId);
    void evictIfNeeded();
    void evictSession(const QString &sessionId);

    // Invalidation. Edits made through the model return their invalidated
    // names to the caller, which publishes them at once. Invalidation that
    // originates elsewhere (a calculation registered or unregistered, a
    // declared preference changed) reaches a loaded session through its
    // engine's listener; it is queued per session and published once per
    // event-loop pass, because one user action can cause many registry changes.
    void attachSession(SessionRow &sr);
    void queueInvalidation(const QString &sessionId, const QSet<DependencyKey> &keys);
    void publishInvalidation(int row, const QSet<DependencyKey> &keys);
    QHash<QString, QSet<DependencyKey>> m_pendingInvalidations;
    bool m_invalidationFlushQueued = false;

    // Cached column values: per-column refresh (see the class comment).
    QVector<StaticDependencies> m_columnDependencies;       // parallel to m_columns
    void rebuildColumnDependencies();                       // union of staticDependencies() over columnNames(col)
    static QList<DependencyKey> columnNames(const LogbookColumn &col);  // the names computeColumnValues reads
    void invalidateColumns(int row, const QSet<DependencyKey> &changedKeys);   // persistent change
    void invalidateAllColumns(int row);                                         // new / replaced session
    void fillMissingColumns(int row, const SessionData &session);               // computes ONLY missing indices
    ColumnWorkStats m_columnWorkStats;

    // Calculation-environment changes: coalesced with the invalidation flush.
    void queueEnvironmentCheck();
    void checkCalculationEnvironment();
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

    // Bulk edit worker (edits + saves one session per tick)
    struct BulkEditItem {
        int row;
        int columnIndex;
        QVariant value;
    };
    int m_bulkEditHighWater = 0;
    int m_bulkEditRemaining = 0;
    QList<BulkEditItem> m_bulkEditQueue;
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
