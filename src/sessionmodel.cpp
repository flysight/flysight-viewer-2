#include "sessionmodel.h"

#include <algorithm>

#include <QDateTime>
#include <QTimeZone>

#include "attributeregistry.h"
#include "calculations/builtincalculations.h"
#include "csvformat.h"
#include "dataimporter.h"
#include "engine/calculationengine.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessionmerge.h"
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
                                       [](const SessionRow &r) { return r.dirty && !r.saveFailed; }); },
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
                                [this](const SessionRow &r) { return needsColumnWork(r); });
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
        } else if (CalculationRegistry::instance().declaredPreferenceKeys().contains(key)) {
            // A declared preference is part of the calculation environment
            queueEnvironmentCheck();
        }
    });

    // Registrations are part of the calculation environment too. One user
    // action can cause many registry changes; the check is coalesced.
    m_registryObserver = CalculationRegistry::instance().addObserver([this]() {
        queueEnvironmentCheck();
    });

    // Writing or deleting a record drops the cached values over it (see CACHED
    // COLUMN VALUES). Direct: the slot only drops state and defers.
    connect(&LogbookManager::instance(), &LogbookManager::calculationRecordsChanged,
            this, &SessionModel::onCalculationRecordsChanged);

    rebuildColumns();
}

SessionModel::~SessionModel()
{
    CalculationRegistry::instance().removeObserver(m_registryObserver);
}

void SessionModel::rebuildColumns()
{
    beginResetModel();
    m_columns = LogbookColumnStore::instance().enabledColumns();
    rebuildColumnDependencies();

    QVector<int> allIndices;
    allIndices.reserve(m_columns.size());
    for (int i = 0; i < m_columns.size(); ++i)
        allIndices.append(i);

    // Rebuild cached values for all sessions to reflect new column set
    LogbookManager &logbook = LogbookManager::instance();
    // Computes `indices` of a loaded row from its session and makes them the
    // row's and the manager's cached values (replacing what either held)
    const auto computeRow = [this, &logbook](SessionRow &row, const QVector<int> &indices) {
        const QMap<LogbookColumn, QVariant> colValues = computeColumnValues(row.session.value(), indices);
        logbook.setCachedValues(row.sessionId, colValues);
        QMap<int, QVariant> indexed;
        for (int i : indices)
            indexed[i] = colValues.value(m_columns[i]);
        row.cachedValues = indexed;
    };
    for (int rowIndex = 0; rowIndex < m_rows.size(); ++rowIndex) {
        SessionRow &row = m_rows[rowIndex];
        // Indices change: the worker settles stubs again (without a load when
        // only those columns are missing)
        row.pendingColumns.clear();
        if (row.isLoaded() && row.loadFailed) {
            // A failed-load placeholder's engine holds no stored result: its
            // explicit-backed columns are settled like a stub's
            QVector<int> indices;
            for (int i : std::as_const(allIndices)) {
                if (!isExplicitBacked(i))
                    indices.append(i);
            }
            computeRow(row, indices);
            settleExplicitColumns(rowIndex);
        } else if (row.isLoaded()) {
            // A row with unsaved changes: the marks name the columns that were
            // enabled when the change was made, and a column enabled since may
            // depend on it just the same. Keep everything out of the index
            // until the session has been saved.
            if (row.dirty || logbook.hasUnsavedColumns(row.sessionId))
                logbook.markSessionUnsaved(row.sessionId);

            // Loaded session: recompute all column values from in-memory data
            // (the one place that computes a whole row)
            computeRow(row, allIndices);
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
    QSet<DependencyKey> changedNames;

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
            // A session file is a line format: line breaks become spaces here,
            // so that memory and disk agree (the exporter would do it anyway).
            QString newVal = CsvFormat::singleLine(value.toString());
            QString oldVal = item.getAttribute(col.attributeKey).toString();
            if (oldVal != newVal) {
                changedNames = item.setAttribute(col.attributeKey, newVal);
                invalidateColumns(index.row(), {DependencyKey::attribute(col.attributeKey)});
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
                changedNames = item.setAttribute(col.attributeKey, newVal);
                invalidateColumns(index.row(), {DependencyKey::attribute(col.attributeKey)});
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
            publishInvalidation(index.row(), changedNames);
            if (!sr.sessionId.isEmpty())
                scheduleSave(sr.sessionId);
        }
        return true;
    }

    return false;
}

QList<MergeResult> SessionModel::mergeSessions(const QList<SessionData> &sessions)
{
    QList<ParsedFile> files;
    files.reserve(sessions.size());
    for (const SessionData &session : sessions)
        files.append(ParsedFile::fromSession(session));
    return mergeSessions(files);
}

QList<MergeResult> SessionModel::mergeSessions(const QList<ParsedFile> &files)
{
    QList<MergeResult> results;
    if (files.isEmpty())
        return results;
    assertRowsMutable("SessionModel::mergeSessions");
    results.reserve(files.size());

    LogbookManager &logbook = LogbookManager::instance();

    // Rows still known by their file stem must be matchable by SESSION_ID
    resolveIdentityStubs();

    // Rows move when m_rows grows: effects are recorded by session id and the
    // rows are looked up again after the loop.
    QStringList createdIds;
    QStringList mergedIds;                                  // first-seen order
    QHash<QString, QSet<DependencyKey>> mergedKeys;
    QStringList newlyLoadedIds;

    // The model is reset only when something changes: a batch of no-ops and
    // failures emits nothing (a reset costs views their selection and scroll
    // position).
    bool resetBegun = false;
    auto beginMutation = [this, &resetBegun]() {
        if (!resetBegun) {
            beginResetModel();
            resetBegun = true;
        }
    };
    auto recordMerge = [&mergedIds, &mergedKeys](const QString &sessionId, const QSet<DependencyKey> &keys) {
        if (!mergedKeys.contains(sessionId))
            mergedIds.append(sessionId);
        mergedKeys[sessionId].unite(keys);
    };

    // In list order: a later file sees the effects of the earlier ones, which
    // is what makes TRACK + SENSOR of one new session work in one batch.
    for (const ParsedFile &file : files) {
        MergeResult result;
        result.filePath = file.filePath;
        result.sessionId = file.sessionId;

        auto fail = [&result](const QString &error, const QString &hint = QString()) {
            result.outcome = MergeResult::Outcome::Failed;
            result.error = error;
            result.hint = hint;
        };

        const int rowIndex = file.sessionId.isEmpty() ? -1 : getSessionRow(file.sessionId);

        if (file.sessionId.isEmpty()) {
            fail(QStringLiteral("File has no SESSION_ID"));
        } else if (rowIndex < 0) {
            // ---- no such session: create one. The only place import-time
            // defaults are applied.
            SessionData created = file.data;
            if (file.applyCreationDefaults)
                DataImporter::applyCreationDefaults(file, created);

            beginMutation();
            SessionRow newRow;
            newRow.sessionId = file.sessionId;
            newRow.visible = created.isVisible();
            newRow.session = std::move(created);
            newRow.session->setVisible(newRow.visible);
            m_rows.append(std::move(newRow));
            // Its logbook identity from now on, so that a result published
            // before the first save is stored (see STORED RESULTS). A new
            // session has no records: nothing to restore.
            logbook.reserveSessionFile(file.sessionId);
            attachSession(m_rows.last());   // the element in m_rows, not the local

            createdIds.append(file.sessionId);
            result.outcome = MergeResult::Outcome::Created;
        } else if (m_rows[rowIndex].isLoaded() && !m_rows[rowIndex].loadFailed) {
            // ---- loaded session: plan, then apply in place ----
            const MergePlan plan = SessionMerge::plan(*m_rows[rowIndex].session, file.data);
            if (!plan.ok()) {
                fail(plan.error, plan.hint);
            } else if (plan.isEmpty()) {
                result.outcome = MergeResult::Outcome::Unchanged;
            } else {
                beginMutation();
                // No restore: the merge uses the ordinary setters, so a
                // stored result it does not touch stays installed with its
                // record, and one it touches is dropped by the input change,
                // which deletes its record.
                QSet<DependencyKey> keys = SessionMerge::apply(*m_rows[rowIndex].session, plan);
                keys.unite(plan.changedKeys());
                recordMerge(file.sessionId, keys);
                result.outcome = MergeResult::Outcome::Merged;
            }
        } else {
            // ---- unloaded session (or a failed-load placeholder): load what is
            // on disk, merge into that copy, install it only on success. Not
            // through sessionRef(): that would install the session, emit, touch
            // the LRU, and apply the backfill before the merge was decided.
            QString reason;
            std::optional<SessionData> loaded = logbook.loadSessionRaw(file.sessionId, &reason);
            if (loaded.has_value()) {
                const QString loadedId = loaded->storedAttribute(SessionKeys::SessionId).toString();
                if (loadedId != file.sessionId) {
                    reason = QStringLiteral("the logbook file belongs to session '%1'").arg(loadedId);
                    loaded.reset();
                }
            }

            if (!loaded.has_value()) {
                // Never a reason to replace the session with the incoming file
                fail(QStringLiteral("Existing session '%1' could not be loaded (%2); the file was not imported.")
                         .arg(file.sessionId, reason));
            } else {
                const MergePlan plan = SessionMerge::plan(*loaded, file.data);
                if (!plan.ok()) {
                    fail(plan.error, plan.hint);                        // the copy is discarded
                } else if (plan.isEmpty()) {
                    result.outcome = MergeResult::Outcome::Unchanged;   // the row stays a stub
                } else {
                    QSet<DependencyKey> keys = SessionMerge::apply(*loaded, plan);
                    keys.unite(plan.changedKeys());

                    // After the merge decision, never before it
                    LogbookManager::applyLegacyBackfill(*loaded);

                    beginMutation();
                    SessionRow &row = m_rows[rowIndex];
                    row.session = std::move(*loaded);
                    row.loadFailed = false;
                    row.session->setVisible(row.visible);
                    attachSession(row);
                    // Checked against the merged state; the names it
                    // invalidated are published with the merge's own.
                    keys.unite(restoreStoredResults(row));

                    // The session stays loaded: it is dirty and is saved from
                    // memory. Normal LRU accounting; one eviction pass per batch.
                    if (!row.visible && row.sessionId != m_focusedSessionId)
                        lruTouch(row.sessionId);

                    if (!newlyLoadedIds.contains(file.sessionId))
                        newlyLoadedIds.append(file.sessionId);
                    recordMerge(file.sessionId, keys);
                    result.outcome = MergeResult::Outcome::Merged;
                }
            }
        }

        switch (result.outcome) {
        case MergeResult::Outcome::Created:
            qDebug().noquote() << "Import:" << file.filePath << "created session" << file.sessionId;
            break;
        case MergeResult::Outcome::Merged:
            qDebug().noquote() << "Import:" << file.filePath << "merged into session" << file.sessionId;
            break;
        case MergeResult::Outcome::Unchanged:
            qDebug().noquote() << "Import:" << file.filePath << "changes nothing in session" << file.sessionId;
            break;
        case MergeResult::Outcome::Failed:
            qDebug().noquote() << "Import:" << file.filePath << "failed:" << result.errorWithHint();
            break;
        }
        results.append(result);
    }

    if (!resetBegun)
        return results;     // nothing changed: nothing is emitted, marked, or scheduled

    endResetModel();

    // ---- effects of a merge (columns invalidated, the session saved, views
    // notified of every changed name), through the model's normal paths. Column
    // invalidation comes before scheduleSave: the unsaved marks must exist
    // before the save runs.
    for (const QString &sessionId : std::as_const(createdIds)) {
        const int row = getSessionRow(sessionId);
        if (row < 0)
            continue;
        invalidateAllColumns(row);
        scheduleSave(sessionId);
    }

    for (const QString &sessionId : std::as_const(mergedIds)) {
        const int row = getSessionRow(sessionId);
        if (row < 0)
            continue;
        const QSet<DependencyKey> keys = mergedKeys.value(sessionId);
        invalidateColumns(row, keys);
        publishInvalidation(row, keys);
        scheduleSave(sessionId);
    }

    for (const QString &sessionId : std::as_const(newlyLoadedIds))
        emit sessionLoaded(sessionId);

    evictIfNeeded();
    startColumnWorker();

    emit modelChanged();
    return results;
}

void SessionModel::resolveIdentityStubs()
{
    LogbookManager &logbook = LogbookManager::instance();

    for (SessionRow &row : m_rows) {
        if (row.isLoaded() || !logbook.isIdentityEntry(row.sessionId))
            continue;

        // Header-only read; the cached values move with the remap. The id is
        // not displayed, so there is nothing to signal.
        const std::optional<QString> realId = logbook.peekSessionId(row.sessionId);
        if (!realId.has_value() || realId->isEmpty() || *realId == row.sessionId)
            continue;
        setRowSessionId(row, *realId);
    }
}

bool SessionModel::setRowSessionId(SessionRow &sr, const QString &realId)
{
    const QString oldId = sr.sessionId;
    if (!LogbookManager::instance().remapSessionId(oldId, realId))
        return false;
    sr.sessionId = realId;

    // Queued bulk edits find their row by id
    for (BulkEditItem &item : m_bulkEditQueue) {
        if (item.sessionId == oldId)
            item.sessionId = realId;
    }
    return true;
}

void SessionModel::populateFromIndex(const QMap<QString, QMap<int, QVariant>> &cachedValues,
                                     const QMap<QString, double> &lastAccessed)
{
    Q_UNUSED(lastAccessed);
    assertRowsMutable("SessionModel::populateFromIndex");

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
    assertRowsMutable("SessionModel::populateFromUuids");

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
    assertRowsMutable("SessionModel::removeSessions");

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

            // Update save counters if this session is queued for saving (a
            // row whose save failed is dirty but no longer counted)
            if (it->dirty && !it->saveFailed) {
                m_saveHighWater--;
                m_saveRemaining--;
            }

            // Update load queue if this session was pending load
            if (m_loadQueue.removeOne(sessionId)) {
                m_loadRemaining--;
                m_loadHighWater--;
            }

            // Update column worker counters if this session has missing values
            if (m_columnWorkerRemaining > 0 && needsColumnWork(*it)) {
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

void SessionModel::forEachLoadedSession(const QStringList &sessionIds,
                                        const std::function<void(const SessionData &)> &fn) const
{
    const RowStabilityGuard guard(*this);
    for (const QString &sessionId : sessionIds) {
        // Resolved per visit: no row or session reference outlives one call of fn
        const int row = getSessionRow(sessionId);
        if (row < 0 || !m_rows.at(row).isLoaded())
            continue;
        fn(m_rows.at(row).session.value());
    }
}

const SessionData *SessionModel::loadedSession(const QString &sessionId) const
{
    Q_ASSERT_X(m_rowStabilityDepth > 0, "SessionModel::loadedSession",
               "the returned pointer is only valid under a RowStabilityGuard");
    if (sessionId.isEmpty())
        return nullptr;
    const int row = getSessionRow(sessionId);
    if (row < 0 || !m_rows.at(row).isLoaded())
        return nullptr;
    return &m_rows.at(row).session.value();
}

SessionData &SessionModel::sessionRef(int row)
{
    Q_ASSERT(row >= 0 && row < m_rows.size());
    SessionRow &sr = m_rows[row];
    if (!sr.isLoaded()) {
        assertRowsMutable("SessionModel::sessionRef (load)");   // loads, and may evict another row
        auto loaded = LogbookManager::instance().loadSession(sr.sessionId);
        if (loaded.has_value()) {
            // Remap UUID-based session ID to real SESSION_ID if needed
            const QString realId = loaded->getAttribute(SessionKeys::SessionId).toString();
            if (!realId.isEmpty() && realId != sr.sessionId)
                setRowSessionId(sr, realId);
            sr.session = std::move(loaded.value());
        } else {
            qWarning() << "SessionModel::sessionRef: failed to load session"
                        << sr.sessionId << "- creating empty SessionData";
            // A placeholder, only because this function returns a reference.
            // It is never attached, saved, or merged into (SessionRow::loadFailed).
            sr.session = SessionData();
            sr.loadFailed = true;
        }
        // Sync visibility from SessionRow to the newly loaded SessionData
        sr.session->setVisible(sr.visible);

        // Listen for broadcast invalidations (after the id remap above, so the
        // listener captures the final session id). Stored results go in
        // before sessionLoaded: no reader ever sees the row without them. The
        // names the restore invalidated are not published: nothing outside
        // the model has read this engine yet, because the row is published
        // only by the sessionLoaded below.
        if (!sr.loadFailed) {
            attachSession(sr);
            restoreStoredResults(sr);
        }

        // Emit sessionLoaded for consistency
        emit sessionLoaded(sr.sessionId);

        // LRU tracking: only track non-visible, non-focused sessions
        if (!sr.visible && sr.sessionId != m_focusedSessionId) {
            lruTouch(sr.sessionId);
            evictIfNeeded(sr.sessionId);    // never the session about to be returned
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

    // Text is kept on one line: a session file is a line format, and memory
    // should equal what a reload would give.
    QVariant value = newValue;
    if (value.typeId() == QMetaType::QString)
        value = CsvFormat::singleLine(value.toString());

    // 3. Retrieve the existing value
    QVariant oldValue = session.getAttribute(attributeKey);

    // 4. Check if there's actually a change
    if (oldValue == value) {
        return false;  // Nothing to update
    }

    // 5. Update the attribute in SessionData (captures all BFS-visited keys)
    QSet<DependencyKey> changedNames = session.setAttribute(attributeKey, value);
    invalidateColumns(row, {DependencyKey::attribute(attributeKey)});

    // 6. Notify views that data has changed, and emit fine-grained
    //    dependencyChanged for each invalidated name
    publishInvalidation(row, changedNames);

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
    QSet<DependencyKey> changedNames = session.removeAttribute(attributeKey);
    invalidateColumns(row, {DependencyKey::attribute(attributeKey)});

    // 5. Notify views that data has changed, and emit fine-grained
    //    dependencyChanged for each invalidated name
    publishInvalidation(row, changedNames);

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
    // the listener travels with the session's engine. The explicit-result
    // listener also captures the engine it is installed on: it is owned by
    // that engine and moves with it.
    const QString sessionId = sr.sessionId;
    sr.session->calculationEngine().setInvalidationListener(
        [this, sessionId](const QSet<DependencyKey> &keys) {
            queueInvalidation(sessionId, keys);
        });

    // Stored results (see STORED RESULTS): both install paths (a job's
    // publish and a synchronous request) reach this listener.
    CalculationEngine &engine = sr.session->calculationEngine();
    engine.setExplicitResultListener(
        [this, sessionId, &engine](const CalculationEngine::ExplicitResultEvent &event) {
            m_resultStore.onExplicitResultEvent(sessionId, engine, event);
        });
}

QSet<DependencyKey> SessionModel::restoreStoredResults(SessionRow &sr)
{
    Q_ASSERT(sr.isLoaded() && !sr.loadFailed);

    // Pending columns were waiting for exactly this: they are computed from
    // the engine on the next pass. The row's cached values stay: they were
    // valid for the records on disk, and the restore installs exactly those
    // (a stale record it deletes, or one it skips, drops its values through
    // the manager).
    const bool hadPending = !sr.pendingColumns.isEmpty();
    sr.pendingColumns.clear();
    const QString sessionId = sr.sessionId;     // sr is not used after the restore
    const QSet<DependencyKey> invalidated =
        m_resultStore.restoreSession(sessionId, sr.session->calculationEngine()).invalidated;
    if (hadPending)
        queueRecordColumnRefresh(sessionId);
    return invalidated;
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
        published = true;
    }

    if (published)
        emit modelChanged();

    // Every broadcast invalidation (declared preference, registration change)
    // is by construction an environment change. The environment handler deals
    // with the cached logbook columns of loaded AND unloaded rows in one place.
    if (m_environmentCheckPending)
        checkCalculationEnvironment();
}

void SessionModel::queueEnvironmentCheck()
{
    m_environmentCheckPending = true;

    if (!m_invalidationFlushQueued) {
        m_invalidationFlushQueued = true;
        QMetaObject::invokeMethod(this, &SessionModel::flushPendingInvalidations, Qt::QueuedConnection);
    }
}

void SessionModel::checkCalculationEnvironment()
{
    m_environmentCheckPending = false;

    // Which columns a name can affect follows the registrations.
    rebuildColumnDependencies();

    LogbookManager &logbook = LogbookManager::instance();

    // Unchanged covers A -> B -> A within one event-loop pass, and the startup
    // registrations, which are complete before LogbookManager::initialize().
    if (calculationEnvironmentFingerprint() == logbook.cacheEnvironment())
        return;

    // Coarse on purpose: every cached value of every row, loaded or not. Rows
    // are not marked dirty or unsaved - persistent state did not change, so
    // the values the worker recomputes are valid for the files on disk.
    logbook.discardCachedValues();
    for (SessionRow &row : m_rows) {
        row.cachedValues.clear();
        row.pendingColumns.clear();
    }

    if (!m_rows.isEmpty() && !m_columns.isEmpty())
        emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {Qt::DisplayRole});

    startColumnWorker();
}

// ---- Cached column values: per-column refresh ---------------------------

void SessionModel::rebuildColumnDependencies()
{
    const CalculationRegistry &registry = CalculationRegistry::instance();

    m_columnDependencies.clear();
    m_columnDependencies.reserve(m_columns.size());
    m_columnExplicitCalculations.clear();
    m_columnExplicitCalculations.reserve(m_columns.size());
    for (const LogbookColumn &col : std::as_const(m_columns)) {
        StaticDependencies deps;
        const QList<DependencyKey> names = logbookColumnNames(col);
        for (const DependencyKey &name : names) {
            const StaticDependencies closure = registry.staticDependencies(name);
            deps.names.unite(closure.names);
            deps.preferences.unite(closure.preferences);
        }
        m_columnDependencies.append(deps);
        m_columnExplicitCalculations.append(logbookColumnExplicitCalculations(col, registry));
    }
}

bool SessionModel::isExplicitBacked(int column) const
{
    return column >= 0 && column < m_columnExplicitCalculations.size()
        && !m_columnExplicitCalculations[column].isEmpty();
}

bool SessionModel::needsColumnWork(const SessionRow &row) const
{
    for (int i = 0; i < m_columns.size(); ++i) {
        if (!row.cachedValues.contains(i) && !row.pendingColumns.contains(i))
            return true;
    }
    return false;
}

void SessionModel::invalidateColumns(int row, const QSet<DependencyKey> &changedKeys)
{
    Q_ASSERT(row >= 0 && row < m_rows.size());
    SessionRow &sr = m_rows[row];
    LogbookManager &logbook = LogbookManager::instance();

    // Values cached for columns that are not enabled right now cannot be
    // checked against the change; they go, and are recomputed if the column
    // comes back.
    logbook.dropCachedValuesExcept(sr.sessionId, m_columns);

    QVector<LogbookColumn> affected;
    for (int i = 0; i < m_columns.size() && i < m_columnDependencies.size(); ++i) {
        if (!m_columnDependencies[i].names.intersects(changedKeys))
            continue;
        sr.cachedValues.remove(i);
        sr.pendingColumns.remove(i);
        affected.append(m_columns[i]);
    }

    if (affected.isEmpty())
        return;

    logbook.markColumnsUnsaved(sr.sessionId, affected);
    startColumnWorker();
}

void SessionModel::invalidateAllColumns(int row)
{
    Q_ASSERT(row >= 0 && row < m_rows.size());
    SessionRow &sr = m_rows[row];

    sr.cachedValues.clear();
    sr.pendingColumns.clear();
    LogbookManager::instance().markSessionUnsaved(sr.sessionId);
    startColumnWorker();
}

void SessionModel::fillMissingColumns(int row, const SessionData &session, ColumnSource source)
{
    Q_ASSERT(row >= 0 && row < m_rows.size());
    SessionRow &sr = m_rows[row];

    // An engine that holds no stored result says nothing about a record:
    // explicit-backed columns are settled from the record set instead.
    if (source == ColumnSource::TemporaryLoad || sr.loadFailed)
        settleExplicitColumns(row);

    QVector<int> missing;
    for (int i = 0; i < m_columns.size(); ++i) {
        if (!sr.cachedValues.contains(i) && !sr.pendingColumns.contains(i))
            missing.append(i);
    }

    // Nothing to do: do not even ask for the engine, which would create it.
    if (missing.isEmpty())
        return;

    const int runsBefore = session.calculationEngine().totalRunCount();
    const QMap<LogbookColumn, QVariant> values = computeColumnValues(session, missing);
    m_columnWorkStats.calculationRuns += session.calculationEngine().totalRunCount() - runsBefore;
    m_columnWorkStats.valuesComputed += int(missing.size());

    LogbookManager::instance().updateCachedValues(sr.sessionId, values);
    for (int i : std::as_const(missing))
        sr.cachedValues[i] = values.value(m_columns[i]);
}

void SessionModel::settleExplicitColumns(int row)
{
    Q_ASSERT(row >= 0 && row < m_rows.size());
    SessionRow &sr = m_rows[row];
    LogbookManager &logbook = LogbookManager::instance();

    // The row's current id: an identity row's is its file stem, under which
    // the manager knows its records (a later remap moves them)
    const QSet<QString> known = logbook.knownCalculationRecords(sr.sessionId);

    // "No record, so unavailable" relies on an invariant nothing enforces:
    // an Explicit calculation's outputs have no other candidate, so while it
    // is not requested (no record: nothing restores it) they read as
    // unavailable, and so does every column over them (docs/CALCULATIONS.md
    // section 8; checked for the registered built-ins by
    // tst_fusion_session::explicitOutputsHaveOneCandidate). A registration
    // that gave such a name a second candidate would make the value cached
    // here wrong.

    QMap<LogbookColumn, QVariant> unavailable;
    for (int i = 0; i < m_columns.size(); ++i) {
        if (sr.cachedValues.contains(i) || sr.pendingColumns.contains(i) || !isExplicitBacked(i))
            continue;
        if (containsAnyOf(m_columnExplicitCalculations[i], known)) {
            sr.pendingColumns.insert(i);        // only a load may read it
        } else {
            sr.cachedValues[i] = QVariant();
            unavailable[m_columns[i]] = QVariant();
            m_columnWorkStats.valuesComputed++;
        }
    }
    logbook.updateCachedValues(sr.sessionId, unavailable);
}

void SessionModel::onCalculationRecordsChanged(const QString &sessionId, const QString &calculationId)
{
    // Inside a record method: no session is read, nothing is emitted, and the
    // row list is not touched.
    const int row = getSessionRow(sessionId);
    if (row < 0)
        return;
    SessionRow &sr = m_rows[row];

    bool removed = false;
    for (int i = 0; i < m_columnExplicitCalculations.size(); ++i) {
        if (!m_columnExplicitCalculations[i].contains(calculationId))
            continue;
        if (sr.cachedValues.remove(i) > 0)
            removed = true;
        if (sr.pendingColumns.remove(i))
            removed = true;
    }
    if (!removed)
        return;

    if (sr.isLoaded() && !sr.loadFailed)
        queueRecordColumnRefresh(sessionId);
    else
        startColumnWorker();
}

void SessionModel::queueRecordColumnRefresh(const QString &sessionId)
{
    m_recordColumnRefresh.insert(sessionId);

    if (!m_recordColumnRefreshQueued) {
        m_recordColumnRefreshQueued = true;
        QMetaObject::invokeMethod(this, &SessionModel::refreshRecordColumns, Qt::QueuedConnection);
    }
}

void SessionModel::refreshRecordColumns()
{
    m_recordColumnRefreshQueued = false;

    QSet<QString> pending;
    pending.swap(m_recordColumnRefresh);

    const int computedBefore = m_columnWorkStats.valuesComputed;
    for (const QString &sessionId : std::as_const(pending)) {
        // A row evicted meanwhile was completed by evictSession
        const int row = getSessionRow(sessionId);
        if (row < 0 || !m_rows[row].isLoaded() || m_rows[row].loadFailed)
            continue;
        fillMissingColumns(row, m_rows[row].session.value(), ColumnSource::LoadedRow);
    }

    // No dataChanged: a loaded row's cells are read live from the session, and
    // the publish or edit that caused the change has already told the views.
    if (m_columnWorkStats.valuesComputed != computedBefore)
        LogbookManager::instance().flushIndex();
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

void SessionModel::publishCalculationInvalidation(const QString &sessionId, const QSet<DependencyKey> &keys)
{
    assertRowsMutable("SessionModel::publishCalculationInvalidation");   // it emits

    if (keys.isEmpty())
        return;
    const int row = getSessionRow(sessionId);
    if (row < 0 || !m_rows[row].isLoaded())
        return;     // removed or evicted: nobody displays these values

    // The values are already invalid in the engine; this tells consumers to
    // read again. Publishing a calculation result is not a persistent change:
    // no column is invalidated, nothing is marked dirty, no save is scheduled.
    // A loaded row is displayed live, so the logbook repaints from dataChanged.
    publishInvalidation(row, keys);
    emit modelChanged();
}

// ---- Pinned sessions ----------------------------------------------------

void SessionModel::pinSession(const QString &sessionId)
{
    if (sessionId.isEmpty())
        return;
    ++m_pinnedSessions[sessionId];
}

void SessionModel::unpinSession(const QString &sessionId)
{
    const auto it = m_pinnedSessions.find(sessionId);
    if (it == m_pinnedSessions.end()) {
        qWarning() << "SessionModel::unpinSession: session is not pinned:" << sessionId;
        return;
    }
    if (--it.value() > 0)
        return;
    m_pinnedSessions.erase(it);

    // The cache may be over capacity by the rows that were pinned. Never evict
    // from here: the caller may be inside a signal emission or hold references.
    if (!m_evictionPassQueued) {
        m_evictionPassQueued = true;
        QMetaObject::invokeMethod(this, [this] {
            m_evictionPassQueued = false;
            evictIfNeeded();
        }, Qt::QueuedConnection);
    }
}

bool SessionModel::isSessionPinned(const QString &sessionId) const
{
    return m_pinnedSessions.value(sessionId) > 0;
}

// ---- Deferred logbook persistence ------------------------------------

void SessionModel::scheduleSave(const QString &sessionId)
{
    int row = getSessionRow(sessionId);
    if (row < 0) return;

    SessionRow &sr = m_rows[row];

    // A failed-load placeholder is never saved: it must not reach the file of
    // the session it stands in for.
    if (sr.loadFailed)
        return;

    if (!sr.dirty || sr.saveFailed) {
        // A row whose last save failed is dirty but no longer queued (nor
        // counted): a new edit queues it again.
        sr.dirty = true;
        sr.saveFailed = false;
        m_saveHighWater++;
        m_saveRemaining++;
    }
    // else: row is already dirty, data is updated in memory,
    // saver will save the current (modified) version when it reaches this row

    m_scheduler.wake();
}

// Saves a loaded row. On success the row is clean. On failure the warning is
// logged (once per failed attempt), the previous session file is intact, and
// the row stays DIRTY with saveFailed set: its in-memory state is the only
// copy of the unsaved change, so it is neither forgotten nor evicted. The
// logbook keeps the affected columns marked unsaved (LogbookManager::
// saveSession clears the marks only on success), so whatever is cached for
// them in memory stays out of index.json. Callers own the progress counters.
bool SessionModel::saveLoadedRow(SessionRow &sr)
{
    Q_ASSERT(sr.isLoaded() && !sr.loadFailed);
    LogbookManager &logbook = LogbookManager::instance();
    if (logbook.saveSession(sr.session.value())) {
        sr.dirty = false;
        sr.saveFailed = false;
        return true;
    }
    qWarning("SessionModel: session %s was not saved: %s",
             qPrintable(sr.sessionId), qPrintable(logbook.lastSaveError()));
    sr.dirty = true;
    sr.saveFailed = true;
    return false;
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
    m_bulkEditSkipped = 0;

    LogbookManager &logbook = LogbookManager::instance();
    bool anySaved = false;
    for (int i = 0; i < m_rows.size(); ++i) {
        SessionRow &sr = m_rows[i];
        if (!sr.dirty)
            continue;
        const SessionData &session = sessionRef(i);
        if (sr.loadFailed) {
            sr.dirty = false;       // a placeholder is never saved
            continue;
        }
        // Rows whose earlier save failed are retried here (shutdown or an
        // explicit flush). One that fails again stays dirty and marked.
        if (!saveLoadedRow(sr))
            continue;
        fillMissingColumns(i, session, ColumnSource::LoadedRow);
        anySaved = true;
    }
    // Also when nothing was dirty: a cache discarded at startup is rewritten
    // with the current marker.
    if (anySaved || logbook.indexNeedsFlush()) {
        logbook.flushIndex();
    }

    // Reset high-water-mark counters (UI is about to be destroyed during shutdown)
    m_saveHighWater = 0;
    m_saveRemaining = 0;
}

void SessionModel::saveNextSession()
{
    // Find the first dirty row that is still queued (see SessionRow::saveFailed)
    int dirtyIdx = -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].dirty && !m_rows[i].saveFailed) {
            dirtyIdx = i;
            break;
        }
    }

    if (dirtyIdx < 0)
        return;

    SessionRow &sr = m_rows[dirtyIdx];

    // Not retried by this task on failure (a full disk would otherwise spin the
    // idle scheduler): the row stays dirty with saveFailed set, which takes it
    // out of this task's work until it is edited again or flushed. The unsaved
    // marks keep index.json consistent with the file that is still on disk.
    const SessionData &session = sessionRef(dirtyIdx);
    if (sr.loadFailed) {
        // A placeholder is never saved
        sr.dirty = false;
        m_saveRemaining--;
        return;
    }
    if (saveLoadedRow(sr))
        fillMissingColumns(dirtyIdx, session, ColumnSource::LoadedRow);

    m_saveRemaining--;      // saved, or no longer queued
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
    assertRowsMutable("SessionModel::loadNextVisibleSession");
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

void SessionModel::evictIfNeeded(const QString &keep)
{
    // Least recently used first. A row that cannot be evicted (its save
    // failed, or it is pinned: see PINNED SESSIONS) keeps its place in the list
    // and is passed over, so the cache may exceed its capacity by the number of
    // such rows.
    //
    // `keep` is the session sessionRef() has just loaded and is about to return
    // a reference to. It is the most recently used entry, so it is reached only
    // when every other entry had to stay loaded; evicting it then would
    // invalidate that reference.
    for (qsizetype i = m_lruList.size() - 1; i >= 0 && m_lruList.size() > m_cacheCapacity; --i) {
        const QString sessionId = m_lruList.at(i);
        if (!keep.isEmpty() && sessionId == keep)
            continue;
        evictSession(sessionId);    // on success removes entry i; i - 1 is next either way
    }
}

bool SessionModel::evictSession(const QString &sessionId)
{
    assertRowsMutable("SessionModel::evictSession");

    // Find the row
    int row = getSessionRow(sessionId);
    if (row < 0) {
        lruRemove(sessionId);
        return true;
    }

    SessionRow &sr = m_rows[row];

    // Already a stub -- nothing to do
    if (!sr.isLoaded()) {
        lruRemove(sessionId);
        return true;
    }

    // Pinned: a job is queued or running for it; it stays loaded and in the
    // LRU list. Nothing is saved or filled for it in this pass; unpinSession()
    // schedules the pass that evicts it.
    if (m_pinnedSessions.value(sessionId) > 0)
        return false;

    // The in-memory session is the only copy of a change that could not be
    // saved: it stays loaded (and in the LRU list). No retry here, so a
    // persistent failure is not reported again on every eviction pass.
    if (sr.saveFailed)
        return false;

    // A failed-load placeholder holds nothing worth keeping: it is never saved
    // and caches no column values. Back to a stub, so that a later access
    // retries the load.
    if (sr.loadFailed) {
        if (sr.dirty) {
            sr.dirty = false;
            if (m_saveRemaining > 0)
                m_saveRemaining--;
        }
        sr.session = std::nullopt;
        sr.loadFailed = false;
        lruRemove(sessionId);
        return true;
    }

    // Save if dirty
    if (sr.dirty) {
        const bool saved = saveLoadedRow(sr);

        // Update saver progress if saves are in flight: saved, or no longer queued
        if (m_saveRemaining > 0) {
            m_saveRemaining--;
        }
        if (!saved)
            return false;       // not evicted: see saveFailed above
    }

    // Complete the cached column values before eviction: the stub displays them
    fillMissingColumns(row, sr.session.value(), ColumnSource::LoadedRow);

    // Values over records the engine could not vouch for (a failed write or
    // removal, a record skipped at the load) go with the engine; the worker
    // settles them against the records on disk.
    const QStringList unconfirmedIds =
        LogbookManager::instance().discardUnconfirmedCalculationRecords(sessionId);
    const QSet<QString> unconfirmed(unconfirmedIds.cbegin(), unconfirmedIds.cend());
    bool dropped = false;
    if (!unconfirmed.isEmpty()) {
        for (int i = 0; i < m_columnExplicitCalculations.size(); ++i) {
            if (containsAnyOf(m_columnExplicitCalculations[i], unconfirmed) && sr.cachedValues.remove(i) > 0)
                dropped = true;
        }
    }

    // Reset to stub
    sr.session = std::nullopt;
    lruRemove(sessionId);
    if (dropped)
        startColumnWorker();
    return true;
}

// ---- Dirty column worker ----------------------------------------------

void SessionModel::startColumnWorker()
{
    // Count sessions with missing column values
    int dirtyCount = 0;
    for (const SessionRow &row : std::as_const(m_rows)) {
        if (needsColumnWork(row)) {
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
        if (needsColumnWork(row)) {
            dirtyIdx = i;
            break;
        }
    }

    if (dirtyIdx < 0)
        return;

    SessionRow &row = m_rows[dirtyIdx];
    LogbookManager &logbook = LogbookManager::instance();

    if (row.isLoaded()) {
        // Session is already loaded; compute from in-memory data
        fillMissingColumns(dirtyIdx, row.session.value(), ColumnSource::LoadedRow);
    } else {
        // Stub session. Explicit-backed columns are settled from the record
        // set first: a stub whose only missing values are those is settled
        // without any load.
        settleExplicitColumns(dirtyIdx);
        if (!needsColumnWork(row)) {
            m_columnWorkerRemaining--;
            emit dataChanged(index(dirtyIdx, 0), index(dirtyIdx, columnCount() - 1), {Qt::DisplayRole});
            return;
        }

        // Load temporarily via LogbookManager::loadSession(). A temporary
        // load: stored results are never read here (see STORED RESULTS);
        // explicit-backed columns are settled from the record set instead.
        auto loaded = logbook.loadSession(row.sessionId);
        if (loaded.has_value()) {
            // Remap UUID-based session ID to real SESSION_ID if needed
            const QString realId = loaded->getAttribute(SessionKeys::SessionId).toString();
            if (!realId.isEmpty() && realId != row.sessionId)
                setRowSessionId(row, realId);
            m_columnWorkStats.sessionsLoaded++;
            fillMissingColumns(dirtyIdx, loaded.value(), ColumnSource::TemporaryLoad);
        } else {
            // Load failed; skip this session
            m_columnWorkerRemaining--;
            return;
        }
        // Temporarily loaded session is discarded here
    }

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

    // The indices are current here and only here: the queue keeps the session
    // id and the attribute key (see BulkEditItem). Appending supports multiple
    // successive edits.
    int queued = 0;
    for (int row : rows) {
        if (row < 0 || row >= m_rows.size())
            continue;
        m_bulkEditQueue.append({m_rows[row].sessionId, col.attributeKey, value});
        ++queued;
    }
    if (queued == 0)
        return;

    // Mark the column unsaved for the whole batch up front, so that the first
    // save performs one pre-save index flush instead of one per row.
    for (int row : rows) {
        if (row >= 0 && row < m_rows.size())
            invalidateColumns(row, {DependencyKey::attribute(col.attributeKey)});
    }

    m_bulkEditHighWater += queued;
    m_bulkEditRemaining += queued;

    // Wake the scheduler so the bulk edit worker picks up new work
    m_scheduler.wake();
}

void SessionModel::cancelBulkEdit()
{
    m_scheduler.cancel(BulkEditTask);
}

void SessionModel::processNextBulkEdit()
{
    // Skip entries that cannot be applied; each one counts as done
    int row = -1;
    const AttributeDefinition *def = nullptr;
    while (!m_bulkEditQueue.isEmpty()) {
        const BulkEditItem &front = m_bulkEditQueue.first();
        def = AttributeRegistry::instance().findByKey(front.attributeKey);
        // Unknown attribute, or a format type that is not edited in bulk
        const bool editableType = def &&
            (def->formatType == AttributeFormatType::Text ||
             def->formatType == AttributeFormatType::Double);
        // The row is wherever the session is now. A session that was removed
        // since the item was queued has none.
        row = editableType ? getSessionRow(front.sessionId) : -1;
        if (row >= 0)
            break;
        if (editableType)
            m_bulkEditSkipped++;
        m_bulkEditQueue.removeFirst();
        m_bulkEditRemaining--;
    }

    if (m_bulkEditQueue.isEmpty())
        return;

    const BulkEditItem item = m_bulkEditQueue.takeFirst();

    // The edit is made by attribute key. invalidateColumns() finds the columns
    // the key affects among the columns enabled now, so an edit whose column
    // was disabled since it was queued still reaches the session; there is
    // just no cached column value to refresh.
    SessionRow &sr = m_rows[row];
    const QString &attributeKey = item.attributeKey;
    LogbookManager &logbook = LogbookManager::instance();

    // Compute the final value (with unit reverse-conversion for Double)
    QVariant newVal;
    switch (def->formatType) {
    case AttributeFormatType::Text:
        newVal = CsvFormat::singleLine(item.value.toString());
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

    if (sr.loadFailed) {
        // A failed-load placeholder is never edited or saved: skipped, like a
        // stub whose load fails below
    } else if (sr.isLoaded()) {
        // --- LOADED PATH ---
        SessionData &session = sr.session.value();
        session.setAttribute(attributeKey, newVal);
        invalidateColumns(row, {DependencyKey::attribute(attributeKey)});

        // Save inline. A row that was already queued for the idle saver
        // (dirty, not failed) leaves that queue either way: saved, or failed.
        const bool wasQueued = sr.dirty && !sr.saveFailed;
        if (saveLoadedRow(sr)) {
            // Recompute only what the edit removed
            fillMissingColumns(row, session, ColumnSource::LoadedRow);
        }
        if (wasQueued)
            m_saveRemaining--;
    } else {
        // --- STUB PATH (avoids LRU/eviction) ---
        // A temporary load: stored results are never read here (see STORED RESULTS)
        auto loaded = logbook.loadSession(sr.sessionId);
        if (loaded.has_value()) {
            // UUID remap (same pattern as column worker)
            const QString realId = loaded->getAttribute(SessionKeys::SessionId).toString();
            if (!realId.isEmpty() && realId != sr.sessionId)
                setRowSessionId(sr, realId);   // later items queued for this session follow

            m_columnWorkStats.sessionsLoaded++;

            // Apply the edit
            loaded->setAttribute(attributeKey, newVal);
            invalidateColumns(row, {DependencyKey::attribute(attributeKey)});

            // Save
            if (logbook.saveSession(loaded.value())) {
                // Recompute only what the edit removed
                fillMissingColumns(row, loaded.value(), ColumnSource::TemporaryLoad);
                // loaded goes out of scope: the session stays a stub
            } else {
                qWarning("SessionModel: session %s was not saved: %s",
                         qPrintable(sr.sessionId), qPrintable(logbook.lastSaveError()));
                // The temporary session holds the only copy of the edit: the
                // row becomes loaded, dirty and saveFailed instead of losing it.
                // Known corner: a VISIBLE stub that is still in m_loadQueue when
                // it is promoted here is later dropped from the queue by
                // loadNextVisibleSession (it is loaded by then) without being
                // added to m_loadedDuringBatch, so no visibilityChanged is
                // emitted for it when the load batch completes.
                assertRowsMutable("SessionModel::processNextBulkEdit (row becomes loaded)");
                sr.session = std::move(loaded.value());
                sr.session->setVisible(sr.visible);
                sr.dirty = true;
                sr.saveFailed = true;
                attachSession(sr);
                // The row is installed now: its stored results go in before
                // the dataChanged below. Nobody has read this engine yet, so
                // the names the restore invalidated are not published.
                restoreStoredResults(sr);
                if (!sr.visible && sr.sessionId != m_focusedSessionId)
                    lruInsert(sr.sessionId);
            }
        }
        // If load failed, silently skip
    }

    // Notify the view that this row has been updated
    if (columnCount() > 0)
        emit dataChanged(index(row, 0), index(row, columnCount() - 1), {Qt::DisplayRole});

    // Update progress
    m_bulkEditRemaining--;
}

void SessionModel::finishBulkEdit()
{
    LogbookManager::instance().flushIndex();

    if (m_bulkEditSkipped > 0)
        qDebug("SessionModel: bulk edit skipped %d session(s) that no longer exist", m_bulkEditSkipped);

    // Reset state
    m_bulkEditHighWater = 0;
    m_bulkEditRemaining = 0;
    m_bulkEditSkipped = 0;
    m_bulkEditQueue.clear();

    emit modelChanged();
}

// ---- Column value extraction ------------------------------------------

QMap<LogbookColumn, QVariant> SessionModel::computeColumnValues(const SessionData &session,
                                                                const QVector<int> &columnIndices) const
{
    QMap<LogbookColumn, QVariant> result;
    for (int columnIndex : columnIndices) {
        if (columnIndex < 0 || columnIndex >= m_columns.size())
            continue;
        const LogbookColumn &col = m_columns[columnIndex];
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

    assertRowsMutable("SessionModel::sort");
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
