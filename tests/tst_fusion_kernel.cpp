// Fusion kernel internals.
//
// What the golden fixtures of tst_fusion_parity cannot reach (every
// stationary gate, exact integration boundaries, heading freedom, the
// solver-failure path), with the literal expectations of the reference's own
// self-test (sensor-fusion-clean-port, tests/fusion_regression.cpp), and the
// fit trace that localizes a parity failure to a stage: initializer first,
// then each optimizer iteration.
//
// The only test source that includes internal src/fusion/ headers, and one of
// the few targets that names gtsam itself.

#include <cmath>
#include <limits>

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
#include "fusion/fusionpipeline.h"
#include "fusion/fusionsamples.h"
#include "fusion/imuintegration.h"
#include "fusion/initializer.h"
#include "fusion/stationarywindow.h"
#include "fusion/trajectoryreconstruction.h"
#include "fusionfixtures.h"
#include "fusiongolden.h"
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

QJsonArray toJsonArray(const Vector3 &v)
{
    return QJsonArray{v.x(), v.y(), v.z()};
}

/// The trace in the shape the capture harness wrote it.
QJsonObject traceJson(const PipelineTrace &trace)
{
    const auto q = trace.attitude.rotation.toQuaternion();
    QJsonArray history;
    for (const FitIteration &h : trace.history)
        history.append(QJsonArray{h.outer, h.iteration, h.before, h.after});
    return {{"method", QString::fromStdString(trace.attitude.method)},
            {"interval_s", QJsonArray{trace.attitude.intervalStart, trace.attitude.intervalEnd}},
            {"anchor_time_s", trace.attitude.anchorTime},
            {"gyro_bias_rad_s", toJsonArray(trace.attitude.gyroBias)},
            {"start_quaternion_xyzw", QJsonArray{q.x(), q.y(), q.z(), q.w()}},
            {"converged", trace.converged},
            {"history", history}};
}

Fusion::Result rejectedBy(const Fusion::Channels &channels)
{
    return runPipeline(channels, Tuning{}, Checkpoint());
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
    void nonConvergenceIsSolverFailure();
};

void FusionKernelTest::solverUsesTbb()
{
    // twoRunsAreBitIdentical (tst_fusion_parity) means something only if the
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
    // difference reported is the first stage that diverged.
    const QJsonObject got = traceJson(trace);
    QCOMPARE(got.value("history").toArray().size(), golden.trace.value("history").toArray().size());
    const QString difference = compareJson(QStringLiteral("trace"), got, golden.trace);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

void FusionKernelTest::nonConvergenceIsSolverFailure()
{
    // One iteration per bias pass, and a pass counts as settled only when its
    // cost stops decreasing altogether: five passes are not enough for that.
    //
    // THE ONE PLACE TO WATCH ON OTHER PLATFORMS. On the capture machine the
    // fifth pass still lowers the cost, but only by about 1.9e-12 on a cost of
    // about 2: roughly a thousand times rounding noise, not more. A platform
    // whose arithmetic differs in the last bits (another libm, fma contraction
    // inside GTSAM) could see that pass change nothing, count as settled, and
    // report convergence. If this test fails elsewhere with Succeeded, that is
    // the reason; the remedy is a harder non-convergence case for that
    // platform (fewer passes' worth of progress), never a change to the kernel.
    Tuning tuning;
    tuning.maxIterations = 1;
    tuning.relativeTolerance = 1e-300;
    PipelineTrace trace;
    const Fusion::Result result = runPipeline(
        toChannels(fusionFixture(QStringLiteral("coarse_maneuver"))), tuning, Checkpoint(), &trace);

    QVERIFY(result.outcome == Fusion::Outcome::SolverFailed);
    QCOMPARE(result.reason,
             QStringLiteral("Batch fusion did not converge; sensor fusion unavailable"));
    QVERIFY(!trace.converged);
    QCOMPARE(trace.history.size(), size_t(5));

    const QJsonObject diagnostics = QJsonDocument::fromJson(result.diagnosticsJson.toUtf8()).object();
    QCOMPARE(diagnostics.keys(), QStringList({QStringLiteral("algorithm"), QStringLiteral("failure")}));
    QCOMPARE(diagnostics.value("failure").toString(), result.reason);
    QVERIFY(result.time.isEmpty() && result.north.isEmpty() && result.accN.isEmpty()
            && result.roll.isEmpty() && result.yaw.isEmpty() && result.qw.isEmpty());
}

FLYSIGHT_TEST_MAIN(FusionKernelTest)
#include "tst_fusion_kernel.moc"
