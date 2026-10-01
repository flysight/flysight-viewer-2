#include "sensorconfiguration.h"

#include <cmath>

namespace FlySight {
namespace SensorConfiguration {

namespace {

// The IMU's output data rates as the part names them, ascending. The
// accelerometer has one more, its low-power 1.6 Hz, which the gyro lacks.
const char *const kImuRates[] = {
    "12.5", "26", "52", "104", "208", "416", "833", "1666", "3333", "6666"
};

// The values a key with a list accepts, in the order its message prints them;
// empty for a key without a list (a positive decimal).
QStringList listedValues(const QString &key)
{
    if (key == QLatin1String(AccelFsG))
        return {QStringLiteral("2"), QStringLiteral("4"), QStringLiteral("8"), QStringLiteral("16")};
    if (key == QLatin1String(GyroFsDegS))
        return {QStringLiteral("250"), QStringLiteral("500"), QStringLiteral("1000"), QStringLiteral("2000")};
    if (key == QLatin1String(AccelOdrHz) || key == QLatin1String(GyroOdrHz)) {
        QStringList rates;
        if (key == QLatin1String(AccelOdrHz))
            rates.append(QStringLiteral("1.6"));
        for (const char *rate : kImuRates)
            rates.append(QString::fromLatin1(rate));
        return rates;
    }
    if (key == QLatin1String(GnssModel)) {
        return {QStringLiteral("portable"), QStringLiteral("stationary"), QStringLiteral("pedestrian"),
                QStringLiteral("automotive"), QStringLiteral("sea"), QStringLiteral("airborne_1g"),
                QStringLiteral("airborne_2g"), QStringLiteral("airborne_4g")};
    }
    return {};
}

// Digits with an optional fraction ("12.5", "7"), no sign, no exponent, and
// above zero: "0", "0.0", "-1", "+1", "1e2", ".5", "5." and "nan" are not.
bool isPositiveDecimal(const QString &text)
{
    const qsizetype n = text.size();
    const auto digitsFrom = [&text, n](qsizetype from) {
        qsizetype end = from;
        while (end < n && text.at(end) >= QLatin1Char('0') && text.at(end) <= QLatin1Char('9'))
            ++end;
        return end;
    };

    const qsizetype integerEnd = digitsFrom(0);
    if (integerEnd == 0)
        return false;
    if (integerEnd < n) {
        if (text.at(integerEnd) != QLatin1Char('.'))
            return false;
        const qsizetype fractionEnd = digitsFrom(integerEnd + 1);
        if (fractionEnd == integerEnd + 1 || fractionEnd != n)
            return false;
    }

    bool ok = false;
    const double value = text.toDouble(&ok);
    return ok && std::isfinite(value) && value > 0.0;
}

} // namespace

QStringList keys()
{
    return {QString::fromLatin1(AccelFsG), QString::fromLatin1(GyroFsDegS), QString::fromLatin1(AccelOdrHz),
            QString::fromLatin1(GyroOdrHz), QString::fromLatin1(BaroOdrHz), QString::fromLatin1(HumOdrHz),
            QString::fromLatin1(MagOdrHz), QString::fromLatin1(GnssModel), QString::fromLatin1(GnssRateHz)};
}

bool isKey(const QString &key)
{
    return keys().contains(key);
}

bool isValidValue(const QString &key, const QVariant &recorded)
{
    if (!isKey(key))
        return false;
    const QString text = recorded.toString().trimmed();
    const QStringList listed = listedValues(key);
    if (!listed.isEmpty())
        return listed.contains(text);   // exact and case-sensitive
    return isPositiveDecimal(text);
}

QString unsupportedMessage(const QString &key, const QVariant &recorded)
{
    const QStringList listed = listedValues(key);
    const QString supported = listed.isEmpty() ? QStringLiteral("a positive decimal number")
                                               : listed.join(QStringLiteral(", "));
    return QStringLiteral("Unsupported %1 '%2' (supported: %3)").arg(key, recorded.toString(), supported);
}

std::optional<QString> defaultValue(const QString &key)
{
    // Firmware FirmwareVersion: +/-16 g, +/-2000 deg/s, 12.5 Hz for both sensors
    if (key == QLatin1String(AccelFsG))
        return QStringLiteral("16");
    if (key == QLatin1String(GyroFsDegS))
        return QStringLiteral("2000");
    if (key == QLatin1String(AccelOdrHz) || key == QLatin1String(GyroOdrHz))
        return QStringLiteral("12.5");
    return std::nullopt;
}

} // namespace SensorConfiguration
} // namespace FlySight
