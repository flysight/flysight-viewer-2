// import.cpp

#include "dataimporter.h"
#include "preferences/preferencesmanager.h"
#include "preferences/preferencekeys.h"
#include "units/unitconversion.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringTokenizer>
#include <QTextStream>

namespace FlySight {

bool DataImporter::importFile(const QString& fileName, SessionData& sessionData) {
    QByteArray fileData;
    if (!readFile(fileName, sessionData, &fileData)) {
        return false;
    }
    initializeFromDevice(fileName, fileData, sessionData);
    return true;
}

bool DataImporter::readFile(const QString& fileName, SessionData& sessionData, QByteArray* fileData) {
    // The importer writes measurements straight into the session's storage
    // (friend access), bypassing the setters that notify the calculation
    // engine. Whatever path leaves this function, drop every calculated value
    // so nothing computed from the previous contents survives. With a fresh
    // target session this is a no-op.
    struct InvalidateOnExit {
        SessionData &session;
        ~InvalidateOnExit() { session.invalidateAllCalculations(); }
    } invalidateOnExit{sessionData};

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

    // Optionally copy raw bytes to the caller
    if (fileData) {
        *fileData = localData;
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

    // Use the fileType to choose the import method
    switch (fileType) {
    case FS_FileType::FS1:
        importSimple(in, sessionData, "GNSS");
        break;
    case FS_FileType::FS2:
        importFS2(in, sessionData);
        break;
    }

    return true;
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

void DataImporter::importSimple(QTextStream& in, SessionData& sessionData, const QString &sensorName) {
    QMap<QString, QVector<QString>> columnOrder;

    // Read the first line (column names)
    QString columnLine = in.readLine();
    QVector<QString> columns = columnLine.split(',', Qt::SkipEmptyParts).toVector().toList().toVector();
    columnOrder[sensorName] = columns;

    // Initialize the data map in sensors
    QMap<QString, QVector<double>>& sensor = sessionData.m_sensors[sensorName];
    for (const QString& colName : columns) {
        sensor[colName]; // Initialize empty QVector<double> for each column
    }

    // Read the second line (units) and capture for SI conversion
    if (in.atEnd()) {
        return;
    }
    QString unitLine = in.readLine();
    QVector<QString> columnUnits;
    for (const auto& part : unitLine.split(',')) {
        columnUnits.append(part.trimmed());
    }

    // Process data lines
    while (!in.atEnd()) {
        QString dataLine = in.readLine();
        importDataRow(dataLine, columnOrder, sessionData, sensorName);
    }

    // Store initial unit strings in SessionData
    const QVector<QString>& cols = columnOrder[sensorName];
    for (int i = 0; i < cols.size() && i < columnUnits.size(); ++i) {
        sessionData.setUnit(sensorName, cols[i], columnUnits[i]);
    }

    // Apply SI normalization based on unit text
    for (int i = 0; i < cols.size() && i < columnUnits.size(); ++i) {
        const QString& unitText = columnUnits[i];
        if (UnitConversion::requiresConversion(unitText)) {
            QVector<double>& data = sessionData.m_sensors[sensorName][cols[i]];
            UnitConversion::toSI(data, unitText);
        }
        // Update stored unit to the post-conversion SI unit
        ConversionSpec spec = UnitConversion::getConversion(unitText);
        sessionData.setUnit(sensorName, cols[i], spec.siUnit);
    }
}

void DataImporter::importFS2(QTextStream& in, SessionData& sessionData) {
    FS_Section section = FS_Section::HEADER;

    // Temporary map to store column order per sensor
    QMap<QString, QVector<QString>> columnOrder;

    // Temporary map to store unit text per sensor (for SI conversion)
    QMap<QString, QVector<QString>> columnUnits;

    // Read and process header lines
    while (!in.atEnd() && (section == FS_Section::HEADER)) {
        QString line = in.readLine();
        section = importHeaderRow(line, columnOrder, columnUnits, sessionData);
    }

    // Process data lines
    while (!in.atEnd()) {
        QString line = in.readLine();
        importDataRow(line, columnOrder, sessionData, "");
    }

    // Apply SI normalization based on unit text
    for (auto sensorIt = columnUnits.constBegin();
         sensorIt != columnUnits.constEnd(); ++sensorIt) {
        const QString& sensor = sensorIt.key();
        const QVector<QString>& units = sensorIt.value();
        const QVector<QString>& cols = columnOrder.value(sensor);

        for (int i = 0; i < cols.size() && i < units.size(); ++i) {
            const QString& unitText = units[i];
            if (UnitConversion::requiresConversion(unitText)) {
                QVector<double>& data = sessionData.m_sensors[sensor][cols[i]];
                UnitConversion::toSI(data, unitText);
            }
            // Update stored unit to the post-conversion SI unit
            ConversionSpec spec = UnitConversion::getConversion(unitText);
            sessionData.setUnit(sensor, cols[i], spec.siUnit);
        }
    }
}

DataImporter::FS_Section DataImporter::importHeaderRow(
    const QString& line,
    QMap<QString, QVector<QString>>& columnOrder,
    QMap<QString, QVector<QString>>& columnUnits,
    SessionData& sessionData)
{
    // By default, stay in HEADER section
    FS_Section section = FS_Section::HEADER;

    if (line == "$DATA") {
        section = FS_Section::DATA;
    } else {
        QStringView lineView(line);
        QStringTokenizer tokenizer(lineView, u',');
        auto it = tokenizer.begin();

        if (it != tokenizer.end()) {
            QStringView token0 = *it++;
            if (token0 == u"$VAR") {
                if (it != tokenizer.end()) {
                    QStringView attributeName = *it++;
                    if (it != tokenizer.end()) {
                        QStringView attributeValue = *it;
                        sessionData.setAttribute(attributeName.toString(), attributeValue.toString());
                    }
                }
            } else if (token0 == u"$COL") {
                if (it != tokenizer.end()) {
                    QStringView sensorName = *it++;
                    QVector<QString> columns;

                    for (; it != tokenizer.end(); ++it) {
                        columns.append(it->toString());
                    }

                    columnOrder[sensorName.toString()] = columns;

                    // Initialize sensor measurements as empty
                    for (const QString &colName : columns) {
                        sessionData.setMeasurement(sensorName.toString(), colName, {});
                    }
                }
            } else if (token0 == u"$UNIT") {
                // Parse and store unit text per sensor for SI conversion
                if (it != tokenizer.end()) {
                    QString sensorName = (*it++).toString();
                    QVector<QString> units;
                    for (; it != tokenizer.end(); ++it) {
                        units.append(it->toString());
                    }
                    columnUnits[sensorName] = units;

                    // Store unit strings in SessionData for later export
                    const QVector<QString>& cols = columnOrder.value(sensorName);
                    for (int i = 0; i < cols.size() && i < units.size(); ++i) {
                        sessionData.setUnit(sensorName, cols[i], units[i]);
                    }
                }
            }
        }
    }

    return section;
}

void DataImporter::importDataRow(const QString& line, const QMap<QString, QVector<QString>>& columnOrder, SessionData& sessionData, QString key) {
    QStringView lineView(line);
    QStringTokenizer tokenizer(lineView, u',');
    auto it = tokenizer.begin();

    // For FS2 format, the first token is the sensor key
    if (line.startsWith("$")) {
        if (it == tokenizer.end()) return;
        QStringView token0 = *it++;
        key = token0.mid(1).toString(); // Remove the '$' at the start
    }

    // Get the column order for this sensor
    const QVector<QString>& cols = columnOrder.value(key);
    if (cols.isEmpty()) {
        // Unknown sensor or no columns defined
        return;
    }

    QVector<QStringView> dataFields;
    for (; it != tokenizer.end(); ++it) {
        dataFields.append(*it);
    }

    if (dataFields.size() != cols.size()) {
        // Handle error: number of data fields does not match number of columns
        return;
    }

    // Temporary storage for parsed values
    QVector<double> tempValues(cols.size());

    // Iterate through each field once
    for (int i = 0; i < cols.size(); ++i) {
        const QString& colName = cols[i];
        const QStringView& dataValueView = dataFields[i];

        if (dataValueView.isEmpty()) {
            // Empty field, skip the entire row
            qWarning() << "Skipping row due to empty field:" << line;
            return;
        }
        else if (dataValueView.endsWith(u'Z')) {
            // Attempt to parse date
            QDateTime dt = QDateTime::fromString(dataValueView.toString(), Qt::ISODate);
            if (!dt.isValid()) {
                // Invalid date found, skip the entire row
                qWarning() << "Skipping row due to invalid date:" << line;
                return;
            }
            // Convert to seconds since epoch
            tempValues[i] = dt.toMSecsSinceEpoch() / 1000.0;
        }
        else {
            // Attempt to parse as double
            bool ok;
            double val = dataValueView.toDouble(&ok);
            if (!ok) {
                // Invalid value found, skip the entire row
                qWarning() << "Skipping row due to invalid value:" << line;
                return;
            }
            tempValues[i] = val;
        }
    }

    // All fields are valid, append to SessionData
    QMap<QString, QVector<double>>& sensor = sessionData.m_sensors[key];
    for (int i = 0; i < cols.size(); ++i) {
        const QString& colName = cols[i];
        sensor[colName].append(tempValues[i]);
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
