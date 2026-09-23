// Fusion kernel internals.
//
// What the golden fixtures of tst_fusion_golden cannot reach (every
// stationary gate, exact integration boundaries, heading freedom, the two
// stopping rules forced through the tuning, the solver-failure path and its
// diagnostics shapes), with the literal expectations of the reference's own
// self-test (sensor-fusion-clean-port, tests/fusion_regression.cpp), and the
// fit trace that localizes a golden failure to a stage: initializer first,
// then each optimizer iteration.
//
// The only test source that includes internal src/fusion/ headers, and one of
// the few targets that names gtsam itself.

#include <cmath>
#include <limits>
#include <set>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <gtsam/config.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/navigation/NavState.h>

#include "calculations/anglehelper.h"
#include "fusion/factorgraphfit.h"
#include "fusion/fusionoutput.h"
#include "fusion/fusionpipeline.h"
#include "fusion/fusionsamples.h"
#include "fusion/imuintegration.h"
#include "fusion/initializer.h"
#include "fusion/stationarywindow.h"
#include "fusion/trajectoryreconstruction.h"
#include "fusionfixtures.h"
#include "fusiongolden.h"
#include "fusiontrace.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySight::Fusion::Detail;
using namespace FlySightTest;
using gtsam::Rot3;
using gtsam::Vector3;
using gtsam::symbol_shorthand::B;
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

/// 40 s at rest with a tilted sensor and a gyro bias; GNSS at 5 Hz.
Samples quietSamples(const Vector3 &gyroBias)
{
    Samples quiet;
    for (int i = 0; i <= 4000; ++i) {
        quiet.imuTime.push_back(i*.01);
        quiet.force.emplace_back(.1, -7., 6.85);
        quiet.gyro.push_back(gyroBias);
    }
    for (int i = 0; i < 200; ++i) {
        quiet.gnssTime.push_back(.037+i*.2);
        quiet.position.push_back(Vector3::Zero());
        quiet.velocity.push_back(Vector3::Zero());
        quiet.positionSigma.push_back(Vector3::Ones());
        quiet.velocitySigma.push_back(Vector3::Constant(.1));
    }
    return quiet;
}

/// `quiet` moving at a constant (40, -15, 8) m/s.
Samples translatingSamples(const Samples &quiet)
{
    Samples translating = quiet;
    for (size_t i = 0; i < translating.velocity.size(); ++i) {
        translating.velocity[i] = Vector3(40, -15, 8);
        translating.position[i] = translating.gnssTime[i]*translating.velocity[i];
    }
    return translating;
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

bool hasGate(const StationaryWindow &window, const char *gate)
{
    return std::find(window.rejected.begin(), window.rejected.end(), gate) != window.rejected.end();
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

} // namespace

class FusionKernelTest : public QObject {
    Q_OBJECT

private slots:
    void solverUsesTbb();
    void unwrapRule();
    void preintegrationHonoursExactBoundaries();
    void validationRejectsEachDefect();
    void backwardPropagationUndoesForward();
    void headingIsUnconstrained();
    void reconstructionTimingAndEndpointCorrection();
    void stationaryGates();
    void stationaryScanPollsSilently();
    void shortInputUsesCoarseInitializer();
    void exactConstantVelocityFit();
    void fitTraceMatchesGolden_data();
    void fitTraceMatchesGolden();
    void biasSettledByCostTest();
    void slowTailAtTheIterationLimit_data();
    void slowTailAtTheIterationLimit();
    void nonConvergenceIsSolverFailure();
    void biasNeverSettlesIsSolverFailure();
    void failureDiagnosticsShape();
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

void FusionKernelTest::stationaryGates()
{
    const Vector3 bg(.004, -.003, .006);
    const Samples quiet = quietSamples(bg);
    QVERIFY(assessStationaryWindow(quiet, 5, 35).accepted);

    // Constant translation is as good as rest, and initializes identically
    const Samples translating = translatingSamples(quiet);
    QVERIFY(assessStationaryWindow(translating, 5, 35).accepted);
    const InitialAttitude quietInit = initialAttitude(quiet, quiet.gnssTime.front());
    const InitialAttitude movingInit = initialAttitude(translating, translating.gnssTime.front());
    QVERIFY(quietInit.method.find("stationary") != std::string::npos);
    QVERIFY(Rot3::Logmap(quietInit.rotation.between(movingInit.rotation)).norm() < 1e-12);
    QVERIFY((quietInit.gyroBias-movingInit.gyroBias).norm() < 1e-12);

    // Sustained drift: caught by the drift gate, not by variability
    Samples changing = translating;
    changing.velocitySigma = Vectors(changing.gnssTime.size(), Vector3::Constant(.2));
    for (size_t i = 0; i < changing.velocity.size(); ++i)
        changing.velocity[i].x() += .021*changing.gnssTime[i];
    const StationaryWindow drift = assessStationaryWindow(changing, 5, 35);
    QVERIFY(!drift.accepted);
    QVERIFY(hasGate(drift, "velocity_drift"));
    QVERIFY(!hasGate(drift, "velocity_variability"));

    // Constant-speed turn
    changing = translating;
    for (size_t i = 0; i < changing.velocity.size(); ++i) {
        const double angle = .005*changing.gnssTime[i];
        changing.velocity[i] = Vector3(40*std::cos(angle), 40*std::sin(angle), 0);
    }
    QVERIFY(!assessStationaryWindow(changing, 5, 35).accepted);

    // Uncertain constant velocity
    changing = translating;
    changing.velocitySigma = Vectors(changing.gnssTime.size(), Vector3::Constant(2));
    QVERIFY(!assessStationaryWindow(changing, 5, 35).accepted);

    // Variation that is small in m/s but large in units of its sigma
    changing = translating;
    changing.velocitySigma = Vectors(changing.gnssTime.size(), Vector3::Constant(.01));
    for (size_t i = 0; i < changing.velocity.size(); ++i)
        changing.velocity[i].x() += .1*std::sin(changing.gnssTime[i]);
    QVERIFY(!assessStationaryWindow(changing, 5, 35).accepted);

    // Constant rotation
    Samples moving = quiet;
    moving.gyro = Vectors(moving.gyro.size(), Vector3(0, 0, .1));
    QVERIFY(!assessStationaryWindow(moving, 5, 35).accepted);

    // Low-speed handling
    moving = quiet;
    for (size_t i = 0; i < moving.gyro.size(); ++i)
        moving.gyro[i].x() += .04*std::sin(moving.imuTime[i]);
    QVERIFY(!assessStationaryWindow(moving, 5, 35).accepted);

    // Uncertain GNSS at rest
    moving = quiet;
    moving.velocitySigma = Vectors(moving.gnssTime.size(), Vector3::Constant(2));
    QVERIFY(!assessStationaryWindow(moving, 5, 35).accepted);

    // Too little data ends the assessment early
    const StationaryWindow outside = assessStationaryWindow(quiet, 100, 130);
    QVERIFY(!outside.accepted);
    QCOMPARE(outside.rejected, std::vector<std::string>{"coverage"});
}

void FusionKernelTest::stationaryScanPollsSilently()
{
    const Samples quiet = quietSamples(Vector3(.004, -.003, .006));
    QStringList texts;
    int asked = 0;
    const auto report = [&texts](const QString &text) { texts.append(text); };

    // 40 s: two candidate windows, [0, 30) and [5, 35); one question before
    // each, and nothing reported.
    const InitialAttitude polled = initialAttitude(quiet, quiet.gnssTime.front(),
        Checkpoint(report, [&asked] { ++asked; return false; }));
    QCOMPARE(asked, 2);
    QVERIFY(texts.isEmpty());

    // Asking changes nothing
    const InitialAttitude plain = initialAttitude(quiet, quiet.gnssTime.front());
    QCOMPARE(polled.method, plain.method);
    QCOMPARE(polled.intervalStart, plain.intervalStart);
    QCOMPARE(polled.anchorTime, plain.anchorTime);
    QVERIFY(polled.gyroBias == plain.gyroBias);
    QVERIFY(polled.rotation.matrix() == plain.rotation.matrix());

    // "Yes" at the second window abandons the scan there
    asked = 0;
    bool cancelled = false;
    try {
        initialAttitude(quiet, quiet.gnssTime.front(),
                        Checkpoint(report, [&asked] { return ++asked >= 2; }));
    } catch (const FusionCancelled &) {
        cancelled = true;
    }
    QVERIFY(cancelled);
    QCOMPARE(asked, 2);
    QVERIFY(texts.isEmpty());

    // A recording shorter than a window has no candidate: nobody is asked
    asked = 0;
    const Samples linear = linearSamples(Vector3(12, -4, 2), Vector3(7, 8, 9));
    initialAttitude(linear, linear.gnssTime.front(),
                    Checkpoint(report, [&asked] { ++asked; return false; }));
    QCOMPARE(asked, 0);

    // The gap limit belongs to the recording: handing it in is the same
    // assessment as deriving it per window.
    const StationaryWindow derived = assessStationaryWindow(quiet, 5, 35);
    const StationaryWindow given = assessStationaryWindow(quiet, 5, 35, imuGapLimit(quiet));
    QCOMPARE(given.accepted, derived.accepted);
    QCOMPARE(given.rejected, derived.rejected);
    QCOMPARE(given.imuCount, derived.imuCount);
    QCOMPARE(given.gnssCount, derived.gnssCount);
    QCOMPARE(given.score, derived.score);
    QVERIFY(given.forceMean == derived.forceMean);
    QVERIFY(given.gyroMean == derived.gyroMean);

    // The three-argument form derives the limit from the IMU time axis, so too
    // few or invalid IMU timestamps are the validation error, not a verdict
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, assessStationaryWindow(Samples{}, 5, 35));
    Samples unordered = quiet;
    unordered.imuTime[1] = unordered.imuTime[0];
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, assessStationaryWindow(unordered, 5, 35));
}

void FusionKernelTest::shortInputUsesCoarseInitializer()
{
    const Samples linear = linearSamples(Vector3(12, -4, 2), Vector3(7, 8, 9));
    const InitialAttitude init = initialAttitude(linear, linear.gnssTime.front());
    QVERIFY(init.method.find("coarse") != std::string::npos);
    QCOMPARE(init.startTime, linear.gnssTime.front());
    QCOMPARE(init.anchorTime, 0.);
    QVERIFY(init.gyroBias.isZero(0));
}

void FusionKernelTest::exactConstantVelocityFit()
{
    // A noiseless constant translation checks the complete optimizer and the
    // dense outputs against a physical trajectory with nonzero velocity.
    const Vector3 speed(12, -4, 2), offset(7, 8, 9);
    const Samples linear = linearSamples(speed, offset);
    const InitialAttitude init = initialAttitude(linear, linear.gnssTime.front());

    const FitResult fitted = fitFactorGraph(linear, 0, Tuning{}, init);
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

    // Initializer first, then every optimizer iteration in order: the first
    // difference reported is the first stage that diverged. traceJson() is
    // the function the capture tool wrote the golden with (fusiontrace.h).
    const QJsonObject got = traceJson(trace);
    QCOMPARE(got.value("history").toArray().size(), golden.trace.value("history").toArray().size());
    const QString difference = compareJson(QStringLiteral("trace"), got, golden.trace);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

void FusionKernelTest::biasSettledByCostTest()
{
    // The spec's bias-settled test, on the production tuning. Under the
    // bias-shift rule this fixture needed a third pass of one iteration that
    // lowered the cost by 8e-15 to prove the bias had stopped moving (the
    // committed history before this phase has passes of 4, 2, 1 iterations);
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
    QCOMPARE(diagnostics.value("algorithm").toString(), QStringLiteral("batch-shared-bias-v2"));

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
    QCOMPARE(diagnostics.value("algorithm").toString(), QStringLiteral("batch-shared-bias-v2"));
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
    // the account, with the text the initializer of a later phase relies on.
    bool caught = false;
    try {
        throw FitFailure("Nonfinite or increasing optimizer cost", s);
    } catch (const std::runtime_error &e) {
        caught = true;
        QCOMPARE(QString::fromUtf8(e.what()), QStringLiteral("Nonfinite or increasing optimizer cost"));
    }
    QVERIFY(caught);
}

FLYSIGHT_TEST_MAIN(FusionKernelTest)
#include "tst_fusion_kernel.moc"
