#ifndef SESSIONMODEL_H
#define SESSIONMODEL_H

#include <optional>

#include <QAbstractTableModel>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QVector>
#include "idlescheduler.h"
#include "logbookcolumn.h"
#include "sessiondata.h"

namespace FlySight {

struct SessionRow {
    QString sessionId;
    QMap<int, QVariant> cachedValues;  // column index -> cached value from index
    std::optional<SessionData> session; // std::nullopt = stub, has_value = loaded
    bool visible = false;  // all sessions default to not visible
    bool dirty = false;    // true when in-memory data has not been persisted

    bool isLoaded() const { return session.has_value(); }
};

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

    IdleScheduler& scheduler() { return m_scheduler; }

    // Data management
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    Qt::ItemFlags flags(const QModelIndex &index) const override;

    void mergeSessions(const QList<SessionData>& sessions);
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
    /// caused in loaded sessions: dataChanged + dependencyChanged per session,
    /// and a refresh of its cached logbook columns. Normally this runs by
    /// itself on the next event-loop pass; tests (and shutdown) call it
    /// directly.
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

    // Extract raw column values from a loaded session (used by index maintenance)
    QMap<LogbookColumn, QVariant> computeColumnValues(const SessionData &session) const;

private slots:
    void rebuildColumns();
};

} // namespace FlySight

#endif // SESSIONMODEL_H
