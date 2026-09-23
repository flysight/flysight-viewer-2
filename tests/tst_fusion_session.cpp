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
const QString kAccH = QStringLiteral("builtin.fusion.accH");
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

/// The 17 measurements, accH and _system_time: everything but the diagnostics.
QList<DependencyKey> valueNames()
{
    QList<DependencyKey> names = fusionNames();
    names.removeAll(DependencyKey::attribute(kDiagnostics));
    return names;
}

/// The measurement behind each of the seventeen "Sensor fusion" plots.
QList<DependencyKey> plotNames()
{
    QList<DependencyKey> names;
    for (const QString &name : fusionMeasurementNames()) {
        if (name != QStringLiteral("_time"))
            names.append(fusionKey(name));
    }
    names.append(fusionKey(QStringLiteral("accH")));
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

/// "Installed 0" (the status as its number), "DroppedByInputChange 0"
QStringList eventTexts(const QList<ExplicitEvent> &events)
{
    QStringList text;
    for (const ExplicitEvent &event : events) {
        text.append((event.kind == ExplicitEvent::Kind::Installed ? QStringLiteral("Installed ")
                                                                  : QStringLiteral("DroppedByInputChange "))
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

    // Registered after every built-in, in this order
    QCOMPARE(registry.registeredIds().mid(registry.registeredIds().size() - 3),
             QStringList({kFit, kAccH, kSystemTime}));
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
        CalcInput::attribute("_LOCAL_ORIGIN_LON"), CalcInput::attribute("_LOCAL_ORIGIN_HMSL")};
    QCOMPARE(inputs.size(), 22);
    QVERIFY(fit->descriptor->inputs == inputs);

    QList<DependencyKey> outputs;
    for (const QString &name : fusionMeasurementNames())
        outputs.append(fusionKey(name));
    outputs.append(DependencyKey::attribute(kDiagnostics));
    QCOMPARE(outputs.size(), 18);
    QVERIFY(fit->descriptor->outputs == outputs);

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

    // Only the fit declares a result version: its kernel's algorithm string
    QCOMPARE(fit->descriptor->resultVersion, QStringLiteral("batch-temperature-bias-v3"));
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

// The premise of every golden comparison below.
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
    QCOMPARE(int(std::size(inputs)), 18);   // the 22 declared inputs less the four origin attributes
    for (const auto &input : inputs) {
        QVERIFY2(sameBitsEverywhere(session.getMeasurement(input.sensor, input.name), *input.expected),
                 input.name);
    }

    QCOMPARE(session.getAttribute("_LOCAL_ORIGIN_INDEX"), QVariant::fromValue(qlonglong(3)));
    QCOMPARE(session.getAttribute("_LOCAL_ORIGIN_LAT"), QVariant(45.0));
    QCOMPARE(session.getAttribute("_LOCAL_ORIGIN_LON"), QVariant(-75.0));
    QCOMPARE(session.getAttribute("_LOCAL_ORIGIN_HMSL"), QVariant(100.0));
    QCOMPARE(session.calculationEngine().runCount(kFit), 0);
}

// Acceptance 5: reads of any fusion value on a session where fusion has not
// been requested return unavailable and run nothing, however many times and in
// whatever order.
void FusionSessionTest::readsNeverRunTheFit()
{
    const SessionData session = fixtureSession(QStringLiteral("coarse_linear"));
    CalculationEngine &engine = session.calculationEngine();
    const QList<DependencyKey> names = fusionNames();
    QCOMPARE(names.size(), 21);

    int runsAfterFirstRound = -1;
    const int strides[] = {4, 5, 8};        // coprime with 21: every name, scrambled
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
    for (const char *name : {"coarse_linear", "coarse_maneuver", "stationary_spin"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

// Acceptance 6: a request runs the fit once and publishes all outputs
// together; accH and the system-time axis appear without being requested; a
// second request runs nothing.
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

    // All seventeen, aligned, and the golden's
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
    const QVector<double> accN = fusion(session, QStringLiteral("accN"));
    const QVector<double> accE = fusion(session, QStringLiteral("accE"));
    const QVector<double> accH = fusion(session, QStringLiteral("accH"));
    const QVector<double> systemTime = fusion(session, QStringLiteral("_system_time"));
    QCOMPARE(accH.size(), length);
    QCOMPARE(systemTime.size(), length);
    for (qsizetype i = 0; i < length; ++i) {
        // accH is recomputed here from products, which a contracting compiler
        // (clang on arm64) may fuse differently from the library: bit-exact in
        // exact mode, within 4 ulp otherwise. The system time is not like
        // that: with the fixture's fit (a = 1) it is one IEEE subtraction and
        // a division by one, which no compiler can round differently.
        QVERIFY2(sameRecomputedValue(accH[i], std::sqrt(accN[i] * accN[i] + accE[i] * accE[i])),
                 qPrintable(QStringLiteral("accH[%1] = %2").arg(i).arg(accH[i], 0, 'g', 17)));
        QVERIFY(sameBits(systemTime[i], time[i] - kFixtureTimeFitB));
    }
    QVERIFY(session.getAttribute(fusionRollAtExit()).isValid());
    QVERIFY(std::isfinite(session.getAttribute(fusionRollAtExit()).toDouble()));
    QCOMPARE(engine.runCount(kFit), 1);
    QCOMPARE(engine.runCount(kAccH), 1);
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

    // One sample of one declared input
    QVector<double> az = session.getMeasurement("IMU", "az");
    az[50] += .5;
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
    for (const char *name : {"reject_nonfinite", "reject_imu_gap", "reject_gnss_gap", "reject_sigma", "reject_origin"})
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
void FusionSessionTest::naturalSessionEndToEnd()
{
    SessionData session = naturalSession(QStringLiteral("n1"));
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
    QCOMPARE(snapshot->resultVersion, QStringLiteral("batch-temperature-bias-v3"));
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
                                               QStringLiteral("DroppedByInputChange ") + kFit + QLatin1Char(' ') + ok}));
    QCOMPARE(eventTexts(eventsB), QStringList({QStringLiteral("DroppedByInputChange ") + kFit + QLatin1Char(' ') + ok}));
    QVERIFY(!engineA.exportResult(kFit).has_value());
    QVERIFY(!engineB.exportResult(kFit).has_value());

    const Restore stale = engineB.restoreResult(*snapshot);
    QVERIFY(stale.kind == Restore::Kind::Stale);
    QVERIFY(stale.staleCheck == Restore::StaleCheck::Fingerprint);
    QCOMPARE(engineB.runCount(kFit), 0);
}

FLYSIGHT_TEST_MAIN(FusionSessionTest)
#include "tst_fusion_session.moc"
