// dataexporter.cpp

#include "dataexporter.h"

#include <functional>

#include <QDebug>
#include <QSaveFile>

#include "csvformat.h"

namespace FlySight {

namespace {

// Standard attribute order (viewer-known keys first, then any others)
const QStringList kAttributeOrder = {
    "FIRMWARE_VER", "DEVICE_ID", "SESSION_ID"
};

// Standard sensor order
const QStringList kSensorOrder = {
    "GNSS", "BARO", "HUM", "MAG", "IMU", "TIME", "VBAT"
};

// Standard measurement column order per sensor
const QMap<QString, QStringList> kMeasurementOrder = {
    {"GNSS", {"time", "lat", "lon", "hMSL", "velN", "velE", "velD",
              "hAcc", "vAcc", "sAcc", "heading", "cAcc", "gpsFix", "numSV"}},
    {"BARO", {"time", "pressure", "temperature"}},
    {"HUM",  {"time", "humidity", "temperature"}},
    {"MAG",  {"time", "x", "y", "z", "temperature"}},
    {"IMU",  {"time", "wx", "wy", "wz", "ax", "ay", "az", "temperature"}},
    {"TIME", {"time", "tow", "week"}},
    {"VBAT", {"time", "voltage"}},
};

// Returns keys reordered: preferred keys first (in preferred order),
// then remaining keys in their original order.
QStringList reorder(const QStringList &keys, const QStringList &preferredOrder)
{
    QStringList result;
    result.reserve(keys.size());

    // Preferred keys that actually exist, in preferred order
    for (const QString &key : preferredOrder) {
        if (keys.contains(key))
            result.append(key);
    }

    // Remaining keys in their original order
    for (const QString &key : keys) {
        if (!result.contains(key))
            result.append(key);
    }

    return result;
}

// Everything the writer needs for one sensor, gathered and validated before a
// single byte is produced. Sample buffers are shared with the session, never
// copied.
struct SensorPlan {
    QString name;
    QStringList columns;                // output order
    QList<SourceColumn> data;           // parallel to columns
    qsizetype rowCount = 0;             // the validated common length
};

QString textError(const QString &sensor, const QString &text)
{
    return QStringLiteral("Sensor '%1': name/column/unit text cannot be written ('%2')")
        .arg(sensor, text);
}

// Builds the per-sensor plan from the source layer. Returns false with *error
// set when the source layer cannot be expressed in the row format.
bool planSensors(const SourceData &source, QList<SensorPlan> &plans, QString *error)
{
    const QStringList sensors = reorder(source.keys(), kSensorOrder);
    for (const QString &sensorKey : sensors) {
        const SourceSensor sensor = source.value(sensorKey);

        SensorPlan plan;
        plan.name = sensorKey;
        plan.columns = reorder(sensor.keys(), kMeasurementOrder.value(sensorKey));

        if (!CsvFormat::isValidName(sensorKey)) {
            if (error)
                *error = textError(sensorKey, sensorKey);
            return false;
        }

        for (const QString &col : std::as_const(plan.columns)) {
            const SourceColumn column = sensor.value(col);
            if (!CsvFormat::isValidName(col)) {
                if (error)
                    *error = textError(sensorKey, col);
                return false;
            }
            if (!CsvFormat::isValidUnit(column.unit)) {
                if (error)
                    *error = textError(sensorKey, column.unit);
                return false;
            }
            plan.data.append(column);
        }

        // Ragged sensors are an error, checked up front so that the row loop
        // can never read out of range.
        if (!plan.data.isEmpty()) {
            plan.rowCount = plan.data.first().samples.size();
            for (qsizetype c = 1; c < plan.data.size(); ++c) {
                if (plan.data[c].samples.size() == plan.rowCount)
                    continue;
                if (error) {
                    *error = QStringLiteral("Sensor '%1' has columns of unequal length (%2: %3, %4: %5)")
                                 .arg(sensorKey, plan.columns.first())
                                 .arg(plan.rowCount)
                                 .arg(plan.columns[c])
                                 .arg(plan.data[c].samples.size());
                }
                return false;
            }
        }

        plans.append(plan);
    }
    return true;
}

// "$FLYS" line through "$DATA" line. Only stored attributes are read.
QByteArray headerBytes(const SessionData &sessionData, const QList<SensorPlan> &plans)
{
    QString header = QStringLiteral("$FLYS,1\n");

    // Attribute lines (standard attributes first, then any others)
    const QStringList attrKeys = reorder(sessionData.attributeKeys(), kAttributeOrder);
    for (const QString &key : attrKeys) {
        if (!CsvFormat::isValidName(key)) {
            qWarning("DataExporter: attribute '%s' cannot be written (%s)",
                     qPrintable(key), "the key contains ',' or a line break");
            continue;
        }
        const std::optional<QString> text =
            CsvFormat::formatAttributeValue(sessionData.storedAttribute(key));
        if (!text) {
            qWarning("DataExporter: attribute '%s' cannot be written (%s)",
                     qPrintable(key), "the value has no text form");
            continue;
        }
        header += QStringLiteral("$VAR,") + key + QLatin1Char(',') + *text + QLatin1Char('\n');
    }

    // Column and unit headers (standard sensors/columns first, then any others)
    for (const SensorPlan &plan : plans) {
        header += QStringLiteral("$COL,") + plan.name;
        for (const QString &col : plan.columns)
            header += QLatin1Char(',') + col;
        header += QLatin1Char('\n');

        header += QStringLiteral("$UNIT,") + plan.name;
        for (const SourceColumn &column : plan.data)
            header += QLatin1Char(',') + column.unit;
        header += QLatin1Char('\n');
    }

    header += QStringLiteral("$DATA\n");
    return header.toUtf8();
}

// Data rows, handed to `sink` in chunks of about 4 MB so that a large session
// is never held in memory as text. Every number goes through
// CsvFormat::formatDouble - nothing else may format a number.
void writeRows(const QList<SensorPlan> &plans, const std::function<void(const QByteArray &)> &sink)
{
    QByteArray buf;
    buf.reserve(1024 * 1024); // pre-allocate 1 MB

    for (const SensorPlan &plan : plans) {
        if (plan.data.isEmpty() || plan.rowCount == 0)
            continue;

        const QByteArray sensorPrefix = QByteArray("$") + plan.name.toUtf8();

        QVector<const double *> columnData;
        columnData.reserve(plan.data.size());
        for (const SourceColumn &column : plan.data)
            columnData.append(column.samples.constData());

        const qsizetype numCols = columnData.size();
        for (qsizetype i = 0; i < plan.rowCount; ++i) {
            buf.append(sensorPrefix);
            for (qsizetype c = 0; c < numCols; ++c) {
                buf.append(',');
                buf.append(CsvFormat::formatDouble(columnData[c][i]));
            }
            buf.append('\n');

            // Flush buffer periodically to avoid excessive memory use
            if (buf.size() > 4 * 1024 * 1024) {
                sink(buf);
                buf.clear();
            }
        }
    }

    if (!buf.isEmpty())
        sink(buf);
}

QString writeError(const QString &filePath, const QSaveFile &file)
{
    return QStringLiteral("Couldn't write file '%1': %2").arg(filePath, file.errorString());
}

} // anonymous namespace

std::optional<QByteArray> DataExporter::toBytes(const SessionData &sessionData, QString *error)
{
    if (error)
        error->clear();

    // Snapshot of the source layer: implicitly shared, no sample is copied.
    const SourceData source = sessionData.sourceData();

    QList<SensorPlan> plans;
    if (!planSensors(source, plans, error))
        return std::nullopt;

    QByteArray bytes = headerBytes(sessionData, plans);
    writeRows(plans, [&bytes](const QByteArray &chunk) { bytes.append(chunk); });
    return bytes;
}

bool DataExporter::exportSession(const QString &filePath, const SessionData &sessionData, QString *error)
{
    if (error)
        error->clear();

    // Snapshot of the source layer: implicitly shared, no sample is copied.
    const SourceData source = sessionData.sourceData();

    // Validate before the file is opened: on failure nothing is written and
    // whatever is at filePath stays as it is.
    QList<SensorPlan> plans;
    if (!planSensors(source, plans, error))
        return false;

    // Atomic write via QSaveFile
    QSaveFile saveFile(filePath);
    if (!saveFile.open(QIODevice::WriteOnly)) {
        if (error)
            *error = writeError(filePath, saveFile);
        return false;
    }

    saveFile.write(headerBytes(sessionData, plans));
    writeRows(plans, [&saveFile](const QByteArray &chunk) { saveFile.write(chunk); });

    if (saveFile.error() != QFileDevice::NoError) {
        if (error)
            *error = writeError(filePath, saveFile);
        saveFile.cancelWriting();
        return false;
    }

    // Commit atomically
    if (!saveFile.commit()) {
        if (error)
            *error = writeError(filePath, saveFile);
        return false;
    }
    return true;
}

} // namespace FlySight
