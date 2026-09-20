// CsvFormat: the on-disk text forms of numbers and attribute values, and the
// exact-round-trip guarantee (a saved double reloads bit-identical).
// Expectations are literals; "bits"
// means std::memcmp of the two doubles. The one exception is roundTripSweep,
// where the expectation is the input itself - that is the property under test.

#include <QtTest>

#include <cmath>
#include <cstring>
#include <limits>
#include <random>

#include <QDateTime>
#include <QPointF>
#include <QTimeZone>

#include "csvformat.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using FlySightTest::sameBits;

class CsvFormatTest : public QObject {
    Q_OBJECT

private slots:
    void shortestForms_data();
    void shortestForms();
    void negativeZero();
    void roundTripBits_data();
    void roundTripBits();
    void roundTripSweep();
    void nonFinite();
    void attributeForms();
    void names();
    void singleLineForms();
};

void CsvFormatTest::shortestForms_data()
{
    QTest::addColumn<double>("value");
    QTest::addColumn<QByteArray>("text");

    QTest::newRow("0.1") << 0.1 << QByteArray("0.1");
    QTest::newRow("third") << 0.3333333333333333 << QByteArray("0.3333333333333333");
    QTest::newRow("62.5") << 62.5 << QByteArray("62.5");
    QTest::newRow("-125") << -125.0 << QByteArray("-125");
    QTest::newRow("import time") << 1718900000.123 << QByteArray("1718900000.123");
    QTest::newRow("gnss time") << 1704110400.4 << QByteArray("1704110400.4");
    QTest::newRow("denormal") << 1e-320 << QByteArray("1e-320");
    QTest::newRow("min denormal") << 4.9e-324 << QByteArray("5e-324");
    QTest::newRow("max") << 1.7976931348623157e308 << QByteArray("1.7976931348623157e+308");
    QTest::newRow("1e21") << 1e21 << QByteArray("1e+21");
    QTest::newRow("zero") << 0.0 << QByteArray("0");
}

void CsvFormatTest::shortestForms()
{
    QFETCH(double, value);
    QFETCH(QByteArray, text);
    QCOMPARE(CsvFormat::formatDouble(value), text);
}

void CsvFormatTest::negativeZero()
{
    QCOMPARE(CsvFormat::formatDouble(-0.0), QByteArray("-0"));

    double parsed = 1.0;
    QVERIFY(CsvFormat::parseDouble(u"-0", &parsed));
    QVERIFY(std::signbit(parsed));
    QVERIFY(parsed == 0.0);
}

void CsvFormatTest::roundTripBits_data()
{
    QTest::addColumn<double>("value");

    const double values[] = {
        0.1, 0.3333333333333333, 62.5, -125.0, 1718900000.123, 1704110400.4,
        1e-320, 4.9e-324, 1.7976931348623157e308, 1e21, 0.0, -0.0,
        9.80665, 0.0001, 1.14688, 71.67999999999999, 123456789012345680000.0,
        2.2250738585072014e-308, -1.5e-7,
    };
    int i = 0;
    for (double v : values)
        QTest::newRow(qPrintable(QStringLiteral("value %1").arg(i++))) << v;
}

void CsvFormatTest::roundTripBits()
{
    QFETCH(double, value);

    const QByteArray text = CsvFormat::formatDouble(value);
    double parsed = 12345.0;
    QVERIFY2(CsvFormat::parseDouble(QString::fromLatin1(text), &parsed), text.constData());
    QVERIFY2(sameBits(parsed, value), text.constData());
}

void CsvFormatTest::roundTripSweep()
{
    std::mt19937_64 rng(12345);

    int tested = 0;
    while (tested < 200000) {
        const quint64 bits = rng();
        double v = 0.0;
        std::memcpy(&v, &bits, sizeof(double));
        if (!std::isfinite(v))
            continue;
        ++tested;

        const QByteArray text = CsvFormat::formatDouble(v);
        double parsed = 0.0;
        if (!CsvFormat::parseDouble(QString::fromLatin1(text), &parsed) || !sameBits(parsed, v))
            QFAIL(qPrintable(QStringLiteral("no exact round trip for bits 0x%1 (text %2)")
                                 .arg(bits, 16, 16, QLatin1Char('0'))
                                 .arg(QString::fromLatin1(text))));
    }
}

void CsvFormatTest::nonFinite()
{
    const double inf = std::numeric_limits<double>::infinity();

    QCOMPARE(CsvFormat::formatDouble(std::numeric_limits<double>::quiet_NaN()), QByteArray("nan"));
    QCOMPARE(CsvFormat::formatDouble(-std::numeric_limits<double>::quiet_NaN()), QByteArray("nan"));
    QCOMPARE(CsvFormat::formatDouble(inf), QByteArray("inf"));
    QCOMPARE(CsvFormat::formatDouble(-inf), QByteArray("-inf"));

    double parsed = 0.0;
    QVERIFY(CsvFormat::parseDouble(u"nan", &parsed));
    QVERIFY(std::isnan(parsed));
    QVERIFY(CsvFormat::parseDouble(u"inf", &parsed));
    QVERIFY(parsed == inf);
    QVERIFY(CsvFormat::parseDouble(u"-inf", &parsed));
    QVERIFY(parsed == -inf);

    parsed = 7.0;
    QVERIFY(!CsvFormat::parseDouble(u"", &parsed));
    QVERIFY(!CsvFormat::parseDouble(u"abc", &parsed));
    QVERIFY(!CsvFormat::parseDouble(u"1e999", &parsed));
    QVERIFY(!CsvFormat::parseDouble(u"-nan", &parsed));
    QVERIFY(!CsvFormat::parseDouble(u"1,2", &parsed));
    QCOMPARE(parsed, 7.0);      // a failed parse leaves *out alone

    // Everything QStringView::toDouble accepts still loads (the importer's rule for
    // data rows). That includes surrounding whitespace, which toDouble itself
    // ignores; parseDouble adds no trimming of its own and no stricter check,
    // so a file that loaded with the previous parser loads the same way now.
    QVERIFY(CsvFormat::parseDouble(u" 1", &parsed));
    QCOMPARE(parsed, 1.0);
    QVERIFY(CsvFormat::parseDouble(u"NaN", &parsed));
    QVERIFY(std::isnan(parsed));
    QVERIFY(CsvFormat::parseDouble(u"+inf", &parsed));
    QVERIFY(parsed == inf);
}

void CsvFormatTest::attributeForms()
{
    using CsvFormat::formatAttributeValue;

    // The baseline wrote this value as "1.7189e+09".
    QCOMPARE(formatAttributeValue(QVariant(1718900000.123)), std::optional<QString>("1718900000.123"));
    QCOMPARE(formatAttributeValue(QVariant(0.0)), std::optional<QString>("0"));
    QCOMPARE(formatAttributeValue(QVariant(80)), std::optional<QString>("80"));
    QCOMPARE(formatAttributeValue(QVariant(true)), std::optional<QString>("true"));
    QCOMPARE(formatAttributeValue(QVariant(false)), std::optional<QString>("false"));
    QCOMPARE(formatAttributeValue(QVariant(qulonglong(18446744073709551615ULL))),
             std::optional<QString>("18446744073709551615"));
    QCOMPARE(formatAttributeValue(QVariant(qlonglong(-9000000000LL))),
             std::optional<QString>("-9000000000"));

    QCOMPARE(formatAttributeValue(QVariant(QStringLiteral("Perris, run 2, windy"))),
             std::optional<QString>("Perris, run 2, windy"));
    QCOMPARE(formatAttributeValue(QVariant(QStringLiteral(" padded "))),
             std::optional<QString>(" padded "));
    QCOMPARE(formatAttributeValue(QVariant(QString(""))), std::optional<QString>(""));
    QCOMPARE(formatAttributeValue(QVariant(QStringLiteral("a\r\nb\nc"))),
             std::optional<QString>("a b c"));
    QCOMPARE(formatAttributeValue(QVariant(QByteArray("bytes\nhere"))),
             std::optional<QString>("bytes here"));

    const QDateTime utc(QDate(2024, 6, 20), QTime(16, 13, 20, 123), QTimeZone::utc());
    QCOMPARE(formatAttributeValue(QVariant(utc)), std::optional<QString>("2024-06-20T16:13:20.123Z"));
    const QDateTime plusTwo = utc.toTimeZone(QTimeZone::fromSecondsAheadOfUtc(2 * 3600));
    QCOMPARE(plusTwo.time(), QTime(18, 13, 20, 123));
    QCOMPARE(formatAttributeValue(QVariant(plusTwo)), std::optional<QString>("2024-06-20T16:13:20.123Z"));
    QCOMPARE(formatAttributeValue(QVariant(QDate(2024, 6, 20))), std::optional<QString>("2024-06-20"));
    QCOMPARE(formatAttributeValue(QVariant(QTime(16, 13, 20, 123))), std::optional<QString>("16:13:20.123"));

    QCOMPARE(formatAttributeValue(QVariant(std::numeric_limits<double>::quiet_NaN())),
             std::optional<QString>("nan"));

    QVERIFY(!formatAttributeValue(QVariant()).has_value());
    QVERIFY(!formatAttributeValue(QVariant::fromValue(QPointF(1.0, 2.0))).has_value());
}

void CsvFormatTest::names()
{
    QVERIFY(CsvFormat::isValidName(QStringLiteral("IMU")));
    QVERIFY(CsvFormat::isValidName(QStringLiteral("bar#1")));
    QVERIFY(CsvFormat::isValidName(QStringLiteral("my col")));
    QVERIFY(!CsvFormat::isValidName(QString()));
    QVERIFY(!CsvFormat::isValidName(QStringLiteral("a,b")));
    QVERIFY(!CsvFormat::isValidName(QStringLiteral("a\nb")));
    QVERIFY(!CsvFormat::isValidName(QStringLiteral("a\rb")));

    QVERIFY(CsvFormat::isValidUnit(QString()));
    QVERIFY(CsvFormat::isValidUnit(QStringLiteral("deg C")));
    QVERIFY(!CsvFormat::isValidUnit(QStringLiteral("m,s")));
    QVERIFY(!CsvFormat::isValidUnit(QStringLiteral("m\ns")));
}

void CsvFormatTest::singleLineForms()
{
    QCOMPARE(CsvFormat::singleLine(QStringLiteral("plain, text")), QStringLiteral("plain, text"));
    QCOMPARE(CsvFormat::singleLine(QStringLiteral("a\r\nb")), QStringLiteral("a b"));
    QCOMPARE(CsvFormat::singleLine(QStringLiteral("a\n\nb")), QStringLiteral("a  b"));
    QCOMPARE(CsvFormat::singleLine(QStringLiteral("a\rb\n")), QStringLiteral("a b "));
    QCOMPARE(CsvFormat::singleLine(QString(QChar(0x2028)) + QChar(0x2029)), QStringLiteral("  "));
}

FLYSIGHT_TEST_MAIN(CsvFormatTest)
#include "tst_csvformat.moc"
