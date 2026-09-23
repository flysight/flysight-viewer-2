#ifndef LOGBOOKCOLUMN_H
#define LOGBOOKCOLUMN_H

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include "dependencykey.h"

namespace FlySight {

class CalculationRegistry;

// ============================================================================
// Column Type Enum
// ============================================================================

enum class ColumnType {
    SessionAttribute,      // References one AttributeRegistry entry
    MeasurementAtMarker,   // References one PlotRegistry entry + one MarkerRegistry entry
    Delta                  // References one PlotRegistry entry + two MarkerRegistry entries
};

// ============================================================================
// LogbookColumn Struct
// ============================================================================

struct LogbookColumn {
    ColumnType type;

    // --- Session Attribute fields ---
    QString attributeKey;

    // --- Measurement fields ---
    QString sensorID;
    QString measurementID;
    QString measurementType;    // For UnitConverter

    // --- Marker fields ---
    QString markerAttributeKey;

    // --- Second marker (Delta only) ---
    QString marker2AttributeKey;

    // --- Common fields ---
    bool enabled = true;
    QString customLabel;

    bool operator==(const LogbookColumn &other) const {
        return type == other.type
            && attributeKey == other.attributeKey
            && sensorID == other.sensorID
            && measurementID == other.measurementID
            && measurementType == other.measurementType
            && markerAttributeKey == other.markerAttributeKey
            && marker2AttributeKey == other.marker2AttributeKey
            && enabled == other.enabled
            && customLabel == other.customLabel;
    }

    bool operator<(const LogbookColumn &other) const {
        if (type != other.type) return type < other.type;
        if (attributeKey != other.attributeKey) return attributeKey < other.attributeKey;
        if (sensorID != other.sensorID) return sensorID < other.sensorID;
        if (measurementID != other.measurementID) return measurementID < other.measurementID;
        if (measurementType != other.measurementType) return measurementType < other.measurementType;
        if (markerAttributeKey != other.markerAttributeKey) return markerAttributeKey < other.markerAttributeKey;
        return marker2AttributeKey < other.marker2AttributeKey;
    }
};

/// Auto-generated display name based on column type and registry lookups
QString logbookColumnDisplayName(const LogbookColumn &col);

/// Returns customLabel if non-empty, otherwise logbookColumnDisplayName(col)
QString logbookColumnLabel(const LogbookColumn &col);

/// Identity of a column's DEFINITION: the type and the fields that type reads.
/// Display-only fields (enabled, customLabel) and fields the type ignores are
/// not part of it. Cached column values in the logbook index are keyed by this
/// string, so two columns with the same key share one cached value.
QString logbookColumnDefinitionKey(const LogbookColumn &col);

/// Collapses columns with the same definition key into one, keeping the first
/// occurrence (its position and label). The kept column is enabled if any of
/// the collapsed ones was, so a visible column never disappears. Columns with
/// distinct definitions are returned untouched and in order.
QVector<LogbookColumn> uniqueLogbookColumns(const QVector<LogbookColumn> &columns);

/// The names a column's value is read from (SessionModel::computeColumnValues
/// reads exactly these): the attribute, or the interpolation key(s) of the
/// measurement at the marker(s).
QList<DependencyKey> logbookColumnNames(const LogbookColumn &col);

/// The explicit calculations the column's value depends on: the union of
/// CalculationRegistry::explicitDependencies() over logbookColumnNames(col),
/// sorted and unique. Empty for a column that is not explicit-backed.
QStringList logbookColumnExplicitCalculations(const LogbookColumn &col,
                                              const CalculationRegistry &registry);

/// True when some id of `ids` (a column's explicit calculations, as above) is
/// in `set`: "does this column depend on one of these calculations".
bool containsAnyOf(const QStringList &ids, const QSet<QString> &set);

// ============================================================================
// LogbookColumnStore — QObject singleton for persistence
// ============================================================================

class LogbookColumnStore : public QObject {
    Q_OBJECT
public:
    static LogbookColumnStore& instance();

    QVector<LogbookColumn> columns() const;
    QVector<LogbookColumn> enabledColumns() const;
    void setColumns(const QVector<LogbookColumn> &columns);
    void load();

signals:
    void columnsChanged();

private:
    LogbookColumnStore();
    void save();
    void loadDefaults();

    QVector<LogbookColumn> m_columns;

    Q_DISABLE_COPY(LogbookColumnStore)
};

} // namespace FlySight

#endif // LOGBOOKCOLUMN_H
