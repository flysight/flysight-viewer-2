// The Python plugin bridge, end to end: this executable boots the real embedded
// interpreter, imports the real flysight_cpp_bridge module from the build tree,
// loads the SDK, the test plugins under tests/python_plugins/ and the bundled
// example, and drives them through real SessionData objects.
//
// There is one interpreter per process and PluginHost initialises once, so
// every plugin file is loaded in initTestCase(). To add a case: add a file
// under tests/python_plugins/ and a test function here - never a second
// initialise(). The registries are global and never cleared: plugin names are
// unique (_PY_* / py*) and no test asserts that a registry is empty.
//
// The environment (PYTHONHOME, PYTHONPATH, PATH) is supplied by CTest; see
// tests/CMakeLists.txt and tests/README.md.

// pybind11 before any Qt header: Qt's `slots` macro breaks the Python headers.
#pragma push_macro("slots")
#undef  slots
#if defined(_MSC_VER)            // mask _DEBUG only while including Python.h
#  pragma push_macro("_DEBUG")
#  undef  _DEBUG
#endif
#include <Python.h>
#if defined(_MSC_VER)
#  pragma pop_macro("_DEBUG")
#endif
#include <pybind11/embed.h>
#include <pybind11/numpy.h>
#pragma pop_macro("slots")

#include <memory>
#include <optional>
#include <string>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QtTest>

#include "calculations/builtincalculations.h"
#include "dataimporter.h"
#include "dependencykey.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "fixturebuilder.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "markerregistry.h"
#include "plotregistry.h"
#include "pluginhost.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionimport.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"

namespace py = pybind11;
using namespace FlySight;
using namespace FlySightTest;

namespace {

bool near(double a, double b) { return qAbs(a - b) <= 1e-9; }

bool allNear(const QVector<double> &values, const QVector<double> &expected)
{
    if (values.size() != expected.size())
        return false;
    for (qsizetype i = 0; i < values.size(); ++i) {
        if (!near(values[i], expected[i]))
            return false;
    }
    return true;
}

using Status = std::optional<ResultStatus>;

// A SENSOR.CSV-like file without SCHEMA_VER (so the legacy gyro correction
// x 1.14688 applies) whose accelerations are recorded in g.
Fs2FileBuilder bridgeSensorFile(const QByteArray &sessionId, bool withRecordedATotal = false)
{
    Fs2FileBuilder b;
    b.var("FIRMWARE_VER", "v2023.09.22").var("SESSION_ID", sessionId).var("DEVICE_ID", "test-device");
    if (!withRecordedATotal) {
        b.sensor("IMU", {"time", "wx", "wy", "wz", "ax", "ay", "az", "temperature"},
                 {"s", "deg/s", "deg/s", "deg/s", "g", "g", "g", "deg C"});
        b.row("IMU", "3,62.5,-125,0,1,0,1,40");
        b.row("IMU", "4,62.5,-125,0,0,0,1,40");
    } else {
        b.sensor("IMU", {"time", "wx", "wy", "wz", "ax", "ay", "az", "temperature", "aTotal"},
                 {"s", "deg/s", "deg/s", "deg/s", "g", "g", "g", "deg C", "m/s^2"});
        b.row("IMU", "3,62.5,-125,0,1,0,1,40,7");
        b.row("IMU", "4,62.5,-125,0,0,0,1,40,7");
    }
    return b;
}

// qWarning capture. Installed by a test after initTestCase, removed in cleanup().
QStringList g_messages;
QtMessageHandler g_previousHandler = nullptr;

void captureMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    g_messages.append(message);
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

int messagesContainingAll(const QStringList &needles)
{
    int count = 0;
    for (const QString &m : std::as_const(g_messages)) {
        bool all = true;
        for (const QString &needle : needles)
            all = all && m.contains(needle);
        if (all)
            ++count;
    }
    return count;
}

} // namespace

class PythonBridgeTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    // boot, single-output plugins, registries
    void bootsRealBridge();
    void singleOutputPluginsReadEffectiveValues();
    void pluginOutputsAreNotEnumerated();
    void plotAndMarkerRegistrationUnaffected();
    void secondInitialiseIsNoOp();

    // plugin registration: rejection and order
    void unknownKindRejectsPluginOnly();
    void registrationOrderIsDeterministic();

    // undeclared reads, no source access, stale views
    void undeclaredReadDiagnostic();
    void swallowedUndeclaredReadStillFails();
    void sourceKindIsUnknownToPlugins();
    void derivedMeasurementReadsEffective();
    void staleViewRaises();

    // failures, return types, precedence
    void exceptionYieldsCleanUnavailable();
    void malformedOutputsAreUnavailable_data();
    void malformedOutputsAreUnavailable();
    void acceptedReturnTypes();
    void returnedArrayIsCopied();
    void pluginBeatsBuiltinStoredBeatsBoth();
    void measurementPluginUnit();

    // multi-output (bundle) plugins
    void multiOutputRunsOnce();
    void partialBundle();
    void effectiveReadAndUnitMatchCpp();
    void bundleExceptionPublishesNothing();
    void malformedBundles_data();
    void malformedBundles();
    void badOutputDeclarationsRejected();

    // the bundled example plugin
    void bundledExampleRuns();

    // a plugin through the application model: import, column cache, save
    void pluginWorkflowThroughModel();

private:
    // Fresh file, fresh importer, fresh session (fresh engine => per-test run counts).
    bool loadFixture(SessionData &session, bool withRecordedATotal = false);
    // The registered calculation id that ends with `suffix` (e.g. "._PY_WX0").
    QString idEndingWith(const QString &suffix) const;
    void startCapture();
    int rejectionsFor(const QString &label, const QString &reasonPart) const;

    QString m_pluginDir;
    int m_fixtureCounter = 0;
    bool m_capturing = false;
};

// ---------------------------------------------------------------- scaffolding

void PythonBridgeTest::initTestCase()
{
    m_pluginDir = TestEnvironment::instance().newTempDir(QStringLiteral("plugins"));

    // A temp directory: PluginHost imports EVERY *.py in the plugin directory,
    // and nothing may be written into the source tree.
    QStringList files{QStringLiteral(FLYSIGHT_PYTHON_PLUGINS_DIR "/flysight_plugin_sdk.py"),
                      QStringLiteral(FLYSIGHT_PYTHON_PLUGINS_DIR "/examples/imu_tilt.py")};
    const QDir testPlugins(QStringLiteral(FLYSIGHT_TEST_PLUGINS_DIR));
    const QStringList testFiles = testPlugins.entryList({QStringLiteral("*.py")}, QDir::Files, QDir::Name);
    QVERIFY(!testFiles.isEmpty());
    for (const QString &name : testFiles)
        files << testPlugins.filePath(name);
    for (const QString &path : std::as_const(files)) {
        QVERIFY2(QFile::copy(path, QDir(m_pluginDir).filePath(QFileInfo(path).fileName())),
                 qPrintable(path));
    }

    // A failure here is a hard failure, not a skip: CMake promised the environment.
    PluginHost::instance().initialise(m_pluginDir);
    QVERIFY2(PluginHost::instance().isInitialised(),
             "the interpreter, flysight_cpp_bridge or the SDK did not load - see the log above "
             "and the environment notes in tests/README.md");
    QVERIFY2(PluginHost::instance().report().failedImports.isEmpty(),
             qPrintable(PluginHost::instance().report().failedImports.join(QLatin1Char(' '))));

    // Plugins before built-ins, exactly like MainWindow.
    TestEnvironment::instance().registerBuiltIns();
}

void PythonBridgeTest::cleanup()
{
    if (m_capturing) {
        qInstallMessageHandler(g_previousHandler);
        g_previousHandler = nullptr;
        m_capturing = false;
    }
    g_messages.clear();
}

void PythonBridgeTest::startCapture()
{
    g_messages.clear();
    g_previousHandler = qInstallMessageHandler(captureMessage);
    m_capturing = true;
}

bool PythonBridgeTest::loadFixture(SessionData &session, bool withRecordedATotal)
{
    const QString dir = TestEnvironment::instance().newTempDir(QStringLiteral("fixture"));
    const QString path = QDir(dir).filePath(QStringLiteral("SENSOR.CSV"));
    const QByteArray id = "bridge-session-" + QByteArray::number(++m_fixtureCounter);
    if (!bridgeSensorFile(id, withRecordedATotal).write(path))
        return false;
    DataImporter importer;
    if (!importer.importFile(path, session)) {
        qWarning() << "import failed:" << importer.getLastError();
        return false;
    }
    return true;
}

QString PythonBridgeTest::idEndingWith(const QString &suffix) const
{
    QString found;
    for (const QString &id : PluginHost::instance().report().registeredIds) {
        if (id.endsWith(suffix)) {
            if (!found.isEmpty())
                return QString();   // ambiguous: the caller's QVERIFY fails
            found = id;
        }
    }
    return found;
}

int PythonBridgeTest::rejectionsFor(const QString &label, const QString &reasonPart) const
{
    int count = 0;
    for (const QString &entry : PluginHost::instance().report().rejected) {
        if (entry.startsWith(label + QStringLiteral(": ")) && entry.contains(reasonPart))
            ++count;
    }
    return count;
}

// ------------------------------ boot, single-output plugins, registries

void PythonBridgeTest::bootsRealBridge()
{
    py::gil_scoped_acquire gil;
    const py::module_ bridge = py::module_::import("flysight_cpp_bridge");

    // The real module from the build tree, not a stub found somewhere else.
    const QString file = QString::fromStdString(bridge.attr("__file__").cast<std::string>());
    QCOMPARE(QFileInfo(file).canonicalPath().toLower(),
             QDir(QStringLiteral(FLYSIGHT_BRIDGE_DIR)).canonicalPath().toLower());

    // Exactly the documented surface: no key type, nothing that can write.
    QSet<QString> exported;
    for (py::handle name : bridge.attr("__dir__")()) {
        const QString n = QString::fromStdString(name.cast<std::string>());
        if (!n.startsWith(QLatin1String("__")))
            exported.insert(n);
    }
    QCOMPARE(exported, QSet<QString>({QStringLiteral("SessionData"),
                                      QStringLiteral("PythonOutputRedirector_CPP"),
                                      QStringLiteral("UndeclaredInputError")}));
}

void PythonBridgeTest::singleOutputPluginsReadEffectiveValues()
{
    SessionData session;
    QVERIFY(loadFixture(session));

    for (int i = 0; i < 3; ++i) {
        QVERIFY(near(session.getAttribute(QStringLiteral("_PY_WX0")).toDouble(), 71.68));
        QVERIFY(allNear(session.getMeasurement(QStringLiteral("IMU"), QStringLiteral("pyWx2")),
                        {143.36, 143.36}));
        QCOMPARE(session.getAttribute(QStringLiteral("_PY_FW")).toString(), QStringLiteral("v2023.09.22"));
    }
    QCOMPARE(session.getAttribute(QStringLiteral("_PY_WX0")).typeId(), int(QMetaType::Double));
    QCOMPARE(session.getAttribute(QStringLiteral("_PY_DURATION")).toDouble(), 1.0);   // README section 2

    // The plugin saw corrected data; the recorded layer is untouched.
    QCOMPARE(session.sourceMeasurement(QStringLiteral("IMU"), QStringLiteral("wx")),
             (QVector<double>{62.5, 62.5}));

    const CalculationEngine &engine = session.calculationEngine();
    for (const char *suffix : {"._PY_WX0", ".IMU/pyWx2", "._PY_FW"}) {
        const QString id = idEndingWith(QLatin1String(suffix));
        QVERIFY2(!id.isEmpty(), suffix);
        QCOMPARE(engine.runCount(id), 1);
        QCOMPARE(engine.resultStatus(id), Status(ResultStatus::Ok));
    }
}

void PythonBridgeTest::pluginOutputsAreNotEnumerated()
{
    SessionData session;
    QVERIFY(loadFixture(session));
    QVERIFY(session.getAttribute(QStringLiteral("_PY_WX0")).isValid());
    QVERIFY(!session.getMeasurement(QStringLiteral("IMU"), QStringLiteral("pyWx2")).isEmpty());

    QVERIFY(!session.hasMeasurement(QStringLiteral("IMU"), QStringLiteral("pyWx2")));
    QVERIFY(!session.hasAttribute(QStringLiteral("_PY_WX0")));
}

void PythonBridgeTest::plotAndMarkerRegistrationUnaffected()
{
    bool plotFound = false;
    for (const PlotValue &plot : PlotRegistry::instance().allPlots()) {
        if (plot.plotName == QLatin1String("Py Wx2")) {
            plotFound = true;
            QCOMPARE(plot.measurementType, QStringLiteral("rotation"));
            QCOMPARE(plot.sensorID, QStringLiteral("IMU"));
            QCOMPARE(plot.measurementID, QStringLiteral("pyWx2"));
        }
    }
    QVERIFY(plotFound);

    bool markerFound = false;
    for (const MarkerDefinition &marker : MarkerRegistry::instance()->allMarkers()) {
        if (marker.attributeKey == QLatin1String("_PY_WX0")) {
            markerFound = true;
            QCOMPARE(marker.measurements.size(), 1);
            QCOMPARE(marker.measurements.first().sensor, QStringLiteral("IMU"));
            QCOMPARE(marker.measurements.first().timeVector, QStringLiteral("_time"));
            QCOMPARE(marker.measurements.first().dataVector, QStringLiteral("wx"));
        }
    }
    QVERIFY(markerFound);
}

void PythonBridgeTest::secondInitialiseIsNoOp()
{
    const QStringList idsBefore = PluginHost::instance().report().registeredIds;
    const QStringList rejectedBefore = PluginHost::instance().report().rejected;
    const QList<CalculationId> registryBefore = CalculationRegistry::instance().registeredIds();
    const qsizetype plotsBefore = PlotRegistry::instance().allPlots().size();

    PluginHost::instance().initialise(TestEnvironment::instance().newTempDir(QStringLiteral("other")));

    QVERIFY(PluginHost::instance().isInitialised());
    QCOMPARE(PluginHost::instance().report().registeredIds, idsBefore);
    QCOMPARE(PluginHost::instance().report().rejected, rejectedBefore);
    QCOMPARE(CalculationRegistry::instance().registeredIds(), registryBefore);
    QCOMPARE(PlotRegistry::instance().allPlots().size(), plotsBefore);
}

// ------------------------------ plugin registration: rejection and order

void PythonBridgeTest::unknownKindRejectsPluginOnly()
{
    struct Bad { const char *cls; const char *output; const char *reason; };
    const Bad bad[] = {
        {"PyBogusKind",    "_PY_BOGUS",   "unknown dependency kind 'bogus'"},
        {"PyPrefKind",     "_PY_PREF",    "preference inputs are not available"},
        {"PyIntKey",       "_PY_INTKEY",  "is not a flysight_plugin_sdk.Key"},
        {"PyNonStrKind",   "_PY_NONSTR",  "unknown dependency kind"},
        {"PyEmptyField",   "_PY_EMPTY",   "empty"},
        {"PyInputsRaises", "_PY_INRAISE", "nope"},
        {"PyNoName",       nullptr,       "name"},
    };
    const CalculationRegistry &registry = CalculationRegistry::instance();
    for (const Bad &b : bad) {
        const QString label = QStringLiteral("t_badkeys.") + QLatin1String(b.cls);
        QVERIFY2(rejectionsFor(label, QLatin1String(b.reason)) == 1, b.cls);
        if (b.output) {
            // Rejected means not registered at all: the report and the registry agree.
            QVERIFY2(!registry.hasCandidateFor(DependencyKey::attribute(QLatin1String(b.output))), b.cls);
            QVERIFY2(idEndingWith(QLatin1Char('.') + QLatin1String(b.output)).isEmpty(), b.cls);
        }
    }

    // Every id the report lists is really registered.
    for (const QString &id : PluginHost::instance().report().registeredIds)
        QVERIFY2(registry.contains(id), qPrintable(id));

    // The neighbours are untouched.
    SessionData session;
    QVERIFY(loadFixture(session));
    QCOMPARE(session.getAttribute(QStringLiteral("_PY_GOOD")).toDouble(), 1.0);
    QVERIFY(registry.hasCandidateFor(DependencyKey::attribute(QStringLiteral("_PY_WX0"))));
    QVERIFY(registry.hasCandidateFor(DependencyKey::measurement(QStringLiteral("IMU"), QStringLiteral("pyWx2"))));
}

void PythonBridgeTest::registrationOrderIsDeterministic()
{
    // All attributes, then all measurements, then all calculations; files in
    // name order (imu_tilt, t_badkeys, t_multi, t_results, t_single, t_view, t_zdocs);
    // the index is the position in the SDK list, so a rejected plugin still
    // consumes one (t_badkeys: attributes 0-7; t_multi: calculations 8-9).
    const QStringList expected{
        QStringLiteral("plugin.attr.8._PY_GOOD"),
        QStringLiteral("plugin.attr.9._PY_RAISE"),
        QStringLiteral("plugin.attr.10._PY_BOOL"),
        QStringLiteral("plugin.attr.11._PY_LIST"),
        QStringLiteral("plugin.attr.12._PY_NAN"),
        QStringLiteral("plugin.attr.13._PY_NONE"),
        QStringLiteral("plugin.attr.14._PY_INT"),
        QStringLiteral("plugin.attr.15._PY_NPF"),
        QStringLiteral("plugin.attr.16._PY_NPI"),
        QStringLiteral("plugin.attr.17._PY_STR"),
        QStringLiteral("plugin.attr.18._PY_WX0"),
        QStringLiteral("plugin.attr.19._PY_FW"),
        QStringLiteral("plugin.attr.20._PY_UNDECL"),
        QStringLiteral("plugin.attr.21._PY_UNDECL_SW"),
        QStringLiteral("plugin.attr.22._PY_VIEW_NO_SOURCE"),
        QStringLiteral("plugin.attr.23._PY_EFF_WTOTAL0"),
        QStringLiteral("plugin.attr.24._PY_STASH"),
        QStringLiteral("plugin.attr.25._PY_DURATION"),
        QStringLiteral("plugin.meas.0.IMU/pyTwoD"),
        QStringLiteral("plugin.meas.1.IMU/pyScalar"),
        QStringLiteral("plugin.meas.2.IMU/pyStrArr"),
        QStringLiteral("plugin.meas.3.IMU/pyBoolArr"),
        QStringLiteral("plugin.meas.4.IMU/pyStrMeas"),
        QStringLiteral("plugin.meas.5.IMU/pyList"),
        QStringLiteral("plugin.meas.6.IMU/pyF32"),
        QStringLiteral("plugin.meas.7.IMU/pyStride"),
        QStringLiteral("plugin.meas.8.IMU/pyKeep"),
        QStringLiteral("plugin.meas.9.IMU/aTotal"),
        QStringLiteral("plugin.meas.10.IMU/pyWx2"),
        QStringLiteral("plugin.calc.0.ImuTilt"),
        QStringLiteral("plugin.calc.1.PyGyroStats"),
        QStringLiteral("plugin.calc.2.PyPartial"),
        QStringLiteral("plugin.calc.3.PyEffectiveProbe"),
        QStringLiteral("plugin.calc.4.PyBundleRaises"),
        QStringLiteral("plugin.calc.5.PyWrongKey"),
        QStringLiteral("plugin.calc.6.PyNotDict"),
        QStringLiteral("plugin.calc.7.PyBadMember"),
    };
    QCOMPARE(PluginHost::instance().report().registeredIds, expected);

    // Plugins precede every built-in in the registry.
    const QList<CalculationId> all = CalculationRegistry::instance().registeredIds();
    QVERIFY(all.size() > expected.size());
    QCOMPARE(QStringList(all.mid(0, expected.size())), expected);
}

// ------------------------------ undeclared reads, no source access, stale views

void PythonBridgeTest::undeclaredReadDiagnostic()
{
    SessionData session;
    QVERIFY(loadFixture(session));
    const QString id = idEndingWith(QStringLiteral("._PY_UNDECL"));
    QVERIFY(!id.isEmpty());

    startCapture();
    QVERIFY(!session.getAttribute(QStringLiteral("_PY_UNDECL")).isValid());

    const CalculationEngine &engine = session.calculationEngine();
    QCOMPARE(engine.resultStatus(id), Status(ResultStatus::UndeclaredRead));
    QCOMPARE(engine.undeclaredReadCount(), 1);
    QCOMPARE(engine.lastUndeclaredRead().first, id);
    QVERIFY(engine.lastUndeclaredRead().second
            == CalcInput::measurement(QStringLiteral("IMU"), QStringLiteral("wy")));

    // Python saw the error at the offending line; the log names plugin, key, and fix.
    const QStringList needles{QStringLiteral("UndeclaredInputError"), QStringLiteral("_PY_UNDECL"),
                              QStringLiteral("IMU/wy"), QStringLiteral("meas('IMU', 'wy')")};
    QCOMPARE(messagesContainingAll(needles), 1);
    QCOMPARE(messagesContainingAll({QStringLiteral("t_view.py")}), 1);     // the traceback

    // Negatively cached: no second run, no second message.
    const qsizetype messagesBefore = g_messages.size();
    QVERIFY(!session.getAttribute(QStringLiteral("_PY_UNDECL")).isValid());
    QCOMPARE(engine.runCount(id), 1);
    QCOMPARE(g_messages.size(), messagesBefore);
}

void PythonBridgeTest::swallowedUndeclaredReadStillFails()
{
    SessionData session;
    QVERIFY(loadFixture(session));
    const QString id = idEndingWith(QStringLiteral("._PY_UNDECL_SW"));
    QVERIFY(!id.isEmpty());

    // The plugin caught the exception and returned 1.0; the evaluation was
    // flagged before the exception was raised, so nothing is published.
    QVERIFY(!session.getAttribute(QStringLiteral("_PY_UNDECL_SW")).isValid());
    QCOMPARE(session.calculationEngine().resultStatus(id), Status(ResultStatus::UndeclaredRead));
    QVERIFY(session.calculationEngine().lastUndeclaredRead().second
            == CalcInput::attribute(QStringLiteral("FIRMWARE_VER")));
}

void PythonBridgeTest::sourceKindIsUnknownToPlugins()
{
    // A key of kind "source" is decoded like any other unrecognised kind: the
    // plugin that declares it is rejected at load, by name, as an input ...
    const CalculationRegistry &registry = CalculationRegistry::instance();
    QCOMPARE(rejectionsFor(QStringLiteral("t_badkeys.PySourceKind"),
                           QStringLiteral("unknown dependency kind 'source'")), 1);
    QVERIFY(!registry.hasCandidateFor(DependencyKey::attribute(QStringLiteral("_PY_SRCKIND"))));
    QVERIFY(idEndingWith(QStringLiteral("._PY_SRCKIND")).isEmpty());
    // ... and as an output.
    QCOMPARE(rejectionsFor(QStringLiteral("t_multi.PySourceOutput"),
                           QStringLiteral("unknown dependency kind 'source'")), 1);

    // The other plugins of the same files still register.
    QVERIFY(!idEndingWith(QStringLiteral("._PY_GOOD")).isEmpty());
    QVERIFY(!idEndingWith(QStringLiteral(".PyGyroStats")).isEmpty());

    // The SDK offers no way to build such a key, and the view no way to read
    // the source layer: checked on the class and on a live view in compute().
    {
        py::gil_scoped_acquire gil;
        const py::module_ sdk = py::module_::import("flysight_plugin_sdk");
        QVERIFY(!py::hasattr(sdk, "source"));
        QVERIFY(!py::hasattr(sdk, "KIND_SOURCE"));
        const py::object view = py::module_::import("flysight_cpp_bridge").attr("SessionData");
        for (const char *name : {"sourceMeasurement", "sourceUnit", "hasSourceMeasurement"})
            QVERIFY2(!py::hasattr(view, name), name);
        for (const char *name : {"getMeasurement", "getAttribute", "effectiveUnit",
                                 "hasMeasurement", "hasAttribute"})
            QVERIFY2(py::hasattr(view, name), name);
    }
    SessionData session;
    QVERIFY(loadFixture(session));
    QCOMPARE(session.getAttribute(QStringLiteral("_PY_VIEW_NO_SOURCE")).toDouble(), 1.0);
}

void PythonBridgeTest::derivedMeasurementReadsEffective()
{
    SessionData session;
    QVERIFY(loadFixture(session));

    // A derived measurement is an ordinary effective read for a plugin ...
    QVERIFY(near(session.getAttribute(QStringLiteral("_PY_EFF_WTOTAL0")).toDouble(), 160.2813526271849));

    // ... and has no source layer at all.
    QVERIFY(!session.hasSourceMeasurement(QStringLiteral("IMU"), QStringLiteral("wTotal")));
    QVERIFY(session.sourceMeasurement(QStringLiteral("IMU"), QStringLiteral("wTotal")).isEmpty());
    QCOMPARE(session.sourceUnit(QStringLiteral("IMU"), QStringLiteral("wTotal")), QString());
}

void PythonBridgeTest::staleViewRaises()
{
    SessionData session;
    QVERIFY(loadFixture(session));
    QCOMPARE(session.getAttribute(QStringLiteral("_PY_STASH")).toDouble(), 1.0);

    py::gil_scoped_acquire gil;
    QVERIFY(py::module_::import("t_view").attr("poke_stale")().cast<bool>());
}

// ------------------------------ failures, return types, precedence

void PythonBridgeTest::exceptionYieldsCleanUnavailable()
{
    SessionData session;
    QVERIFY(loadFixture(session));
    const QString id = idEndingWith(QStringLiteral("._PY_RAISE"));
    QVERIFY(!id.isEmpty());
    const CalculationEngine &engine = session.calculationEngine();

    startCapture();
    QVERIFY(!session.getAttribute(QStringLiteral("_PY_RAISE")).isValid());
    QCOMPARE(engine.resultStatus(id), Status(ResultStatus::Failed));
    QCOMPARE(messagesContainingAll({QStringLiteral("ValueError: boom"), QStringLiteral("t_results.py")}), 1);

    // Negative caching: not called again, nothing logged again.
    const qsizetype messagesBefore = g_messages.size();
    for (int i = 0; i < 3; ++i)
        QVERIFY(!session.getAttribute(QStringLiteral("_PY_RAISE")).isValid());
    QCOMPARE(engine.runCount(id), 1);
    QCOMPARE(g_messages.size(), messagesBefore);

    // Other plugins are unaffected.
    QVERIFY(near(session.getAttribute(QStringLiteral("_PY_WX0")).toDouble(), 71.68));

    // A declared input changes: the plugin is asked again, and now succeeds.
    const QSet<DependencyKey> changed = session.setSourceMeasurement(
        QStringLiteral("IMU"), QStringLiteral("wz"), {1.0, 1.0}, QStringLiteral("deg/s"));
    QVERIFY(changed.contains(DependencyKey::attribute(QStringLiteral("_PY_RAISE"))));
    QCOMPARE(session.getAttribute(QStringLiteral("_PY_RAISE")).toDouble(), 1.0);
    QCOMPARE(engine.runCount(id), 2);
    QCOMPARE(engine.resultStatus(id), Status(ResultStatus::Ok));
    QCOMPARE(engine.scopeDepth(), 0);
}

void PythonBridgeTest::malformedOutputsAreUnavailable_data()
{
    QTest::addColumn<bool>("isAttribute");
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("reason");

    QTest::newRow("2-D array")      << false << "pyTwoD"    << "expected a 1-D array, got 2-D";
    QTest::newRow("scalar")         << false << "pyScalar"  << "expected a 1-D array, got 0-D";
    QTest::newRow("string array")   << false << "pyStrArr"  << "is not numeric";
    QTest::newRow("bool array")     << false << "pyBoolArr" << "dtype bool is not numeric";
    QTest::newRow("str")            << false << "pyStrMeas" << "str is not a valid measurement";
    QTest::newRow("bool attribute") << true  << "_PY_BOOL"  << "bool is not a valid attribute value";
    QTest::newRow("list attribute") << true  << "_PY_LIST"  << "list is not a valid attribute value";
}

void PythonBridgeTest::malformedOutputsAreUnavailable()
{
    QFETCH(bool, isAttribute);
    QFETCH(QString, name);
    QFETCH(QString, reason);

    SessionData session;
    QVERIFY(loadFixture(session));
    const QString id = idEndingWith(isAttribute ? QLatin1Char('.') + name : QStringLiteral(".IMU/") + name);
    QVERIFY(!id.isEmpty());

    startCapture();
    for (int i = 0; i < 3; ++i) {
        if (isAttribute)
            QVERIFY(!session.getAttribute(name).isValid());
        else
            QVERIFY(session.getMeasurement(QStringLiteral("IMU"), name).isEmpty());
    }
    QCOMPARE(session.calculationEngine().resultStatus(id), Status(ResultStatus::Failed));
    QCOMPARE(session.calculationEngine().runCount(id), 1);
    // One adapter warning naming the plugin and the reason.
    QCOMPARE(messagesContainingAll({QStringLiteral("[PluginHost]"), id,
                                    QStringLiteral("returned malformed output"), reason}), 1);
}

void PythonBridgeTest::acceptedReturnTypes()
{
    SessionData session;
    QVERIFY(loadFixture(session));
    const CalculationEngine &engine = session.calculationEngine();

    const auto attribute = [&session](const char *key) { return session.getAttribute(QLatin1String(key)); };
    for (const char *key : {"_PY_INT", "_PY_NPF", "_PY_NPI"})
        QCOMPARE(attribute(key).typeId(), int(QMetaType::Double));
    QCOMPARE(attribute("_PY_INT").toDouble(), 3.0);
    QCOMPARE(attribute("_PY_NPF").toDouble(), 2.5);
    QCOMPARE(attribute("_PY_NPI").toDouble(), 7.0);

    // A string is a string: never parsed as a date.
    QCOMPARE(attribute("_PY_STR").typeId(), int(QMetaType::QString));
    QCOMPARE(attribute("_PY_STR").toString(), QStringLiteral("2024-01-01T00:00:00Z"));

    // None and NaN: no value, and not an error.
    for (const char *key : {"_PY_NAN", "_PY_NONE"}) {
        QVERIFY2(!attribute(key).isValid(), key);
        const QString id = idEndingWith(QLatin1Char('.') + QLatin1String(key));
        QVERIFY2(!id.isEmpty(), key);
        QCOMPARE(engine.resultStatus(id), Status(ResultStatus::Ok));
        QCOMPARE(engine.runCount(id), 1);
    }

    const auto samples = [&session](const char *name) {
        return session.getMeasurement(QStringLiteral("IMU"), QLatin1String(name));
    };
    QCOMPARE(samples("pyList"), (QVector<double>{1.0, 2.0}));
    QCOMPARE(samples("pyF32"), (QVector<double>{1.5, 2.5}));
    QCOMPARE(samples("pyStride"), (QVector<double>{0.0, 2.0}));
}

void PythonBridgeTest::returnedArrayIsCopied()
{
    SessionData session;
    QVERIFY(loadFixture(session));
    const QString id = idEndingWith(QStringLiteral(".IMU/pyKeep"));
    QVERIFY(!id.isEmpty());

    QCOMPARE(session.getMeasurement(QStringLiteral("IMU"), QStringLiteral("pyKeep")),
             (QVector<double>{1.0, 2.0, 3.0}));

    // The plugin overwrites and frees the buffer it returned.
    {
        py::gil_scoped_acquire gil;
        py::module_::import("t_results").attr("poke")();
    }

    QCOMPARE(session.getMeasurement(QStringLiteral("IMU"), QStringLiteral("pyKeep")),
             (QVector<double>{1.0, 2.0, 3.0}));
    QCOMPARE(session.calculationEngine().runCount(id), 1);
}

void PythonBridgeTest::pluginBeatsBuiltinStoredBeatsBoth()
{
    const QString pluginId = idEndingWith(QStringLiteral(".IMU/aTotal"));
    QVERIFY(!pluginId.isEmpty());
    {
        // Registered before the built-ins, the plugin is the first candidate.
        SessionData session;
        QVERIFY(loadFixture(session));
        QCOMPARE(session.getMeasurement(QStringLiteral("IMU"), QStringLiteral("aTotal")),
                 (QVector<double>{42.0, 42.0}));
        QCOMPARE(session.calculationEngine().runCount(pluginId), 1);
        QCOMPARE(session.calculationEngine().runCount(QStringLiteral("builtin.imu.aTotal")), 0);
    }
    {
        // Recorded data beats both.
        SessionData session;
        QVERIFY(loadFixture(session, /*withRecordedATotal=*/true));
        QCOMPARE(session.getMeasurement(QStringLiteral("IMU"), QStringLiteral("aTotal")),
                 (QVector<double>{7.0, 7.0}));
        QCOMPARE(session.calculationEngine().runCount(pluginId), 0);
        QCOMPARE(session.calculationEngine().runCount(QStringLiteral("builtin.imu.aTotal")), 0);
    }
}

void PythonBridgeTest::measurementPluginUnit()
{
    SessionData session;
    QVERIFY(loadFixture(session));
    QCOMPARE(session.effectiveUnit(QStringLiteral("IMU"), QStringLiteral("pyWx2")), QStringLiteral("deg/s"));
}

// ------------------------------ multi-output (bundle) plugins

void PythonBridgeTest::multiOutputRunsOnce()
{
    SessionData session;
    QVERIFY(loadFixture(session));
    const QString id = QStringLiteral("plugin.calc.1.PyGyroStats");
    QVERIFY(CalculationRegistry::instance().contains(id));
    const CalculationEngine &engine = session.calculationEngine();

    const auto norm = [&session] { return session.getMeasurement(QStringLiteral("IMU"), QStringLiteral("pyWNorm")); };
    const auto wMin = [&session] { return session.getAttribute(QStringLiteral("_PY_W_MIN")).toDouble(); };
    const auto wMax = [&session] { return session.getAttribute(QStringLiteral("_PY_W_MAX")).toDouble(); };

    QVERIFY(allNear(norm(), {160.2813526271849, 160.2813526271849}));
    QVERIFY(near(wMin(), -143.36));
    QVERIFY(near(wMax(), 71.68));
    QVERIFY(near(wMax(), 71.68));
    QVERIFY(allNear(norm(), {160.2813526271849, 160.2813526271849}));
    QVERIFY(near(wMin(), -143.36));
    QCOMPARE(session.effectiveUnit(QStringLiteral("IMU"), QStringLiteral("pyWNorm")), QStringLiteral("deg/s"));
    QCOMPARE(engine.runCount(id), 1);

    // A declared input changes: every output that was read is invalidated,
    // and the computation runs exactly once more.
    const QSet<DependencyKey> changed = session.setSourceMeasurement(
        QStringLiteral("IMU"), QStringLiteral("wy"), {0.0, 0.0}, QStringLiteral("deg/s"));
    QVERIFY(changed.contains(DependencyKey::attribute(QStringLiteral("_PY_W_MAX"))));
    QVERIFY(changed.contains(DependencyKey::attribute(QStringLiteral("_PY_W_MIN"))));
    QVERIFY(changed.contains(DependencyKey::measurement(QStringLiteral("IMU"), QStringLiteral("pyWNorm"))));
    QCOMPARE(wMin(), 0.0);
    QVERIFY(near(wMax(), 71.68));
    QVERIFY(allNear(norm(), {71.68, 71.68}));
    QCOMPARE(engine.runCount(id), 2);

    // An unrelated change runs nothing.
    session.setAttribute(QStringLiteral("_DESCRIPTION"), QStringLiteral("x"));
    QCOMPARE(wMin(), 0.0);
    QVERIFY(allNear(norm(), {71.68, 71.68}));
    QCOMPARE(engine.runCount(id), 2);
}

void PythonBridgeTest::partialBundle()
{
    SessionData session;
    QVERIFY(loadFixture(session));
    const QString id = QStringLiteral("plugin.calc.2.PyPartial");

    QCOMPARE(session.getAttribute(QStringLiteral("_PY_PART_A")).toDouble(), 1.0);
    QVERIFY(!session.getAttribute(QStringLiteral("_PY_PART_B")).isValid());     // None
    QVERIFY(!session.getAttribute(QStringLiteral("_PY_PART_C")).isValid());     // no entry
    QCOMPARE(session.calculationEngine().resultStatus(id), Status(ResultStatus::Ok));
    QCOMPARE(session.calculationEngine().runCount(id), 1);
}

void PythonBridgeTest::effectiveReadAndUnitMatchCpp()
{
    SessionData session;
    QVERIFY(loadFixture(session));
    const QString imu = QStringLiteral("IMU");
    const QString ax = QStringLiteral("ax");
    const QString id = QStringLiteral("plugin.calc.3.PyEffectiveProbe");

    // Recorded as 1 g; the plugin sees what every other consumer sees.
    QCOMPARE(session.sourceMeasurement(imu, ax), (QVector<double>{1.0, 0.0}));
    QCOMPARE(session.sourceUnit(imu, ax), QStringLiteral("g"));
    QCOMPARE(session.effectiveUnit(imu, ax), QStringLiteral("m/s^2"));
    QCOMPARE(session.getAttribute(QStringLiteral("_PY_EFF_UNIT")).toString(), session.effectiveUnit(imu, ax));
    QVERIFY(near(session.getAttribute(QStringLiteral("_PY_EFF_AX0")).toDouble(), 9.80665));
    QCOMPARE(session.calculationEngine().runCount(id), 1);

    // The recorded samples are still a tracked dependency, through the conversion.
    const QSet<DependencyKey> changed = session.setSourceMeasurement(imu, ax, {2.0, 0.0}, QStringLiteral("g"));
    QVERIFY(changed.contains(DependencyKey::attribute(QStringLiteral("_PY_EFF_AX0"))));
    QVERIFY(near(session.getAttribute(QStringLiteral("_PY_EFF_AX0")).toDouble(), 2 * 9.80665));
    QCOMPARE(session.calculationEngine().runCount(id), 2);
}

void PythonBridgeTest::bundleExceptionPublishesNothing()
{
    SessionData session;
    QVERIFY(loadFixture(session));
    const QString id = QStringLiteral("plugin.calc.4.PyBundleRaises");
    const CalculationEngine &engine = session.calculationEngine();

    // A was already in the dict when compute() raised: still nothing is published.
    for (int i = 0; i < 2; ++i) {
        QVERIFY(!session.getAttribute(QStringLiteral("_PY_BR_A")).isValid());
        QVERIFY(!session.getAttribute(QStringLiteral("_PY_BR_B")).isValid());
    }
    QCOMPARE(engine.resultStatus(id), Status(ResultStatus::Failed));
    QCOMPARE(engine.runCount(id), 1);

    session.setSourceMeasurement(QStringLiteral("IMU"), QStringLiteral("wz"), {1.0, 1.0}, QStringLiteral("deg/s"));
    QCOMPARE(session.getAttribute(QStringLiteral("_PY_BR_A")).toDouble(), 1.0);
    QCOMPARE(session.getAttribute(QStringLiteral("_PY_BR_B")).toDouble(), 2.0);
    QCOMPARE(engine.runCount(id), 2);
}

void PythonBridgeTest::malformedBundles_data()
{
    QTest::addColumn<QString>("id");
    QTest::addColumn<QStringList>("attributes");
    QTest::addColumn<QString>("measurement");
    QTest::addColumn<QString>("reason");

    QTest::newRow("undeclared key") << "plugin.calc.5.PyWrongKey" << QStringList{"_PY_WK_A"} << QString()
                                    << "attribute _PY_WK_OTHER, which is not in outputs()";
    QTest::newRow("not a dict")     << "plugin.calc.6.PyNotDict" << QStringList{"_PY_ND_A"} << QString()
                                    << "expected a dict";
    QTest::newRow("bad member")     << "plugin.calc.7.PyBadMember" << QStringList{"_PY_BM_A"} << "pyBmM"
                                    << "measurement IMU/pyBmM: expected a 1-D array, got 2-D";
}

void PythonBridgeTest::malformedBundles()
{
    QFETCH(QString, id);
    QFETCH(QStringList, attributes);
    QFETCH(QString, measurement);
    QFETCH(QString, reason);

    SessionData session;
    QVERIFY(loadFixture(session));

    startCapture();
    for (int i = 0; i < 2; ++i) {
        // EVERY declared output, including the ones whose own value was fine
        for (const QString &key : std::as_const(attributes))
            QVERIFY2(!session.getAttribute(key).isValid(), qPrintable(key));
        if (!measurement.isEmpty())
            QVERIFY(session.getMeasurement(QStringLiteral("IMU"), measurement).isEmpty());
    }
    QCOMPARE(session.calculationEngine().resultStatus(id), Status(ResultStatus::Failed));
    QCOMPARE(session.calculationEngine().runCount(id), 1);
    QCOMPARE(messagesContainingAll({QStringLiteral("[PluginHost]"), id,
                                    QStringLiteral("returned malformed output"), reason}), 1);
}

void PythonBridgeTest::badOutputDeclarationsRejected()
{
    QCOMPARE(rejectionsFor(QStringLiteral("t_multi.PySourceOutput"), QStringLiteral("unknown dependency kind 'source'")), 1);
    QCOMPARE(rejectionsFor(QStringLiteral("t_multi.PyNoOutputs"), QStringLiteral("at least one")), 1);
    QVERIFY(idEndingWith(QStringLiteral(".PySourceOutput")).isEmpty());
    QVERIFY(idEndingWith(QStringLiteral(".PyNoOutputs")).isEmpty());
}

// ------------------------------ the bundled example plugin

void PythonBridgeTest::bundledExampleRuns()
{
    // The shipped file, python_plugins/examples/imu_tilt.py, not a copy under tests/.
    SessionData session;
    QVERIFY(loadFixture(session));
    const QString id = QStringLiteral("plugin.calc.0.ImuTilt");
    const QString imu = QStringLiteral("IMU");

    // Effective ax = {9.80665, 0}, ay = {0, 0}, az = {9.80665, 9.80665}
    for (int i = 0; i < 2; ++i) {
        QVERIFY(allNear(session.getMeasurement(imu, QStringLiteral("tiltPitch")), {-45.0, 0.0}));
        QVERIFY(allNear(session.getMeasurement(imu, QStringLiteral("tiltRoll")), {0.0, 0.0}));
        QVERIFY(near(session.getAttribute(QStringLiteral("_IMU_PEAK_ACCEL")).toDouble(), 13.868697431446114));
    }
    QCOMPARE(session.effectiveUnit(imu, QStringLiteral("tiltPitch")), QStringLiteral("deg"));
    QCOMPARE(session.effectiveUnit(imu, QStringLiteral("tiltRoll")), QStringLiteral("deg"));
    QCOMPARE(session.calculationEngine().runCount(id), 1);

    QSet<QString> plots;
    for (const PlotValue &plot : PlotRegistry::instance().allPlots()) {
        if (plot.category == QLatin1String("IMU (examples)"))
            plots.insert(plot.plotName);
    }
    QCOMPARE(plots, QSet<QString>({QStringLiteral("Tilt pitch"), QStringLiteral("Tilt roll")}));
}

// ------------------------------ a plugin through the application model

// Acceptance 17 / 19: the plugin workflow end to end - a file imported through
// the application's import path, a logbook column fed by a multi-output Python
// plugin, a save, and a restart. Plugin outputs are cached for the logbook but
// never persisted in the session file.
void PythonBridgeTest::pluginWorkflowThroughModel()
{
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumn pluginColumn;
    pluginColumn.type = ColumnType::SessionAttribute;
    pluginColumn.attributeKey = QStringLiteral("_PY_W_MAX");    // t_multi.py, PyGyroStats
    LogbookColumnStore::instance().setColumns({pluginColumn});

    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    logbook.initialize();       // plugins and built-ins are registered already

    const QString folder = env.newTempDir(QStringLiteral("card")) + QStringLiteral("/24-01-01/12-00-00");
    QVERIFY(QDir().mkpath(folder));
    const QString path = QDir(folder).filePath(QStringLiteral("SENSOR.CSV"));
    QVERIFY(bridgeSensorFile("bridge-workflow").write(path));

    auto model = std::make_unique<SessionModel>();
    const SessionImport::BatchResult batch = SessionImport::importFiles(*model, {path});
    QCOMPARE(batch.files.size(), 1);
    QVERIFY(batch.files.first().outcome == MergeResult::Outcome::Created);
    QVERIFY(waitForIdle(*model));

    // Live, and cached for the logbook
    QCOMPARE(model->rowCount(), 1);
    QVERIFY(near(model->sessionRef(0).getAttribute(QStringLiteral("_PY_W_MAX")).toDouble(), 71.68));
    QVERIFY(near(model->rowAt(0).cachedValues.value(0).toDouble(), 71.68));
    const QString fingerprint = calculationEnvironmentFingerprint();

    // Plugin outputs are never persisted
    const QStringList csvFiles = QDir(env.sessionsDir()).entryList({QStringLiteral("*.csv")}, QDir::Files);
    QCOMPARE(csvFiles.size(), 1);
    const QByteArray csv = readFileBytes(QDir(env.sessionsDir()).filePath(csvFiles.first()));
    QVERIFY(csv.contains("$IMU,3,62.5,-125,0,1,0,1,40\n"));
    QVERIFY(!csv.contains("_PY_"));
    QVERIFY(!csv.contains("pyWNorm"));

    // Restart: the same plugin set, so the cached value is served from index.json
    model.reset();
    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(!logbook.cachedValuesDiscardedOnLoad());
    QCOMPARE(calculationEnvironmentFingerprint(), fingerprint);

    model = std::make_unique<SessionModel>();
    model->populateFromIndex(logbook.cachedColumnValues(LogbookColumnStore::instance().enabledColumns()),
                             logbook.lastAccessedMap());
    model->startColumnWorker();
    QVERIFY(waitForIdle(*model));
    QCOMPARE(model->rowCount(), 1);
    QVERIFY(!model->rowAt(0).isLoaded());
    QVERIFY(near(model->rowAt(0).cachedValues.value(0).toDouble(), 71.68));
    QCOMPARE(model->columnWorkStats().sessionsLoaded, 0);

    model.reset();
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

FLYSIGHT_TEST_MAIN(PythonBridgeTest)
#include "tst_python_bridge.moc"
