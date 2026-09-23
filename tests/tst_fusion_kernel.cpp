// Fusion kernel internals.
//
// What the golden fixtures of tst_fusion_golden cannot reach (the segmented
// initializer: segment cutting on fixes, the smallest-sAcc anchor, prefix
// growth on the marginal yaw sigma, the fallback when every start fails, its
// progress texts and the synthetic recordings of the specification; exact
// integration boundaries, heading freedom, the two stopping rules forced
// through the tuning, the per-step covariance, the temperature-dependent gyro
// bias (the custom factor's Jacobians, the section 6 cases), the
// solver-failure path and its diagnostics shapes), with the literal expectations of the reference's own
// self-test (sensor-fusion-clean-port, tests/fusion_regression.cpp), and the
// fit trace that localizes a golden failure to a stage: the segment account
// first, then each optimizer iteration.
//
// The only test source that includes internal src/fusion/ headers, and one of
// the few targets that names gtsam itself.

#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <string>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QtTest>

#include <gtsam/config.h>
#include <gtsam/base/numericalDerivative.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/navigation/GPSFactor.h>
#include <gtsam/navigation/ImuFactor.h>
#include <gtsam/navigation/NavState.h>
#include <gtsam/slam/PriorFactor.h>

#include "calculations/anglehelper.h"
#include "fusion/factorgraphfit.h"
#include "fusion/fusionoutput.h"
#include "fusion/fusionpipeline.h"
#include "fusion/fusionsamples.h"
#include "fusion/imuintegration.h"
#include "fusion/initializer.h"
#include "fusion/inputadapter.h"
#include "fusion/temperatureimufactor.h"
#include "fusion/trajectoryreconstruction.h"
#include "fusionfixtures.h"
#include "fusiongolden.h"
#include "fusiontrace.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySight::Fusion::Detail;
using namespace FlySightTest;
using gtsam::Rot3;
using gtsam::Vector3;
using gtsam::symbol_shorthand::B;
using gtsam::symbol_shorthand::T;
using gtsam::symbol_shorthand::V;
using gtsam::symbol_shorthand::X;

namespace {

const Vector3 kTestGravity(0, 0, 9.80665);

/// 1 s of 100 Hz IMU under constant acceleration (1, -2, .5), not rotating,
/// with two GNSS fixes that fall between IMU samples.
Samples boundarySamples(const Vector3 &acceleration)
{
    Samples d;
    for (int i = 0; i <= 100; ++i) {
        d.imuTime.push_back(i*.01);
        d.force.push_back(acceleration-kTestGravity);
        d.gyro.push_back(Vector3::Zero());
    }
    d.gnssTime = {.037, .863};
    d.position = Vectors(2, Vector3::Zero());
    d.velocity = d.position;
    d.positionSigma = Vectors(2, Vector3::Ones());
    d.velocitySigma = d.positionSigma;
    return d;
}

/// 2 s of 100 Hz IMU: the constant acceleration (1, -2, .5) for the first
/// second, then none, not rotating, with three GNSS fixes between IMU samples
/// (the last after the manoeuvre). A single interval of constant force leaves
/// the rotation about that force axis unobservable, and the vertical has a
/// component along it; the change of force direction between the two
/// intervals is what makes every rotation, the yaw included, observable.
Samples manoeuvreSamples()
{
    const Vector3 acceleration(1, -2, .5);
    Samples d;
    for (int i = 0; i <= 200; ++i) {
        d.imuTime.push_back(i*.01);
        d.force.push_back((i < 100 ? acceleration : Vector3(0, 0, 0))-kTestGravity);
        d.gyro.push_back(Vector3::Zero());
    }
    d.gnssTime = {.037, .863, 1.663};
    for (const double t : d.gnssTime) {
        // The trajectory of the piecewise force, to within the one ramp step.
        const double moving = std::min(t, 1.), coasting = std::max(t-1, 0.);
        d.position.push_back(Vector3(acceleration*(.5*moving*moving+coasting)));
        d.velocity.push_back(Vector3(acceleration*moving));
    }
    d.positionSigma = Vectors(3, Vector3::Ones());
    d.velocitySigma = Vectors(3, Vector3::Constant(.1));
    return d;
}

/// 1 s of IMU at 8 Hz whose rate and force ramp by exactly .03125 rad/s and
/// .125 m/s^2 per step of exactly .125 s: every time and value is a small
/// integer times a power of two, so every step's change is the same bit for
/// bit. No GNSS: preintegrateImu() reads none.
Samples rampSamples()
{
    Samples d;
    for (int i = 0; i <= 8; ++i) {
        d.imuTime.push_back(i*.125);
        d.gyro.emplace_back(0, 0, i*.03125);
        d.force.emplace_back(i*.125, 0, -9.80665);
    }
    return d;
}

/// One integration step of length `dt` with a change of .5 rad/s and 1 m/s^2
/// whatever `dt` is.
Samples singleStepSamples(double dt)
{
    Samples d;
    d.imuTime = {0, dt};
    d.gyro = {Vector3(0, 0, 0), Vector3(0, 0, .5)};
    d.force = {Vector3(0, 0, -9.80665), Vector3(1, 0, -9.80665)};
    return d;
}

/// The production tuning with the two per-step slopes replaced.
Tuning withSlopes(double gyro, double acc)
{
    Tuning t;
    t.gyroStepSlope = gyro;
    t.accStepSlope = acc;
    return t;
}

/// `t` with the per-step term folded into the densities and the slopes zero.
/// On a recording whose every step has length `dt` and the given changes, a
/// preintegration with this tuning is the expected value of one with `t`:
/// the same covariance on every step, so the same arithmetic, without
/// restating how the covariance propagates.
Tuning densityFor(const Tuning &t, double dt, double deltaGyro, double deltaForce)
{
    const double sigmaW = t.gyroStepSlope*dt*deltaGyro, sigmaA = t.accStepSlope*dt*deltaForce;
    Tuning folded = t;
    folded.gyroDensity = std::sqrt(t.gyroDensity*t.gyroDensity + sigmaW*sigmaW*dt);
    folded.accDensity = std::sqrt(t.accDensity*t.accDensity + sigmaA*sigmaA*dt);
    folded.gyroStepSlope = 0;
    folded.accStepSlope = 0;
    return folded;
}

/// The tolerance exists only because sqrt(x)^2 is not x in floating point; a
/// missing term, a missing dt factor or midpoint differences instead of end
/// minus start move a covariance by 1e-3 to 1e0 relative in the tests below.
bool sameCovariance(const gtsam::Matrix &got, const gtsam::Matrix &expected)
{
    return (got-expected).cwiseAbs().maxCoeff() <= 1e-9*expected.cwiseAbs().maxCoeff();
}

/// The reference self-test's exact constant-velocity recording, with
/// epoch-relative (exactly representable) times.
Samples linearSamples(const Vector3 &speed, const Vector3 &offset)
{
    Samples linear;
    for (int i = 0; i <= 200; ++i) {
        linear.imuTime.push_back(i*.01);
        linear.force.push_back(-kTestGravity);
        linear.gyro.push_back(Vector3::Zero());
    }
    for (int i = 0; i <= 8; ++i) {
        const double t = .037+i*.2;
        linear.gnssTime.push_back(t);
        linear.position.push_back(offset+t*speed);
        linear.velocity.push_back(speed);
        linear.positionSigma.push_back(Vector3::Ones());
        linear.velocitySigma.push_back(Vector3::Constant(.1));
    }
    return linear;
}

Fusion::Result rejectedBy(const Fusion::Channels &channels)
{
    return runPipeline(channels, Tuning{}, Checkpoint());
}

QJsonObject diagnosticsOf(const Fusion::Result &result)
{
    return QJsonDocument::fromJson(result.diagnosticsJson.toUtf8()).object();
}

/// The failure diagnostics of a fit that completed its passes: QJsonObject
/// sorts its keys.
const QStringList kCompletedPassFailureKeys{QStringLiteral("algorithm"), QStringLiteral("failure"),
                                            QStringLiteral("quality"), QStringLiteral("stopping")};

bool allChannelsEmpty(const Fusion::Result &result)
{
    return result.time.isEmpty() && result.north.isEmpty() && result.accN.isEmpty()
        && result.roll.isEmpty() && result.yaw.isEmpty() && result.qw.isEmpty();
}

/// A golden fixture or one of the initializer's recordings, by name.
FusionFixture fixtureNamed(const QString &name)
{
    const FusionFixture golden = fusionFixture(name);
    return golden.name.isEmpty() ? initializerFixture(name) : golden;
}

/// `tuning` with the IMU gap limit the pipeline derives for a fixture.
Tuning pipelineTuning(const QString &name, Tuning tuning)
{
    tuning.maxGap = kImuGapMedians*medianInterval(prepareInput(toChannels(fixtureNamed(name))).recording.imuTime);
    return tuning;
}

/// The fitted window of a fixture exactly as the pipeline cuts it: the
/// prepared recording from its usable start to its last fix, validated with
/// the IMU gap limit the pipeline derives.
Samples windowOf(const QString &name, const Tuning &tuning)
{
    const PreparedInput prepared = prepareInput(toChannels(fixtureNamed(name)));
    const Samples &full = prepared.recording;
    const Samples window = fittedWindow(full, prepared.usableStart, full.gnssTime.back());
    validateSamples(window, pipelineTuning(name, tuning));
    return window;
}

/// One of the initializer's recordings through the whole pipeline, with the
/// trace and the parsed diagnostics.
struct InitializerRun {
    Fusion::Result result;
    PipelineTrace trace;
    QJsonObject diagnostics;
    QJsonArray segments;        ///< diagnostics["initializer"]["segments"]
};

InitializerRun runInitializerFixture(const QString &name, const Tuning &tuning,
                                     const Checkpoint &checkpoint = Checkpoint())
{
    InitializerRun run;
    run.result = runPipeline(toChannels(fixtureNamed(name)), tuning, checkpoint, &run.trace);
    run.diagnostics = diagnosticsOf(run.result);
    run.segments = run.diagnostics.value("initializer").toObject().value("segments").toArray();
    return run;
}

/// The budget of every prefix fit (spec section 3.3): one pass of at most 50
/// iterations, in the diagnostics and in the account.
void verifyPrefixBudget(const QJsonObject &segment, const SegmentAccount &account)
{
    QCOMPARE(segment.value("prefix_passes").toInt(-1), 1);
    QVERIFY(segment.value("prefix_iterations").toInt(999) <= 50);
    QCOMPARE(account.prefixPasses, 1);
    QVERIFY(account.prefixIterations <= 50);
}

/// The difference of two angles in degrees, on the circle.
double angleDifference(double a, double b)
{
    return std::remainder(a-b, 360.);
}

/// The truth attitude of a fixture built from its exact rotation matrix.
Rot3 rotationFromMatrix(double r00, double r01, double r02,
                        double r10, double r11, double r12,
                        double r20, double r21, double r22)
{
    gtsam::Matrix3 m;
    m << r00, r01, r02, r10, r11, r12, r20, r21, r22;
    return Rot3(m);
}

} // namespace

class FusionKernelTest : public QObject {
    Q_OBJECT

private slots:
    void solverUsesTbb();
    void unwrapRule();
    void preintegrationHonoursExactBoundaries();
    void perStepTermIsZeroWithoutSignalChange();
    void perStepTermMatchesSpecifiedCovariance();
    void perStepTermScalesWithStep();
    void diagnosticsReportPerStepConstants();
    void validationRejectsEachDefect();
    void backwardPropagationUndoesForward();
    void headingIsUnconstrained();
    void reconstructionTimingAndEndpointCorrection();
    void shortWindowIsOneSegment();
    void segmentsAreCutOnFixes();
    void yawSigmaIsMarginalAboutTheVertical();
    void initializerProgressTexts();
    void initializerDiagnosticsShape();
    void exactConstantVelocityFit();
    void initializerFixturesAreDeterministic();
    void fitTraceMatchesGolden_data();
    void fitTraceMatchesGolden();
    void biasSettledByCostTest();
    void slowTailAtTheIterationLimit_data();
    void slowTailAtTheIterationLimit();
    void nonConvergenceIsSolverFailure();
    void biasNeverSettlesIsSolverFailure();
    void failureDiagnosticsShape();
    void startsInMotionGrowsToTheManoeuvre();
    void atRestPrefixStopsGrowing();
    void smallestSaccFixIsTheAnchor();
    void driftingBiasSegmentsConverge();
    void allPrefixFitsFailFallsBack_data();
    void allPrefixFitsFailFallsBack();
    void prefixFitsFailAfterACompletedLength();
    void startsOnTheLimitAreStillUsed();
    void temperatureFactorJacobians();
    void temperatureGraphShape();
    void reconstructionUsesIntervalBias();
    void constantTemperatureKeepsSlopeAtPrior();
};

void FusionKernelTest::solverUsesTbb()
{
    // twoRunsAreBitIdentical (tst_fusion_golden) means something only if the
    // solver really is multi-threaded.
#ifndef GTSAM_USE_TBB
    QFAIL("GTSAM was built without TBB (GTSAM_USE_TBB is not defined)");
#endif
}

void FusionKernelTest::unwrapRule()
{
    using Calculations::unwrapDegrees;
    QVERIFY(unwrapDegrees({}).isEmpty());
    QCOMPARE(unwrapDegrees({45}), QVector<double>({45}));
    QCOMPARE(unwrapDegrees({170, 179, -179, -170, -10, 150, -50, 110, -90}),
             QVector<double>({170, 179, 181, 190, 350, 510, 670, 830, 990}));
    QCOMPARE(unwrapDegrees({-170, -179, 179, 170, 10, -150, 50, -110, 90}),
             QVector<double>({-170, -179, -181, -190, -350, -510, -670, -830, -990}));
    // A reversal stays on the branch it was on
    QCOMPARE(unwrapDegrees({170, -170, 170, -170}), QVector<double>({170, 190, 170, 190}));
    // Exactly half a turn is not wrapped
    QCOMPARE(unwrapDegrees({0, 180, 0, -180}), QVector<double>({0, 180, 0, -180}));
}

void FusionKernelTest::preintegrationHonoursExactBoundaries()
{
    const Vector3 acceleration(1, -2, .5);
    const Samples d = boundarySamples(acceleration);
    const Tuning tuning;
    validateSamples(d, tuning);

    const gtsam::imuBias::ConstantBias bias;
    const auto pim = preintegrateImu(d, .037, .863, bias, tuning);
    const auto predicted = pim.predict(gtsam::NavState(gtsam::Pose3(), Vector3::Zero()), bias);
    const double duration = .863-.037;
    QVERIFY((predicted.velocity()-acceleration*duration).norm() < 1e-10);
    QVERIFY((predicted.position()-.5*acceleration*duration*duration).norm() < 1e-10);
}

void FusionKernelTest::perStepTermIsZeroWithoutSignalChange()
{
    // Spec section 10: a step with zero signal change has the density
    // covariance exactly. Constant force and zero rate, so every step's change
    // is exactly zero and the term is exactly 0.0 whatever the slope, even
    // 1e3: the comparison is bitwise (Eigen's == is element-wise equality),
    // with no tolerance.
    const Samples d = boundarySamples(Vector3(1, -2, .5));
    const gtsam::imuBias::ConstantBias bias;
    const auto without = preintegrateImu(d, .037, .863, bias, withSlopes(0, 0));
    for (const Tuning &tuning : { Tuning{}, withSlopes(1e3, 1e3) }) {
        const auto with = preintegrateImu(d, .037, .863, bias, tuning);
        QVERIFY(with.preintMeasCov() == without.preintMeasCov());
        QVERIFY(with.deltaPij() == without.deltaPij());
        QVERIFY(with.deltaVij() == without.deltaVij());
        QVERIFY(with.deltaRij().matrix() == without.deltaRij().matrix());
    }
}

void FusionKernelTest::perStepTermMatchesSpecifiedCovariance()
{
    // Spec section 10: a step with a known change has the specified
    // covariance. Eight steps of .125 s, each with a change of .03125 rad/s
    // and .125 m/s^2, so every step's term is the same and the expectation is
    // a second, density-only preintegration with sqrt(density^2 + sigma^2 dt)
    // as the density. With slopes 8 and 4 the term dominates (sigma_w = .03125
    // rad, sigma_w^2 dt = 1.22e-4 against 1e-6; sigma_a = .0625 m/s, sigma_a^2
    // dt = 4.9e-4 against 2.25e-4); with the production slopes it is 1.3e-3 of
    // the gyro covariance and 2.2 % of the accelerometer's, both far above the
    // tolerance, as the negative check against the density-only value proves.
    const Samples d = rampSamples();
    const gtsam::imuBias::ConstantBias bias;
    const auto densityOnly = preintegrateImu(d, 0, 1, bias, withSlopes(0, 0));
    for (const Tuning &tuning : { withSlopes(8, 4), Tuning{} }) {
        const auto got = preintegrateImu(d, 0, 1, bias, tuning);
        const auto expected = preintegrateImu(d, 0, 1, bias, densityFor(tuning, .125, .03125, .125));
        QVERIFY(sameCovariance(got.preintMeasCov(), expected.preintMeasCov()));
        QVERIFY(!sameCovariance(got.preintMeasCov(), densityOnly.preintMeasCov()));
    }
}

void FusionKernelTest::perStepTermScalesWithStep()
{
    // Spec section 10: the term scales with dt. The same change (.5 rad/s,
    // 1 m/s^2) over one step of .125 s and one of .25 s: the sigma doubles
    // with the step (.5 then 1.0 rad; .5 then 1.0 m/s) and each preintegration
    // matches its own expectation. The cross check pins the dt factor in the
    // sigma: the longer step is not within tolerance of the shorter step's
    // sigma, the added variance differing by more than a factor four.
    const Tuning tuning = withSlopes(8, 4);
    const gtsam::imuBias::ConstantBias bias;
    const Samples shortStep = singleStepSamples(.125), longStep = singleStepSamples(.25);

    const auto gotShort = preintegrateImu(shortStep, 0, .125, bias, tuning);
    const auto expectedShort = preintegrateImu(shortStep, 0, .125, bias, densityFor(tuning, .125, .5, 1));
    QVERIFY(sameCovariance(gotShort.preintMeasCov(), expectedShort.preintMeasCov()));

    const auto gotLong = preintegrateImu(longStep, 0, .25, bias, tuning);
    const auto expectedLong = preintegrateImu(longStep, 0, .25, bias, densityFor(tuning, .25, .5, 1));
    QVERIFY(sameCovariance(gotLong.preintMeasCov(), expectedLong.preintMeasCov()));

    const auto shorterSigma = preintegrateImu(longStep, 0, .25, bias, densityFor(tuning, .125, .5, 1));
    QVERIFY(!sameCovariance(gotLong.preintMeasCov(), shorterSigma.preintMeasCov()));
    QVERIFY((gotLong.preintMeasCov()-shorterSigma.preintMeasCov()).cwiseAbs().maxCoeff()
            > 1e-3*shorterSigma.preintMeasCov().cwiseAbs().maxCoeff());
}

void FusionKernelTest::diagnosticsReportPerStepConstants()
{
    QCOMPARE(Tuning{}.gyroStepSlope, .026);
    QCOMPARE(Tuning{}.accStepSlope, .40);

    // The constants reported are the ones the fit ran with, not the defaults.
    // coarse_linear has zero signal change, so both fits are the same fit and
    // their objectives are identical.
    const Fusion::Channels channels = toChannels(fusionFixture(QStringLiteral("coarse_linear")));
    QJsonObject first;
    for (const Tuning &tuning : { Tuning{}, withSlopes(.5, .7) }) {
        const Fusion::Result result = runPipeline(channels, tuning, Checkpoint());
        QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
        const QJsonObject diagnostics = diagnosticsOf(result);
        const QJsonObject model = diagnostics.value("model").toObject();
        QCOMPARE(model.keys(), QStringList({QStringLiteral("gyro_bias"), QStringLiteral("per_step")}));
        // A constant 25 degC series (kFixtureTemperatureDegC) has exactly that mean.
        QCOMPARE(model.value("gyro_bias").toObject().value("t_ref_degc").toDouble(), kFixtureTemperatureDegC);
        const QJsonObject perStep = model.value("per_step").toObject();
        QCOMPARE(perStep.keys(), QStringList({QStringLiteral("acc_slope_s"), QStringLiteral("gyro_slope_s")}));
        QCOMPARE(perStep.value("gyro_slope_s").toDouble(), tuning.gyroStepSlope);
        QCOMPARE(perStep.value("acc_slope_s").toDouble(), tuning.accStepSlope);
        if (first.isEmpty())
            first = diagnostics;
        else
            QVERIFY(first.value("objective").toDouble() == diagnostics.value("objective").toDouble());
    }
}

void FusionKernelTest::validationRejectsEachDefect()
{
    const Samples d = boundarySamples(Vector3(1, -2, .5));
    const Tuning tuning;

    // The kernel's own validation
    Samples bad = d;
    bad.imuTime[4] = bad.imuTime[3];
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(bad, tuning));
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(Samples{}, tuning));
    bad = d;
    bad.force.pop_back();
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(bad, tuning));
    bad = d;
    bad.gyro[5].x() = std::numeric_limits<double>::quiet_NaN();
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(bad, tuning));
    bad = d;
    bad.velocitySigma[0].x() = 0;
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(bad, tuning));
    bad = d;
    bad.gnssTime.back() = 1.1;                       // beyond IMU coverage
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(bad, tuning));
    bad = d;                                         // IMU samples 40..49 missing
    bad.imuTime.erase(bad.imuTime.begin()+40, bad.imuTime.begin()+50);
    bad.force.erase(bad.force.begin()+40, bad.force.begin()+50);
    bad.gyro.erase(bad.gyro.begin()+40, bad.gyro.begin()+50);
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(bad, tuning));

    // The tuning: the stopping thresholds must be finite and the window at
    // least one iteration; a negative tolerance or bound is legal (a test's
    // "never" forcing).
    Tuning t;
    t.slowTailWindow = 0;
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(d, t));
    t = Tuning{};
    t.biasSettledTolerance = std::numeric_limits<double>::quiet_NaN();
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(d, t));
    t = Tuning{};
    t.slowTailMaxNrms = std::numeric_limits<double>::infinity();
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(d, t));
    t = Tuning{};
    t.relativeTolerance = -1;
    validateSamples(d, t);
    t = Tuning{};
    t.biasSettledTolerance = -1;
    validateSamples(d, t);

    // The per-step slopes: zero is the term switched off; negative or
    // non-finite is invalid.
    t = Tuning{};
    t.gyroStepSlope = -1e-3;
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(d, t));
    t = Tuning{};
    t.accStepSlope = std::numeric_limits<double>::quiet_NaN();
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(d, t));
    validateSamples(d, withSlopes(0, 0));

    // The b1 prior sigma must be strictly positive.
    t = Tuning{};
    t.gyroBiasSlopeSigma = 0;
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(d, t));
    t = Tuning{};
    t.gyroBiasSlopeSigma = -1;
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(d, t));

    // The temperature series: absent (the stock path) or one finite value
    // per IMU sample.
    bad = d;
    bad.temperature = std::vector<double>(bad.imuTime.size()-1, 20.);
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(bad, tuning));
    bad.temperature = std::vector<double>(bad.imuTime.size(), 20.);
    bad.temperature[3] = std::numeric_limits<double>::quiet_NaN();
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(bad, tuning));
    bad.temperature.assign(bad.imuTime.size(), 20.);
    validateSamples(bad, tuning);

    // The initializer's lengths must be positive.
    t = Tuning{};
    t.segmentLength = 0;
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(d, t));
    t = Tuning{};
    t.minFinalSegment = -1;
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(d, t));
    t = Tuning{};
    t.maxPasses = 0;
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(d, t));

    // The initial state: one attitude per fix and a finite bias.
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, initialValues(d, InitialState{}));
    InitialState nanBias{std::vector<Rot3>(2), Vector3(0, std::numeric_limits<double>::quiet_NaN(), 0)};
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, initialValues(d, nanBias));
    const gtsam::Values values = initialValues(d, InitialState{std::vector<Rot3>(2), Vector3::Zero()});
    QCOMPARE(values.size(), size_t(5));

    // The whole pipeline: a malformed recording is a Rejected result, never
    // an exception and never a crash.
    const Fusion::Channels good = toChannels(fusionFixture(QStringLiteral("coarse_linear")));
    QVERIFY(rejectedBy(good).outcome == Fusion::Outcome::Succeeded);
    QVERIFY(rejectedBy(Fusion::Channels{}).outcome == Fusion::Outcome::Rejected);

    Fusion::Channels c = good;
    c.ax.removeLast();
    QVERIFY(rejectedBy(c).outcome == Fusion::Outcome::Rejected);
    c = good;
    c.north.clear();
    QVERIFY(rejectedBy(c).outcome == Fusion::Outcome::Rejected);
    c = good;
    c.gnssTime[0] = std::numeric_limits<double>::quiet_NaN();   // makes the epoch NaN
    QVERIFY(rejectedBy(c).outcome == Fusion::Outcome::Rejected);
    c = good;
    c.wx[0] = std::numeric_limits<double>::infinity();
    QVERIFY(rejectedBy(c).outcome == Fusion::Outcome::Rejected);
    c = good;
    c.gnssTime[1] = c.gnssTime[0];
    QVERIFY(rejectedBy(c).outcome == Fusion::Outcome::Rejected);
    c = good;
    c.sAcc[0] = 0;
    QVERIFY(rejectedBy(c).outcome == Fusion::Outcome::Rejected);

    // Spec section 10's "a recording without a temperature channel", kernel
    // side: rejected by name, and after every other channel's defect.
    const Fusion::Channels maneuver = toChannels(fusionFixture(QStringLiteral("coarse_maneuver")));
    QVERIFY(rejectedBy(maneuver).outcome == Fusion::Outcome::Succeeded);
    c = maneuver;
    c.imuTemperature.clear();
    Fusion::Result rejected = rejectedBy(c);
    QVERIFY(rejected.outcome == Fusion::Outcome::Rejected);
    QCOMPARE(rejected.reason, QStringLiteral("Missing or mismatched IMU/temperature"));
    c = maneuver;
    c.imuTemperature.removeLast();
    rejected = rejectedBy(c);
    QVERIFY(rejected.outcome == Fusion::Outcome::Rejected);
    QCOMPARE(rejected.reason, QStringLiteral("Missing or mismatched IMU/temperature"));
    c = maneuver;
    c.imuTemperature[5] = std::numeric_limits<double>::quiet_NaN();
    rejected = rejectedBy(c);
    QVERIFY(rejected.outcome == Fusion::Outcome::Rejected);
    QCOMPARE(rejected.reason, QStringLiteral("Nonfinite IMU/temperature"));
    c = maneuver;
    c.imuTemperature.clear();
    c.wz[0] = std::numeric_limits<double>::infinity();
    rejected = rejectedBy(c);
    QVERIFY(rejected.outcome == Fusion::Outcome::Rejected);
    QCOMPARE(rejected.reason, QStringLiteral("Nonfinite IMU/wz"));
}

void FusionKernelTest::backwardPropagationUndoesForward()
{
    // Rates about different axes in the two halves do not commute, so going
    // back must undo the steps in reverse order.
    Samples d = boundarySamples(Vector3(1, -2, .5));
    const Vector3 bg(.004, -.003, .006);
    for (size_t i = 0; i < d.gyro.size(); ++i)
        d.gyro[i] = bg+(i < 50 ? Vector3(.1, 0, 0) : Vector3(0, .2, 0));

    const Rot3 r = Rot3::RzRyRx(.3, -.2, .1);
    const Rot3 forward = propagateAttitude(d, r, .037, .863, bg);
    const Rot3 backward = propagateAttitude(d, forward, .863, .037, bg);
    QVERIFY(Rot3::Logmap(forward.between(r)).norm() > 1e-3);
    QVERIFY(Rot3::Logmap(backward.between(r)).norm() < 1e-12);
}

void FusionKernelTest::headingIsUnconstrained()
{
    Samples d = boundarySamples(Vector3::Zero());
    const gtsam::imuBias::ConstantBias bias;
    const auto graph = buildFactorGraph(d, bias, Tuning{});

    std::vector<double> costs;
    for (double yaw : {0., .8, 2.}) {
        gtsam::Values values;
        values.insert(B(0), bias);
        for (size_t k = 0; k < 2; ++k) {
            values.insert(X(k), gtsam::Pose3(Rot3::Rz(yaw), Vector3::Zero()));
            values.insert(V(k), Vector3(0, 0, 0));
        }
        costs.push_back(graph.error(values));
    }
    QVERIFY(std::abs(costs[0]-costs[1]) < 1e-8);
    QVERIFY(std::abs(costs[0]-costs[2]) < 1e-8);
}

void FusionKernelTest::reconstructionTimingAndEndpointCorrection()
{
    const Samples d = boundarySamples(Vector3::Zero());
    const double duration = .863-.037;

    // Two states that differ by a yaw of .2 rad which the (zero) gyro does
    // not explain: the difference must be spread linearly over the interval.
    FitResult endpoints;
    endpoints.values.insert(B(0), gtsam::imuBias::ConstantBias());
    endpoints.values.insert(X(0), gtsam::Pose3());
    endpoints.values.insert(X(1), gtsam::Pose3(Rot3::Rz(.2), Vector3::Zero()));
    endpoints.values.insert(V(0), Vector3(0, 0, 0));
    endpoints.values.insert(V(1), Vector3(0, 0, 0));

    const DenseTrajectory dense = reconstructTrajectory(d, endpoints);
    QCOMPARE(dense.time.size(), size_t(83));
    QCOMPARE(dense.time.front(), .04);
    QCOMPARE(dense.time.back(), .86);
    QCOMPARE(dense.endpointCorrection.size(), size_t(1));
    for (size_t i = 0; i < dense.time.size(); ++i) {
        const Rot3 expected = Rot3::Rz(.2*(dense.time[i]-.037)/duration);
        QVERIFY(Rot3::Logmap(dense.rotation[i].between(expected)).norm() < 1e-12);
        QVERIFY(dense.acceleration[i].norm() < 1e-12);
    }
}

void FusionKernelTest::shortWindowIsOneSegment()
{
    // 2 s of exact constant velocity: one segment, whose first prefix window
    // (60 s centred on the first fix, every sAcc being .1 so the earliest
    // wins) covers it, so one length is tried with four starts and the
    // segment fit runs. Exact constant velocity has no yaw information: the
    // marginal yaw sigma is the cap.
    const Samples linear = linearSamples(Vector3(12, -4, 2), Vector3(7, 8, 9));
    const Initialization init = initialize(linear, Tuning{});
    QCOMPARE(init.account.segmentLength, 600.);
    QCOMPARE(init.account.segments.size(), size_t(1));
    const SegmentAccount &s = init.account.segments[0];
    QCOMPARE(s.index, 0);
    QCOMPARE(s.firstFix, size_t(0));
    QCOMPARE(s.lastFix, size_t(8));
    QCOMPARE(s.start, linear.gnssTime.front());
    QCOMPARE(s.end, linear.gnssTime.back());
    QCOMPARE(s.anchorTime, s.start);
    QCOMPARE(s.anchorSacc, .1);
    QCOMPARE(s.prefixLength, 60.);
    QCOMPARE(s.prefixStart, s.start);
    QCOMPARE(s.prefixEnd, s.end);
    QCOMPARE(s.prefixFits, 4);
    QCOMPARE(s.prefixYawSigmaDeg.size(), size_t(1));
    qInfo() << "linear: yaw sigma" << s.yawSigmaDeg << "deg, growth stop" << s.growthStop.c_str();
    QCOMPARE(s.yawSigmaDeg, 180.);
    QCOMPARE(s.prefixYawSigmaDeg[0], s.yawSigmaDeg);
    QCOMPARE(s.growthStop, std::string("covers"));
    QCOMPARE(s.prefixPasses, 1);
    QVERIFY(s.prefixIterations >= 1 && s.prefixIterations <= 50);
    QVERIFY(!s.fallback);
    QVERIFY(s.iterations > 0);
    QVERIFY(s.converged);
    QCOMPARE(init.state.rotations.size(), size_t(9));
    QVERIFY(init.state.gyroBias == s.gyroBias);
    QVERIFY(init.state.gyroBias.allFinite());
}

void FusionKernelTest::segmentsAreCutOnFixes()
{
    using Bounds = std::vector<std::pair<size_t, size_t>>;
    const auto times = [](double step, int count) {
        std::vector<double> t;
        for (int i = 0; i < count; ++i)
            t.push_back(i*step);
        return t;
    };
    // 1 Hz, 0..200 s, 60 s segments with a 12 s minimum final piece: the
    // final piece 180..200 is 21 s and stays.
    QCOMPARE(segmentBounds(times(1, 201), 60, 12), Bounds({{0, 59}, {60, 119}, {120, 179}, {180, 200}}));
    // 0..190 s: the final piece 180..190 is 10 s, merged into the one before.
    QCOMPARE(segmentBounds(times(1, 191), 60, 12), Bounds({{0, 59}, {60, 119}, {120, 190}}));
    // Shorter than one segment: one piece.
    QCOMPARE(segmentBounds(times(1, 31), 60, 12), Bounds({{0, 30}}));
    // 0..60 s: the final piece holds one fix (t = 60), merged.
    QCOMPARE(segmentBounds(times(1, 61), 60, 12), Bounds({{0, 60}}));
    // The production lengths on 200 s: one piece.
    QCOMPARE(segmentBounds(times(1, 201), 600, 120), Bounds({{0, 200}}));
    // 0.2 Hz, 0, 5, ..., 200: the final piece 180..200 is 20 s, five fixes:
    // kept with a 12 s minimum, merged with a 30 s one.
    QCOMPARE(segmentBounds(times(5, 41), 60, 12), Bounds({{0, 11}, {12, 23}, {24, 35}, {36, 40}}));
    QCOMPARE(segmentBounds(times(5, 41), 60, 30), Bounds({{0, 11}, {12, 23}, {24, 40}}));
    // Every fix is in exactly one piece and the pieces are consecutive.
    const Bounds pieces = segmentBounds(times(.2, 1234), 60, 12);
    QCOMPARE(pieces.front().first, size_t(0));
    QCOMPARE(pieces.back().second, size_t(1233));
    for (size_t i = 1; i < pieces.size(); ++i)
        QCOMPARE(pieces[i].first, pieces[i-1].second+1);

    // The coarse attitude at the last fix uses the backward difference and
    // does not throw; at the first fix it is the forward one.
    const Samples window = windowOf(QStringLiteral("coarse_maneuver"), Tuning{});
    const Rot3 atLast = coarseAttitude(window, window.gnssTime.size()-1);
    QVERIFY(atLast.matrix().allFinite());
    const size_t n = window.gnssTime.size();
    const Vector3 backward = (window.velocity[n-1]-window.velocity[n-2])/(window.gnssTime[n-1]-window.gnssTime[n-2]);
    const Rot3 expectedLast = rotationAligning(interpolateAt(window.imuTime, window.force, window.gnssTime[n-1]),
                                               backward-kTestGravity);
    QVERIFY(atLast.matrix() == expectedLast.matrix());
    const Vector3 forward = (window.velocity[1]-window.velocity[0])/(window.gnssTime[1]-window.gnssTime[0]);
    const Rot3 expectedFirst = rotationAligning(interpolateAt(window.imuTime, window.force, window.gnssTime[0]),
                                                forward-kTestGravity);
    QVERIFY(coarseAttitude(window, 0).matrix() == expectedFirst.matrix());
}

void FusionKernelTest::yawSigmaIsMarginalAboutTheVertical()
{
    const gtsam::imuBias::ConstantBias bias;

    // No horizontal acceleration: the yaw column of the system is zero, so
    // the system is indeterminate or its covariance is not finite; either
    // way the cap.
    Samples still = boundarySamples(Vector3::Zero());
    still.velocitySigma = Vectors(2, Vector3::Constant(.1));
    const auto stillGraph = buildFactorGraph(still, bias, Tuning{});
    gtsam::Values stillValues;
    stillValues.insert(B(0), bias);
    for (size_t k = 0; k < 2; ++k) {
        stillValues.insert(X(k), gtsam::Pose3());
        stillValues.insert(V(k), Vector3(0, 0, 0));
    }
    QCOMPARE(yawSigmaDeg(stillGraph, stillValues, X(0)), 180.);

    // 2.2 m/s^2 of horizontal acceleration over .83 s, then none, against a
    // velocity sigma of .1 m/s determines the yaw to a few degrees (and the
    // bias, common to both intervals, cancels out of the difference).
    const Samples moving = manoeuvreSamples();
    const auto movingGraph = buildFactorGraph(moving, bias, Tuning{});
    // The linearization point yawed as a whole (attitudes, positions and
    // velocities): gravity is invariant under a yaw and the GNSS sigmas are
    // isotropic, so the graph sees the same body-frame quantities.
    const auto valuesWithYaw = [&](const Rot3 &yaw) {
        gtsam::Values values;
        values.insert(B(0), bias);
        for (size_t k = 0; k < moving.gnssTime.size(); ++k) {
            values.insert(X(k), gtsam::Pose3(yaw, Vector3(yaw.rotate(moving.position[k]))));
            values.insert(V(k), Vector3(yaw.rotate(moving.velocity[k])));
        }
        return values;
    };
    const double level = yawSigmaDeg(movingGraph, valuesWithYaw(Rot3()), X(0));
    qInfo() << "moving: yaw sigma" << level << "deg";
    QVERIFY(std::isfinite(level));
    QVERIFY(level > 0);
    QVERIFY(level < 20);

    // A yaw rotation of the linearization point rotates the body-frame block
    // and the rotation into the navigation frame undoes it.
    const double yawed = yawSigmaDeg(movingGraph, valuesWithYaw(Rot3::Rz(.8)), X(0));
    qInfo() << "moving, yawed by .8 rad: yaw sigma" << yawed << "deg";
    QVERIFY(std::abs(yawed-level) <= 1e-6*level);

    // The single-interval graph of boundarySamples(): the rotation about its
    // constant force axis is unobservable, so the yaw about the vertical is
    // undetermined there too, and the cap says so.
    Samples oneInterval = boundarySamples(Vector3(1, -2, .5));
    oneInterval.velocitySigma = Vectors(2, Vector3::Constant(.1));
    gtsam::Values oneValues;
    oneValues.insert(B(0), bias);
    for (size_t k = 0; k < 2; ++k) {
        oneValues.insert(X(k), gtsam::Pose3());
        oneValues.insert(V(k), Vector3(0, 0, 0));
    }
    QCOMPARE(yawSigmaDeg(buildFactorGraph(oneInterval, bias, Tuning{}), oneValues, X(0)), 180.);
}

void FusionKernelTest::initializerProgressTexts()
{
    // coarse_maneuver is 6 s: one segment whose 60 s prefix covers it, so
    // the four prefix fits at 60 s come first, then the segment fit, each
    // graph build reporting "Integrating IMU factors".
    QStringList texts;
    const Checkpoint collecting([&texts](const QString &text) { texts.append(text); }, {});
    initialize(windowOf(QStringLiteral("coarse_maneuver"), Tuning{}),
               pipelineTuning(QStringLiteral("coarse_maneuver"), Tuning{}), collecting);
    QVERIFY(!texts.isEmpty());

    const QRegularExpression pattern(QStringLiteral("^Segment 1 of 1: (prefix 60 s, )?pass [0-9]+, iteration [0-9]+$"));
    const QString build = QStringLiteral("Integrating IMU factors");
    const QString firstPrefix = QStringLiteral("Segment 1 of 1: prefix 60 s, pass 1, iteration 1");
    const QString firstSegment = QStringLiteral("Segment 1 of 1: pass 1, iteration 1");
    QString firstNonBuild;
    bool segmentSeen = false;
    for (const QString &text : texts) {
        QVERIFY2(text == build || pattern.match(text).hasMatch(), qPrintable(text));
        QVERIFY(!text.startsWith(QStringLiteral("Pass ")));
        if (firstNonBuild.isEmpty() && text != build)
            firstNonBuild = text;
        if (text == firstSegment)
            segmentSeen = true;
        if (segmentSeen)
            QVERIFY2(!text.contains(QStringLiteral("prefix")), qPrintable(text));
    }
    QCOMPARE(firstNonBuild, firstPrefix);
    QVERIFY(segmentSeen);
    QCOMPARE(texts.count(firstPrefix), 4);
    QCOMPARE(texts.count(firstSegment), 1);
    QVERIFY(texts.count(build) >= 5);

    // motion_start grows from 60 s to 120 s and no further.
    texts.clear();
    initialize(windowOf(QStringLiteral("motion_start"), Tuning{}),
               pipelineTuning(QStringLiteral("motion_start"), Tuning{}), collecting);
    bool sixty = false, hundredTwenty = false;
    for (const QString &text : texts) {
        if (!text.contains(QStringLiteral("prefix")))
            continue;
        if (text.contains(QStringLiteral("prefix 60 s")))
            sixty = true;
        else if (text.contains(QStringLiteral("prefix 120 s")))
            hundredTwenty = true;
        else
            QFAIL(qPrintable(text));
    }
    QVERIFY(sixty);
    QVERIFY(hundredTwenty);
}

void FusionKernelTest::initializerDiagnosticsShape()
{
    PipelineTrace trace;
    const Fusion::Result result = runPipeline(
        toChannels(fusionFixture(QStringLiteral("coarse_maneuver"))), Tuning{}, Checkpoint(), &trace);
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
    const QJsonObject diagnostics = diagnosticsOf(result);

    // Every key of a successful fit's diagnostics, `initializer` among them
    // (QJsonObject sorts its keys).
    QCOMPARE(diagnostics.keys(), QStringList({
        "algorithm", "anchor_time_s", "display_position_velocity", "end_s", "gnss_states", "imu_outputs",
        "initialization", "initializer", "input", "limitations", "max_endpoint_correction_deg",
        "max_seed_vs_selected_acceleration_m_s2", "max_seed_vs_selected_angle_deg", "model", "objective",
        "orientation", "quality", "residuals", "seed_comparison_performed", "seeds", "selected_heading_deg",
        "start_s", "stationary_interval_s", "stopping"}));
    QCOMPARE(diagnostics.value("initialization").toString(),
             QStringLiteral("segmented initialization; heading from segment fits"));
    QVERIFY(diagnostics.value("stationary_interval_s").isNull());
    QVERIFY(diagnostics.value("anchor_time_s").isNull());
    QVERIFY(diagnostics.value("selected_heading_deg").isNull());
    const QJsonObject seed = diagnostics.value("seeds").toArray().first().toObject();
    QVERIFY(seed.contains("heading_deg"));
    QVERIFY(seed.value("heading_deg").isNull());

    const QJsonObject initializer = diagnostics.value("initializer").toObject();
    QCOMPARE(initializer.keys(), QStringList({"fallback_segments", "segment_length_s", "segments"}));
    QCOMPARE(initializer.value("segment_length_s").toDouble(), 600.);
    QVERIFY(initializer.value("fallback_segments").toArray().isEmpty());
    const QJsonArray segments = initializer.value("segments").toArray();
    QCOMPARE(segments.size(), 1);
    const QJsonObject segment = segments.first().toObject();
    QCOMPARE(segment.keys(), QStringList({
        "anchor_s", "anchor_sacc_m_s", "end_s", "fallback", "growth_stop", "index", "iterations",
        "prefix_fits", "prefix_iterations", "prefix_length_s", "prefix_on_limit", "prefix_passes",
        "segment_on_limit", "start_s", "yaw_sigma_deg"}));
    QCOMPARE(segment.value("index").toInt(-1), 0);
    QCOMPARE(segment.value("start_s").toDouble(), diagnostics.value("start_s").toDouble());
    QCOMPARE(segment.value("end_s").toDouble(), diagnostics.value("end_s").toDouble());
    QCOMPARE(segment.value("anchor_s").toDouble(), segment.value("start_s").toDouble());
    QCOMPARE(segment.value("anchor_sacc_m_s").toDouble(), .3);
    QCOMPARE(segment.value("prefix_length_s").toDouble(), 60.);
    QCOMPARE(segment.value("prefix_fits").toInt(-1), 4);
    QCOMPARE(segment.value("fallback").toBool(true), false);
    // 6 s of changing acceleration: the yaw is observable at the first length.
    QCOMPARE(segment.value("growth_stop").toString(), QStringLiteral("observable"));
    QVERIFY(segment.value("yaw_sigma_deg").toDouble(999) <= 20);
    QCOMPARE(segment.value("iterations").toInt(-1), trace.initializer.segments[0].iterations);
    QVERIFY(segment.value("iterations").toInt(-1) > 0);
    QVERIFY(segment.value("yaw_sigma_deg").isDouble());
    verifyPrefixBudget(segment, trace.initializer.segments[0]);

    // The trace object: the diagnostics' segment object plus the prefix
    // window, `converged`, the start quaternion and the two biases.
    const QJsonObject traced = traceJson(trace);
    QCOMPARE(traced.keys(), QStringList({"converged", "history", "initializer"}));
    const QJsonObject tracedSegment =
        traced.value("initializer").toObject().value("segments").toArray().first().toObject();
    QCOMPARE(tracedSegment.keys(), QStringList({
        "anchor_s", "anchor_sacc_m_s", "converged", "end_s", "fallback", "growth_stop", "gyro_bias_rad_s",
        "index", "iterations", "prefix_end_s", "prefix_fits", "prefix_iterations", "prefix_length_s",
        "prefix_on_limit", "prefix_passes", "prefix_start_s", "segment_on_limit", "start_gyro_bias_rad_s",
        "start_quaternion_xyzw", "start_s", "yaw_sigma_deg"}));
    QCOMPARE(tracedSegment.value("converged").toBool(false), true);
}

void FusionKernelTest::exactConstantVelocityFit()
{
    // A noiseless constant translation checks the complete optimizer and the
    // dense outputs against a physical trajectory with nonzero velocity.
    const Vector3 speed(12, -4, 2), offset(7, 8, 9);
    const Samples linear = linearSamples(speed, offset);
    const Initialization init = initialize(linear, Tuning{});

    // The yaw is arbitrary and unasserted; the position, velocity and
    // acceleration checks hold for any yaw.
    const FitResult fitted = fitFactorGraph(linear, init.state, Tuning{});
    QVERIFY(fitted.converged);
    QVERIFY(fitted.objective < 1e-12);

    // An exact fit settles in one pass and re-preintegrating at its bias
    // changes nothing; every quality metric is at the noise floor.
    QCOMPARE(fitted.stopping.rule, std::string(StopRule::kSettled));
    QCOMPARE(fitted.stopping.passes, 1);
    QVERIFY(fitted.stopping.repreintegrationCostDifference < 1e-12);
    QVERIFY(fitted.quality.positionNrms < 1e-5 && fitted.quality.velocityNrms < 1e-5
            && fitted.quality.imuNrms < 1e-5);
    QVERIFY(fitted.quality.objectivePerState < 1e-12);
    QVERIFY(std::isfinite(fitted.stopping.lastPassMeanRelativeDecrease));

    const DenseTrajectory output = reconstructTrajectory(linear, fitted);
    QVERIFY(!output.time.empty());
    for (size_t i = 0; i < output.time.size(); ++i) {
        QVERIFY((output.position[i]-offset-output.time[i]*speed).norm() < 1e-8);
        QVERIFY((output.velocity[i]-speed).norm() < 1e-8);
        QVERIFY(output.acceleration[i].norm() < 1e-8);
    }

    // Two fixes are not a fit
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, fittedWindow(linear, 0, .3));
}

void FusionKernelTest::initializerFixturesAreDeterministic()
{
    for (const char *name : {"motion_start", "rest_throughout", "sacc_anchor", "drifting_bias"}) {
        const FusionFixture a = initializerFixture(QLatin1String(name));
        const FusionFixture b = initializerFixture(QLatin1String(name));
        QCOMPARE(a.name, QLatin1String(name));
        QCOMPARE(b.name, a.name);
        QCOMPARE(a.originIndex, b.originIndex);
        QVERIFY(a.expectSuccess);
        const QVector<double> *as[] = {&a.gnssTime, &a.north, &a.east, &a.down, &a.velN, &a.velE, &a.velD,
                                       &a.hAcc, &a.vAcc, &a.sAcc, &a.imuTime, &a.ax, &a.ay, &a.az,
                                       &a.wx, &a.wy, &a.wz, &a.imuTemperature};
        const QVector<double> *bs[] = {&b.gnssTime, &b.north, &b.east, &b.down, &b.velN, &b.velE, &b.velD,
                                       &b.hAcc, &b.vAcc, &b.sAcc, &b.imuTime, &b.ax, &b.ay, &b.az,
                                       &b.wx, &b.wy, &b.wz, &b.imuTemperature};
        for (int c = 0; c < 18; ++c)
            QVERIFY2(sameBitsEverywhere(*as[c], *bs[c]), name);
        // Every recording carries one temperature per IMU sample: constant 25
        // except drifting_bias, whose ramp runs from 25 to 45 degC.
        QCOMPARE(a.imuTemperature.size(), a.imuTime.size());
        if (a.name == QStringLiteral("drifting_bias")) {
            QCOMPARE(a.imuTemperature.first(), 25.);
            QCOMPARE(a.imuTemperature.last(), 45.);
        } else {
            QCOMPARE(a.imuTemperature.first(), kFixtureTemperatureDegC);
            QCOMPARE(a.imuTemperature.last(), kFixtureTemperatureDegC);
        }
    }
    for (const FusionFixture &golden : fusionFixtures()) {
        QCOMPARE(golden.imuTemperature.size(), golden.imuTime.size());
        for (double temperature : golden.imuTemperature)
            QCOMPARE(temperature, kFixtureTemperatureDegC);
    }
    // Any other name is nothing, and the golden fixtures are untouched.
    QVERIFY(initializerFixture(QStringLiteral("coarse_linear")).name.isEmpty());
    QCOMPARE(fusionFixtures().size(), 12);
}

void FusionKernelTest::fitTraceMatchesGolden_data()
{
    QTest::addColumn<QString>("name");
    for (const FusionFixture &fixture : fusionFixtures()) {
        if (fixture.expectSuccess)
            QTest::newRow(qPrintable(fixture.name)) << fixture.name;
    }
}

void FusionKernelTest::fitTraceMatchesGolden()
{
    QFETCH(QString, name);
    const FusionGolden golden = loadFusionGolden(name);

    PipelineTrace trace;
    const Fusion::Result result =
        runPipeline(toChannels(fusionFixture(name)), Tuning{}, Checkpoint(), &trace);
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));

    // The segment account first, then every optimizer iteration in order: the
    // first difference reported is the first stage that diverged. traceJson()
    // is the function the capture tool wrote the golden with (fusiontrace.h).
    const QJsonObject got = traceJson(trace);
    QCOMPARE(got.value("history").toArray().size(), golden.trace.value("history").toArray().size());
    const QString difference = compareJson(QStringLiteral("trace"), got, golden.trace);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

void FusionKernelTest::biasSettledByCostTest()
{
    // The spec's bias-settled test, on the production tuning. Under the
    // bias-shift rule this fixture needed a third pass of one iteration that
    // lowered the cost by 8e-15 to prove the bias had stopped moving (under
    // that rule this fixture's history had passes of 4, 2, 1 iterations);
    // under the cost test the second pass's re-preintegration changes the cost
    // by 3.8e-12 relative and the fit is converged there.
    PipelineTrace trace;
    const Fusion::Result result = runPipeline(
        toChannels(fusionFixture(QStringLiteral("coarse_maneuver"))), Tuning{}, Checkpoint(), &trace);
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
    QVERIFY(trace.converged);
    QVERIFY(trace.stopping.rule == StopRule::kSettled);
    qInfo() << "coarse_maneuver converged after" << trace.stopping.passes << "passes";
    QVERIFY(trace.stopping.passes <= 2);

    std::set<int> outers;
    for (const FitIteration &h : trace.history)
        outers.insert(h.outer);
    QCOMPARE(int(outers.size()), trace.stopping.passes);

    // The cost test re-derived from the trace: the reported objective is the
    // cost of the graph rebuilt at the fitted bias, the last history row's
    // `after` is the pass's final cost.
    const QJsonObject diagnostics = diagnosticsOf(result);
    const QJsonObject stopping = diagnostics.value("stopping").toObject();
    const double after = trace.history.back().after;
    const double objective = diagnostics.value("objective").toDouble();
    const double ratio = std::abs(objective-after)/std::max(1., after);
    QVERIFY(std::abs(objective-after) <= 1e-6*std::max(1., after));
    QVERIFY(std::abs(stopping.value("repreintegration_cost_difference").toDouble()-ratio) <= 1e-9);
    QCOMPARE(stopping.value("rule").toString(), QStringLiteral("settled"));
    QCOMPARE(stopping.value("passes").toInt(), trace.stopping.passes);
    QCOMPARE(stopping.value("bias_settled_tolerance").toDouble(), 1e-6);
    const QJsonObject slowTail = stopping.value("slow_tail").toObject();
    QCOMPARE(slowTail.value("window").toInt(), 20);
    QCOMPARE(slowTail.value("max_mean_relative_decrease").toDouble(), 1e-4);
    QCOMPARE(slowTail.value("max_nrms").toDouble(), 2.);
    const QJsonObject seed = diagnostics.value("seeds").toArray().first().toObject();
    QCOMPARE(seed.value("converged").toBool(false), true);
    QCOMPARE(seed.value("iterations").toInt(), int(trace.history.size()));
    QCOMPARE(diagnostics.value("algorithm").toString(), QStringLiteral("batch-temperature-bias-v3"));

    // The quality metrics recomputed from the residuals array: 28 states, so
    // 28 position and velocity factors of dimension 3 and 27 IMU factors of
    // dimension 9.
    QCOMPARE(diagnostics.value("gnss_states").toInt(), 28);
    double sumPosition = 0, sumVelocity = 0, sumImu = 0;
    for (const QJsonValue &entry : diagnostics.value("residuals").toArray()) {
        const QJsonObject r = entry.toObject();
        const QString kind = r.value("kind").toString();
        const double error = r.value("squared_whitened_error").toDouble();
        if (kind == QStringLiteral("position"))
            sumPosition += error;
        else if (kind == QStringLiteral("velocity"))
            sumVelocity += error;
        else if (kind == QStringLiteral("imu"))
            sumImu += error;
    }
    const QJsonObject quality = diagnostics.value("quality").toObject();
    QVERIFY(withinPortableBound(quality.value("position_nrms").toDouble(), std::sqrt(sumPosition/(3*28))));
    QVERIFY(withinPortableBound(quality.value("velocity_nrms").toDouble(), std::sqrt(sumVelocity/(3*28))));
    QVERIFY(withinPortableBound(quality.value("imu_nrms").toDouble(), std::sqrt(sumImu/(9*27))));
    QVERIFY(sameRecomputedValue(quality.value("objective_per_state").toDouble(), objective/28.));
}

void FusionKernelTest::slowTailAtTheIterationLimit_data()
{
    QTest::addColumn<double>("maxNrms");
    QTest::addColumn<double>("maxMeanRelativeDecrease");
    QTest::addColumn<bool>("accepted");
    QTest::newRow("accepted") << 2. << 1e-4 << true;
    QTest::newRow("nrms bound fails") << 0. << 1e-4 << false;
    QTest::newRow("decrease bound fails") << 2. << 0. << false;
}

void FusionKernelTest::slowTailAtTheIterationLimit()
{
    // The spec's slow-tail test. A negative relative tolerance means no pass
    // ever settles (before - after >= -1e-6 by the cost-increase guard, so it
    // is never <= -max(1, before)), so every pass runs its 25 iterations: the
    // first four do the work and the rest are steps of order 1e-15 or exact
    // no-ops (GTSAM's LM leaves the values untouched when it rejects a step).
    // The last 20 iterations of pass five therefore have a mean relative
    // decrease of about 0, and the fit's position and velocity normalized RMS
    // are about 0.098 and 0.19: accepted with the production bounds, refused
    // with either bound at zero (the comparisons are strict).
    QFETCH(double, maxNrms);
    QFETCH(double, maxMeanRelativeDecrease);
    QFETCH(bool, accepted);

    Tuning tuning;
    tuning.relativeTolerance = -1;
    tuning.maxIterations = 25;
    tuning.slowTailMaxNrms = maxNrms;
    tuning.slowTailMaxMeanRelativeDecrease = maxMeanRelativeDecrease;
    PipelineTrace trace;
    const Fusion::Result result = runPipeline(
        toChannels(fusionFixture(QStringLiteral("coarse_maneuver"))), tuning, Checkpoint(), &trace);

    QCOMPARE(trace.history.size(), size_t(125));
    for (const FitIteration &h : trace.history)
        QVERIFY2(h.before >= h.after, qPrintable(QStringLiteral("pass %1, iteration %2").arg(h.outer).arg(h.iteration)));
    QCOMPARE(trace.stopping.passes, 5);
    const QJsonObject diagnostics = diagnosticsOf(result);
    const QJsonObject stopping = diagnostics.value("stopping").toObject();
    const QJsonObject quality = diagnostics.value("quality").toObject();
    QCOMPARE(stopping.value("passes").toInt(), 5);
    const double meanDecrease = stopping.value("last_pass_mean_relative_decrease").toDouble(-1);

    if (accepted) {
        QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
        QVERIFY(result.reason.isEmpty());
        for (const QVector<double> *channel : { &result.time, &result.north, &result.east, &result.down,
                                                &result.velN, &result.velE, &result.velD,
                                                &result.accN, &result.accE, &result.accD,
                                                &result.roll, &result.pitch, &result.yaw,
                                                &result.qx, &result.qy, &result.qz, &result.qw })
            QVERIFY(channel->size() > 0);
        QVERIFY(trace.converged);
        QVERIFY(trace.stopping.rule == StopRule::kSlowTailAccepted);
        QCOMPARE(stopping.value("rule").toString(), QStringLiteral("slow tail accepted"));
        QVERIFY(meanDecrease >= 0 && meanDecrease < 1e-4);
        QVERIFY(quality.value("position_nrms").toDouble(9) < 2);
        QVERIFY(quality.value("velocity_nrms").toDouble(9) < 2);
        const QJsonObject seed = diagnostics.value("seeds").toArray().first().toObject();
        QCOMPARE(seed.value("converged").toBool(false), true);
        QCOMPARE(seed.value("iterations").toInt(), 125);
    } else {
        QVERIFY(result.outcome == Fusion::Outcome::SolverFailed);
        QCOMPARE(result.reason,
                 QStringLiteral("Batch fusion did not converge (iteration limit); sensor fusion unavailable"));
        QVERIFY(!trace.converged);
        QVERIFY(trace.stopping.rule == StopRule::kIterationLimit);
        QCOMPARE(diagnostics.keys(), kCompletedPassFailureKeys);
        QCOMPARE(diagnostics.value("failure").toString(), result.reason);
        QCOMPARE(stopping.value("rule").toString(), QStringLiteral("iteration limit"));
        const QJsonObject slowTail = stopping.value("slow_tail").toObject();
        QCOMPARE(slowTail.value("max_nrms").toDouble(-1), maxNrms);
        QCOMPARE(slowTail.value("max_mean_relative_decrease").toDouble(-1), maxMeanRelativeDecrease);
        QVERIFY(quality.value("position_nrms").toDouble() > 0);
        // A rejected LM step is an exact no-op and an accepted one lowers the
        // cost, so the mean is never negative and a zero bound always refuses.
        QVERIFY(meanDecrease >= 0);
        QVERIFY(allChannelsEmpty(result));
    }
}

void FusionKernelTest::nonConvergenceIsSolverFailure()
{
    // One iteration per bias pass and a negative tolerance: no pass can settle
    // on any platform, so the fifth pass ends at its one-iteration limit
    // deterministically, and one iteration cannot fill the 20-iteration
    // slow-tail window, so the slow tail is not judged.
    Tuning tuning;
    tuning.maxIterations = 1;
    tuning.relativeTolerance = -1;
    PipelineTrace trace;
    const Fusion::Result result = runPipeline(
        toChannels(fusionFixture(QStringLiteral("coarse_maneuver"))), tuning, Checkpoint(), &trace);

    QVERIFY(result.outcome == Fusion::Outcome::SolverFailed);
    QCOMPARE(result.reason,
             QStringLiteral("Batch fusion did not converge (iteration limit); sensor fusion unavailable"));
    QVERIFY(!trace.converged);
    QCOMPARE(trace.history.size(), size_t(5));
    QVERIFY(trace.stopping.rule == StopRule::kIterationLimit);
    QCOMPARE(trace.stopping.passes, 5);

    const QJsonObject diagnostics = diagnosticsOf(result);
    QCOMPARE(diagnostics.keys(), kCompletedPassFailureKeys);
    QCOMPARE(diagnostics.value("failure").toString(), result.reason);
    QCOMPARE(diagnostics.value("stopping").toObject().value("passes").toInt(), 5);
    QVERIFY(allChannelsEmpty(result));
}

void FusionKernelTest::biasNeverSettlesIsSolverFailure()
{
    // A negative bias-settled tolerance: the cost test can never pass, so
    // every settled pass is followed by another until the fifth.
    Tuning tuning;
    tuning.biasSettledTolerance = -1;
    PipelineTrace trace;
    const Fusion::Result result = runPipeline(
        toChannels(fusionFixture(QStringLiteral("coarse_linear"))), tuning, Checkpoint(), &trace);

    QVERIFY(result.outcome == Fusion::Outcome::SolverFailed);
    QCOMPARE(result.reason,
             QStringLiteral("Batch fusion did not converge (bias not settled); sensor fusion unavailable"));
    QVERIFY(!trace.converged);
    QVERIFY(trace.stopping.rule == StopRule::kBiasNotSettled);
    QCOMPARE(trace.stopping.passes, 5);

    const QJsonObject diagnostics = diagnosticsOf(result);
    QCOMPARE(diagnostics.keys(), kCompletedPassFailureKeys);
    QCOMPARE(diagnostics.value("failure").toString(), result.reason);
    QVERIFY(diagnostics.value("quality").toObject().value("position_nrms").toDouble(9) < 1e-3);
    QCOMPARE(diagnostics.value("stopping").toObject().value("bias_settled_tolerance").toDouble(), -1.);
    QVERIFY(allChannelsEmpty(result));
}

void FusionKernelTest::failureDiagnosticsShape()
{
    // The `cost increased` shape, proven on the writer directly: LM rejects
    // an increasing step by construction, so the guard is reachable only
    // through non-finite arithmetic, which no deterministic input forces.
    const Tuning tuning;
    Stopping s;
    s.rule = StopRule::kCostIncreased;
    s.passes = 2;
    s.biasSettledTolerance = tuning.biasSettledTolerance;
    s.slowTailWindow = tuning.slowTailWindow;
    s.slowTailMaxMeanRelativeDecrease = tuning.slowTailMaxMeanRelativeDecrease;
    s.slowTailMaxNrms = tuning.slowTailMaxNrms;
    QVERIFY(std::isnan(s.lastPassMeanRelativeDecrease) && std::isnan(s.repreintegrationCostDifference));

    const QJsonObject diagnostics = failureDiagnostics(QStringLiteral("Nonfinite or increasing optimizer cost"), &s);
    QCOMPARE(diagnostics.keys(), QStringList({QStringLiteral("algorithm"), QStringLiteral("failure"),
                                              QStringLiteral("stopping")}));
    QCOMPARE(diagnostics.value("algorithm").toString(), QStringLiteral("batch-temperature-bias-v3"));
    QCOMPARE(diagnostics.value("failure").toString(), QStringLiteral("Nonfinite or increasing optimizer cost"));
    const QJsonObject stopping = diagnostics.value("stopping").toObject();
    QCOMPARE(stopping.value("rule").toString(), QStringLiteral("cost increased"));
    QCOMPARE(stopping.value("passes").toInt(), 2);
    QVERIFY(stopping.value("last_pass_mean_relative_decrease").isNull());
    QVERIFY(stopping.value("repreintegration_cost_difference").isNull());
    QCOMPARE(stopping.value("slow_tail").toObject().value("window").toInt(), 20);

    // A rejection: the algorithm and the reason, nothing else.
    QCOMPARE(failureDiagnostics(QStringLiteral("x")).keys(),
             QStringList({QStringLiteral("algorithm"), QStringLiteral("failure")}));

    // The exception the kernel throws for it is a std::runtime_error carrying
    // the account, with the text that becomes the SolverFailed reason (and
    // that the initializer tests' forcings throw).
    bool caught = false;
    try {
        throw FitFailure("Nonfinite or increasing optimizer cost", s);
    } catch (const std::runtime_error &e) {
        caught = true;
        QCOMPARE(QString::fromUtf8(e.what()), QStringLiteral("Nonfinite or increasing optimizer cost"));
    }
    QVERIFY(caught);
}

void FusionKernelTest::startsInMotionGrowsToTheManoeuvre()
{
    // Spec section 10, "starts in motion". From the fixture: every sAcc is
    // .3, so the anchor is the first fix and the 60 s window is [0, 30],
    // which holds no horizontal acceleration (yaw unobservable: sigma above
    // 20 or the cap); the 120 s window [0, 60] holds the manoeuvre at
    // 40-50 s and determines the yaw to a few degrees.
    const InitializerRun run = runInitializerFixture(QStringLiteral("motion_start"), Tuning{});
    QVERIFY2(run.result.outcome == Fusion::Outcome::Succeeded, qPrintable(run.result.reason));
    QVERIFY(run.trace.converged);
    QCOMPARE(run.segments.size(), 1);
    const QJsonObject segment = run.segments.first().toObject();
    const SegmentAccount &s = run.trace.initializer.segments.at(0);
    QCOMPARE(segment.value("prefix_length_s").toDouble(), 120.);
    QCOMPARE(segment.value("prefix_fits").toInt(-1), 8);
    QCOMPARE(s.prefixYawSigmaDeg.size(), size_t(2));
    qInfo() << "motion_start: yaw sigmas" << s.prefixYawSigmaDeg[0] << s.prefixYawSigmaDeg[1]
            << "deg, growth stop" << s.growthStop.c_str();
    QVERIFY(s.prefixYawSigmaDeg[0] > 20);
    QVERIFY(s.prefixYawSigmaDeg[1] < 20);
    QVERIFY(segment.value("yaw_sigma_deg").toDouble(999) < 20);
    QCOMPARE(segment.value("growth_stop").toString(), QStringLiteral("observable"));
    QCOMPARE(segment.value("fallback").toBool(true), false);
    QCOMPARE(s.prefixStart, 0.);
    QCOMPARE(s.prefixEnd, 60.);
    verifyPrefixBudget(segment, s);

    // The full fit against the truth Rz(psi), cos psi = .6, sin psi = .8:
    // every output sample within 2 degrees (the unwrapped channels; the
    // fixture never turns).
    const Rot3 truth = rotationFromMatrix(.6, -.8, 0, .8, .6, 0, 0, 0, 1);
    const Vector3 truthRpy = truth.rpy()*180/kPi;
    QVERIFY(std::abs(truthRpy.z()-53.13) < .01);
    QVERIFY(!run.result.yaw.isEmpty());
    for (qsizetype i = 0; i < run.result.yaw.size(); ++i) {
        QVERIFY2(std::abs(angleDifference(run.result.yaw[i], truthRpy.z())) < 2, qPrintable(QString::number(i)));
        QVERIFY2(std::abs(angleDifference(run.result.roll[i], 0)) < 2, qPrintable(QString::number(i)));
        QVERIFY2(std::abs(angleDifference(run.result.pitch[i], 0)) < 2, qPrintable(QString::number(i)));
    }
}

void FusionKernelTest::atRestPrefixStopsGrowing()
{
    // Spec section 10, "at rest throughout". From the fixture: every sAcc is
    // .1, so the anchor is the first fix (the fixture's t = .1, which is 0
    // in the kernel's time, relative to the first fix); the 60 s window is
    // [0, 30] (clipped at the segment start) and has no yaw information, and
    // neither has the 120 s window [0, 60]: its sigma is not 20 % below the
    // first, so growth stops there (no_gain) although the 300 s segment is
    // far from covered.
    const InitializerRun run = runInitializerFixture(QStringLiteral("rest_throughout"), Tuning{});
    QVERIFY2(run.result.outcome == Fusion::Outcome::Succeeded, qPrintable(run.result.reason));
    QVERIFY(run.trace.converged);
    QCOMPARE(run.segments.size(), 1);
    const QJsonObject segment = run.segments.first().toObject();
    const SegmentAccount &s = run.trace.initializer.segments.at(0);
    QCOMPARE(segment.value("prefix_length_s").toDouble(), 120.);
    QCOMPARE(segment.value("prefix_fits").toInt(-1), 8);
    QCOMPARE(segment.value("growth_stop").toString(), QStringLiteral("no_gain"));
    QCOMPARE(s.prefixYawSigmaDeg.size(), size_t(2));
    qInfo() << "rest_throughout: yaw sigmas" << s.prefixYawSigmaDeg[0] << s.prefixYawSigmaDeg[1]
            << "deg; prefix on limit" << segment.value("prefix_on_limit").toBool()
            << "after" << segment.value("prefix_iterations").toInt() << "iterations";
    QVERIFY(s.prefixYawSigmaDeg[0] > 20);
    QVERIFY(s.prefixYawSigmaDeg[1] > 20);
    QVERIFY(s.prefixYawSigmaDeg[1] > .8*s.prefixYawSigmaDeg[0]);
    // The window's fixes: from the segment's first fix to the last fix at
    // most 60 s after the anchor (the fixes are .2 s apart).
    QCOMPARE(s.prefixStart, s.start);
    QCOMPARE(s.anchorTime, 0.);
    QVERIFY(s.prefixEnd <= 60);
    QVERIFY(s.prefixEnd > 60-.2-1e-6);
    QVERIFY(s.end > 299);
    QCOMPARE(segment.value("fallback").toBool(true), false);
    verifyPrefixBudget(segment, s);

    // Roll and pitch of every output sample within .5 degrees of the truth
    // Ry(theta), sin theta = .28 (pitch about 16.26 degrees, roll 0); the yaw
    // is arbitrary and unasserted.
    const Rot3 truth = rotationFromMatrix(.96, 0, .28, 0, 1, 0, -.28, 0, .96);
    const Vector3 truthRpy = truth.rpy()*180/kPi;
    QVERIFY(std::abs(truthRpy.y()-16.26) < .01);
    QVERIFY(!run.result.roll.isEmpty());
    for (qsizetype i = 0; i < run.result.roll.size(); ++i) {
        QVERIFY2(std::abs(angleDifference(run.result.roll[i], truthRpy.x())) < .5, qPrintable(QString::number(i)));
        QVERIFY2(std::abs(angleDifference(run.result.pitch[i], truthRpy.y())) < .5, qPrintable(QString::number(i)));
    }
}

void FusionKernelTest::smallestSaccFixIsTheAnchor()
{
    // Spec section 10, the sAcc anchor. From the fixture: every fix carries
    // sAcc 2 except the one at 200 s with .3, so that fix is the anchor, the
    // 60 s window centred on it is 170..230 (unclipped) and contains the
    // manoeuvre at 190-200 s.
    const InitializerRun run = runInitializerFixture(QStringLiteral("sacc_anchor"), Tuning{});
    QVERIFY2(run.result.outcome == Fusion::Outcome::Succeeded, qPrintable(run.result.reason));
    QCOMPARE(run.segments.size(), 1);
    const QJsonObject segment = run.segments.first().toObject();
    const SegmentAccount &s = run.trace.initializer.segments.at(0);
    QCOMPARE(segment.value("anchor_s").toDouble(), 200.);
    QCOMPARE(segment.value("anchor_sacc_m_s").toDouble(), .3);
    QCOMPARE(s.prefixStart, 170.);
    QCOMPARE(s.prefixEnd, 230.);
    QCOMPARE(segment.value("prefix_length_s").toDouble(), 60.);
    QCOMPARE(segment.value("prefix_fits").toInt(-1), 4);
    qInfo() << "sacc_anchor: yaw sigma" << segment.value("yaw_sigma_deg").toDouble() << "deg";
    QVERIFY(segment.value("yaw_sigma_deg").toDouble(999) < 20);
    QCOMPARE(segment.value("fallback").toBool(true), false);
    verifyPrefixBudget(segment, s);

    // The start attitude at the segment's first fix is the prefix fit's
    // carried back by the gyro with the prefix fit's bias: the initializer
    // performs this very call.
    const Samples window = windowOf(QStringLiteral("sacc_anchor"), Tuning{});
    const Rot3 expected = propagateAttitude(window, s.prefixRotation, s.prefixStart, s.start, s.prefixGyroBias);
    QVERIFY(Rot3::Logmap(s.startRotation.between(expected)).norm() < 1e-9);
    QVERIFY(s.startGyroBias == s.prefixGyroBias);
}

void FusionKernelTest::driftingBiasSegmentsConverge()
{
    // Spec section 10, the drifting bias, under 60 s segments (the spec's
    // 600:120 ratio). From the fixture: 201 fixes at 1 Hz cut into (0, 59),
    // (60, 119), (120, 179) and (180, 200) (the last piece is 20 s, above the
    // 12 s minimum); each anchor is its segment's first fix and the manoeuvre
    // lies in the first 20 s of every 30 s block, inside the 30 s
    // half-window, so every prefix is observable at 60 s.
    Tuning tuning;
    tuning.segmentLength = 60;
    tuning.minFinalSegment = 12;
    const InitializerRun run = runInitializerFixture(QStringLiteral("drifting_bias"), tuning);
    const QJsonObject initializer = run.diagnostics.value("initializer").toObject();
    QCOMPARE(run.trace.initializer.segments.size(), size_t(4));
    QCOMPARE(run.trace.initializer.segmentLength, 60.);
    const double starts[] = {0, 60, 120, 180}, ends[] = {59, 119, 179, 200};
    for (size_t i = 0; i < 4; ++i) {
        const SegmentAccount &s = run.trace.initializer.segments[i];
        QCOMPARE(s.index, int(i));
        QCOMPARE(s.start, starts[i]);
        QCOMPARE(s.end, ends[i]);
        QCOMPARE(s.anchorTime, starts[i]);
        qInfo() << "drifting_bias: segment" << i << "yaw sigma" << s.yawSigmaDeg << "deg, segment fit"
                << s.iterations << "iterations, converged" << s.converged
                << ", gyro bias z" << s.gyroBias.z()*180/kPi << "deg/s";
        QVERIFY(s.converged);
        QVERIFY(!s.fallback);
        QVERIFY(s.iterations > 0);
        QCOMPARE(s.prefixLength, 60.);
        QCOMPARE(s.prefixFits, 4);
        QVERIFY(s.yawSigmaDeg < 20);
        QCOMPARE(s.prefixPasses, 1);
        QVERIFY(s.prefixIterations <= 50);
    }

    // The full fit started from the stitched state and ran with the
    // temperature model; the drift is exactly linear in the fixture's
    // temperature ramp, so the model represents it.
    QVERIFY(!run.trace.history.empty());
    QVERIFY(std::isfinite(run.trace.history.front().before));
    qInfo() << "drifting_bias: full fit" << run.trace.history.size() << "iterations, rule"
            << run.trace.stopping.rule.c_str() << ", outcome" << int(run.result.outcome)
            << qPrintable(run.result.reason);
    QVERIFY2(run.result.outcome == Fusion::Outcome::Succeeded, qPrintable(run.result.reason));
    QCOMPARE(initializer.value("segment_length_s").toDouble(), 60.);
    QCOMPARE(run.segments.size(), 4);
    for (qsizetype i = 0; i < 4; ++i) {
        const QJsonObject segment = run.segments.at(i).toObject();
        QCOMPARE(segment.value("index").toInt(-1), int(i));
        QCOMPARE(segment.value("start_s").toDouble(), starts[i]);
        QCOMPARE(segment.value("end_s").toDouble(), ends[i]);
        QCOMPARE(segment.value("fallback").toBool(true), false);
        QVERIFY(segment.value("iterations").toInt(-1) > 0);
        QCOMPARE(segment.value("prefix_length_s").toDouble(), 60.);
        QCOMPARE(segment.value("prefix_fits").toInt(-1), 4);
        QVERIFY(segment.value("yaw_sigma_deg").toDouble(999) < 20);
        verifyPrefixBudget(segment, run.trace.initializer.segments[size_t(i)]);
    }
    QVERIFY(initializer.value("fallback_segments").toArray().isEmpty());
    const QJsonObject seed = run.diagnostics.value("seeds").toArray().first().toObject();
    const QJsonArray bias = seed.value("gyro_bias_rad_s").toArray();
    QCOMPARE(bias.size(), 3);
    for (const QJsonValue &component : bias)
        QVERIFY(std::isfinite(component.toDouble(std::numeric_limits<double>::quiet_NaN())));

    // Spec section 10, the drifting-bias recording under the temperature
    // model: b1 within 20 % of the truth in at most 30 iterations (the sum
    // over the full fit's passes). From the fixture's construction: the z
    // bias .3 + t / 200 deg/s over the ramp 25 + t / 10 degC is 0.05 deg/s
    // per degC (b1z), T_ref = 35, and b0z = .8 deg/s, the bias at T_ref.
    QVERIFY(run.trace.converged);
    QCOMPARE(seed.value("iterations").toInt(999), int(run.trace.history.size()));
    QVERIFY(seed.value("iterations").toInt(999) <= 30);
    const QJsonObject gyroBias = run.diagnostics.value("model").toObject().value("gyro_bias").toObject();
    const QJsonArray b1 = gyroBias.value("b1_rad_s_per_degc").toArray();
    QCOMPARE(b1.size(), 3);
    const double b1z = .05*kPi/180;
    qInfo() << "drifting_bias: b1" << b1.at(0).toDouble()*180/kPi << b1.at(1).toDouble()*180/kPi
            << b1.at(2).toDouble()*180/kPi << "deg/s/degC (truth 0, 0, .05); b0"
            << bias.at(0).toDouble()*180/kPi << bias.at(1).toDouble()*180/kPi << bias.at(2).toDouble()*180/kPi
            << "deg/s (truth .2, -.15, .8); t_ref" << gyroBias.value("t_ref_degc").toDouble()
            << "degC; full fit" << run.trace.history.size() << "iterations";
    QVERIFY(std::abs(b1.at(2).toDouble()-b1z) <= .2*b1z);
    QVERIFY(std::abs(b1.at(0).toDouble()) <= .2*b1z);
    QVERIFY(std::abs(b1.at(1).toDouble()) <= .2*b1z);
    QVERIFY(std::abs(gyroBias.value("t_ref_degc").toDouble(-1)-35) < 1e-9);
    const QJsonArray b0 = gyroBias.value("b0_rad_s").toArray();
    QCOMPARE(b0.size(), 3);
    for (int i = 0; i < 3; ++i)
        QVERIFY(b0.at(i).toDouble() == bias.at(i).toDouble());
    QVERIFY(std::abs(b0.at(2).toDouble()-.8*kPi/180) < .1*kPi/180);

    // The residuals: the two priors last, in order, and one IMU factor per
    // interval (201 states).
    const QJsonArray residuals = run.diagnostics.value("residuals").toArray();
    QVERIFY(residuals.size() >= 2);
    QCOMPARE(residuals.last().toObject().value("kind").toString(), QStringLiteral("slope_prior"));
    QCOMPARE(residuals.at(residuals.size()-2).toObject().value("kind").toString(), QStringLiteral("bias_prior"));
    int imuResiduals = 0;
    for (const QJsonValue &entry : residuals) {
        if (entry.toObject().value("kind").toString() == QStringLiteral("imu"))
            ++imuResiduals;
    }
    QCOMPARE(imuResiduals, 200);

    // The attitude never rotates in the fixture: every output sample within
    // 2 degrees of level and of the first sample's yaw.
    QVERIFY(!run.result.roll.isEmpty());
    for (qsizetype i = 0; i < run.result.roll.size(); ++i) {
        QVERIFY2(std::abs(angleDifference(run.result.roll[i], 0)) < 2, qPrintable(QString::number(i)));
        QVERIFY2(std::abs(angleDifference(run.result.pitch[i], 0)) < 2, qPrintable(QString::number(i)));
        QVERIFY2(std::abs(angleDifference(run.result.yaw[i], run.result.yaw[0])) < 2, qPrintable(QString::number(i)));
    }
}

void FusionKernelTest::allPrefixFitsFailFallsBack_data()
{
    QTest::addColumn<QString>("name");
    QTest::newRow("coarse_maneuver") << QStringLiteral("coarse_maneuver");
    QTest::newRow("motion_start") << QStringLiteral("motion_start");
}

void FusionKernelTest::allPrefixFitsFailFallsBack()
{
    // Spec section 10, "a segment whose prefix fits all fail". The forcing: a
    // progress function that throws FitFailure at the first iteration
    // boundary of every prefix fit (the texts containing ": prefix "), so
    // every start fails at every length; the window grows to the segment,
    // the segment falls back to the coarse attitude carried with zero bias,
    // no segment fit runs, and the full fit (whose texts are "Pass ...") is
    // not affected. The kernel promises nothing about a throwing progress
    // function, which is why this forcing lives only here.
    QFETCH(QString, name);
    const Checkpoint failingPrefixes(
        [](const QString &text) {
            if (text.contains(QStringLiteral(": prefix ")))
                throw FitFailure(std::string("Nonfinite or increasing optimizer cost"), Stopping{});
        },
        [] { return false; });
    const InitializerRun run = runInitializerFixture(name, Tuning{}, failingPrefixes);
    QCOMPARE(run.trace.initializer.segments.size(), size_t(1));
    const SegmentAccount &s = run.trace.initializer.segments.at(0);
    QVERIFY(s.fallback);
    QCOMPARE(s.iterations, 0);
    QVERIFY(!s.converged);
    QCOMPARE(s.growthStop, std::string("all_failed"));
    QVERIFY(std::isnan(s.yawSigmaDeg));
    QCOMPARE(s.prefixIterations, 0);
    QCOMPARE(s.prefixPasses, 0);
    QVERIFY(s.startGyroBias.isZero(0));
    QVERIFY(s.gyroBias.isZero(0));
    QVERIFY(!run.trace.history.empty());
    QVERIFY(run.trace.stopping.passes >= 1);

    if (name == QStringLiteral("coarse_maneuver")) {
        // 6 s: the first window covers, so four fits fail and the fallback
        // start is the coarse attitude at the first fix carried with zero
        // bias: exactly the start the kernel used before the segmented
        // initializer, which this fixture's history proves converges.
        QVERIFY2(run.result.outcome == Fusion::Outcome::Succeeded, qPrintable(run.result.reason));
        QCOMPARE(run.segments.size(), 1);
        const QJsonObject segment = run.segments.first().toObject();
        QCOMPARE(segment.value("fallback").toBool(false), true);
        QCOMPARE(run.diagnostics.value("initializer").toObject().value("fallback_segments").toArray(),
                 QJsonArray{0});
        QCOMPARE(segment.value("prefix_fits").toInt(-1), 4);
        QCOMPARE(segment.value("prefix_length_s").toDouble(), 60.);
        QVERIFY(segment.value("yaw_sigma_deg").isNull());
        QCOMPARE(segment.value("iterations").toInt(-1), 0);
        QCOMPARE(segment.value("growth_stop").toString(), QStringLiteral("all_failed"));
        QCOMPARE(s.prefixYawSigmaDeg.size(), size_t(1));
        // Bitwise: propagateAttitude() with equal times returns its input.
        const Samples window = windowOf(name, Tuning{});
        QVERIFY(s.startRotation.matrix() == coarseAttitude(window, 0).matrix());
        QCOMPARE(run.trace.stopping.rule, std::string(StopRule::kSettled));
    } else {
        // 90 s, anchored at the first fix: a window centred there reaches
        // half its nominal length forward, so [0, 30] and [0, 60] do not
        // cover and the all-fail rule grows twice; the 240 s window is the
        // segment, and twelve fits fail. The full fit ran from a zero-yaw
        // coarse start; its outcome is logged, not asserted.
        QCOMPARE(s.prefixFits, 12);
        QCOMPARE(s.prefixLength, 240.);
        QCOMPARE(s.prefixYawSigmaDeg.size(), size_t(3));
        for (const double sigma : s.prefixYawSigmaDeg)
            QVERIFY(std::isnan(sigma));
        QCOMPARE(s.prefixStart, s.start);
        QCOMPARE(s.prefixEnd, s.end);
        qInfo() << "motion_start fallback: full fit outcome" << int(run.result.outcome)
                << qPrintable(run.result.reason) << ", rule" << run.trace.stopping.rule.c_str()
                << "after" << run.trace.history.size() << "iterations";
    }
}

void FusionKernelTest::prefixFitsFailAfterACompletedLength()
{
    // The other way to `all_failed`: a length completes, then every start of
    // every longer window fails. The account must describe the fallback,
    // not the completed fit that was not used. The forcing spares the 60 s
    // texts only: on motion_start the 60 s window [0, 30] completes with an
    // unobservable yaw (sigma above 20; a first length never stops on gain),
    // the 120 s window [0, 60] fails all four starts and does not cover, and
    // the 240 s window is the segment and fails all four.
    const Checkpoint failingLongerPrefixes(
        [](const QString &text) {
            if (text.contains(QStringLiteral(": prefix ")) && !text.contains(QStringLiteral(": prefix 60 s")))
                throw FitFailure(std::string("Nonfinite or increasing optimizer cost"), Stopping{});
        },
        [] { return false; });
    const QString name = QStringLiteral("motion_start");
    const InitializerRun run = runInitializerFixture(name, Tuning{}, failingLongerPrefixes);
    QCOMPARE(run.trace.initializer.segments.size(), size_t(1));
    const SegmentAccount &s = run.trace.initializer.segments.at(0);
    QCOMPARE(s.prefixFits, 12);
    QCOMPARE(s.prefixLength, 240.);
    QCOMPARE(s.prefixYawSigmaDeg.size(), size_t(3));
    QVERIFY(std::isfinite(s.prefixYawSigmaDeg[0]) && s.prefixYawSigmaDeg[0] > 20);
    QVERIFY(std::isnan(s.prefixYawSigmaDeg[1]));
    QVERIFY(std::isnan(s.prefixYawSigmaDeg[2]));
    QCOMPARE(s.growthStop, std::string("all_failed"));
    QVERIFY(s.fallback);
    QVERIFY(std::isnan(s.yawSigmaDeg));
    QCOMPARE(s.prefixStart, s.start);
    QCOMPARE(s.prefixEnd, s.end);

    // No prefix fit was used, so the account's prefix-fit fields are their
    // defaults (initializer.h), not the 60 s fit's, and the diagnostics agree.
    QCOMPARE(s.prefixIterations, 0);
    QCOMPARE(s.prefixPasses, 0);
    QVERIFY(!s.prefixOnLimit);
    QVERIFY(s.prefixRotation.matrix() == Rot3().matrix());
    QVERIFY(s.prefixGyroBias.isZero(0));
    // The propagated start: the coarse attitude at the anchor (the first fix,
    // so bitwise) carried with zero bias; no segment fit ran.
    const Samples window = windowOf(name, Tuning{});
    QVERIFY(s.startRotation.matrix() == coarseAttitude(window, 0).matrix());
    QVERIFY(s.startGyroBias.isZero(0));
    QVERIFY(s.gyroBias.isZero(0));
    QCOMPARE(s.iterations, 0);
    QVERIFY(!s.converged);
    QCOMPARE(run.segments.size(), 1);
    const QJsonObject segment = run.segments.first().toObject();
    QCOMPARE(segment.value("growth_stop").toString(), QStringLiteral("all_failed"));
    QCOMPARE(segment.value("fallback").toBool(false), true);
    QCOMPARE(segment.value("prefix_fits").toInt(-1), 12);
    QCOMPARE(segment.value("prefix_length_s").toDouble(), 240.);
    QCOMPARE(segment.value("prefix_iterations").toInt(-1), 0);
    QCOMPARE(segment.value("prefix_passes").toInt(-1), 0);
    QCOMPARE(segment.value("prefix_on_limit").toBool(true), false);
    QVERIFY(segment.value("yaw_sigma_deg").isNull());
    // The full fit still ran, from the fallback start; its outcome is
    // logged, not asserted.
    QVERIFY(!run.trace.history.empty());
    qInfo() << "motion_start fallback after a completed 60 s fit: full fit outcome" << int(run.result.outcome)
            << qPrintable(run.result.reason) << ", rule" << run.trace.stopping.rule.c_str()
            << "after" << run.trace.history.size() << "iterations";
}

void FusionKernelTest::startsOnTheLimitAreStillUsed()
{
    // Spec section 3.3, Budgets: one iteration per pass and a negative
    // tolerance (the forcing nonConvergenceIsSolverFailure uses), so every
    // prefix fit ends after its one iteration on the limit and the segment
    // fit after its one iteration per pass; nothing throws, and both starts
    // are used: the account says on-limit, not fallback. The full fit ran
    // from it and ends SolverFailed on the iteration limit as before.
    Tuning forced;
    forced.maxIterations = 1;
    forced.relativeTolerance = -1;
    const QString name = QStringLiteral("coarse_maneuver");
    const InitializerRun limited = runInitializerFixture(name, forced);
    QVERIFY(limited.result.outcome == Fusion::Outcome::SolverFailed);
    QCOMPARE(limited.trace.stopping.rule, std::string(StopRule::kIterationLimit));
    QCOMPARE(limited.trace.initializer.segments.size(), size_t(1));
    const SegmentAccount &s = limited.trace.initializer.segments.at(0);
    QVERIFY(s.prefixOnLimit);
    QCOMPARE(s.prefixIterations, 1);
    QCOMPARE(s.prefixPasses, 1);
    QVERIFY(s.segmentOnLimit);
    QVERIFY(!s.converged);
    QVERIFY(!s.fallback);
    QVERIFY(s.iterations >= 1);
    // 6 s: the first window covers the segment, so one length is tried
    // (whether its one-iteration linearization already counts as observable
    // is not the fixture's to say).
    QVERIFY2(s.growthStop == "observable" || s.growthStop == "covers", s.growthStop.c_str());
    QCOMPARE(s.prefixLength, 60.);
    QCOMPARE(s.prefixYawSigmaDeg.size(), size_t(1));
    QCOMPARE(s.prefixFits, 4);
    // The start was used, not the fallback: the prefix fit's attitude
    // carried back to the segment's first fix with the prefix fit's bias.
    const Samples window = windowOf(name, forced);
    const Rot3 expected = propagateAttitude(window, s.prefixRotation, s.prefixStart, s.start, s.prefixGyroBias);
    QVERIFY(Rot3::Logmap(s.startRotation.between(expected)).norm() < 1e-9);
    QVERIFY(s.startGyroBias == s.prefixGyroBias);
    // The failure diagnostics keep the completed-pass shape: no initializer object.
    QCOMPARE(limited.diagnostics.keys(), kCompletedPassFailureKeys);
    QCOMPARE(limited.diagnostics.value("stopping").toObject().value("rule").toString(),
             QStringLiteral("iteration limit"));

    // The production tuning on the same fixture: nothing is on the limit,
    // and the chosen prefix fit's iteration count is the golden's (an exact
    // key of the re-captured golden).
    const InitializerRun plain = runInitializerFixture(name, Tuning{});
    QVERIFY2(plain.result.outcome == Fusion::Outcome::Succeeded, qPrintable(plain.result.reason));
    QCOMPARE(plain.segments.size(), 1);
    const QJsonObject segment = plain.segments.first().toObject();
    QCOMPARE(segment.value("prefix_on_limit").toBool(true), false);
    QCOMPARE(segment.value("segment_on_limit").toBool(true), false);
    verifyPrefixBudget(segment, plain.trace.initializer.segments.at(0));
    const QJsonObject golden = loadFusionGolden(name).diagnostics.value("initializer").toObject()
                                   .value("segments").toArray().first().toObject();
    QCOMPARE(segment.value("prefix_iterations").toInt(-1), golden.value("prefix_iterations").toInt(-2));
    QCOMPARE(segment.value("prefix_on_limit").toBool(true), golden.value("prefix_on_limit").toBool(true));
}

void FusionKernelTest::temperatureFactorJacobians()
{
    // The overview's risk note: the custom factor's six Jacobians against
    // finite differences before any fit uses it. Every value differs from the
    // linearization point so no block is trivial; dT and the slope are
    // non-zero so H6 is not.
    using gtsam::imuBias::ConstantBias;
    using gtsam::Pose3;
    Samples d = boundarySamples(Vector3(1, -2, .5));
    for (Vector3 &rate : d.gyro)
        rate = Vector3(.1, -.05, .2);
    validateSamples(d, Tuning{});
    const ConstantBias linearizedAt(Vector3(.05, -.03, .08), Vector3(.003, -.002, .004));
    const auto pim = preintegrateImu(d, .037, .863, linearizedAt, Tuning{});
    const double dT = 4.5;
    const TemperatureImuFactor factor(X(0), V(0), X(1), V(1), B(0), T(0), pim, dT);
    QCOMPARE(factor.temperatureDelta(), dT);

    const Pose3 pose_i(Rot3::RzRyRx(.3, -.2, .1), Vector3(1, 2, 3));
    const Vector3 vel_i(2, -1, .5);
    const Pose3 pose_j(Rot3::RzRyRx(.35, -.15, .12), Vector3(2.5, 1.2, 3.4));
    const Vector3 vel_j(2.6, -2.4, .9);
    const ConstantBias bias(Vector3(.04, -.02, .07), Vector3(.002, -.001, .005));
    const Vector3 slope(2e-4, -1e-4, 3e-4);

    gtsam::Matrix H1, H2, H3, H4, H5, H6;
    const gtsam::Vector error = factor.evaluateError(pose_i, vel_i, pose_j, vel_j, bias, slope,
                                                     &H1, &H2, &H3, &H4, &H5, &H6);
    QCOMPARE(error.size(), Eigen::Index(9));

    // The perturbations are the manifolds' own retracts, the tangents the
    // analytical Jacobians are taken in (GTSAM's testImuFactor checks
    // ImuFactor the same way). Entries are of order 1 to 10.
    const std::function<gtsam::Vector9(const Pose3 &, const Vector3 &, const Pose3 &, const Vector3 &,
                                       const ConstantBias &, const Vector3 &)> h =
        [&factor](const Pose3 &pi, const Vector3 &vi, const Pose3 &pj, const Vector3 &vj,
                  const ConstantBias &b, const Vector3 &s) -> gtsam::Vector9 {
            return factor.evaluateError(pi, vi, pj, vj, b, s);
        };
    const gtsam::Matrix N1 = gtsam::numericalDerivative61<gtsam::Vector9, Pose3, Vector3, Pose3, Vector3, ConstantBias, Vector3>(h, pose_i, vel_i, pose_j, vel_j, bias, slope);
    const gtsam::Matrix N2 = gtsam::numericalDerivative62<gtsam::Vector9, Pose3, Vector3, Pose3, Vector3, ConstantBias, Vector3>(h, pose_i, vel_i, pose_j, vel_j, bias, slope);
    const gtsam::Matrix N3 = gtsam::numericalDerivative63<gtsam::Vector9, Pose3, Vector3, Pose3, Vector3, ConstantBias, Vector3>(h, pose_i, vel_i, pose_j, vel_j, bias, slope);
    const gtsam::Matrix N4 = gtsam::numericalDerivative64<gtsam::Vector9, Pose3, Vector3, Pose3, Vector3, ConstantBias, Vector3>(h, pose_i, vel_i, pose_j, vel_j, bias, slope);
    const gtsam::Matrix N5 = gtsam::numericalDerivative65<gtsam::Vector9, Pose3, Vector3, Pose3, Vector3, ConstantBias, Vector3>(h, pose_i, vel_i, pose_j, vel_j, bias, slope);
    const gtsam::Matrix N6 = gtsam::numericalDerivative66<gtsam::Vector9, Pose3, Vector3, Pose3, Vector3, ConstantBias, Vector3>(h, pose_i, vel_i, pose_j, vel_j, bias, slope);
    const struct { const char *name; const gtsam::Matrix *analytic, *numeric; } blocks[] = {
        {"H1", &H1, &N1}, {"H2", &H2, &N2}, {"H3", &H3, &N3}, {"H4", &H4, &N4}, {"H5", &H5, &N5}, {"H6", &H6, &N6}};
    for (const auto &block : blocks) {
        QCOMPARE(block.analytic->rows(), block.numeric->rows());
        QCOMPARE(block.analytic->cols(), block.numeric->cols());
        const double worst = (*block.analytic-*block.numeric).cwiseAbs().maxCoeff();
        qInfo() << block.name << "max |analytic - numeric|" << worst;
        QVERIFY2(worst < 1e-6, block.name);
    }
    QCOMPARE(H6.rows(), Eigen::Index(9));
    QCOMPARE(H6.cols(), Eigen::Index(3));
    QVERIFY(H6.cwiseAbs().maxCoeff() > 1e-3);   // not vacuous: dT and the rotation Jacobian are non-zero

    // At zero slope the factor is ImuFactor on the same pim, bit for bit
    // (Eigen's == is element-wise equality): the error and H1..H5. H6, the
    // derivative with respect to the slope, does not depend on the slope: it
    // is ImuFactor's gyro-bias columns times dT, the same arithmetic.
    const gtsam::ImuFactor stock(X(0), V(0), X(1), V(1), B(0), pim);
    gtsam::Matrix G1, G2, G3, G4, G5;
    const gtsam::Vector stockError = stock.evaluateError(pose_i, vel_i, pose_j, vel_j, bias, &G1, &G2, &G3, &G4, &G5);
    gtsam::Matrix Z1, Z2, Z3, Z4, Z5, Z6;
    const gtsam::Vector zeroSlopeError = factor.evaluateError(pose_i, vel_i, pose_j, vel_j, bias, Vector3::Zero(),
                                                              &Z1, &Z2, &Z3, &Z4, &Z5, &Z6);
    QVERIFY(zeroSlopeError == stockError);
    QVERIFY(Z1 == G1 && Z2 == G2 && Z3 == G3 && Z4 == G4 && Z5 == G5);
    QVERIFY(Z6 == gtsam::Matrix(G5.rightCols<3>()*dT));
    QVERIFY(!Z6.isZero(0));
    QVERIFY(!(error == stockError));   // and with the slope and dT it is not

    // The same at dT = 0 with the non-zero slope (a second factor); there
    // the slope has no effect at all, so H6 is the zero matrix.
    const TemperatureImuFactor atReference(X(0), V(0), X(1), V(1), B(0), T(0), pim, 0.);
    gtsam::Matrix R1, R2, R3, R4, R5, R6;
    const gtsam::Vector referenceError = atReference.evaluateError(pose_i, vel_i, pose_j, vel_j, bias, slope,
                                                                   &R1, &R2, &R3, &R4, &R5, &R6);
    QVERIFY(referenceError == stockError);
    QVERIFY(R1 == G1 && R2 == G2 && R3 == G3 && R4 == G4 && R5 == G5);
    QVERIFY(R6.isZero(0));

    // Through Values: the whitened errors agree (the same covariance), the
    // clone evaluates like the original.
    gtsam::Values values;
    values.insert(X(0), pose_i);
    values.insert(V(0), vel_i);
    values.insert(X(1), pose_j);
    values.insert(V(1), vel_j);
    values.insert(B(0), bias);
    values.insert(T(0), slope);
    QVERIFY(atReference.whitenedError(values) == stock.whitenedError(values));
    gtsam::Values zeroSlope = values;
    zeroSlope.update(T(0), Vector3(Vector3::Zero()));
    QVERIFY(factor.whitenedError(zeroSlope) == stock.whitenedError(zeroSlope));
    const gtsam::NonlinearFactor::shared_ptr clone = factor.clone();
    QVERIFY(clone != nullptr);
    QVERIFY(clone.get() != &factor);
    QVERIFY(clone->error(values) == factor.error(values));
    QVERIFY(factor.error(values) > 0);
}

void FusionKernelTest::temperatureGraphShape()
{
    using gtsam::imuBias::ConstantBias;
    Samples d = boundarySamples(Vector3(1, -2, .5));
    for (size_t i = 0; i < d.imuTime.size(); ++i)
        d.temperature.push_back(40+.01*double(i));
    QCOMPARE(d.temperature.size(), size_t(101));
    validateSamples(d, Tuning{});

    // The model: the plain index-order mean.
    const GyroBiasModel model = gyroBiasModelFor(d);
    QVERIFY(model.temperatureLinear);
    double sum = 0;
    for (double t : d.temperature)
        sum += t;
    QVERIFY(std::abs(model.tRef-sum/101) < 1e-12);

    // The temperature graph: per state the position and velocity factors,
    // the temperature factor between them, the bias prior, the slope prior last.
    const auto graph = buildFactorGraph(d, BiasLinearization{ConstantBias(), Vector3::Zero()}, model, Tuning{});
    QCOMPARE(graph.size(), size_t(7));
    QVERIFY(dynamic_cast<const gtsam::GPSFactor *>(graph.at(0).get()));
    QVERIFY(dynamic_cast<const gtsam::PriorFactor<Vector3> *>(graph.at(1).get()));
    QVERIFY(dynamic_cast<const gtsam::GPSFactor *>(graph.at(2).get()));
    QVERIFY(dynamic_cast<const gtsam::PriorFactor<Vector3> *>(graph.at(3).get()));
    const auto *imu = dynamic_cast<const TemperatureImuFactor *>(graph.at(4).get());
    QVERIFY(imu);
    QVERIFY(imu->temperatureDelta() == temperatureAtFix(d, 0)-model.tRef);
    QVERIFY(imu->keys() == gtsam::KeyVector({X(0), V(0), X(1), V(1), B(0), T(0)}));
    QVERIFY(dynamic_cast<const gtsam::PriorFactor<ConstantBias> *>(graph.at(5).get()));
    const auto *slopePrior = dynamic_cast<const gtsam::PriorFactor<Vector3> *>(graph.at(6).get());
    QVERIFY(slopePrior);
    QVERIFY(slopePrior->keys() == gtsam::KeyVector({T(0)}));
    QVERIFY(slopePrior->prior().isZero(0));
    const auto sigmas = std::dynamic_pointer_cast<gtsam::noiseModel::Diagonal>(slopePrior->noiseModel());
    QVERIFY(sigmas != nullptr);
    QVERIFY(sigmas->sigmas() == gtsam::Vector(Vector3::Constant(Tuning{}.gyroBiasSlopeSigma)));
    // Spec section 6's priors, as literals: b0 keeps 0.03 rad/s, b1 is
    // 0.010 deg/s per degC.
    QCOMPARE(Tuning{}.gyroBiasSigma, .03);
    QCOMPARE(Tuning{}.gyroBiasSlopeSigma, .010*kPi/180);

    // The stock four-argument builder is unchanged: six factors, ImuFactor at 4.
    const auto stock = buildFactorGraph(d, ConstantBias(), Tuning{});
    QCOMPARE(stock.size(), size_t(6));
    QVERIFY(dynamic_cast<const gtsam::ImuFactor *>(stock.at(4).get()));
    QVERIFY(dynamic_cast<const gtsam::PriorFactor<ConstantBias> *>(stock.at(5).get()));

    // The interval bias: the argument itself under the constant model; the
    // gyro part shifted by slope (T_k - tRef) under the temperature model.
    const ConstantBias bias(Vector3(.05, -.03, .08), Vector3(.003, -.002, .004));
    const Vector3 slope(1e-3, 0, 0);
    QVERIFY(intervalBias(d, 0, bias, slope, GyroBiasModel{}).vector() == bias.vector());
    const ConstantBias shifted = intervalBias(d, 0, bias, slope, model);
    QVERIFY(shifted.accelerometer() == bias.accelerometer());
    QCOMPARE(shifted.gyroscope().x(), bias.gyroscope().x()+1e-3*(temperatureAtFix(d, 0)-model.tRef));
    QCOMPARE(shifted.gyroscope().y(), bias.gyroscope().y());
    QCOMPARE(shifted.gyroscope().z(), bias.gyroscope().z());

    // The temperature at a fix is the scalar interpolation, which agrees with
    // the vector overload on (v, 0, 0) bit for bit; on a sample it is the sample.
    QVERIFY(temperatureAtFix(d, 0) == interpolateAt(d.imuTime, d.temperature, .037));
    Vectors asVectors;
    for (double t : d.temperature)
        asVectors.emplace_back(t, 0, 0);
    QVERIFY(temperatureAtFix(d, 0) == interpolateAt(d.imuTime, asVectors, .037).x());
    QVERIFY(temperatureAtFix(d, 1) == interpolateAt(d.imuTime, asVectors, .863).x());
    QVERIFY(interpolateAt(d.imuTime, d.temperature, .5) == d.temperature[50]);
    QVERIFY(interpolateAt(d.imuTime, d.temperature, 0) == d.temperature[0]);
    QVERIFY(interpolateAt(d.imuTime, d.temperature, .037) != d.temperature[3]);

    // A window's temperature is exactly that of its IMU samples (a window
    // needs three fixes, so the nine-fix linear recording); without a series
    // there is no temperature model.
    Samples linear = linearSamples(Vector3(12, -4, 2), Vector3(7, 8, 9));
    for (size_t i = 0; i < linear.imuTime.size(); ++i)
        linear.temperature.push_back(40+.01*double(i));
    const Samples window = fittedWindow(linear, .437, 1.237);   // fixes 2..6, IMU samples 43..124
    QCOMPARE(window.gnssTime.size(), size_t(5));
    QCOMPARE(window.temperature.size(), window.imuTime.size());
    QVERIFY(window.temperature.front() == linear.temperature[43]);
    QVERIFY(window.temperature.back() == linear.temperature[124]);
    QVERIFY(window.imuTime.front() == linear.imuTime[43]);
    Samples without = linear;
    without.temperature.clear();
    QVERIFY(fittedWindow(without, .437, 1.237).temperature.empty());
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, gyroBiasModelFor(without));

    // A constant series has exactly that mean.
    Samples constant = d;
    constant.temperature.assign(101, 25.);
    QCOMPARE(gyroBiasModelFor(constant).tRef, 25.);
}

void FusionKernelTest::reconstructionUsesIntervalBias()
{
    // The proof of the reconstruction's per-interval bias: X(1) is X(0)
    // propagated with the interval's temperature-dependent bias, so the
    // reconstruction lands on it exactly under the temperature model, and
    // misses it by |slope dT| x .826 s = .0165 rad = .95 degrees under the
    // constant model (the .5 degree bound is the margin).
    using gtsam::imuBias::ConstantBias;
    Samples d = boundarySamples(Vector3::Zero());
    const Vector3 g0(.01, -.02, .03);   // the bias the gyro reads
    for (Vector3 &rate : d.gyro)
        rate = g0;
    d.temperature.assign(d.imuTime.size(), 45.0);
    validateSamples(d, Tuning{});
    GyroBiasModel model;
    model.temperatureLinear = true;
    model.tRef = 35.0;                   // dT = 10 at fix 0
    const Vector3 slope(0, 0, 2e-3);
    const ConstantBias b0(Vector3::Zero(), g0);
    const Vector3 intervalGyroBias = intervalBias(d, 0, b0, slope, model).gyroscope();
    QVERIFY((intervalGyroBias-Vector3(.01, -.02, .05)).norm() < 1e-15);

    FitResult fit;
    fit.values.insert(B(0), b0);
    fit.values.insert(T(0), slope);
    fit.values.insert(X(0), gtsam::Pose3());
    fit.values.insert(X(1), gtsam::Pose3(propagateAttitude(d, Rot3(), .037, .863, intervalGyroBias), Vector3::Zero()));
    fit.values.insert(V(0), Vector3(0, 0, 0));
    fit.values.insert(V(1), Vector3(0, 0, 0));
    fit.gyroBiasSlope = slope;
    fit.biasModel = model;

    const DenseTrajectory dense = reconstructTrajectory(d, fit);
    QCOMPARE(dense.endpointCorrection.size(), size_t(1));
    qInfo() << "endpoint correction: temperature model" << dense.endpointCorrection[0] << "deg";
    QVERIFY(dense.endpointCorrection[0] < 1e-9);

    fit.biasModel = GyroBiasModel{};
    fit.gyroBiasSlope = Vector3::Zero();
    const DenseTrajectory constant = reconstructTrajectory(d, fit);
    qInfo() << "endpoint correction: constant model" << constant.endpointCorrection[0] << "deg";
    QVERIFY(constant.endpointCorrection[0] > .5);
}

void FusionKernelTest::constantTemperatureKeepsSlopeAtPrior()
{
    // The spec's "constant temperature" case: with T_k - T_ref exactly zero
    // at every fix the factor's H6 is zero, the slope's normal equation is
    // its prior's alone with a zero right-hand side, and every LM step
    // leaves it at 0.0. The bound is 1 % of the prior sigma, the margin
    // against a solver that visits a rounding-size value and steps back.
    Tuning t;
    t.segmentLength = 60;
    t.minFinalSegment = 12;
    FusionFixture f = initializerFixture(QStringLiteral("drifting_bias"));
    QCOMPARE(f.imuTemperature.size(), 2001);
    f.imuTemperature = QVector<double>(2001, 35.0);
    PipelineTrace trace;
    const Fusion::Result result = runPipeline(toChannels(f), t, Checkpoint(), &trace);
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
    const QJsonObject diagnostics = diagnosticsOf(result);
    QCOMPARE(diagnostics.value("algorithm").toString(), QStringLiteral("batch-temperature-bias-v3"));
    const QJsonObject gyroBias = diagnostics.value("model").toObject().value("gyro_bias").toObject();
    const QJsonArray b1 = gyroBias.value("b1_rad_s_per_degc").toArray();
    QCOMPARE(b1.size(), 3);
    for (const QJsonValue &component : b1)
        QVERIFY(std::abs(component.toDouble(1)) < .01*Tuning{}.gyroBiasSlopeSigma);
    // 2001 copies of 35: the sum 70035 and the quotient are exact.
    QCOMPARE(gyroBias.value("t_ref_degc").toDouble(), 35.);
    const QJsonArray residuals = diagnostics.value("residuals").toArray();
    const QJsonObject last = residuals.last().toObject();
    QCOMPARE(last.value("kind").toString(), QStringLiteral("slope_prior"));
    QVERIFY(last.value("squared_whitened_error").toDouble(1) < 1e-10);
    const QJsonArray b0 = gyroBias.value("b0_rad_s").toArray();
    const QJsonArray seedBias = diagnostics.value("seeds").toArray().first().toObject().value("gyro_bias_rad_s").toArray();
    QCOMPARE(b0.size(), 3);
    for (int i = 0; i < 3; ++i)
        QVERIFY(b0.at(i).toDouble() == seedBias.at(i).toDouble());

    // The consistency check with the stock path through the internal seams:
    // the same window, the same initializer, the constant-bias fit (the
    // default model) is the same model at b1 = 0, so both converge to the
    // same objective under the settle tolerance (1e-6 relative is the margin
    // for a different elimination ordering).
    const PreparedInput prepared = prepareInput(toChannels(f));
    Tuning derived = t;
    derived.maxGap = kImuGapMedians*medianInterval(prepared.recording.imuTime);
    const Samples window = fittedWindow(prepared.recording, prepared.usableStart, prepared.recording.gnssTime.back());
    validateSamples(window, derived);
    const Initialization init = initialize(window, derived);
    const FitResult stock = fitFactorGraph(window, init.state, derived);
    QVERIFY(stock.converged);
    QVERIFY(!stock.biasModel.temperatureLinear);
    QVERIFY(stock.gyroBiasSlope.isZero(0));
    QCOMPARE(stock.residuals.back().kind, std::string("bias_prior"));
    const double objective = diagnostics.value("objective").toDouble();
    qInfo() << "constant temperature: objective" << objective << ", stock fit" << stock.objective
            << ", full fit" << trace.history.size() << "iterations";
    QVERIFY(std::abs(stock.objective-objective) <= 1e-6*std::max(1., objective));
}

FLYSIGHT_TEST_MAIN(FusionKernelTest)
#include "tst_fusion_kernel.moc"
