#include "logbookcolumn.h"

#include <algorithm>

#include <QHash>
#include <QSettings>

#include "attributeregistry.h"
#include "calculations/builtincalculations.h"
#include "engine/calculationregistry.h"
#include "markerregistry.h"
#include "plotregistry.h"
#include "sessiondata.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"

using namespace FlySight;

// ============================================================================
// Display name helpers
// ============================================================================

QString FlySight::logbookColumnDisplayName(const LogbookColumn &col)
{
    switch (col.type) {
    case ColumnType::SessionAttribute: {
        // Look up the display name from AttributeRegistry
        const auto attrs = AttributeRegistry::instance().allAttributes();
        for (const auto &def : attrs) {
            if (def.attributeKey == col.attributeKey)
                return def.displayName;
        }
        // Fallback: return the raw key
        return col.attributeKey;
    }
    case ColumnType::MeasurementAtMarker: {
        // Build "{plotName} @ {markerDisplayName}"
        QString plotName;
        const auto plots = PlotRegistry::instance().allPlots();
        for (const auto &pv : plots) {
            if (pv.sensorID == col.sensorID && pv.measurementID == col.measurementID) {
                plotName = pv.plotName;
                break;
            }
        }
        if (plotName.isEmpty())
            plotName = col.sensorID + QStringLiteral("/") + col.measurementID;

        QString markerName;
        const auto markers = MarkerRegistry::instance()->allMarkers();
        for (const auto &md : markers) {
            if (md.attributeKey == col.markerAttributeKey) {
                markerName = md.displayName;
                break;
            }
        }
        if (markerName.isEmpty())
            markerName = col.markerAttributeKey;

        return plotName + QStringLiteral(" @ ") + markerName;
    }
    case ColumnType::Delta: {
        // Build "{plotName} ({marker1DisplayName} -> {marker2DisplayName})"
        QString plotName;
        const auto plots = PlotRegistry::instance().allPlots();
        for (const auto &pv : plots) {
            if (pv.sensorID == col.sensorID && pv.measurementID == col.measurementID) {
                plotName = pv.plotName;
                break;
            }
        }
        if (plotName.isEmpty())
            plotName = col.sensorID + QStringLiteral("/") + col.measurementID;

        QString marker1Name, marker2Name;
        const auto markers = MarkerRegistry::instance()->allMarkers();
        for (const auto &md : markers) {
            if (md.attributeKey == col.markerAttributeKey)
                marker1Name = md.displayName;
            if (md.attributeKey == col.marker2AttributeKey)
                marker2Name = md.displayName;
        }
        if (marker1Name.isEmpty())
            marker1Name = col.markerAttributeKey;
        if (marker2Name.isEmpty())
            marker2Name = col.marker2AttributeKey;

        return plotName + QStringLiteral(" (") + marker1Name
               + QStringLiteral(" -> ") + marker2Name + QStringLiteral(")");
    }
    }

    return QString();
}

QString FlySight::logbookColumnLabel(const LogbookColumn &col)
{
    if (!col.customLabel.isEmpty())
        return col.customLabel;
    return logbookColumnDisplayName(col);
}

// ============================================================================
// Column identity
// ============================================================================

QString FlySight::logbookColumnDefinitionKey(const LogbookColumn &col)
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

QVector<LogbookColumn> FlySight::uniqueLogbookColumns(const QVector<LogbookColumn> &columns)
{
    QVector<LogbookColumn> result;
    result.reserve(columns.size());

    QHash<QString, int> keptIndex;      // definition key -> index in result
    for (const LogbookColumn &col : columns) {
        const QString key = logbookColumnDefinitionKey(col);
        const auto it = keptIndex.constFind(key);
        if (it == keptIndex.constEnd()) {
            keptIndex.insert(key, result.size());
            result.append(col);
        } else if (col.enabled) {
            result[it.value()].enabled = true;
        }
    }
    return result;
}

QList<DependencyKey> FlySight::logbookColumnNames(const LogbookColumn &col)
{
    // The same keys SessionModel::computeColumnValues reads
    switch (col.type) {
    case ColumnType::SessionAttribute:
        return {DependencyKey::attribute(col.attributeKey)};
    case ColumnType::MeasurementAtMarker:
        return {DependencyKey::attribute(SessionData::interpolationKey(
            col.markerAttributeKey, col.sensorID, SessionKeys::Time, col.measurementID))};
    case ColumnType::Delta:
        return {DependencyKey::attribute(SessionData::interpolationKey(
                    col.markerAttributeKey, col.sensorID, SessionKeys::Time, col.measurementID)),
                DependencyKey::attribute(SessionData::interpolationKey(
                    col.marker2AttributeKey, col.sensorID, SessionKeys::Time, col.measurementID))};
    }
    return {};
}

QStringList FlySight::logbookColumnExplicitCalculations(const LogbookColumn &col,
                                                        const CalculationRegistry &registry)
{
    QStringList ids;
    const QList<DependencyKey> names = logbookColumnNames(col);
    for (const DependencyKey &name : names)
        ids += registry.explicitDependencies(name);
    ids.sort();
    ids.removeDuplicates();
    return ids;
}

QString FlySight::logbookColumnEnvironment(const LogbookColumn &col, const CalculationRegistry &registry)
{
    return calculationEnvironmentDigest(logbookColumnNames(col), registry);
}

bool FlySight::containsAnyOf(const QStringList &ids, const QSet<QString> &set)
{
    return std::any_of(ids.cbegin(), ids.cend(), [&set](const QString &id) { return set.contains(id); });
}

// ============================================================================
// LogbookColumnStore
// ============================================================================

static const QString kSettingsArrayKey = QStringLiteral("logbook/columns");

LogbookColumnStore& LogbookColumnStore::instance()
{
    static LogbookColumnStore store;
    return store;
}

LogbookColumnStore::LogbookColumnStore()
{
    // Register the version preference so PreferencesManager can track it
    PreferencesManager::instance().registerPreference(
        PreferenceKeys::LogbookColumnsVersion, 0);
}

QVector<LogbookColumn> LogbookColumnStore::columns() const
{
    return m_columns;
}

QVector<LogbookColumn> LogbookColumnStore::enabledColumns() const
{
    QVector<LogbookColumn> result;
    for (const auto &col : m_columns) {
        if (col.enabled)
            result.append(col);
    }
    return result;
}

void LogbookColumnStore::setColumns(const QVector<LogbookColumn> &columns)
{
    // Two columns with one definition share a single cached value in the
    // logbook index, so the list never holds more than one of them.
    const QVector<LogbookColumn> unique = uniqueLogbookColumns(columns);
    if (unique == m_columns)
        return;

    m_columns = unique;
    save();
    emit columnsChanged();
}

void LogbookColumnStore::load()
{
    QSettings settings;
    int count = settings.beginReadArray(kSettingsArrayKey);

    m_columns.clear();
    m_columns.reserve(count);

    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);

        LogbookColumn col;
        int typeInt = settings.value(QStringLiteral("type"), 0).toInt();
        if (typeInt < 0 || typeInt > static_cast<int>(ColumnType::Delta))
            typeInt = 0;
        col.type = static_cast<ColumnType>(typeInt);
        col.attributeKey = settings.value(QStringLiteral("attributeKey")).toString();
        col.sensorID = settings.value(QStringLiteral("sensorID")).toString();
        col.measurementID = settings.value(QStringLiteral("measurementID")).toString();
        col.measurementType = settings.value(QStringLiteral("measurementType")).toString();
        col.markerAttributeKey = settings.value(QStringLiteral("markerAttributeKey")).toString();
        col.marker2AttributeKey = settings.value(QStringLiteral("marker2AttributeKey")).toString();
        col.enabled = settings.value(QStringLiteral("enabled"), true).toBool();
        col.customLabel = settings.value(QStringLiteral("customLabel")).toString();

        m_columns.append(col);
    }
    settings.endArray();

    // Settings written before duplicates were refused may hold some: collapse
    // them and write the cleaned list back so this happens once.
    const QVector<LogbookColumn> unique = uniqueLogbookColumns(m_columns);
    if (unique.size() != m_columns.size()) {
        m_columns = unique;
        save();
    }

    if (m_columns.isEmpty())
        loadDefaults();

    emit columnsChanged();
}

void LogbookColumnStore::save()
{
    // Write the QSettings array FIRST (before any scalar preference updates)
    QSettings settings;
    settings.beginWriteArray(kSettingsArrayKey, m_columns.size());
    for (int i = 0; i < m_columns.size(); ++i) {
        settings.setArrayIndex(i);
        const auto &col = m_columns[i];

        settings.setValue(QStringLiteral("type"), static_cast<int>(col.type));
        settings.setValue(QStringLiteral("attributeKey"), col.attributeKey);
        settings.setValue(QStringLiteral("sensorID"), col.sensorID);
        settings.setValue(QStringLiteral("measurementID"), col.measurementID);
        settings.setValue(QStringLiteral("measurementType"), col.measurementType);
        settings.setValue(QStringLiteral("markerAttributeKey"), col.markerAttributeKey);
        settings.setValue(QStringLiteral("marker2AttributeKey"), col.marker2AttributeKey);
        settings.setValue(QStringLiteral("enabled"), col.enabled);
        settings.setValue(QStringLiteral("customLabel"), col.customLabel);
    }
    settings.endArray();

    // Increment version to notify listeners
    auto &prefs = PreferencesManager::instance();
    int version = prefs.getValue(PreferenceKeys::LogbookColumnsVersion).toInt();
    prefs.setValue(PreferenceKeys::LogbookColumnsVersion, version + 1);
}

void LogbookColumnStore::loadDefaults()
{
    m_columns.clear();

    // Six SessionAttribute columns matching the current hard-coded logbook layout
    auto makeCol = [](const char *key) {
        LogbookColumn col;
        col.type = ColumnType::SessionAttribute;
        col.attributeKey = QString::fromLatin1(key);
        col.enabled = true;
        return col;
    };

    m_columns.append(makeCol(SessionKeys::Description));
    m_columns.append(makeCol(SessionKeys::DeviceId));
    m_columns.append(makeCol(SessionKeys::StartTime));
    m_columns.append(makeCol(SessionKeys::Duration));
    m_columns.append(makeCol(SessionKeys::ExitTime));
    m_columns.append(makeCol(SessionKeys::GroundElev));

    save();
}
