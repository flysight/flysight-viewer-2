// The conversion layer (src/conversion/sourceconversion.*) on the calculation
// engine, against a fake session state and a private registry: schema
// correction, unit normalization, their order, buffer sharing, candidate
// selection by SCHEMA_VER, and the dependencies that make the choice follow
// the attribute.
//
// Every expectation is a literal. 62.5 * 1.14688 evaluates to
// 71.679999999999993, which is not the double nearest 71.68, so corrected gyro
// values are compared with an absolute tolerance of 1e-9, never with ==.
// Values whose conversion multiplies exactly representable inputs (1 g,
// 1 gauss, identities, zero) are compared with ==.

#include <cmath>
#include <memory>

#include <QtTest>

#include "calculations/imucalculations.h"
#include "conversion/sourceconversion.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "fakesessionstate.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;
using Synthetic::attr;
using Synthetic::measKey;

namespace {

bool isNear(double a, double b)
{
    return qAbs(a - b) <= 1e-9;
}

const QString kSchemaFamily = QStringLiteral("builtin.conversion.schema");
const QString kDefaultFamily = QStringLiteral("builtin.conversion.default");

int g_warningCount = 0;
QStringList g_warnings;

void countingHandler(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg) {
        ++g_warningCount;
        g_warnings.append(message);
    }
}

// Counts warnings for as long as it lives.
class WarningCounter {
public:
    WarningCounter()
    {
        g_warningCount = 0;
        g_warnings.clear();
        m_previous = qInstallMessageHandler(countingHandler);
    }
    ~WarningCounter() { qInstallMessageHandler(m_previous); }
    int count() const { return g_warningCount; }
    QStringList messages() const { return g_warnings; }

private:
    QtMessageHandler m_previous = nullptr;
};

// A private registry holding only the conversion layer, a fake session state,
// and an engine over them. Members are destroyed in reverse order, so the
// engine goes before the registry.
struct World {
    CalculationRegistry registry;
    FakeSessionState state;
    std::unique_ptr<CalculationEngine> engine;

    World()
    {
        Calculations::registerSourceConversions(registry);
        engine = std::make_unique<CalculationEngine>(&state, &registry);
    }

    void addLegacyGyro()
    {
        state.setMeasurement("IMU", "wx", {62.5}, "deg/s");
        state.setMeasurement("IMU", "wy", {-125.0}, "deg/s");
        state.setMeasurement("IMU", "wz", {0.0}, "deg/s");
    }

    double first(const char *sensor, const char *name)
    {
        const QVector<double> values = engine->measurement(sensor, name);
        return values.size() == 1 ? values.at(0) : qQNaN();
    }
};

} // namespace

class ConversionEngineTest : public QObject {
    Q_OBJECT

private slots:
    void registration();

    void legacyGyroCorrected();
    void schema2Unchanged();
    void schema1Explicit();
    void absentSchemaDependencyRecorded();
    void schemaAppearsLater();
    void unsupportedInMemory();

    void unitNormalization();
    void internalLabelsAreIdentity();
    void identitySharesBuffer();
    void negativeZeroSurvivesIdentity();
    void orderSchemaThenUnit();

    void hashInColumnName();
    void emptySourceUnavailable();
    void unitChangeInvalidates();
    void derivedNotConverted();
};

void ConversionEngineTest::registration()
{
    CalculationRegistry registry;
    Calculations::registerSourceConversions(registry);

    QCOMPARE(registry.registeredIds(),
             QList<CalculationId>({"builtin.conversion.schema", "builtin.conversion.default"}));
    QVERIFY(registry.isFamily("builtin.conversion.schema"));
    QVERIFY(registry.isFamily("builtin.conversion.default"));
    QVERIFY(registry.hasSourceConversions());

    // Conversions are not ordinary candidates for a name.
    QVERIFY(registry.candidatesFor(measKey("IMU", "wx")).isEmpty());
    QCOMPARE(registry.sourceConversionsFor("IMU", "wx").size(), 2);
    QCOMPARE(registry.sourceConversionsFor("IMU", "wx").at(0).instanceId,
             QStringLiteral("builtin.conversion.schema#IMU/wx"));
    QCOMPARE(registry.sourceConversionsFor("IMU", "wx").at(1).instanceId,
             QStringLiteral("builtin.conversion.default#IMU/wx"));
    QCOMPARE(registry.sourceConversionsFor("IMU", "ax").size(), 1);
    QCOMPARE(registry.sourceConversionsFor("IMU", "ax").at(0).instanceId,
             QStringLiteral("builtin.conversion.default#IMU/ax"));

    // Source inputs stay reserved for the conversion layer.
    WarningCounter warnings;
    CalculationDescriptor plain;
    plain.id = QStringLiteral("test.readsSource");
    plain.inputs = { CalcInput::sourceMeasurement("IMU", "wx") };
    plain.outputs = { attr("X") };
    plain.compute = [](const EvaluationContext &) { return CalculationResult(); };
    QVERIFY(!registry.registerCalculation(plain));
    QVERIFY(!registry.contains("test.readsSource"));
}

void ConversionEngineTest::legacyGyroCorrected()
{
    World w;
    w.addLegacyGyro();

    QVERIFY(isNear(w.first("IMU", "wx"), 71.68));
    QVERIFY(isNear(w.first("IMU", "wy"), -143.36));
    QVERIFY(w.first("IMU", "wz") == 0.0);
    QCOMPARE(w.engine->measurementUnit("IMU", "wx"), QStringLiteral("deg/s"));

    const std::optional<ResultStatus> schemaStatus = w.engine->resultStatus(kSchemaFamily, measKey("IMU", "wx"));
    QVERIFY(schemaStatus.has_value());
    QCOMPARE(*schemaStatus, ResultStatus::MissingInput);
    QCOMPARE(w.engine->runCountForInstance("builtin.conversion.schema#IMU/wx"), 0);
    QCOMPARE(w.engine->runCountForInstance("builtin.conversion.default#IMU/wx"), 1);
}

void ConversionEngineTest::schema2Unchanged()
{
    World w;
    w.addLegacyGyro();
    w.state.setAttribute("SCHEMA_VER", QStringLiteral("2"));

    const QVector<double> source = w.state.sourceMeasurement("IMU", "wx");
    const QVector<double> effective = w.engine->measurement("IMU", "wx");
    QCOMPARE(effective, QVector<double>({62.5}));
    QCOMPARE(w.engine->measurement("IMU", "wy"), QVector<double>({-125.0}));
    QCOMPARE(w.engine->measurement("IMU", "wz"), QVector<double>({0.0}));
    QCOMPARE(w.engine->measurementUnit("IMU", "wx"), QStringLiteral("deg/s"));

    // The schema family won; the default candidate was never tried.
    QCOMPARE(w.engine->runCountForInstance("builtin.conversion.schema#IMU/wx"), 1);
    QCOMPARE(w.engine->runCountForInstance("builtin.conversion.default#IMU/wx"), 0);

    // No correction and no unit scaling: no second buffer.
    QVERIFY(source.constData() == effective.constData());
}

void ConversionEngineTest::schema1Explicit()
{
    World w;
    w.addLegacyGyro();
    w.state.setAttribute("SCHEMA_VER", QStringLiteral("1"));

    QVERIFY(isNear(w.first("IMU", "wx"), 71.68));
    QVERIFY(isNear(w.first("IMU", "wy"), -143.36));
    QCOMPARE(w.engine->runCountForInstance("builtin.conversion.schema#IMU/wx"), 1);
    QCOMPARE(w.engine->runCountForInstance("builtin.conversion.default#IMU/wx"), 0);
}

void ConversionEngineTest::absentSchemaDependencyRecorded()
{
    World w;
    w.addLegacyGyro();
    w.state.setMeasurement("IMU", "ax", {1.0}, "g");

    w.engine->measurement("IMU", "wx");
    w.engine->measurement("IMU", "ax");

    // The rejected candidate is a dependency of the name, so the choice is
    // revisited when SCHEMA_VER appears.
    const QSet<GraphNode> wxDeps = w.engine->dependenciesOf(GraphNode::resolution(measKey("IMU", "wx")));
    const QSet<GraphNode> expected = {
        GraphNode::sourceMeasurement("IMU", "wx"),
        GraphNode::result("builtin.conversion.schema#IMU/wx"),
        GraphNode::result("builtin.conversion.default#IMU/wx")
    };
    QCOMPARE(wxDeps, expected);

    // The rejected candidate stopped at the attribute, which it recorded.
    QVERIFY(w.engine->dependenciesOf(GraphNode::result("builtin.conversion.schema#IMU/wx"))
                .contains(GraphNode::resolution(attr("SCHEMA_VER"))));

    // A measurement that no schema version corrects never looks at SCHEMA_VER.
    const QSet<GraphNode> axDeps = w.engine->dependenciesOf(GraphNode::resolution(measKey("IMU", "ax")));
    const QSet<GraphNode> expectedAx = {
        GraphNode::sourceMeasurement("IMU", "ax"),
        GraphNode::result("builtin.conversion.default#IMU/ax")
    };
    QCOMPARE(axDeps, expectedAx);
    QVERIFY(!w.engine->dependenciesOf(GraphNode::result("builtin.conversion.default#IMU/ax"))
                 .contains(GraphNode::resolution(attr("SCHEMA_VER"))));
}

void ConversionEngineTest::schemaAppearsLater()
{
    World w;
    w.addLegacyGyro();
    w.state.setMeasurement("IMU", "ax", {1.0}, "g");

    QVERIFY(isNear(w.first("IMU", "wx"), 71.68));
    QVERIFY(w.first("IMU", "ax") == 9.80665);

    // SCHEMA_VER appears: only the gyro is invalidated, and nothing computes.
    int runs = w.engine->totalRunCount();
    QSet<DependencyKey> invalidated = w.state.setAttribute(*w.engine, "SCHEMA_VER", QStringLiteral("2"));
    QVERIFY(invalidated.contains(measKey("IMU", "wx")));
    QVERIFY(!invalidated.contains(measKey("IMU", "ax")));
    QCOMPARE(w.engine->totalRunCount(), runs);

    QCOMPARE(w.engine->measurement("IMU", "wx"), QVector<double>({62.5}));
    QCOMPARE(w.engine->runCountForInstance("builtin.conversion.schema#IMU/wx"), 1);
    QCOMPARE(w.engine->runCountForInstance("builtin.conversion.default#IMU/ax"), 1);

    // SCHEMA_VER goes away: the default candidate is selected again, and its
    // result - which never looked at the attribute - is still valid.
    runs = w.engine->totalRunCount();
    invalidated = w.state.removeAttribute(*w.engine, "SCHEMA_VER");
    QVERIFY(invalidated.contains(measKey("IMU", "wx")));
    QVERIFY(!invalidated.contains(measKey("IMU", "ax")));
    QCOMPARE(w.engine->totalRunCount(), runs);

    QVERIFY(isNear(w.first("IMU", "wx"), 71.68));
    QCOMPARE(w.engine->runCountForInstance("builtin.conversion.default#IMU/wx"), 1);

    QVERIFY(w.engine->verifyAgainstFresh({measKey("IMU", "wx"), measKey("IMU", "wy"),
                                          measKey("IMU", "wz"), measKey("IMU", "ax")}).isEmpty());
}

void ConversionEngineTest::unsupportedInMemory()
{
    // Unreachable through import or logbook load, which reject such files. An
    // explicit but unrecognized declaration is never treated as legacy data.
    World w;
    w.addLegacyGyro();
    w.state.setAttribute("SCHEMA_VER", QStringLiteral("3"));

    {
        WarningCounter warnings;
        QCOMPARE(w.engine->measurement("IMU", "wx"), QVector<double>({62.5}));
        QCOMPARE(w.engine->measurement("IMU", "wx"), QVector<double>({62.5}));   // cached: no second warning
        QCOMPARE(warnings.count(), 1);
        QVERIFY2(warnings.messages().at(0).contains(QStringLiteral("'3'")),
                 qPrintable(warnings.messages().at(0)));
        QVERIFY(warnings.messages().at(0).contains(QStringLiteral("IMU/wx")));
    }
    QCOMPARE(w.engine->runCountForInstance("builtin.conversion.default#IMU/wx"), 0);

    w.state.setAttribute(*w.engine, "SCHEMA_VER", QStringLiteral("abc"));
    {
        WarningCounter warnings;
        QCOMPARE(w.engine->measurement("IMU", "wx"), QVector<double>({62.5}));
        QCOMPARE(warnings.count(), 1);
        QVERIFY(warnings.messages().at(0).contains(QStringLiteral("'abc'")));
    }
    QCOMPARE(w.engine->runCountForInstance("builtin.conversion.default#IMU/wx"), 0);

    {
        WarningCounter warnings;   // the fresh evaluation warns again; not counted above
        QVERIFY(w.engine->verifyAgainstFresh({measKey("IMU", "wx")}).isEmpty());
    }
}

void ConversionEngineTest::unitNormalization()
{
    World w;
    w.state.setMeasurement("IMU", "ax", {1.0}, "g");
    w.state.setMeasurement("MAG", "x", {1.0}, "gauss");
    w.state.setMeasurement("MAG", "z", {-0.5}, "gauss");
    w.state.setMeasurement("IMU", "temperature", {40.0}, "deg C");
    w.state.setMeasurement("X", "c", {7.0}, "furlongs");
    w.state.setMeasurement("BARO", "pressure", {90000.0}, "Pa");

    WarningCounter warnings;

    QVERIFY(w.first("IMU", "ax") == 9.80665);
    QCOMPARE(w.engine->measurementUnit("IMU", "ax"), QStringLiteral("m/s^2"));

    QVERIFY(w.first("MAG", "x") == 0.0001);
    QVERIFY(w.first("MAG", "z") == -0.00005);
    QCOMPARE(w.engine->measurementUnit("MAG", "x"), QStringLiteral("T"));
    QCOMPARE(w.engine->measurementUnit("MAG", "z"), QStringLiteral("T"));

    QVERIFY(w.first("IMU", "temperature") == 40.0);
    QCOMPARE(w.engine->measurementUnit("IMU", "temperature"), QStringLiteral("degC"));

    QVERIFY(w.first("X", "c") == 7.0);
    QCOMPARE(w.engine->measurementUnit("X", "c"), QStringLiteral("furlongs"));

    QVERIFY(w.first("BARO", "pressure") == 90000.0);
    QCOMPARE(w.engine->measurementUnit("BARO", "pressure"), QStringLiteral("Pa"));

    // Unknown unit text is the normal case for custom columns: silent.
    QCOMPARE(warnings.count(), 0);

    // The source is untouched.
    QCOMPARE(w.state.sourceMeasurement("IMU", "ax"), QVector<double>({1.0}));
    QCOMPARE(w.state.sourceUnit("IMU", "ax"), QStringLiteral("g"));
}

void ConversionEngineTest::internalLabelsAreIdentity()
{
    // What released Viewer versions wrote into logbook files.
    World w;
    w.state.setMeasurement("IMU", "ax", {9.80665}, "m/s^2");
    w.state.setMeasurement("MAG", "x", {0.0001}, "T");
    w.state.setMeasurement("IMU", "temperature", {40.0}, "degC");

    struct Expected { const char *sensor; const char *name; double value; const char *unit; };
    const Expected expected[] = {
        {"IMU", "ax", 9.80665, "m/s^2"},
        {"MAG", "x", 0.0001, "T"},
        {"IMU", "temperature", 40.0, "degC"},
    };
    for (const Expected &e : expected) {
        const QVector<double> source = w.state.sourceMeasurement(e.sensor, e.name);
        const QVector<double> effective = w.engine->measurement(e.sensor, e.name);
        QCOMPARE(effective.size(), 1);
        QVERIFY2(effective.at(0) == e.value, e.name);
        QCOMPARE(w.engine->measurementUnit(e.sensor, e.name), QString::fromLatin1(e.unit));
        QVERIFY2(source.constData() == effective.constData(), e.name);
    }
}

void ConversionEngineTest::identitySharesBuffer()
{
    World w;
    QVector<double> altitude(1000);
    for (int i = 0; i < altitude.size(); ++i)
        altitude[i] = 4000.0 - i;
    w.state.setMeasurement("GNSS", "hMSL", altitude, "m");
    w.state.setMeasurement("IMU", "temperature", {40.0, 41.0}, "deg C");   // label-only change
    w.state.setMeasurement("IMU", "ax", {1.0, 2.0}, "g");

    {
        const QVector<double> source = w.state.sourceMeasurement("GNSS", "hMSL");
        const QVector<double> effective = w.engine->measurement("GNSS", "hMSL");
        QCOMPARE(effective.size(), 1000);
        QVERIFY(effective.at(999) == 3001.0);
        QVERIFY(source.constData() == effective.constData());
    }
    {
        const QVector<double> source = w.state.sourceMeasurement("IMU", "temperature");
        const QVector<double> effective = w.engine->measurement("IMU", "temperature");
        QCOMPARE(w.engine->measurementUnit("IMU", "temperature"), QStringLiteral("degC"));
        QVERIFY(source.constData() == effective.constData());
    }
    {
        const QVector<double> source = w.state.sourceMeasurement("IMU", "ax");
        const QVector<double> effective = w.engine->measurement("IMU", "ax");
        QCOMPARE(effective.size(), 2);
        QVERIFY(source.constData() != effective.constData());
        QCOMPARE(source, QVector<double>({1.0, 2.0}));      // and the source is unchanged
    }
}

void ConversionEngineTest::negativeZeroSurvivesIdentity()
{
    // The schema step applies, the unit step is the identity and is skipped:
    // v * 1.0 + 0.0 would turn -0.0 into +0.0.
    World w;
    w.state.setMeasurement("IMU", "wx", {-0.0}, "deg/s");

    const QVector<double> effective = w.engine->measurement("IMU", "wx");
    QCOMPARE(effective.size(), 1);
    QVERIFY(effective.at(0) == 0.0);
    QVERIFY(std::signbit(effective.at(0)));
}

void ConversionEngineTest::orderSchemaThenUnit()
{
    // Hypothetical: a legacy gyro column recorded in "g". 1 x 1.14688 x 9.80665.
    World w;
    w.state.setMeasurement("IMU", "wx", {1.0}, "g");

    QVERIFY(isNear(w.first("IMU", "wx"), 11.247050752));
    QCOMPARE(w.engine->measurementUnit("IMU", "wx"), QStringLiteral("m/s^2"));
}

void ConversionEngineTest::hashInColumnName()
{
    // '#' separates a family id from its instance key, and a source column
    // without a conversion instance would be unreadable.
    World w;
    w.state.setMeasurement("X", "temp#1", {5.0}, "");
    w.state.setMeasurement("X", "50%", {6.0}, "");

    QVERIFY(w.registry.hasSourceConversions());
    QVERIFY(w.first("X", "temp#1") == 5.0);
    QVERIFY(w.first("X", "50%") == 6.0);

    const QList<CalculationInstance> instances = w.registry.sourceConversionsFor("X", "temp#1");
    QCOMPARE(instances.size(), 1);
    QCOMPARE(instances.at(0).instanceId, QStringLiteral("builtin.conversion.default#X/temp%231"));
    QCOMPARE(w.registry.sourceConversionsFor("X", "50%").at(0).instanceId,
             QStringLiteral("builtin.conversion.default#X/50%25"));
}

void ConversionEngineTest::emptySourceUnavailable()
{
    World w;
    w.state.setMeasurement("IMU", "wx", {}, "deg/s");

    QVERIFY(w.engine->measurement("IMU", "wx").isEmpty());
    QCOMPARE(w.engine->measurementUnit("IMU", "wx"), QString());
    QVERIFY(!w.engine->isAvailable(measKey("IMU", "wx")));
    QCOMPARE(w.engine->cachedState(measKey("IMU", "wx")), CalculationEngine::CachedState::Unavailable);
    QCOMPARE(w.engine->totalRunCount(), 0);
}

void ConversionEngineTest::unitChangeInvalidates()
{
    World w;
    w.state.setMeasurement("IMU", "ax", {1.0}, "g");
    QVERIFY(w.first("IMU", "ax") == 9.80665);

    const QSet<DependencyKey> invalidated = w.state.setUnit(*w.engine, "IMU", "ax", "m/s^2");
    QVERIFY(invalidated.contains(measKey("IMU", "ax")));

    QVERIFY(w.first("IMU", "ax") == 1.0);
    QCOMPARE(w.engine->measurementUnit("IMU", "ax"), QStringLiteral("m/s^2"));
    QCOMPARE(w.engine->runCountForInstance("builtin.conversion.default#IMU/ax"), 2);
}

void ConversionEngineTest::derivedNotConverted()
{
    // A derived measurement reads corrected inputs; it is not itself converted.
    World w;
    Calculations::registerImuCalculations(w.registry);
    w.addLegacyGyro();

    const QVector<double> wTotal = w.engine->measurement("IMU", "wTotal");
    QCOMPARE(wTotal.size(), 1);
    QVERIFY(isNear(wTotal.at(0), 160.28135262718));

    QCOMPARE(w.engine->runCount("builtin.imu.wTotal"), 1);
    QCOMPARE(w.engine->runCount(kDefaultFamily), 3);     // wx, wy, wz only
    QCOMPARE(w.engine->runCountForInstance("builtin.conversion.default#IMU/wTotal"), 0);
    QCOMPARE(w.engine->runCount(kSchemaFamily), 0);
}

FLYSIGHT_TEST_MAIN(ConversionEngineTest)

#include "tst_conversion_engine.moc"
