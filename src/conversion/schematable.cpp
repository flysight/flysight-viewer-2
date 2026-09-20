#include "schematable.h"

#include <QStringList>

namespace FlySight {
namespace Schema {

namespace {

// Legacy FlySight 2 firmware scaled the LSM6DSO gyroscope by the nominal
// full-scale range (2000 deg/s / 32768 counts) instead of ST's sensitivity of
// 0.070 deg/s per count, so schema 1 gyro rates are too small by this factor
// (see docs/DATA_SCHEMA.md). Written as the decimal literal on purpose: the
// expression 0.070 / (2000.0 / 32768.0) evaluates to a different double.
constexpr double kLegacyGyroScale = 1.14688;

struct CorrectionRow {
    int version;
    const char *sensor;
    const char *name;
    double scale;
};

// Schema 2 has no rows: it is recorded correctly.
const CorrectionRow kCorrections[] = {
    {1, "IMU", "wx", kLegacyGyroScale},
    {1, "IMU", "wy", kLegacyGyroScale},
    {1, "IMU", "wz", kLegacyGyroScale},
};

bool rowMatches(const CorrectionRow &row, const QString &sensor, const QString &name)
{
    return sensor == QLatin1String(row.sensor) && name == QLatin1String(row.name);
}

} // namespace

QList<int> supportedVersions()
{
    return {1, 2};
}

std::optional<int> parseVersion(const QVariant &recorded)
{
    const QString text = recorded.toString().trimmed();
    if (text.isEmpty())
        return std::nullopt;

    // Decimal digits only: no sign, no fraction, no exponent.
    for (const QChar c : text) {
        if (c < QLatin1Char('0') || c > QLatin1Char('9'))
            return std::nullopt;
    }

    bool ok = false;
    const int version = text.toInt(&ok);
    if (!ok || !supportedVersions().contains(version))
        return std::nullopt;
    return version;
}

QString unsupportedMessage(const QVariant &recorded)
{
    QStringList supported;
    for (int version : supportedVersions())
        supported << QString::number(version);

    return QStringLiteral("Unsupported %1 '%2' (supported: %3)")
        .arg(QLatin1String(AttributeKey), recorded.toString(), supported.join(QStringLiteral(", ")));
}

bool isSchemaDependent(const QString &sensor, const QString &name)
{
    for (const CorrectionRow &row : kCorrections) {
        if (rowMatches(row, sensor, name))
            return true;
    }
    return false;
}

double correctionScale(int version, const QString &sensor, const QString &name)
{
    for (const CorrectionRow &row : kCorrections) {
        if (row.version == version && rowMatches(row, sensor, name))
            return row.scale;
    }
    return 1.0;
}

} // namespace Schema
} // namespace FlySight
