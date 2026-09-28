// What is derived from the fit's published outputs, on real SessionData
// engines bound to the global registry: fused elevation and the fused
// along-track and cross-track accelerations. The solver never runs here. The
// fit's outputs are stored as data (syntheticFitSession(), fusionsessions.h),
// so the derivations are held to exact known answers, and only the tests
// that the derived values wait on the fit use a fixture session with the
// fit's inputs, which they never request.
//
// Expected values are literals chosen to be exactly representable, except
// where "the fused value is the GNSS definition" is the rule being tested:
// there the GNSS calculation on the same samples is the expectation.

#include <cmath>
#include <optional>

#include <QtTest>

#include "calculationdemand.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "fusion/fusionregistration.h"
#include "fusiongolden.h"
#include "fusionsessions.h"
#include "sessiondata.h"
#include "testenvironment.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;

using BlockerState = BlockerReport::State;

namespace {

const QString kFit = QStringLiteral("builtin.fusion.fit");
const QString kAccH = QStringLiteral("builtin.fusion.accH");
const QString kSystemTime = QStringLiteral("builtin.fusion.systemTime");
const QString kZ = QStringLiteral("builtin.fusion.z");
const QString kAlong = QStringLiteral("builtin.fusion.accAlongTrack");
const QString kCross = QStringLiteral("builtin.fusion.accCrossTrack");

QVector<double> fusion(const SessionData &session, const QString &name)
{
    return session.getMeasurement(QStringLiteral("Fusion"), name);
}

/// The inputs of a track-relative acceleration of `sensor`, in the order of
/// the GNSS registrations.
QList<CalcInput> trackInputs(const char *sensor)
{
    return {CalcInput::measurement(sensor, "accN"), CalcInput::measurement(sensor, "accE"),
            CalcInput::measurement(sensor, "accD"), CalcInput::measurement(sensor, "velN"),
            CalcInput::measurement(sensor, "velE"), CalcInput::measurement(sensor, "velD"),
            CalcInput::attribute("_WIND_N"), CalcInput::attribute("_WIND_E")};
}

/// One sample of acceleration and velocity, north-east-down.
struct TrackSample {
    double accN, accE, accD;
    double velN, velE, velD;
};

/// The six channels of `samples` as synthetic fit outputs.
QHash<QString, QVector<double>> trackChannels(const QList<TrackSample> &samples)
{
    QHash<QString, QVector<double>> channels;
    for (const TrackSample &s : samples) {
        channels[QStringLiteral("accN")].append(s.accN);
        channels[QStringLiteral("accE")].append(s.accE);
        channels[QStringLiteral("accD")].append(s.accD);
        channels[QStringLiteral("velN")].append(s.velN);
        channels[QStringLiteral("velE")].append(s.velE);
        channels[QStringLiteral("velD")].append(s.velD);
    }
    return channels;
}

/// A synthetic session over `samples`, with the wind stored when given.
SessionData trackSession(const QString &id, const QList<TrackSample> &samples,
                         const std::optional<std::pair<QVariant, QVariant>> &wind)
{
    SessionData session = syntheticFitSession(id, trackChannels(samples));
    if (wind) {
        session.setAttribute(SessionKeys::WindN, wind->first);
        session.setAttribute(SessionKeys::WindE, wind->second);
    }
    return session;
}

/// Four samples with exact answers under zero wind: velocity along north
/// (along 3, cross 4), reversed (along -3, cross 4), a vertical descent with
/// vertical acceleration (along = accD = 5, cross 0), and no motion through
/// the air (along 0, cross |a| = 5).
QList<TrackSample> calmSamples()
{
    return {{3, 4, 0, 10, 0, 0},
            {3, 4, 0, -10, 0, 0},
            {0, 0, 5, 0, 0, 20},
            {3, 4, 0, 0, 0, 0}};
}
const QVector<double> kCalmAlong{3, -3, 5, 0};
const QVector<double> kCalmCross{4, 4, 0, 5};

} // namespace

class FusionDerivedTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void syntheticOutputsAreServedWithoutAFit();
    void derivedRegistrationShape();
    void derivedValuesWaitOnTheFit();
    void elevationIsOriginHeightMinusDownAboveGround();
    void trackAccelerationsKnownAnswers();
    void trackAccelerationsAreTheGnssDefinitions();

private:
    QStringList m_registryBefore;
};

void FusionDerivedTest::initTestCase()
{
    // As the application does: the built-ins, then sensor fusion
    TestEnvironment::instance().registerBuiltIns();
    registerFusionOnce();
}

void FusionDerivedTest::init()
{
    TestEnvironment::instance().resetPreferencesToDefaults();
    m_registryBefore = CalculationRegistry::instance().registeredIds();
}

void FusionDerivedTest::cleanup()
{
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

// The premise of every synthetic test below: stored fit outputs are what a
// reader gets, bit for bit, and nothing runs the fit to get them.
void FusionDerivedTest::syntheticOutputsAreServedWithoutAFit()
{
    QHash<QString, QVector<double>> channels;
    const QStringList names = fusionMeasurementNames();
    QCOMPARE(names.size(), 17);
    for (qsizetype k = 0; k < names.size(); ++k) {
        QVector<double> samples;
        for (int i = 0; i < 5; ++i)
            samples.append(0.1 * (k + 1) + i / 3.0);    // not exactly representable: bits matter
        channels.insert(names.at(k), samples);
    }
    const SessionData session = syntheticFitSession(QStringLiteral("s1"), channels);
    CalculationEngine &engine = session.calculationEngine();

    for (const QString &name : names)
        QVERIFY2(sameBitsEverywhere(fusion(session, name), channels.value(name)), qPrintable(name));
    QCOMPARE(session.getAttribute(SessionKeys::SessionId).toString(), QStringLiteral("s1"));

    QCOMPARE(engine.runCount(kFit), 0);
    QVERIFY(!engine.resultStatus(kFit).has_value());
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// Criterion 1 and the registry half of criterion 2.
void FusionDerivedTest::derivedRegistrationShape()
{
    const CalculationRegistry &registry = CalculationRegistry::instance();

    const QStringList ids = registry.registeredIds();
    QCOMPARE(ids.mid(ids.size() - 6), QStringList({kFit, kAccH, kSystemTime, kZ, kAlong, kCross}));

    const struct {
        const QString &id;
        const char *output;
        QList<CalcInput> inputs;
    } expected[] = {
        {kZ, "z",
         {CalcInput::measurement("Fusion", "down"), CalcInput::attribute("_LOCAL_ORIGIN_HMSL"),
          CalcInput::attribute("_GROUND_ELEV")}},
        {kAlong, "accAlongTrack", trackInputs("Fusion")},
        {kCross, "accCrossTrack", trackInputs("Fusion")},
    };

    for (const auto &e : expected) {
        const std::optional<CalculationInstance> instance = registry.instance(e.id);
        QVERIFY2(instance.has_value(), qPrintable(e.id));
        const CalculationDescriptor &d = *instance->descriptor;
        QVERIFY2(d.policy == EvaluationPolicy::OnDemand, qPrintable(e.id));
        QVERIFY2(d.title.isEmpty(), qPrintable(e.id));
        QVERIFY2(d.resultVersion.isEmpty(), qPrintable(e.id));
        QVERIFY2(d.inputs == e.inputs, qPrintable(e.id));
        const DependencyKey output = fusionKey(QString::fromLatin1(e.output));
        QVERIFY2(d.outputs == QList<DependencyKey>({output}), qPrintable(e.id));

        // One candidate: nothing else produces the name
        const QList<CalculationInstance> candidates = registry.candidatesFor(output);
        QCOMPARE(candidates.size(), 1);
        QCOMPARE(candidates.first().instanceId, e.id);

        // Behind it stands the fit, and only the fit
        QCOMPARE(registry.explicitDependencies(output), QStringList({kFit}));
    }

    // The fused track accelerations declare what the GNSS ones declare
    const std::optional<CalculationInstance> gnssAlong = registry.instance(QStringLiteral("builtin.gnss.accAlongTrack"));
    QVERIFY(gnssAlong.has_value());
    QVERIFY(gnssAlong->descriptor->inputs == trackInputs("GNSS"));

    // Vertical acceleration is the fit's own accD: no second producer
    QCOMPARE(registry.candidatesFor(fusionKey(QStringLiteral("accD"))).size(), 1);
    QCOMPARE(registry.candidatesFor(fusionKey(QStringLiteral("accD"))).first().instanceId, kFit);
}

// The session half of criterion 2: with the fit's inputs and no fit, each
// derived value waits on the fit and on nothing else, inspection says so, and
// nothing starts it.
void FusionDerivedTest::derivedValuesWaitOnTheFit()
{
    SessionData session = fixtureSession(QStringLiteral("coarse_linear"));
    session.setAttribute(SessionKeys::GroundElev, 50.0);
    CalculationEngine &engine = session.calculationEngine();

    for (const char *name : {"z", "accAlongTrack", "accCrossTrack"}) {
        const DependencyKey key = fusionKey(QString::fromLatin1(name));
        const BlockerReport report = engine.blockers(key);
        QVERIFY2(report.state == BlockerState::Blocked, name);
        QCOMPARE(report.blockers.size(), 1);
        QCOMPARE(report.blockers.first().registrationId, kFit);
        QCOMPARE(report.blockers.first().instanceId, kFit);
        QVERIFY(report.notProduced.isEmpty());
        QVERIFY2(CalculationDemand::isMerelyUncomputed(session, QStringLiteral("Fusion"), QString::fromLatin1(name)),
                 name);
        QVERIFY2(fusion(session, QString::fromLatin1(name)).isEmpty(), name);
    }

    QCOMPARE(engine.runCount(kFit), 0);
    const std::optional<ResultStatus> status = engine.resultStatus(kFit);
    QVERIFY(!status.has_value() || *status == ResultStatus::NotRequested);
}

// Criterion 3: elevation above the same ground as GNSS/z, recomputed from an
// attribute edit without the fit, and unavailable when an attribute is not a
// number.
void FusionDerivedTest::elevationIsOriginHeightMinusDownAboveGround()
{
    SessionData session = syntheticFitSession(QStringLiteral("e1"),
                                              {{QStringLiteral("down"), {0.0, 1.5, -2.25, 100.0}}});
    session.setAttribute(SessionKeys::LocalOriginHmsl, 500.0);
    session.setAttribute(SessionKeys::GroundElev, 120.5);
    CalculationEngine &engine = session.calculationEngine();

    QCOMPARE(fusion(session, QStringLiteral("z")), QVector<double>({379.5, 378.0, 381.75, 279.5}));
    QCOMPARE(engine.runCount(kZ), 1);

    // A new ground elevation
    QVERIFY(session.setAttribute(SessionKeys::GroundElev, 100.25).contains(fusionKey(QStringLiteral("z"))));
    QCOMPARE(fusion(session, QStringLiteral("z")), QVector<double>({399.75, 398.25, 402.0, 299.75}));
    QCOMPARE(engine.runCount(kZ), 2);

    // A new origin height
    QVERIFY(session.setAttribute(SessionKeys::LocalOriginHmsl, 600.0).contains(fusionKey(QStringLiteral("z"))));
    QCOMPARE(fusion(session, QStringLiteral("z")), QVector<double>({499.75, 498.25, 502.0, 399.75}));
    QCOMPARE(engine.runCount(kZ), 3);

    // Not a number: unavailable, and back when it is one again
    session.setAttribute(SessionKeys::GroundElev, QStringLiteral("not a number"));
    QVERIFY(fusion(session, QStringLiteral("z")).isEmpty());
    session.setAttribute(SessionKeys::GroundElev, 100.25);
    QCOMPARE(fusion(session, QStringLiteral("z")), QVector<double>({499.75, 498.25, 502.0, 399.75}));
    session.setAttribute(SessionKeys::LocalOriginHmsl, QStringLiteral("high"));
    QVERIFY(fusion(session, QStringLiteral("z")).isEmpty());

    QCOMPARE(engine.runCount(kFit), 0);
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// Criterion 4: exact answers with stored wind, the wind rule, phase 2's zero
// wind default, and unequal lengths.
void FusionDerivedTest::trackAccelerationsKnownAnswers()
{
    const auto along = [](const SessionData &s) { return fusion(s, QStringLiteral("accAlongTrack")); };
    const auto cross = [](const SessionData &s) { return fusion(s, QStringLiteral("accCrossTrack")); };

    // Stored zero wind: north, reversed, vertical descent, no motion
    {
        const SessionData session = trackSession(QStringLiteral("t1"), calmSamples(),
                                                 std::make_pair(QVariant(0.0), QVariant(0.0)));
        QCOMPARE(along(session), kCalmAlong);
        QCOMPARE(cross(session), kCalmCross);
        QCOMPARE(along(session)[2], fusion(session, QStringLiteral("accD"))[2]);
        QCOMPARE(session.calculationEngine().runCount(kFit), 0);
        QCOMPARE(session.calculationEngine().undeclaredReadCount(), 0);
    }

    // A wind that turns a skewed ground velocity into a northward air
    // velocity, and one that cancels the ground velocity: below 1e-9 m/s
    // through the air, along-track is 0 and cross-track is |a|
    {
        const SessionData session = trackSession(QStringLiteral("t2"),
                                                 {{3, 4, 0, 12, 3, 0}, {3, 4, 0, 2, 3, 0}},
                                                 std::make_pair(QVariant(2.0), QVariant(3.0)));
        QCOMPARE(along(session), QVector<double>({3, 0}));
        QCOMPARE(cross(session), QVector<double>({4, 5}));
    }

    // A stored wind that is not a number reads as zero
    {
        const SessionData session = trackSession(QStringLiteral("t3"), calmSamples(),
                                                 std::make_pair(QVariant(QStringLiteral("calm")),
                                                                QVariant(QStringLiteral(""))));
        QCOMPARE(along(session), kCalmAlong);
        QCOMPARE(cross(session), kCalmCross);
    }

    // No stored wind: the constant defaults, zero
    {
        const SessionData session = trackSession(QStringLiteral("t4"), calmSamples(), std::nullopt);
        QVERIFY(!session.hasStoredAttribute(SessionKeys::WindN));
        QCOMPARE(session.getAttribute(SessionKeys::WindN).toDouble(), 0.0);
        QCOMPARE(along(session), kCalmAlong);
        QCOMPARE(cross(session), kCalmCross);

        // The same bits as with a stored zero wind, on samples with no exact answer
        const QList<TrackSample> skewed{{1.25, -0.5, 0.75, 17.0, 6.0, 9.0}, {-2.0, 1.0, 0.5, 3.0, -11.0, 4.0}};
        const SessionData withoutWind = trackSession(QStringLiteral("t5"), skewed, std::nullopt);
        const SessionData zeroWind = trackSession(QStringLiteral("t6"), skewed,
                                                  std::make_pair(QVariant(0.0), QVariant(0.0)));
        QVERIFY(!along(withoutWind).isEmpty());
        QVERIFY(sameBitsEverywhere(along(withoutWind), along(zeroWind)));
        QVERIFY(sameBitsEverywhere(cross(withoutWind), cross(zeroWind)));
    }

    // Arrays of unequal length: both unavailable
    {
        QHash<QString, QVector<double>> channels = trackChannels(calmSamples());
        channels[QStringLiteral("velD")].removeLast();
        SessionData session = syntheticFitSession(QStringLiteral("t7"), channels);
        session.setAttribute(SessionKeys::WindN, 0.0);
        session.setAttribute(SessionKeys::WindE, 0.0);
        QVERIFY(along(session).isEmpty());
        QVERIFY(cross(session).isEmpty());
        QCOMPARE(session.calculationEngine().runCount(kAlong), 1);
        QCOMPARE(session.calculationEngine().runCount(kCross), 1);
    }
}

// Criterion 5: one definition. The same samples under GNSS and Fusion, with
// the same wind, give the same along-track and cross-track accelerations.
//
// flysight_fusion is compiled without contraction and flysight_core is not,
// so on a contracting compiler (clang on arm64) the two copies of the one
// inline definition may round differently. The samples are chosen so that
// neither result amplifies that difference: every term of the along-track
// dot product is positive (the air velocity and the acceleration point into
// the same octant: north, east and down all positive), and the acceleration
// is mostly across the track, so cross-track is never a small difference of
// squares. Emulated under every contraction pattern of the GNSS copy, the
// two differ by at most 1.9 ulp (along) and 1.2 ulp (cross), inside the
// 4 ulp of sameRecomputedValue(). The guards below keep that premise if the
// samples are ever changed.
void FusionDerivedTest::trackAccelerationsAreTheGnssDefinitions()
{
    constexpr int n = 60;
    constexpr double windN = 1.5;
    constexpr double windE = -2.25;
    QList<TrackSample> samples;
    for (int i = 0; i < n; ++i) {
        samples.append({1.0 + 0.5 * std::sin(i * 0.3), 5.0 + std::cos(i * 0.2), 1.0 + 0.5 * std::sin(i * 0.11),
                        20.0 + 5.0 * std::sin(i * 0.1), 3.0 + 2.0 * std::cos(i * 0.13), 15.0 + 3.0 * std::sin(i * 0.07)});
    }
    const QHash<QString, QVector<double>> channels = trackChannels(samples);

    SessionData session = syntheticFitSession(QStringLiteral("g1"), channels);
    for (const char *name : {"accN", "accE", "accD"})
        session.setSourceMeasurement("GNSS", name, channels.value(QString::fromLatin1(name)), "m/s^2");
    for (const char *name : {"velN", "velE", "velD"})
        session.setSourceMeasurement("GNSS", name, channels.value(QString::fromLatin1(name)), "m/s");
    session.setAttribute(SessionKeys::WindN, windN);
    session.setAttribute(SessionKeys::WindE, windE);

    // Stored GNSS data wins over builtin.gnss.accN and the other derivatives
    for (const char *name : {"accN", "accE", "accD", "velN", "velE", "velD"}) {
        QVERIFY2(sameBitsEverywhere(session.getMeasurement("GNSS", name), channels.value(QString::fromLatin1(name))),
                 name);
    }

    const QVector<double> gnssAlong = session.getMeasurement("GNSS", "accAlongTrack");
    const QVector<double> gnssCross = session.getMeasurement("GNSS", "accCrossTrack");
    const QVector<double> fusedAlong = fusion(session, QStringLiteral("accAlongTrack"));
    const QVector<double> fusedCross = fusion(session, QStringLiteral("accCrossTrack"));
    QCOMPARE(gnssAlong.size(), n);
    QCOMPARE(gnssCross.size(), n);
    QCOMPARE(fusedAlong.size(), n);
    QCOMPARE(fusedCross.size(), n);

    for (int i = 0; i < n; ++i) {
        // The premise: no cancellation in along-track (its terms do not
        // cancel) and none in cross-track (the acceleration is not mostly
        // along the track)
        const TrackSample &s = samples.at(i);
        const double wcN = s.velN - windN;
        const double wcE = s.velE - windE;
        const double wcD = s.velD;
        const double wcMag = std::sqrt(wcN * wcN + wcE * wcE + wcD * wcD);
        const double terms = std::abs(s.accN * wcN / wcMag) + std::abs(s.accE * wcE / wcMag)
                           + std::abs(s.accD * wcD / wcMag);
        QVERIFY2(std::abs(gnssAlong[i]) >= 0.5 * terms, qPrintable(QString::number(i)));
        const double magnitude = std::sqrt(s.accN * s.accN + s.accE * s.accE + s.accD * s.accD);
        QVERIFY2(gnssCross[i] >= 0.5 * magnitude, qPrintable(QString::number(i)));

        // Two libraries compile the one inline definition, one of them with
        // contraction off: bit-exact in exact mode, within 4 ulp otherwise
        QVERIFY2(sameRecomputedValue(fusedAlong[i], gnssAlong[i]),
                 qPrintable(QStringLiteral("accAlongTrack[%1]: %2 vs %3")
                                .arg(i).arg(fusedAlong[i], 0, 'g', 17).arg(gnssAlong[i], 0, 'g', 17)));
        QVERIFY2(sameRecomputedValue(fusedCross[i], gnssCross[i]),
                 qPrintable(QStringLiteral("accCrossTrack[%1]: %2 vs %3")
                                .arg(i).arg(fusedCross[i], 0, 'g', 17).arg(gnssCross[i], 0, 'g', 17)));
    }

    QCOMPARE(session.calculationEngine().runCount(kFit), 0);
    QCOMPARE(session.calculationEngine().undeclaredReadCount(), 0);
}

FLYSIGHT_TEST_MAIN(FusionDerivedTest)
#include "tst_fusion_derived.moc"
