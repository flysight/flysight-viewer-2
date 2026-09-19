#include "sessionmodel.h"

#include <algorithm>

#include <QDateTime>
#include <QTimeZone>

#include "attributeregistry.h"
#include "engine/calculationengine.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "units/unitconverter.h"

namespace FlySight {

SessionModel::SessionModel(QObject *parent)
    : QAbstractTableModel(parent)
{
    connect(&LogbookColumnStore::instance(), &LogbookColumnStore::columnsChanged,
            this, &SessionModel::rebuildColumns);

    connect(&UnitConverter::instance(), &UnitConverter::systemChanged, this, [this]() {
        if (m_rows.isEmpty() || m_columns.isEmpty()) return;
        emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {Qt::DisplayRole});
        emit headerDataChanged(Qt::Horizontal, 0, columnCount() - 1);
    });

    // Register idle-scheduler tasks (priority 1 = highest)
    m_scheduler.registerTask(SaveTask, {
        /*priority*/    1,
        /*step*/        [this]() { saveNextSession(); },
        /*hasWork*/     [this]() { return std::any_of(m_rows.cbegin(), m_rows.cend(),
                                       [](const SessionRow &r) { return r.dirty; }); },
        /*progress*/    [this]() -> Progress { return {m_saveRemaining, m_saveHighWater}; },
        /*onComplete*/  [this](bool /*cancelled*/) {
                            LogbookManager::instance().flushIndex();
                            m_saveHighWater = 0;
                            m_saveRemaining = 0;
                        },
        /*cancellable*/ false
    });

    m_scheduler.registerTask(LoadTask, {
        /*priority*/    2,
        /*step*/        [this]() { loadNextVisibleSession(); },
        /*hasWork*/     [this]() { return !m_loadQueue.isEmpty(); },
        /*progress*/    [this]() -> Progress { return {m_loadRemaining, m_loadHighWater}; },
        /*onComplete*/  [this](bool cancelled) {
                            if (cancelled) {
                                // Untick sessions still in m_loadQueue
                                QSet<QString> hiddenIds;
                                int minRow = INT_MAX;
                                int maxRow = 0;
                                for (const QString &sessionId : std::as_const(m_loadQueue)) {
                                    int row = getSessionRow(sessionId);
                                    if (row >= 0 && m_rows[row].visible && !m_rows[row].isLoaded()) {
                                        m_rows[row].visible = false;
                                        hiddenIds.insert(sessionId);
                                        minRow = std::min(minRow, row);
                                        maxRow = std::max(maxRow, row);
                                    }
                                }
                                m_loadQueue.clear();

                                QSet<QString> shownIds = std::move(m_loadedDuringBatch);
                                m_loadedDuringBatch.clear();

                                if (!shownIds.isEmpty() || !hiddenIds.isEmpty())
                                    emit visibilityChanged(shownIds, hiddenIds);
                                if (minRow <= maxRow)
                                    emit dataChanged(index(minRow, 0), index(maxRow, columnCount() - 1), {Qt::CheckStateRole});
                            } else {
                                if (!m_loadedDuringBatch.isEmpty()) {
                                    QSet<QString> shown = std::move(m_loadedDuringBatch);
                                    m_loadedDuringBatch.clear();
                                    emit visibilityChanged(shown, {});
                                }
                            }
                            m_loadHighWater = 0;
                            m_loadRemaining = 0;
                            m_loadedDuringBatch.clear();
                        },
        /*cancellable*/ true
    });

    m_scheduler.registerTask(BulkEditTask, {
        /*priority*/    3,
        /*step*/        [this]() { processNextBulkEdit(); },
        /*hasWork*/     [this]() { return !m_bulkEditQueue.isEmpty(); },
        /*progress*/    [this]() -> Progress { return {m_bulkEditRemaining, m_bulkEditHighWater}; },
        /*onComplete*/  [this](bool cancelled) {
                            if (cancelled)
                                m_bulkEditQueue.clear();
                            finishBulkEdit();
                        },
        /*cancellable*/ true
    });

    m_scheduler.registerTask(ColumnTask, {
        /*priority*/    4,
        /*step*/        [this]() { processNextDirtyColumn(); },
        /*hasWork*/     [this]() {
                            return std::any_of(m_rows.cbegin(), m_rows.cend(),
                                [this](const SessionRow &r) { return r.cachedValues.size() < m_columns.size(); });
                        },
        /*progress*/    [this]() -> Progress { return {m_columnWorkerRemaining, m_columnWorkerHighWater}; },
        /*onComplete*/  [this](bool /*cancelled*/) {
                            LogbookManager::instance().flushIndex();
                            m_columnWorkerHighWater = 0;
                            m_columnWorkerRemaining = 0;
                        },
        /*cancellable*/ true
    });

    // LRU cache capacity preference
    PreferencesManager &prefs = PreferencesManager::instance();
    prefs.registerPreference(PreferenceKeys::LogbookCacheSize, 50);
    m_cacheCapacity = prefs.getValue(PreferenceKeys::LogbookCacheSize).toInt();

    connect(&prefs, &PreferencesManager::preferenceChanged,
            this, [this](const QString &key, const QVariant &value) {
        if (key == PreferenceKeys::LogbookCacheSize) {
            m_cacheCapacity = value.toInt();
            evictIfNeeded();
        }
    });

    rebuildColumns();
}

void SessionModel::rebuildColumns()
{
    beginResetModel();
    m_columns = LogbookColumnStore::instance().enabledColumns();

    // Rebuild cached values for all sessions to reflect new column set
    LogbookManager &logbook = LogbookManager::instance();
    for (SessionRow &row : m_rows) {
        if (row.isLoaded()) {
            // Loaded session: recompute all column values from in-memory data
            QMap<LogbookColumn, QVariant> colValues = computeColumnValues(row.session.value());
            logbook.setCachedValues(row.sessionId, colValues);
            QMap<int, QVariant> indexed;
            for (int i = 0; i < m_columns.size(); ++i)
                indexed[i] = colValues.value(m_columns[i]);
            row.cachedValues = indexed;
        } else {
            // Stub session: remap existing cached values to new column indices.
            // Columns that already have values keep them; new columns are left
            // absent so the column worker will fill them in.
            QMap<int, QVariant> indexed;
            const auto &stored = logbook.cachedValuesForSession(row.sessionId);
            for (int i = 0; i < m_columns.size(); ++i) {
                const QString defKey = logbook.columnDefKey(m_columns[i]);
                auto it = stored.constFind(defKey);
                if (it != stored.constEnd())
                    indexed[i] = logbook.jsonToVariant(it.value());
            }
            row.cachedValues = indexed;
        }
    }

    endResetModel();

    // Flush the index so it reflects the current column set
    if (!m_rows.isEmpty())
        logbook.flushIndex();

    // Start the dirty column worker to compute values for any missing columns
    startColumnWorker();
}

int SessionModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return m_rows.size();
}

int SessionModel::columnCount(const QModelIndex &parent) const {
    Q_UNUSED(parent);
    return m_columns.size();
}

QVariant SessionModel::formatAttributeValue(const SessionData &session, const LogbookColumn &col) const
{
    QVariant value = session.getAttribute(col.attributeKey);
    if (!value.isValid())
        return QVariant();

    // Look up the attribute definition to determine formatting
    const auto *def = AttributeRegistry::instance().findByKey(col.attributeKey);
    AttributeFormatType formatType = def ? def->formatType : AttributeFormatType::Text;

    switch (formatType) {
    case AttributeFormatType::Text:
        return value.toString();

    case AttributeFormatType::DateTime: {
        bool ok = false;
        double utcSeconds = value.toDouble(&ok);
        if (!ok)
            return QVariant();
        QDateTime dt = QDateTime::fromMSecsSinceEpoch(
            qint64(utcSeconds * 1000.0), QTimeZone::utc());
        return dt.toString("yyyy/MM/dd HH:mm:ss");
    }

    case AttributeFormatType::Duration: {
        bool ok = false;
        double durationSec = value.toDouble(&ok);
        if (!ok) return QVariant();
        int totalSec = static_cast<int>(durationSec);
        int minutes = totalSec / 60;
        int seconds = totalSec % 60;
        return QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
    }

    case AttributeFormatType::Double: {
        bool ok = false;
        double val = value.toDouble(&ok);
        if (!ok) return QVariant();
        if (def && !def->measurementType.isEmpty())
            return UnitConverter::instance().formatValue(val, def->measurementType);
        return QString::number(val);
    }
    }

    return QVariant();
}

QVariant SessionModel::formatMeasurementValue(const SessionData &session, const LogbookColumn &col) const
{
    QString interpKey = SessionData::interpolationKey(
        col.markerAttributeKey, col.sensorID, SessionKeys::Time, col.measurementID);
    QVariant value = session.getAttribute(interpKey);
    if (!value.isValid())
        return QVariant();

    return UnitConverter::instance().formatValue(value.toDouble(), col.measurementType);
}

QVariant SessionModel::formatDeltaValue(const SessionData &session, const LogbookColumn &col) const
{
    QString interpKey1 = SessionData::interpolationKey(
        col.markerAttributeKey, col.sensorID, SessionKeys::Time, col.measurementID);
    QString interpKey2 = SessionData::interpolationKey(
        col.marker2AttributeKey, col.sensorID, SessionKeys::Time, col.measurementID);

    QVariant value1 = session.getAttribute(interpKey1);
    QVariant value2 = session.getAttribute(interpKey2);

    if (!value1.isValid() || !value2.isValid())
        return QVariant();

    double delta = value2.toDouble() - value1.toDouble();
    return UnitConverter::instance().formatValue(delta, col.measurementType);
}

QVariant SessionModel::formatRawValue(const QVariant &rawValue, const LogbookColumn &col) const
{
    if (!rawValue.isValid())
        return QVariant();

    switch (col.type) {
    case ColumnType::SessionAttribute: {
        const auto *def = AttributeRegistry::instance().findByKey(col.attributeKey);
        AttributeFormatType formatType = def ? def->formatType : AttributeFormatType::Text;

        switch (formatType) {
        case AttributeFormatType::Text:
            return rawValue.toString();

        case AttributeFormatType::DateTime: {
            bool ok = false;
            double utcSeconds = rawValue.toDouble(&ok);
            if (!ok) return QVariant();
            QDateTime dt = QDateTime::fromMSecsSinceEpoch(
                qint64(utcSeconds * 1000.0), QTimeZone::utc());
            return dt.toString("yyyy/MM/dd HH:mm:ss");
        }

        case AttributeFormatType::Duration: {
            bool ok = false;
            double durationSec = rawValue.toDouble(&ok);
            if (!ok) return QVariant();
            int totalSec = static_cast<int>(durationSec);
            int minutes = totalSec / 60;
            int seconds = totalSec % 60;
            return QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
        }

        case AttributeFormatType::Double: {
            bool ok = false;
            double val = rawValue.toDouble(&ok);
            if (!ok) return QVariant();
            if (def && !def->measurementType.isEmpty())
                return UnitConverter::instance().formatValue(val, def->measurementType);
            return QString::number(val);
        }
        }
        break;
    }
    case ColumnType::MeasurementAtMarker:
    case ColumnType::Delta:
        return UnitConverter::instance().formatValue(rawValue.toDouble(), col.measurementType);
    }

    return QVariant();
}

QVariant SessionModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size() || index.column() >= m_columns.size())
        return QVariant();

    const SessionRow &row = m_rows.at(index.row());
    const LogbookColumn &col = m_columns[index.column()];

    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
        if (row.isLoaded()) {
            // Loaded row: use existing formatting logic
            const SessionData &item = row.session.value();
            switch (col.type) {
            case ColumnType::SessionAttribute:
                return formatAttributeValue(item, col);
            case ColumnType::MeasurementAtMarker:
                return formatMeasurementValue(item, col);
            case ColumnType::Delta:
                return formatDeltaValue(item, col);
            }
        } else {
            // Stub row: use cached values
            QVariant cached = row.cachedValues.value(index.column());
            if (!cached.isValid())
                return QVariant();
            return formatRawValue(cached, col);
        }
        break;

    case Qt::CheckStateRole:
        // Handle checkbox only for the first column
        if (index.column() == 0) {
            return row.visible ? Qt::Checked : Qt::Unchecked;
        }
        break;

    case CustomRoles::IsHoveredRole:
        return (row.sessionId == m_hoveredSessionId);

    default:
        break;
    }

    return QVariant();
}

QVariant SessionModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || section >= m_columns.size())
        return QVariant();

    if (role == Qt::DisplayRole) {
        QString label = logbookColumnLabel(m_columns[section]);
        QString unit = columnUnitLabel(m_columns[section]);
        if (!unit.isEmpty())
            return label + QStringLiteral("\n(") + unit + QStringLiteral(")");
        return label;
    }
    if (role == Qt::TextAlignmentRole)
        return QVariant::fromValue(Qt::AlignCenter);
    return QVariant();
}

Qt::ItemFlags SessionModel::flags(const QModelIndex &index) const {
    if (!index.isValid())
        return Qt::NoItemFlags;

    Qt::ItemFlags flags = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    if (index.column() == 0) {
        // The first column has a checkbox
        flags |= Qt::ItemIsUserCheckable;
    }

    const LogbookColumn &col = m_columns[index.column()];
    if (col.type == ColumnType::SessionAttribute) {
        const auto *def = AttributeRegistry::instance().findByKey(col.attributeKey);
        if (def && def->editable)
            flags |= Qt::ItemIsEditable;
    }
    // MeasurementAtMarker and Delta are never editable

    return flags;
}

bool SessionModel::setData(const QModelIndex &index, const QVariant &value, int role) {
    if (!index.isValid() || index.row() >= m_rows.size() || index.column() >= m_columns.size())
        return false;

    SessionRow &sr = m_rows[index.row()];
    const LogbookColumn &col = m_columns[index.column()];

    bool somethingChanged = false;
    bool attributeChanged = false;
    QSet<DependencyKey> visitedKeys;

    if (role == Qt::CheckStateRole && index.column() == 0) {
        // Update visibility based on the checkbox
        bool newVisible = (value.toInt() == Qt::Checked);
        if (sr.visible != newVisible) {
            sr.visible = newVisible;
            // Force-load if setting visible on a stub
            if (newVisible && !sr.isLoaded()) {
                sessionRef(index.row());
            }
            // Sync to loaded SessionData if present
            if (sr.isLoaded()) {
                sr.session->setVisible(newVisible);
            }
            // LRU transitions
            if (newVisible) {
                // Becoming visible: remove from LRU pool (now pinned)
                lruRemove(sr.sessionId);
            } else if (sr.isLoaded()) {
                // Becoming non-visible while loaded: enter LRU pool
                lruInsert(sr.sessionId);
                evictIfNeeded();
            }
            // Update lastAccessed when toggling visibility to true
            if (newVisible) {
                double now = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch() / 1000.0;
                LogbookManager::instance().setLastAccessed(sr.sessionId, now);
            }
            somethingChanged = true;
        }
    } else if (role == Qt::EditRole && col.type == ColumnType::SessionAttribute) {
        // Force-load before editing
        SessionData &item = sessionRef(index.row());

        // Look up the attribute definition
        const auto *def = AttributeRegistry::instance().findByKey(col.attributeKey);
        if (!def || !def->editable)
            return false;

        switch (def->formatType) {
        case AttributeFormatType::Text: {
            QString newVal = value.toString();
            QString oldVal = item.getAttribute(col.attributeKey).toString();
            if (oldVal != newVal) {
                visitedKeys = item.setAttribute(col.attributeKey, newVal);
                somethingChanged = true;
                attributeChanged = true;
            }
            break;
        }
        case AttributeFormatType::Double: {
            double displayVal = value.toDouble();
            double newVal = displayVal;
            if (def && !def->measurementType.isEmpty())
                newVal = UnitConverter::instance().reverseConvert(displayVal, def->measurementType);
            double oldVal = item.getAttribute(col.attributeKey).toDouble();
            if (oldVal != newVal) {
                visitedKeys = item.setAttribute(col.attributeKey, newVal);
                somethingChanged = true;
                attributeChanged = true;
            }
            break;
        }
        case AttributeFormatType::DateTime:
        case AttributeFormatType::Duration:
            // DateTime and Duration are not editable
            return false;
        }
    }

    if (somethingChanged) {
        emit dataChanged(index, index, {role});
        if (role == Qt::CheckStateRole) {
            QSet<QString> shown, hidden;
            if (sr.visible)
                shown.insert(sr.sessionId);
            else
                hidden.insert(sr.sessionId);
            emit visibilityChanged(shown, hidden);
        } else {
            emit modelChanged();
        }
        if (attributeChanged) {
            publishInvalidation(index.row(), visitedKeys);
            if (!sr.sessionId.isEmpty())
                scheduleSave(sr.sessionId);
        }
        return true;
    }

    return false;
}

void SessionModel::mergeSessions(const QList<SessionData>& sessions)
{
    if (sessions.isEmpty())
        return;

    // Names invalidated in sessions that were already loaded, by session id
    QHash<QString, QSet<DependencyKey>> mergeInvalidations;

    beginResetModel();
    for (const SessionData& newSession : sessions) {
        if (!newSession.hasAttribute(SessionKeys::SessionId)) {
            qWarning() << "Skipping session with no SESSION_ID";
            continue;
        }

        QString newSessionID = newSession.getAttribute(SessionKeys::SessionId).toString();

        auto rowIt = std::find_if(
            m_rows.begin(), m_rows.end(),
            [&newSessionID](const SessionRow &row) {
                return row.sessionId == newSessionID;
            });

        if (rowIt != m_rows.end()) {
            // Found existing row
            if (rowIt->isLoaded()) {
                // Merge into existing loaded session
                SessionData &existingSession = rowIt->session.value();

                // Everything the merge invalidates is published once the
                // model reset is complete.
                QSet<DependencyKey> &invalidated = mergeInvalidations[newSessionID];

                for (const QString &attributeKey : newSession.attributeKeys()) {
                    invalidated.unite(
                        existingSession.setAttribute(attributeKey, newSession.getAttribute(attributeKey)));
                }

                for (const QString &sensorKey : newSession.sensorKeys()) {
                    for (const QString &measurementKey : newSession.measurementKeys(sensorKey)) {
                        invalidated.unite(
                            existingSession.setMeasurement(sensorKey, measurementKey,
                                newSession.getMeasurement(sensorKey, measurementKey)));
                    }
                }

                if (!rowIt->dirty) {
                    rowIt->dirty = true;
                    m_saveHighWater++;
                    m_saveRemaining++;
                }
                qDebug() << "Merged SessionData into loaded row with SESSION_ID:" << newSessionID;
            } else {
                // Replace stub with loaded data
                rowIt->session = newSession;
                rowIt->visible = newSession.isVisible();
                rowIt->session->setVisible(rowIt->visible);
                attachSession(*rowIt);

                rowIt->dirty = true;
                m_saveHighWater++;
                m_saveRemaining++;
                qDebug() << "Replaced stub with loaded SessionData for SESSION_ID:" << newSessionID;
            }
        } else {
            // Add as new loaded row
            SessionRow newRow;
            newRow.sessionId = newSessionID;
            newRow.session = newSession;
            newRow.visible = newSession.isVisible();
            newRow.session->setVisible(newRow.visible);
            newRow.dirty = true;
            m_saveHighWater++;
            m_saveRemaining++;
            m_rows.append(std::move(newRow));
            attachSession(m_rows.last());   // the element in m_rows, not the local
            qDebug() << "Added new SessionData with SESSION_ID:" << newSessionID;
        }
    }
    endResetModel();

    // Tell subscribers what the merge invalidated in already-loaded sessions
    for (auto it = mergeInvalidations.cbegin(); it != mergeInvalidations.cend(); ++it) {
        for (const DependencyKey &key : it.value())
            emit dependencyChanged(it.key(), key);
    }

    // Cache column values for all loaded sessions so the index is populated
    LogbookManager &logbook = LogbookManager::instance();
    for (const SessionRow &row : std::as_const(m_rows)) {
        if (row.isLoaded()) {
            logbook.setCachedValues(row.sessionId, computeColumnValues(row.session.value()));
        }
    }

    // Wake the scheduler so the saver picks up newly dirty rows
    m_scheduler.wake();

    emit modelChanged();
}

void SessionModel::populateFromIndex(const QMap<QString, QMap<int, QVariant>> &cachedValues,
                                     const QMap<QString, double> &lastAccessed)
{
    Q_UNUSED(lastAccessed);

    beginResetModel();
    m_rows.clear();

    for (auto it = cachedValues.constBegin(); it != cachedValues.constEnd(); ++it) {
        SessionRow row;
        row.sessionId = it.key();
        row.cachedValues = it.value();
        row.session = std::nullopt;
        row.visible = false;
        m_rows.append(std::move(row));
    }

    endResetModel();
    emit modelChanged();
}

void SessionModel::populateFromUuids(const QStringList &uuids)
{
    beginResetModel();
    m_rows.clear();

    for (const QString &uuid : uuids) {
        SessionRow row;
        row.sessionId = uuid;
        row.cachedValues = {};
        row.session = std::nullopt;
        row.visible = false;
        m_rows.append(std::move(row));
    }

    endResetModel();
    emit modelChanged();
}

void SessionModel::setRowsVisibility(const QMap<int, bool>& rowVisibility)
{
    if (rowVisibility.isEmpty())
        return;

    int minRow = INT_MAX;
    int maxRow = 0;
    double now = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch() / 1000.0;
    QSet<QString> shownIds, hiddenIds;
    QList<QString> newStubs;  // stubs that need background loading

    for (auto it = rowVisibility.constBegin(); it != rowVisibility.constEnd(); ++it) {
        int row = it.key();
        bool visible = it.value();
        if (row >= 0 && row < m_rows.size()) {
            bool wasVisible = m_rows[row].visible;
            m_rows[row].visible = visible;

            if (visible && !m_rows[row].isLoaded()) {
                // Stub becoming visible: queue for background loading
                newStubs.append(m_rows[row].sessionId);
            }

            // Sync to loaded SessionData if present
            if (m_rows[row].isLoaded()) {
                m_rows[row].session->setVisible(visible);
            }

            // LRU transitions
            if (visible && !wasVisible) {
                // Becoming visible: remove from LRU pool (now pinned)
                lruRemove(m_rows[row].sessionId);
                if (m_rows[row].isLoaded())
                    shownIds.insert(m_rows[row].sessionId);
                // else: will be added to shownIds when loaded by the worker
            } else if (!visible && wasVisible) {
                hiddenIds.insert(m_rows[row].sessionId);
                // Remove from load queue if it was pending
                if (m_loadQueue.removeOne(m_rows[row].sessionId)) {
                    m_loadRemaining--;
                    m_loadHighWater--;
                }
                if (m_rows[row].isLoaded()) {
                    // Becoming non-visible while loaded: enter LRU pool
                    lruInsert(m_rows[row].sessionId);
                }
            }

            // Update lastAccessed when toggling visibility to true
            if (visible) {
                LogbookManager::instance().setLastAccessed(m_rows[row].sessionId, now);
            }
            minRow = std::min(minRow, row);
            maxRow = std::max(maxRow, row);
        }
    }

    // Handle stubs that need loading
    int totalPending = m_loadQueue.size() + newStubs.size();
    if (totalPending > 0 && totalPending <= kSyncLoadThreshold) {
        // Small batch: load everything synchronously (including any prior queue)
        for (const QString &id : std::as_const(m_loadQueue)) {
            int row = getSessionRow(id);
            if (row >= 0 && m_rows[row].visible && !m_rows[row].isLoaded()) {
                sessionRef(row);
                shownIds.insert(id);
            }
        }
        m_loadQueue.clear();
        m_loadHighWater = 0;
        m_loadRemaining = 0;

        for (const QString &id : std::as_const(newStubs)) {
            int row = getSessionRow(id);
            if (row >= 0 && m_rows[row].visible && !m_rows[row].isLoaded()) {
                sessionRef(row);
                shownIds.insert(id);
            }
        }
    } else if (!newStubs.isEmpty()) {
        // Large batch: append to background load queue
        m_loadQueue.append(newStubs);
        m_loadHighWater += newStubs.size();
        m_loadRemaining += newStubs.size();

        // Wake the scheduler so the loader picks up new work
        m_scheduler.wake();
    }

    // Enforce cache bounds once after all visibility changes
    evictIfNeeded();

    // Clamp load counters
    if (m_loadHighWater <= 0) {
        m_loadHighWater = 0;
        m_loadRemaining = 0;
    }

    if (minRow <= maxRow) {
        emit dataChanged(index(minRow, 0), index(maxRow, columnCount() - 1), {Qt::CheckStateRole});
        if (!shownIds.isEmpty() || !hiddenIds.isEmpty())
            emit visibilityChanged(shownIds, hiddenIds);
    }
}

bool SessionModel::removeSessions(const QList<QString> &sessionIds)
{
    if (sessionIds.isEmpty())
        return false;

    bool anyRemoved = false;

    beginResetModel();

    for (const QString &sessionId : sessionIds) {
        auto it = std::find_if(
            m_rows.begin(),
            m_rows.end(),
            [&sessionId](const SessionRow &row) {
                return row.sessionId == sessionId;
            });

        if (it != m_rows.end()) {
            // Remove from LRU list before erasing
            lruRemove(sessionId);

            // Update save counters if this session is dirty
            if (it->dirty) {
                m_saveHighWater--;
                m_saveRemaining--;
            }

            // Update load queue if this session was pending load
            if (m_loadQueue.removeOne(sessionId)) {
                m_loadRemaining--;
                m_loadHighWater--;
            }

            // Update column worker counters if this session has missing values
            if (m_columnWorkerRemaining > 0 && it->cachedValues.size() < m_columns.size()) {
                m_columnWorkerHighWater--;
                m_columnWorkerRemaining--;
            }

            m_rows.erase(it);

            anyRemoved = true;

            qDebug() << "Removed session with SESSION_ID:" << sessionId;
        } else {
            qWarning() << "SessionModel::removeSessions: SESSION_ID not found:" << sessionId;
        }
    }

    endResetModel();

    if (anyRemoved) {
        // Clamp counters to zero (should never go negative)
        if (m_saveHighWater <= 0) {
            m_saveHighWater = 0;
            m_saveRemaining = 0;
        }
        if (m_loadHighWater <= 0) {
            m_loadHighWater = 0;
            m_loadRemaining = 0;
        }
        if (m_columnWorkerHighWater <= 0) {
            m_columnWorkerHighWater = 0;
            m_columnWorkerRemaining = 0;
        }

        emit modelChanged();
    }

    return anyRemoved;
}

const SessionRow& SessionModel::rowAt(int row) const
{
    Q_ASSERT(row >= 0 && row < m_rows.size());
    return m_rows.at(row);
}

SessionRow& SessionModel::rowAt(int row)
{
    Q_ASSERT(row >= 0 && row < m_rows.size());
    return m_rows[row];
}

SessionData &SessionModel::sessionRef(int row)
{
    Q_ASSERT(row >= 0 && row < m_rows.size());
    SessionRow &sr = m_rows[row];
    if (!sr.isLoaded()) {
        auto loaded = LogbookManager::instance().loadSession(sr.sessionId);
        if (loaded.has_value()) {
            // Remap UUID-based session ID to real SESSION_ID if needed
            const QString realId = loaded->getAttribute(SessionKeys::SessionId).toString();
            if (!realId.isEmpty() && realId != sr.sessionId) {
                LogbookManager &logbook = LogbookManager::instance();
                if (logbook.remapSessionId(sr.sessionId, realId))
                    sr.sessionId = realId;
            }
            sr.session = std::move(loaded.value());
        } else {
            qWarning() << "SessionModel::sessionRef: failed to load session"
                        << sr.sessionId << "- creating empty SessionData";
            sr.session = SessionData();
        }
        // Sync visibility from SessionRow to the newly loaded SessionData
        sr.session->setVisible(sr.visible);

        // Listen for broadcast invalidations (after the id remap above, so the
        // listener captures the final session id)
        attachSession(sr);

        // Emit sessionLoaded for consistency
        emit sessionLoaded(sr.sessionId);

        // LRU tracking: only track non-visible, non-focused sessions
        if (!sr.visible && sr.sessionId != m_focusedSessionId) {
            lruTouch(sr.sessionId);
            evictIfNeeded();
        }
    } else {
        // Already loaded: bump recency for non-visible, non-focused sessions
        if (!sr.visible && sr.sessionId != m_focusedSessionId) {
            lruTouch(sr.sessionId);
        }
    }
    return sr.session.value();
}

QString SessionModel::hoveredSessionId() const
{
    return m_hoveredSessionId;
}

int SessionModel::getSessionRow(const QString& sessionId) const
{
    for (int row = 0; row < m_rows.size(); ++row) {
        if (m_rows[row].sessionId == sessionId) {
            return row;
        }
    }
    return -1;
}

void SessionModel::setHoveredSessionId(const QString& sessionId)
{
    if (m_hoveredSessionId == sessionId)
        return; // No change

    QString oldSessionId = m_hoveredSessionId;
    m_hoveredSessionId = sessionId;

    qDebug() << "SessionModel: hoveredSessionId changed from" << oldSessionId << "to" << m_hoveredSessionId;

    emit hoveredSessionChanged(sessionId);

    // Notify dataChanged for old hovered session (all columns)
    if (!oldSessionId.isEmpty()) {
        int oldRow = getSessionRow(oldSessionId);
        if (oldRow != -1) {
            QModelIndex topLeft = this->index(oldRow, 0);
            QModelIndex bottomRight = this->index(oldRow, columnCount() - 1);
            emit dataChanged(topLeft, bottomRight, {Qt::BackgroundRole, CustomRoles::IsHoveredRole});
        }
    }

    // Notify dataChanged for new hovered session (all columns)
    if (!sessionId.isEmpty()) {
        int newRow = getSessionRow(sessionId);
        if (newRow != -1) {
            QModelIndex topLeft = this->index(newRow, 0);
            QModelIndex bottomRight = this->index(newRow, columnCount() - 1);
            emit dataChanged(topLeft, bottomRight, {Qt::BackgroundRole, CustomRoles::IsHoveredRole});
        }
    }
}

QString SessionModel::focusedSessionId() const
{
    return m_focusedSessionId;
}

void SessionModel::setFocusedSessionId(const QString& sessionId)
{
    if (m_focusedSessionId == sessionId)
        return; // No change

    QString oldSessionId = m_focusedSessionId;
    m_focusedSessionId = sessionId;

    // Return old focused session to LRU pool if it's loaded and non-visible
    if (!oldSessionId.isEmpty()) {
        int oldRow = getSessionRow(oldSessionId);
        if (oldRow >= 0) {
            SessionRow &oldSr = m_rows[oldRow];
            if (oldSr.isLoaded() && !oldSr.visible)
                lruTouch(oldSessionId);
        }
    }

    // Load and pin the new focused session
    if (!sessionId.isEmpty()) {
        int row = getSessionRow(sessionId);
        if (row >= 0) {
            sessionRef(row);        // Load if stub
            lruRemove(sessionId);   // Remove from LRU so it won't be evicted
        }
    }

    evictIfNeeded();

    qDebug() << "SessionModel: focusedSessionId changed from" << oldSessionId << "to" << m_focusedSessionId;

    emit focusedSessionChanged(sessionId);
}

bool SessionModel::updateAttribute(const QString &sessionId,
                                   const QString &attributeKey,
                                   const QVariant &newValue)
{
    // 1. Locate the row for the given session ID
    int row = getSessionRow(sessionId);
    if (row < 0) {
        qWarning() << "SessionModel::updateAttribute: No session found with ID:" << sessionId;
        return false;
    }

    // 2. Force-load before mutating
    SessionData &session = sessionRef(row);

    // 3. Retrieve the existing value
    QVariant oldValue = session.getAttribute(attributeKey);

    // 4. Check if there's actually a change
    if (oldValue == newValue) {
        return false;  // Nothing to update
    }

    // 5. Update the attribute in SessionData (captures all BFS-visited keys)
    QSet<DependencyKey> visitedKeys = session.setAttribute(attributeKey, newValue);

    // 6. Notify views that data has changed, and emit fine-grained
    //    dependencyChanged for each invalidated name
    publishInvalidation(row, visitedKeys);

    // 7. Schedule deferred logbook save
    scheduleSave(sessionId);

    return true;
}

bool SessionModel::removeAttribute(const QString &sessionId,
                                   const QString &attributeKey)
{
    // 1. Locate the row for the given session ID
    int row = getSessionRow(sessionId);
    if (row < 0) {
        qWarning() << "SessionModel::removeAttribute: No session found with ID:" << sessionId;
        return false;
    }

    // 2. Force-load before mutating
    SessionData &session = sessionRef(row);

    // 3. If the attribute is not stored, there is nothing to remove
    if (!session.hasAttribute(attributeKey)) {
        return false;
    }

    // 4. Remove the attribute and capture all BFS-visited keys
    QSet<DependencyKey> visitedKeys = session.removeAttribute(attributeKey);

    // 5. Notify views that data has changed, and emit fine-grained
    //    dependencyChanged for each invalidated name
    publishInvalidation(row, visitedKeys);

    // 6. Schedule deferred logbook save
    scheduleSave(sessionId);

    return true;
}

// ---- Invalidation ------------------------------------------------------

void SessionModel::attachSession(SessionRow &sr)
{
    if (!sr.isLoaded())
        return;

    // The listener captures the model and the session id, never the address of
    // a row or a session: rows move when the vector grows or is sorted, and
    // the listener travels with the session's engine.
    const QString sessionId = sr.sessionId;
    sr.session->calculationEngine().setInvalidationListener(
        [this, sessionId](const QSet<DependencyKey> &keys) {
            queueInvalidation(sessionId, keys);
        });
}

void SessionModel::queueInvalidation(const QString &sessionId, const QSet<DependencyKey> &keys)
{
    // The values are already invalid: a read before the flush returns the new
    // value. The flush only tells views to read again, so it can be deferred
    // and coalesced.
    m_pendingInvalidations[sessionId].unite(keys);

    if (!m_invalidationFlushQueued) {
        m_invalidationFlushQueued = true;
        QMetaObject::invokeMethod(this, &SessionModel::flushPendingInvalidations, Qt::QueuedConnection);
    }
}

void SessionModel::flushPendingInvalidations()
{
    m_invalidationFlushQueued = false;

    QHash<QString, QSet<DependencyKey>> pending;
    pending.swap(m_pendingInvalidations);

    bool published = false;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        const int row = getSessionRow(it.key());
        if (row < 0 || !m_rows[row].isLoaded())
            continue;   // removed or evicted in the meantime

        publishInvalidation(row, it.value());

        // The cached logbook columns may be stale; the idle column worker
        // recomputes them from the in-memory session and updates the index.
        // Persistent state did not change, so the session is not marked dirty.
        m_rows[row].cachedValues.clear();
        published = true;
    }

    if (published) {
        startColumnWorker();
        emit modelChanged();
    }
}

void SessionModel::publishInvalidation(int row, const QSet<DependencyKey> &keys)
{
    Q_ASSERT(row >= 0 && row < m_rows.size());
    const QString sessionId = m_rows[row].sessionId;

    emit dataChanged(index(row, 0), index(row, columnCount() - 1),
                     {Qt::DisplayRole, Qt::EditRole, Qt::CheckStateRole});

    for (const DependencyKey &key : keys)
        emit dependencyChanged(sessionId, key);
}

// ---- Deferred logbook persistence ------------------------------------

void SessionModel::scheduleSave(const QString &sessionId)
{
    int row = getSessionRow(sessionId);
    if (row < 0) return;

    SessionRow &sr = m_rows[row];
    if (!sr.dirty) {
        sr.dirty = true;
        m_saveHighWater++;
        m_saveRemaining++;
    }
    // else: row is already dirty, data is updated in memory,
    // saver will save the current (modified) version when it reaches this row

    m_scheduler.wake();
}

void SessionModel::flushDirtySessions()
{
    // Clear all work sources so the scheduler finds nothing to do
    m_loadQueue.clear();
    m_loadedDuringBatch.clear();
    m_loadHighWater = 0;
    m_loadRemaining = 0;
    m_columnWorkerHighWater = 0;
    m_columnWorkerRemaining = 0;
    m_bulkEditQueue.clear();
    m_bulkEditHighWater = 0;
    m_bulkEditRemaining = 0;

    LogbookManager &logbook = LogbookManager::instance();
    bool anySaved = false;
    for (int i = 0; i < m_rows.size(); ++i) {
        SessionRow &sr = m_rows[i];
        if (!sr.dirty)
            continue;
        logbook.saveSession(sessionRef(i));
        if (sr.isLoaded()) {
            logbook.setCachedValues(sr.sessionId, computeColumnValues(sr.session.value()));
        }
        sr.dirty = false;
        anySaved = true;
    }
    if (anySaved) {
        logbook.flushIndex();
    }

    // Reset high-water-mark counters (UI is about to be destroyed during shutdown)
    m_saveHighWater = 0;
    m_saveRemaining = 0;
}

void SessionModel::saveNextSession()
{
    // Find the first dirty row
    int dirtyIdx = -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].dirty) {
            dirtyIdx = i;
            break;
        }
    }

    if (dirtyIdx < 0)
        return;

    SessionRow &sr = m_rows[dirtyIdx];
    LogbookManager &logbook = LogbookManager::instance();

    logbook.saveSession(sessionRef(dirtyIdx));
    if (sr.isLoaded()) {
        logbook.setCachedValues(sr.sessionId, computeColumnValues(sr.session.value()));
    }
    sr.dirty = false;

    m_saveRemaining--;
}

// ---- Background visibility loader --------------------------------------

void SessionModel::loadNextVisibleSession()
{
    // Skip invalid entries
    while (!m_loadQueue.isEmpty()) {
        const QString &frontId = m_loadQueue.first();
        int row = getSessionRow(frontId);
        if (row < 0 || !m_rows[row].visible || m_rows[row].isLoaded()) {
            m_loadQueue.removeFirst();
            m_loadRemaining--;
            continue;
        }
        break;
    }

    if (m_loadQueue.isEmpty())
        return;

    QString sessionId = m_loadQueue.takeFirst();
    int row = getSessionRow(sessionId);

    // Load one session
    sessionRef(row);
    m_loadedDuringBatch.insert(sessionId);

    m_loadRemaining--;
}

void SessionModel::cancelLoader()
{
    m_scheduler.cancel(LoadTask);
}

// ---- LRU cache management ---------------------------------------------

void SessionModel::lruTouch(const QString &sessionId)
{
    m_lruList.removeOne(sessionId);
    m_lruList.prepend(sessionId);
}

void SessionModel::lruRemove(const QString &sessionId)
{
    m_lruList.removeOne(sessionId);
}

void SessionModel::lruInsert(const QString &sessionId)
{
    m_lruList.removeOne(sessionId);
    m_lruList.prepend(sessionId);
}

void SessionModel::evictIfNeeded()
{
    while (m_lruList.size() > m_cacheCapacity) {
        QString sessionId = m_lruList.last();
        evictSession(sessionId);
    }
}

void SessionModel::evictSession(const QString &sessionId)
{
    // Remove from LRU list first
    lruRemove(sessionId);

    // Find the row
    int row = getSessionRow(sessionId);
    if (row < 0)
        return;

    SessionRow &sr = m_rows[row];

    // Already a stub -- nothing to do
    if (!sr.isLoaded())
        return;

    LogbookManager &logbook = LogbookManager::instance();

    // Save if dirty
    if (sr.dirty) {
        logbook.saveSession(sr.session.value());
        sr.dirty = false;

        // Update saver progress if saves are in flight
        if (m_saveRemaining > 0) {
            m_saveRemaining--;
        }
    }

    // Compute and cache column values before eviction
    QMap<LogbookColumn, QVariant> colValues = computeColumnValues(sr.session.value());
    logbook.setCachedValues(sr.sessionId, colValues);

    QMap<int, QVariant> indexed;
    for (int i = 0; i < m_columns.size(); ++i)
        indexed[i] = colValues.value(m_columns[i]);
    sr.cachedValues = indexed;

    // Reset to stub
    sr.session = std::nullopt;
}

// ---- Dirty column worker ----------------------------------------------

void SessionModel::startColumnWorker()
{
    // Count sessions with missing column values
    int dirtyCount = 0;
    for (const SessionRow &row : std::as_const(m_rows)) {
        if (row.cachedValues.size() < m_columns.size()) {
            dirtyCount++;
        }
    }

    if (dirtyCount == 0)
        return;

    m_columnWorkerHighWater = dirtyCount;
    m_columnWorkerRemaining = dirtyCount;

    m_scheduler.wake();
}

void SessionModel::cancelColumnWorker()
{
    m_scheduler.cancel(ColumnTask);
}

void SessionModel::processNextDirtyColumn()
{
    // Find next session with missing column values
    int dirtyIdx = -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        const SessionRow &row = m_rows[i];
        if (row.cachedValues.size() < m_columns.size()) {
            dirtyIdx = i;
            break;
        }
    }

    if (dirtyIdx < 0)
        return;

    SessionRow &row = m_rows[dirtyIdx];
    LogbookManager &logbook = LogbookManager::instance();

    QMap<LogbookColumn, QVariant> colValues;

    if (row.isLoaded()) {
        // Session is already loaded; compute from in-memory data
        colValues = computeColumnValues(row.session.value());
    } else {
        // Stub session: load temporarily via LogbookManager::loadSession()
        auto loaded = logbook.loadSession(row.sessionId);
        if (loaded.has_value()) {
            // Remap UUID-based session ID to real SESSION_ID if needed
            const QString realId = loaded->getAttribute(SessionKeys::SessionId).toString();
            if (!realId.isEmpty() && realId != row.sessionId) {
                if (logbook.remapSessionId(row.sessionId, realId))
                    row.sessionId = realId;
            }
            colValues = computeColumnValues(loaded.value());
        } else {
            // Load failed; skip this session
            m_columnWorkerRemaining--;
            return;
        }
        // Temporarily loaded session is discarded here
    }

    // Persist computed values
    logbook.setCachedValues(row.sessionId, colValues);

    // Rebuild index-based cached values map
    QMap<int, QVariant> indexed;
    for (int i = 0; i < m_columns.size(); ++i)
        indexed[i] = colValues.value(m_columns[i]);
    row.cachedValues = indexed;

    m_columnWorkerRemaining--;

    // Notify the view that this row has been updated
    emit dataChanged(index(dirtyIdx, 0), index(dirtyIdx, columnCount() - 1), {Qt::DisplayRole});
}

// ---- Bulk edit worker --------------------------------------------------

void SessionModel::startBulkEdit(const QList<int> &rows, int columnIndex, const QVariant &value)
{
    // Validate column
    if (columnIndex < 0 || columnIndex >= m_columns.size())
        return;
    const LogbookColumn &col = m_columns[columnIndex];
    if (col.type != ColumnType::SessionAttribute)
        return;
    const auto *def = AttributeRegistry::instance().findByKey(col.attributeKey);
    if (!def || !def->editable)
        return;

    if (rows.isEmpty())
        return;

    // Append work items to the queue (supports multiple successive edits)
    for (int row : rows)
        m_bulkEditQueue.append({row, columnIndex, value});

    m_bulkEditHighWater += rows.size();
    m_bulkEditRemaining += rows.size();

    // Wake the scheduler so the bulk edit worker picks up new work
    m_scheduler.wake();
}

void SessionModel::cancelBulkEdit()
{
    m_scheduler.cancel(BulkEditTask);
}

void SessionModel::processNextBulkEdit()
{
    // Skip invalid entries
    while (!m_bulkEditQueue.isEmpty()) {
        const BulkEditItem &front = m_bulkEditQueue.first();
        if (front.row < 0 || front.row >= m_rows.size() ||
            front.columnIndex < 0 || front.columnIndex >= m_columns.size()) {
            m_bulkEditQueue.removeFirst();
            m_bulkEditRemaining--;
            continue;
        }
        const LogbookColumn &col = m_columns[front.columnIndex];
        const auto *def = AttributeRegistry::instance().findByKey(col.attributeKey);
        if (!def) {
            m_bulkEditQueue.removeFirst();
            m_bulkEditRemaining--;
            continue;
        }
        // Check non-editable format types
        if (def->formatType != AttributeFormatType::Text &&
            def->formatType != AttributeFormatType::Double) {
            m_bulkEditQueue.removeFirst();
            m_bulkEditRemaining--;
            continue;
        }
        break;
    }

    if (m_bulkEditQueue.isEmpty())
        return;

    BulkEditItem item = m_bulkEditQueue.takeFirst();

    SessionRow &sr = m_rows[item.row];
    const LogbookColumn &col = m_columns[item.columnIndex];
    const auto *def = AttributeRegistry::instance().findByKey(col.attributeKey);
    LogbookManager &logbook = LogbookManager::instance();

    // Compute the final value (with unit reverse-conversion for Double)
    QVariant newVal;
    switch (def->formatType) {
    case AttributeFormatType::Text:
        newVal = item.value.toString();
        break;
    case AttributeFormatType::Double: {
        double displayVal = item.value.toDouble();
        if (!def->measurementType.isEmpty())
            newVal = UnitConverter::instance().reverseConvert(displayVal, def->measurementType);
        else
            newVal = displayVal;
        break;
    }
    default:
        m_bulkEditRemaining--;
        return;
    }

    if (sr.isLoaded()) {
        // --- LOADED PATH ---
        SessionData &session = sr.session.value();
        session.setAttribute(col.attributeKey, newVal);

        // Save inline
        logbook.saveSession(session);

        // Update cached column values
        QMap<LogbookColumn, QVariant> colValues = computeColumnValues(session);
        logbook.setCachedValues(sr.sessionId, colValues);
        QMap<int, QVariant> indexed;
        for (int i = 0; i < m_columns.size(); ++i)
            indexed[i] = colValues.value(m_columns[i]);
        sr.cachedValues = indexed;

        // Clear dirty flag if set — we just saved
        if (sr.dirty) {
            sr.dirty = false;
            m_saveRemaining--;
        }
    } else {
        // --- STUB PATH (avoids LRU/eviction) ---
        auto loaded = logbook.loadSession(sr.sessionId);
        if (loaded.has_value()) {
            // UUID remap (same pattern as column worker)
            const QString realId = loaded->getAttribute(SessionKeys::SessionId).toString();
            if (!realId.isEmpty() && realId != sr.sessionId) {
                if (logbook.remapSessionId(sr.sessionId, realId))
                    sr.sessionId = realId;
            }

            // Apply the edit
            loaded->setAttribute(col.attributeKey, newVal);

            // Save
            logbook.saveSession(loaded.value());

            // Compute and cache column values
            QMap<LogbookColumn, QVariant> colValues = computeColumnValues(loaded.value());
            logbook.setCachedValues(sr.sessionId, colValues);
            QMap<int, QVariant> indexed;
            for (int i = 0; i < m_columns.size(); ++i)
                indexed[i] = colValues.value(m_columns[i]);
            sr.cachedValues = indexed;

            // loaded goes out of scope — session stays a stub
        }
        // If load failed, silently skip
    }

    // Notify the view that this row has been updated
    emit dataChanged(index(item.row, 0), index(item.row, columnCount() - 1), {Qt::DisplayRole});

    // Update progress
    m_bulkEditRemaining--;
}

void SessionModel::finishBulkEdit()
{
    LogbookManager::instance().flushIndex();

    // Reset state
    m_bulkEditHighWater = 0;
    m_bulkEditRemaining = 0;
    m_bulkEditQueue.clear();

    emit modelChanged();
}

// ---- Column value extraction ------------------------------------------

QMap<LogbookColumn, QVariant> SessionModel::computeColumnValues(const SessionData &session) const
{
    QMap<LogbookColumn, QVariant> result;
    for (const LogbookColumn &col : std::as_const(m_columns)) {
        QVariant value;
        switch (col.type) {
        case ColumnType::SessionAttribute:
            value = session.getAttribute(col.attributeKey);
            break;
        case ColumnType::MeasurementAtMarker: {
            QString interpKey = SessionData::interpolationKey(
                col.markerAttributeKey, col.sensorID, SessionKeys::Time, col.measurementID);
            value = session.getAttribute(interpKey);
            break;
        }
        case ColumnType::Delta: {
            QString interpKey1 = SessionData::interpolationKey(
                col.markerAttributeKey, col.sensorID, SessionKeys::Time, col.measurementID);
            QString interpKey2 = SessionData::interpolationKey(
                col.marker2AttributeKey, col.sensorID, SessionKeys::Time, col.measurementID);
            QVariant v1 = session.getAttribute(interpKey1);
            QVariant v2 = session.getAttribute(interpKey2);
            if (v1.isValid() && v2.isValid())
                value = v2.toDouble() - v1.toDouble();
            break;
        }
        }
        result[col] = value;
    }
    return result;
}

// ----------------------------------------------------------------------

void SessionModel::sort(int column, Qt::SortOrder order)
{
    if (column < 0 || column >= m_columns.size())
        return;

    const LogbookColumn &col = m_columns[column];

    // Lambda to extract raw value from a SessionRow (handles both stubs and loaded rows)
    auto getRawValue = [&col, column](const SessionRow &sr) -> QVariant {
        if (sr.isLoaded()) {
            const SessionData &s = sr.session.value();
            switch (col.type) {
            case ColumnType::SessionAttribute:
                return s.getAttribute(col.attributeKey);
            case ColumnType::MeasurementAtMarker: {
                QString interpKey = SessionData::interpolationKey(
                    col.markerAttributeKey, col.sensorID, SessionKeys::Time, col.measurementID);
                return s.getAttribute(interpKey);
            }
            case ColumnType::Delta: {
                QString interpKey1 = SessionData::interpolationKey(
                    col.markerAttributeKey, col.sensorID, SessionKeys::Time, col.measurementID);
                QString interpKey2 = SessionData::interpolationKey(
                    col.marker2AttributeKey, col.sensorID, SessionKeys::Time, col.measurementID);
                QVariant v1 = s.getAttribute(interpKey1);
                QVariant v2 = s.getAttribute(interpKey2);
                if (!v1.isValid() || !v2.isValid())
                    return QVariant();
                return v2.toDouble() - v1.toDouble();
            }
            }
        } else {
            // Stub: use cached values
            return sr.cachedValues.value(column);
        }
        return QVariant();
    };

    // Determine whether to use string or numeric comparison
    bool useStringCompare = false;
    if (col.type == ColumnType::SessionAttribute) {
        const auto *def = AttributeRegistry::instance().findByKey(col.attributeKey);
        if (def && def->formatType == AttributeFormatType::Text)
            useStringCompare = true;
    }

    beginResetModel();

    std::sort(m_rows.begin(), m_rows.end(),
              [&getRawValue, useStringCompare, order](const SessionRow &a, const SessionRow &b) {
        QVariant va = getRawValue(a);
        QVariant vb = getRawValue(b);

        bool aValid = va.isValid();
        bool bValid = vb.isValid();

        // Missing values sort to bottom in both ascending and descending order
        if (!aValid && !bValid) return false;
        if (!aValid) return false; // a goes to bottom
        if (!bValid) return true;  // b goes to bottom

        int result;
        if (useStringCompare) {
            result = QString::compare(va.toString(), vb.toString(), Qt::CaseInsensitive);
        } else {
            bool okA = false, okB = false;
            double da = va.toDouble(&okA);
            double db = vb.toDouble(&okB);
            if (!okA && !okB) result = 0;
            else if (!okA) result = -1;
            else if (!okB) result = 1;
            else result = (da < db) ? -1 : (da == db ? 0 : 1);
        }

        if (order == Qt::AscendingOrder)
            return result < 0;
        else
            return result > 0;
    });

    endResetModel();
}

QString SessionModel::columnUnitLabel(const LogbookColumn &col) const
{
    switch (col.type) {
    case ColumnType::MeasurementAtMarker:
    case ColumnType::Delta:
        return UnitConverter::instance().getUnitLabel(col.measurementType);
    case ColumnType::SessionAttribute: {
        const auto *def = AttributeRegistry::instance().findByKey(col.attributeKey);
        if (def && !def->measurementType.isEmpty())
            return UnitConverter::instance().getUnitLabel(def->measurementType);
        return QString();
    }
    }
    return QString();
}

} // namespace FlySight
