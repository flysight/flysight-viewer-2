// The sensor configuration vocabulary (src/sensorconfiguration.*): the nine
// header keys, the values each accepts, the message for one it does not, and
// the default of the four IMU keys with the firmware it describes. Every
// expectation is a literal.

#include <QtTest>

#include "sensorconfiguration.h"
#include "testmain.h"

using namespace FlySight;

namespace {

// Every value a key with a list accepts, as the specification writes them.
QStringList listed(const QString &key)
{
    const QStringList imuRates = {"12.5", "26", "52", "104", "208", "416", "833", "1666", "3333", "6666"};
    if (key == QLatin1String("ACCEL_FS_G"))
        return {"2", "4", "8", "16"};
    if (key == QLatin1String("GYRO_FS_DEG_S"))
        return {"250", "500", "1000", "2000"};
    if (key == QLatin1String("ACCEL_ODR_HZ"))
        return QStringList({"1.6"}) + imuRates;
    if (key == QLatin1String("GYRO_ODR_HZ"))
        return imuRates;
    if (key == QLatin1String("GNSS_MODEL"))
        return {"portable", "stationary", "pedestrian", "automotive", "sea", "airborne_1g", "airborne_2g",
                "airborne_4g"};
    return {};
}

} // namespace

class SensorConfigurationTest : public QObject {
    Q_OBJECT

private slots:
    void keysAndValueForms();
    void malformedValues_data();
    void malformedValues();
    void unsupportedMessage();
    void defaultIsTheFirmwareConfiguration();
};

// Clauses 1 and 5: exactly nine keys, with the unit in the name; every listed
// value of every listed key accepted, also with surrounding whitespace; the
// free-form keys accept a plain positive decimal. No key names a filter.
void SensorConfigurationTest::keysAndValueForms()
{
    QCOMPARE(QString::fromLatin1(SensorConfiguration::AccelFsG), QStringLiteral("ACCEL_FS_G"));
    QCOMPARE(QString::fromLatin1(SensorConfiguration::GyroFsDegS), QStringLiteral("GYRO_FS_DEG_S"));
    QCOMPARE(QString::fromLatin1(SensorConfiguration::AccelOdrHz), QStringLiteral("ACCEL_ODR_HZ"));
    QCOMPARE(QString::fromLatin1(SensorConfiguration::GyroOdrHz), QStringLiteral("GYRO_ODR_HZ"));
    QCOMPARE(QString::fromLatin1(SensorConfiguration::BaroOdrHz), QStringLiteral("BARO_ODR_HZ"));
    QCOMPARE(QString::fromLatin1(SensorConfiguration::HumOdrHz), QStringLiteral("HUM_ODR_HZ"));
    QCOMPARE(QString::fromLatin1(SensorConfiguration::MagOdrHz), QStringLiteral("MAG_ODR_HZ"));
    QCOMPARE(QString::fromLatin1(SensorConfiguration::GnssModel), QStringLiteral("GNSS_MODEL"));
    QCOMPARE(QString::fromLatin1(SensorConfiguration::GnssRateHz), QStringLiteral("GNSS_RATE_HZ"));

    const QStringList keys = {"ACCEL_FS_G", "GYRO_FS_DEG_S", "ACCEL_ODR_HZ", "GYRO_ODR_HZ", "BARO_ODR_HZ",
                              "HUM_ODR_HZ", "MAG_ODR_HZ", "GNSS_MODEL", "GNSS_RATE_HZ"};
    QCOMPARE(SensorConfiguration::keys(), keys);
    for (const QString &key : keys)
        QVERIFY2(SensorConfiguration::isKey(key), qPrintable(key));

    // The filters are not configurable in the firmware: no key names one
    for (const QString &key : SensorConfiguration::keys()) {
        QVERIFY2(!key.contains(QStringLiteral("LPF")) && !key.contains(QStringLiteral("FILTER"))
                     && !key.contains(QStringLiteral("BW")),
                 qPrintable(key));
    }
    for (const char *other : {"SCHEMA_VER", "FIRMWARE_VER", "accel_fs_g", "ACCEL_FS", "ACCEL_LPF", ""})
        QVERIFY2(!SensorConfiguration::isKey(QString::fromLatin1(other)), other);
    QVERIFY(!SensorConfiguration::isValidValue(QStringLiteral("SCHEMA_VER"), QStringLiteral("2")));

    // Every listed value, as is and with surrounding whitespace
    int listedCount = 0;
    for (const QString &key : keys) {
        for (const QString &value : listed(key)) {
            QVERIFY2(SensorConfiguration::isValidValue(key, value), qPrintable(key + QLatin1Char('=') + value));
            QVERIFY2(SensorConfiguration::isValidValue(key, QStringLiteral(" ") + value + QStringLiteral("\t ")),
                     qPrintable(key + QStringLiteral("= ") + value));
            ++listedCount;
        }
    }
    QCOMPARE(listedCount, 4 + 4 + 11 + 10 + 8);

    // The free-form keys
    for (const char *key : {"BARO_ODR_HZ", "HUM_ODR_HZ", "MAG_ODR_HZ", "GNSS_RATE_HZ"}) {
        for (const char *value : {"1", "25", "12.5", "0.5", "007", "1000000", " 5 "}) {
            QVERIFY2(SensorConfiguration::isValidValue(QString::fromLatin1(key), QString::fromLatin1(value)),
                     qPrintable(QStringLiteral("%1=%2").arg(key, value)));
        }
    }
}

void SensorConfigurationTest::malformedValues_data()
{
    QTest::addColumn<QString>("key");
    QTest::addColumn<QVariant>("value");

    const QStringList listedKeys = {"ACCEL_FS_G", "GYRO_FS_DEG_S", "ACCEL_ODR_HZ", "GYRO_ODR_HZ", "GNSS_MODEL"};
    const QStringList freeKeys = {"BARO_ODR_HZ", "HUM_ODR_HZ", "MAG_ODR_HZ", "GNSS_RATE_HZ"};

    // Every key: the empty value, an invalid variant (a "$VAR,<key>" line
    // without a value reads as ""), text, an exponent, a sign, non-finite
    for (const QString &key : listedKeys + freeKeys) {
        QTest::addRow("%s empty", qPrintable(key)) << key << QVariant(QString());
        QTest::addRow("%s blank", qPrintable(key)) << key << QVariant(QStringLiteral("   "));
        QTest::addRow("%s invalid variant", qPrintable(key)) << key << QVariant();
        QTest::addRow("%s abc", qPrintable(key)) << key << QVariant(QStringLiteral("abc"));
        QTest::addRow("%s 1e2", qPrintable(key)) << key << QVariant(QStringLiteral("1e2"));
        QTest::addRow("%s nan", qPrintable(key)) << key << QVariant(QStringLiteral("nan"));
        QTest::addRow("%s inf", qPrintable(key)) << key << QVariant(QStringLiteral("inf"));
        QTest::addRow("%s 0", qPrintable(key)) << key << QVariant(QStringLiteral("0"));
        QTest::addRow("%s -1", qPrintable(key)) << key << QVariant(QStringLiteral("-1"));
        QTest::addRow("%s +16", qPrintable(key)) << key << QVariant(QStringLiteral("+16"));
    }

    // Listed keys: only the exact spellings
    QTest::newRow("ACCEL_FS_G 16.0") << "ACCEL_FS_G" << QVariant(QStringLiteral("16.0"));
    QTest::newRow("ACCEL_FS_G 016") << "ACCEL_FS_G" << QVariant(QStringLiteral("016"));
    QTest::newRow("ACCEL_FS_G 32") << "ACCEL_FS_G" << QVariant(QStringLiteral("32"));
    QTest::newRow("ACCEL_FS_G 16 g") << "ACCEL_FS_G" << QVariant(QStringLiteral("16 g"));
    QTest::newRow("GYRO_FS_DEG_S 2000.0") << "GYRO_FS_DEG_S" << QVariant(QStringLiteral("2000.0"));
    QTest::newRow("GYRO_FS_DEG_S 02000") << "GYRO_FS_DEG_S" << QVariant(QStringLiteral("02000"));
    QTest::newRow("GYRO_FS_DEG_S 125") << "GYRO_FS_DEG_S" << QVariant(QStringLiteral("125"));
    QTest::newRow("ACCEL_ODR_HZ 12.50") << "ACCEL_ODR_HZ" << QVariant(QStringLiteral("12.50"));
    QTest::newRow("ACCEL_ODR_HZ 3332") << "ACCEL_ODR_HZ" << QVariant(QStringLiteral("3332"));
    QTest::newRow("ACCEL_ODR_HZ 13") << "ACCEL_ODR_HZ" << QVariant(QStringLiteral("13"));
    QTest::newRow("GYRO_ODR_HZ 1.6") << "GYRO_ODR_HZ" << QVariant(QStringLiteral("1.6"));
    QTest::newRow("GYRO_ODR_HZ 6664") << "GYRO_ODR_HZ" << QVariant(QStringLiteral("6664"));
    QTest::newRow("GYRO_ODR_HZ 012.5") << "GYRO_ODR_HZ" << QVariant(QStringLiteral("012.5"));
    QTest::newRow("GNSS_MODEL Portable") << "GNSS_MODEL" << QVariant(QStringLiteral("Portable"));
    QTest::newRow("GNSS_MODEL AIRBORNE_4G") << "GNSS_MODEL" << QVariant(QStringLiteral("AIRBORNE_4G"));
    QTest::newRow("GNSS_MODEL airborne 4g") << "GNSS_MODEL" << QVariant(QStringLiteral("airborne 4g"));
    QTest::newRow("GNSS_MODEL 8") << "GNSS_MODEL" << QVariant(QStringLiteral("8"));

    // Free-form keys: a plain positive decimal and nothing else
    for (const QString &key : freeKeys) {
        QTest::addRow("%s 0.0", qPrintable(key)) << key << QVariant(QStringLiteral("0.0"));
        QTest::addRow("%s .5", qPrintable(key)) << key << QVariant(QStringLiteral(".5"));
        QTest::addRow("%s 5.", qPrintable(key)) << key << QVariant(QStringLiteral("5."));
        QTest::addRow("%s 1.2.3", qPrintable(key)) << key << QVariant(QStringLiteral("1.2.3"));
        QTest::addRow("%s 1,5", qPrintable(key)) << key << QVariant(QStringLiteral("1,5"));
        QTest::addRow("%s 0x10", qPrintable(key)) << key << QVariant(QStringLiteral("0x10"));
        QTest::addRow("%s 10 Hz", qPrintable(key)) << key << QVariant(QStringLiteral("10 Hz"));
    }
}

// Clause 3: each of these is malformed
void SensorConfigurationTest::malformedValues()
{
    QFETCH(QString, key);
    QFETCH(QVariant, value);
    QVERIFY(SensorConfiguration::isKey(key));
    QVERIFY(!SensorConfiguration::isValidValue(key, value));
}

// Clause 3: the message names the key and the value as recorded (untrimmed),
// in the form of the SCHEMA_VER message, the list in the order of the
// specification
void SensorConfigurationTest::unsupportedMessage()
{
    QCOMPARE(SensorConfiguration::unsupportedMessage(QStringLiteral("ACCEL_FS_G"), QStringLiteral("16.0")),
             QStringLiteral("Unsupported ACCEL_FS_G '16.0' (supported: 2, 4, 8, 16)"));
    QCOMPARE(SensorConfiguration::unsupportedMessage(QStringLiteral("GYRO_FS_DEG_S"), QStringLiteral(" 3000")),
             QStringLiteral("Unsupported GYRO_FS_DEG_S ' 3000' (supported: 250, 500, 1000, 2000)"));
    QCOMPARE(SensorConfiguration::unsupportedMessage(QStringLiteral("ACCEL_ODR_HZ"), QString()),
             QStringLiteral("Unsupported ACCEL_ODR_HZ '' (supported: 1.6, 12.5, 26, 52, 104, 208, 416, 833, "
                            "1666, 3333, 6666)"));
    QCOMPARE(SensorConfiguration::unsupportedMessage(QStringLiteral("GYRO_ODR_HZ"), QStringLiteral("1.6")),
             QStringLiteral("Unsupported GYRO_ODR_HZ '1.6' (supported: 12.5, 26, 52, 104, 208, 416, 833, 1666, "
                            "3333, 6666)"));
    QCOMPARE(SensorConfiguration::unsupportedMessage(QStringLiteral("GNSS_MODEL"), QStringLiteral("Portable")),
             QStringLiteral("Unsupported GNSS_MODEL 'Portable' (supported: portable, stationary, pedestrian, "
                            "automotive, sea, airborne_1g, airborne_2g, airborne_4g)"));
    QCOMPARE(SensorConfiguration::unsupportedMessage(QStringLiteral("BARO_ODR_HZ"), QStringLiteral("0")),
             QStringLiteral("Unsupported BARO_ODR_HZ '0' (supported: a positive decimal number)"));
    QCOMPARE(SensorConfiguration::unsupportedMessage(QStringLiteral("GNSS_RATE_HZ"), QStringLiteral("1e2")),
             QStringLiteral("Unsupported GNSS_RATE_HZ '1e2' (supported: a positive decimal number)"));
}

// Clause 4: the default is firmware v2023.09.22's: +/-16 g, +/-2000 deg/s,
// 12.5 Hz for both sensors, every one a value its key accepts; no dynamic
// model and no rate
void SensorConfigurationTest::defaultIsTheFirmwareConfiguration()
{
    QCOMPARE(QString::fromLatin1(SensorConfiguration::FirmwareVersion), QStringLiteral("v2023.09.22"));

    const struct { const char *key; const char *value; } defaults[] = {
        {"ACCEL_FS_G", "16"}, {"GYRO_FS_DEG_S", "2000"}, {"ACCEL_ODR_HZ", "12.5"}, {"GYRO_ODR_HZ", "12.5"}};
    for (const auto &entry : defaults) {
        const QString key = QString::fromLatin1(entry.key);
        const std::optional<QString> value = SensorConfiguration::defaultValue(key);
        QVERIFY2(value.has_value(), entry.key);
        QCOMPARE(*value, QString::fromLatin1(entry.value));
        QVERIFY2(SensorConfiguration::isValidValue(key, *value), entry.key);
    }

    for (const char *key : {"BARO_ODR_HZ", "HUM_ODR_HZ", "MAG_ODR_HZ", "GNSS_MODEL", "GNSS_RATE_HZ"})
        QVERIFY2(!SensorConfiguration::defaultValue(QString::fromLatin1(key)).has_value(), key);
    QVERIFY(!SensorConfiguration::defaultValue(QStringLiteral("SCHEMA_VER")).has_value());
}

FLYSIGHT_TEST_MAIN(SensorConfigurationTest)

#include "tst_sensor_configuration.moc"
