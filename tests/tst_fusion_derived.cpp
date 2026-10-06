// What is derived from the fit's published outputs, on real SessionData
// engines bound to the global registry: fused elevation, the fused horizontal
// and total speeds, the fused along-track and cross-track accelerations, the
// fused accuracies (horizontal, vertical and speed, from the published
// covariance blocks), and the attitude of the body
// frame (heading, pitch and roll) that the orientation attribute defines. The
// solver never runs here. The fit's outputs are stored as data
// (syntheticFitSession(), fusionsessions.h), so the derivations are held to
// exact known answers, and only the tests that the derived values wait on
// the fit use a fixture session with the fit's inputs, which they never
// request.
//
// Expected values are literals chosen to be exactly representable, except
// where "the fused value is the GNSS definition" is the rule being tested:
// there the GNSS calculation on the same samples is the expectation. The
// attitude's expectations are built by hand here: Euler angles to a
// body-to-north-east-down quaternion, composed with a hand-written constant
// for the default mount, never from the orientation type's own rotation.
// Tokens are spelled literally.
//
// The orientation vocabulary (Fusion::Orientation) and the Orientation
// column's model, edit and bulk edit (through ChoiceFixture, on fixture
// sessions that may enter a SessionModel) are tested here too: this is the
// executable that links the fusion library, which registers the attribute.

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>

#include <QSignalSpy>
#include <QtTest>

#include "attributeregistry.h"
#include "calculationdemand.h"
#include "calculations/anglehelper.h"
#include "choicefixture.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "fixturebuilder.h"
#include "fusion/fusionregistration.h"
#include "fusion/orientation.h"
#include "fusiongolden.h"
#include "fusionsessions.h"
#include "idlescheduler.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "logbookprobe.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;

using BlockerState = BlockerReport::State;

namespace {

const QString kFit = QStringLiteral("builtin.fusion.fit");
const QString kVelH = QStringLiteral("builtin.fusion.velH");
const QString kVel = QStringLiteral("builtin.fusion.vel");
const QString kAccH = QStringLiteral("builtin.fusion.accH");
const QString kHAcc = QStringLiteral("builtin.fusion.hAcc");
const QString kVAcc = QStringLiteral("builtin.fusion.vAcc");
const QString kSAcc = QStringLiteral("builtin.fusion.sAcc");
const QString kSystemTime = QStringLiteral("builtin.fusion.systemTime");
const QString kZ = QStringLiteral("builtin.fusion.z");
const QString kAlong = QStringLiteral("builtin.fusion.accAlongTrack");
const QString kCross = QStringLiteral("builtin.fusion.accCrossTrack");
const QString kOrientationDefault = QStringLiteral("builtin.default._ORIENTATION");
const QString kAttitude = QStringLiteral("builtin.fusion.attitude");

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

/// Velocities whose speeds are exact: Pythagorean triples at each step, signs
/// mixed, a standstill, and two triples halved.
const QVector<double> kVelN{3, -5, 8, 0, 1.5};
const QVector<double> kVelE{4, 12, -15, 0, 2};
const QVector<double> kVelD{12, -84, 144, 0, -6};
const QVector<double> kSpeedH{5, 13, 17, 0, 2.5};
const QVector<double> kSpeed{13, 85, 145, 0, 6.5};

/// The three velocity channels as synthetic fit outputs.
QHash<QString, QVector<double>> velocityChannels(const QVector<double> &velN, const QVector<double> &velE,
                                                 const QVector<double> &velD)
{
    return {{QStringLiteral("velN"), velN}, {QStringLiteral("velE"), velE}, {QStringLiteral("velD"), velD}};
}

/// The upper triangle of a symmetric 3x3 block in the navigation frame, in
/// the order of the published names: NN, NE, ND, EE, ED, DD.
using Block = std::array<double, 6>;

/// The six entries of each block spread over the six channels
/// <prefix>NN ... <prefix>DD ("posCov" or "velCov"), one sample per block, as
/// synthetic fit outputs.
QHash<QString, QVector<double>> blockChannels(const char *prefix, const QList<Block> &blocks)
{
    const char *const entries[] = {"NN", "NE", "ND", "EE", "ED", "DD"};
    QHash<QString, QVector<double>> channels;
    for (const Block &block : blocks) {
        for (std::size_t k = 0; k < block.size(); ++k)
            channels[QString::fromLatin1(prefix) + QString::fromLatin1(entries[k])].append(block[k]);
    }
    return channels;
}

/// Position blocks whose horizontal eigenvalues are exact: diagonal (9, 4)
/// and (4, 9), rotated (8, 2, 5) with eigenvalues 9 and 4, (5, +-4, 5) with
/// eigenvalues 9 and 1, the zero block, and the first four again with down
/// entries the horizontal accuracy does not read. Each down variance is 6.25
/// or 0.
const QList<Block> kPositionBlocks{{9, 0, 0, 4, 0, 6.25},       {4, 0, 0, 9, 0, 0},
                                   {8, 2, 0, 5, 0, 6.25},       {5, 4, 0, 5, 0, 0},
                                   {5, -4, 0, 5, 0, 6.25},      {0, 0, 0, 0, 0, 0},
                                   {9, 0, 1.5, 4, -1, 6.25},    {8, 2, -0.5, 5, 0.75, 0},
                                   {5, 4, 0.25, 5, 0.5, 6.25},  {4, 0, 0.5, 9, 0.25, 6.25}};
const QVector<double> kHAccAnswers{3, 3, 3, 3, 3, 0, 3, 3, 3, 3};
const QVector<double> kVAccAnswers{2.5, 0, 2.5, 0, 2.5, 0, 2.5, 0, 2.5, 2.5};

/// Velocity blocks and velocities with exact answers. Under diag(4, 9, 16):
/// along north at 10 m/s, its sigma 2 (not the largest, 4); along east at
/// 3 m/s, exactly its sigma 3, which takes the along branch; down at 10 m/s,
/// 4; north at 1 m/s, below its sigma 2, and at rest, both the largest, 4.
/// Under the full block (5, 2, 2, 5, 2, 5), eigenvalues 9, 3 and 3: along
/// (3, 3, 3), quadratic form 243/27 = 9 below a speed of 3 sqrt 3, so 3; north
/// at 1 m/s, below its sigma sqrt 5, the largest by the 3x3 form, 3 (q = 5,
/// p = 2, det(B) = 2, r = 1). Under (5, 4, 0, 5, 0, 2), eigenvalues 9, 2 and
/// 1, at rest: 3 within 1e-12 (q = 4, p = sqrt(19/3), not dyadic). The zero
/// block at rest and in motion: 0.
const QList<Block> kVelocityBlocks{{4, 0, 0, 9, 0, 16}, {4, 0, 0, 9, 0, 16}, {4, 0, 0, 9, 0, 16},
                                   {4, 0, 0, 9, 0, 16}, {4, 0, 0, 9, 0, 16}, {5, 2, 2, 5, 2, 5},
                                   {5, 2, 2, 5, 2, 5},  {5, 4, 0, 5, 0, 2},  {0, 0, 0, 0, 0, 0},
                                   {0, 0, 0, 0, 0, 0}};
const QVector<double> kAccuracyVelN{10, 0, 0, 1, 0, 3, 1, 0, 0, 10};
const QVector<double> kAccuracyVelE{0, 3, 0, 0, 0, 3, 0, 0, 0, 0};
const QVector<double> kAccuracyVelD{0, 0, -10, 0, 0, 3, 0, 0, 0, 0};
const QVector<double> kSAccAnswers{2, 3, 4, 4, 4, 3, 3, 3, 0, 0};
/// The sample of kSAccAnswers that is exact only within 1e-12.
constexpr qsizetype kSAccInexact = 7;

/// The fit's outputs every fused accuracy reads: the two blocks and the
/// velocity, ten samples each.
QHash<QString, QVector<double>> accuracyChannels()
{
    QHash<QString, QVector<double>> channels = blockChannels("posCov", kPositionBlocks);
    channels.insert(blockChannels("velCov", kVelocityBlocks));
    channels.insert(velocityChannels(kAccuracyVelN, kAccuracyVelE, kAccuracyVelD));
    return channels;
}

/// Every sample of `actual` equals `expected` exactly, apart from the samples
/// in `within`, which may differ by 1e-12. Empty when they do, else the first
/// difference.
QString exactAnswers(const char *name, const QVector<double> &actual, const QVector<double> &expected,
                     const QList<qsizetype> &within = {})
{
    if (actual.size() != expected.size())
        return QStringLiteral("%1: %2 samples, expected %3").arg(name).arg(actual.size()).arg(expected.size());
    for (qsizetype i = 0; i < actual.size(); ++i) {
        const bool equal = within.contains(i) ? std::abs(actual[i] - expected[i]) <= 1e-12 : actual[i] == expected[i];
        if (!equal) {
            return QStringLiteral("%1[%2] = %3, expected %4")
                .arg(name).arg(i).arg(actual[i], 0, 'g', 17).arg(expected[i], 0, 'g', 17);
        }
    }
    return QString();
}

// ---- the attitude's hand-built expectations ---------------------------------------

constexpr double kDeg = 3.14159265358979323846 / 180.0;
/// Angles within this many degrees of their expectation pass (criterion 8).
constexpr double kAngleTolerance = 1e-9;

/// A quaternion, x, y, z, w (Hamilton).
struct Quat {
    double x, y, z, w;
};

Quat hamilton(const Quat &a, const Quat &b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

/// The body-to-north-east-down rotation of the aircraft Euler angles, in
/// degrees: heading about down, then pitch about the new right axis, then
/// roll about the forward axis.
Quat fromEuler(double heading, double pitch, double roll)
{
    const double ch = std::cos(heading * kDeg / 2), sh = std::sin(heading * kDeg / 2);
    const double cp = std::cos(pitch * kDeg / 2), sp = std::sin(pitch * kDeg / 2);
    const double cr = std::cos(roll * kDeg / 2), sr = std::sin(roll * kDeg / 2);
    return {sr * cp * ch - cr * sp * sh,
            cr * sp * ch + sr * cp * sh,
            cr * cp * sh - sr * sp * ch,
            cr * cp * ch + sr * sp * sh};
}

/// The default mount (forward +y, up +z), written by hand: the body's forward
/// axis is the device's y, right is x and down is -z. That rotation is half a
/// turn about (x + y)/sqrt 2 and its own inverse, so the device-to-NED
/// rotation of a body attitude B is B composed with it.
const Quat kDefaultMount{std::sqrt(0.5), std::sqrt(0.5), 0.0, 0.0};

/// The fit's quaternion of a device mounted in the default orientation on a
/// body with these angles.
Quat deviceQuaternion(double heading, double pitch, double roll)
{
    return hamilton(fromEuler(heading, pitch, roll), kDefaultMount);
}

/// The fit's quaternion channels of `samples`.
QHash<QString, QVector<double>> quaternionChannels(const QList<Quat> &samples)
{
    QHash<QString, QVector<double>> channels;
    for (const Quat &q : samples) {
        channels[QStringLiteral("qx")].append(q.x);
        channels[QStringLiteral("qy")].append(q.y);
        channels[QStringLiteral("qz")].append(q.z);
        channels[QStringLiteral("qw")].append(q.w);
    }
    return channels;
}

/// Stores a GNSS track on `session` whose raw course at fix j is courseDeg[j]
/// (at 10 m/s), one fix a second from kFixtureEpochUtc, and a course
/// reference before it. Only headingUnwrapsThroughAFullTurn uses it, to show
/// that neither the track nor the reference reaches the attitude.
void addGnssCourse(SessionData &session, const QVector<double> &courseDeg)
{
    QVector<double> time, velN, velE;
    for (qsizetype j = 0; j < courseDeg.size(); ++j) {
        time.append(kFixtureEpochUtc + double(j));
        velN.append(10.0 * std::cos(courseDeg[j] * kDeg));
        velE.append(10.0 * std::sin(courseDeg[j] * kDeg));
    }
    session.setSourceMeasurement("GNSS", "time", time, "s");
    session.setSourceMeasurement("GNSS", "velN", velN, "m/s");
    session.setSourceMeasurement("GNSS", "velE", velE, "m/s");
    session.setAttribute(SessionKeys::CourseRef, kFixtureEpochUtc - 1000.0);
}

/// A synthetic session with these quaternions and nothing else: the attitude
/// needs no GNSS data.
SessionData attitudeSession(const QString &id, const QList<Quat> &samples)
{
    return syntheticFitSession(id, quaternionChannels(samples));
}

QVector<double> heading(const SessionData &s) { return fusion(s, QStringLiteral("bodyHeading")); }
QVector<double> pitch(const SessionData &s) { return fusion(s, QStringLiteral("bodyPitch")); }
QVector<double> roll(const SessionData &s) { return fusion(s, QStringLiteral("bodyRoll")); }

/// Whether two angles agree within kAngleTolerance, modulo 360 (so that an
/// expected 180 accepts -180).
bool sameAngle(double got, double expected, double tolerance = kAngleTolerance)
{
    return std::abs(std::remainder(got - expected, 360.0)) <= tolerance;
}

/// Empty when the one-sample attitude of `session` is (h, p, r): heading and
/// roll modulo 360, pitch plainly, each within the tolerance.
QString attitudeDifference(const SessionData &session, double h, double p, double r)
{
    const QVector<double> hs = heading(session), ps = pitch(session), rs = roll(session);
    if (hs.size() != 1 || ps.size() != 1 || rs.size() != 1)
        return QStringLiteral("sizes %1, %2, %3").arg(hs.size()).arg(ps.size()).arg(rs.size());
    if (!sameAngle(hs[0], h) || std::abs(ps[0] - p) > kAngleTolerance || !sameAngle(rs[0], r)) {
        return QStringLiteral("got (%1, %2, %3), expected (%4, %5, %6)")
            .arg(hs[0], 0, 'g', 17).arg(ps[0], 0, 'g', 17).arg(rs[0], 0, 'g', 17)
            .arg(h).arg(p).arg(r);
    }
    return QString();
}

/// The unit vector of an axis text ("+x" ... "-z"), read here and not from
/// the orientation type.
std::array<int, 3> axisOf(const QString &text)
{
    std::array<int, 3> v{0, 0, 0};
    v[size_t(text.at(1).toLatin1() - 'x')] = text.at(0) == QLatin1Char('+') ? 1 : -1;
    return v;
}

// ---- the Orientation column ------------------------------------------------------------

const QString kDefaultLabel = QStringLiteral("forward +y, up +z");

/// The bulk edit task's activations on the model's scheduler (as tst_choice_attribute)
struct BulkEditSignals {
    explicit BulkEditSignals(SessionModel &model)
        : active(&model.scheduler(), &IdleScheduler::activeTaskChanged)
    {}
    int activations() const
    {
        int count = 0;
        for (const QList<QVariant> &args : active)
            count += args.at(0).toInt() == SessionModel::BulkEditTask ? 1 : 0;
        return count;
    }
    QSignalSpy active;
};

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
    void fusedSpeedsKnownAnswers();
    void fusedSpeedsAreTheGnssDefinitions();
    void fusedAccuraciesKnownAnswers();

    void orientationVocabularyHasTwentyFourPairs();
    void orientationRotationIsProper();
    void orientationDefinitionIsTheEnumeration();
    void attitudeRegistrationShape();
    void attitudeWaitsOnTheFit();
    void levelNorthFacingBodyReadsZero();
    void knownAnglesComeBack_data();
    void knownAnglesComeBack();
    void headingUnwrapsThroughAFullTurn();
    void rollAndPitchStayInTheirNaturalRanges();
    void forwardAxisVerticalGivesTheStandardFormulas();
    void sideMountPermutesTheAngles_data();
    void sideMountPermutesTheAngles();
    void deviceFrameMountGivesTheFitsOwnAngles();
    void invalidStoredOrientationMakesAttitudeUnavailable();
    void storedOrientationRecomputesWithoutAFit();

    void orientationColumnShowsTheDefaultWithoutAWrite_data();
    void orientationColumnShowsTheDefaultWithoutAWrite();
    void orientationEditStoresATokenAndRefusesOthers_data();
    void orientationEditStoresATokenAndRefusesOthers();
    void orientationBulkEdit_data();
    void orientationBulkEdit();

private:
    static void addRowKinds()
    {
        QTest::addColumn<bool>("stubs");
        QTest::newRow("loaded") << false;
        QTest::newRow("stubs") << true;
    }
    /// Two fixture sessions, o1 storing `o1Token` when given and o2 nothing,
    /// in a SessionModel over a fresh logbook with the Orientation column,
    /// loaded or as stubs after a restart. Empty on success.
    [[nodiscard]] QString startOrientationWorld(bool stubs, const std::optional<QString> &o1Token = std::nullopt);
    static QByteArray fileBytes(const QString &id) { return readFileBytes(sessionFilePath(id)); }

    QStringList m_registryBefore;
    std::unique_ptr<ChoiceFixture> m_fixture;
};

void FusionDerivedTest::initTestCase()
{
    // As the application does: the built-ins, then sensor fusion
    TestEnvironment::instance().registerBuiltIns();
    registerFusionOnce();
    // For the Orientation column's tests, as every suite that sets columns
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumnStore::instance().setColumns({descriptionColumn()});
}

void FusionDerivedTest::init()
{
    TestEnvironment::instance().resetPreferencesToDefaults();
    m_registryBefore = CalculationRegistry::instance().registeredIds();
}

// The model goes before the checks
void FusionDerivedTest::cleanup()
{
    m_fixture.reset();
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

// The premise of every synthetic test below: stored fit outputs are what a
// reader gets, bit for bit, and nothing runs the fit to get them.
void FusionDerivedTest::syntheticOutputsAreServedWithoutAFit()
{
    QHash<QString, QVector<double>> channels;
    const QStringList names = fusionMeasurementNames();
    QCOMPARE(names.size(), 33);
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

    // The speeds after the fit, velH before vel, which reads it, then accH
    // and the fused accuracies; the rest in this order after the system time.
    // The whole tail is pinned by tst_fusion_session::registrationShape
    const QStringList ids = registry.registeredIds();
    const qsizetype fit = ids.indexOf(kFit);
    QVERIFY(fit >= 0);
    QCOMPARE(ids.mid(fit + 1, 6), QStringList({kVelH, kVel, kAccH, kHAcc, kVAcc, kSAcc}));
    const qsizetype systemTime = ids.indexOf(kSystemTime);
    QVERIFY(systemTime >= 0);
    QCOMPARE(ids.mid(systemTime + 1, 3), QStringList({kZ, kAlong, kCross}));

    const struct {
        const QString &id;
        const char *output;
        QList<CalcInput> inputs;
    } expected[] = {
        {kVelH, "velH", {CalcInput::measurement("Fusion", "velN"), CalcInput::measurement("Fusion", "velE")}},
        {kVel, "vel", {CalcInput::measurement("Fusion", "velH"), CalcInput::measurement("Fusion", "velD")}},
        {kHAcc, "hAcc",
         {CalcInput::measurement("Fusion", "posCovNN"), CalcInput::measurement("Fusion", "posCovNE"),
          CalcInput::measurement("Fusion", "posCovEE")}},
        {kVAcc, "vAcc", {CalcInput::measurement("Fusion", "posCovDD")}},
        {kSAcc, "sAcc",
         {CalcInput::measurement("Fusion", "velCovNN"), CalcInput::measurement("Fusion", "velCovNE"),
          CalcInput::measurement("Fusion", "velCovND"), CalcInput::measurement("Fusion", "velCovEE"),
          CalcInput::measurement("Fusion", "velCovED"), CalcInput::measurement("Fusion", "velCovDD"),
          CalcInput::measurement("Fusion", "velN"), CalcInput::measurement("Fusion", "velE"),
          CalcInput::measurement("Fusion", "velD")}},
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
    // Vertical speed is the fit's own velD: no second producer
    QCOMPARE(registry.candidatesFor(fusionKey(QStringLiteral("velD"))).size(), 1);
    QCOMPARE(registry.candidatesFor(fusionKey(QStringLiteral("velD"))).first().instanceId, kFit);
}

// The session half of criterion 2: with the fit's inputs and no fit, each
// derived value waits on the fit and on nothing else, inspection says so, and
// nothing starts it.
void FusionDerivedTest::derivedValuesWaitOnTheFit()
{
    SessionData session = fixtureSession(QStringLiteral("coarse_linear"));
    session.setAttribute(SessionKeys::GroundElev, 50.0);
    CalculationEngine &engine = session.calculationEngine();

    for (const char *name : {"velH", "vel", "hAcc", "vAcc", "sAcc", "z", "accAlongTrack", "accCrossTrack"}) {
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

// The fused speeds: exact answers on chosen velocities, read without the fit,
// and unavailable when the inputs' lengths differ. A short velE takes velH and
// with it vel; a short velD takes vel alone.
void FusionDerivedTest::fusedSpeedsKnownAnswers()
{
    {
        const SessionData session = syntheticFitSession(QStringLiteral("v1"), velocityChannels(kVelN, kVelE, kVelD));
        QCOMPARE(fusion(session, QStringLiteral("velH")), kSpeedH);
        QCOMPARE(fusion(session, QStringLiteral("vel")), kSpeed);
        // Vertical speed is the fit's own channel, served as stored
        QVERIFY(sameBitsEverywhere(fusion(session, QStringLiteral("velD")), kVelD));
        QCOMPARE(session.calculationEngine().runCount(kVelH), 1);
        QCOMPARE(session.calculationEngine().runCount(kVel), 1);
        QCOMPARE(session.calculationEngine().runCount(kFit), 0);
        QCOMPARE(session.calculationEngine().undeclaredReadCount(), 0);
    }

    // A short velE: neither speed
    {
        QVector<double> shortE = kVelE;
        shortE.removeLast();
        const SessionData session = syntheticFitSession(QStringLiteral("v2"), velocityChannels(kVelN, shortE, kVelD));
        QVERIFY(fusion(session, QStringLiteral("velH")).isEmpty());
        QVERIFY(fusion(session, QStringLiteral("vel")).isEmpty());
        QCOMPARE(session.calculationEngine().runCount(kVelH), 1);
        QCOMPARE(session.calculationEngine().runCount(kFit), 0);
    }

    // A short velD: the horizontal speed stands, the total is unavailable
    {
        QVector<double> shortD = kVelD;
        shortD.removeLast();
        const SessionData session = syntheticFitSession(QStringLiteral("v3"), velocityChannels(kVelN, kVelE, shortD));
        QCOMPARE(fusion(session, QStringLiteral("velH")), kSpeedH);
        QVERIFY(fusion(session, QStringLiteral("vel")).isEmpty());
        QCOMPARE(session.calculationEngine().runCount(kVel), 1);
        QCOMPARE(session.calculationEngine().runCount(kFit), 0);
    }
}

// One definition: the same velocities under GNSS and Fusion give the same
// horizontal and total speeds. flysight_fusion is compiled without
// contraction and flysight_core is not, so on a contracting compiler the two
// copies of sqrt(a*a + b*b) may round the sum differently. Every term is a
// square, so the sum never cancels and the difference stays within the 4 ulp
// of sameRecomputedValue(); in exact mode the two are bit for bit.
void FusionDerivedTest::fusedSpeedsAreTheGnssDefinitions()
{
    constexpr int n = 60;
    QVector<double> velN, velE, velD;
    for (int i = 0; i < n; ++i) {
        velN.append(20.0 + 5.0 * std::sin(i * 0.1));
        velE.append(-3.0 + 2.0 * std::cos(i * 0.13));
        velD.append(15.0 + 3.0 * std::sin(i * 0.07));
    }
    const QHash<QString, QVector<double>> channels = velocityChannels(velN, velE, velD);

    SessionData session = syntheticFitSession(QStringLiteral("g2"), channels);
    for (const char *name : {"velN", "velE", "velD"})
        session.setSourceMeasurement("GNSS", name, channels.value(QString::fromLatin1(name)), "m/s");

    // Stored GNSS data is what the GNSS speeds read
    for (const char *name : {"velN", "velE", "velD"}) {
        QVERIFY2(sameBitsEverywhere(session.getMeasurement("GNSS", name), channels.value(QString::fromLatin1(name))),
                 name);
    }

    const QVector<double> gnssH = session.getMeasurement("GNSS", "velH");
    const QVector<double> gnssTotal = session.getMeasurement("GNSS", "vel");
    const QVector<double> fusedH = fusion(session, QStringLiteral("velH"));
    const QVector<double> fusedTotal = fusion(session, QStringLiteral("vel"));
    QCOMPARE(gnssH.size(), n);
    QCOMPARE(gnssTotal.size(), n);
    QCOMPARE(fusedH.size(), n);
    QCOMPARE(fusedTotal.size(), n);

    for (int i = 0; i < n; ++i) {
        QVERIFY2(sameRecomputedValue(fusedH[i], gnssH[i]),
                 qPrintable(QStringLiteral("velH[%1]: %2 vs %3")
                                .arg(i).arg(fusedH[i], 0, 'g', 17).arg(gnssH[i], 0, 'g', 17)));
        QVERIFY2(sameRecomputedValue(fusedTotal[i], gnssTotal[i]),
                 qPrintable(QStringLiteral("vel[%1]: %2 vs %3")
                                .arg(i).arg(fusedTotal[i], 0, 'g', 17).arg(gnssTotal[i], 0, 'g', 17)));
    }

    QCOMPARE(session.calculationEngine().runCount(kFit), 0);
    QCOMPARE(session.calculationEngine().undeclaredReadCount(), 0);
}

// The fused accuracies (item 1708): exact answers on chosen blocks, read
// without the fit, and unavailable without the blocks and on unequal lengths.
// The speed accuracy's rule is the horizontal acceleration accuracy's: a
// velocity at or above its along-track sigma gives that sigma, one below it
// and a zero velocity give the largest eigenvalue, and a full block exercises
// the 3x3 form on both branches. The rule lives in fitcovariance.cpp too, which
// this executable may not include, so the known answers are what hold it.
void FusionDerivedTest::fusedAccuraciesKnownAnswers()
{
    const auto hAcc = [](const SessionData &s) { return fusion(s, QStringLiteral("hAcc")); };
    const auto vAcc = [](const SessionData &s) { return fusion(s, QStringLiteral("vAcc")); };
    const auto sAcc = [](const SessionData &s) { return fusion(s, QStringLiteral("sAcc")); };

    {
        const SessionData session = syntheticFitSession(QStringLiteral("a1"), accuracyChannels());
        QString difference = exactAnswers("hAcc", hAcc(session), kHAccAnswers);
        QVERIFY2(difference.isEmpty(), qPrintable(difference));
        difference = exactAnswers("vAcc", vAcc(session), kVAccAnswers);
        QVERIFY2(difference.isEmpty(), qPrintable(difference));
        difference = exactAnswers("sAcc", sAcc(session), kSAccAnswers, {kSAccInexact});
        QVERIFY2(difference.isEmpty(), qPrintable(difference));
        CalculationEngine &engine = session.calculationEngine();
        QCOMPARE(engine.runCount(kHAcc), 1);
        QCOMPARE(engine.runCount(kVAcc), 1);
        QCOMPARE(engine.runCount(kSAcc), 1);
        QCOMPARE(engine.runCount(kFit), 0);
        QCOMPARE(engine.undeclaredReadCount(), 0);
    }

    // The velocity without its covariance (no fit, or a fit whose covariance
    // failed): no speed accuracy, and nothing starts the fit
    {
        const SessionData session = syntheticFitSession(
            QStringLiteral("a2"), velocityChannels(kAccuracyVelN, kAccuracyVelE, kAccuracyVelD));
        QVERIFY(sAcc(session).isEmpty());
        QVERIFY(hAcc(session).isEmpty());
        QVERIFY(vAcc(session).isEmpty());
        QCOMPARE(session.calculationEngine().runCount(kFit), 0);
        QCOMPARE(session.calculationEngine().undeclaredReadCount(), 0);
    }

    // The down variance alone: the vertical accuracy and no horizontal one
    {
        const SessionData session = syntheticFitSession(
            QStringLiteral("a3"), {{QStringLiteral("posCovDD"), blockChannels("posCov", kPositionBlocks)
                                                                     .value(QStringLiteral("posCovDD"))}});
        QString difference = exactAnswers("vAcc", vAcc(session), kVAccAnswers);
        QVERIFY2(difference.isEmpty(), qPrintable(difference));
        QVERIFY(hAcc(session).isEmpty());
        QVERIFY(sAcc(session).isEmpty());
        QCOMPARE(session.calculationEngine().runCount(kFit), 0);
    }

    // One entry one sample short: the calculation that reads it is
    // unavailable, and the others are as they were
    const struct {
        const char *shortened;
        bool hAccServed, sAccServed;
    } shortCases[] = {
        {"posCovEE", false, true},
        {"posCovNE", false, true},
        {"velCovED", true, false},
        {"velCovNN", true, false},
        {"velD", true, false},
    };
    for (const auto &c : shortCases) {
        QHash<QString, QVector<double>> channels = accuracyChannels();
        channels[QString::fromLatin1(c.shortened)].removeLast();
        const SessionData session = syntheticFitSession(QStringLiteral("a4"), channels);

        QVERIFY2(hAcc(session).isEmpty() != c.hAccServed, c.shortened);
        if (c.hAccServed) {
            const QString difference = exactAnswers("hAcc", hAcc(session), kHAccAnswers);
            QVERIFY2(difference.isEmpty(), qPrintable(difference));
        }
        QVERIFY2(sAcc(session).isEmpty() != c.sAccServed, c.shortened);
        if (c.sAccServed) {
            const QString difference = exactAnswers("sAcc", sAcc(session), kSAccAnswers, {kSAccInexact});
            QVERIFY2(difference.isEmpty(), qPrintable(difference));
        }
        const QString difference = exactAnswers("vAcc", vAcc(session), kVAccAnswers);
        QVERIFY2(difference.isEmpty(), qPrintable(difference + QLatin1Char(' ') + QLatin1String(c.shortened)));
        QCOMPARE(session.calculationEngine().runCount(kFit), 0);
        QCOMPARE(session.calculationEngine().undeclaredReadCount(), 0);
    }
}

// ---- The orientation vocabulary --------------------------------------------------------

// Criterion 1: 24 distinct pairs in the enumeration order of D1, the default
// first; tokens and labels distinct, each token parsing back to its pair, and
// nothing else parsing.
void FusionDerivedTest::orientationVocabularyHasTwentyFourPairs()
{
    using Fusion::Orientation;
    const std::vector<Orientation> &all = Orientation::all();
    QCOMPARE(all.size(), size_t(24));

    QStringList tokens;
    QStringList labels;
    for (const Orientation &orientation : all) {
        tokens.append(orientation.token());
        labels.append(orientation.label());
    }
    QCOMPARE(tokens, QStringList({QStringLiteral("+y,+z"),
                                  QStringLiteral("+x,+y"), QStringLiteral("+x,-y"), QStringLiteral("+x,+z"),
                                  QStringLiteral("+x,-z"),
                                  QStringLiteral("-x,+y"), QStringLiteral("-x,-y"), QStringLiteral("-x,+z"),
                                  QStringLiteral("-x,-z"),
                                  QStringLiteral("+y,+x"), QStringLiteral("+y,-x"), QStringLiteral("+y,-z"),
                                  QStringLiteral("-y,+x"), QStringLiteral("-y,-x"), QStringLiteral("-y,+z"),
                                  QStringLiteral("-y,-z"),
                                  QStringLiteral("+z,+x"), QStringLiteral("+z,-x"), QStringLiteral("+z,+y"),
                                  QStringLiteral("+z,-y"),
                                  QStringLiteral("-z,+x"), QStringLiteral("-z,-x"), QStringLiteral("-z,+y"),
                                  QStringLiteral("-z,-y")}));
    QCOMPARE(QSet<QString>(tokens.cbegin(), tokens.cend()).size(), 24);
    QCOMPARE(QSet<QString>(labels.cbegin(), labels.cend()).size(), 24);

    for (size_t i = 0; i < all.size(); ++i) {
        const QString &token = tokens.at(qsizetype(i));
        // The label spells the token's axes, ASCII hyphen-minus included
        const QStringList axes = token.split(QLatin1Char(','));
        QCOMPARE(labels.at(qsizetype(i)), QStringLiteral("forward %1, up %2").arg(axes.at(0), axes.at(1)));
        // Forward and up neither equal nor opposite
        QVERIFY2(axes.at(0).at(1) != axes.at(1).at(1), qPrintable(token));
        // Distinct pairs
        for (size_t j = i + 1; j < all.size(); ++j)
            QVERIFY(all[i] != all[j]);
        // Parses back to its pair
        const std::optional<Orientation> parsed = Orientation::fromToken(token);
        QVERIFY2(parsed.has_value(), qPrintable(token));
        QVERIFY(*parsed == all[i]);
        QCOMPARE(parsed->token(), token);
    }

    QVERIFY(all.front() == Orientation::defaultOrientation());
    QCOMPARE(Orientation::defaultOrientation().token(), QStringLiteral("+y,+z"));
    QCOMPARE(Orientation::defaultOrientation().label(), QStringLiteral("forward +y, up +z"));

    for (const QString &text : {QStringLiteral("+y,+y"), QStringLiteral("+y,-y"), QStringLiteral("-z,+z"),
                                QStringLiteral("y,z"), QStringLiteral("+y, +z"), QStringLiteral("+Y,+Z"),
                                QString(), QStringLiteral(""), QStringLiteral(" +y,+z"), QStringLiteral("+y,+z "),
                                QStringLiteral("+y;+z"), QStringLiteral("+y,+z,+x"),
                                QStringLiteral("forward +y, up +z"),
                                QString(QChar(0x2212)) + QStringLiteral("y,+z")})
        QVERIFY2(!Orientation::fromToken(text).has_value(), qPrintable(text));
}

// Criterion 2: for every pair an exact proper rotation whose columns are
// forward, forward x up and -up (the axes read from the token here), and the
// default's matrix exactly.
void FusionDerivedTest::orientationRotationIsProper()
{
    using Fusion::Orientation;
    for (const Orientation &orientation : Orientation::all()) {
        const QString token = orientation.token();
        const QStringList axes = token.split(QLatin1Char(','));
        const std::array<int, 3> f = axisOf(axes.at(0));
        const std::array<int, 3> u = axisOf(axes.at(1));
        const std::array<int, 3> right{f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2],
                                       f[0] * u[1] - f[1] * u[0]};
        const Orientation::Matrix m = orientation.bodyToDevice();

        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                const double v = m[r][c];
                QVERIFY2(v == -1.0 || v == 0.0 || v == 1.0, qPrintable(token));
            }
            QVERIFY2(m[r][0] == f[r], qPrintable(token));
            QVERIFY2(m[r][1] == right[r], qPrintable(token));
            QVERIFY2(m[r][2] == -u[r], qPrintable(token));
        }
        // Orthonormal
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                const double dot = m[0][i] * m[0][j] + m[1][i] * m[1][j] + m[2][i] * m[2][j];
                QVERIFY2(dot == (i == j ? 1.0 : 0.0), qPrintable(token));
            }
        }
        // A rotation, never a reflection
        const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
                         - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                         + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        QVERIFY2(det == 1.0, qPrintable(token));
    }

    // Forward +y, up +z: columns (0,1,0), (1,0,0), (0,0,-1)
    const Orientation::Matrix expected{{{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}};
    QVERIFY(Orientation::defaultOrientation().bodyToDevice() == expected);
}

// Criterion 4: exactly one definition, its fields, its choices the
// enumeration token for token and label for label, also after the entry
// point registers on another registry (which gets the constant default).
void FusionDerivedTest::orientationDefinitionIsTheEnumeration()
{
    const QString key = QStringLiteral("_ORIENTATION");
    const auto check = [&key]() -> QString {
        int count = 0;
        for (const AttributeDefinition &definition : AttributeRegistry::instance().allAttributes())
            count += definition.attributeKey == key ? 1 : 0;
        if (count != 1)
            return QStringLiteral("%1 definitions").arg(count);

        const AttributeDefinition *definition = AttributeRegistry::instance().findByKey(key);
        if (definition->category != QLatin1String("Session") || definition->displayName != QLatin1String("Orientation")
            || definition->formatType != AttributeFormatType::Choice || !definition->editable
            || !definition->measurementType.isEmpty())
            return QStringLiteral("the definition's fields");

        const std::vector<Fusion::Orientation> &all = Fusion::Orientation::all();
        if (definition->choices.size() != qsizetype(all.size()))
            return QStringLiteral("%1 choices").arg(definition->choices.size());
        for (size_t i = 0; i < all.size(); ++i) {
            const AttributeChoice &choice = definition->choices.at(qsizetype(i));
            if (choice.token != all[i].token() || choice.label != all[i].label())
                return QStringLiteral("choice %1: %2 / %3").arg(i).arg(choice.token, choice.label);
        }
        return QString();
    };

    QCOMPARE(check(), QString());
    QCOMPARE(AttributeRegistry::instance().findByKey(key)->choices.first().token, QStringLiteral("+y,+z"));
    QCOMPARE(AttributeRegistry::instance().findByKey(key)->choices.first().label, QStringLiteral("forward +y, up +z"));

    {
        CalculationRegistry privateRegistry;
        Fusion::registerFusionCalculations(privateRegistry);
        QVERIFY(privateRegistry.contains(kOrientationDefault));
        QVERIFY(privateRegistry.contains(kAttitude));
    }
    QCOMPARE(check(), QString());
}

// ---- The attitude --------------------------------------------------------------------

// Criteria 5 (the registry half) and 6: the constant default and the attitude
// as registered, in that order after the track accelerations.
void FusionDerivedTest::attitudeRegistrationShape()
{
    const CalculationRegistry &registry = CalculationRegistry::instance();

    const QStringList ids = registry.registeredIds();
    QVERIFY(ids.indexOf(kCross) >= 0);
    QVERIFY(ids.indexOf(kCross) < ids.indexOf(kOrientationDefault));
    QVERIFY(ids.indexOf(kOrientationDefault) < ids.indexOf(kAttitude));

    // The constant default: no inputs, the attribute its one output, a string
    const DependencyKey orientationKey = DependencyKey::attribute(QStringLiteral("_ORIENTATION"));
    const std::optional<CalculationInstance> constant = registry.instance(kOrientationDefault);
    QVERIFY(constant.has_value());
    QVERIFY(constant->descriptor->inputs.isEmpty());
    QVERIFY(constant->descriptor->outputs == QList<DependencyKey>({orientationKey}));
    QVERIFY(constant->descriptor->policy == EvaluationPolicy::OnDemand);
    QCOMPARE(registry.candidatesFor(orientationKey).size(), 1);
    QCOMPARE(registry.candidatesFor(orientationKey).first().instanceId, kOrientationDefault);
    QVERIFY(registry.explicitDependencies(orientationKey).isEmpty());
    {
        const SessionData bare;
        const QVariant value = bare.getAttribute(QStringLiteral("_ORIENTATION"));
        QCOMPARE(value.typeId(), int(QMetaType::QString));
        QCOMPARE(value.toString(), QStringLiteral("+y,+z"));
    }

    // The attitude
    const std::optional<CalculationInstance> attitude = registry.instance(kAttitude);
    QVERIFY(attitude.has_value());
    const CalculationDescriptor &d = *attitude->descriptor;
    QVERIFY(d.policy == EvaluationPolicy::OnDemand);
    QVERIFY(d.title.isEmpty());
    QVERIFY(d.resultVersion.isEmpty());
    // The quaternion and the orientation, and nothing of GNSS: a compass
    // heading is not referenced to the course reference
    QVERIFY(d.inputs == QList<CalcInput>({CalcInput::measurement("Fusion", "qx"), CalcInput::measurement("Fusion", "qy"),
                                          CalcInput::measurement("Fusion", "qz"), CalcInput::measurement("Fusion", "qw"),
                                          CalcInput::attribute("_ORIENTATION")}));

    const QList<DependencyKey> outputs{fusionKey(QStringLiteral("bodyHeading")), fusionKey(QStringLiteral("bodyPitch")),
                                       fusionKey(QStringLiteral("bodyRoll"))};
    QVERIFY(d.outputs == outputs);
    for (const DependencyKey &output : outputs) {
        const QList<CalculationInstance> candidates = registry.candidatesFor(output);
        QCOMPARE(candidates.size(), 1);
        QCOMPARE(candidates.first().instanceId, kAttitude);
        QCOMPARE(registry.explicitDependencies(output), QStringList({kFit}));
    }
}

// Criterion 7: with the fit's inputs (and the GNSS velocity the heading
// reference reads, which fixtureSession() stores) and no fit, each angle waits
// on the fit alone, and nothing starts it.
void FusionDerivedTest::attitudeWaitsOnTheFit()
{
    SessionData session = fixtureSession(QStringLiteral("coarse_linear"));
    QVERIFY(!session.getMeasurement("GNSS", "course").isEmpty());
    CalculationEngine &engine = session.calculationEngine();

    for (const char *name : {"bodyHeading", "bodyPitch", "bodyRoll"}) {
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

// Criterion 8, first case: under the default orientation, a device with y
// north, x east and z up (either sign of its quaternion) is a level,
// north-facing body.
void FusionDerivedTest::levelNorthFacingBodyReadsZero()
{
    const double h = std::sqrt(0.5);
    const Quat level = deviceQuaternion(0, 0, 0);
    QVERIFY(level.x == h && level.y == h && level.z == 0.0 && level.w == 0.0);

    for (const Quat &q : {Quat{h, h, 0.0, 0.0}, Quat{-h, -h, 0.0, 0.0}}) {
        const SessionData session = attitudeSession(QStringLiteral("l1"), {q});
        QVERIFY(!session.hasAttribute(SessionKeys::Orientation));
        QCOMPARE(attitudeDifference(session, 0, 0, 0), QString());
        QCOMPARE(session.calculationEngine().runCount(kFit), 0);
        QCOMPARE(session.calculationEngine().undeclaredReadCount(), 0);
    }
}

void FusionDerivedTest::knownAnglesComeBack_data()
{
    QTest::addColumn<double>("h");
    QTest::addColumn<double>("p");
    QTest::addColumn<double>("r");
    QTest::newRow("heading 179") << 179.0 << 0.0 << 0.0;
    QTest::newRow("heading -179") << -179.0 << 0.0 << 0.0;
    QTest::newRow("heading 180") << 180.0 << 0.0 << 0.0;
    QTest::newRow("heading 90") << 90.0 << 0.0 << 0.0;
    QTest::newRow("pitch 80") << 0.0 << 80.0 << 0.0;
    QTest::newRow("pitch -80") << 0.0 << -80.0 << 0.0;
    QTest::newRow("roll 170") << 0.0 << 0.0 << 170.0;
    QTest::newRow("roll -170") << 0.0 << 0.0 << -170.0;
    QTest::newRow("roll 180") << 0.0 << 0.0 << 180.0;
    QTest::newRow("179, 80, 170") << 179.0 << 80.0 << 170.0;
    QTest::newRow("-179, -80, -170") << -179.0 << -80.0 << -170.0;
    QTest::newRow("-179, 80, -170") << -179.0 << 80.0 << -170.0;
    QTest::newRow("37.5, -12.25, 101") << 37.5 << -12.25 << 101.0;
}

// Criterion 8: a body built by hand from heading, pitch and roll reads them
// back under the default orientation.
void FusionDerivedTest::knownAnglesComeBack()
{
    QFETCH(double, h);
    QFETCH(double, p);
    QFETCH(double, r);
    const SessionData session = attitudeSession(QStringLiteral("k1"), {deviceQuaternion(h, p, r)});
    QCOMPARE(attitudeDifference(session, h, p, r), QString());
    QCOMPARE(session.calculationEngine().runCount(kFit), 0);
}

// Criterion 8: two full turns of heading read continuous, the one unwrap rule
// over the wrapped headings, measured from north. Heading is a compass
// heading: a GNSS track and a course reference, inside or outside its time
// range, stored as a number, as text or not at all, change nothing, and the
// attitude is available without any GNSS data.
void FusionDerivedTest::headingUnwrapsThroughAFullTurn()
{
    QList<Quat> turn;
    QVector<double> wrapped;
    for (int k = 0; k <= 72; ++k) {
        turn.append(deviceQuaternion(10.0 * k, 0, 0));
        wrapped.append(std::remainder(10.0 * k, 360.0));
    }
    const QVector<double> unwrapped = Calculations::unwrapDegrees(wrapped);

    // No GNSS data at all: heading from north, pitch and roll beside it
    const SessionData bare = attitudeSession(QStringLiteral("h1"), turn);
    QVERIFY(bare.getMeasurement("GNSS", "course").isEmpty());
    const QVector<double> h0 = heading(bare);
    QCOMPARE(h0.size(), turn.size());
    for (int k = 0; k <= 72; ++k) {
        QVERIFY2(std::abs(h0[k] - unwrapped[k]) <= kAngleTolerance, qPrintable(QString::number(k)));
        QVERIFY2(std::abs(h0[k] - 10.0 * k) <= kAngleTolerance, qPrintable(QString::number(k)));
        QVERIFY(std::abs(pitch(bare)[k]) <= kAngleTolerance);
        QVERIFY(sameAngle(roll(bare)[k], 0.0));
    }

    // A GNSS track with a course, and a course reference of every kind: the
    // course is offset by its reference, the heading by nothing
    const QVector<double> rawCourse{20, 30, 40, 50, 60};
    for (const QVariant &courseRef : {QVariant(kFixtureEpochUtc + 1.5), QVariant(kFixtureEpochUtc - 0.5),
                                      QVariant(kFixtureEpochUtc + 100.0), QVariant(QStringLiteral("later")),
                                      QVariant(qQNaN()), QVariant()}) {
        SessionData s = attitudeSession(QStringLiteral("h2"), turn);
        addGnssCourse(s, rawCourse);
        if (courseRef.isValid())
            s.setAttribute(SessionKeys::CourseRef, courseRef);
        else
            s.removeAttribute(SessionKeys::CourseRef);
        QCOMPARE(s.getMeasurement("GNSS", "course").isEmpty(), !courseRef.isValid());
        QVERIFY2(sameBitsEverywhere(heading(s), h0), qPrintable(courseRef.toString()));
        QCOMPARE(pitch(s).size(), turn.size());
        QCOMPARE(roll(s).size(), turn.size());
    }

    QCOMPARE(bare.calculationEngine().runCount(kFit), 0);
    QCOMPARE(bare.calculationEngine().undeclaredReadCount(), 0);
}

// Criterion 8: a barrel roll keeps roll in (-180, 180] and a loop keeps pitch
// in [-90, 90], each reading the angle it was built from, modulo 360.
void FusionDerivedTest::rollAndPitchStayInTheirNaturalRanges()
{
    // A barrel roll through two turns at heading 30
    QList<Quat> barrel;
    for (int k = 0; k <= 72; ++k)
        barrel.append(deviceQuaternion(30.0, 0.0, 10.0 * k));
    const SessionData rolling = attitudeSession(QStringLiteral("b1"), barrel);
    const QVector<double> rolls = roll(rolling);
    QCOMPARE(rolls.size(), barrel.size());
    for (int k = 0; k <= 72; ++k) {
        QVERIFY2(rolls[k] > -180.0 && rolls[k] <= 180.0, qPrintable(QString::number(rolls[k], 'g', 17)));
        QVERIFY2(sameAngle(rolls[k], 10.0 * k), qPrintable(QString::number(k)));
        QVERIFY(std::abs(pitch(rolling)[k]) <= kAngleTolerance);
        QVERIFY(std::abs(heading(rolling)[k] - 30.0) <= kAngleTolerance);
    }

    // A loop: the body turned about its right axis by a = 5, 15, ..., 355
    // degrees, never exactly vertical (heading and roll are not defined there)
    QList<Quat> loop;
    QVector<double> angles;
    for (int k = 0; k < 36; ++k) {
        const double a = 5.0 + 10.0 * k;
        angles.append(a);
        loop.append(hamilton(Quat{0.0, std::sin(a * kDeg / 2), 0.0, std::cos(a * kDeg / 2)}, kDefaultMount));
    }
    const SessionData looping = attitudeSession(QStringLiteral("b2"), loop);
    const QVector<double> pitches = pitch(looping);
    QCOMPARE(pitches.size(), loop.size());
    for (int k = 0; k < 36; ++k) {
        const double a = angles[k];
        // Past the vertical the body is inverted: heading and roll turn by half a turn
        const bool inverted = a > 90.0 && a < 270.0;
        const double expectedPitch = a <= 90.0 ? a : (inverted ? 180.0 - a : a - 360.0);
        QVERIFY2(pitches[k] >= -90.0 && pitches[k] <= 90.0, qPrintable(QString::number(pitches[k], 'g', 17)));
        QVERIFY2(std::abs(pitches[k] - expectedPitch) <= kAngleTolerance, qPrintable(QString::number(a)));
        QVERIFY2(sameAngle(heading(looping)[k], inverted ? 180.0 : 0.0), qPrintable(QString::number(a)));
        QVERIFY2(sameAngle(roll(looping)[k], inverted ? 180.0 : 0.0), qPrintable(QString::number(a)));
        QVERIFY(roll(looping)[k] > -180.0 && roll(looping)[k] <= 180.0);
    }
}

// Criterion 8, the vertical clause: where the forward axis is vertical,
// heading and roll are not defined and the derivation reports what the
// standard formulas give. Whatever those are, all three angles are published,
// finite, one per sample, pitch is exactly vertical, roll is in its range, and
// the samples after a vertical one still read their own angles: one NaN would
// reach every later heading through the unwrap.
//
// The vertical quaternions are exact on every platform. The loop's
// construction at a = 90 and a = 270 is written with sqrt(0.5) for the
// half-angle sine and cosine, and composed with the default mount each of its
// components is a single product, so contraction in this file cannot change
// its bits. The level device under a vertical mount is the sample for which
// rounding takes -M20 past 1 (by 4e-16): without the clamp before the arcsine
// its pitch is NaN.
void FusionDerivedTest::forwardAxisVerticalGivesTheStandardFormulas()
{
    // Empty when all three angles of `session` are published, `n` values
    // each, and every value is finite with roll in (-180, 180]
    const auto published = [](const SessionData &session, qsizetype n) -> QString {
        const QVector<double> hs = heading(session), ps = pitch(session), rs = roll(session);
        if (hs.size() != n || ps.size() != n || rs.size() != n)
            return QStringLiteral("sizes %1, %2, %3 of %4").arg(hs.size()).arg(ps.size()).arg(rs.size()).arg(n);
        for (qsizetype i = 0; i < n; ++i) {
            if (!std::isfinite(hs[i]) || !std::isfinite(ps[i]) || !std::isfinite(rs[i]))
                return QStringLiteral("sample %1 not finite: (%2, %3, %4)").arg(i).arg(hs[i]).arg(ps[i]).arg(rs[i]);
            if (!(rs[i] > -180.0 && rs[i] <= 180.0))
                return QStringLiteral("sample %1 roll %2").arg(i).arg(rs[i], 0, 'g', 17);
        }
        return QString();
    };

    // The default orientation: the body pitched exactly up and exactly down,
    // between ordinary samples
    const double s = std::sqrt(0.5);
    const Quat up = hamilton(Quat{0.0, s, 0.0, s}, kDefaultMount);
    const Quat down = hamilton(Quat{0.0, s, 0.0, -s}, kDefaultMount);
    const double r = s * s;
    QVERIFY(up.x == r && up.y == r && up.z == -r && up.w == -r);
    QVERIFY(down.x == -r && down.y == -r && down.z == -r && down.w == -r);

    struct Ordinary {
        qsizetype index;
        double h, p, r;
    };
    const Ordinary ordinary[] = {{0, 20.0, 10.0, 5.0}, {2, -40.0, -20.0, 15.0}, {4, 100.0, 35.0, -120.0}};
    const QList<Quat> samples{deviceQuaternion(20.0, 10.0, 5.0), up, deviceQuaternion(-40.0, -20.0, 15.0), down,
                              deviceQuaternion(100.0, 35.0, -120.0)};
    const SessionData session = attitudeSession(QStringLiteral("v1"), samples);
    QCOMPARE(published(session, samples.size()), QString());
    const QVector<double> hs = heading(session), ps = pitch(session), rs = roll(session);
    QVERIFY2(std::abs(ps[1] - 90.0) <= kAngleTolerance, qPrintable(QString::number(ps[1], 'g', 17)));
    QVERIFY2(std::abs(ps[3] + 90.0) <= kAngleTolerance, qPrintable(QString::number(ps[3], 'g', 17)));
    // Heading modulo 360: the unwrap may carry a turn across the vertical
    for (const Ordinary &o : ordinary) {
        QVERIFY2(sameAngle(hs[o.index], o.h), qPrintable(QString::number(o.index)));
        QVERIFY2(std::abs(ps[o.index] - o.p) <= kAngleTolerance, qPrintable(QString::number(o.index)));
        QVERIFY2(sameAngle(rs[o.index], o.r), qPrintable(QString::number(o.index)));
    }
    QCOMPARE(session.calculationEngine().runCount(kFit), 0);
    QCOMPARE(session.calculationEngine().undeclaredReadCount(), 0);

    // A level device mounted with its forward axis straight up and straight
    // down
    const struct {
        const char *token;
        double p;
    } mounts[] = {{"+z,+y", 90.0}, {"-z,+y", -90.0}};
    for (const auto &mount : mounts) {
        SessionData vertical = attitudeSession(QStringLiteral("v2"), {deviceQuaternion(0.0, 0.0, 0.0)});
        vertical.setAttribute(SessionKeys::Orientation, QString::fromLatin1(mount.token));
        QVERIFY2(published(vertical, 1).isEmpty(), qPrintable(QString::fromLatin1(mount.token) + QStringLiteral(": ")
                                                             + published(vertical, 1)));
        QVERIFY2(std::abs(pitch(vertical)[0] - mount.p) <= kAngleTolerance,
                 qPrintable(QStringLiteral("%1: %2").arg(QString::fromLatin1(mount.token))
                                .arg(pitch(vertical)[0], 0, 'g', 17)));
        QCOMPARE(vertical.calculationEngine().runCount(kFit), 0);
    }
}

void FusionDerivedTest::sideMountPermutesTheAngles_data()
{
    QTest::addColumn<QString>("token");
    QTest::addColumn<double>("devicePitch");
    QTest::addColumn<double>("h");
    QTest::addColumn<double>("p");
    QTest::addColumn<double>("r");
    // The level device of levelNorthFacingBodyReadsZero: y north, x east, z up
    QTest::newRow("+y,+z") << QStringLiteral("+y,+z") << 0.0 << 0.0 << 0.0 << 0.0;
    QTest::newRow("+x,+z") << QStringLiteral("+x,+z") << 0.0 << 90.0 << 0.0 << 0.0;
    QTest::newRow("-x,+z") << QStringLiteral("-x,+z") << 0.0 << -90.0 << 0.0 << 0.0;
    QTest::newRow("+y,+x") << QStringLiteral("+y,+x") << 0.0 << 0.0 << 0.0 << 90.0;
    QTest::newRow("+y,-x") << QStringLiteral("+y,-x") << 0.0 << 0.0 << 0.0 << -90.0;
    QTest::newRow("+x,+y") << QStringLiteral("+x,+y") << 0.0 << 90.0 << 0.0 << -90.0;
    // The device pitched up 30 degrees under the default: forward is still
    // its y, pitched, and up its x, which stays level (east)
    QTest::newRow("+y,+x, pitched") << QStringLiteral("+y,+x") << 30.0 << 0.0 << 30.0 << 90.0;
}

// Criterion 8: a stored side mount changes the angles as the axis permutation
// predicts.
void FusionDerivedTest::sideMountPermutesTheAngles()
{
    QFETCH(QString, token);
    QFETCH(double, devicePitch);
    QFETCH(double, h);
    QFETCH(double, p);
    QFETCH(double, r);
    SessionData session = attitudeSession(QStringLiteral("m1"), {deviceQuaternion(0.0, devicePitch, 0.0)});
    session.setAttribute(SessionKeys::Orientation, token);
    QCOMPARE(attitudeDifference(session, h, p, r), QString());
    QCOMPARE(session.calculationEngine().runCount(kFit), 0);
}

// Criterion 9 (D8): with the device frame as the body frame (forward +x, up
// -z) the angles derived from a success golden's quaternion are that golden's
// own yaw, pitch and roll. Modulo 360: the fit unwraps all three and the
// derivation only heading.
void FusionDerivedTest::deviceFrameMountGivesTheFitsOwnAngles()
{
    const FusionGolden golden = loadFusionGolden(QStringLiteral("coarse_maneuver"));
    QCOMPARE(golden.outcome, QStringLiteral("succeeded"));
    const QVector<double> yaw = golden.channels.value(QStringLiteral("yaw"));
    const QVector<double> goldenPitch = golden.channels.value(QStringLiteral("pitch"));
    const QVector<double> goldenRoll = golden.channels.value(QStringLiteral("roll"));
    QVERIFY(yaw.size() > 100);

    // The premise: well away from the vertical, where heading and roll are defined
    double steepest = 0.0;
    for (const double sample : goldenPitch)
        steepest = std::max(steepest, std::abs(sample));
    QVERIFY2(steepest < 60.0, qPrintable(QString::number(steepest)));

    QHash<QString, QVector<double>> channels;
    for (const char *name : {"_time", "qx", "qy", "qz", "qw"})
        channels.insert(QString::fromLatin1(name), golden.channels.value(QString::fromLatin1(name)));
    SessionData session = syntheticFitSession(QStringLiteral("d1"), channels);
    session.setAttribute(SessionKeys::Orientation, QStringLiteral("+x,-z"));

    const QVector<double> h = heading(session);
    const QVector<double> p = pitch(session);
    const QVector<double> r = roll(session);
    QCOMPARE(h.size(), yaw.size());
    QCOMPARE(p.size(), yaw.size());
    QCOMPARE(r.size(), yaw.size());
    constexpr double tolerance = 1e-6;
    for (qsizetype i = 0; i < yaw.size(); ++i) {
        QVERIFY2(sameAngle(h[i], yaw[i], tolerance),
                 qPrintable(QStringLiteral("heading[%1] %2 vs yaw %3").arg(i).arg(h[i], 0, 'g', 17).arg(yaw[i], 0, 'g', 17)));
        QVERIFY2(sameAngle(p[i], goldenPitch[i], tolerance),
                 qPrintable(QStringLiteral("pitch[%1] %2 vs %3").arg(i).arg(p[i], 0, 'g', 17).arg(goldenPitch[i], 0, 'g', 17)));
        QVERIFY2(sameAngle(r[i], goldenRoll[i], tolerance),
                 qPrintable(QStringLiteral("roll[%1] %2 vs %3").arg(i).arg(r[i], 0, 'g', 17).arg(goldenRoll[i], 0, 'g', 17)));
    }
    QCOMPARE(session.calculationEngine().runCount(kFit), 0);
}

// Criterion 10: a stored value that is not one of the 24 tokens makes all
// three angles unavailable (a stored value wins, even an invalid one);
// removing it restores them.
void FusionDerivedTest::invalidStoredOrientationMakesAttitudeUnavailable()
{
    SessionData session = attitudeSession(QStringLiteral("i1"),
                                          {deviceQuaternion(0, 0, 0), deviceQuaternion(45, 10, -20)});
    QCOMPARE(heading(session).size(), 2);

    for (const QString &text : {QString(), QStringLiteral("+y,+y"), QStringLiteral("+y,-y"), QStringLiteral("-z,+z"),
                                QStringLiteral("y,z"), QStringLiteral("+Y,+Z"), QStringLiteral("+y, +z"),
                                QStringLiteral("forward +y, up +z"), QStringLiteral("+y,+z,+x")}) {
        session.setAttribute(SessionKeys::Orientation, text);
        QVERIFY2(session.hasAttribute(SessionKeys::Orientation), qPrintable(text));
        QVERIFY2(heading(session).isEmpty(), qPrintable(text));
        QVERIFY2(pitch(session).isEmpty(), qPrintable(text));
        QVERIFY2(roll(session).isEmpty(), qPrintable(text));
    }

    session.removeAttribute(SessionKeys::Orientation);
    QVERIFY(!session.hasAttribute(SessionKeys::Orientation));
    const QVector<double> h = heading(session), p = pitch(session), r = roll(session);
    QCOMPARE(h.size(), 2);
    QVERIFY(sameAngle(h[0], 0) && std::abs(p[0]) <= kAngleTolerance && sameAngle(r[0], 0));
    QVERIFY(sameAngle(h[1], 45) && std::abs(p[1] - 10) <= kAngleTolerance && sameAngle(r[1], -20));
    QCOMPARE(session.calculationEngine().runCount(kFit), 0);
    QCOMPARE(session.calculationEngine().undeclaredReadCount(), 0);
}

// Criterion 11 and the engine half of criterion 12: with nothing stored the
// session reads the default and stores nothing; a stored token wins and the
// angles recompute through ordinary invalidation, without the fit and without
// touching its own channels; removing it returns to the default's angles.
void FusionDerivedTest::storedOrientationRecomputesWithoutAFit()
{
    QHash<QString, QVector<double>> channels = quaternionChannels({deviceQuaternion(0, 0, 0)});
    // The fit's own angles, deliberately not the quaternion's: nothing may move them
    channels.insert(QStringLiteral("roll"), {1.5});
    channels.insert(QStringLiteral("pitch"), {-2.25});
    channels.insert(QStringLiteral("yaw"), {370.0});
    SessionData session = syntheticFitSession(QStringLiteral("r1"), channels);
    CalculationEngine &engine = session.calculationEngine();

    QVERIFY(!session.hasAttribute(SessionKeys::Orientation));
    QCOMPARE(session.getAttribute(SessionKeys::Orientation).toString(), QStringLiteral("+y,+z"));
    QVERIFY(!session.hasAttribute(SessionKeys::Orientation));
    QCOMPARE(attitudeDifference(session, 0, 0, 0), QString());
    QCOMPARE(engine.runCount(kAttitude), 1);

    // A stored token wins
    const QSet<DependencyKey> changed = session.setAttribute(SessionKeys::Orientation, QStringLiteral("+x,+z"));
    for (const char *name : {"bodyHeading", "bodyPitch", "bodyRoll"})
        QVERIFY2(changed.contains(fusionKey(QString::fromLatin1(name))), name);
    QCOMPARE(session.getAttribute(SessionKeys::Orientation).toString(), QStringLiteral("+x,+z"));
    QCOMPARE(attitudeDifference(session, 90, 0, 0), QString());
    QCOMPARE(engine.runCount(kAttitude), 2);

    // The fit's channels are what they were
    for (auto it = channels.cbegin(); it != channels.cend(); ++it)
        QVERIFY2(sameBitsEverywhere(fusion(session, it.key()), it.value()), qPrintable(it.key()));

    // Removed: the default's angles again
    QVERIFY(session.removeAttribute(SessionKeys::Orientation).contains(fusionKey(QStringLiteral("bodyHeading"))));
    QCOMPARE(attitudeDifference(session, 0, 0, 0), QString());
    QCOMPARE(engine.runCount(kAttitude), 3);

    QCOMPARE(engine.runCount(kFit), 0);
    QVERIFY(!engine.resultStatus(kFit).has_value());
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// ---- The Orientation column ----------------------------------------------------------------

QString FusionDerivedTest::startOrientationWorld(bool stubs, const std::optional<QString> &o1Token)
{
    TestEnvironment::instance().useFreshLogbook();
    LogbookManager::instance().initialize();

    SessionData o1 = fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("o1"));
    if (o1Token)
        o1.setAttribute(SessionKeys::Orientation, *o1Token);
    const SessionData o2 = fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("o2"));

    m_fixture = std::make_unique<ChoiceFixture>(QString::fromLatin1(SessionKeys::Orientation));
    return m_fixture->start({o1, o2}, stubs ? ChoiceFixture::Rows::Stubs : ChoiceFixture::Rows::Loaded);
}

void FusionDerivedTest::orientationColumnShowsTheDefaultWithoutAWrite_data() { addRowKinds(); }

// Criterion 12: every recording without a stored value shows the default's
// label, its file has no line for it, and index.json caches its token.
void FusionDerivedTest::orientationColumnShowsTheDefaultWithoutAWrite()
{
    QFETCH(bool, stubs);
    QCOMPARE(startOrientationWorld(stubs), QString());
    QCOMPARE(logbookColumnLabel(m_fixture->choiceColumn()), QStringLiteral("Orientation"));

    for (const QString &id : {QStringLiteral("o1"), QStringLiteral("o2")}) {
        QCOMPARE(m_fixture->displayText(id), kDefaultLabel);
        QCOMPARE(m_fixture->fileToken(id), std::optional<QString>());
        QVERIFY2(!fileBytes(id).isEmpty(), qPrintable(id));
        QVERIFY2(!fileBytes(id).contains("$VAR,_ORIENTATION"), qPrintable(id));
        QCOMPARE(m_fixture->indexValue(id), QJsonValue(QStringLiteral("+y,+z")));
    }
    for (int r = 0; r < m_fixture->model().rowCount(); ++r)
        QCOMPARE(std::as_const(m_fixture->model()).rowAt(r).isLoaded(), !stubs);
}

void FusionDerivedTest::orientationEditStoresATokenAndRefusesOthers_data() { addRowKinds(); }

// Criterion 13, setData(): a token of the list is stored and written
// verbatim, comma included; a token outside the list, a label, an empty
// string and an invalid value are refused (nothing an edit can carry removes
// a stored orientation); after a restart the stub shows the stored label.
void FusionDerivedTest::orientationEditStoresATokenAndRefusesOthers()
{
    QFETCH(bool, stubs);
    QCOMPARE(startOrientationWorld(stubs), QString());
    SessionModel &model = m_fixture->model();

    QVERIFY(m_fixture->setData(QStringLiteral("o1"), QStringLiteral("+x,+z")));
    QCOMPARE(m_fixture->displayText(QStringLiteral("o1")), QStringLiteral("forward +x, up +z"));
    QVERIFY(waitForIdle(model));
    QVERIFY(fileBytes(QStringLiteral("o1")).contains("\n$VAR,_ORIENTATION,+x,+z\n"));
    QCOMPARE(m_fixture->fileToken(QStringLiteral("o1")), std::optional<QString>(QStringLiteral("+x,+z")));
    QCOMPARE(m_fixture->indexValue(QStringLiteral("o1")), QJsonValue(QStringLiteral("+x,+z")));

    // Refused: nothing changes, nothing is written
    const QByteArray o1Bytes = fileBytes(QStringLiteral("o1"));
    const QByteArray o2Bytes = fileBytes(QStringLiteral("o2"));
    QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);
    for (const QVariant &value : {QVariant(QStringLiteral("+y,+y")), QVariant(QStringLiteral("+z,-z")),
                                  QVariant(QStringLiteral("y,z")), QVariant(QStringLiteral("+X,+Z")),
                                  QVariant(QStringLiteral("+x, +z")), QVariant(QStringLiteral("forward +y, up +x")),
                                  QVariant(QString()), QVariant(QStringLiteral("")), QVariant()}) {
        for (const QString &id : {QStringLiteral("o1"), QStringLiteral("o2")})
            QVERIFY2(!m_fixture->setData(id, value), qPrintable(id + QLatin1Char(' ') + value.toString()));
    }
    QCOMPARE(dataSpy.count(), 0);
    QVERIFY(waitForIdle(model));
    QCOMPARE(fileBytes(QStringLiteral("o1")), o1Bytes);
    QCOMPARE(fileBytes(QStringLiteral("o2")), o2Bytes);
    QCOMPARE(m_fixture->displayText(QStringLiteral("o1")), QStringLiteral("forward +x, up +z"));
    QCOMPARE(m_fixture->displayText(QStringLiteral("o2")), kDefaultLabel);

    // After a restart every row is a stub, showing the stored label
    QCOMPARE(m_fixture->restartAsStubs(), QString());
    QCOMPARE(m_fixture->displayText(QStringLiteral("o1")), QStringLiteral("forward +x, up +z"));
    QCOMPARE(m_fixture->displayText(QStringLiteral("o2")), kDefaultLabel);
}

void FusionDerivedTest::orientationBulkEdit_data() { addRowKinds(); }

// Criterion 13, the bulk edit: a token for every selected session, and a
// token outside the list, or an invalid value, queues nothing.
void FusionDerivedTest::orientationBulkEdit()
{
    QFETCH(bool, stubs);
    QCOMPARE(startOrientationWorld(stubs), QString());
    SessionModel &model = m_fixture->model();
    const QStringList both{QStringLiteral("o1"), QStringLiteral("o2")};

    QVERIFY(m_fixture->bulkEdit(both, QStringLiteral("+y,+x")));
    for (const QString &id : both) {
        QCOMPARE(m_fixture->fileToken(id), std::optional<QString>(QStringLiteral("+y,+x")));
        QCOMPARE(m_fixture->indexValue(id), QJsonValue(QStringLiteral("+y,+x")));
        QCOMPARE(m_fixture->displayText(id), QStringLiteral("forward +y, up +x"));
    }


    // Outside the list: refused before anything is queued
    const QByteArray o1Bytes = fileBytes(QStringLiteral("o1"));
    const QByteArray o2Bytes = fileBytes(QStringLiteral("o2"));
    const QList<int> rows{m_fixture->row(QStringLiteral("o1")), m_fixture->row(QStringLiteral("o2"))};
    BulkEditSignals bulk(model);
    QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);
    for (const QVariant &value : {QVariant(QStringLiteral("+x,-x")), QVariant(QStringLiteral("forward +y, up +x")),
                                  QVariant(QString()), QVariant(QStringLiteral("zz")), QVariant()})
        model.startBulkEdit(rows, m_fixture->column(), value);
    QVERIFY(waitForIdle(model));
    QCOMPARE(bulk.activations(), 0);
    QCOMPARE(dataSpy.count(), 0);
    QCOMPARE(fileBytes(QStringLiteral("o1")), o1Bytes);
    QCOMPARE(fileBytes(QStringLiteral("o2")), o2Bytes);
    for (int r = 0; r < model.rowCount(); ++r)
        QCOMPARE(std::as_const(model).rowAt(r).isLoaded(), !stubs);
}

FLYSIGHT_TEST_MAIN(FusionDerivedTest)
#include "tst_fusion_derived.moc"
