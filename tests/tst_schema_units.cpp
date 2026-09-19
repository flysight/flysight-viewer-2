// The two tables behind the conversion layer: the schema table
// (src/conversion/schematable.*) and the unit normalization table
// (src/units/unitconversion.h). Every expectation is a literal.

#include <QtTest>

#include "conversion/schematable.h"
#include "testmain.h"
#include "units/unitconversion.h"

using namespace FlySight;

namespace {

int g_warningCount = 0;

void countingHandler(QtMsgType type, const QMessageLogContext &, const QString &)
{
    if (type == QtWarningMsg || type == QtCriticalMsg)
        ++g_warningCount;
}

} // namespace

class SchemaUnitsTest : public QObject {
    Q_OBJECT

private slots:
    // schema table
    void supportedVersions();
    void parseVersion_data();
    void parseVersion();
    void parseVersionInvalidVariant();
    void unsupportedMessage();
    void schemaDependence();
    void correctionScale();

    // unit normalization table
    void convertingUnits();
    void identityUnits_data();
    void identityUnits();
    void unknownUnitsPassThroughVerbatim();
    void lookupTrimsButLabelOfUnknownIsVerbatim();
    void requiresConversion();
    void lookupsAreSilent();
};

// ─────────────────────────────── schema table

void SchemaUnitsTest::supportedVersions()
{
    QCOMPARE(Schema::supportedVersions(), QList<int>({1, 2}));
    QCOMPARE(Schema::ImpliedVersion, 1);
    QCOMPARE(QString::fromLatin1(Schema::AttributeKey), QStringLiteral("SCHEMA_VER"));
}

void SchemaUnitsTest::parseVersion_data()
{
    QTest::addColumn<QVariant>("recorded");
    QTest::addColumn<int>("expected");     // 0 = nullopt

    QTest::newRow("1") << QVariant(QStringLiteral("1")) << 1;
    QTest::newRow("2") << QVariant(QStringLiteral("2")) << 2;
    QTest::newRow("leading space") << QVariant(QStringLiteral(" 2")) << 2;
    QTest::newRow("int 2") << QVariant(2) << 2;
    QTest::newRow("empty") << QVariant(QStringLiteral("")) << 0;
    QTest::newRow("abc") << QVariant(QStringLiteral("abc")) << 0;
    QTest::newRow("3") << QVariant(QStringLiteral("3")) << 0;
    QTest::newRow("0") << QVariant(QStringLiteral("0")) << 0;
    QTest::newRow("2.0") << QVariant(QStringLiteral("2.0")) << 0;
    QTest::newRow("-1") << QVariant(QStringLiteral("-1")) << 0;
    QTest::newRow("+2") << QVariant(QStringLiteral("+2")) << 0;
    QTest::newRow("overflow") << QVariant(QStringLiteral("99999999999999999999")) << 0;
}

void SchemaUnitsTest::parseVersion()
{
    QFETCH(QVariant, recorded);
    QFETCH(int, expected);

    const std::optional<int> version = Schema::parseVersion(recorded);
    if (expected == 0) {
        QVERIFY(!version.has_value());
    } else {
        QVERIFY(version.has_value());
        QCOMPARE(*version, expected);
    }
}

void SchemaUnitsTest::parseVersionInvalidVariant()
{
    QVERIFY(!Schema::parseVersion(QVariant()).has_value());
}

void SchemaUnitsTest::unsupportedMessage()
{
    QCOMPARE(Schema::unsupportedMessage(QStringLiteral("3")),
             QStringLiteral("Unsupported SCHEMA_VER '3' (supported: 1, 2)"));
    QCOMPARE(Schema::unsupportedMessage(QStringLiteral("abc")),
             QStringLiteral("Unsupported SCHEMA_VER 'abc' (supported: 1, 2)"));
    QCOMPARE(Schema::unsupportedMessage(QStringLiteral("")),
             QStringLiteral("Unsupported SCHEMA_VER '' (supported: 1, 2)"));
}

void SchemaUnitsTest::schemaDependence()
{
    QVERIFY(Schema::isSchemaDependent("IMU", "wx"));
    QVERIFY(Schema::isSchemaDependent("IMU", "wy"));
    QVERIFY(Schema::isSchemaDependent("IMU", "wz"));

    QVERIFY(!Schema::isSchemaDependent("IMU", "wTotal"));
    QVERIFY(!Schema::isSchemaDependent("IMU", "ax"));
    QVERIFY(!Schema::isSchemaDependent("GNSS", "wx"));
}

void SchemaUnitsTest::correctionScale()
{
    // Exact: the factor is the decimal literal, not a quotient.
    QVERIFY(Schema::correctionScale(1, "IMU", "wx") == 1.14688);
    QVERIFY(Schema::correctionScale(1, "IMU", "wy") == 1.14688);
    QVERIFY(Schema::correctionScale(1, "IMU", "wz") == 1.14688);

    QVERIFY(Schema::correctionScale(2, "IMU", "wx") == 1.0);
    QVERIFY(Schema::correctionScale(1, "IMU", "ax") == 1.0);
    QVERIFY(Schema::correctionScale(1, "GNSS", "wx") == 1.0);
    QVERIFY(Schema::correctionScale(7, "IMU", "wx") == 1.0);
}

// ─────────────────────────────── unit normalization table

void SchemaUnitsTest::convertingUnits()
{
    const ConversionSpec g = UnitConversion::getConversion("g");
    QVERIFY(g.scale == 9.80665);
    QVERIFY(g.offset == 0.0);
    QCOMPARE(g.siUnit, QStringLiteral("m/s^2"));

    const ConversionSpec gauss = UnitConversion::getConversion("gauss");
    QVERIFY(gauss.scale == 0.0001);
    QVERIFY(gauss.offset == 0.0);
    QCOMPARE(gauss.siUnit, QStringLiteral("T"));
}

void SchemaUnitsTest::identityUnits_data()
{
    QTest::addColumn<QString>("unitText");
    QTest::addColumn<QString>("label");

    // Relabelled only
    QTest::newRow("deg C") << "deg C" << "degC";
    QTest::newRow("(m)") << "(m)" << "m";
    QTest::newRow("(m/s)") << "(m/s)" << "m/s";
    QTest::newRow("(deg)") << "(deg)" << "deg";
    QTest::newRow("volt") << "volt" << "V";
    QTest::newRow("percent") << "percent" << "%";

    // Already internal, including the labels released Viewer versions wrote
    QTest::newRow("m/s^2") << "m/s^2" << "m/s^2";
    QTest::newRow("T") << "T" << "T";
    QTest::newRow("degC") << "degC" << "degC";
    QTest::newRow("deg/s") << "deg/s" << "deg/s";
    QTest::newRow("m") << "m" << "m";
    QTest::newRow("m/s") << "m/s" << "m/s";
    QTest::newRow("Pa") << "Pa" << "Pa";
    QTest::newRow("s") << "s" << "s";
    QTest::newRow("deg") << "deg" << "deg";
    QTest::newRow("V") << "V" << "V";
    QTest::newRow("%") << "%" << "%";
    QTest::newRow("empty") << "" << "";
}

void SchemaUnitsTest::identityUnits()
{
    QFETCH(QString, unitText);
    QFETCH(QString, label);

    const ConversionSpec spec = UnitConversion::getConversion(unitText);
    QVERIFY(spec.scale == 1.0);
    QVERIFY(spec.offset == 0.0);
    QCOMPARE(spec.siUnit, label);
}

void SchemaUnitsTest::unknownUnitsPassThroughVerbatim()
{
    const ConversionSpec furlongs = UnitConversion::getConversion("furlongs");
    QVERIFY(furlongs.scale == 1.0);
    QVERIFY(furlongs.offset == 0.0);
    QCOMPARE(furlongs.siUnit, QStringLiteral("furlongs"));

    const ConversionSpec odd = UnitConversion::getConversion(" odd unit ");
    QVERIFY(odd.scale == 1.0);
    QVERIFY(odd.offset == 0.0);
    QCOMPARE(odd.siUnit, QStringLiteral(" odd unit "));
}

void SchemaUnitsTest::lookupTrimsButLabelOfUnknownIsVerbatim()
{
    // The lookup key is the trimmed text ...
    const ConversionSpec g = UnitConversion::getConversion(" g ");
    QVERIFY(g.scale == 9.80665);
    QCOMPARE(g.siUnit, QStringLiteral("m/s^2"));

    // ... and the table is case sensitive: "G" is not "g".
    const ConversionSpec upper = UnitConversion::getConversion("G");
    QVERIFY(upper.scale == 1.0);
    QCOMPARE(upper.siUnit, QStringLiteral("G"));
}

void SchemaUnitsTest::requiresConversion()
{
    QVERIFY(UnitConversion::requiresConversion("g"));
    QVERIFY(UnitConversion::requiresConversion("gauss"));

    for (const char *unit : {"deg C", "degC", "m/s^2", "T", "deg/s", "m", "(m)", "(m/s)", "m/s", "Pa",
                             "s", "deg", "(deg)", "V", "volt", "%", "percent", "", "furlongs",
                             " odd unit "}) {
        QVERIFY2(!UnitConversion::requiresConversion(QString::fromLatin1(unit)), unit);
    }
}

void SchemaUnitsTest::lookupsAreSilent()
{
    g_warningCount = 0;
    const QtMessageHandler previous = qInstallMessageHandler(countingHandler);

    for (const char *unit : {"g", "gauss", "deg C", "m/s^2", "T", "degC", "deg/s", "", "(m/s)",
                             "percent", "furlongs", " odd unit "}) {
        UnitConversion::getConversion(QString::fromLatin1(unit));
        UnitConversion::requiresConversion(QString::fromLatin1(unit));
    }

    qInstallMessageHandler(previous);
    QCOMPARE(g_warningCount, 0);
}

FLYSIGHT_TEST_MAIN(SchemaUnitsTest)

#include "tst_schema_units.moc"
