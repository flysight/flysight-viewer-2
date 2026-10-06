// Sensor fusion as a registered calculation, on real SessionData engines bound
// to the global registry, with the real fit on the test's main thread (64 MiB
// stack through flysight_add_fusion_test).
//
// Sensor-fusion-jobs acceptance 5, 6, 7, 8 (second half), 9, 10 (adapter
// half), 11 and 12 at session level. The queue's half is tst_fusion_jobs.
//
// Expected values are the committed goldens of the kernel (tests/data/fusion/,
// compared in the mode chosen by FLYSIGHT_FUSION_EXACT) and literals; never a
// second call of the code under test, except where "synchronous equals
// asynchronous" is the rule being tested.
//
// The fit is never run on asyncdriver.h's thread modes: those threads have
// default stacks. ComputeMode::Inline runs it here.

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "asyncdriver.h"
#include "dataexporter.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "engine/storedcalculationresult.h"
#include "fusion/fusionregistration.h"
#include "fusionfixtures.h"
#include "fusiongolden.h"
#include "fusionsessions.h"
#include "sessiondata.h"
#include "storedresults.h"
#include "testenvironment.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;

using BlockerState = BlockerReport::State;
using ReadyState = CalculationReadiness::State;
using PrepareKind = CalculationEngine::PrepareOutcome::Kind;
using Restore = CalculationEngine::RestoreOutcome;
using ExplicitEvent = CalculationEngine::ExplicitResultEvent;

namespace {

const QString kFit = QStringLiteral("builtin.fusion.fit");
const QString kVelH = QStringLiteral("builtin.fusion.velH");
const QString kVel = QStringLiteral("builtin.fusion.vel");
const QString kAccH = QStringLiteral("builtin.fusion.accH");
const QString kHAcc = QStringLiteral("builtin.fusion.hAcc");
const QString kVAcc = QStringLiteral("builtin.fusion.vAcc");
const QString kSAcc = QStringLiteral("builtin.fusion.sAcc");
const QString kSystemTime = QStringLiteral("builtin.fusion.systemTime");
const QString kDiagnostics = QStringLiteral("_FUSION_DIAGNOSTICS");

QVector<double> fusion(const SessionData &session, const QString &name)
{
    return session.getMeasurement(QStringLiteral("Fusion"), name);
}

bool isAvailable(const SessionData &session, const DependencyKey &name)
{
    if (name.type == DependencyKey::Type::Attribute)
        return session.getAttribute(name.attributeKey).isValid();
    return !session.getMeasurement(name.measurementKey.first, name.measurementKey.second).isEmpty();
}

/// The names of `names` that are available, as text; empty when none is.
QString availableAmong(const SessionData &session, const QList<DependencyKey> &names)
{
    QStringList available;
    for (const DependencyKey &name : names) {
        if (isAvailable(session, name))
            available.append(name.type == DependencyKey::Type::Attribute
                                 ? name.attributeKey
                                 : name.measurementKey.first + QLatin1Char('/') + name.measurementKey.second);
    }
    return available.join(QStringLiteral(", "));
}

/// The 33 measurements, velH, vel, accH, the fused accuracies hAcc, vAcc and
/// sAcc, _system_time and the roll at exit: everything but the diagnostics.
QList<DependencyKey> valueNames()
{
    QList<DependencyKey> names = fusionNames();
    names.removeAll(DependencyKey::attribute(kDiagnostics));
    return names;
}

/// The measurement behind each of the eighteen "Sensor fusion" plots
/// (fusionPlots()): the fit's own velD, accD and four accuracies, and the
/// derivations that wait on it, the horizontal, vertical and speed
/// accuracies derived from its covariance blocks among them.
QList<DependencyKey> plotNames()
{
    QList<DependencyKey> names;
    for (const PlotValue &plot : fusionPlots())
        names.append(fusionKey(plot.measurementID));
    return names;
}

QJsonObject diagnosticsOf(const SessionData &session)
{
    return QJsonDocument::fromJson(session.getAttribute(kDiagnostics).toString().toUtf8()).object();
}

/// Empty when the published diagnostics match the golden's.
QString diagnosticsDifference(const SessionData &session, const FusionGolden &golden)
{
    return compareJson(QStringLiteral("diagnostics"), diagnosticsOf(session), golden.diagnostics);
}

QString failureOf(const FusionGolden &golden)
{
    return golden.diagnostics.value(QStringLiteral("failure")).toString();
}

/// A copy of `session`'s stored state without one sensor.
SessionData withoutSensor(const SessionData &session, const QString &sensor)
{
    SessionData copy;
    for (const QString &key : session.attributeKeys())
        copy.setAttribute(key, session.storedAttribute(key));
    SourceData source = session.sourceData();
    source.remove(sensor);
    copy.mergeSourceData(source);
    return copy;
}

/// Everything a blocker report says: state, blocker instance ids, and each
/// note's instance id, status and detail.
QString reportText(const BlockerReport &report)
{
    QStringList text;
    text.append(QString::number(int(report.state)));
    for (const CalculationBlocker &blocker : report.blockers)
        text.append(QStringLiteral("blocker ") + blocker.instanceId);
    for (const UnproducedNote &note : report.notProduced)
        text.append(QStringLiteral("note %1 %2 %3")
                        .arg(note.calculation.instanceId, QString::number(int(note.status)), note.detail));
    return text.join(QStringLiteral("; "));
}

/// "Installed 0" (the status as its number), "Dropped 0"
QStringList eventTexts(const QList<ExplicitEvent> &events)
{
    QStringList text;
    for (const ExplicitEvent &event : events) {
        text.append((event.kind == ExplicitEvent::Kind::Installed ? QStringLiteral("Installed ")
                                                                  : QStringLiteral("Dropped "))
                    + event.instanceId + QLatin1Char(' ') + QString::number(int(event.status)));
    }
    return text;
}

/// Cancels from its n-th isCancelled() call on, and records the texts.
class CancelAtCall : public CalculationProgress {
public:
    explicit CancelAtCall(int call) : m_cancelAt(call) {}
    void report(const QString &text) override { m_texts.append(text); }
    bool isCancelled() const override { return ++m_calls >= m_cancelAt; }
    QStringList texts() const { return m_texts; }

private:
    int m_cancelAt;
    mutable int m_calls = 0;
    QStringList m_texts;
};

} // namespace

class FusionSessionTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void registrationShape();
    void explicitOutputsHaveOneCandidate();
    void inputsAreBitIdenticalToFixture();
    void configurationDefaults();
    void configurationReachesTheKernel();
    void readsNeverRunTheFit();
    void requestRunsOnceAndPublishesTogether_data();
    void requestRunsOnceAndPublishesTogether();
    void asyncMatchesSync_data();
    void asyncMatchesSync();
    void changeAfterPublicationDropsEverything();
    void rejectionIsACachedResult_data();
    void rejectionIsACachedResult();
    void cancelStopsAtNextBoundary_data();
    void cancelStopsAtNextBoundary();
    void missingInputsAreNotApplicable_data();
    void missingInputsAreNotApplicable();
    void temperatureReachesTheKernel();
    void blockersReportFusion();
    void naturalSessionEndToEnd();
    void twoSessionsAreIndependent();
    void restoredFitIsIndistinguishable_data();
    void restoredFitIsIndistinguishable();

private:
    QStringList m_registryBefore;
};

void FusionSessionTest::initTestCase()
{
    // As the application does: the built-ins, then sensor fusion
    TestEnvironment::instance().registerBuiltIns();
    registerFusionOnce();
}

void FusionSessionTest::init()
{
    TestEnvironment::instance().resetPreferencesToDefaults();
    m_registryBefore = CalculationRegistry::instance().registeredIds();
}

void FusionSessionTest::cleanup()
{
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

void FusionSessionTest::registrationShape()
{
    const CalculationRegistry &registry = CalculationRegistry::instance();

    // Seventeen registrations after every built-in, in this order: the
    // configuration's constant defaults first (configurationDefaults), then
    // the fit; the derived values' descriptors (the fused accuracies among
    // them), the orientation's constant default and the attitude are
    // tst_fusion_derived's
    QCOMPARE(registry.registeredIds().mid(registry.registeredIds().size() - 17),
             QStringList({QStringLiteral("builtin.default.ACCEL_FS_G"),
                          QStringLiteral("builtin.default.GYRO_FS_DEG_S"),
                          QStringLiteral("builtin.default.ACCEL_ODR_HZ"),
                          QStringLiteral("builtin.default.GYRO_ODR_HZ"),
                          kFit, kVelH, kVel, kAccH, kHAcc, kVAcc, kSAcc, kSystemTime,
                          QStringLiteral("builtin.fusion.z"),
                          QStringLiteral("builtin.fusion.accAlongTrack"),
                          QStringLiteral("builtin.fusion.accCrossTrack"),
                          QStringLiteral("builtin.default._ORIENTATION"),
                          QStringLiteral("builtin.fusion.attitude")}));
    QCOMPARE(QString::fromLatin1(Fusion::FitCalculationId), kFit);
    QCOMPARE(registry.title(kFit), QStringLiteral("Sensor fusion"));

    const std::optional<CalculationInstance> fit = registry.instance(kFit);
    QVERIFY(fit.has_value());
    QVERIFY(fit->descriptor->policy == EvaluationPolicy::Explicit);
    QCOMPARE(fit->descriptor->title, QStringLiteral("Sensor fusion"));

    const QList<CalcInput> inputs{
        CalcInput::measurement("GNSS", "_time"),
        CalcInput::measurement("Local", "north"), CalcInput::measurement("Local", "east"),
        CalcInput::measurement("Local", "down"), CalcInput::measurement("Local", "velN"),
        CalcInput::measurement("Local", "velE"), CalcInput::measurement("Local", "velD"),
        CalcInput::measurement("GNSS", "hAcc"), CalcInput::measurement("GNSS", "vAcc"),
        CalcInput::measurement("GNSS", "sAcc"),
        CalcInput::measurement("IMU", "_time"),
        CalcInput::measurement("IMU", "ax"), CalcInput::measurement("IMU", "ay"),
        CalcInput::measurement("IMU", "az"), CalcInput::measurement("IMU", "wx"),
        CalcInput::measurement("IMU", "wy"), CalcInput::measurement("IMU", "wz"),
        CalcInput::measurement("IMU", "temperature"),
        CalcInput::attribute("_LOCAL_ORIGIN_INDEX"), CalcInput::attribute("_LOCAL_ORIGIN_LAT"),
        CalcInput::attribute("_LOCAL_ORIGIN_LON"), CalcInput::attribute("_LOCAL_ORIGIN_HMSL"),
        CalcInput::attribute("ACCEL_FS_G"), CalcInput::attribute("GYRO_FS_DEG_S"),
        CalcInput::attribute("ACCEL_ODR_HZ"), CalcInput::attribute("GYRO_ODR_HZ")};
    QCOMPARE(inputs.size(), 26);
    QVERIFY(fit->descriptor->inputs == inputs);

    // Thirty-four outputs: the seventeen measurements of the state, the four
    // accuracies (clause 33 of the specification of 1001-1065), the twelve
    // entries of the position and velocity covariance blocks (item 1701), and
    // the diagnostics, as literals.
    QList<DependencyKey> outputs;
    for (const char *name : {"_time", "north", "east", "down", "velN", "velE", "velD", "accN", "accE", "accD",
                             "roll", "pitch", "yaw", "qx", "qy", "qz", "qw",
                             "headingAcc", "tiltAcc", "accHAcc", "accDAcc",
                             "posCovNN", "posCovNE", "posCovND", "posCovEE", "posCovED", "posCovDD",
                             "velCovNN", "velCovNE", "velCovND", "velCovEE", "velCovED", "velCovDD"})
        outputs.append(fusionKey(QString::fromLatin1(name)));
    outputs.append(DependencyKey::attribute(kDiagnostics));
    QCOMPARE(outputs.size(), 34);
    QVERIFY(fit->descriptor->outputs == outputs);
    QStringList measurements;
    for (const Fusion::FitOutputChannel &channel : Fusion::fitOutputChannels(Fusion::Result()))
        measurements.append(channel.name);
    QCOMPARE(measurements, fusionMeasurementNames());

    // The goldens' columns are these names
    QCOMPARE(fusionChannelNames(), fusionMeasurementNames());

    const std::optional<CalculationInstance> accH = registry.instance(kAccH);
    QVERIFY(accH.has_value());
    QVERIFY(accH->descriptor->policy == EvaluationPolicy::OnDemand);
    QVERIFY(accH->descriptor->inputs
            == QList<CalcInput>({CalcInput::measurement("Fusion", "accN"), CalcInput::measurement("Fusion", "accE")}));
    QVERIFY(accH->descriptor->outputs == QList<DependencyKey>({fusionKey("accH")}));

    const std::optional<CalculationInstance> systemTime = registry.instance(kSystemTime);
    QVERIFY(systemTime.has_value());
    QVERIFY(systemTime->descriptor->policy == EvaluationPolicy::OnDemand);
    QVERIFY(systemTime->descriptor->inputs
            == QList<CalcInput>({CalcInput::measurement("Fusion", "_time"), CalcInput::attribute("_TIME_FIT_A"),
                                 CalcInput::attribute("_TIME_FIT_B")}));
    QVERIFY(systemTime->descriptor->outputs == QList<DependencyKey>({fusionKey("_system_time")}));

    // Only the fit declares a result version: its kernel's algorithm string,
    // v10 since the fit publishes the covariance blocks (item 1702)
    QCOMPARE(fit->descriptor->resultVersion, QStringLiteral("batch-temperature-bias-v10"));
    QVERIFY(accH->descriptor->resultVersion.isEmpty());
    QVERIFY(systemTime->descriptor->resultVersion.isEmpty());

    // One candidate per name: nothing else produces a fusion value
    const QList<CalculationInstance> accHCandidates = registry.candidatesFor(fusionKey("accH"));
    QCOMPARE(accHCandidates.size(), 1);
    QCOMPARE(accHCandidates.first().instanceId, kAccH);
    QCOMPARE(registry.candidatesFor(fusionKey("roll")).size(), 1);
    QCOMPARE(registry.candidatesFor(DependencyKey::attribute(kDiagnostics)).size(), 1);
}

// docs/CALCULATIONS.md section 8: no output of an Explicit calculation has
// another candidate, so while the calculation is not requested its outputs
// read as unavailable. SessionModel::settleExplicitColumns relies on it ("no
// record, so unavailable"); nothing enforces it at registration. Every
// registered plain calculation is checked (family instances are not
// enumerable; no built-in family is explicit).
void FusionSessionTest::explicitOutputsHaveOneCandidate()
{
    const CalculationRegistry &registry = CalculationRegistry::instance();
    QStringList explicitIds;
    for (const QString &id : registry.registeredIds()) {
        if (registry.isFamily(id))
            continue;
        const std::optional<CalculationInstance> instance = registry.instance(id);
        QVERIFY2(instance.has_value(), qPrintable(id));
        if (instance->descriptor->policy != EvaluationPolicy::Explicit)
            continue;
        explicitIds.append(id);
        for (const DependencyKey &output : instance->descriptor->outputs) {
            const QString name = output.type == DependencyKey::Type::Attribute
                ? output.attributeKey
                : output.measurementKey.first + QLatin1Char('/') + output.measurementKey.second;
            const QList<CalculationInstance> candidates = registry.candidatesFor(output);
            QVERIFY2(candidates.size() == 1, qPrintable(id + QStringLiteral(": ") + name));
            QCOMPARE(candidates.first().instanceId, id);
        }
    }
    QCOMPARE(explicitIds, QStringList({kFit}));     // the fit is the only explicit built-in today
}

// The premise of every golden comparison below: the eighteen measurements and
// the four origin attributes are the fixture's bit for bit, and the four
// configuration attributes are the fixture's stated configuration, stored as
// the importer stores a header attribute: the recorded text (item 1045).
void FusionSessionTest::inputsAreBitIdenticalToFixture()
{
    const FusionFixture f = fusionFixture(QStringLiteral("coarse_maneuver"));
    const SessionData session = sessionFromFixture(f, QStringLiteral("f1"));

    const struct { const char *sensor; const char *name; const QVector<double> *expected; } inputs[] = {
        {"GNSS", "_time", &f.gnssTime},
        {"Local", "north", &f.north}, {"Local", "east", &f.east}, {"Local", "down", &f.down},
        {"Local", "velN", &f.velN}, {"Local", "velE", &f.velE}, {"Local", "velD", &f.velD},
        {"GNSS", "hAcc", &f.hAcc}, {"GNSS", "vAcc", &f.vAcc}, {"GNSS", "sAcc", &f.sAcc},
        {"IMU", "_time", &f.imuTime},
        {"IMU", "ax", &f.ax}, {"IMU", "ay", &f.ay}, {"IMU", "az", &f.az},
        {"IMU", "wx", &f.wx}, {"IMU", "wy", &f.wy}, {"IMU", "wz", &f.wz},
        {"IMU", "temperature", &f.imuTemperature}};
    QCOMPARE(int(std::size(inputs)), 18);   // the 26 declared inputs less the eight attributes
    for (const auto &input : inputs) {
        QVERIFY2(sameBitsEverywhere(session.getMeasurement(input.sensor, input.name), *input.expected),
                 input.name);
    }

    QCOMPARE(session.getAttribute("_LOCAL_ORIGIN_INDEX"), QVariant::fromValue(qlonglong(3)));
    QCOMPARE(session.getAttribute("_LOCAL_ORIGIN_LAT"), QVariant(45.0));
    QCOMPARE(session.getAttribute("_LOCAL_ORIGIN_LON"), QVariant(-75.0));
    QCOMPARE(session.getAttribute("_LOCAL_ORIGIN_HMSL"), QVariant(100.0));

    // The fixture states its configuration: +/-16 g, +/-2000 deg/s, 104 Hz
    QCOMPARE(f.accelOdrHz, 104.);
    const struct { const char *key; const char *text; } stated[] = {
        {"ACCEL_FS_G", "16"}, {"GYRO_FS_DEG_S", "2000"}, {"ACCEL_ODR_HZ", "104"}, {"GYRO_ODR_HZ", "104"}};
    for (const auto &entry : stated) {
        QVERIFY2(session.hasStoredAttribute(QString::fromLatin1(entry.key)), entry.key);
        QCOMPARE(session.getAttribute(entry.key), QVariant(QString::fromLatin1(entry.text)));
    }
    QCOMPARE(session.calculationEngine().runCount(kFit), 0);
}

// Items 1004, 1050: the four configuration attributes the fit reads have
// constant defaults, firmware v2023.09.22's, as text: calculations with no
// inputs and the attribute as their one output, which a session without the
// keys reads and a stored value overrides. The other five keys have none.
void FusionSessionTest::configurationDefaults()
{
    const CalculationRegistry &registry = CalculationRegistry::instance();
    const struct { const char *key; const char *value; } defaults[] = {
        {"ACCEL_FS_G", "16"}, {"GYRO_FS_DEG_S", "2000"}, {"ACCEL_ODR_HZ", "12.5"}, {"GYRO_ODR_HZ", "12.5"}};
    for (const auto &entry : defaults) {
        const QString key = QString::fromLatin1(entry.key);
        const std::optional<CalculationInstance> instance =
            registry.instance(QStringLiteral("builtin.default.") + key);
        QVERIFY2(instance.has_value(), entry.key);
        QVERIFY(instance->descriptor->inputs.isEmpty());
        QVERIFY(instance->descriptor->outputs == QList<DependencyKey>({DependencyKey::attribute(key)}));
        QVERIFY(instance->descriptor->policy == EvaluationPolicy::OnDemand);
        QVERIFY(instance->descriptor->resultVersion.isEmpty());
        const QList<CalculationInstance> candidates = registry.candidatesFor(DependencyKey::attribute(key));
        QCOMPARE(candidates.size(), 1);
        QCOMPARE(candidates.first().instanceId, QStringLiteral("builtin.default.") + key);
    }
    for (const char *key : {"BARO_ODR_HZ", "HUM_ODR_HZ", "MAG_ODR_HZ", "GNSS_MODEL", "GNSS_RATE_HZ"})
        QVERIFY2(registry.candidatesFor(DependencyKey::attribute(QString::fromLatin1(key))).isEmpty(), key);

    // A fixture session states its configuration; without the keys it reads
    // the defaults
    SessionData session = fixtureSession(QStringLiteral("coarse_linear"));
    for (const auto &entry : defaults)
        session.removeAttribute(QString::fromLatin1(entry.key));
    for (const auto &entry : defaults) {
        const QString key = QString::fromLatin1(entry.key);
        QVERIFY(!session.hasStoredAttribute(key));
        QCOMPARE(session.getAttribute(key).userType(), int(QMetaType::QString));
        QCOMPARE(session.getAttribute(key).toString(), QString::fromLatin1(entry.value));
    }
    for (const char *key : {"BARO_ODR_HZ", "HUM_ODR_HZ", "MAG_ODR_HZ", "GNSS_MODEL", "GNSS_RATE_HZ"})
        QVERIFY2(!session.getAttribute(QString::fromLatin1(key)).isValid(), key);

    // A stored value wins, also one that is not a value of the key; removing
    // it returns to the default
    session.setAttribute("ACCEL_FS_G", QStringLiteral("8"));
    QCOMPARE(session.getAttribute("ACCEL_FS_G"), QVariant(QStringLiteral("8")));
    session.setAttribute("GYRO_ODR_HZ", QStringLiteral("abc"));
    QCOMPARE(session.getAttribute("GYRO_ODR_HZ"), QVariant(QStringLiteral("abc")));
    session.removeAttribute("ACCEL_FS_G");
    QCOMPARE(session.getAttribute("ACCEL_FS_G"), QVariant(QStringLiteral("16")));
    QCOMPARE(session.calculationEngine().runCount(kFit), 0);
}

// Items 1007, 1010, 1045: the configuration reaches the kernel in
// Channels::imuConfiguration, as numbers, for a stated and a defaulted
// session, and as NaN for a stored value that is not a number. The stated
// path: a fixture session, which stores its fixture's configuration, is
// fitted to the golden, channels and diagnostics (whose `configuration` is
// the stated one). The default path: the same session with the four keys
// removed reads firmware v2023.09.22's 12.5 Hz, which its 100 Hz logging
// disagrees with, and is rejected with the rate text naming both.
void FusionSessionTest::configurationReachesTheKernel()
{
    const QString fixture = QStringLiteral("coarse_maneuver");
    const auto channelsOf = [](const SessionData &session) {
        return Fusion::channelsFrom(
            [&session](const QString &sensor, const QString &name) { return session.getMeasurement(sensor, name); },
            [&session](const QString &key) { return session.getAttribute(key); });
    };
    const QStringList keys{QStringLiteral("ACCEL_FS_G"), QStringLiteral("GYRO_FS_DEG_S"),
                           QStringLiteral("ACCEL_ODR_HZ"), QStringLiteral("GYRO_ODR_HZ")};

    // Stated: the fixture session carries the direct kernel run's
    // configuration
    SessionData stated = fixtureSession(fixture, QStringLiteral("s1"));
    const Fusion::ImuConfiguration direct = toChannels(fusionFixture(fixture)).imuConfiguration;
    const Fusion::ImuConfiguration fromStated = channelsOf(stated).imuConfiguration;
    QCOMPARE(fromStated.accelFsG, 16.0);
    QCOMPARE(fromStated.gyroFsDegS, 2000.0);
    QCOMPARE(fromStated.accelOdrHz, 104.0);
    QCOMPARE(fromStated.gyroOdrHz, 104.0);
    QVERIFY(fromStated.accelFsG == direct.accelFsG && fromStated.gyroFsDegS == direct.gyroFsDegS);
    QVERIFY(fromStated.accelOdrHz == direct.accelOdrHz && fromStated.gyroOdrHz == direct.gyroOdrHz);

    // Defaulted: no key stored
    SessionData defaulted = fixtureSession(fixture, QStringLiteral("d1"));
    for (const QString &key : keys)
        defaulted.removeAttribute(key);
    const Fusion::ImuConfiguration fromDefaults = channelsOf(defaulted).imuConfiguration;
    QCOMPARE(fromDefaults.accelFsG, 16.0);
    QCOMPARE(fromDefaults.gyroFsDegS, 2000.0);
    QCOMPARE(fromDefaults.accelOdrHz, 12.5);
    QCOMPARE(fromDefaults.gyroOdrHz, 12.5);

    // Stored as the importer stores it (text, whitespace kept); a stored value
    // that is not a number is NaN, never 0, and the others are kept
    SessionData edited = fixtureSession(fixture, QStringLiteral("e1"));
    edited.setAttribute("GYRO_FS_DEG_S", QStringLiteral(" 500"));
    edited.setAttribute("GYRO_ODR_HZ", QStringLiteral("fast"));
    const Fusion::ImuConfiguration fromEdited = channelsOf(edited).imuConfiguration;
    QCOMPARE(fromEdited.gyroFsDegS, 500.0);
    QVERIFY(std::isnan(fromEdited.gyroOdrHz));
    QCOMPARE(fromEdited.accelFsG, 16.0);
    QCOMPARE(fromEdited.accelOdrHz, 104.0);

    // The stated path: the golden's channels and diagnostics
    const FusionGolden golden = loadFusionGolden(fixture);
    QCOMPARE(stated.calculationEngine().request(kFit).status, ResultStatus::Ok);
    QVERIFY(stated.calculationEngine().resultDetail(kFit).isEmpty());
    const QString difference = goldenDifference(stated, golden);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    const QString jsonDifference = diagnosticsDifference(stated, golden);
    QVERIFY2(jsonDifference.isEmpty(), qPrintable(jsonDifference));
    if (exactParityRequested()) {
        QCOMPARE(stated.getAttribute(kDiagnostics).toString().toUtf8(),
                 QJsonDocument(golden.diagnostics).toJson(QJsonDocument::Compact));
    }
    const QJsonObject configuration = diagnosticsOf(stated).value(QStringLiteral("configuration")).toObject();
    QCOMPARE(configuration.value(QStringLiteral("accel_odr_hz")).toDouble(), 104.);

    // The default path: rejected, a cached result with the rate's reason
    const QString rate =
        QStringLiteral("ACCEL_ODR_HZ states 12.5 Hz but the IMU is logged at 100.0 Hz; sensor fusion unavailable");
    QCOMPARE(defaulted.calculationEngine().request(kFit).status, ResultStatus::Ok);
    QCOMPARE(defaulted.calculationEngine().resultDetail(kFit), rate);
    QCOMPARE(diagnosticsOf(defaulted).value(QStringLiteral("failure")).toString(), rate);
    QVERIFY2(availableAmong(defaulted, valueNames()).isEmpty(), qPrintable(availableAmong(defaulted, valueNames())));
}

// Acceptance 5: reads of any fusion value on a session where fusion has not
// been requested return unavailable and run nothing, however many times and in
// whatever order.
void FusionSessionTest::readsNeverRunTheFit()
{
    const SessionData session = fixtureSession(QStringLiteral("coarse_linear"));
    CalculationEngine &engine = session.calculationEngine();
    const QList<DependencyKey> names = fusionNames();
    QCOMPARE(names.size(), 42);

    int runsAfterFirstRound = -1;
    // Each stride coprime with the count, so that every round reads every
    // name, scrambled; a stride sharing a factor with it would skip names
    // silently, so the strides change with the count
    const int strides[] = {5, 11, 13};
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < names.size(); ++i) {
            const DependencyKey &name = names.at((i * strides[round] + round * 3) % names.size());
            QVERIFY(!isAvailable(session, name));
            if (i % 4 == 0)
                QVERIFY(engine.blockers(name).state == BlockerState::Blocked);
            if (i % 7 == 0)
                QVERIFY(engine.readiness(kFit).state == ReadyState::Ready);
        }
        if (round == 0)
            runsAfterFirstRound = engine.totalRunCount();
    }

    QCOMPARE(engine.runCount(kFit), 0);
    QCOMPARE(engine.runCount(kAccH), 0);
    QCOMPARE(engine.runCount(kHAcc), 0);
    QCOMPARE(engine.runCount(kVAcc), 0);
    QCOMPARE(engine.runCount(kSAcc), 0);
    QCOMPARE(engine.runCount(kSystemTime), 0);
    QCOMPARE(engine.totalRunCount(), runsAfterFirstRound);
    QCOMPARE(engine.preparedCount(), 0);
    const std::optional<ResultStatus> status = engine.resultStatus(kFit);
    QVERIFY(!status.has_value() || *status == ResultStatus::NotRequested);
    QVERIFY(engine.verifyAgainstFresh(names).isEmpty());

    // The exporter reads stored data only
    const std::optional<QByteArray> bytes = DataExporter::toBytes(session);
    QVERIFY(bytes.has_value());
    QVERIFY(!bytes->contains("Fusion"));
    QVERIFY(!bytes->contains("FUSION"));
    QCOMPARE(engine.runCount(kFit), 0);
    QCOMPARE(engine.totalRunCount(), runsAfterFirstRound);
}

void FusionSessionTest::requestRunsOnceAndPublishesTogether_data()
{
    QTest::addColumn<QString>("fixture");
    for (const char *name : {"coarse_linear", "coarse_maneuver", "stationary_spin", "bridged_hole"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

// Acceptance 6: a request runs the fit once and publishes all outputs
// together; the fused speeds, accH, the fused accuracies and the system-time
// axis appear without being requested; a second request runs nothing.
void FusionSessionTest::requestRunsOnceAndPublishesTogether()
{
    QFETCH(QString, fixture);
    const FusionGolden golden = loadFusionGolden(fixture);
    const SessionData session = fixtureSession(fixture);
    CalculationEngine &engine = session.calculationEngine();

    const QList<DependencyKey> names = fusionNames();
    QVERIFY2(availableAmong(session, names).isEmpty(), qPrintable(availableAmong(session, names)));

    const CalculationEngine::RequestOutcome outcome = engine.request(kFit);
    QVERIFY(outcome.found);
    QCOMPARE(outcome.status, ResultStatus::Ok);
    for (const DependencyKey &name : names)
        QVERIFY(outcome.invalidated.contains(name));
    QVERIFY(engine.resultDetail(kFit).isEmpty());

    // All thirty-three, aligned, and the golden's
    const qsizetype length = fusion(session, QStringLiteral("_time")).size();
    QVERIFY(length > 0);
    for (const QString &name : fusionMeasurementNames())
        QCOMPARE(fusion(session, name).size(), length);
    const QString difference = goldenDifference(session, golden);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    const QString jsonDifference = diagnosticsDifference(session, golden);
    QVERIFY2(jsonDifference.isEmpty(), qPrintable(jsonDifference));
    if (exactParityRequested()) {
        QCOMPARE(session.getAttribute(kDiagnostics).toString().toUtf8(),
                 QJsonDocument(golden.diagnostics).toJson(QJsonDocument::Compact));
    }

    // The derived values, never requested
    const QVector<double> time = fusion(session, QStringLiteral("_time"));
    const QVector<double> velN = fusion(session, QStringLiteral("velN"));
    const QVector<double> velE = fusion(session, QStringLiteral("velE"));
    const QVector<double> velD = fusion(session, QStringLiteral("velD"));
    const QVector<double> velH = fusion(session, QStringLiteral("velH"));
    const QVector<double> vel = fusion(session, QStringLiteral("vel"));
    const QVector<double> accN = fusion(session, QStringLiteral("accN"));
    const QVector<double> accE = fusion(session, QStringLiteral("accE"));
    const QVector<double> accH = fusion(session, QStringLiteral("accH"));
    const QVector<double> systemTime = fusion(session, QStringLiteral("_system_time"));
    QCOMPARE(velH.size(), length);
    QCOMPARE(vel.size(), length);
    QCOMPARE(accH.size(), length);
    QCOMPARE(systemTime.size(), length);
    for (qsizetype i = 0; i < length; ++i) {
        // The magnitudes are recomputed here from products, which a
        // contracting compiler (clang on arm64) may fuse differently from the
        // library: bit-exact in exact mode, within 4 ulp otherwise. The system
        // time is not like that: with the fixture's fit (a = 1) it is one IEEE
        // subtraction and a division by one, which no compiler can round
        // differently.
        QVERIFY2(sameRecomputedValue(velH[i], std::sqrt(velN[i] * velN[i] + velE[i] * velE[i])),
                 qPrintable(QStringLiteral("velH[%1] = %2").arg(i).arg(velH[i], 0, 'g', 17)));
        QVERIFY2(sameRecomputedValue(vel[i], std::sqrt(velH[i] * velH[i] + velD[i] * velD[i])),
                 qPrintable(QStringLiteral("vel[%1] = %2").arg(i).arg(vel[i], 0, 'g', 17)));
        QVERIFY2(sameRecomputedValue(accH[i], std::sqrt(accN[i] * accN[i] + accE[i] * accE[i])),
                 qPrintable(QStringLiteral("accH[%1] = %2").arg(i).arg(accH[i], 0, 'g', 17)));
        QVERIFY(sameBits(systemTime[i], time[i] - kFixtureTimeFitB));
    }
    QVERIFY(session.getAttribute(fusionRollAtExit()).isValid());
    QVERIFY(std::isfinite(session.getAttribute(fusionRollAtExit()).toDouble()));

    // The fused accuracies, never requested (item 1708). The vertical one is
    // one correctly rounded square root of the published down variance, which
    // no compiler can round differently; the other two are held to their
    // known answers in tst_fusion_derived, and here to their contract
    const QVector<double> posCovDD = fusion(session, QStringLiteral("posCovDD"));
    const QVector<double> hAcc = fusion(session, QStringLiteral("hAcc"));
    const QVector<double> vAcc = fusion(session, QStringLiteral("vAcc"));
    const QVector<double> sAcc = fusion(session, QStringLiteral("sAcc"));
    QCOMPARE(hAcc.size(), length);
    QCOMPARE(vAcc.size(), length);
    QCOMPARE(sAcc.size(), length);
    for (qsizetype i = 0; i < length; ++i) {
        QVERIFY2(sameBits(vAcc[i], std::sqrt(posCovDD[i])),
                 qPrintable(QStringLiteral("vAcc[%1] = %2").arg(i).arg(vAcc[i], 0, 'g', 17)));
        QVERIFY2(std::isfinite(hAcc[i]) && hAcc[i] >= 0,
                 qPrintable(QStringLiteral("hAcc[%1] = %2").arg(i).arg(hAcc[i], 0, 'g', 17)));
        QVERIFY2(std::isfinite(sAcc[i]) && sAcc[i] >= 0,
                 qPrintable(QStringLiteral("sAcc[%1] = %2").arg(i).arg(sAcc[i], 0, 'g', 17)));
    }

    QCOMPARE(engine.runCount(kFit), 1);
    QCOMPARE(engine.runCount(kVelH), 1);
    QCOMPARE(engine.runCount(kVel), 1);
    QCOMPARE(engine.runCount(kAccH), 1);
    QCOMPARE(engine.runCount(kHAcc), 1);
    QCOMPARE(engine.runCount(kVAcc), 1);
    QCOMPARE(engine.runCount(kSAcc), 1);
    QCOMPARE(engine.runCount(kSystemTime), 1);

    // Asking again returns what is cached
    const CalculationEngine::RequestOutcome again = engine.request(kFit);
    QCOMPARE(again.status, ResultStatus::Ok);
    QVERIFY(again.invalidated.isEmpty());
    QCOMPARE(engine.runCount(kFit), 1);

    // Sibling reads run nothing
    const int runs = engine.totalRunCount();
    for (int round = 0; round < 100; ++round) {
        for (const DependencyKey &name : names)
            QVERIFY(isAvailable(session, name));
    }
    QCOMPARE(engine.totalRunCount(), runs);
    QCOMPARE(engine.undeclaredReadCount(), 0);

    if (fixture == QStringLiteral("stationary_spin")) {
        // Unwrapped across the whole fit: more than two turns, no wrap
        const QVector<double> yaw = fusion(session, QStringLiteral("yaw"));
        QVERIFY(qAbs(yaw.last() - yaw.first()) > 360.0);
        for (qsizetype i = 1; i < yaw.size(); ++i)
            QVERIFY(qAbs(yaw[i] - yaw[i - 1]) <= 180.0);
    }
}

void FusionSessionTest::asyncMatchesSync_data()
{
    QTest::addColumn<QString>("fixture");
    for (const char *name : {"coarse_linear", "coarse_maneuver", "reject_nonfinite"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

// Acceptance 7: prepare / compute / publish gives what request() gives.
void FusionSessionTest::asyncMatchesSync()
{
    QFETCH(QString, fixture);
    const bool expectSuccess = fusionFixture(fixture).expectSuccess;
    const SessionData syncSession = fixtureSession(fixture, QStringLiteral("sync"));
    const SessionData asyncSession = fixtureSession(fixture, QStringLiteral("async"));
    CalculationEngine &syncEngine = syncSession.calculationEngine();
    CalculationEngine &asyncEngine = asyncSession.calculationEngine();

    QCOMPARE(syncEngine.request(kFit).status, ResultStatus::Ok);

    CalculationEngine::PrepareOutcome prepared = asyncEngine.prepare(kFit);
    QVERIFY(prepared.kind == PrepareKind::Ready);
    QVERIFY(prepared.ticket != nullptr);
    QCOMPARE(prepared.ticket->title(), QStringLiteral("Sensor fusion"));
    QCOMPARE(asyncEngine.preparedCount(), 1);

    RecordingProgress progress;
    ComputedCalculation computed = computeOn(ComputeMode::Inline, *prepared.ticket, &progress);
    QVERIFY(computed.kind == ComputedCalculation::Kind::Completed);
    // Not published yet: still "not requested" for every reader
    QVERIFY(!asyncSession.getAttribute(kDiagnostics).isValid());

    const PublishOutcome published = prepared.ticket->publish(std::move(computed));
    QVERIFY(published.kind == PublishOutcome::Kind::Published);
    QCOMPARE(published.status, ResultStatus::Ok);
    QVERIFY(published.invalidated.contains(DependencyKey::attribute(kDiagnostics)));
    QCOMPARE(asyncEngine.preparedCount(), 0);

    for (const DependencyKey &name : valueNames()) {
        QVERIFY(name.type == DependencyKey::Type::Measurement
                || name == DependencyKey::attribute(fusionRollAtExit()));
        if (name.type == DependencyKey::Type::Attribute) {
            QCOMPARE(asyncSession.getAttribute(name.attributeKey), syncSession.getAttribute(name.attributeKey));
            QCOMPARE(asyncSession.getAttribute(name.attributeKey).isValid(), expectSuccess);
            continue;
        }
        const QString &measurement = name.measurementKey.second;
        QVERIFY2(sameBitsEverywhere(fusion(asyncSession, measurement), fusion(syncSession, measurement)),
                 qPrintable(measurement));
        QCOMPARE(!fusion(asyncSession, measurement).isEmpty(), expectSuccess);
    }
    QVERIFY(asyncSession.getAttribute(kDiagnostics).isValid());
    QCOMPARE(asyncSession.getAttribute(kDiagnostics).toString(), syncSession.getAttribute(kDiagnostics).toString());

    QVERIFY(asyncEngine.resultStatus(kFit) == syncEngine.resultStatus(kFit));
    QCOMPARE(asyncEngine.resultDetail(kFit), syncEngine.resultDetail(kFit));
    QCOMPARE(published.detail, syncEngine.resultDetail(kFit));
    QCOMPARE(asyncEngine.runCount(kFit), 1);
    QCOMPARE(syncEngine.runCount(kFit), 1);
    QCOMPARE(asyncEngine.undeclaredReadCount(), 0);
    QCOMPARE(syncEngine.undeclaredReadCount(), 0);

    if (expectSuccess) {
        QVERIFY(published.detail.isEmpty());
        QVERIFY(progress.texts().contains(QStringLiteral("Starting fit")));
    } else {
        QCOMPARE(published.detail, failureOf(loadFusionGolden(fixture)));
    }
}

// Acceptance 8, second half: a change of a declared input after publication
// drops the result and every dependent. Markers are not inputs.
void FusionSessionTest::changeAfterPublicationDropsEverything()
{
    SessionData session = fixtureSession(QStringLiteral("coarse_linear"));
    CalculationEngine &engine = session.calculationEngine();
    const QList<DependencyKey> names = fusionNames();

    QCOMPARE(engine.request(kFit).status, ResultStatus::Ok);
    for (const DependencyKey &name : names)
        QVERIFY(isAvailable(session, name));
    const QVector<double> accDBefore = fusion(session, QStringLiteral("accD"));

    // Markers and ground elevation do not move the fit
    for (const char *marker : {"_EXIT_TIME", "_ANALYSIS_START_TIME", "_GROUND_ELEV"}) {
        const QSet<DependencyKey> dropped = session.setAttribute(marker, kFixtureExitTime + .25);
        for (const DependencyKey &name : dropped)
            QVERIFY2(name.type != DependencyKey::Type::Measurement || name.measurementKey.first != QStringLiteral("Fusion"),
                     marker);
        QVERIFY(!dropped.contains(DependencyKey::attribute(kDiagnostics)));
    }
    QVERIFY(isAvailable(session, fusionKey(QStringLiteral("roll"))));
    QVERIFY(isAvailable(session, DependencyKey::attribute(fusionRollAtExit())));
    QCOMPARE(engine.runCount(kFit), 1);

    // One sample of one declared input, by 100 counts of the stated +/-16 g
    // range: a reading that range can show, so that the fit runs again
    QVector<double> az = session.getMeasurement("IMU", "az");
    az[50] += 100 * (16. / 32768 * 9.80665);
    const QSet<DependencyKey> dropped = session.setMeasurement("IMU", "az", az);
    for (const DependencyKey &name : names)
        QVERIFY(dropped.contains(name));
    QVERIFY2(availableAmong(session, names).isEmpty(), qPrintable(availableAmong(session, names)));
    QVERIFY(engine.readiness(kFit).state == ReadyState::Ready);
    QVERIFY(engine.blockers(fusionKey(QStringLiteral("accH"))).state == BlockerState::Blocked);
    QCOMPARE(engine.runCount(kFit), 1);

    QCOMPARE(engine.request(kFit).status, ResultStatus::Ok);
    QCOMPARE(engine.runCount(kFit), 2);
    const QVector<double> accDAfter = fusion(session, QStringLiteral("accD"));
    QVERIFY(!accDAfter.isEmpty());
    QVERIFY(!sameBitsEverywhere(accDAfter, accDBefore));
}

void FusionSessionTest::rejectionIsACachedResult_data()
{
    QTest::addColumn<QString>("fixture");
    for (const char *name : {"reject_nonfinite", "reject_imu_gap", "reject_sigma", "reject_origin"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

// Acceptance 9: a recording the model rejects is a cached result with a
// reason; asking again runs nothing; a change of its inputs makes it
// requestable again.
void FusionSessionTest::rejectionIsACachedResult()
{
    QFETCH(QString, fixture);
    const QString failure = failureOf(loadFusionGolden(fixture));
    QVERIFY(!failure.isEmpty());

    SessionData session = fixtureSession(fixture);
    CalculationEngine &engine = session.calculationEngine();

    const CalculationEngine::RequestOutcome outcome = engine.request(kFit);
    QVERIFY(outcome.found);
    QCOMPARE(outcome.status, ResultStatus::Ok);

    const QString available = availableAmong(session, valueNames());
    QVERIFY2(available.isEmpty(), qPrintable(available));
    QVERIFY(session.getAttribute(kDiagnostics).isValid());
    const QJsonObject diagnostics = diagnosticsOf(session);
    QCOMPARE(diagnostics.keys(), QStringList({QStringLiteral("algorithm"), QStringLiteral("failure")}));
    QCOMPARE(diagnostics.value(QStringLiteral("failure")).toString(), failure);
    QCOMPARE(engine.resultDetail(kFit), failure);

    // Ran and did not produce: nothing offers to run it again
    for (const char *name : {"roll", "accH"}) {
        const BlockerReport report = engine.blockers(fusionKey(QString::fromLatin1(name)));
        QVERIFY2(report.state == BlockerState::NotProduced, name);
        QVERIFY(report.blockers.isEmpty());
        QCOMPARE(report.notProduced.size(), 1);
        QCOMPARE(report.notProduced.first().calculation.registrationId, kFit);
        QCOMPARE(report.notProduced.first().status, ResultStatus::Ok);
        QCOMPARE(report.notProduced.first().detail, failure);
    }
    QVERIFY(engine.blockers(DependencyKey::attribute(kDiagnostics)).state == BlockerState::Available);

    QCOMPARE(engine.request(kFit).status, ResultStatus::Ok);
    QCOMPARE(engine.runCount(kFit), 1);
    const CalculationEngine::PrepareOutcome prepared = engine.prepare(kFit);
    QVERIFY(prepared.kind == PrepareKind::AlreadyValid);
    QCOMPARE(prepared.status, ResultStatus::Ok);
    QVERIFY(engine.readiness(kFit).state == ReadyState::Done);
    QCOMPARE(engine.runCount(kFit), 1);

    if (fixture != QStringLiteral("reject_sigma"))
        return;

    // The inputs change: requestable again, and this time it fits
    session.setMeasurement("GNSS", "sAcc", fusionFixture(QStringLiteral("coarse_linear")).sAcc);
    QVERIFY(engine.readiness(kFit).state == ReadyState::Ready);
    const BlockerReport blocked = engine.blockers(fusionKey(QStringLiteral("roll")));
    QVERIFY(blocked.state == BlockerState::Blocked);
    QCOMPARE(blocked.blockers.size(), 1);
    QVERIFY(!session.getAttribute(kDiagnostics).isValid());

    QCOMPARE(engine.request(kFit).status, ResultStatus::Ok);
    QCOMPARE(engine.runCount(kFit), 2);
    QVERIFY(engine.resultDetail(kFit).isEmpty());
    const QString difference = goldenDifference(session, loadFusionGolden(QStringLiteral("coarse_linear")));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

void FusionSessionTest::cancelStopsAtNextBoundary_data()
{
    QTest::addColumn<int>("cancelAtCall");
    QTest::newRow("before the fit") << 1;
    QTest::newRow("graph construction") << 2;
    QTest::newRow("first iteration") << 3;
    QTest::newRow("fourth boundary") << 4;
}

// Acceptance 10, the adapter's half: a cancellation request observed through
// the engine's facility stops the kernel at that boundary and surfaces as the
// engine's cancellation, so nothing is published and nothing is cached.
void FusionSessionTest::cancelStopsAtNextBoundary()
{
    QFETCH(int, cancelAtCall);
    const QString fixture = QStringLiteral("coarse_maneuver");
    const SessionData session = fixtureSession(fixture);
    CalculationEngine &engine = session.calculationEngine();

    CalculationEngine::PrepareOutcome prepared = engine.prepare(kFit);
    QVERIFY(prepared.kind == PrepareKind::Ready);

    CancelAtCall progress(cancelAtCall);
    ComputedCalculation computed = computeOn(ComputeMode::Inline, *prepared.ticket, &progress);
    QVERIFY(computed.kind == ComputedCalculation::Kind::Cancelled);
    // The cancelling boundary is the last one reached
    QCOMPARE(progress.texts().size(), cancelAtCall);
    QCOMPARE(progress.texts().first(), QStringLiteral("Starting fit"));

    const PublishOutcome published = prepared.ticket->publish(std::move(computed));
    QVERIFY(published.kind == PublishOutcome::Kind::Discarded);
    QVERIFY(published.reason == PublishOutcome::Reason::Cancelled);
    QVERIFY(published.invalidated.isEmpty());

    // As if it had never been asked
    const QString available = availableAmong(session, fusionNames());
    QVERIFY2(available.isEmpty(), qPrintable(available));
    const std::optional<ResultStatus> status = engine.resultStatus(kFit);
    QVERIFY(!status.has_value() || *status == ResultStatus::NotRequested);
    QCOMPARE(engine.preparedCount(), 0);
    QVERIFY(engine.readiness(kFit).state == ReadyState::Ready);

    QCOMPARE(engine.request(kFit).status, ResultStatus::Ok);
    const QString difference = goldenDifference(session, loadFusionGolden(fixture));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

void FusionSessionTest::missingInputsAreNotApplicable_data()
{
    QTest::addColumn<QString>("kind");
    QTest::newRow("no IMU data") << QStringLiteral("no-imu");
    QTest::newRow("no local origin") << QStringLiteral("no-origin");
    QTest::newRow("no shared UTC time for the IMU") << QStringLiteral("no-time");
    QTest::newRow("no IMU temperature") << QStringLiteral("no-temperature");
}

// Acceptance 11: a session that lacks an input has nothing to compute. It is
// never "not requested", and no request can run.
void FusionSessionTest::missingInputsAreNotApplicable()
{
    QFETCH(QString, kind);
    SessionData session;
    if (kind == QStringLiteral("no-imu")) {
        session = sessionWithoutImu(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("m1"));
    } else if (kind == QStringLiteral("no-origin")) {
        // No fix ever reaches 10 m horizontal accuracy
        session = naturalSession(QStringLiteral("m1"));
        session.setMeasurement("GNSS", "hAcc", QVector<double>(200, 10.0));
    } else if (kind == QStringLiteral("no-temperature")) {
        // Spec section 10's "a recording without a temperature channel", on
        // the engine side: a declared input the session lacks, like any other.
        FusionFixture f = fusionFixture(QStringLiteral("coarse_linear"));
        f.imuTemperature.clear();
        session = sessionFromFixture(f, QStringLiteral("m1"));
        QVERIFY(session.hasSensor("IMU"));
        QVERIFY(session.hasMeasurement("IMU", "wz"));
        QVERIFY(!session.hasMeasurement("IMU", "temperature"));
    } else {
        session = withoutSensor(naturalSession(QStringLiteral("m1")), QStringLiteral("TIME"));
        QVERIFY(session.hasSensor("IMU"));
        QVERIFY(!session.hasSensor("TIME"));
    }
    CalculationEngine &engine = session.calculationEngine();

    const CalculationReadiness readiness = engine.readiness(kFit);
    QVERIFY(readiness.state == ReadyState::MissingInput);
    QVERIFY(readiness.blockers.isEmpty());

    for (const DependencyKey &name : plotNames()) {
        const BlockerReport report = engine.blockers(name);
        QVERIFY2(report.state == BlockerState::NotApplicable, qPrintable(name.measurementKey.second));
        QVERIFY(report.blockers.isEmpty());
        QVERIFY(report.notProduced.isEmpty());
    }

    const CalculationEngine::PrepareOutcome prepared = engine.prepare(kFit);
    QVERIFY(prepared.kind == PrepareKind::NothingToRun);
    QCOMPARE(prepared.status, ResultStatus::MissingInput);
    QVERIFY(prepared.ticket == nullptr);

    const CalculationEngine::RequestOutcome outcome = engine.request(kFit);
    QVERIFY(outcome.found);
    QCOMPARE(outcome.status, ResultStatus::MissingInput);

    const QString available = availableAmong(session, fusionNames());
    QVERIFY2(available.isEmpty(), qPrintable(available));
    for (const DependencyKey &name : plotNames())
        QVERIFY(engine.blockers(name).state == BlockerState::NotApplicable);
    QCOMPARE(engine.runCount(kFit), 0);
}

// The temperature column reaches the kernel as recorded: a stored
// IMU/temperature in "deg C" is served as degC bit for bit, and the engine
// path's diagnostics equal the direct kernel call's (temperature included).
// drifting_bias carries a temperature ramp, so the model's slope is exercised
// and not merely carried; with the default Tuning this is one 201-state fit
// in a single 600 s segment.
void FusionSessionTest::temperatureReachesTheKernel()
{
    const FusionFixture fixture = initializerFixture(QStringLiteral("drifting_bias"));
    QVERIFY(!fixture.imuTemperature.isEmpty());
    SessionData session = sessionFromFixture(fixture, QStringLiteral("d1"));
    QVERIFY(sameBitsEverywhere(session.getMeasurement("IMU", "temperature"), fixture.imuTemperature));
    QCOMPARE(session.effectiveUnit("IMU", "temperature"), QStringLiteral("degC"));

    CalculationEngine &engine = session.calculationEngine();
    QVERIFY(engine.readiness(kFit).state == ReadyState::Ready);
    QCOMPARE(engine.request(kFit).status, ResultStatus::Ok);

    const Fusion::Result direct = Fusion::run(toChannels(fixture));
    QVERIFY2(direct.outcome == Fusion::Outcome::Succeeded, qPrintable(direct.reason));
    const QJsonObject diagnostics = diagnosticsOf(session);
    const QString difference = compareJson(QStringLiteral("diagnostics"), diagnostics,
                                           QJsonDocument::fromJson(direct.diagnosticsJson.toUtf8()).object());
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    // T_ref from the fixture's construction: the mean of 25 + i * .1 / 10
    // over i = 0..2000 is 35.
    const QJsonObject gyroBias = diagnostics.value(QStringLiteral("model")).toObject()
                                     .value(QStringLiteral("gyro_bias")).toObject();
    QVERIFY(qAbs(gyroBias.value(QStringLiteral("t_ref_degc")).toDouble(-1) - 35.0) < 1e-9);
    QCOMPARE(gyroBias.value(QStringLiteral("b1_rad_s_per_degc")).toArray().size(), 3);
}

// Acceptance 12: blocker inspection reports fusion through on-demand
// intermediates, nothing after publication, and never starts a fit.
void FusionSessionTest::blockersReportFusion()
{
    const SessionData session = fixtureSession(QStringLiteral("coarse_linear"));
    CalculationEngine &engine = session.calculationEngine();
    const QList<DependencyKey> names{fusionKey(QStringLiteral("accH")), fusionKey(QStringLiteral("_system_time")),
                                     fusionKey(QStringLiteral("roll")),
                                     DependencyKey::attribute(fusionRollAtExit())};

    for (int round = 0; round < 3; ++round) {
        for (const DependencyKey &name : names) {
            const BlockerReport report = engine.blockers(name);
            QVERIFY(report.state == BlockerState::Blocked);
            QVERIFY(report.notProduced.isEmpty());
            QCOMPARE(report.blockers.size(), 1);
            const CalculationBlocker &blocker = report.blockers.first();
            QCOMPARE(blocker.registrationId, kFit);
            QVERIFY(isEmptyName(blocker.instanceOutput));
            QCOMPARE(blocker.instanceId, kFit);
            QCOMPARE(blocker.title, QStringLiteral("Sensor fusion"));
        }
    }
    QCOMPARE(engine.runCount(kFit), 0);
    QCOMPARE(engine.runCount(kAccH), 0);
    QVERIFY2(availableAmong(session, names).isEmpty(), qPrintable(availableAmong(session, names)));

    // What inspection reported is what a request accepts
    const CalculationBlocker blocker = engine.blockers(names.first()).blockers.first();
    QCOMPARE(engine.request(blocker.registrationId, blocker.instanceOutput).status, ResultStatus::Ok);
    for (const DependencyKey &name : names) {
        const BlockerReport report = engine.blockers(name);
        QVERIFY(report.state == BlockerState::Available);
        QVERIFY(report.blockers.isEmpty());
        QVERIFY(report.notProduced.isEmpty());
    }
    QCOMPARE(engine.runCount(kFit), 1);
}

// The real input chain: GNSS/lat..velD -> Local and the origin, TIME -> the
// time fit -> IMU/_time. Expectations are structural; the numbers are the
// kernel's and are under golden test elsewhere.
// Item 1045, the default path: a recording as the importer would leave it,
// without any configuration key, logged at 12.5 Hz with readings on the
// +/-16 g and +/-2000 deg/s lattices, is fitted under the defaults, which the
// diagnostics report.
void FusionSessionTest::naturalSessionEndToEnd()
{
    SessionData session = naturalSession(QStringLiteral("n1"));
    for (const char *key : {"ACCEL_FS_G", "GYRO_FS_DEG_S", "ACCEL_ODR_HZ", "GYRO_ODR_HZ"})
        QVERIFY2(!session.hasStoredAttribute(QString::fromLatin1(key)), key);
    session.setAttribute(SessionKeys::ExitTime, kFixtureEpochUtc + 10.0);
    CalculationEngine &engine = session.calculationEngine();

    QVERIFY(engine.blockers(fusionKey(QStringLiteral("roll"))).state == BlockerState::Blocked);

    // Preparing computes the on-demand inputs, once, and not the fit
    CalculationEngine::PrepareOutcome prepared = engine.prepare(kFit);
    QVERIFY(prepared.kind == PrepareKind::Ready);
    QCOMPARE(engine.runCount(QStringLiteral("builtin.local.coordinates")), 1);
    QCOMPARE(engine.runCount(QStringLiteral("builtin.time.fit")), 1);
    QCOMPARE(engine.runCount(kFit), 0);

    QElapsedTimer timer;
    timer.start();
    RecordingProgress progress;
    ComputedCalculation computed = computeOn(ComputeMode::Inline, *prepared.ticket, &progress);
    qInfo() << "natural session fit:" << timer.elapsed() << "ms," << progress.texts().size() << "progress texts";
    QVERIFY(computed.kind == ComputedCalculation::Kind::Completed);
    const PublishOutcome published = prepared.ticket->publish(std::move(computed));
    QVERIFY(published.kind == PublishOutcome::Kind::Published);
    QCOMPARE(published.status, ResultStatus::Ok);
    QVERIFY2(published.detail.isEmpty(), qPrintable(published.detail));
    QCOMPARE(engine.runCount(kFit), 1);
    QCOMPARE(engine.runCount(QStringLiteral("builtin.local.coordinates")), 1);
    QCOMPARE(engine.runCount(QStringLiteral("builtin.time.fit")), 1);
    QCOMPARE(engine.undeclaredReadCount(), 0);

    // The fused samples are the original IMU samples inside the fitted interval
    const QVector<double> time = fusion(session, QStringLiteral("_time"));
    const QVector<double> imuTime = session.getMeasurement("IMU", "_time");
    const QVector<double> imuSystemTime = session.getMeasurement("IMU", "time");
    QVERIFY(!time.isEmpty());
    const qsizetype first = imuTime.indexOf(time.first());
    QVERIFY(first >= 0);
    QVERIFY(first + time.size() <= imuTime.size());
    QVERIFY(sameBitsEverywhere(time, imuTime.mid(first, time.size())));
    for (const QString &name : fusionMeasurementNames())
        QCOMPARE(fusion(session, name).size(), time.size());
    QCOMPARE(fusion(session, QStringLiteral("accH")).size(), time.size());

    const QVector<double> systemTime = fusion(session, QStringLiteral("_system_time"));
    QCOMPARE(systemTime.size(), time.size());
    for (qsizetype i = 0; i < systemTime.size(); ++i)
        QVERIFY(qAbs(systemTime[i] - imuSystemTime[first + i]) <= 1e-6);
    QVERIFY(session.getAttribute(fusionRollAtExit()).isValid());

    const QJsonObject diagnostics = diagnosticsOf(session);
    QCOMPARE(diagnostics.value(QStringLiteral("input")).toObject().value(QStringLiteral("origin_index")).toInt(-1), 0);
    QCOMPARE(diagnostics.value(QStringLiteral("gnss_states")).toInt(), 200);
    const QJsonObject configuration = diagnostics.value(QStringLiteral("configuration")).toObject();
    QCOMPARE(configuration.value(QStringLiteral("accel_fs_g")).toDouble(), 16.);
    QCOMPARE(configuration.value(QStringLiteral("gyro_fs_deg_s")).toDouble(), 2000.);
    QCOMPARE(configuration.value(QStringLiteral("accel_odr_hz")).toDouble(), 12.5);
    QCOMPARE(configuration.value(QStringLiteral("gyro_odr_hz")).toDouble(), 12.5);
    QCOMPARE(diagnostics.value(QStringLiteral("imu_outputs")).toInt(), int(time.size()));

    // A transitive input (TIME/tow -> the time fit -> IMU/_time) reaches the result
    QVector<double> tow = session.getMeasurement("TIME", "tow");
    for (double &value : tow)
        value += 1;
    const QSet<DependencyKey> dropped = session.setMeasurement("TIME", "tow", tow);
    QVERIFY(dropped.contains(fusionKey(QStringLiteral("roll"))));
    QVERIFY(dropped.contains(fusionKey(QStringLiteral("_system_time"))));
    QVERIFY(dropped.contains(DependencyKey::attribute(kDiagnostics)));
    QVERIFY2(availableAmong(session, fusionNames()).isEmpty(), qPrintable(availableAmong(session, fusionNames())));
    QVERIFY(engine.readiness(kFit).state == ReadyState::Ready);
    QCOMPARE(engine.runCount(kFit), 1);
}

void FusionSessionTest::twoSessionsAreIndependent()
{
    SessionData linear = fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("a"));
    const SessionData maneuver = fixtureSession(QStringLiteral("coarse_maneuver"), QStringLiteral("b"));

    // One descriptor serves both
    QCOMPARE(linear.calculationEngine().request(kFit).status, ResultStatus::Ok);
    QVERIFY(!maneuver.getAttribute(kDiagnostics).isValid());
    QCOMPARE(maneuver.calculationEngine().request(kFit).status, ResultStatus::Ok);

    QString difference = goldenDifference(linear, loadFusionGolden(QStringLiteral("coarse_linear")));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    difference = goldenDifference(maneuver, loadFusionGolden(QStringLiteral("coarse_maneuver")));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    // Invalidating one leaves the other published
    QVector<double> wz = linear.getMeasurement("IMU", "wz");
    wz[3] += 1.0;
    QVERIFY(linear.setMeasurement("IMU", "wz", wz).contains(fusionKey(QStringLiteral("yaw"))));
    QVERIFY(fusion(linear, QStringLiteral("yaw")).isEmpty());
    difference = goldenDifference(maneuver, loadFusionGolden(QStringLiteral("coarse_maneuver")));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QCOMPARE(linear.calculationEngine().runCount(kFit), 1);
    QCOMPARE(maneuver.calculationEngine().runCount(kFit), 1);
}

void FusionSessionTest::restoredFitIsIndistinguishable_data()
{
    QTest::addColumn<QString>("fixture");
    QTest::addColumn<bool>("expectSuccess");
    QTest::newRow("coarse_linear") << QStringLiteral("coarse_linear") << true;
    QTest::newRow("reject_sigma") << QStringLiteral("reject_sigma") << false;
}

// Export -> restore into a second session over the same recording: the same
// bits, the same graph, the same blockers, the same invalidation, and no run.
// Symmetric in A (published) and B (restored). Never verifyAgainstFresh here:
// the oracle would run the fit again.
void FusionSessionTest::restoredFitIsIndistinguishable()
{
    QFETCH(QString, fixture);
    QFETCH(bool, expectSuccess);
    const FusionGolden golden = loadFusionGolden(fixture);
    const QList<DependencyKey> names = fusionNames();

    QList<ExplicitEvent> eventsA, eventsB;
    SessionData a = fixtureSession(fixture, QStringLiteral("f1"));
    SessionData b = fixtureSession(fixture, QStringLiteral("f1"));
    CalculationEngine &engineA = a.calculationEngine();
    CalculationEngine &engineB = b.calculationEngine();
    engineA.setExplicitResultListener([&eventsA](const ExplicitEvent &e) { eventsA.append(e); });
    engineB.setExplicitResultListener([&eventsB](const ExplicitEvent &e) { eventsB.append(e); });

    // 1. The same reads in both, while the fit is not requested
    for (SessionData *session : {&a, &b}) {
        const QString available = availableAmong(*session, names);
        QVERIFY2(available.isEmpty(), qPrintable(available));
    }

    // 2. A publishes; its snapshot
    const CalculationEngine::RequestOutcome requested = engineA.request(kFit);
    QCOMPARE(requested.status, ResultStatus::Ok);
    const std::optional<StoredCalculationResult> snapshot = engineA.exportResult(kFit);
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->calculationId, kFit);
    QCOMPARE(snapshot->resultVersion, QStringLiteral("batch-temperature-bias-v10"));
    const QJsonObject diagnostics = QJsonDocument::fromJson(
        snapshot->bundle.attributeValue(kDiagnostics).toString().toUtf8()).object();
    QCOMPARE(diagnostics.value(QStringLiteral("algorithm")).toString(), snapshot->resultVersion);
    for (qsizetype i = 1; i < snapshot->leaves.size(); ++i)
        QVERIFY(storedLeafLess(snapshot->leaves.at(i - 1), snapshot->leaves.at(i)));    // sorted, unique
    QVERIFY(snapshot->leaves.contains(GraphNode::sourceMeasurement("IMU", "az")));
    QVERIFY(snapshot->leaves.contains(GraphNode::storedAttribute("_LOCAL_ORIGIN_LAT")));
    // SCHEMA_VER is reached through the schema conversion of the gyro inputs
    // (never declared by the fit): an edit of it makes the record stale
    QVERIFY(snapshot->leaves.contains(GraphNode::storedAttribute("SCHEMA_VER")));
    QCOMPARE(snapshot->inputFingerprint.size(), InputFingerprintSize);
    // What every looked-up name resolved to: sorted, unique, one entry per
    // declared input at least, the fit itself never among the providers
    QVERIFY(!snapshot->resolutions.isEmpty());
    for (qsizetype i = 1; i < snapshot->resolutions.size(); ++i)
        QVERIFY(storedResolutionLess(snapshot->resolutions.at(i - 1), snapshot->resolutions.at(i)));
    const auto calculated = [](const DependencyKey &name, const QString &instanceId) {
        return storedResolution(name, StoredResolution::Provider::Calculation, instanceId);
    };
    // The fixture recorded the Local channels as source data, so the
    // conversion layer provides them, as it does the IMU channels. az is not
    // schema-dependent: only the default conversion accepts it; the gyro
    // channels go through the schema conversion.
    QVERIFY(snapshot->resolutions.contains(calculated(DependencyKey::measurement("Local", "north"),
                                                      QStringLiteral("builtin.conversion.default#Local/north"))));
    QVERIFY(snapshot->resolutions.contains(calculated(DependencyKey::measurement("IMU", "az"),
                                                      QStringLiteral("builtin.conversion.default#IMU/az"))));
    QVERIFY(snapshot->resolutions.contains(calculated(DependencyKey::measurement("IMU", "wx"),
                                                      QStringLiteral("builtin.conversion.schema#IMU/wx"))));
    const std::optional<CalculationInstance> fit = CalculationRegistry::instance().instance(kFit);
    QVERIFY(fit.has_value());
    for (const CalcInput &input : fit->descriptor->inputs) {
        std::optional<DependencyKey> name;
        if (input.kind == CalcInput::Kind::Attribute)
            name = DependencyKey::attribute(input.key);
        else if (input.kind == CalcInput::Kind::Measurement)
            name = DependencyKey::measurement(input.sensor, input.name);
        if (!name)
            continue;
        const bool found = std::any_of(snapshot->resolutions.cbegin(), snapshot->resolutions.cend(),
                                       [&name](const StoredResolution &r) { return r.name == *name; });
        QVERIFY2(found, qPrintable(describe(*name)));
    }
    for (const StoredResolution &r : snapshot->resolutions)
        QVERIFY(r.instanceId != kFit);

    // 3. B restores it
    const Restore restored = engineB.restoreResult(*snapshot);
    QVERIFY(restored.kind == Restore::Kind::Restored);
    QCOMPARE(restored.status, ResultStatus::Ok);
    QCOMPARE(restored.invalidated, requested.invalidated);
    QCOMPARE(engineB.runCount(kFit), 0);

    // 4. The same reads again, then everything compared
    for (SessionData *session : {&a, &b}) {
        availableAmong(*session, names);
        QCOMPARE(isAvailable(*session, fusionKey(QStringLiteral("roll"))), expectSuccess);
        QVERIFY(isAvailable(*session, DependencyKey::attribute(kDiagnostics)));
    }
    for (const DependencyKey &name : valueNames()) {
        if (name.type == DependencyKey::Type::Attribute) {
            QCOMPARE(b.getAttribute(name.attributeKey), a.getAttribute(name.attributeKey));
            continue;
        }
        const QString &sensor = name.measurementKey.first;
        const QString &measurement = name.measurementKey.second;
        QVERIFY2(sameBitsEverywhere(b.getMeasurement(sensor, measurement), a.getMeasurement(sensor, measurement)),
                 qPrintable(measurement));
    }
    QCOMPARE(b.getAttribute(kDiagnostics).toString().toUtf8(), a.getAttribute(kDiagnostics).toString().toUtf8());
    QVERIFY(engineB.resultStatus(kFit) == engineA.resultStatus(kFit));
    QCOMPARE(engineB.resultDetail(kFit), engineA.resultDetail(kFit));
    QCOMPARE(engineB.dependenciesOf(GraphNode::result(kFit)), engineA.dependenciesOf(GraphNode::result(kFit)));
    QCOMPARE(engineB.edgeCount(), engineA.edgeCount());
    QCOMPARE(engineB.cachedNodeCount(), engineA.cachedNodeCount());
    for (const DependencyKey &name : {fusionKey(QStringLiteral("roll")), fusionKey(QStringLiteral("accH")),
                                      DependencyKey::attribute(kDiagnostics),
                                      DependencyKey::attribute(fusionRollAtExit())}) {
        QCOMPARE(reportText(engineB.blockers(name)), reportText(engineA.blockers(name)));
    }
    if (expectSuccess) {
        const QString difference = goldenDifference(b, golden);
        QVERIFY2(difference.isEmpty(), qPrintable(difference));
    } else {
        const BlockerReport report = engineB.blockers(fusionKey(QStringLiteral("roll")));
        QVERIFY(report.state == BlockerState::NotProduced);
        QCOMPARE(report.notProduced.size(), 1);
        QCOMPARE(report.notProduced.first().detail, failureOf(golden));
    }
    const std::optional<StoredCalculationResult> again = engineB.exportResult(kFit);
    QVERIFY(again.has_value());
    QVERIFY(sameContent(*again, *snapshot));
    QVERIFY(again->resolutions == snapshot->resolutions);

    // 5. One sample of one declared input changes in both: the same drop
    QVector<double> az = a.getMeasurement("IMU", "az");
    az[50] += .5;
    const QSet<DependencyKey> droppedA = a.setMeasurement("IMU", "az", az);
    const QSet<DependencyKey> droppedB = b.setMeasurement("IMU", "az", az);
    QCOMPARE(droppedB, droppedA);
    for (const DependencyKey &name : names)
        QVERIFY(droppedB.contains(name));

    const QString ok = QString::number(int(ResultStatus::Ok));
    QCOMPARE(eventTexts(eventsA), QStringList({QStringLiteral("Installed ") + kFit + QLatin1Char(' ') + ok,
                                               QStringLiteral("Dropped ") + kFit + QLatin1Char(' ') + ok}));
    QCOMPARE(eventTexts(eventsB), QStringList({QStringLiteral("Dropped ") + kFit + QLatin1Char(' ') + ok}));
    QVERIFY(!engineA.exportResult(kFit).has_value());
    QVERIFY(!engineB.exportResult(kFit).has_value());

    const Restore stale = engineB.restoreResult(*snapshot);
    QVERIFY(stale.kind == Restore::Kind::Stale);
    QVERIFY(stale.staleCheck == Restore::StaleCheck::Fingerprint);
    QCOMPARE(engineB.runCount(kFit), 0);
}

FLYSIGHT_TEST_MAIN(FusionSessionTest)
#include "tst_fusion_session.moc"
