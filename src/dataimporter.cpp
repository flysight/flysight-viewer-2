// import.cpp

#include "dataimporter.h"
#include "conversion/schematable.h"
#include "preferences/preferencesmanager.h"
#include "preferences/preferencekeys.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QRegularExpression>
#include <QStringTokenizer>
#include <QTextStream>
#include <utility>

namespace FlySight {

// ---- staging ---------------------------------------------------------------
// Everything parsed from a file lives here until the whole file has been
// accepted. Nothing reaches a SessionData before publish().

struct DataImporter::StagedSensor {
    QVector<QString> columns;               // labels, verbatim, in file order
    QVector<QString> units;                 // unit text, verbatim; same size as columns
    QVector<QVector<double>> samples;       // one vector per column
    bool hasUnitLine = false;
};

struct DataImporter::StagedFile {
    QVector<QPair<QString, QString>> attributes;    // file order; conflicts already rejected
    QMap<QString, StagedSensor> sensors;
    int skippedRows = 0;
};

namespace {

// Splits on ',' keeping empty parts.
QVector<QString> splitFields(QStringView text)
{
    QVector<QString> fields;
    for (QStringView part : QStringTokenizer(text, u','))
        fields.append(part.toString());
    return fields;
}

} // namespace

bool DataImporter::importFile(const QString& fileName, SessionData& sessionData) {
    m_lastError.clear();

    QByteArray fileData;
    if (!readFile(fileName, sessionData, &fileData)) {
        return false;
    }
    initializeFromDevice(fileName, fileData, sessionData);
    return true;
}

bool DataImporter::readFile(const QString& fileName, SessionData& sessionData, QByteArray* fileData) {
    m_lastError.clear();

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        m_lastError = "Couldn't read file";
        return false;
    }

    // Read the entire file into a QByteArray
    QByteArray localData = file.readAll();

    if (localData.isEmpty()) {
        m_lastError = "Empty file";
        return false;
    }

    // Extract the first line directly from QByteArray
    int firstNewline = localData.indexOf('\n');
    QString firstLine;
    if (firstNewline != -1) {
        firstLine = QString::fromUtf8(localData.left(firstNewline)).trimmed();
    } else {
        firstLine = QString::fromUtf8(localData).trimmed();
    }

    // Determine file type based on the first line
    FS_FileType fileType;
    if (firstLine.startsWith("time,lat,lon,hMSL")) {
        fileType = FS_FileType::FS1;
    } else if (firstLine.startsWith("$FLYS")) {
        fileType = FS_FileType::FS2;
    } else {
        m_lastError = "Unknown file format";
        return false;
    }

    // Create a QTextStream from the QByteArray
    QTextStream in(&localData, QIODevice::ReadOnly);

    // Parse and validate into the staging structure. The session is not
    // touched until the whole file has been accepted.
    StagedFile staged;
    bool ok = false;
    switch (fileType) {
    case FS_FileType::FS1:
        ok = importSimple(in, staged, DefaultSensorId);
        break;
    case FS_FileType::FS2:
        ok = importFS2(in, staged);
        break;
    }
    if (!ok) {
        return false;
    }

    // Real recordings end in a truncated line after power loss and now and
    // then contain a glitched row: tolerated, and reported once per file.
    if (staged.skippedRows > 0) {
        qWarning("%s: skipped %d malformed data row(s)", qPrintable(fileName), staged.skippedRows);
    }

    // ---- publication: nothing below this line can fail ----
    publish(staged, sessionData);

    // Optionally copy raw bytes to the caller
    if (fileData) {
        *fileData = localData;
    }

    return true;
}

void DataImporter::publish(const StagedFile& staged, SessionData& sessionData) {
    // Header attributes exactly as recorded, unknown keys included. Nothing is
    // added: a file that does not declare SCHEMA_VER yields a session without it.
    for (const auto &attribute : staged.attributes) {
        sessionData.setAttribute(attribute.first, attribute.second);
    }

    // Each staged vector is handed over, not copied (implicit sharing). Declared
    // columns of a file without rows are published as empty source measurements.
    SourceData source;
    for (auto sensorIt = staged.sensors.constBegin(); sensorIt != staged.sensors.constEnd(); ++sensorIt) {
        const StagedSensor &sensor = sensorIt.value();
        SourceSensor &columns = source[sensorIt.key()];
        for (int i = 0; i < sensor.columns.size(); ++i) {
            columns.insert(sensor.columns[i], SourceColumn{ sensor.samples[i], sensor.units[i] });
        }
    }
    sessionData.mergeSourceData(source);
}

bool DataImporter::structuralError(int lineNumber, const QString& reason) {
    m_lastError = QStringLiteral("Line %1: %2").arg(lineNumber).arg(reason);
    return false;
}

void DataImporter::initializeFromDevice(const QString& fileName, const QByteArray& fileData, SessionData& sessionData) {
    // Set default description
    sessionData.setAttribute(SessionKeys::Description, getDescription(fileName));

    // Determine file type from the first line for device ID extraction
    int firstNewline = fileData.indexOf('\n');
    QString firstLine = (firstNewline != -1)
        ? QString::fromUtf8(fileData.left(firstNewline)).trimmed()
        : QString::fromUtf8(fileData).trimmed();
    bool isFS2 = firstLine.startsWith("$FLYS");

    // Attempt to extract DEVICE_ID based on file type
    if (!sessionData.hasAttribute(SessionKeys::DeviceId)) {
        if (isFS2) {
            extractDeviceId(fileName, sessionData, "Device_ID");
        } else {
            extractDeviceId(fileName, sessionData, "Processor serial number");
        }
    }

    // If DEVICE_ID still isn't set, set it
    if (!sessionData.hasAttribute(SessionKeys::DeviceId)) {
        sessionData.setAttribute(SessionKeys::DeviceId, "n/a");
    }

    // After importing, check if SESSION_ID is set
    if (!sessionData.hasAttribute(SessionKeys::SessionId)) {
        // Compute MD5 hash of fileData
        QByteArray md5Hash = QCryptographicHash::hash(fileData, QCryptographicHash::Md5);
        QString md5HashString = md5Hash.toHex();
        sessionData.setAttribute(SessionKeys::SessionId, md5HashString);
    }

    // Record the import time
    double now = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch() / 1000.0;
    sessionData.setAttribute(SessionKeys::ImportTime, now);

    // Set wind defaults (placeholder; will be calculated per-track in future)
    sessionData.setAttribute(SessionKeys::WindN, 0.0);
    sessionData.setAttribute(SessionKeys::WindE, 0.0);

    // Set aerodynamic defaults from preferences
    PreferencesManager &prefs = PreferencesManager::instance();
    sessionData.setAttribute(SessionKeys::JumperMass, prefs.getValue(PreferenceKeys::AeroMass));
    sessionData.setAttribute(SessionKeys::PlanformArea, prefs.getValue(PreferenceKeys::AeroArea));

    // For fixed ground elevation mode, bake the value at import time
    QString groundMode = prefs.getValue(PreferenceKeys::ImportGroundReferenceMode).toString();
    if (groundMode == "Fixed") {
        double fixedElev = prefs.getValue(PreferenceKeys::ImportFixedElevation).toDouble();
        sessionData.setAttribute(SessionKeys::GroundElev, fixedElev);
    }
}

bool DataImporter::importSimple(QTextStream& in, StagedFile& staged, const QString &sensorName) {
    StagedSensor sensor;

    // Line 1: column names. Empty parts are kept: dropping one would silently
    // shift every later column under the wrong name.
    sensor.columns = splitFields(in.readLine());
    for (int i = 0; i < sensor.columns.size(); ++i) {
        const QString &column = sensor.columns[i];
        if (column.isEmpty()) {
            return structuralError(1, QStringLiteral("column header has an empty name"));
        }
        if (sensor.columns.indexOf(column) != i) {
            return structuralError(1, QStringLiteral("column header repeats '") + column + QLatin1Char('\''));
        }
    }
    sensor.samples.resize(sensor.columns.size());

    // Line 2: units, verbatim (the parenthesized FS1 form included). A file
    // that ends after the column line is valid and has no rows.
    if (!in.atEnd()) {
        sensor.units = splitFields(in.readLine());
        sensor.hasUnitLine = true;
        if (sensor.units.size() > sensor.columns.size()) {
            return structuralError(2, QStringLiteral("unit line has more units than columns"));
        }
    }
    sensor.units.resize(sensor.columns.size());     // missing units are ""

    staged.sensors.insert(sensorName, sensor);

    // Data lines
    while (!in.atEnd()) {
        importDataRow(in.readLine(), staged, sensorName);
    }

    return true;
}

bool DataImporter::importFS2(QTextStream& in, StagedFile& staged) {
    FS_Section section = FS_Section::HEADER;
    int lineNumber = 0;

    // Header lines, up to and including $DATA
    while (!in.atEnd() && (section == FS_Section::HEADER)) {
        const QString line = in.readLine();
        ++lineNumber;
        if (!importHeaderRow(line, lineNumber, staged, section)) {
            return false;
        }
    }

    if (section != FS_Section::DATA) {
        m_lastError = "Missing $DATA section";
        return false;
    }

    // Validate the declared schema before reading any data row: only
    // SCHEMA_VER decides the schema, and a value Viewer does not understand
    // rejects the file. An absent SCHEMA_VER is not an error and stays absent.
    for (const auto &attribute : std::as_const(staged.attributes)) {
        if (attribute.first == QLatin1String(Schema::AttributeKey)
            && !Schema::parseVersion(attribute.second).has_value()) {
            m_lastError = Schema::unsupportedMessage(attribute.second);
            return false;
        }
    }

    // Data lines
    while (!in.atEnd()) {
        importDataRow(in.readLine(), staged, QString());
    }

    return true;
}

bool DataImporter::importHeaderRow(const QString& line, int lineNumber, StagedFile& staged, FS_Section& section)
{
    if (line.trimmed() == QLatin1String("$DATA")) {
        section = FS_Section::DATA;
        return true;
    }

    const qsizetype firstComma = line.indexOf(u',');
    const QStringView lineView(line);
    const QStringView token0 = (firstComma < 0) ? lineView : lineView.left(firstComma);
    const QStringView rest = (firstComma < 0) ? QStringView() : lineView.mid(firstComma + 1);

    if (token0 == u"$VAR") {
        // $VAR,<key>,<value>: the value is everything after the second comma,
        // verbatim, commas included. No second comma: the value is "".
        const qsizetype secondComma = rest.indexOf(u',');
        const QString key = ((secondComma < 0) ? rest : rest.left(secondComma)).toString();
        const QString value = (secondComma < 0) ? QString() : rest.mid(secondComma + 1).toString();

        if (key.isEmpty()) {
            return structuralError(lineNumber, QStringLiteral("$VAR with empty name"));
        }
        for (const auto &attribute : std::as_const(staged.attributes)) {
            if (attribute.first != key)
                continue;
            if (attribute.second != value) {
                return structuralError(lineNumber, QStringLiteral("conflicting values for $VAR ") + key);
            }
            return true;    // the same key with an equal value collapses
        }
        staged.attributes.append(qMakePair(key, value));
        return true;
    }

    if (token0 == u"$COL") {
        const QVector<QString> fields = splitFields(rest);
        const QString sensorName = fields.isEmpty() ? QString() : fields.first();
        if (firstComma < 0 || sensorName.isEmpty()) {
            return structuralError(lineNumber, QStringLiteral("$COL without sensor name"));
        }
        if (staged.sensors.contains(sensorName)) {
            return structuralError(lineNumber, QStringLiteral("duplicate $COL for sensor ") + sensorName);
        }
        if (fields.size() < 2) {
            return structuralError(lineNumber, QStringLiteral("$COL ") + sensorName + QStringLiteral(" has no columns"));
        }

        StagedSensor sensor;
        for (int i = 1; i < fields.size(); ++i) {
            const QString &column = fields[i];
            if (column.isEmpty()) {
                return structuralError(lineNumber, QStringLiteral("$COL ") + sensorName
                                                   + QStringLiteral(" has an empty column name"));
            }
            // A repeated label would receive every row twice.
            if (sensor.columns.contains(column)) {
                return structuralError(lineNumber, QStringLiteral("$COL ") + sensorName
                                                   + QStringLiteral(" repeats column '") + column + QLatin1Char('\''));
            }
            sensor.columns.append(column);  // verbatim, custom columns included
        }
        sensor.units.resize(sensor.columns.size());     // "" until a $UNIT line says otherwise
        sensor.samples.resize(sensor.columns.size());
        staged.sensors.insert(sensorName, sensor);
        return true;
    }

    if (token0 == u"$UNIT") {
        const QVector<QString> fields = splitFields(rest);
        const QString sensorName = fields.isEmpty() ? QString() : fields.first();

        auto sensorIt = staged.sensors.find(sensorName);
        if (firstComma < 0 || sensorIt == staged.sensors.end()) {
            return structuralError(lineNumber, QStringLiteral("$UNIT for unknown sensor ") + sensorName);
        }
        StagedSensor &sensor = sensorIt.value();
        if (sensor.hasUnitLine) {
            return structuralError(lineNumber, QStringLiteral("duplicate $UNIT for sensor ") + sensorName);
        }
        if (fields.size() - 1 > sensor.columns.size()) {
            return structuralError(lineNumber, QStringLiteral("$UNIT ") + sensorName
                                               + QStringLiteral(" has more units than columns"));
        }

        // Verbatim, not trimmed. Fewer units than columns: the rest stay "".
        for (int i = 1; i < fields.size(); ++i) {
            sensor.units[i - 1] = fields[i];
        }
        sensor.hasUnitLine = true;
        return true;
    }

    // Blank lines, $FLYS, and anything else: ignored, for forward compatibility.
    return true;
}

void DataImporter::importDataRow(const QString& line, StagedFile& staged, const QString& fixedSensor) {
    if (line.isEmpty()) {
        return;     // a blank line is not a row
    }

    QStringView lineView(line);
    QStringTokenizer tokenizer(lineView, u',');
    auto it = tokenizer.begin();

    // FS2: the first token is "$<sensor>". FS1: every row belongs to one sensor.
    QString key = fixedSensor;
    if (fixedSensor.isEmpty()) {
        if (it == tokenizer.end() || !(*it).startsWith(u'$')) {
            ++staged.skippedRows;
            return;
        }
        key = (*it).mid(1).toString();
        ++it;
    }

    auto sensorIt = staged.sensors.find(key);
    if (sensorIt == staged.sensors.end()) {
        // No $COL declared this sensor
        ++staged.skippedRows;
        return;
    }
    StagedSensor &sensor = sensorIt.value();
    const int columnCount = int(sensor.columns.size());

    // A row is appended only when every field parsed: all or nothing.
    QVector<double> values;
    values.reserve(columnCount);

    for (; it != tokenizer.end(); ++it) {
        const QStringView field = *it;

        if (values.size() == columnCount || field.isEmpty()) {
            // Too many fields, or an empty one
            ++staged.skippedRows;
            return;
        }

        if (field.endsWith(u'Z')) {
            // ISO-8601 UTC date-time -> seconds since the epoch, millisecond precision
            const QDateTime dt = QDateTime::fromString(field.toString(), Qt::ISODate);
            if (!dt.isValid()) {
                ++staged.skippedRows;
                return;
            }
            values.append(dt.toMSecsSinceEpoch() / 1000.0);
        } else {
            // Correctly rounded double; no further processing
            bool ok = false;
            const double value = field.toDouble(&ok);
            if (!ok) {
                ++staged.skippedRows;
                return;
            }
            values.append(value);
        }
    }

    if (values.size() != columnCount) {
        // Too few fields: typically the last line of a recording cut short by power loss
        ++staged.skippedRows;
        return;
    }

    for (int i = 0; i < columnCount; ++i) {
        sensor.samples[i].append(values[i]);
    }
}

void DataImporter::extractDeviceId(const QString& fileName, SessionData& sessionData, const QString& expectedKey) {
    // Find the root directory of the FlySight device
    QString flySightRoot = findFlySightRoot(fileName);

    if (flySightRoot.isEmpty()) {
        // FLYSIGHT.TXT not found
        qWarning() << "FLYSIGHT.TXT not found in any parent directories of:" << fileName;
        return;
    }

    QString flysightTxtPath = QDir(flySightRoot).absoluteFilePath("FLYSIGHT.TXT");

    // Open FLYSIGHT.TXT and search for the expected key
    QFile flysightFile(flysightTxtPath);
    if (!flysightFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // Fail silently if the file cannot be opened
        qWarning() << "Failed to open FLYSIGHT.TXT at:" << flysightTxtPath;
        return;
    }

    QTextStream in(&flysightFile);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();

        // Handle comments: ignore everything after ';'
        int commentIndex = line.indexOf(';');
        if (commentIndex != -1) {
            line = line.left(commentIndex).trimmed();
        }

        if (line.isEmpty()) {
            // Skip empty lines
            continue;
        }

        // Split the line into key and value at the first ':'
        int colonIndex = line.indexOf(':');
        if (colonIndex == -1) {
            // No colon found; invalid line format
            qWarning() << "Invalid line format (no colon found):" << line;
            continue;
        }

        QString key = line.left(colonIndex).trimmed();
        QString value = line.mid(colonIndex + 1).trimmed();

        if (key == expectedKey) {
            sessionData.setAttribute(SessionKeys::DeviceId, value);
            return;
        }
    }

    // Optionally, log that the expected key was not found
    qWarning() << expectedKey << "not found in FLYSIGHT.TXT";
}

QString DataImporter::findFlySightRoot(const QString& filePath) {
    QFileInfo fileInfo(filePath);
    QDir currentDir = fileInfo.absoluteDir();

    while (true) {
        QString flysightPath = currentDir.absoluteFilePath("FLYSIGHT.TXT");
        if (QFile::exists(flysightPath)) {
            // Found FLYSIGHT.TXT in the current directory
            return currentDir.absolutePath();
        }

        if (!currentDir.cdUp()) {
            // Reached the root of the filesystem without finding FLYSIGHT.TXT
            break;
        }
    }

    // FLYSIGHT.TXT not found
    return QString();
}

QString DataImporter::getLastError() const {
    return m_lastError;
}

QString DataImporter::getDescription(const QString& fileName)
{
    QString description;

    QFileInfo fileInfo(fileName);
    QDir parentDir = fileInfo.dir();

    QString baseName = fileInfo.baseName();
    QString parentName = parentDir.dirName();

    // Declare the regular expression as static to avoid recompiling it on each function call
    static const QRegularExpression timePattern("^\\d{2}-\\d{2}-\\d{2}$", QRegularExpression::CaseInsensitiveOption);

    if (timePattern.match(baseName).hasMatch()) {
        description = baseName;
    }

    while (timePattern.match(parentName).hasMatch()) {
        if (!description.isEmpty()) {
            description = QString("/") + description;
        }

        // Add folder name to description
        description = parentName + description;

        parentDir.cdUp();
        parentName = parentDir.dirName();
    }

    if (description.isEmpty()) {
        description = fileInfo.fileName();
    }

    return description;
}

} // namespace FlySight
