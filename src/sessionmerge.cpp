#include "sessionmerge.h"

#include <algorithm>
#include <cstring>

#include "conversion/schematable.h"
#include "csvformat.h"

namespace FlySight {

QSet<DependencyKey> MergePlan::changedKeys() const
{
    QSet<DependencyKey> keys;
    for (const auto &attribute : attributesToSet)
        keys.insert(DependencyKey::attribute(attribute.first));
    for (auto sensorIt = columnsToSet.constBegin(); sensorIt != columnsToSet.constEnd(); ++sensorIt) {
        for (auto colIt = sensorIt.value().constBegin(); colIt != sensorIt.value().constEnd(); ++colIt)
            keys.insert(DependencyKey::measurement(sensorIt.key(), colIt.key()));
    }
    return keys;
}

namespace SessionMerge {

namespace {

struct Conflict {
    QString key;
    QString sessionText;
    QString fileText;
};

QString attributeText(const QVariant &value)
{
    return CsvFormat::formatAttributeValue(value).value_or(QString());
}

bool isDeviceIdPlaceholder(const QString &key, const QVariant &value)
{
    return key == QLatin1String(SessionKeys::DeviceId)
        && value.toString() == QLatin1String(SessionKeys::DeviceIdUnknown);
}

QString conflictMessage(QVector<Conflict> conflicts)
{
    std::sort(conflicts.begin(), conflicts.end(),
              [](const Conflict &a, const Conflict &b) { return a.key < b.key; });

    QString message;
    bool schemaInvolved = false;
    for (const Conflict &conflict : std::as_const(conflicts)) {
        message += QStringLiteral("Attribute '%1' conflicts with the existing session (session: '%2', file: '%3'). ")
                       .arg(conflict.key, conflict.sessionText, conflict.fileText);
        if (conflict.key == QLatin1String(Schema::AttributeKey))
            schemaInvolved = true;
    }

    message += schemaInvolved
        ? QStringLiteral("To change a session's schema version, delete the session and re-import its files.")
        : QStringLiteral("To replace the session, delete it and re-import its files.");
    return message;
}

// The ragged rule for one sensor the plan touches. Returns the error, or "".
QString raggedError(const QString &sensorName, const SourceSensor &planned, const SourceSensor &existing)
{
    // What the file brings for this sensor. A parsed file has columns of one
    // length; a session built in memory might not.
    const qsizetype fileRows = planned.constBegin().value().samples.size();
    for (auto it = planned.constBegin(); it != planned.constEnd(); ++it) {
        if (it.value().samples.size() != fileRows) {
            return QStringLiteral("Sensor '%1': the file's columns have unequal lengths (column '%2' has %3 rows, "
                                  "column '%4' has %5).")
                .arg(sensorName, planned.constBegin().key()).arg(fileRows)
                .arg(it.key()).arg(it.value().samples.size());
        }
    }

    // The columns the merge keeps must end up as long as the ones it writes.
    for (auto it = existing.constBegin(); it != existing.constEnd(); ++it) {
        if (planned.contains(it.key()))
            continue;
        if (it.value().samples.size() != fileRows) {
            return QStringLiteral("Sensor '%1': the file has %2 rows but the session's column '%3' has %4. "
                                  "Delete the session and re-import its files.")
                .arg(sensorName).arg(fileRows).arg(it.key()).arg(it.value().samples.size());
        }
    }
    return QString();
}

} // namespace

bool sameAttributeValue(const QVariant &a, const QVariant &b)
{
    return CsvFormat::formatAttributeValue(a) == CsvFormat::formatAttributeValue(b);
}

bool sameColumn(const SourceColumn &a, const SourceColumn &b)
{
    if (a.unit != b.unit || a.samples.size() != b.samples.size())
        return false;
    if (a.samples.isEmpty() || a.samples.constData() == b.samples.constData())
        return true;    // the same (implicitly shared) buffer

    // Bitwise: NaN equals NaN, so an identical re-import is recognised.
    return std::memcmp(a.samples.constData(), b.samples.constData(),
                       size_t(a.samples.size()) * sizeof(double)) == 0;
}

MergePlan plan(const SessionData &existing, const SessionData &incoming)
{
    MergePlan result;

    // ---- attributes (stored values only) ----
    QVector<Conflict> conflicts;
    const QStringList incomingKeys = incoming.attributeKeys();
    for (const QString &key : incomingKeys) {
        const QVariant incomingValue = incoming.storedAttribute(key);

        // A Viewer placeholder is not a recorded fact
        if (isDeviceIdPlaceholder(key, incomingValue))
            continue;

        if (!existing.hasStoredAttribute(key)) {
            result.attributesToSet.append(qMakePair(key, incomingValue));
            continue;
        }

        // Viewer attribute: the session's value wins, never a conflict
        if (key.startsWith(QLatin1Char('_')))
            continue;

        const QVariant existingValue = existing.storedAttribute(key);
        if (sameAttributeValue(existingValue, incomingValue))
            continue;

        // The session's placeholder counts as absent
        if (isDeviceIdPlaceholder(key, existingValue)) {
            result.attributesToSet.append(qMakePair(key, incomingValue));
            continue;
        }

        conflicts.append({ key, attributeText(existingValue), attributeText(incomingValue) });
    }

    if (!conflicts.isEmpty()) {
        result.attributesToSet.clear();
        result.error = conflictMessage(conflicts);
        return result;
    }

    // ---- measurements (source layer only; buffers are shared, not copied) ----
    const SourceData existingSource = existing.sourceData();
    const SourceData incomingSource = incoming.sourceData();
    for (auto sensorIt = incomingSource.constBegin(); sensorIt != incomingSource.constEnd(); ++sensorIt) {
        const SourceSensor existingSensor = existingSource.value(sensorIt.key());
        for (auto colIt = sensorIt.value().constBegin(); colIt != sensorIt.value().constEnd(); ++colIt) {
            auto existingIt = existingSensor.constFind(colIt.key());
            if (existingIt != existingSensor.constEnd() && sameColumn(existingIt.value(), colIt.value()))
                continue;
            result.columnsToSet[sensorIt.key()].insert(colIt.key(), colIt.value());
        }
    }

    // ---- ragged rule: only sensors the plan touches ----
    for (auto sensorIt = result.columnsToSet.constBegin(); sensorIt != result.columnsToSet.constEnd(); ++sensorIt) {
        const QString error = raggedError(sensorIt.key(), sensorIt.value(), existingSource.value(sensorIt.key()));
        if (!error.isEmpty()) {
            result.attributesToSet.clear();
            result.columnsToSet.clear();
            result.error = error;
            return result;
        }
    }

    return result;
}

QSet<DependencyKey> apply(SessionData &existing, const MergePlan &plan)
{
    QSet<DependencyKey> invalidated;
    if (!plan.ok())
        return invalidated;     // a rejected plan is never applied

    for (const auto &attribute : plan.attributesToSet)
        invalidated.unite(existing.setAttribute(attribute.first, attribute.second));
    invalidated.unite(existing.mergeSourceData(plan.columnsToSet));
    return invalidated;
}

} // namespace SessionMerge
} // namespace FlySight
