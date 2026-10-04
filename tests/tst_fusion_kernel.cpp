// Fusion kernel internals.
//
// What the golden fixtures of tst_fusion_golden cannot reach (the segmented
// initializer: segment cutting on fixes, the smallest-sAcc anchor, prefix
// growth on the marginal yaw sigma, the fallback when every start fails, its
// progress texts and the synthetic recordings of the specification; exact
// integration boundaries, heading freedom, the stopping rules forced through
// the tuning, the datasheet's noise by configuration, the lattice and rate
// checks, the step model (the sampling term and the mid-step remainder against
// their derivations), the temperature-dependent gyro
// bias (the section 6 cases), the scale state
// (the divided readings, the scale Jacobian against central differences, the
// scaled factor's Jacobians, the graph, the re-preintegration, the
// reconstruction, at rest and on the scale recording, its diagnostics), the
// scale factors' release (the held and released stages and their account, the
// fallback to the held fit, divergence in either stage), the
// solver-failure path and its diagnostics shapes, the IMU-rate reconstruction
// pass against a dense reference graph and its per-interval seam, the
// channels the fit publishes as that pass and their time axis, the accuracy
// (the covariance step against the joint marginals and the heading check,
// its composition at the samples against a graph with a state at every edge,
// the accuracy formulas on known answers, the widening, the undetermined
// heading, a failed covariance step, the scale sigmas), the GNSS holes (a
// hole bridged, the longest one bridged and a longer one rejected, the
// cutter's sparse pieces, the holes in the input audit)), with the literal expectations of the reference's own
// self-test (sensor-fusion-clean-port, tests/fusion_regression.cpp), and the
// fit trace that localizes a golden failure to a stage: the segment account
// first, then each optimizer iteration.
//
// The only test source that includes internal src/fusion/ headers, and one of
// the few targets that names gtsam itself.

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

#include <QElapsedTimer>
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
#include <gtsam/linear/GaussianFactorGraph.h>
#include <gtsam/linear/JacobianFactor.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/Marginals.h>
#include <gtsam/nonlinear/NonlinearEquality.h>
#include <gtsam/slam/PriorFactor.h>

#include "calculations/anglehelper.h"
#include "fusion/factorgraphfit.h"
#include "fusion/fitcovariance.h"
#include "fusion/fusionoutput.h"
#include "fusion/fusionpipeline.h"
#include "fusion/fusionsamples.h"
#include "fusion/imuintegration.h"
#include "fusion/initializer.h"
#include "fusion/inputadapter.h"
#include "fusion/scaledimufactor.h"
#include "fusion/sensornoise.h"
#include "fusion/trajectoryreconstruction.h"
#include "fusionfixtures.h"
#include "fusiongolden.h"
#include "samplecontinuity.h"
#include "fusiontrace.h"
#include "sensorconfiguration.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySight::Fusion::Detail;
using namespace FlySightTest;
using FlySight::Fusion::ImuConfiguration;
using gtsam::Rot3;
using gtsam::Vector3;
using gtsam::Vector6;
using gtsam::symbol_shorthand::B;
using gtsam::symbol_shorthand::S;
using gtsam::symbol_shorthand::T;
using gtsam::symbol_shorthand::V;
using gtsam::symbol_shorthand::X;

namespace {

const Vector3 kTestGravity(0, 0, 9.80665);

/// The noise of a fixture's configuration, +/-16 g and +/-2000 deg/s at
/// `rate` for both sensors. A hand-built recording takes the listed rate
/// nearest its sampling (only the densities are read; an 8 Hz recording takes
/// 12.5 Hz).
ImuNoise fixtureNoise(double rate)
{
    ImuConfiguration configuration;
    configuration.accelFsG = 16;
    configuration.gyroFsDegS = 2000;
    configuration.accelOdrHz = rate;
    configuration.gyroOdrHz = rate;
    return imuNoise(configuration);
}

/// The production tuning with the noise of fixtureNoise(rate), as planFit()
/// would derive it: what the internal seams take for a hand-built recording.
Tuning tuningAt(double rate)
{
    Tuning tuning;
    tuning.noise = fixtureNoise(rate);
    return tuning;
}

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

/// IMU samples at `times` whose force and rate are `force(t)` and `rate(t)`.
/// No GNSS: preintegrateImu() reads none.
Samples signalSamples(const std::vector<double> &times, const std::function<Vector3(double)> &force,
                      const std::function<Vector3(double)> &rate)
{
    Samples d;
    for (const double t : times) {
        d.imuTime.push_back(t);
        d.force.push_back(force(t));
        d.gyro.push_back(rate(t));
    }
    return d;
}

/// 1 s of 100 Hz IMU turning at about 2 rad/s and accelerating, every axis
/// of both sensors changing smoothly, with two GNSS fixes between samples:
/// the window of the scale state's integration and factor tests.
Samples turningSamples()
{
    Samples d;
    for (int i = 0; i <= 100; ++i) {
        const double t = i*.01;
        d.imuTime.push_back(t);
        d.force.emplace_back(2+std::sin(3*t), -1+std::cos(2*t), -9.8+.5*t);
        d.gyro.emplace_back(.5*std::sin(2*t), 2+.3*t, -.4*std::cos(t));
    }
    d.gnssTime = {.037, .863};
    d.position = Vectors(2, Vector3::Zero());
    d.velocity = d.position;
    d.positionSigma = Vectors(2, Vector3::Ones());
    d.velocitySigma = d.positionSigma;
    return d;
}

/// A scale away from one on every axis: the linearization of the scale
/// state's integration and factor tests.
Vector6 offNominalScale()
{
    Vector6 scale;
    scale << 1.01, .99, 1.02, .98, 1.015, 1.005;
    return scale;
}

/// max |got - expected| over max |expected|, entry by entry.
double relativeDifference(const gtsam::Matrix &got, const gtsam::Matrix &expected)
{
    return (got-expected).cwiseAbs().maxCoeff()/expected.cwiseAbs().maxCoeff();
}

/// Each step of a preintegration and the two sensor covariances it was
/// integrated with, read through the observer and pim.p(): at the observer's
/// call for a step the shared params hold the step before's, and after the
/// return the last step's (imuintegration.h).
struct StepCovariances {
    std::vector<ImuStep> steps;
    std::vector<gtsam::Matrix3> accelerometer, gyroscope;
    gtsam::PreintegratedImuMeasurements pim;
};

StepCovariances stepCovariances(const Samples &d, double start, double end,
                                const gtsam::imuBias::ConstantBias &bias, const ImuNoise &noise)
{
    std::vector<ImuStep> steps;
    std::vector<gtsam::Matrix3> accelerometer, gyroscope;
    const auto observer = [&](const gtsam::PreintegratedImuMeasurements &pim, const ImuStep &step) {
        if (!steps.empty()) {
            accelerometer.push_back(pim.p().accelerometerCovariance);
            gyroscope.push_back(pim.p().gyroscopeCovariance);
        }
        steps.push_back(step);
    };
    const gtsam::PreintegratedImuMeasurements pim = preintegrateImu(d, start, end, bias, Vector6::Ones(), noise,
                                                                    observer);
    accelerometer.push_back(pim.p().accelerometerCovariance);
    gyroscope.push_back(pim.p().gyroscopeCovariance);
    return {steps, accelerometer, gyroscope, pim};
}

/// |got - expected| <= relative |expected|, element by element of the diagonal
/// and exactly zero off it (the step model is isotropic).
bool isotropicWithin(const gtsam::Matrix3 &got, double expected, double relative)
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (r != c ? got(r, c) != 0 : !(std::abs(got(r, c)-expected) <= relative*std::abs(expected)))
                return false;
        }
    }
    return true;
}

/// w = 1/2 integral_a^b (t - t0)(t1 - t) dt by Simpson's rule, which is exact
/// for the quadratic integrand: a second derivation of the sampling weight,
/// not the kernel's.
double simpsonWeight(double t0, double t1, double a, double b)
{
    const auto g = [&](double t) { return (t-t0)*(t1-t); };
    return .5*(b-a)/6*(g(a)+4*g((a+b)/2)+g(b));
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

/// `result` is a rejection with `reason`, in the result and the diagnostics.
bool rejectedWith(const Fusion::Result &result, const QString &reason)
{
    return result.outcome == Fusion::Outcome::Rejected && result.reason == reason
        && diagnosticsOf(result).value("failure").toString() == reason;
}

/// The failure diagnostics of a fit that completed its passes: QJsonObject
/// sorts its keys.
const QStringList kCompletedPassFailureKeys{QStringLiteral("algorithm"), QStringLiteral("failure"),
                                            QStringLiteral("quality"), QStringLiteral("stopping")};

bool allChannelsEmpty(const Fusion::Result &result)
{
    return result.time.isEmpty() && result.north.isEmpty() && result.accN.isEmpty()
        && result.roll.isEmpty() && result.yaw.isEmpty() && result.qw.isEmpty()
        && result.headingAcc.isEmpty() && result.tiltAcc.isEmpty() && result.accHAcc.isEmpty()
        && result.accDAcc.isEmpty();
}

/// A golden fixture or one of the initializer's recordings, by name.
FusionFixture fixtureNamed(const QString &name)
{
    const FusionFixture golden = fusionFixture(name);
    return golden.name.isEmpty() ? initializerFixture(name) : golden;
}

/// `tuning` with the IMU gap limit and the noise the pipeline derives for a
/// fixture.
Tuning pipelineTuning(const QString &name, Tuning tuning)
{
    const Fusion::Channels channels = toChannels(fixtureNamed(name));
    tuning.maxGap = SampleContinuity::holeThreshold(prepareInput(channels).recording.imuTime);
    tuning.noise = imuNoise(channels.imuConfiguration);
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

/// `run` with its result's diagnostics parsed.
void parseDiagnostics(InitializerRun &run)
{
    run.diagnostics = diagnosticsOf(run.result);
    run.segments = run.diagnostics.value("initializer").toObject().value("segments").toArray();
}

InitializerRun runInitializerFixture(const QString &name, const Tuning &tuning,
                                     const Checkpoint &checkpoint = Checkpoint())
{
    InitializerRun run;
    run.result = runPipeline(toChannels(fixtureNamed(name)), tuning, checkpoint, &run.trace);
    parseDiagnostics(run);
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

/// A fitted window with the tuning its fit ran with: what reconstructAtImuRate() takes.
struct WindowFit {
    Samples window;
    Tuning tuning;
    FitResult fit;
    InitializerAccount account;     ///< the initializer's, which assembleSuccess() reports
};

/// The full fit of `channels` through the internal seams, in the order of
/// planFit() and fitAndAssemble(): the prepared recording, the derived IMU gap
/// limit and noise, the fitted window validated, the temperature model (with
/// its scale factors), the initializer and the fit. The caller checks
/// convergence.
WindowFit fitOfChannels(const Fusion::Channels &channels)
{
    WindowFit f;
    const PreparedInput prepared = prepareInput(channels);
    const Samples &full = prepared.recording;
    f.tuning.maxGap = SampleContinuity::holeThreshold(full.imuTime);
    f.tuning.noise = imuNoise(channels.imuConfiguration);
    f.window = fittedWindow(full, prepared.usableStart, full.gnssTime.back());
    validateSamples(f.window, f.tuning);
    const GyroBiasModel model = gyroBiasModelFor(f.window);
    const Initialization init = initialize(f.window, f.tuning);
    f.account = init.account;
    f.fit = fitFactorGraph(f.window, init.state, f.tuning, QString::fromLatin1(kFullFitPassFormat), Checkpoint(), model);
    return f;
}

/// A fixture's full fit (fitOfChannels()). The fits dominate this
/// executable's time, so each is made once per run.
const WindowFit &fixtureFit(const QString &name)
{
    static std::map<QString, WindowFit> fits;
    const auto found = fits.find(name);
    if (found != fits.end())
        return found->second;
    return fits.emplace(name, fitOfChannels(toChannels(fixtureNamed(name)))).first->second;
}

/// The covariance step on fixtureFit(name), once per run.
const FitCovariance &fixtureCovariance(const QString &name)
{
    static std::map<QString, FitCovariance> covariances;
    const auto found = covariances.find(name);
    if (found != covariances.end())
        return found->second;
    const WindowFit &f = fixtureFit(name);
    return covariances.emplace(name, fitCovariance(f.fit, f.window.gnssTime.size())).first->second;
}

/// The reconstruction of fixtureFit(name) with its covariance composed at
/// every sample, once per run.
const ImuRateTrajectory &fixtureTrajectory(const QString &name)
{
    static std::map<QString, ImuRateTrajectory> trajectories;
    const auto found = trajectories.find(name);
    if (found != trajectories.end())
        return found->second;
    const WindowFit &f = fixtureFit(name);
    return trajectories.emplace(name, reconstructAtImuRate(f.window, f.fit, f.tuning, &fixtureCovariance(name)))
        .first->second;
}

/// What the pipeline publishes for a fixture with the production tuning, once
/// per run: the success assembly (assembleSuccess(), the pipeline's seam after
/// the covariance step) on fixtureFit(name) with fixtureCovariance(name), so
/// that no fixture is fitted twice. imuRateIsWhatTheFitPublishes holds the
/// pipeline's own runs to the reconstruction of these fits bit for bit. A fit
/// that did not converge is a SolverFailed result without channels, as the
/// pipeline's is.
const Fusion::Result &publishedRun(const QString &name)
{
    static std::map<QString, Fusion::Result> runs;
    const auto found = runs.find(name);
    if (found != runs.end())
        return found->second;
    const WindowFit &f = fixtureFit(name);
    Fusion::Result result;
    if (f.fit.converged) {
        result = assembleSuccess(prepareInput(toChannels(fixtureNamed(name))), f.account, f.window, f.fit, f.tuning,
                                 fixtureCovariance(name));
    } else {
        result.outcome = Fusion::Outcome::SolverFailed;
        result.reason = QStringLiteral("the full fit did not converge (%1)")
                            .arg(QString::fromStdString(f.fit.stopping.rule));
    }
    return runs.emplace(name, std::move(result)).first->second;
}

/// rest_throughout as the pipeline publishes it (publishedRun()), with the
/// trace of its full fit and the initializer's account (fixtureFit()'s). Its
/// fit is the longest of the fixtures', and the initializer's tests, the scale
/// state's and the accuracy's all read it, so it is made once per run.
const InitializerRun &restThroughoutRun()
{
    static const InitializerRun run = [] {
        const QString name = QStringLiteral("rest_throughout");
        const WindowFit &f = fixtureFit(name);
        InitializerRun r;
        r.result = publishedRun(name);
        r.trace.initializer = f.account;
        r.trace.history = f.fit.history;
        r.trace.converged = f.fit.converged;
        r.trace.stopping = f.fit.stopping;
        parseDiagnostics(r);
        return r;
    }();
    return run;
}

/// The joint covariance of `keys` (in that order) from the library's joint
/// marginals, as one dense matrix.
gtsam::Matrix jointOf(const gtsam::Marginals &marginals, const gtsam::KeyVector &keys, const gtsam::Values &values)
{
    const gtsam::JointMarginal joint = marginals.jointMarginalCovariance(keys);
    std::vector<Eigen::Index> offset{0};
    for (const gtsam::Key key : keys)
        offset.push_back(offset.back()+Eigen::Index(values.at(key).dim()));
    gtsam::Matrix m(offset.back(), offset.back());
    for (size_t a = 0; a < keys.size(); ++a) {
        for (size_t b = 0; b < keys.size(); ++b)
            m.block(offset[a], offset[b], offset[a+1]-offset[a], offset[b+1]-offset[b]) = joint(keys[a], keys[b]);
    }
    return m;
}

/// The keys of z_k = (x_k, x_k+1, B(0), T(0), S(0)), FitCovariance::pair()'s order.
gtsam::KeyVector pairKeys(size_t k)
{
    return {X(k), V(k), X(k+1), V(k+1), B(0), T(0), S(0)};
}

/// The Frobenius norm of got - expected over that of expected.
double frobeniusRelative(const gtsam::Matrix &got, const gtsam::Matrix &expected)
{
    return (got-expected).norm()/expected.norm();
}

/// The 9x9 joint of (phi body, b_a, s_a) at fix k from the covariance step:
/// what a sample on that fix composes to.
gtsam::Matrix9 fixAttitudeBiasScale(const FitCovariance &c, size_t k)
{
    gtsam::Matrix9 joint;
    joint.block<3, 3>(0, 0) = c.node[k].block<3, 3>(0, 0);
    joint.block<3, 3>(0, 3) = c.global[k].block<3, 3>(0, 0);
    joint.block<3, 3>(0, 6) = c.global[k].block<3, 3>(0, 9);
    joint.block<3, 3>(3, 0) = joint.block<3, 3>(0, 3).transpose();
    joint.block<3, 3>(6, 0) = joint.block<3, 3>(0, 6).transpose();
    joint.block<3, 3>(3, 3) = c.globals.block<3, 3>(0, 0);
    joint.block<3, 3>(3, 6) = c.globals.block<3, 3>(0, 9);
    joint.block<3, 3>(6, 3) = c.globals.block<3, 3>(9, 0);
    joint.block<3, 3>(6, 6) = c.globals.block<3, 3>(9, 9);
    return joint;
}

/// |got - expected| <= relative |expected|.
bool withinRelative(double got, double expected, double relative)
{
    return std::abs(got-expected) <= relative*std::abs(expected);
}

/// A 9x9 joint of (phi, b_a, s_a) from its three diagonal blocks.
gtsam::Matrix9 blockDiagonal(const gtsam::Matrix3 &attitude, const gtsam::Matrix3 &bias, const gtsam::Matrix3 &scale)
{
    gtsam::Matrix9 joint = gtsam::Matrix9::Zero();
    joint.block<3, 3>(0, 0) = attitude;
    joint.block<3, 3>(3, 3) = bias;
    joint.block<3, 3>(6, 6) = scale;
    return joint;
}

/// The fitted state at fix `k` of `fit`.
gtsam::NavState fixState(const FitResult &fit, size_t k)
{
    return gtsam::NavState(fit.values.at<gtsam::Pose3>(X(k)), fit.values.at<Vector3>(V(k)));
}

/// The angle between two attitudes, rad.
double angleBetween(const Rot3 &a, const Rot3 &b)
{
    return Rot3::Logmap(a.between(b)).norm();
}

/// Criterion of "the same state to rounding": attitude within 1e-12 rad,
/// velocity and position within 1e-12 x (1 + their norm).
bool sameStateToRounding(const gtsam::NavState &got, const gtsam::NavState &expected)
{
    return angleBetween(got.attitude(), expected.attitude()) <= 1e-12
        && (got.velocity()-expected.velocity()).norm() <= 1e-12*(1+expected.velocity().norm())
        && (got.position()-expected.position()).norm() <= 1e-12*(1+expected.position().norm());
}

/// The per-interval seam of a whole window, on the window's one sequence of
/// edges and steps: each interval's edges but its last, then the last fix.
struct WindowSeams {
    std::vector<IntervalReconstruction> intervals;
    std::vector<double> edges;            ///< each fix once
    NavStates forward, corrected;         ///< per edge
    Vectors stepCorrection;               ///< per step
    std::vector<size_t> stepInterval;     ///< the fix interval of each step
    std::vector<size_t> fixEdge;          ///< the edge of each fix
};

WindowSeams seamsOf(const WindowFit &f)
{
    WindowSeams w;
    for (size_t k = 0; k+1 < f.window.gnssTime.size(); ++k) {
        w.intervals.push_back(reconstructInterval(f.window, f.fit, f.tuning, k));
        const IntervalReconstruction &r = w.intervals.back();
        w.fixEdge.push_back(w.edges.size());
        for (size_t j = 0; j < r.stepCorrection.size(); ++j) {
            w.edges.push_back(r.edges[j]);
            w.forward.push_back(r.forward[j]);
            w.corrected.push_back(r.corrected[j]);
            w.stepCorrection.push_back(r.stepCorrection[j]);
            w.stepInterval.push_back(k);
        }
    }
    const IntervalReconstruction &last = w.intervals.back();
    w.fixEdge.push_back(w.edges.size());
    w.edges.push_back(last.edges.back());
    w.forward.push_back(last.forward.back());
    w.corrected.push_back(last.corrected.back());
    return w;
}

/// The edge of the window's sequence at time `t`, which must be one.
size_t edgeAt(const WindowSeams &w, double t)
{
    return size_t(std::lower_bound(w.edges.begin(), w.edges.end(), t)-w.edges.begin());
}

/// The largest norm of each part of the mismatch over the intervals.
struct Mismatch { double attitude = 0, position = 0, velocity = 0; };

Mismatch largestMismatch(const WindowSeams &w)
{
    Mismatch m;
    for (const IntervalReconstruction &r : w.intervals) {
        m.attitude = std::max(m.attitude, r.mismatch.head<3>().norm());
        m.position = std::max(m.position, r.mismatch.segment<3>(3).norm());
        m.velocity = std::max(m.velocity, r.mismatch.tail<3>().norm());
    }
    return m;
}

/// The reference of the equivalence tests: a state at every edge of the
/// window, a one-step IMU factor across every step, the fix states and the
/// bias (and slope) held at the fit's values, solved from the forward states.
/// Each step's factor is preintegrated by preintegrateImu() over that step
/// alone, which gives the loop's own midpoint reading and per-step covariance
/// without restating either, at its interval's bias under the fit's model and
/// at the fit's scale. The scale is held too, so each step's factor is the
/// fit's own scaled factor with S at its linearization.
/// The fix states are held because, freed with GNSS factors, they move along
/// the unobservable heading, which is not what the tests measure.
NavStates heldEndsReference(const WindowFit &f, const WindowSeams &w, int &iterations)
{
    using gtsam::imuBias::ConstantBias;
    const ConstantBias bias = f.fit.values.at<ConstantBias>(B(0));
    const GyroBiasModel &model = f.fit.biasModel;
    gtsam::NonlinearFactorGraph graph;
    gtsam::Values values;
    for (size_t j = 0; j < w.edges.size(); ++j) {
        values.insert(X(j), w.forward[j].pose());
        values.insert(V(j), Vector3(w.forward[j].velocity()));
    }
    for (size_t k = 0; k < w.fixEdge.size(); ++k) {
        const gtsam::Pose3 pose = f.fit.values.at<gtsam::Pose3>(X(k));
        const Vector3 velocity = f.fit.values.at<Vector3>(V(k));
        values.update(X(w.fixEdge[k]), pose);
        values.update(V(w.fixEdge[k]), velocity);
        graph.emplace_shared<gtsam::NonlinearEquality<gtsam::Pose3>>(X(w.fixEdge[k]), pose);
        graph.emplace_shared<gtsam::NonlinearEquality<Vector3>>(V(w.fixEdge[k]), velocity);
    }
    values.insert(B(0), bias);
    graph.emplace_shared<gtsam::NonlinearEquality<ConstantBias>>(B(0), bias);
    if (model.temperatureLinear) {
        values.insert(T(0), f.fit.gyroBiasSlope);
        graph.emplace_shared<gtsam::NonlinearEquality<Vector3>>(T(0), f.fit.gyroBiasSlope);
        values.insert(S(0), f.fit.scale);
        graph.emplace_shared<gtsam::NonlinearEquality<Vector6>>(S(0), f.fit.scale);
    }
    for (size_t j = 0; j+1 < w.edges.size(); ++j) {
        const size_t k = w.stepInterval[j];
        gtsam::Matrix96 scaleJacobian;
        const auto pim = preintegrateImu(f.window, w.edges[j], w.edges[j+1],
                                         intervalBias(f.window, k, bias, f.fit.gyroBiasSlope, model), f.fit.scale,
                                         f.tuning.noise, ImuStepObserver(), &scaleJacobian);
        if (model.temperatureLinear)
            graph.emplace_shared<ScaledImuFactor>(X(j), V(j), X(j+1), V(j+1), B(0), T(0), S(0), pim,
                                                  temperatureAtFix(f.window, k)-model.tRef, scaleJacobian,
                                                  f.fit.scale);
        else
            graph.emplace_shared<gtsam::ImuFactor>(X(j), V(j), X(j+1), V(j+1), B(0), pim);
    }
    // The fit's solver, driven to a tight settle.
    gtsam::LevenbergMarquardtParams params;
    params.setLinearSolverType("MULTIFRONTAL_QR");
    gtsam::LevenbergMarquardtOptimizer optimizer(graph, values, params);
    for (iterations = 0; iterations < 100;) {
        const double before = optimizer.error();
        optimizer.iterate();
        ++iterations;
        if (before-optimizer.error() <= 1e-14*std::max(1., before))
            break;
    }
    NavStates states;
    for (size_t j = 0; j < w.edges.size(); ++j)
        states.emplace_back(optimizer.values().at<gtsam::Pose3>(X(j)), optimizer.values().at<Vector3>(V(j)));
    return states;
}

/// The largest difference between the published states and `reference` at
/// their edges: attitude (rad), velocity, position.
struct StateDifference { double attitude = 0, velocity = 0, position = 0; };

StateDifference largestDifference(const ImuRateTrajectory &out, const WindowSeams &w, const NavStates &reference)
{
    StateDifference d;
    for (size_t i = 0; i < out.time.size(); ++i) {
        const gtsam::NavState &r = reference[edgeAt(w, out.time[i])];
        d.attitude = std::max(d.attitude, angleBetween(out.rotation[i], r.attitude()));
        d.velocity = std::max(d.velocity, (out.velocity[i]-r.velocity()).norm());
        d.position = std::max(d.position, (out.position[i]-r.position()).norm());
    }
    return d;
}

/// The rotating recording of the equivalence test: 3 rad/s about the
/// horizontal y axis (body and navigation), a smooth translation, IMU at
/// `imuRate` from 0 to 6 s and GNSS at 5 Hz from .013 s, exact readings, the
/// true states at the fixes as the fit, zero bias, the constant model.
struct Tumble {
    static constexpr double kRate = 3;   ///< rad/s
    static Rot3 attitude(double t) { return Rot3::Expmap(Vector3(0, kRate*t, 0)); }
    static Vector3 acceleration(double t) { return Vector3(1., .5*std::sin(.7*t), -.3); }
    static Vector3 velocity(double t) { return Vector3(30+t, 2-.5/.7*(std::cos(.7*t)-1), 10-.3*t); }
    static Vector3 position(double t)
    {
        return Vector3(30*t+t*t/2, 2*t-.5/.7*(std::sin(.7*t)/.7-t), 10*t-.15*t*t);
    }
};

/// The rotating recording's samples and fixes, with no fitted state yet.
WindowFit tumbleWindow(double imuRate)
{
    WindowFit f;
    Samples &d = f.window;
    const int last = int(std::floor(6*imuRate));
    for (int i = 0; i <= last; ++i) {
        const double t = i/imuRate;
        d.imuTime.push_back(t);
        d.force.push_back(Tumble::attitude(t).unrotate(Tumble::acceleration(t)-kTestGravity));
        d.gyro.emplace_back(0, Tumble::kRate, 0);
    }
    for (int k = 0; .013+k*.2 <= d.imuTime.back()-.05; ++k)
        d.gnssTime.push_back(.013+k*.2);
    f.tuning.maxGap = SampleContinuity::holeThreshold(d.imuTime);
    // The listed rate nearest the sampling: 12.5, 26 or 104 Hz.
    f.tuning.noise = fixtureNoise(imuRate < 19 ? 12.5 : imuRate < 60 ? 26 : 104);
    return f;
}

WindowFit tumbleFit(double imuRate)
{
    WindowFit f = tumbleWindow(imuRate);
    const Samples &d = f.window;
    f.fit.values.insert(B(0), gtsam::imuBias::ConstantBias());
    for (size_t k = 0; k < d.gnssTime.size(); ++k) {
        const double t = d.gnssTime[k];
        f.fit.values.insert(X(k), gtsam::Pose3(Tumble::attitude(t), Tumble::position(t)));
        f.fit.values.insert(V(k), Tumble::velocity(t));
    }
    return f;
}

/// The covariance of the IMU factor between fixes `k` and `k+1` in `graph`,
/// found by its keys; empty when there is none.
gtsam::Matrix imuFactorCovariance(const gtsam::NonlinearFactorGraph &graph, size_t k)
{
    for (const auto &factor : graph) {
        if (!factor || factor->keys().size() < 4 || factor->keys()[0] != X(k) || factor->keys()[2] != X(k+1))
            continue;
        if (const auto *scaled = dynamic_cast<const ScaledImuFactor *>(factor.get()))
            return scaled->preintegratedMeasurements().preintMeasCov();
        if (const auto *stock = dynamic_cast<const gtsam::ImuFactor *>(factor.get()))
            return stock->preintegratedMeasurements().preintMeasCov();
    }
    return gtsam::Matrix();
}

/// The fixes and IMU samples of a WindowFit whose fitted states are each the
/// prediction of the one before, at the fit's scale (ones unless the caller
/// set another), perturbed by `perturbation` (zero: the forward integration
/// exactly, interval by interval).
void predictFits(WindowFit &f, const gtsam::imuBias::ConstantBias &bias, const gtsam::NavState &first,
                 const gtsam::Vector9 &perturbation)
{
    f.fit.values.insert(B(0), bias);
    gtsam::NavState state = first;
    for (size_t k = 0; k < f.window.gnssTime.size(); ++k) {
        if (k) {
            const auto pim = preintegrateImu(f.window, f.window.gnssTime[k-1], f.window.gnssTime[k], bias, f.fit.scale,
                                             f.tuning.noise);
            state = pim.predict(state, bias);
            if (!perturbation.isZero(0))
                state = state.retract(perturbation);
        }
        f.fit.values.insert(X(k), state.pose());
        f.fit.values.insert(V(k), Vector3(state.velocity()));
    }
}

/// The IMU samples of `window` in [first fix, last fix): the published time axis.
std::vector<double> samplesBetweenFirstAndLastFix(const Samples &window)
{
    const auto first = std::lower_bound(window.imuTime.begin(), window.imuTime.end(), window.gnssTime.front());
    const auto last = std::lower_bound(window.imuTime.begin(), window.imuTime.end(), window.gnssTime.back());
    return std::vector<double>(first, last);
}

/// The published time axis as a run's diagnostics define it: the epoch
/// (`input.epoch_utc_s`) plus every IMU sample of the prepared recording in
/// [start_s, end_s), the first and last fitted fix.
QVector<double> expectedTimeAxis(const Fusion::Channels &channels, const QJsonObject &diagnostics)
{
    const double epoch = diagnostics.value("input").toObject().value("epoch_utc_s").toDouble();
    const double start = diagnostics.value("start_s").toDouble(), end = diagnostics.value("end_s").toDouble();
    QVector<double> axis;
    for (const double t : prepareInput(channels).recording.imuTime) {
        if (t >= start && t < end)
            axis.append(epoch+t);
    }
    return axis;
}

/// Criterion of the time-axis tests: `time` is `expected` element by element
/// (==), and as long as the diagnostics' imu_outputs says.
void verifyTimeAxis(const QVector<double> &time, const QVector<double> &expected, const QJsonObject &diagnostics)
{
    QCOMPARE(time.size(), expected.size());
    QCOMPARE(qsizetype(diagnostics.value("imu_outputs").toInt(-1)), expected.size());
    for (qsizetype i = 0; i < expected.size(); ++i)
        QVERIFY2(time[i] == expected[i], qPrintable(QString::number(i)));
}

/// The published acceleration at sample `i` as spec section 6 states it, from
/// the seam's corrections: the step before and the step after the sample's
/// edge, the step after alone on the window's first edge; the reading divided
/// by the fitted accelerometer scale.
Vector3 expectedAcceleration(const WindowFit &f, const WindowSeams &w, const ImuRateTrajectory &out, size_t i)
{
    const size_t e = edgeAt(w, out.time[i]);
    const size_t sample = size_t(std::lower_bound(f.window.imuTime.begin(), f.window.imuTime.end(), out.time[i])
                                 -f.window.imuTime.begin());
    const Vector3 correction = e ? Vector3((w.stepCorrection[e-1]+w.stepCorrection[e])/2) : w.stepCorrection[e];
    const Vector3 accBias = f.fit.values.at<gtsam::imuBias::ConstantBias>(B(0)).accelerometer();
    const Vector3 accScale = f.fit.scale.head<3>();
    return out.rotation[i].rotate(f.window.force[sample].cwiseQuotient(accScale)-accBias)+kGravity+correction;
}

} // namespace

class FusionKernelTest : public QObject {
    Q_OBJECT

private slots:
    void solverUsesTbb();
    void unwrapRule();
    void preintegrationHonoursExactBoundaries();
    void noiseFollowsTheTable();
    void configurationWithoutEntryIsRejected();
    void constantSignalHasNoSamplingTerm();
    void samplingTermFollowsTheDerivation();
    void rotationRemainderFollowsTheDerivation();
    void rotationRemainderMatchesTheSchemeError();
    void latticeCheckFindsTheCoarsestRange();
    void latticeCheckIdentifiesEveryFixture();
    void rateCheckToleratesTenPercent();
    void configurationChecksComeAfterTheOthers();
    void diagnosticsReportTheNoiseModel();
    void validationRejectsEachDefect();
    void backwardPropagationUndoesForward();
    void propagationRefusesIntervalAboveItsThreshold();
    void imuGapRuleIsTheContinuityThreshold();
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
    void dampingSaturationIsASolverFailure();
    void dampingCeilingChangesNothingBelowIt();
    void saturationAtAMinimumIsSettled();
    void startsInMotionGrowsToTheManoeuvre();
    void atRestPrefixStopsGrowing();
    void smallestSaccFixIsTheAnchor();
    void driftingBiasSegmentsConverge();
    void allPrefixFitsFailFallsBack_data();
    void allPrefixFitsFailFallsBack();
    void prefixFitsFailAfterACompletedLength();
    void startsOnTheLimitAreStillUsed();
    void temperatureGraphShape();
    void reconstructionUsesIntervalBias();
    void constantTemperatureKeepsSlopeAtPrior();
    void preintegrationDividesByTheScale();
    void scaleJacobianMatchesCentralDifferences();
    void scaleFactorJacobians();
    void scaleGraphShape();
    void fitRepreintegratesAtTheFittedScale();
    void reconstructionUsesTheFittedScale();
    void restLeavesTheScaleAtItsPrior();
    void scaleRecordingRecoversTheFactor();
    void diagnosticsReportTheScale();
    void scaleReleaseIsAccountedFor();
    void releaseFailureFallsBackToTheHeldFit();
    void divergenceEndsTheHeldStage_data();
    void divergenceEndsTheHeldStage();
    void divergenceEndsTheReleasedStage();
    void imuRateEndsAreTheFit_data();
    void imuRateEndsAreTheFit();
    void imuRateSampleOnAFixIsPublishedOnce();
    void imuRateMatchesHeldEndsGraph();
    void imuRateMatchesHeldEndsGraphUnderRotation();
    void imuRateSharesByNoise();
    void imuRateZeroMismatchIsForward();
    void imuRateZeroMismatchUnderRotation();
    void imuRateAccelerationIntegratesToVelocity_data();
    void imuRateAccelerationIntegratesToVelocity();
    void imuRateIsWhatTheFitPublishes_data();
    void imuRateIsWhatTheFitPublishes();
    void imuRateAxisWhenGnssIsFasterThanImu();
    void covarianceMatchesJointMarginals();
    void firstNodeHeadingIsTheHeadingCheck();
    void sampleCovarianceMatchesTheEdgeGraph();
    void sampleOnAFixHasTheFixMarginal();
    void attitudeAccuracyFollowsTheNavigationFrame();
    void accelerationAccuracyFollowsItsPropagation();
    void wideningWindowAndFactor();
    void wideningIsOneAtTheModel();
    void wideningGrowsWithAnUnderstatedSigma();
    void accuraciesFiniteAndPositive();
    void gnssAccuracyScalingNeverLowersThem();
    void undeterminedHeadingIsCapped();
    void covarianceFailureLeavesTheFitAsItIs();
    void diagnosticsReportTheScaleSigma();
    void bridgedHoleFollowsTheTruth();
    void longHoleConverges();
    void holeAboveTheCapIsRejected();
    void sparsePiecesAreMerged();
    void gnssHolesInTheAudit();
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
    const Tuning tuning = tuningAt(104);
    validateSamples(d, tuning);

    const gtsam::imuBias::ConstantBias bias;
    const auto pim = preintegrateImu(d, .037, .863, bias, Vector6::Ones(), tuning.noise);
    const auto predicted = pim.predict(gtsam::NavState(gtsam::Pose3(), Vector3::Zero()), bias);
    const double duration = .863-.037;
    QVERIFY((predicted.velocity()-acceleration*duration).norm() < 1e-10);
    QVERIFY((predicted.position()-.5*acceleration*duration*duration).norm() < 1e-10);
}

void FusionKernelTest::noiseFollowsTheTable()
{
    // Clauses 11, 13 and 52: for each configuration the fixtures state, the
    // per-sample sigma and the integration density are the formula of the
    // specification with the datasheet's literals (DS12140 Rev 3: An 110
    // ug/sqrt(Hz) at +/-16 g, Rn 3.8 mdps/sqrt(Hz), the gyro's LPF2 cutoff of
    // Table 18, the accelerometer at ODR / 2, the step the accelerometer's
    // range over 32768 counts and the gyro's sensitivity). The datasheet's
    // densities, the bandwidths and the steps involve no sum, so no compiler
    // contracts them, and are compared exactly; the sigma and the density,
    // from a sum of products, are recomputed values under tests/README.md
    // section 11's policy (the same bits on the capture compiler, within
    // 4 ulp where a compiler may contract the sum in one place only).
    const double g = 9.80665, radians = kPi/180;
    const struct { double rate, gyroBandwidth; } configurations[] = {{12.5, 4.2}, {26, 8.3}, {104, 33.0}};
    for (const auto &c : configurations) {
        const ImuNoise n = fixtureNoise(c.rate);
        QCOMPARE(n.configuration.accelFsG, 16.);
        QCOMPARE(n.configuration.gyroFsDegS, 2000.);
        QCOMPARE(n.configuration.gyroOdrHz, c.rate);

        const double accDatasheet = 110e-6*g, accStep = 16./32768*g, accBandwidth = c.rate/2;
        const double accSigma = std::sqrt(accDatasheet*accDatasheet*accBandwidth + accStep*accStep/12);
        const SensorNoise &a = n.accelerometer;
        QVERIFY(a.rate == c.rate);
        QVERIFY(a.datasheetDensity == accDatasheet);
        QVERIFY(a.bandwidth == accBandwidth);
        QVERIFY(a.step == accStep);
        QVERIFY(sameRecomputedValue(a.sampleSigma, accSigma));
        QVERIFY(sameRecomputedValue(a.density, accSigma*std::sqrt(1/c.rate)));
        QVERIFY(a.sensitivityTolerance == .01);

        const double gyroDatasheet = 3.8e-3*radians, gyroStep = 70e-3*radians;
        const double gyroSigma = std::sqrt(gyroDatasheet*gyroDatasheet*c.gyroBandwidth + gyroStep*gyroStep/12);
        const SensorNoise &w = n.gyroscope;
        QVERIFY(w.rate == c.rate);
        QVERIFY(w.datasheetDensity == gyroDatasheet);
        QVERIFY(w.bandwidth == c.gyroBandwidth);
        QVERIFY(w.step == gyroStep);
        QVERIFY(sameRecomputedValue(w.sampleSigma, gyroSigma));
        QVERIFY(sameRecomputedValue(w.density, gyroSigma*std::sqrt(1/c.rate)));
        QVERIFY(w.sensitivityTolerance == .01);
        qInfo() << c.rate << "Hz: accelerometer sigma" << a.sampleSigma << "m/s^2, density" << a.density
                << "; gyro sigma" << w.sampleSigma << "rad/s, density" << w.density;
    }
    // The 12.5 Hz accelerometer sigma is the corpus's quietest-window floor
    // (0.0029-0.0035 m/s^2); the overview's table to its five digits.
    QVERIFY(std::abs(fixtureNoise(12.5).accelerometer.sampleSigma-.0030304) < 1e-7);
    QVERIFY(std::abs(fixtureNoise(104).gyroscope.density-5.0909e-5) < 1e-9);

    // Every value the vocabulary lists has an entry (the accelerometer's
    // 1.6 Hz excepted: configurationWithoutEntryIsRejected), with the
    // datasheet's step at each range and bandwidth at each rate.
    const struct { const char *text; double value, step; } accelerometerRanges[] = {
        {"2", 2, 2./32768*g}, {"4", 4, 4./32768*g}, {"8", 8, 8./32768*g}, {"16", 16, 16./32768*g}};
    const struct { const char *text; double value, step; } gyroRanges[] = {
        {"250", 250, 8.75e-3}, {"500", 500, 17.5e-3}, {"1000", 1000, 35e-3}, {"2000", 2000, 70e-3}};
    const struct { const char *text; double value, gyroBandwidth; } rates[] = {
        {"12.5", 12.5, 4.2}, {"26", 26, 8.3}, {"52", 52, 16.6}, {"104", 104, 33.0}, {"208", 208, 66.8},
        {"416", 416, 135.9}, {"833", 833, 295.5}, {"1666", 1666, 1108.1}, {"3333", 3333, 1320.7},
        {"6666", 6666, 1441.8}};
    for (const auto &accelerometer : accelerometerRanges) {
        QVERIFY(SensorConfiguration::isValidValue(QString::fromLatin1(SensorConfiguration::AccelFsG), QString::fromLatin1(accelerometer.text)));
        for (const auto &gyro : gyroRanges) {
            QVERIFY(SensorConfiguration::isValidValue(QString::fromLatin1(SensorConfiguration::GyroFsDegS), QString::fromLatin1(gyro.text)));
            for (const auto &rate : rates) {
                QVERIFY(SensorConfiguration::isValidValue(QString::fromLatin1(SensorConfiguration::AccelOdrHz), QString::fromLatin1(rate.text)));
                QVERIFY(SensorConfiguration::isValidValue(QString::fromLatin1(SensorConfiguration::GyroOdrHz), QString::fromLatin1(rate.text)));
                ImuConfiguration configuration;
                configuration.accelFsG = accelerometer.value;
                configuration.gyroFsDegS = gyro.value;
                configuration.accelOdrHz = rate.value;
                configuration.gyroOdrHz = rate.value;
                const ImuNoise n = imuNoise(configuration);
                QVERIFY(n.accelerometer.step == accelerometer.step);
                QVERIFY(n.gyroscope.step == gyro.step*radians);
                QVERIFY(n.accelerometer.bandwidth == rate.value/2);
                QVERIFY(n.gyroscope.bandwidth == rate.gyroBandwidth);
                for (const SensorNoise *s : {&n.accelerometer, &n.gyroscope})
                    QVERIFY(std::isfinite(s->density) && s->density > 0 && s->sampleSigma > s->step/std::sqrt(12.));
            }
        }
    }
}

void FusionKernelTest::configurationWithoutEntryIsRejected()
{
    // Clauses 46 and 52, the negative half: a value outside a key's list, or
    // not a number, has no entry, and the rejection names the first such key
    // in key order. The accelerometer's 1.6 Hz is a low-power rate with no
    // entry; +/-125 deg/s is a datasheet range the keys do not list.
    const double NaN = std::numeric_limits<double>::quiet_NaN();
    const auto noEntry = [](const char *key, const char *value) {
        return QStringLiteral("No datasheet entry for %1 = %2; sensor fusion unavailable")
            .arg(QLatin1String(key), QLatin1String(value));
    };
    const auto reasonFor = [](const ImuConfiguration &configuration) {
        try {
            imuNoise(configuration);
        } catch (const std::invalid_argument &e) {
            return QString::fromUtf8(e.what());
        }
        return QString();
    };
    const ImuConfiguration valid = fixtureNoise(12.5).configuration;
    struct Case { double ImuConfiguration::*member; double value; const char *key, *text; };
    const Case cases[] = {
        {&ImuConfiguration::accelOdrHz, 1.6, SensorConfiguration::AccelOdrHz, "1.6"},
        {&ImuConfiguration::accelOdrHz, 3, SensorConfiguration::AccelOdrHz, "3"},
        {&ImuConfiguration::accelOdrHz, NaN, SensorConfiguration::AccelOdrHz, "nan"},
        {&ImuConfiguration::accelFsG, 3, SensorConfiguration::AccelFsG, "3"},
        {&ImuConfiguration::accelFsG, NaN, SensorConfiguration::AccelFsG, "nan"},
        {&ImuConfiguration::gyroFsDegS, 125, SensorConfiguration::GyroFsDegS, "125"},
        {&ImuConfiguration::gyroFsDegS, NaN, SensorConfiguration::GyroFsDegS, "nan"},
        {&ImuConfiguration::gyroOdrHz, 1.6, SensorConfiguration::GyroOdrHz, "1.6"},
        {&ImuConfiguration::gyroOdrHz, 3332, SensorConfiguration::GyroOdrHz, "3332"},
        {&ImuConfiguration::gyroOdrHz, NaN, SensorConfiguration::GyroOdrHz, "nan"}};
    for (const Case &c : cases) {
        ImuConfiguration configuration = valid;
        configuration.*(c.member) = c.value;
        QCOMPARE(reasonFor(configuration), noEntry(c.key, c.text));
    }
    // Key order: the first key without an entry is named.
    QCOMPARE(reasonFor(ImuConfiguration{}), noEntry(SensorConfiguration::AccelFsG, "nan"));
    ImuConfiguration two = valid;
    two.gyroOdrHz = 1.6;
    two.gyroFsDegS = 125;
    QCOMPARE(reasonFor(two), noEntry(SensorConfiguration::GyroFsDegS, "125"));

    // Through the pipeline: a Rejected result with that reason.
    Fusion::Channels channels = toChannels(fusionFixture(QStringLiteral("coarse_linear")));
    channels.imuConfiguration.accelOdrHz = 1.6;
    QVERIFY(rejectedWith(rejectedBy(channels), noEntry(SensorConfiguration::AccelOdrHz, "1.6")));
    channels.imuConfiguration = ImuConfiguration{};
    QVERIFY(rejectedWith(rejectedBy(channels), noEntry(SensorConfiguration::AccelFsG, "nan")));

    // The integration refuses a noise that was never derived.
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                             preintegrateImu(boundarySamples(Vector3::Zero()), .037, .863,
                                             gtsam::imuBias::ConstantBias(), Vector6::Ones(), ImuNoise{}));
}

void FusionKernelTest::constantSignalHasNoSamplingTerm()
{
    // Clauses 14 and 53: a constant force and zero rate (zero bias): every
    // step, the two part steps beside the fixes included, has the densities'
    // covariance exactly (Eigen's == is element-wise equality, no
    // tolerance): no sampling term and no remainder.
    const Samples d = boundarySamples(Vector3(1, -2, .5));
    const ImuNoise noise = fixtureNoise(104);
    const StepCovariances s = stepCovariances(d, .037, .863, gtsam::imuBias::ConstantBias(), noise);
    QCOMPARE(s.steps.size(), size_t(84));
    QVERIFY(s.steps.front().dt < .01 && s.steps.back().dt < .01);
    const double accelerometer = noise.accelerometer.density, gyroscope = noise.gyroscope.density;
    const gtsam::Matrix3 expectedAccelerometer = gtsam::I_3x3*(accelerometer*accelerometer);
    const gtsam::Matrix3 expectedGyroscope = gtsam::I_3x3*(gyroscope*gyroscope);
    for (size_t j = 0; j < s.steps.size(); ++j) {
        QVERIFY2(s.accelerometer[j] == expectedAccelerometer, qPrintable(QString::number(j)));
        QVERIFY2(s.gyroscope[j] == expectedGyroscope, qPrintable(QString::number(j)));
    }
}

void FusionKernelTest::samplingTermFollowsTheDerivation()
{
    // Clauses 14, 47 and 53, as settled: on a signal with a known, constant
    // second derivative, every step's covariance is the density's plus
    // (w f'')^2 / dt, w = 1/2 integral over the step of (t - t_k)(t_k+1 - t),
    // here by Simpson's rule (exact for it), for whole steps and the part
    // steps beside the two ends; the samples are unevenly spaced, and the
    // change of slope estimates f'' exactly there too. The remainder is zero:
    // the force case does not rotate, and in the rate case the force is zero
    // and the rate keeps its axis. And the sampling term is what the
    // integration gets wrong: the summed w f'' is the preintegrated
    // velocity's departure from the exact integral of the force (no rotation,
    // so the preintegration is the sum of the midpoint readings).
    const std::vector<double> times{0, .08, .17, .24, .33, .40, .49, .56};
    const double q = 5, start = .05, end = .45;      // f = q t^2, f'' = 2 q
    const ImuNoise noise = fixtureNoise(12.5);
    const double accelerometer = noise.accelerometer.density, gyroscope = noise.gyroscope.density;

    // The interval of `times` that holds the step, and the step's weight.
    const auto weightOf = [&](const ImuStep &step) {
        const size_t k = size_t(std::upper_bound(times.begin(), times.end(), (step.start+step.end)/2)
                                -times.begin())-1;
        return simpsonWeight(times[k], times[k+1], step.start, step.end);
    };

    const Samples forceCase = signalSamples(times, [q](double t) { return Vector3(q*t*t, 0, -9.80665); },
                                            [](double) { return Vector3(0, 0, 0); });
    const StepCovariances f = stepCovariances(forceCase, start, end, gtsam::imuBias::ConstantBias(), noise);
    QCOMPARE(f.steps.size(), size_t(6));
    double summed = 0, worst = 0;
    for (size_t j = 0; j < f.steps.size(); ++j) {
        const ImuStep &step = f.steps[j];
        const double w = weightOf(step), sampling = w*2*q;
        summed += sampling;
        const double expected = accelerometer*accelerometer + sampling*sampling/step.dt;
        worst = std::max(worst, std::abs(f.accelerometer[j](0, 0)-expected)/expected);
        QVERIFY2(isotropicWithin(f.accelerometer[j], expected, 1e-12), qPrintable(QString::number(j)));
        QVERIFY2(f.gyroscope[j] == gtsam::Matrix3(gtsam::I_3x3*(gyroscope*gyroscope)), qPrintable(QString::number(j)));
        // Not vacuous: the term is a part of the step's variance worth having.
        QVERIFY(sampling*sampling/step.dt > 1e-3*accelerometer*accelerometer);
    }
    // The whole steps carry h^3 / 12 of f'', the trapezoid rule's error.
    const double h = times[2]-times[1];
    QVERIFY(std::abs(weightOf(f.steps[1])-h*h*h/12) <= 1e-12*h*h*h);
    const double exact = q*(end*end*end-start*start*start)/3;
    const double departure = f.pim.deltaVij().x()-exact;
    qInfo() << "sampling term: worst covariance departure" << worst << "relative; summed w f''" << summed
            << "m/s against the preintegration's departure from the exact integral" << departure << "m/s";
    QVERIFY(std::abs(departure-summed) <= 1e-9*summed);

    const Samples rateCase = signalSamples(times, [](double) { return Vector3(0, 0, 0); },
                                           [q](double t) { return Vector3(0, 0, q*t*t); });
    const StepCovariances r = stepCovariances(rateCase, start, end, gtsam::imuBias::ConstantBias(), noise);
    for (size_t j = 0; j < r.steps.size(); ++j) {
        const double sampling = weightOf(r.steps[j])*2*q;
        const double expected = gyroscope*gyroscope + sampling*sampling/r.steps[j].dt;
        QVERIFY2(isotropicWithin(r.gyroscope[j], expected, 1e-12), qPrintable(QString::number(j)));
        QVERIFY2(r.accelerometer[j] == gtsam::Matrix3(gtsam::I_3x3*(accelerometer*accelerometer)),
                 qPrintable(QString::number(j)));
    }

    // Fewer than three samples: no change of slope exists, no term.
    const Samples two = signalSamples({0, .08}, [q](double t) { return Vector3(q*t, 0, -9.80665); },
                                      [](double) { return Vector3(0, 0, 0); });
    const StepCovariances one = stepCovariances(two, 0, .08, gtsam::imuBias::ConstantBias(), noise);
    QCOMPARE(one.steps.size(), size_t(1));
    QVERIFY(one.accelerometer[0] == gtsam::Matrix3(gtsam::I_3x3*(accelerometer*accelerometer)));
}

void FusionKernelTest::rotationRemainderFollowsTheDerivation()
{
    // Clauses 15 and 53: a constant turn, rate omega about z and force f along
    // x, so the signal is constant (no sampling term) and the step's rotation
    // theta = omega dt is perpendicular to the force: the remainder is the
    // pure rotation term alone, r_v = dt theta^2 |f| / 24, and the angle's
    // remainder is zero (the rate does not change).
    const double omega = .5, force = 10, dt = .08;   // theta = .04 at 12.5 Hz
    const ImuNoise noise = fixtureNoise(12.5);
    const double accelerometer = noise.accelerometer.density, gyroscope = noise.gyroscope.density;
    std::vector<double> times;
    for (int i = 0; i <= 4; ++i)
        times.push_back(i*dt);
    const Samples turn = signalSamples(times, [force](double) { return Vector3(force, 0, 0); },
                                       [omega](double) { return Vector3(0, 0, omega); });
    const StepCovariances s = stepCovariances(turn, 0, times.back(), gtsam::imuBias::ConstantBias(), noise);
    QCOMPARE(s.steps.size(), size_t(4));
    for (size_t j = 0; j < s.steps.size(); ++j) {
        const double theta = omega*s.steps[j].dt;
        const double remainder = s.steps[j].dt*theta*theta*force/24;
        QVERIFY2(isotropicWithin(s.accelerometer[j], accelerometer*accelerometer + remainder*remainder/s.steps[j].dt,
                                 1e-12), qPrintable(QString::number(j)));
        QVERIFY2(s.gyroscope[j] == gtsam::Matrix3(gtsam::I_3x3*(gyroscope*gyroscope)), qPrintable(QString::number(j)));
    }

    // The library's one-step velocity against the closed form of the true
    // velocity change, f / omega (sin theta, 1 - cos theta, 0): they differ
    // by r_v, to 1 %, for turns up to .05 rad per step.
    for (const double theta : {.01, .03, .05}) {
        const double step = theta/omega;
        const Samples one = signalSamples({0, step}, [force](double) { return Vector3(force, 0, 0); },
                                          [omega](double) { return Vector3(0, 0, omega); });
        const auto pim = preintegrateImu(one, 0, step, gtsam::imuBias::ConstantBias(), Vector6::Ones(), noise);
        const Vector3 truth = force/omega*Vector3(std::sin(theta), 1-std::cos(theta), 0);
        const double difference = (truth-pim.deltaVij()).norm();
        const double remainder = step*theta*theta*force/24;
        qInfo() << "constant turn of" << theta << "rad: scheme error" << difference << "m/s, r_v" << remainder;
        QVERIFY(std::abs(difference-remainder) <= .01*remainder);
    }
}

void FusionKernelTest::rotationRemainderMatchesTheSchemeError()
{
    // Clause 15, as settled: on a linear ramp of rate and force across one
    // step, the true velocity change minus the scheme's is, to second order,
    // the vector (dt/24) theta x (theta x fbar) + (dt/12) (theta x df -
    // dtheta x fbar), and the angle's |theta x dtheta| / 12 (coning). The
    // truth is the same signal preintegrated in 1000 steps (each a thousandth
    // of the turn, its own scheme error a millionth); the remainder of the
    // expansion is third order, under 5 % here. Zero bias.
    const double dt = .08;
    const Vector3 rateStart(.3, -.2, .5), rateEnd(.4, .1, .7);
    const Vector3 forceStart(1.5, -.5, -9.5), forceEnd(2.5, .5, -9.);
    const auto rate = [&](double t) { return Vector3(rateStart+(rateEnd-rateStart)*(t/dt)); };
    const auto force = [&](double t) { return Vector3(forceStart+(forceEnd-forceStart)*(t/dt)); };
    const ImuNoise noise = fixtureNoise(12.5);

    const auto one = preintegrateImu(signalSamples({0, dt}, force, rate), 0, dt, gtsam::imuBias::ConstantBias(),
                                     Vector6::Ones(), noise);
    std::vector<double> fine;
    for (int j = 0; j <= 1000; ++j)
        fine.push_back(dt*j/1000);
    fine.back() = dt;
    const auto many = preintegrateImu(signalSamples(fine, force, rate), 0, dt, gtsam::imuBias::ConstantBias(),
                                      Vector6::Ones(), noise);

    const Vector3 theta = rate(dt/2)*dt, dTheta = (rateEnd-rateStart)*dt;
    const Vector3 fbar = force(dt/2), dForce = forceEnd-forceStart;
    const Vector3 remainder = dt/24*theta.cross(theta.cross(fbar)) + dt/12*(theta.cross(dForce)-dTheta.cross(fbar));
    const Vector3 difference = many.deltaVij()-one.deltaVij();
    const double angle = Rot3::Logmap(one.deltaRij().between(many.deltaRij())).norm();
    const double coning = theta.cross(dTheta).norm()/12;
    qInfo() << "ramp: scheme error" << difference.x() << difference.y() << difference.z() << "m/s, r_v"
            << remainder.x() << remainder.y() << remainder.z() << "(departure"
            << (difference-remainder).norm()/remainder.norm() << "); angle" << angle << "rad, coning" << coning;
    QVERIFY((difference-remainder).norm() <= .05*remainder.norm());
    QVERIFY(std::abs(angle-coning) <= .05*coning);

    // And the covariance of that one step is the density's plus r_v and
    // r_theta, plus its sampling term, which is zero on a linear signal.
    const StepCovariances s = stepCovariances(signalSamples({0, dt}, force, rate), 0, dt,
                                              gtsam::imuBias::ConstantBias(), noise);
    const double accelerometer = noise.accelerometer.density, gyroscope = noise.gyroscope.density;
    QVERIFY(isotropicWithin(s.accelerometer[0], accelerometer*accelerometer + remainder.squaredNorm()/dt, 1e-12));
    QVERIFY(isotropicWithin(s.gyroscope[0], gyroscope*gyroscope + coning*coning/dt, 1e-12));
}

void FusionKernelTest::latticeCheckFindsTheCoarsestRange()
{
    // Clauses 8 and 9, as settled. The lattice steps: the range over 32768
    // counts times 9.80665 for the accelerometer, the datasheet's sensitivity
    // for the gyro. The tolerance: one unit of the file's last decimal
    // through the conversion layer's largest factor, 1e-5 g x 9.80665 and
    // 1e-3 deg/s x 1.14688 (the legacy correction, 70 mdps / (2000 / 32768)).
    const double g = 9.80665, accelerometerTolerance = 1e-5*g, gyroTolerance = 1e-3*1.14688;
    const double NaN = std::numeric_limits<double>::quiet_NaN();
    const auto accelerometerRange = [](const QVector<double> &x, const QVector<double> &y, const QVector<double> &z) {
        return rangeShownByReadings(ImuSensor::Accelerometer, x, y, z);
    };
    const auto gyroRange = [](const QVector<double> &x, const QVector<double> &y, const QVector<double> &z) {
        return rangeShownByReadings(ImuSensor::Gyroscope, x, y, z);
    };
    const auto same = [](double got, double expected) { return std::isnan(expected) ? std::isnan(got) : got == expected; };

    // Each range's lattice: odd multiples of its step lie on it and on no
    // coarser one, the three axes together.
    for (const double range : {16., 8., 4., 2.}) {
        const double s = range/32768*g;
        QVERIFY2(same(accelerometerRange({s, -3*s}, {7*s}, {-2047*s}), range), qPrintable(QString::number(range)));
        // One axis off the finer lattice decides for all three.
        QVERIFY(same(accelerometerRange({2*s}, {4*s}, {s}), range));
    }
    for (const auto &[range, s] : {std::pair<double, double>{2000, 70e-3}, {1000, 35e-3}, {500, 17.5e-3},
                                   {250, 8.75e-3}}) {
        QVERIFY2(same(gyroRange({s, -3*s}, {7*s}, {-4095*s}), range), qPrintable(QString::number(range)));
    }
    // Zero lies on every lattice: the coarsest is shown.
    QVERIFY(same(accelerometerRange({0}, {0}, {-2048*16./32768*g}), 16));
    QVERIFY(same(gyroRange({0}, {0}, {0}), 2000));

    // A legacy-style reading: the firmware writes counts x range / 32768
    // truncated to the file's decimals (g to five, deg/s to three), and the
    // conversion layer multiplies the gyro's by the legacy correction.
    // 12345 counts at +/-16 g are 6.02783203125 g, written 6.02783; 4321
    // counts at +/-2000 deg/s are 263.73291015625 deg/s, written 263.732 and
    // read as 302.46895616, 1.04e-3 deg/s from the lattice point 302.47.
    QVERIFY(same(accelerometerRange({6.02783*g}, {-6.02783*g}, {0}), 16));
    QVERIFY(same(gyroRange({263.732*1.14688}, {-263.732*1.14688}, {0}), 2000));
    // The printed 0.488 mg/LSB lattice would put the same reading 5.3e-4
    // m/s^2 off it, more than twice the tolerance: the lattice is the range
    // over 32768 counts, not the printed sensitivity.
    QVERIFY(std::abs(6.02783*g-std::round(6.02783/.488e-3)*.488e-3*g) > 2*accelerometerTolerance);

    // The tolerance's edges: a reading one tolerance from a lattice point fits
    // and one 1.5 tolerances from it does not; then no range's lattice fits.
    QVERIFY(same(accelerometerRange({accelerometerTolerance}, {0}, {0}), 16));
    QVERIFY(same(accelerometerRange({1.5*accelerometerTolerance}, {0}, {0}), NaN));
    QVERIFY(same(gyroRange({gyroTolerance}, {0}, {0}), 2000));
    QVERIFY(same(gyroRange({1.5*gyroTolerance}, {0}, {0}), NaN));

    // A few readings off the lattice among thousands do not decide: all but
    // one in a thousand must fit. 24-09-05/11-16-56 has two gyro readings of
    // 64,686 nine units off at 513 deg/s and was rejected before this rule.
    // Here 3000 readings on the +/-2000 lattice with three off (one in a
    // thousand) still show it, and four off show no range at all; a wrong
    // range leaves about half off, nowhere near the line. The offset,
    // 4.4e-3 deg/s, is half the finest step, so an offset reading lies on
    // no range's lattice.
    {
        QVector<double> axis(1000);
        for (int i = 0; i < 1000; ++i)
            axis[i] = (2*i+1)*70e-3;
        QVector<double> a = axis, b = axis, c = axis;
        a[10] += 4.4e-3;  b[500] += 4.4e-3;  c[999] -= 4.4e-3;
        QVERIFY(same(gyroRange(a, b, c), 2000));
        a[11] += 4.4e-3;
        QVERIFY(same(gyroRange(a, b, c), NaN));
    }

    // The kernel rule names the range stated and the range shown, or none.
    Fusion::Channels c = toChannels(fusionFixture(QStringLiteral("coarse_linear")));
    const Fusion::Channels linear = c;
    requireReadingsOnLattice(c);
    c.ax[5] = 8./32768*g;   // one reading on the +/-8 g lattice only
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, requireReadingsOnLattice(c));
    QVERIFY(rejectedWith(rejectedBy(c), QStringLiteral(
        "ACCEL_FS_G states +/-16 g but the accelerometer readings lie on the +/-8 g lattice; sensor fusion unavailable")));
    c.ax[5] = 1.5*accelerometerTolerance;
    QVERIFY(rejectedWith(rejectedBy(c), QStringLiteral(
        "ACCEL_FS_G states +/-16 g but the accelerometer readings lie on no range's lattice; sensor fusion unavailable")));
    c = linear;
    c.imuConfiguration.accelFsG = 8;   // a finer range stated than the readings show
    QVERIFY(rejectedWith(rejectedBy(c), QStringLiteral(
        "ACCEL_FS_G states +/-8 g but the accelerometer readings lie on the +/-16 g lattice; sensor fusion unavailable")));
    c = linear;
    c.wz[7] = 35e-3;
    QVERIFY(rejectedWith(rejectedBy(c), QStringLiteral(
        "GYRO_FS_DEG_S states +/-2000 deg/s but the gyro readings lie on the +/-1000 deg/s lattice; sensor fusion unavailable")));
    // Every reading counts, inside the fitted window or not: the last IMU
    // sample of coarse_linear lies past its last fix.
    c = linear;
    c.ay.last() = 8./32768*g;
    QVERIFY(c.imuTime.last() > c.gnssTime.last());
    QVERIFY(rejectedBy(c).outcome == Fusion::Outcome::Rejected);
}

void FusionKernelTest::latticeCheckIdentifiesEveryFixture()
{
    // Clause 51: the range shown by every committed fixture's readings is
    // the range it states (reject_lattice, built to show +/-8 g under a
    // stated +/-16 g, excepted; reject_nonfinite's NaN reading lies on no
    // lattice, and its earlier check rejects it). naturalSession's readings
    // (tst_fusion_session; this executable does not build sessions) are 0
    // and -9.80665 m/s^2 and 0 deg/s: the default's +/-16 g and +/-2000 deg/s.
    QList<FusionFixture> fixtures = fusionFixtures();
    for (const char *name : {"motion_start", "rest_throughout", "sacc_anchor", "drifting_bias", "scale_recording"})
        fixtures.append(initializerFixture(QLatin1String(name)));
    QCOMPARE(fixtures.size(), 19);
    for (const FusionFixture &f : fixtures) {
        const double accelerometer = rangeShownByReadings(ImuSensor::Accelerometer, f.ax, f.ay, f.az);
        const double gyro = rangeShownByReadings(ImuSensor::Gyroscope, f.wx, f.wy, f.wz);
        QCOMPARE(f.accelFsG, 16.);
        QCOMPARE(f.gyroFsDegS, 2000.);
        if (f.name == QStringLiteral("reject_lattice"))
            QCOMPARE(accelerometer, 8.);
        else if (f.name == QStringLiteral("reject_nonfinite"))
            QVERIFY(std::isnan(accelerometer));
        else
            QVERIFY2(accelerometer == f.accelFsG, qPrintable(f.name));
        QVERIFY2(gyro == f.gyroFsDegS, qPrintable(f.name));
    }
    const QVector<double> zeros(501, 0.), down(501, -9.80665);
    QCOMPARE(rangeShownByReadings(ImuSensor::Accelerometer, zeros, zeros, down), 16.);
    QCOMPARE(rangeShownByReadings(ImuSensor::Gyroscope, zeros, zeros, zeros), 2000.);
}

void FusionKernelTest::rateCheckToleratesTenPercent()
{
    // Clause 10, as settled: the median logged interval against 1 / rate of
    // each stated rate; 10 % passes at 9.9 % and rejects at 10.1 %, either
    // side; the accelerometer's rate is checked first; a gyro-only mismatch
    // names the gyro.
    const ImuConfiguration stated = fixtureNoise(12.5).configuration;
    const auto reasonFor = [](double median, const ImuConfiguration &configuration) {
        try {
            requireStatedRates(median, configuration);
        } catch (const std::invalid_argument &e) {
            return QString::fromUtf8(e.what());
        }
        return QString();
    };
    const double nominal = 1/12.5;
    QCOMPARE(reasonFor(nominal, stated), QString());
    QCOMPARE(reasonFor(nominal*1.099, stated), QString());
    QCOMPARE(reasonFor(nominal*.901, stated), QString());
    QCOMPARE(reasonFor(nominal*1.101, stated),
             QStringLiteral("ACCEL_ODR_HZ states 12.5 Hz but the IMU is logged at 11.4 Hz; sensor fusion unavailable"));
    QCOMPARE(reasonFor(nominal*.899, stated),
             QStringLiteral("ACCEL_ODR_HZ states 12.5 Hz but the IMU is logged at 13.9 Hz; sensor fusion unavailable"));
    // The recordings on disk: 75.6 ms, 13.2 Hz, 5.5 % fast.
    QCOMPARE(reasonFor(.0756, stated), QString());
    ImuConfiguration gyroOnly = stated;
    gyroOnly.gyroOdrHz = 26;
    QCOMPARE(reasonFor(nominal, gyroOnly),
             QStringLiteral("GYRO_ODR_HZ states 26 Hz but the IMU is logged at 12.5 Hz; sensor fusion unavailable"));
    ImuConfiguration both = stated;
    both.accelOdrHz = both.gyroOdrHz = 104;
    QVERIFY(reasonFor(nominal, both).startsWith(QStringLiteral("ACCEL_ODR_HZ states 104 Hz")));

    // Through the pipeline, on the whole recording's median interval.
    Fusion::Channels c = toChannels(fusionFixture(QStringLiteral("coarse_linear")));
    c.imuConfiguration.gyroOdrHz = 52;
    QVERIFY(rejectedWith(rejectedBy(c), QStringLiteral(
        "GYRO_ODR_HZ states 52 Hz but the IMU is logged at 100.0 Hz; sensor fusion unavailable")));
}

void FusionKernelTest::configurationChecksComeAfterTheOthers()
{
    // Decision 13 and clause 9: the configuration's checks come after every
    // existing check, in the order table entry, lattice, rate, so a recording
    // with several defects reports the earliest and every existing rejection
    // keeps its reason.
    const Fusion::Channels linear = toChannels(fusionFixture(QStringLiteral("coarse_linear")));
    const Fusion::Channels gapped = toChannels(fusionFixture(QStringLiteral("reject_imu_gap")));
    const QString gapReason = rejectedBy(gapped).reason;
    QVERIFY(gapReason.startsWith(QStringLiteral("IMU gap at ")));

    // An IMU gap and a configuration without an entry: the gap.
    Fusion::Channels c = gapped;
    c.imuConfiguration = ImuConfiguration{};
    QVERIFY(rejectedWith(rejectedBy(c), gapReason));
    // A non-finite reading, off every lattice, and a stated rate the logging
    // disagrees with: the non-finite input.
    c = linear;
    c.ax[10] = std::numeric_limits<double>::quiet_NaN();
    c.imuConfiguration.accelOdrHz = 12.5;
    QVERIFY(rejectedWith(rejectedBy(c), QStringLiteral("Nonfinite IMU/ax")));
    // No entry, readings off the lattice and a wrong rate: no entry.
    c = linear;
    c.ax[10] = 1e-3;
    c.imuConfiguration.accelOdrHz = 1.6;
    QVERIFY(rejectedBy(c).reason.startsWith(QStringLiteral("No datasheet entry for ACCEL_ODR_HZ")));
    // Readings off the lattice and a wrong rate: the lattice.
    c.imuConfiguration.accelOdrHz = 12.5;
    QVERIFY(rejectedBy(c).reason.startsWith(QStringLiteral("ACCEL_FS_G states +/-16 g")));
    // A wrong rate alone: the rate.
    c.ax[10] = 0;
    QVERIFY(rejectedBy(c).reason.startsWith(QStringLiteral("ACCEL_ODR_HZ states 12.5 Hz")));
}

void FusionKernelTest::diagnosticsReportTheNoiseModel()
{
    // Clause 40 and criterion 9: a successful fit's diagnostics carry the
    // configuration it ran under and model.noise, with exactly these keys
    // and the values of imuNoise() for the fixture's configuration (the
    // diagnostics' JSON round-trips a double exactly); model holds the noise,
    // the gyro bias model and the scale factors; stopping reports the damping
    // ceiling.
    const FusionFixture fixture = fusionFixture(QStringLiteral("coarse_linear"));
    const Fusion::Result result = runPipeline(toChannels(fixture), Tuning{}, Checkpoint());
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
    const QJsonObject diagnostics = diagnosticsOf(result);

    const QJsonObject configuration = diagnostics.value("configuration").toObject();
    QCOMPARE(configuration.keys(), QStringList({"accel_fs_g", "accel_odr_hz", "gyro_fs_deg_s", "gyro_odr_hz"}));
    QCOMPARE(configuration.value("accel_fs_g").toDouble(), fixture.accelFsG);
    QCOMPARE(configuration.value("gyro_fs_deg_s").toDouble(), fixture.gyroFsDegS);
    QCOMPARE(configuration.value("accel_odr_hz").toDouble(), fixture.accelOdrHz);
    QCOMPARE(configuration.value("gyro_odr_hz").toDouble(), fixture.gyroOdrHz);

    const QJsonObject model = diagnostics.value("model").toObject();
    QCOMPARE(model.keys(), QStringList({QStringLiteral("gyro_bias"), QStringLiteral("noise"), QStringLiteral("scale")}));
    // A constant 25 degC series (kFixtureTemperatureDegC) has exactly that mean.
    QCOMPARE(model.value("gyro_bias").toObject().value("t_ref_degc").toDouble(), kFixtureTemperatureDegC);
    const QJsonObject noise = model.value("noise").toObject();
    QCOMPARE(noise.keys(), QStringList({QStringLiteral("acc"), QStringLiteral("gyro")}));
    const ImuNoise expected = imuNoise(toChannels(fixture).imuConfiguration);
    const QJsonObject acc = noise.value("acc").toObject(), gyro = noise.value("gyro").toObject();
    QCOMPARE(acc.keys(), QStringList({"bandwidth_hz", "datasheet_density_m_s2_rthz", "density_m_s2_rthz",
                                      "sample_sigma_m_s2", "step_m_s2"}));
    QCOMPARE(gyro.keys(), QStringList({"bandwidth_hz", "datasheet_density_rad_s_rthz", "density_rad_s_rthz",
                                       "sample_sigma_rad_s", "step_rad_s"}));
    const SensorNoise &a = expected.accelerometer, &w = expected.gyroscope;
    QVERIFY(acc.value("datasheet_density_m_s2_rthz").toDouble() == a.datasheetDensity);
    QVERIFY(acc.value("bandwidth_hz").toDouble() == a.bandwidth);
    QVERIFY(acc.value("step_m_s2").toDouble() == a.step);
    QVERIFY(acc.value("sample_sigma_m_s2").toDouble() == a.sampleSigma);
    QVERIFY(acc.value("density_m_s2_rthz").toDouble() == a.density);
    QVERIFY(gyro.value("datasheet_density_rad_s_rthz").toDouble() == w.datasheetDensity);
    QVERIFY(gyro.value("bandwidth_hz").toDouble() == w.bandwidth);
    QVERIFY(gyro.value("step_rad_s").toDouble() == w.step);
    QVERIFY(gyro.value("sample_sigma_rad_s").toDouble() == w.sampleSigma);
    QVERIFY(gyro.value("density_rad_s_rthz").toDouble() == w.density);

    QCOMPARE(diagnostics.value("stopping").toObject().value("lambda_upper_bound").toDouble(), 1e12);
    QCOMPARE(Tuning{}.lambdaUpperBound, 1e12);
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

    // The whole recording needs three IMU samples, not two: the IMU gap rule
    // reads the nominal interval of the whole IMU axis, which two samples do
    // not have
    const auto recordingDefect = [](const Samples &recording) {
        try {
            requireUsableRecording(recording, 0., 0.);
        } catch (const std::invalid_argument &e) {
            return QString::fromUtf8(e.what());
        }
        return QString();
    };
    Samples few;
    few.imuTime = {0., .01, .02};
    few.force = Vectors(3, Vector3(-kTestGravity));
    few.gyro = Vectors(3, Vector3::Zero());
    few.gnssTime = {0., .01, .02};
    few.position = Vectors(3, Vector3::Zero());
    few.velocity = few.position;
    few.positionSigma = Vectors(3, Vector3::Ones());
    few.velocitySigma = few.positionSigma;
    QCOMPARE(recordingDefect(few), QString());
    few.imuTime.pop_back();
    few.force.pop_back();
    few.gyro.pop_back();
    QCOMPARE(recordingDefect(few),
             QStringLiteral("Sensor fusion needs at least three GNSS fixes and three IMU samples"));

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

    // The damping ceiling must be finite and positive.
    t = Tuning{};
    t.lambdaUpperBound = 0;
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(d, t));
    t = Tuning{};
    t.lambdaUpperBound = std::numeric_limits<double>::infinity();
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, validateSamples(d, t));

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
    const double maxGap = SampleContinuity::holeThreshold(d.imuTime);
    const Rot3 forward = propagateAttitude(d, r, .037, .863, bg, maxGap);
    const Rot3 backward = propagateAttitude(d, forward, .863, .037, bg, maxGap);
    QVERIFY(Rot3::Logmap(forward.between(r)).norm() > 1e-3);
    QVERIFY(Rot3::Logmap(backward.between(r)).norm() < 1e-12);
}

// The attitude propagation judges IMU intervals against the threshold its
// caller gives it (the tuning's maxGap), not one of its own over the samples
// it is handed: across an interval above that threshold it refuses, and with
// the threshold of the axis it carries the attitude.
void FusionKernelTest::propagationRefusesIntervalAboveItsThreshold()
{
    Samples d = boundarySamples(Vector3(1, -2, .5));
    // IMU samples 40..49 missing: an .11 s interval inside [.037, .863]
    d.imuTime.erase(d.imuTime.begin()+40, d.imuTime.begin()+50);
    d.force.erase(d.force.begin()+40, d.force.begin()+50);
    d.gyro.erase(d.gyro.begin()+40, d.gyro.begin()+50);
    const Rot3 r = Rot3::RzRyRx(.3, -.2, .1);
    const double threshold = SampleContinuity::holeThreshold(d.imuTime);
    QCOMPARE(threshold, .015);

    try {
        propagateAttitude(d, r, .037, .863, Vector3::Zero(), threshold);
        QFAIL("propagateAttitude() carried an attitude across an interval above its threshold");
    } catch (const std::invalid_argument &e) {
        QCOMPARE(QString::fromLatin1(e.what()), QStringLiteral("Anchor propagation cannot bridge an IMU gap"));
    }
    // The same span under a threshold above the interval, and a span that
    // ends before the hole under the axis's own threshold: carried
    propagateAttitude(d, r, .037, .863, Vector3::Zero(), .12);
    propagateAttitude(d, r, .037, .35, Vector3::Zero(), threshold);
}

// planFit() sets maxGap to the continuity rule's threshold over the whole
// recording's IMU axis, 1.5 nominal intervals: one IMU interval of 1.55
// nominal intervals inside the GNSS span is rejected as a gap, one of 1.45
// is not (the old 1.6 factor accepted both). The seam is the rejection
// reason of runPipeline().
void FusionKernelTest::imuGapRuleIsTheContinuityThreshold()
{
    for (const auto &[stretch, rejected] : { std::pair<double, bool>{1.55, true},
                                            std::pair<double, bool>{1.45, false} }) {
        FusionFixture f = fusionFixture(QStringLiteral("coarse_linear"));
        QVERIFY(!f.name.isEmpty());
        // Samples 40 on move later: the interval between samples 39 and 40
        // (.39 .. .40 s, inside the GNSS span) becomes `stretch` times .01 s
        const double shift = (stretch-1)*.01;
        for (qsizetype i = 40; i < f.imuTime.size(); ++i)
            f.imuTime[i] += shift;
        const Fusion::Result result = rejectedBy(toChannels(f));
        const bool gap = result.outcome == Fusion::Outcome::Rejected
                         && result.reason.startsWith(QStringLiteral("IMU gap at "));
        QVERIFY2(gap == rejected, qPrintable(QStringLiteral("stretch %1: %2").arg(stretch).arg(result.reason)));
    }
}

void FusionKernelTest::headingIsUnconstrained()
{
    Samples d = boundarySamples(Vector3::Zero());
    const gtsam::imuBias::ConstantBias bias;
    const auto graph = buildFactorGraph(d, bias, tuningAt(104));

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

    // Two states at rest that differ by a yaw of .2 rad which the (zero) gyro
    // does not explain. The force is vertical and does not change, so every
    // step carries the density's noise alone, uniform in time, and a yaw
    // about the force axis does not couple into velocity or position: the
    // pass shares the yaw in proportion to elapsed time and nothing else, so
    // the attitude is Rz(.2 (t - t0) / T) at every sample, the velocity and
    // position stay zero, the step corrections are zero and so is the
    // published acceleration, all to rounding. The tolerances are a hundred
    // times or more what the pass measures: attitude 5.6e-17 rad against
    // Rz, velocity and position 3.5e-15 (the forward integration's gravity
    // and force cancel to rounding), acceleration 4.2e-14 m/s^2, velocity
    // mismatch 7.1e-15 m/s, step correction 8.0e-14 m/s^2.
    FitResult endpoints;
    endpoints.values.insert(B(0), gtsam::imuBias::ConstantBias());
    endpoints.values.insert(X(0), gtsam::Pose3());
    endpoints.values.insert(X(1), gtsam::Pose3(Rot3::Rz(.2), Vector3::Zero()));
    endpoints.values.insert(V(0), Vector3(0, 0, 0));
    endpoints.values.insert(V(1), Vector3(0, 0, 0));

    const ImuRateTrajectory out = reconstructAtImuRate(d, endpoints, tuningAt(104));
    QCOMPARE(out.time.size(), size_t(83));
    QCOMPARE(out.time.front(), .04);
    QCOMPARE(out.time.back(), .86);
    double attitude = 0, state = 0, acceleration = 0;
    for (size_t i = 0; i < out.time.size(); ++i) {
        const Rot3 expected = Rot3::Rz(.2*(out.time[i]-.037)/duration);
        attitude = std::max(attitude, angleBetween(out.rotation[i], expected));
        state = std::max({state, out.velocity[i].norm(), out.position[i].norm()});
        acceleration = std::max(acceleration, out.acceleration[i].norm());
    }
    qInfo() << "yaw share: attitude against Rz(.2 (t - t0) / T)" << attitude << "rad, velocity and position" << state
            << ", acceleration" << acceleration << "m/s^2; endpoint correction" << out.maxEndpointCorrectionDeg
            << "deg (" << out.maxEndpointCorrectionDeg-.2*180/kPi << "from .2 rad), velocity mismatch" << out.maxVelocityMismatch << "m/s, step correction" << out.maxStepCorrection
            << "m/s^2";
    QVERIFY(attitude < 1e-14);
    QVERIFY(state < 1e-12);
    QVERIFY(acceleration < 1e-11);
    QVERIFY(std::abs(out.maxEndpointCorrectionDeg-.2*180/kPi) < 1e-12);
    QVERIFY(out.maxVelocityMismatch < 1e-12);
    QVERIFY(out.maxStepCorrection < 1e-11);
}

void FusionKernelTest::shortWindowIsOneSegment()
{
    // 2 s of exact constant velocity: one segment, whose first prefix window
    // (60 s centred on the first fix, every sAcc being .1 so the earliest
    // wins) covers it, so one length is tried with four starts and the
    // segment fit runs. Exact constant velocity has no yaw information: the
    // marginal yaw sigma is the cap.
    const Samples linear = linearSamples(Vector3(12, -4, 2), Vector3(7, 8, 9));
    const Initialization init = initialize(linear, tuningAt(104));
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
    const auto stillGraph = buildFactorGraph(still, bias, tuningAt(104));
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
    const auto movingGraph = buildFactorGraph(moving, bias, tuningAt(104));
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
    QCOMPARE(yawSigmaDeg(buildFactorGraph(oneInterval, bias, tuningAt(104)), oneValues, X(0)), 180.);
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
        "accuracy", "algorithm", "anchor_time_s", "configuration", "dense_output", "end_s", "gnss_states", "imu_outputs",
        "initialization", "initializer", "input", "limitations", "max_endpoint_correction_deg",
        "max_seed_vs_selected_acceleration_m_s2", "max_seed_vs_selected_angle_deg", "max_step_correction_m_s2",
        "max_step_correction_time_s", "max_velocity_mismatch_m_s", "model", "objective",
        "orientation", "quality", "residuals", "scale_release", "seed_comparison_performed", "seeds",
        "selected_heading_deg", "start_s", "stationary_interval_s", "stopping"}));
    // The account of the reconstruction names it; the limitations no longer
    // disclaim it.
    QVERIFY(diagnostics.value("dense_output").toString().startsWith(QStringLiteral("IMU-rate reconstruction")));
    QVERIFY(diagnostics.value("limitations").toString().startsWith(
        QStringLiteral("Local batch convergence; heading may be ambiguous.")));
    QVERIFY(!diagnostics.value("limitations").toString().contains(QStringLiteral("smoothing posterior")));
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
    const Tuning tuning = tuningAt(104);
    const Initialization init = initialize(linear, tuning);

    // The yaw is arbitrary and unasserted; the position, velocity and
    // acceleration checks hold for any yaw.
    const FitResult fitted = fitFactorGraph(linear, init.state, tuning);
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

    // The published samples are the IMU-rate reconstruction of the fit with
    // the tuning it ran with: on exact data the exact trajectory.
    const ImuRateTrajectory output = reconstructAtImuRate(linear, fitted, tuning);
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
    for (const char *name : {"motion_start", "rest_throughout", "sacc_anchor", "drifting_bias", "scale_recording",
                             "long_hole"}) {
        const FusionFixture a = initializerFixture(QLatin1String(name));
        const FusionFixture b = initializerFixture(QLatin1String(name));
        QCOMPARE(a.name, QLatin1String(name));
        QCOMPARE(b.name, a.name);
        QCOMPARE(a.originIndex, b.originIndex);
        QVERIFY(a.expectSuccess);
        // Every recording states its configuration: +/-16 g, +/-2000 deg/s and
        // a listed rate within 4 % of its sampling (12.5 Hz for the two that
        // log at 12.5 Hz, 26 Hz for the three at 25 Hz, 104 Hz for long_hole
        // at 100 Hz).
        QCOMPARE(a.accelFsG, 16.);
        QCOMPARE(a.gyroFsDegS, 2000.);
        QCOMPARE(a.accelOdrHz, b.accelOdrHz);
        QCOMPARE(a.gyroOdrHz, a.accelOdrHz);
        const bool slow = a.name == QStringLiteral("sacc_anchor") || a.name == QStringLiteral("drifting_bias");
        const bool fast = a.name == QStringLiteral("long_hole");
        QCOMPARE(a.accelOdrHz, slow ? 12.5 : fast ? 104. : 26.);
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
    QCOMPARE(fusionFixtures().size(), 14);
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
    // under the cost test the fit converges within two passes (in one, of four
    // iterations, under the datasheet's noise). Each stage of the full fit
    // does: the held stage, then the released stage, which is kept and whose
    // account `stopping` is (item 1406).
    PipelineTrace trace;
    const Fusion::Result result = runPipeline(
        toChannels(fusionFixture(QStringLiteral("coarse_maneuver"))), Tuning{}, Checkpoint(), &trace);
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
    QVERIFY(trace.converged);
    QVERIFY(trace.stopping.rule == StopRule::kSettled);
    const QJsonObject diagnostics = diagnosticsOf(result);
    const QJsonObject release = diagnostics.value("scale_release").toObject();
    const QJsonObject held = release.value("held").toObject(), released = release.value("released").toObject();
    qInfo() << "coarse_maneuver converged after" << held.value("passes").toInt() << "held and"
            << trace.stopping.passes << "released passes";
    QCOMPARE(release.value("kept").toBool(false), true);
    QCOMPARE(held.value("rule").toString(), QStringLiteral("settled"));
    QVERIFY(held.value("passes").toInt(99) <= 2);
    QVERIFY(trace.stopping.passes <= 2);
    QCOMPARE(released.value("passes").toInt(), trace.stopping.passes);

    // The pass index counts on across the stages.
    std::set<int> outers;
    for (const FitIteration &h : trace.history)
        outers.insert(h.outer);
    QCOMPARE(int(outers.size()), held.value("passes").toInt()+released.value("passes").toInt());

    // The cost test re-derived from the trace: the reported objective is the
    // cost of the graph rebuilt at the fitted bias, the last history row's
    // `after` is the pass's final cost.
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
    QCOMPARE(stopping.value("divergence_max_imu_nrms").toDouble(), 10.);
    QCOMPARE(stopping.value("divergence_scale_range").toArray(), QJsonArray({.5, 2.}));
    const QJsonObject seed = diagnostics.value("seeds").toArray().first().toObject();
    QCOMPARE(seed.value("converged").toBool(false), true);
    QCOMPARE(seed.value("iterations").toInt(), int(trace.history.size()));
    QCOMPARE(diagnostics.value("algorithm").toString(), QStringLiteral("batch-temperature-bias-v9"));

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
    QTest::addColumn<bool>("imuIgnored");
    QTest::addColumn<bool>("accepted");
    QTest::newRow("accepted") << 2. << 1e-4 << false << true;
    QTest::newRow("nrms bound fails") << 0. << 1e-4 << false << false;
    QTest::newRow("decrease bound fails") << 2. << 0. << false << false;
    QTest::newRow("imu nrms bound fails") << 2. << 1e-4 << true << false;
}

namespace {

/// coarse_linear made into a fit that satisfies its fixes while it ignores
/// the IMU: every GNSS sigma divided by 1e5, so that the exact fixes hold the
/// states, and a square wave of +/-8 m/s^2 (on the lattice of the stated
/// range) added to the forward specific force, its sign alternating every ten
/// samples, which no state held by the fixes can follow.
Fusion::Channels imuIgnoredChannels()
{
    Fusion::Channels channels = toChannels(fusionFixture(QStringLiteral("coarse_linear")));
    const double step = 16./32768*9.80665;
    for (qsizetype i = 0; i < channels.ax.size(); ++i)
        channels.ax[i] += ((i/10)%2 ? -1 : 1)*std::round(8/step)*step;
    for (QVector<double> *sigmas : {&channels.hAcc, &channels.vAcc, &channels.sAcc}) {
        for (double &sigma : *sigmas)
            sigma /= 1e5;
    }
    return channels;
}

} // namespace

void FusionKernelTest::slowTailAtTheIterationLimit()
{
    // The spec's slow-tail test. A negative relative tolerance means no pass
    // ever settles (before - after >= -1e-6 by the cost-increase guard, so it
    // is never <= -max(1, before)), so every pass runs its 25 iterations: the
    // first four do the work and the rest are steps of order 1e-15 or exact
    // no-ops (GTSAM's LM leaves the values untouched when it rejects a step).
    // The last 20 iterations of pass five therefore have a mean relative
    // decrease of about 0, and the fit's position, velocity and IMU normalized
    // RMS are about 0.098, 0.19 and 0.0002: accepted with the production
    // bounds, refused with either bound at zero (the comparisons are strict).
    // Item 1313 (and 244 as amended): the bound holds the IMU normalized RMS
    // too. On imuIgnoredChannels() the same forced tail ends with the position
    // and velocity normalized RMS about 0.013 and 0.0005, far below the bound,
    // and the IMU's about 2.5, above it (the step model's sampling term grows
    // with the wave, so the IMU's normalized misfit is about that whatever the
    // wave's amplitude): refused on the IMU bound alone. Dividing the IMU's
    // noise instead does not raise it: the stiffer factor is satisfied and the
    // misfit moves to the fixes. All of this is the held stage's (item 1406):
    // accepted, it is followed by the released stage, its budget of three
    // passes of 25 iterations under the same forcing, whose tail is accepted
    // the same way, so the fit has 200 iterations and reports the released
    // stage's three passes; refused, the held stage ends the fit at 125.
    QFETCH(double, maxNrms);
    QFETCH(double, maxMeanRelativeDecrease);
    QFETCH(bool, imuIgnored);
    QFETCH(bool, accepted);

    Tuning tuning;
    tuning.relativeTolerance = -1;
    tuning.maxIterations = 25;
    tuning.slowTailMaxNrms = maxNrms;
    tuning.slowTailMaxMeanRelativeDecrease = maxMeanRelativeDecrease;
    PipelineTrace trace;
    const Fusion::Result result = runPipeline(
        imuIgnored ? imuIgnoredChannels() : toChannels(fusionFixture(QStringLiteral("coarse_maneuver"))), tuning,
        Checkpoint(), &trace);

    QCOMPARE(trace.history.size(), size_t(accepted ? 200 : 125));
    for (const FitIteration &h : trace.history)
        QVERIFY2(h.before >= h.after, qPrintable(QStringLiteral("pass %1, iteration %2").arg(h.outer).arg(h.iteration)));
    QCOMPARE(trace.stopping.passes, accepted ? 3 : 5);
    const QJsonObject diagnostics = diagnosticsOf(result);
    const QJsonObject stopping = diagnostics.value("stopping").toObject();
    const QJsonObject quality = diagnostics.value("quality").toObject();
    QCOMPARE(stopping.value("passes").toInt(), accepted ? 3 : 5);
    const double meanDecrease = stopping.value("last_pass_mean_relative_decrease").toDouble(-1);
    qInfo() << "slow tail: position nrms" << quality.value("position_nrms").toDouble() << ", velocity nrms"
            << quality.value("velocity_nrms").toDouble() << ", imu nrms" << quality.value("imu_nrms").toDouble()
            << "; mean relative decrease" << meanDecrease << "; rule" << stopping.value("rule").toString();

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
        QVERIFY(quality.value("imu_nrms").toDouble(9) < 2);
        const QJsonObject seed = diagnostics.value("seeds").toArray().first().toObject();
        QCOMPARE(seed.value("converged").toBool(false), true);
        QCOMPARE(seed.value("iterations").toInt(), 200);
        const QJsonObject release = diagnostics.value("scale_release").toObject();
        const QJsonObject held = release.value("held").toObject(), released = release.value("released").toObject();
        QCOMPARE(held.value("rule").toString(), QStringLiteral("slow tail accepted"));
        QCOMPARE(held.value("passes").toInt(), 5);
        QCOMPARE(held.value("iterations").toInt(), 125);
        QCOMPARE(released.value("rule").toString(), QStringLiteral("slow tail accepted"));
        QCOMPARE(released.value("passes").toInt(), 3);
        QCOMPARE(released.value("iterations").toInt(), 75);
        QCOMPARE(release.value("kept").toBool(false), true);
        QVERIFY(release.value("reason").isNull());
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
        if (imuIgnored) {
            // Every other bound holds: the IMU's alone refuses the tail.
            QVERIFY(meanDecrease < maxMeanRelativeDecrease);
            QVERIFY(quality.value("position_nrms").toDouble(9) < maxNrms);
            QVERIFY(quality.value("velocity_nrms").toDouble(9) < maxNrms);
            QVERIFY(quality.value("imu_nrms").toDouble() > maxNrms);
        }
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
    // The shapes of the two failures a pass cannot continue from, `cost
    // increased` and `damping saturated`, proven on the writer directly: LM
    // rejects an increasing step by construction, so the first guard is
    // reachable only through non-finite arithmetic, which no deterministic
    // input forces (dampingSaturationIsASolverFailure reaches the second).
    const Tuning tuning;
    const struct { const char *rule, *failure; } failures[] = {
        {StopRule::kCostIncreased, "Nonfinite or increasing optimizer cost"},
        {StopRule::kDampingSaturated, "Optimizer damping saturated without progress"}};
    Stopping s;
    for (const auto &failure : failures) {
        s.rule = failure.rule;
        s.passes = 2;
        s.biasSettledTolerance = tuning.biasSettledTolerance;
        s.lambdaUpperBound = tuning.lambdaUpperBound;
        s.slowTailWindow = tuning.slowTailWindow;
        s.slowTailMaxMeanRelativeDecrease = tuning.slowTailMaxMeanRelativeDecrease;
        s.slowTailMaxNrms = tuning.slowTailMaxNrms;
        s.divergenceMaxImuNrms = tuning.divergenceMaxImuNrms;
        s.divergenceScaleRange = tuning.divergenceScaleRange;
        QVERIFY(std::isnan(s.lastPassMeanRelativeDecrease) && std::isnan(s.repreintegrationCostDifference));

        const QJsonObject diagnostics = failureDiagnostics(QString::fromLatin1(failure.failure), &s);
        QCOMPARE(diagnostics.keys(), QStringList({QStringLiteral("algorithm"), QStringLiteral("failure"),
                                                  QStringLiteral("stopping")}));
        QCOMPARE(diagnostics.value("algorithm").toString(), QStringLiteral("batch-temperature-bias-v9"));
        QCOMPARE(diagnostics.value("failure").toString(), QString::fromLatin1(failure.failure));
        const QJsonObject stopping = diagnostics.value("stopping").toObject();
        QCOMPARE(stopping.value("rule").toString(), QString::fromLatin1(failure.rule));
        QCOMPARE(stopping.value("passes").toInt(), 2);
        QVERIFY(stopping.value("last_pass_mean_relative_decrease").isNull());
        QVERIFY(stopping.value("repreintegration_cost_difference").isNull());
        QCOMPARE(stopping.value("lambda_upper_bound").toDouble(), 1e12);
        QCOMPARE(stopping.value("slow_tail").toObject().value("window").toInt(), 20);
        // The divergence bounds in force, beside the other thresholds.
        QCOMPARE(stopping.value("divergence_max_imu_nrms").toDouble(), 10.);
        QCOMPARE(stopping.value("divergence_scale_range").toArray(), QJsonArray({.5, 2.}));
        QCOMPARE(stopping.keys(), QStringList({"bias_settled_tolerance", "divergence_max_imu_nrms",
                                               "divergence_scale_range", "lambda_upper_bound",
                                               "last_pass_mean_relative_decrease", "passes",
                                               "repreintegration_cost_difference", "rule", "slow_tail"}));
    }
    QCOMPARE(QString::fromLatin1(StopRule::kDampingSaturated), QStringLiteral("damping saturated"));
    s.rule = StopRule::kCostIncreased;

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

void FusionKernelTest::dampingSaturationIsASolverFailure()
{
    // Decision 7 and criterion 14: under GTSAM's default damping ceiling,
    // 1e5, the full fit of rest_throughout from its coarse start (the
    // coarse attitude at the anchor carried with zero bias, the start the
    // initializer falls back to) saturates Levenberg-Marquardt's damping (the
    // IMU blocks of the Hessian are about 1e9 under the datasheet's
    // densities): every iteration returns the same values, and a pass that
    // settled there would call that start converged. It is the solver failure
    // `damping saturated` instead (the linearization still predicts a
    // decrease, there the whole cost, 3.8e10), with the failure diagnostics'
    // shape of a failed pass (no quality: no pass completed). The forcing makes every
    // prefix fit fail (allPrefixFitsFailFallsBack's), so the full fit starts
    // there; through the initializer's own starts the recording converges
    // even under 1e5 (41 iterations: a prefix start that saturated would be
    // a failed start). Under the default ceiling, 1e12, the recording
    // converges.
    Tuning lowCeiling;
    lowCeiling.lambdaUpperBound = 1e5;
    const Checkpoint failingPrefixes(
        [](const QString &text) {
            if (text.contains(QStringLiteral(": prefix ")))
                throw FitFailure(std::string("Nonfinite or increasing optimizer cost"), Stopping{});
        },
        [] { return false; });
    PipelineTrace trace;
    const Fusion::Result result = runPipeline(toChannels(initializerFixture(QStringLiteral("rest_throughout"))),
                                              lowCeiling, failingPrefixes, &trace);
    qInfo() << "rest_throughout from its coarse start under a 1e5 ceiling: outcome" << int(result.outcome)
            << qPrintable(result.reason) << ", in pass" << trace.stopping.passes << "of the full fit";
    QCOMPARE(trace.initializer.segments.size(), size_t(1));
    QVERIFY(trace.initializer.segments.front().fallback);
    QVERIFY(result.outcome == Fusion::Outcome::SolverFailed);
    QCOMPARE(result.reason, QStringLiteral("Optimizer damping saturated without progress"));
    // The full fit's: the initializer's fits catch a failure as a failed
    // start, and with every prefix start failing no segment fit runs.
    QCOMPARE(trace.stopping.rule, std::string(StopRule::kDampingSaturated));
    QVERIFY(!trace.converged);
    QVERIFY(trace.stopping.passes >= 1);
    QCOMPARE(trace.initializer.segments.front().iterations, 0);
    const QJsonObject diagnostics = diagnosticsOf(result);
    QCOMPARE(diagnostics.keys(), QStringList({QStringLiteral("algorithm"), QStringLiteral("failure"),
                                              QStringLiteral("stopping")}));
    QCOMPARE(diagnostics.value("failure").toString(), result.reason);
    const QJsonObject stopping = diagnostics.value("stopping").toObject();
    QCOMPARE(stopping.value("rule").toString(), QStringLiteral("damping saturated"));
    QCOMPARE(stopping.value("lambda_upper_bound").toDouble(), 1e5);
    QCOMPARE(stopping.value("passes").toInt(), trace.stopping.passes);
    QVERIFY(allChannelsEmpty(result));

    const InitializerRun &run = restThroughoutRun();
    QVERIFY2(run.result.outcome == Fusion::Outcome::Succeeded, qPrintable(run.result.reason));
    QCOMPARE(run.trace.stopping.rule, std::string(StopRule::kSettled));
    QCOMPARE(run.diagnostics.value("stopping").toObject().value("lambda_upper_bound").toDouble(), 1e12);
}

void FusionKernelTest::dampingCeilingChangesNothingBelowIt()
{
    // Criterion 14: a fit whose damping never reaches the ceiling does not
    // depend on it. The four success fixtures, through the whole pipeline
    // (coarse_maneuver's four prefix fits, its segment fit and the full fit
    // among them), are the same bits under a 1e5 and the default 1e12
    // ceiling: every channel, every iteration of the trace and the
    // initializer's account; the diagnostics differ in the ceiling they
    // report and nowhere else.
    Tuning lowCeiling;
    lowCeiling.lambdaUpperBound = 1e5;
    for (const char *name : {"coarse_linear", "coarse_maneuver", "stationary_spin", "bridged_hole"}) {
        const Fusion::Channels channels = toChannels(fusionFixture(QLatin1String(name)));
        PipelineTrace low, high;
        const Fusion::Result a = runPipeline(channels, lowCeiling, Checkpoint(), &low);
        const Fusion::Result b = runPipeline(channels, Tuning{}, Checkpoint(), &high);
        QVERIFY2(a.outcome == Fusion::Outcome::Succeeded && b.outcome == Fusion::Outcome::Succeeded, name);
        for (const QString &channel : fusionChannelNames())
            QVERIFY2(sameBitsEverywhere(fusionChannel(a, channel), fusionChannel(b, channel)), qPrintable(channel));
        QCOMPARE(low.history.size(), high.history.size());
        for (size_t i = 0; i < low.history.size(); ++i) {
            QVERIFY(low.history[i].before == high.history[i].before);
            QVERIFY(low.history[i].after == high.history[i].after);
        }
        QVERIFY2(traceJson(low) == traceJson(high), name);
        QJsonObject diagnosticsA = diagnosticsOf(a), diagnosticsB = diagnosticsOf(b);
        QCOMPARE(diagnosticsA.value("stopping").toObject().value("lambda_upper_bound").toDouble(), 1e5);
        QCOMPARE(diagnosticsB.value("stopping").toObject().value("lambda_upper_bound").toDouble(), 1e12);
        for (QJsonObject *d : {&diagnosticsA, &diagnosticsB}) {
            QJsonObject stopping = d->value("stopping").toObject();
            stopping.remove("lambda_upper_bound");
            d->insert("stopping", stopping);
        }
        QVERIFY2(diagnosticsA == diagnosticsB, name);
    }
}

void FusionKernelTest::saturationAtAMinimumIsSettled()
{
    // Decision 7, as amended: a pass that stalls at the damping ceiling has
    // settled when the linearization predicts no decrease, and is the solver
    // failure only while it still does. At a minimum the library judges each
    // trial step by the sign of a rounding-level linearized change, and a run
    // of negative signs raises the damping to the ceiling without the cost
    // ever being evaluated (motion_start's full fit on Intel macOS,
    // 2026-10-02). The exact constant-velocity recording from its exact start
    // (the identity attitude, zero gyro bias) is at its minimum, and a ceiling
    // at the library's initial damping, 1e-5, puts the first iteration at the
    // ceiling whatever that sign: the pass settles with one iteration.
    const Vector3 speed(12, -4, 2), offset(7, 8, 9);
    const Samples linear = linearSamples(speed, offset);
    Tuning atTheCeiling = tuningAt(104);
    atTheCeiling.lambdaUpperBound = 1e-5;
    InitialState exact;
    exact.rotations.assign(linear.gnssTime.size(), Rot3());
    const FitResult fitted = fitFactorGraph(linear, exact, atTheCeiling);
    QVERIFY(fitted.converged);
    QCOMPARE(fitted.stopping.rule, std::string(StopRule::kSettled));
    QCOMPARE(fitted.stopping.passes, 1);
    QCOMPARE(fitted.history.size(), size_t(1));
    QVERIFY(fitted.history.front().after <= fitted.history.front().before);
    QVERIFY(fitted.objective < 1e-12);
    QCOMPARE(fitted.stopping.lambdaUpperBound, 1e-5);

    // The measure the rule reads, on a hand-built graph: a prior off its mean
    // predicts the whole cost (one Gauss-Newton step reaches the mean), a
    // prior at its mean predicts nothing.
    gtsam::NonlinearFactorGraph graph;
    graph.emplace_shared<gtsam::PriorFactor<Vector3>>(V(0), Vector3(1, 2, 3),
                                                      gtsam::noiseModel::Isotropic::Sigma(3, .5));
    gtsam::Values off, at;
    off.insert(V(0), Vector3(1.5, 2, 2));
    at.insert(V(0), Vector3(1, 2, 3));
    const double cost = graph.error(off);
    QVERIFY(cost > 1);
    QVERIFY(withinRelative(predictedDecrease(*graph.linearize(off)), cost, 1e-12));
    QCOMPARE(predictedDecrease(*graph.linearize(at)), 0.);
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
    const InitializerRun &run = restThroughoutRun();
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
    const Rot3 expected = propagateAttitude(window, s.prefixRotation, s.prefixStart, s.start, s.prefixGyroBias,
                                            pipelineTuning(QStringLiteral("sacc_anchor"), Tuning{}).maxGap);
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
    // model: b1 within 20 % of the truth in at most 30 iterations (item 241:
    // the full fit's, the sum over all its passes, both stages counted, since
    // the item bounds what the fit costs, not one stage of it; the split
    // between the held and the released stage is logged). From the
    // fixture's construction: the z
    // bias .3 + t / 200 deg/s over the ramp 25 + t / 10 degC is 0.05 deg/s
    // per degC (b1z), T_ref = 35, and b0z = .8 deg/s, the bias at T_ref.
    QVERIFY(run.trace.converged);
    QCOMPARE(seed.value("iterations").toInt(999), int(run.trace.history.size()));
    QVERIFY(seed.value("iterations").toInt(999) <= 30);
    const QJsonObject release = run.diagnostics.value("scale_release").toObject();
    const QJsonObject gyroBias = run.diagnostics.value("model").toObject().value("gyro_bias").toObject();
    const QJsonArray b1 = gyroBias.value("b1_rad_s_per_degc").toArray();
    QCOMPARE(b1.size(), 3);
    const double b1z = .05*kPi/180;
    qInfo() << "drifting_bias: b1" << b1.at(0).toDouble()*180/kPi << b1.at(1).toDouble()*180/kPi
            << b1.at(2).toDouble()*180/kPi << "deg/s/degC (truth 0, 0, .05); b0"
            << bias.at(0).toDouble()*180/kPi << bias.at(1).toDouble()*180/kPi << bias.at(2).toDouble()*180/kPi
            << "deg/s (truth .2, -.15, .8); t_ref" << gyroBias.value("t_ref_degc").toDouble()
            << "degC; full fit" << run.trace.history.size() << "iterations ("
            << release.value("held").toObject().value("iterations").toInt() << "held,"
            << release.value("released").toObject().value("iterations").toInt() << "released, kept"
            << release.value("kept").toBool() << ")";
    QVERIFY(std::abs(b1.at(2).toDouble()-b1z) <= .2*b1z);
    QVERIFY(std::abs(b1.at(0).toDouble()) <= .2*b1z);
    QVERIFY(std::abs(b1.at(1).toDouble()) <= .2*b1z);
    QVERIFY(std::abs(gyroBias.value("t_ref_degc").toDouble(-1)-35) < 1e-9);
    const QJsonArray b0 = gyroBias.value("b0_rad_s").toArray();
    QCOMPARE(b0.size(), 3);
    for (int i = 0; i < 3; ++i)
        QVERIFY(b0.at(i).toDouble() == bias.at(i).toDouble());
    QVERIFY(std::abs(b0.at(2).toDouble()-.8*kPi/180) < .1*kPi/180);

    // The residuals: the three priors last, in order, and one IMU factor per
    // interval (201 states).
    const QJsonArray residuals = run.diagnostics.value("residuals").toArray();
    QVERIFY(residuals.size() >= 3);
    QCOMPARE(residuals.last().toObject().value("kind").toString(), QStringLiteral("scale_prior"));
    QCOMPARE(residuals.at(residuals.size()-2).toObject().value("kind").toString(), QStringLiteral("slope_prior"));
    QCOMPARE(residuals.at(residuals.size()-3).toObject().value("kind").toString(), QStringLiteral("bias_prior"));
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
    const Rot3 expected = propagateAttitude(window, s.prefixRotation, s.prefixStart, s.start, s.prefixGyroBias,
                                            pipelineTuning(name, forced).maxGap);
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
    // the scaled IMU factor between them (the temperature model is inside
    // it), the bias prior, the slope prior, and the scale prior last.
    const auto graph = buildFactorGraph(d, BiasLinearization{ConstantBias(), Vector3::Zero()}, model, tuningAt(104));
    QCOMPARE(graph.size(), size_t(8));
    QVERIFY(dynamic_cast<const gtsam::GPSFactor *>(graph.at(0).get()));
    QVERIFY(dynamic_cast<const gtsam::PriorFactor<Vector3> *>(graph.at(1).get()));
    QVERIFY(dynamic_cast<const gtsam::GPSFactor *>(graph.at(2).get()));
    QVERIFY(dynamic_cast<const gtsam::PriorFactor<Vector3> *>(graph.at(3).get()));
    const auto *imu = dynamic_cast<const ScaledImuFactor *>(graph.at(4).get());
    QVERIFY(imu);
    QVERIFY(imu->temperatureDelta() == temperatureAtFix(d, 0)-model.tRef);
    QVERIFY(imu->keys() == gtsam::KeyVector({X(0), V(0), X(1), V(1), B(0), T(0), S(0)}));
    QVERIFY(dynamic_cast<const gtsam::PriorFactor<ConstantBias> *>(graph.at(5).get()));
    const auto *slopePrior = dynamic_cast<const gtsam::PriorFactor<Vector3> *>(graph.at(6).get());
    QVERIFY(slopePrior);
    QVERIFY(slopePrior->keys() == gtsam::KeyVector({T(0)}));
    QVERIFY(slopePrior->prior().isZero(0));
    const auto sigmas = std::dynamic_pointer_cast<gtsam::noiseModel::Diagonal>(slopePrior->noiseModel());
    QVERIFY(sigmas != nullptr);
    QVERIFY(sigmas->sigmas() == gtsam::Vector(Vector3::Constant(Tuning{}.gyroBiasSlopeSigma)));
    QVERIFY(dynamic_cast<const gtsam::PriorFactor<Vector6> *>(graph.at(7).get()));
    // Spec section 6's priors, as literals: the accelerometer bias keeps
    // 0.3 m/s^2, b0 keeps 0.03 rad/s, b1 is 0.010 deg/s per degC (the
    // documented noise model's item 1017).
    QCOMPARE(Tuning{}.accBiasSigma, .3);
    QCOMPARE(Tuning{}.gyroBiasSigma, .03);
    QCOMPARE(Tuning{}.gyroBiasSlopeSigma, .010*kPi/180);

    // The stock four-argument builder is unchanged: six factors, ImuFactor at 4.
    const auto stock = buildFactorGraph(d, ConstantBias(), tuningAt(104));
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
    // forward integration lands on it exactly under the temperature model
    // (the attitude part of the mismatch, maxEndpointCorrectionDeg, is zero),
    // and misses it by |slope dT| x .826 s = .0165 rad = .95 degrees under the
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
    fit.values.insert(X(1), gtsam::Pose3(propagateAttitude(d, Rot3(), .037, .863, intervalGyroBias,
                                                           SampleContinuity::holeThreshold(d.imuTime)),
                                         Vector3::Zero()));
    fit.values.insert(V(0), Vector3(0, 0, 0));
    fit.values.insert(V(1), Vector3(0, 0, 0));
    fit.gyroBiasSlope = slope;
    fit.biasModel = model;

    // One fix interval, so the largest endpoint correction is its own.
    const ImuRateTrajectory temperature = reconstructAtImuRate(d, fit, tuningAt(104));
    qInfo() << "endpoint correction: temperature model" << temperature.maxEndpointCorrectionDeg << "deg";
    QVERIFY(temperature.maxEndpointCorrectionDeg < 1e-9);

    fit.biasModel = GyroBiasModel{};
    fit.gyroBiasSlope = Vector3::Zero();
    const ImuRateTrajectory constant = reconstructAtImuRate(d, fit, tuningAt(104));
    qInfo() << "endpoint correction: constant model" << constant.maxEndpointCorrectionDeg << "deg";
    QVERIFY(constant.maxEndpointCorrectionDeg > .5);
}

void FusionKernelTest::constantTemperatureKeepsSlopeAtPrior()
{
    // The spec's "constant temperature" case (item 246 as amended): with
    // T_k - T_ref exactly zero at every fix the factor's H6 is zero, the
    // slope's normal equation is its prior's alone with a zero right-hand
    // side, and every LM step leaves it at 0.0, with the scale factors, as
    // the pipeline runs it. The bound is 1 % of the prior sigma, the margin
    // against a solver that visits a rounding-size value and steps back.
    Tuning t;
    t.segmentLength = 60;
    t.minFinalSegment = 12;
    FusionFixture f = initializerFixture(QStringLiteral("drifting_bias"));
    const qsizetype samples = f.imuTime.size();
    QCOMPARE(samples, 2501);
    f.imuTemperature = QVector<double>(samples, 35.0);
    PipelineTrace trace;
    const Fusion::Result result = runPipeline(toChannels(f), t, Checkpoint(), &trace);
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
    const QJsonObject diagnostics = diagnosticsOf(result);
    QCOMPARE(diagnostics.value("algorithm").toString(), QStringLiteral("batch-temperature-bias-v9"));
    const QJsonObject gyroBias = diagnostics.value("model").toObject().value("gyro_bias").toObject();
    const QJsonArray b1 = gyroBias.value("b1_rad_s_per_degc").toArray();
    QCOMPARE(b1.size(), 3);
    for (const QJsonValue &component : b1)
        QVERIFY(std::abs(component.toDouble(1)) < .01*Tuning{}.gyroBiasSlopeSigma);
    // 2501 copies of 35: the sum 87535 and the quotient are exact.
    QCOMPARE(gyroBias.value("t_ref_degc").toDouble(), 35.);
    const QJsonArray residuals = diagnostics.value("residuals").toArray();
    const QJsonObject slopePrior = residuals.at(residuals.size()-2).toObject();
    QCOMPARE(slopePrior.value("kind").toString(), QStringLiteral("slope_prior"));
    QVERIFY(slopePrior.value("squared_whitened_error").toDouble(1) < 1e-10);
    const QJsonArray b0 = gyroBias.value("b0_rad_s").toArray();
    const QJsonArray seedBias = diagnostics.value("seeds").toArray().first().toObject().value("gyro_bias_rad_s").toArray();
    QCOMPARE(b0.size(), 3);
    for (int i = 0; i < 3; ++i)
        QVERIFY(b0.at(i).toDouble() == seedBias.at(i).toDouble());

    // The consistency check with the stock path through the internal seams:
    // the same window, the same initializer, the constant-bias fit (the
    // default model) is the same model at b1 = 0 and unit scale, so it
    // converges to the objective of the full fit's held stage, whose scale
    // prior holds every factor at one (its account, scale_release.held, of
    // the pipeline run above), under the settle tolerance (1e-6 relative is
    // the margin for a different elimination ordering). Released, the scale
    // moves the objective by about 4.5 % on this recording (246.3 against
    // 258.0 held), so the constant-bias fit is reproduced only with it held.
    const PreparedInput prepared = prepareInput(toChannels(f));
    Tuning derived = t;
    derived.maxGap = SampleContinuity::holeThreshold(prepared.recording.imuTime);
    derived.noise = imuNoise(toChannels(f).imuConfiguration);
    const Samples window = fittedWindow(prepared.recording, prepared.usableStart, prepared.recording.gnssTime.back());
    validateSamples(window, derived);
    const Initialization init = initialize(window, derived);
    const FitResult stock = fitFactorGraph(window, init.state, derived);
    QVERIFY(stock.converged);
    QVERIFY(!stock.biasModel.temperatureLinear);
    QVERIFY(stock.gyroBiasSlope.isZero(0));
    QCOMPARE(stock.residuals.back().kind, std::string("bias_prior"));
    const QJsonObject held = diagnostics.value("scale_release").toObject().value("held").toObject();
    QCOMPARE(held.value("rule").toString(), QStringLiteral("settled"));
    const double heldObjective = held.value("objective").toDouble(-1);
    const double objective = diagnostics.value("objective").toDouble();
    qInfo() << "constant temperature: objective" << objective << "as reported," << heldObjective
            << "held at one, stock fit" << stock.objective << "; full fit" << trace.history.size() << "iterations";
    QVERIFY(std::abs(stock.objective-heldObjective) <= 1e-6*std::max(1., heldObjective));
}

void FusionKernelTest::preintegrationDividesByTheScale()
{
    // Clause 19 and criterion 1: preintegrating the readings f at the scale s
    // is preintegrating the readings f ./ s at unit scale: the preintegrated
    // vector to 1e-12, the covariance and the last step's two sensor
    // covariances to 1e-9 (relative to their largest entry), so the turn, the
    // sampling term and the remainder are all of the divided readings. A
    // turning, accelerating window, a non-zero bias. And at unit scale (or
    // any other) asking for the scale Jacobian or passing an observer
    // changes no bit of the result, and the transition the observer is
    // handed is the library's update() of the step.
    using gtsam::imuBias::ConstantBias;
    const Samples d = turningSamples();
    const ImuNoise noise = fixtureNoise(104);
    const ConstantBias bias(Vector3(.05, -.03, .08), Vector3(.003, -.002, .004));
    const Vector6 scale = offNominalScale();
    Samples divided = d;
    for (size_t i = 0; i < d.imuTime.size(); ++i) {
        divided.force[i] = d.force[i].cwiseQuotient(scale.head<3>());
        divided.gyro[i] = d.gyro[i].cwiseQuotient(scale.tail<3>());
    }
    const auto atScale = preintegrateImu(d, .037, .863, bias, scale, noise);
    const auto atOne = preintegrateImu(divided, .037, .863, bias, Vector6::Ones(), noise);
    const double delta = relativeDifference(atScale.preintegrated(), atOne.preintegrated());
    const double covariance = relativeDifference(atScale.preintMeasCov(), atOne.preintMeasCov());
    const double accelerometer = relativeDifference(atScale.p().accelerometerCovariance,
                                                    atOne.p().accelerometerCovariance);
    const double gyroscope = relativeDifference(atScale.p().gyroscopeCovariance, atOne.p().gyroscopeCovariance);
    qInfo() << "divided readings: preintegrated" << delta << ", covariance" << covariance
            << ", last step's sensor covariances" << accelerometer << gyroscope << "(relative)";
    QVERIFY(delta <= 1e-12);
    QVERIFY(covariance <= 1e-9);
    QVERIFY(accelerometer <= 1e-9);
    QVERIFY(gyroscope <= 1e-9);
    // Not vacuous: the scale moves the preintegration and its covariance, and
    // the last step's sensor covariances, whose sampling terms and remainders
    // are of the readings the step integrates.
    const auto undivided = preintegrateImu(d, .037, .863, bias, Vector6::Ones(), noise);
    QVERIFY(relativeDifference(undivided.preintegrated(), atScale.preintegrated()) > 1e-3);
    QVERIFY(!(undivided.preintMeasCov() == atScale.preintMeasCov()));
    QVERIFY(!(undivided.p().accelerometerCovariance == atScale.p().accelerometerCovariance));
    QVERIFY(!(undivided.p().gyroscopeCovariance == atScale.p().gyroscopeCovariance));

    for (const Vector6 &at : {Vector6(Vector6::Ones()), scale}) {
        const auto plain = preintegrateImu(d, .037, .863, bias, at, noise);
        gtsam::Matrix96 jacobian;
        const auto withJacobian = preintegrateImu(d, .037, .863, bias, at, noise, ImuStepObserver(), &jacobian);
        int steps = 0;
        bool transitionsAgree = true;
        const auto observed = preintegrateImu(d, .037, .863, bias, at, noise,
            [&](const gtsam::PreintegratedImuMeasurements &pim, const ImuStep &step) {
                gtsam::PreintegratedImuMeasurements copy = pim;
                gtsam::Matrix9 A;
                gtsam::Matrix93 byForce, byRate;
                copy.update(step.force, step.gyro, step.dt, &A, &byForce, &byRate);
                transitionsAgree = transitionsAgree && A == step.transition;
                ++steps;
            });
        QCOMPARE(steps, 84);
        QVERIFY(transitionsAgree);
        for (const gtsam::PreintegratedImuMeasurements *other : {&withJacobian, &observed}) {
            QVERIFY(other->preintegrated() == plain.preintegrated());
            QVERIFY(other->preintMeasCov() == plain.preintMeasCov());
            QVERIFY(other->deltaTij() == plain.deltaTij());
            QVERIFY(other->p().accelerometerCovariance == plain.p().accelerometerCovariance);
            QVERIFY(other->p().gyroscopeCovariance == plain.p().gyroscopeCovariance);
        }
    }
}

void FusionKernelTest::scaleJacobianMatchesCentralDifferences()
{
    // Clause 19 and criterion 2: the scale Jacobian the integration
    // accumulates against central differences of preintegrated() (step 1e-6,
    // all six factors) at a scale away from one, a non-zero bias, on a
    // window turning at about 2 rad/s and accelerating, to 1e-7 of the
    // differences' largest entry. Without the half-step turn's dependence on
    // the gyro scale it would not agree: the test accumulates that Jacobian
    // itself, from the library's own per-step Jacobians the observer is
    // handed, and its gyro columns miss by far more.
    using gtsam::imuBias::ConstantBias;
    const Samples d = turningSamples();
    const ImuNoise noise = fixtureNoise(104);
    const ConstantBias bias(Vector3(.05, -.03, .08), Vector3(.003, -.002, .004));
    const Vector6 at = offNominalScale();
    gtsam::Matrix96 H;
    preintegrateImu(d, .037, .863, bias, at, noise, ImuStepObserver(), &H);

    const double step = 1e-6;
    gtsam::Matrix96 N;
    for (int c = 0; c < 6; ++c) {
        Vector6 plus = at, minus = at;
        plus(c) += step;
        minus(c) -= step;
        N.col(c) = (preintegrateImu(d, .037, .863, bias, plus, noise).preintegrated()
                    - preintegrateImu(d, .037, .863, bias, minus, noise).preintegrated())/(2*step);
    }
    const double largest = N.cwiseAbs().maxCoeff();
    const double worst = (H-N).cwiseAbs().maxCoeff();

    // The same accumulation without the turn term: G = [-B R F | -C W].
    gtsam::Matrix96 withoutTurn = gtsam::Matrix96::Zero();
    preintegrateImu(d, .037, .863, bias, at, noise,
        [&](const gtsam::PreintegratedImuMeasurements &pim, const ImuStep &s) {
            gtsam::PreintegratedImuMeasurements copy = pim;
            gtsam::Matrix9 A;
            gtsam::Matrix93 byForce, byRate;
            copy.update(s.force, s.gyro, s.dt, &A, &byForce, &byRate);
            const Rot3 halfStep = Rot3::Expmap((s.gyro-bias.gyroscope())*(s.dt/2));
            const Vector3 forceMid = halfStep.unrotate(s.force-bias.accelerometer())+bias.accelerometer();
            const gtsam::Matrix3 F = forceMid.cwiseQuotient(at.head<3>()).asDiagonal();
            const gtsam::Matrix3 W = s.gyro.cwiseQuotient(at.tail<3>()).asDiagonal();
            gtsam::Matrix96 input;
            input.leftCols<3>() = -byForce*halfStep.matrix()*F;
            input.rightCols<3>() = -byRate*W;
            withoutTurn = A*withoutTurn+input;
        });
    const double accelerometerColumns = (withoutTurn-H).leftCols<3>().cwiseAbs().maxCoeff();
    const double gyroColumnsWithoutTurn = (withoutTurn-N).rightCols<3>().cwiseAbs().maxCoeff();
    qInfo() << "scale Jacobian against central differences:" << worst/largest << "of the largest entry" << largest
            << "; without the turn term the gyro columns miss by" << gyroColumnsWithoutTurn/largest
            << "; the accelerometer columns of both" << accelerometerColumns/largest << "apart";
    QVERIFY(worst <= 1e-7*largest);
    QVERIFY(gyroColumnsWithoutTurn > 1e-3*largest);
    QVERIFY(accelerometerColumns <= 1e-9*largest);
}

void FusionKernelTest::scaleFactorJacobians()
{
    // Clauses 18 and 48, criterion 3: the scaled factor's seven Jacobians
    // against numerical derivatives (each block with the six other
    // variables bound: numericalDerivative.h stops at six arguments) at a
    // point where every variable differs from the linearization, the scale
    // included; at S = s^ the error and H1..H5 are the stock gtsam::ImuFactor's
    // on the same preintegration at the interval bias, bit for bit, H6 its
    // gyro-bias columns times dT, and H7 is not zero; at zero slope and unit
    // scale it is the stock factor at the bias itself, the library's own
    // reference; the clone and the whitened error through Values.
    using gtsam::imuBias::ConstantBias;
    using gtsam::Pose3;
    using gtsam::Vector9;
    const Samples d = turningSamples();
    const ConstantBias linearizedAt(Vector3(.05, -.03, .08), Vector3(.003, -.002, .004));
    const Vector6 sHat = offNominalScale();
    gtsam::Matrix96 Hs;
    const auto pim = preintegrateImu(d, .037, .863, linearizedAt, sHat, fixtureNoise(104), ImuStepObserver(), &Hs);
    const double dT = 4.5;
    const ScaledImuFactor factor(X(0), V(0), X(1), V(1), B(0), T(0), S(0), pim, dT, Hs, sHat);
    QCOMPARE(factor.temperatureDelta(), dT);
    QVERIFY(factor.scaleJacobian() == Hs);
    QVERIFY(factor.linearizationScale() == sHat);
    QVERIFY(factor.keys() == gtsam::KeyVector({X(0), V(0), X(1), V(1), B(0), T(0), S(0)}));
    QVERIFY(factor.preintegratedMeasurements().preintegrated() == pim.preintegrated());

    const Pose3 pose_i(Rot3::RzRyRx(.3, -.2, .1), Vector3(1, 2, 3));
    const Vector3 vel_i(2, -1, .5);
    const Pose3 pose_j(Rot3::RzRyRx(.35, -.15, .12), Vector3(2.5, 1.2, 3.4));
    const Vector3 vel_j(2.6, -2.4, .9);
    const ConstantBias bias(Vector3(.04, -.02, .07), Vector3(.002, -.001, .005));
    const Vector3 slope(2e-4, -1e-4, 3e-4);
    Vector6 scale;
    scale << 1.012, .985, 1.03, .975, 1.02, 1.0;

    gtsam::Matrix H1, H2, H3, H4, H5, H6, H7;
    const gtsam::Vector error = factor.evaluateError(pose_i, vel_i, pose_j, vel_j, bias, slope, scale,
                                                     &H1, &H2, &H3, &H4, &H5, &H6, &H7);
    QCOMPARE(error.size(), Eigen::Index(9));
    const auto e = [&factor](const Pose3 &pi, const Vector3 &vi, const Pose3 &pj, const Vector3 &vj,
                             const ConstantBias &b, const Vector3 &sl, const Vector6 &sc) -> Vector9 {
        return factor.evaluateError(pi, vi, pj, vj, b, sl, sc);
    };
    const gtsam::Matrix N1 = gtsam::numericalDerivative11<Vector9, Pose3>(
        [&](const Pose3 &x) { return e(x, vel_i, pose_j, vel_j, bias, slope, scale); }, pose_i);
    const gtsam::Matrix N2 = gtsam::numericalDerivative11<Vector9, Vector3>(
        [&](const Vector3 &x) { return e(pose_i, x, pose_j, vel_j, bias, slope, scale); }, vel_i);
    const gtsam::Matrix N3 = gtsam::numericalDerivative11<Vector9, Pose3>(
        [&](const Pose3 &x) { return e(pose_i, vel_i, x, vel_j, bias, slope, scale); }, pose_j);
    const gtsam::Matrix N4 = gtsam::numericalDerivative11<Vector9, Vector3>(
        [&](const Vector3 &x) { return e(pose_i, vel_i, pose_j, x, bias, slope, scale); }, vel_j);
    const gtsam::Matrix N5 = gtsam::numericalDerivative11<Vector9, ConstantBias>(
        [&](const ConstantBias &x) { return e(pose_i, vel_i, pose_j, vel_j, x, slope, scale); }, bias);
    const gtsam::Matrix N6 = gtsam::numericalDerivative11<Vector9, Vector3>(
        [&](const Vector3 &x) { return e(pose_i, vel_i, pose_j, vel_j, bias, x, scale); }, slope);
    const gtsam::Matrix N7 = gtsam::numericalDerivative11<Vector9, Vector6>(
        [&](const Vector6 &x) { return e(pose_i, vel_i, pose_j, vel_j, bias, slope, x); }, scale);
    const struct { const char *name; const gtsam::Matrix *analytic, *numeric; } blocks[] = {
        {"H1", &H1, &N1}, {"H2", &H2, &N2}, {"H3", &H3, &N3}, {"H4", &H4, &N4}, {"H5", &H5, &N5}, {"H6", &H6, &N6},
        {"H7", &H7, &N7}};
    for (const auto &block : blocks) {
        QCOMPARE(block.analytic->rows(), block.numeric->rows());
        QCOMPARE(block.analytic->cols(), block.numeric->cols());
        const double worst = (*block.analytic-*block.numeric).cwiseAbs().maxCoeff();
        qInfo() << block.name << "max |analytic - numeric|" << worst;
        QVERIFY2(worst < 1e-6, block.name);
    }
    QCOMPARE(H7.cols(), Eigen::Index(6));

    // At S = s^ the stock factor on the same preintegration, evaluated at the
    // interval bias [B_a; B_g + T dT], bit for bit (Eigen's == is element-wise
    // equality): the error and H1..H5; H6 is its gyro-bias columns times dT,
    // the same arithmetic; H7 is the scale's chain.
    const ConstantBias atInterval(bias.accelerometer(), bias.gyroscope() + slope*dT);
    const gtsam::ImuFactor stock(X(0), V(0), X(1), V(1), B(0), pim);
    gtsam::Matrix G1, G2, G3, G4, G5, Z1, Z2, Z3, Z4, Z5, Z6, Z7;
    const gtsam::Vector stockError = stock.evaluateError(pose_i, vel_i, pose_j, vel_j, atInterval,
                                                         &G1, &G2, &G3, &G4, &G5);
    const gtsam::Vector atLinearization = factor.evaluateError(pose_i, vel_i, pose_j, vel_j, bias, slope, sHat,
                                                               &Z1, &Z2, &Z3, &Z4, &Z5, &Z6, &Z7);
    QVERIFY(atLinearization == stockError);
    QVERIFY(Z1 == G1 && Z2 == G2 && Z3 == G3 && Z4 == G4 && Z5 == G5);
    QVERIFY(Z6 == gtsam::Matrix(G5.rightCols<3>()*dT));
    QVERIFY(Z7.cwiseAbs().maxCoeff() > 1e-3);
    QVERIFY(!(error == stockError));   // and away from s^ it is not

    // At zero slope and unit scale (the factor preintegrated at s^ = ones):
    // the stock factor on the same preintegration at the bias itself, bit for
    // bit. H6 does not depend on the slope: it is the stock gyro-bias columns
    // times dT. At dT = 0 the slope has no effect at all, and H6 is zero.
    const Vector6 ones = Vector6::Ones();
    gtsam::Matrix96 unitJacobian;
    const auto unitPim = preintegrateImu(d, .037, .863, linearizedAt, ones, fixtureNoise(104), ImuStepObserver(),
                                         &unitJacobian);
    const ScaledImuFactor unit(X(0), V(0), X(1), V(1), B(0), T(0), S(0), unitPim, dT, unitJacobian, ones);
    const gtsam::ImuFactor unitStock(X(0), V(0), X(1), V(1), B(0), unitPim);
    gtsam::Matrix U1, U2, U3, U4, U5, W1, W2, W3, W4, W5, W6, W7;
    const gtsam::Vector unitStockError = unitStock.evaluateError(pose_i, vel_i, pose_j, vel_j, bias,
                                                                 &U1, &U2, &U3, &U4, &U5);
    const gtsam::Vector zeroSlopeError = unit.evaluateError(pose_i, vel_i, pose_j, vel_j, bias, Vector3::Zero(),
                                                            ones, &W1, &W2, &W3, &W4, &W5, &W6, &W7);
    QVERIFY(zeroSlopeError == unitStockError);
    QVERIFY(W1 == U1 && W2 == U2 && W3 == U3 && W4 == U4 && W5 == U5);
    QVERIFY(W6 == gtsam::Matrix(U5.rightCols<3>()*dT));
    QVERIFY(!W6.isZero(0));
    const ScaledImuFactor atReference(X(0), V(0), X(1), V(1), B(0), T(0), S(0), unitPim, 0., unitJacobian, ones);
    gtsam::Matrix R1, R2, R3, R4, R5, R6, R7;
    const gtsam::Vector referenceError = atReference.evaluateError(pose_i, vel_i, pose_j, vel_j, bias, slope, ones,
                                                                   &R1, &R2, &R3, &R4, &R5, &R6, &R7);
    QVERIFY(referenceError == unitStockError);
    QVERIFY(R1 == U1 && R2 == U2 && R3 == U3 && R4 == U4 && R5 == U5);
    QVERIFY(R6.isZero(0));

    // Through Values: at zero slope and unit scale the whitened errors agree
    // (the same covariance), and at dT = 0 with the slope; the clone
    // evaluates like the original away from s^.
    gtsam::Values values;
    values.insert(X(0), pose_i);
    values.insert(V(0), vel_i);
    values.insert(X(1), pose_j);
    values.insert(V(1), vel_j);
    values.insert(B(0), bias);
    values.insert(T(0), Vector3(Vector3::Zero()));
    values.insert(S(0), ones);
    QVERIFY(unit.whitenedError(values) == unitStock.whitenedError(values));
    values.update(T(0), slope);
    QVERIFY(atReference.whitenedError(values) == unitStock.whitenedError(values));
    values.update(S(0), scale);
    const gtsam::NonlinearFactor::shared_ptr clone = factor.clone();
    QVERIFY(clone != nullptr);
    QVERIFY(clone.get() != &factor);
    QVERIFY(clone->error(values) == factor.error(values));
    QVERIFY(factor.error(values) > 0);
}

void FusionKernelTest::scaleGraphShape()
{
    // Clauses 18 and 48, criterion 4: with the scale state the graph holds,
    // per state, the position and velocity factors and the scaled IMU factor
    // over X(k-1), V(k-1), X(k), V(k), B(0), T(0), S(0), preintegrated at
    // the interval's bias and the linearization's scale with its Jacobian;
    // then the bias prior, the slope prior and last the scale prior: mean
    // ones, sigmas the noise unit's sensitivity tolerances (G_So%, 1 %, for
    // both sensors). A full fit's values hold S(0). The constant model's
    // graph has neither S(0) nor T(0): its IMU factors are stock.
    using gtsam::imuBias::ConstantBias;
    Samples d = boundarySamples(Vector3(1, -2, .5));
    for (Vector3 &rate : d.gyro)
        rate = Vector3(.1, -.05, .2);
    for (size_t i = 0; i < d.imuTime.size(); ++i)
        d.temperature.push_back(40+.01*double(i));
    validateSamples(d, Tuning{});
    const Tuning tuning = tuningAt(104);
    const GyroBiasModel model = gyroBiasModelFor(d);
    const ConstantBias bias(Vector3(.05, -.03, .08), Vector3(.003, -.002, .004));
    const Vector3 slope(1e-3, 0, 0);
    const Vector6 at = offNominalScale();
    const auto graph = buildFactorGraph(d, BiasLinearization{bias, slope, at}, model, tuning);
    QCOMPARE(graph.size(), size_t(8));
    QVERIFY(dynamic_cast<const gtsam::GPSFactor *>(graph.at(0).get()));
    QVERIFY(dynamic_cast<const gtsam::PriorFactor<Vector3> *>(graph.at(1).get()));
    QVERIFY(dynamic_cast<const gtsam::GPSFactor *>(graph.at(2).get()));
    QVERIFY(dynamic_cast<const gtsam::PriorFactor<Vector3> *>(graph.at(3).get()));
    const auto *imu = dynamic_cast<const ScaledImuFactor *>(graph.at(4).get());
    QVERIFY(imu);
    QVERIFY(imu->keys() == gtsam::KeyVector({X(0), V(0), X(1), V(1), B(0), T(0), S(0)}));
    QVERIFY(imu->temperatureDelta() == temperatureAtFix(d, 0)-model.tRef);
    QVERIFY(imu->linearizationScale() == at);
    gtsam::Matrix96 Hs;
    const auto pim = preintegrateImu(d, .037, .863, intervalBias(d, 0, bias, slope, model), at, tuning.noise,
                                     ImuStepObserver(), &Hs);
    QVERIFY(imu->preintegratedMeasurements().preintegrated() == pim.preintegrated());
    QVERIFY(imu->preintegratedMeasurements().preintMeasCov() == pim.preintMeasCov());
    QVERIFY(imu->scaleJacobian() == Hs);
    QVERIFY(dynamic_cast<const gtsam::PriorFactor<ConstantBias> *>(graph.at(5).get()));
    const auto *slopePrior = dynamic_cast<const gtsam::PriorFactor<Vector3> *>(graph.at(6).get());
    QVERIFY(slopePrior && slopePrior->keys() == gtsam::KeyVector({T(0)}));
    const auto *scalePrior = dynamic_cast<const gtsam::PriorFactor<Vector6> *>(graph.at(7).get());
    QVERIFY(scalePrior);
    QVERIFY(scalePrior->keys() == gtsam::KeyVector({S(0)}));
    QVERIFY(scalePrior->prior() == Vector6::Ones());
    const auto sigmas = std::dynamic_pointer_cast<gtsam::noiseModel::Diagonal>(scalePrior->noiseModel());
    QVERIFY(sigmas != nullptr);
    const double accelerometer = tuning.noise.accelerometer.sensitivityTolerance;
    const double gyroscope = tuning.noise.gyroscope.sensitivityTolerance;
    Vector6 expectedSigmas;
    expectedSigmas << accelerometer, accelerometer, accelerometer, gyroscope, gyroscope, gyroscope;
    QVERIFY(sigmas->sigmas() == gtsam::Vector(expectedSigmas));
    QCOMPARE(accelerometer, .01);
    QCOMPARE(gyroscope, .01);

    // The constant model's graph: six factors, the stock factor at 4, no
    // factor on T(0) or S(0).
    const auto stock = buildFactorGraph(d, BiasLinearization{bias}, GyroBiasModel{}, tuning);
    QCOMPARE(stock.size(), size_t(6));
    QVERIFY(dynamic_cast<const gtsam::ImuFactor *>(stock.at(4).get()));
    for (const auto &factor : stock) {
        for (const gtsam::Key key : {T(0), S(0)})
            QVERIFY(std::find(factor->keys().begin(), factor->keys().end(), key) == factor->keys().end());
    }

    // A full fit (coarse_linear's, as the pipeline runs it).
    const WindowFit &f = fixtureFit(QStringLiteral("coarse_linear"));
    QVERIFY(f.fit.converged);
    QVERIFY(f.fit.biasModel.temperatureLinear);
    QVERIFY(f.fit.values.exists(S(0)));
    QVERIFY(f.fit.scale == f.fit.values.at<Vector6>(S(0)));
    const size_t n = f.window.gnssTime.size();
    QCOMPARE(f.fit.graph.size(), 3*n-1+3);
    for (size_t k = 1; k < n; ++k) {
        const auto *scaled = dynamic_cast<const ScaledImuFactor *>(f.fit.graph.at(3*k+1).get());
        QVERIFY2(scaled, qPrintable(QString::number(k)));
        QVERIFY(scaled->keys() == gtsam::KeyVector({X(k-1), V(k-1), X(k), V(k), B(0), T(0), S(0)}));
    }
    QVERIFY(dynamic_cast<const gtsam::PriorFactor<Vector6> *>(f.fit.graph.at(f.fit.graph.size()-1).get()));
}

void FusionKernelTest::fitRepreintegratesAtTheFittedScale()
{
    // Clause 19 and criterion 5: after a converged fit every IMU factor of
    // the reported graph was preintegrated at the fitted scale (its
    // linearization scale is fit.scale bit for bit, which is S(0) of the
    // values), its preintegration and scale Jacobian are preintegrateImu()'s
    // at the interval's bias and that scale, so at the fit's values its
    // S - s^ is zero and it evaluates as the stock gtsam::ImuFactor on the
    // same preintegration at the interval bias, bit for bit; the scale prior
    // is the graph's last factor, and the fit settled under the same cost
    // test.
    using gtsam::imuBias::ConstantBias;
    for (const char *name : {"coarse_linear", "coarse_maneuver", "stationary_spin", "bridged_hole"}) {
        const WindowFit &f = fixtureFit(QLatin1String(name));
        QVERIFY2(f.fit.converged, name);
        QCOMPARE(f.fit.stopping.rule, std::string(StopRule::kSettled));
        QVERIFY(f.fit.scale == f.fit.values.at<Vector6>(S(0)));
        const ConstantBias bias = f.fit.values.at<ConstantBias>(B(0));
        size_t factors = 0;
        bool sameScale = true, samePreintegration = true, sameJacobian = true, stockError = true;
        for (const auto &factor : f.fit.graph) {
            const auto *scaled = dynamic_cast<const ScaledImuFactor *>(factor.get());
            if (!scaled)
                continue;
            ++factors;
            const size_t k = gtsam::Symbol(scaled->keys()[2]).index();
            gtsam::Matrix96 Hs;
            const auto pim = preintegrateImu(f.window, f.window.gnssTime[k-1], f.window.gnssTime[k],
                                             intervalBias(f.window, k-1, bias, f.fit.gyroBiasSlope, f.fit.biasModel),
                                             f.fit.scale, f.tuning.noise, ImuStepObserver(), &Hs);
            sameScale = sameScale && scaled->linearizationScale() == f.fit.scale;
            samePreintegration = samePreintegration
                && scaled->preintegratedMeasurements().preintegrated() == pim.preintegrated()
                && scaled->preintegratedMeasurements().preintMeasCov() == pim.preintMeasCov();
            sameJacobian = sameJacobian && scaled->scaleJacobian() == Hs;
            const gtsam::Values &v = f.fit.values;
            const gtsam::Pose3 pose_i = v.at<gtsam::Pose3>(X(k-1)), pose_j = v.at<gtsam::Pose3>(X(k));
            const Vector3 vel_i = v.at<Vector3>(V(k-1)), vel_j = v.at<Vector3>(V(k));
            const gtsam::ImuFactor stock(X(k-1), V(k-1), X(k), V(k), B(0), scaled->preintegratedMeasurements());
            gtsam::Matrix G1, G2, G3, G4, G5;
            stockError = stockError
                && scaled->evaluateError(pose_i, vel_i, pose_j, vel_j, bias, f.fit.gyroBiasSlope, f.fit.scale)
                       == stock.evaluateError(pose_i, vel_i, pose_j, vel_j,
                                              intervalBias(f.window, k-1, bias, f.fit.gyroBiasSlope, f.fit.biasModel),
                                              &G1, &G2, &G3, &G4, &G5);
        }
        qInfo() << name << ": fitted scale acc" << f.fit.scale(0) << f.fit.scale(1) << f.fit.scale(2) << "gyro"
                << f.fit.scale(3) << f.fit.scale(4) << f.fit.scale(5) << "after" << f.fit.stopping.passes << "passes";
        QCOMPARE(factors, f.window.gnssTime.size()-1);
        QVERIFY2(sameScale, name);
        QVERIFY2(samePreintegration, name);
        QVERIFY2(sameJacobian, name);
        QVERIFY2(stockError, name);
        const auto *last = dynamic_cast<const gtsam::PriorFactor<Vector6> *>(f.fit.graph.at(f.fit.graph.size()-1).get());
        QVERIFY2(last && last->keys() == gtsam::KeyVector({S(0)}), name);
        QCOMPARE(f.fit.residuals.back().kind, std::string("scale_prior"));
    }
}

void FusionKernelTest::reconstructionUsesTheFittedScale()
{
    // Clause 19 and criterion 6, in the pattern of
    // reconstructionUsesIntervalBias: one fix interval whose second state is
    // the forward integration of the readings divided by a scale s away from
    // one (predictFits() at fit.scale = s), turning and accelerating, with a
    // non-zero bias. Reconstructed with fit.scale = s the mismatch is at
    // rounding; with ones the forward integration misses the fitted end by
    // the scale's share of the force over the interval. The published
    // acceleration is R (f ./ s_a - b_a) + g + the mean of the corrections.
    using gtsam::imuBias::ConstantBias;
    WindowFit f;
    f.window = boundarySamples(Vector3(1, -2, .5));
    for (Vector3 &rate : f.window.gyro)
        rate = Vector3(.1, -.05, .2);
    f.tuning = tuningAt(104);
    f.tuning.maxGap = SampleContinuity::holeThreshold(f.window.imuTime);
    Vector6 s;
    s << 1.02, .99, 1.01, 1.01, .98, 1.02;
    f.fit.scale = s;
    predictFits(f, ConstantBias(Vector3(.05, -.03, .08), Vector3(.003, -.002, .004)),
                gtsam::NavState(Rot3::RzRyRx(.1, -.2, .3), Vector3(1, 2, 3), Vector3(20, -3, 5)),
                gtsam::Vector9::Zero());

    const IntervalReconstruction atScale = reconstructInterval(f.window, f.fit, f.tuning, 0);
    WindowFit unit = f;
    unit.fit.scale = Vector6::Ones();
    const IntervalReconstruction atOne = reconstructInterval(unit.window, unit.fit, unit.tuning, 0);
    qInfo() << "mismatch: at the fitted scale" << atScale.mismatch.cwiseAbs().maxCoeff() << ", at ones: attitude"
            << atOne.mismatch.head<3>().norm() << "rad, velocity" << atOne.mismatch.tail<3>().norm() << "m/s";
    QVERIFY(atScale.mismatch.cwiseAbs().maxCoeff() <= 1e-12);
    QVERIFY(atOne.mismatch.tail<3>().norm() > 1e-2);
    QVERIFY(atOne.mismatch.head<3>().norm() > 1e-4);

    const WindowSeams w = seamsOf(f);
    const ImuRateTrajectory out = reconstructAtImuRate(f.window, f.fit, f.tuning);
    QVERIFY(out.time.size() > 80);
    double scaleShare = 0;
    for (size_t i = 0; i < out.time.size(); ++i) {
        const Vector3 expected = expectedAcceleration(f, w, out, i);
        QVERIFY2((out.acceleration[i]-expected).norm() <= 1e-12*(1+expected.norm()), qPrintable(QString::number(i)));
        // Not vacuous: the undivided reading would publish something else.
        const Vector3 undivided = expectedAcceleration(unit, w, out, i);
        scaleShare = std::max(scaleShare, (expected-undivided).norm());
    }
    QVERIFY(scaleShare > .05);
}

void FusionKernelTest::restLeavesTheScaleAtItsPrior()
{
    // Clause 20 (as settled) and clause 54, criterion 7 (as settled): at rest
    // every fitted factor of rest_throughout stays at one within its prior's
    // sigma, the sensitivity tolerance. The accelerometer's stay within a
    // tenth of it: the data constrain only the corrected reading along
    // gravity, which the scale and the bias of each axis share in proportion
    // to their priors' variances (about a tenth of a 0.05 m/s^2 bias is
    // 5e-4 of the factor). That sharing holds for the deterministic part of a
    // reading, and the gyro's are not bound that tightly: the factor whitens
    // the residual of the divided readings with a noise model that does not
    // depend on the scale, so a factor above one shrinks the noise left in
    // that residual while its weight stays the same, and at rest the noise is
    // all the gyro reads besides its bias. Dividing the density by the
    // linearization scale would not remove this, since the covariance is
    // fixed within a pass; only a weight that changes with the scale itself
    // would. On this recording the y axis, whose bias of -.1 deg/s lies
    // between two lattice points so that its readings dither by a whole step,
    // fits 1.0033 (a third of its tolerance); the departure grows with the
    // noise's variance and the prior's, and with the gyro's noise removed
    // every gyro factor fits one.
    const InitializerRun &run = restThroughoutRun();
    QVERIFY2(run.result.outcome == Fusion::Outcome::Succeeded, qPrintable(run.result.reason));
    const ImuNoise noise = imuNoise(toChannels(initializerFixture(QStringLiteral("rest_throughout"))).imuConfiguration);
    const QJsonObject scale = run.diagnostics.value("model").toObject().value("scale").toObject();
    const QJsonArray acc = scale.value("acc").toArray(), gyro = scale.value("gyro").toArray();
    QCOMPARE(acc.size(), 3);
    QCOMPARE(gyro.size(), 3);
    double accelerometer = 0, gyroscope = 0;
    for (int i = 0; i < 3; ++i) {
        accelerometer = std::max(accelerometer, std::abs(acc.at(i).toDouble()-1));
        gyroscope = std::max(gyroscope, std::abs(gyro.at(i).toDouble()-1));
    }
    qInfo() << "rest_throughout: scale acc" << acc.at(0).toDouble() << acc.at(1).toDouble() << acc.at(2).toDouble()
            << "gyro" << gyro.at(0).toDouble() << gyro.at(1).toDouble() << gyro.at(2).toDouble()
            << "; largest departure: accelerometer" << accelerometer/noise.accelerometer.sensitivityTolerance
            << "of its tolerance, gyro" << gyroscope/noise.gyroscope.sensitivityTolerance;
    QVERIFY(accelerometer <= .1*noise.accelerometer.sensitivityTolerance);
    QVERIFY(gyroscope <= noise.gyroscope.sensitivityTolerance);

    // Item 1409: through both stages. The factors held at one, then released
    // from the held solution, and the released stage settles and is kept.
    const QJsonObject release = run.diagnostics.value("scale_release").toObject();
    qInfo() << "rest_throughout: held" << release.value("held").toObject().toVariantMap() << "; released"
            << release.value("released").toObject().toVariantMap();
    QCOMPARE(release.value("kept").toBool(false), true);
    QVERIFY(release.value("reason").isNull());
    QCOMPARE(release.value("released").toObject().value("rule").toString(), QStringLiteral("settled"));
}

void FusionKernelTest::scaleRecordingRecoversTheFactor()
{
    // Clause 54, criterion 8, and item 1408: on scale_recording, whose
    // accelerometer x axis reads 2 % high under a zero-mean periodic north
    // acceleration, the full fit converges through its released stage, which
    // settles and is kept, and recovers the factor within the prior's
    // tolerance; the other axes stay at one (y and the gyro within a tenth of
    // their tolerance; z, which shares the z bias under gravity, within its
    // tolerance); and the objective falls below the held stage's, read from
    // the fit's account: the held stage is the fit with the scale held at one,
    // on the same window from the same initialization.
    const QString name = QStringLiteral("scale_recording");
    const Tuning tuning = pipelineTuning(name, Tuning{});
    const Samples window = windowOf(name, Tuning{});
    const Initialization init = initialize(window, tuning);
    QCOMPARE(init.account.segments.size(), size_t(1));
    QVERIFY(!init.account.segments.front().fallback);
    const GyroBiasModel model = gyroBiasModelFor(window);
    const FitResult with = fitFactorGraph(window, init.state, tuning, QString::fromLatin1(kFullFitPassFormat),
                                          Checkpoint(), model);
    const ScaleRelease &release = with.scaleRelease;
    QVERIFY(release.released.has_value());
    for (const auto &[label, stage] : {std::pair<const char *, const StageAccount *>{"held", &release.held},
                                       std::pair<const char *, const StageAccount *>{"released", &*release.released}}) {
        qInfo() << "scale_recording" << label << "stage: rule" << stage->rule.c_str() << ", passes" << stage->passes
                << ", iterations" << stage->iterations << ", objective" << stage->objective;
    }
    const auto bias = with.values.at<gtsam::imuBias::ConstantBias>(B(0));
    qInfo() << "scale_recording: scale acc" << with.scale(0) << with.scale(1) << with.scale(2) << "gyro"
            << with.scale(3) << with.scale(4) << with.scale(5) << "; acc bias" << bias.accelerometer().x()
            << bias.accelerometer().y() << bias.accelerometer().z() << "; position RMS" << with.positionRms
            << "m, velocity RMS" << with.velocityRms << "m/s; s_ax against 1.02 (" << (with.scale(0)-1.02)/.01
            << "of the tolerance); objective ratio released / held" << with.objective/release.held.objective;
    QVERIFY(with.converged);
    QVERIFY(release.kept);
    QVERIFY(release.reason.empty());
    QCOMPARE(release.released->rule, std::string(StopRule::kSettled));
    QVERIFY(with.objective == release.released->objective);
    const double accelerometer = tuning.noise.accelerometer.sensitivityTolerance;
    const double gyroscope = tuning.noise.gyroscope.sensitivityTolerance;
    QVERIFY(std::abs(with.scale(0)-1.02) <= accelerometer);
    QVERIFY(std::abs(with.scale(1)-1) <= .1*accelerometer);
    QVERIFY(std::abs(with.scale(2)-1) <= accelerometer);
    for (int i = 3; i < 6; ++i)
        QVERIFY2(std::abs(with.scale(i)-1) <= .1*gyroscope, qPrintable(QString::number(i)));
    QVERIFY(with.objective < release.held.objective);
}

void FusionKernelTest::diagnosticsReportTheScale()
{
    // Clause 21, criterion 9: a successful fit's diagnostics carry
    // model.scale, the fitted factors themselves (acc x, y, z and gyro x, y,
    // z: fit.scale of the same fit through the seams, bit for bit) beside
    // their sigmas (diagnosticsReportTheScaleSigma); model holds exactly
    // gyro_bias, noise and scale; the residuals end with the three priors,
    // bias, slope and scale, the scale prior's the squared whitened departure
    // from one; the IMU kind stays `imu`; and the limitations name the scale
    // factors the reconstruction holds and the accuracy published.
    const QString name = QStringLiteral("coarse_maneuver");
    const Fusion::Result result = runPipeline(toChannels(fusionFixture(name)), Tuning{}, Checkpoint());
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
    const QJsonObject diagnostics = diagnosticsOf(result);
    const QJsonObject model = diagnostics.value("model").toObject();
    QCOMPARE(model.keys(), QStringList({QStringLiteral("gyro_bias"), QStringLiteral("noise"), QStringLiteral("scale")}));
    const QJsonObject scale = model.value("scale").toObject();
    QCOMPARE(scale.keys(), QStringList({QStringLiteral("acc"), QStringLiteral("acc_sigma"), QStringLiteral("gyro"),
                                        QStringLiteral("gyro_sigma")}));
    const QJsonArray acc = scale.value("acc").toArray(), gyro = scale.value("gyro").toArray();
    QCOMPARE(acc.size(), 3);
    QCOMPARE(gyro.size(), 3);
    const WindowFit &f = fixtureFit(name);
    for (int i = 0; i < 3; ++i) {
        QVERIFY(acc.at(i).toDouble() == f.fit.scale(i));
        QVERIFY(gyro.at(i).toDouble() == f.fit.scale(3+i));
    }

    const QJsonArray residuals = diagnostics.value("residuals").toArray();
    QVERIFY(residuals.size() >= 3);
    const QJsonObject scalePrior = residuals.last().toObject();
    QCOMPARE(scalePrior.value("kind").toString(), QStringLiteral("scale_prior"));
    QCOMPARE(residuals.at(residuals.size()-2).toObject().value("kind").toString(), QStringLiteral("slope_prior"));
    QCOMPARE(residuals.at(residuals.size()-3).toObject().value("kind").toString(), QStringLiteral("bias_prior"));
    const ImuNoise noise = imuNoise(toChannels(fusionFixture(name)).imuConfiguration);
    double expected = 0;
    for (int i = 0; i < 6; ++i) {
        const double tolerance = i < 3 ? noise.accelerometer.sensitivityTolerance : noise.gyroscope.sensitivityTolerance;
        expected += (f.fit.scale(i)-1)*(f.fit.scale(i)-1)/(tolerance*tolerance);
    }
    const double got = scalePrior.value("squared_whitened_error").toDouble(-1);
    qInfo() << name << ": scale prior's squared whitened error" << got << "(recomputed" << expected << ")";
    QVERIFY(std::abs(got-expected) <= 1e-9*expected);
    QVERIFY(expected > 0);
    int imu = 0;
    for (const QJsonValue &entry : residuals) {
        if (entry.toObject().value("kind").toString() == QStringLiteral("imu"))
            ++imu;
    }
    QCOMPARE(imu, diagnostics.value("gnss_states").toInt()-1);
    QCOMPARE(diagnostics.value("limitations").toString(), QStringLiteral(
        "Local batch convergence; heading may be ambiguous. Between fixes one linearized pass with the fitted fix "
        "states, biases and scale factors held. Accuracies are first-order, one standard deviation under the "
        "documented noise model, widened where the residuals exceed it."));
}

void FusionKernelTest::scaleReleaseIsAccountedFor()
{
    // Items 1401, 1405 and 1406: the full fit of coarse_maneuver runs the
    // held stage, reports "Releasing the scale factors" once, between the
    // held stage's last iteration and the released stage's first graph build,
    // and runs the released stage, whose passes are numbered on from the
    // held stage's in the texts and in the trace. scale_release accounts for
    // both, its counts adding up to the history and seeds[0].iterations; the
    // reported stopping account is the released stage's own, since it is
    // kept; and the fit's own account (FitResult::scaleRelease, through the
    // seams) is what the diagnostics wrote, field by field.
    const QString name = QStringLiteral("coarse_maneuver");
    QStringList texts;
    const Checkpoint collecting([&texts](const QString &text) { texts.append(text); }, {});
    PipelineTrace trace;
    const Fusion::Result result = runPipeline(toChannels(fusionFixture(name)), Tuning{}, collecting, &trace);
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
    const QJsonObject diagnostics = diagnosticsOf(result);

    const QJsonObject release = diagnostics.value("scale_release").toObject();
    QCOMPARE(release.keys(), QStringList({"held", "kept", "reason", "released"}));
    const QJsonObject held = release.value("held").toObject(), released = release.value("released").toObject();
    const QStringList accountKeys{"iterations", "objective", "passes", "rule"};
    QCOMPARE(held.keys(), accountKeys);
    QCOMPARE(released.keys(), accountKeys);
    qInfo() << name << ": held" << held.toVariantMap() << "; released" << released.toVariantMap();
    QCOMPARE(held.value("rule").toString(), QStringLiteral("settled"));
    QCOMPARE(released.value("rule").toString(), QStringLiteral("settled"));
    QCOMPARE(release.value("kept").toBool(false), true);
    QVERIFY(release.value("reason").isNull());

    // The reported stage is the released one.
    const QJsonObject stopping = diagnostics.value("stopping").toObject();
    QCOMPARE(stopping.value("rule").toString(), released.value("rule").toString());
    QCOMPARE(stopping.value("passes").toInt(), released.value("passes").toInt());
    QCOMPARE(trace.stopping.passes, released.value("passes").toInt());
    QVERIFY(diagnostics.value("objective").toDouble() == released.value("objective").toDouble());
    QVERIFY(held.value("objective").isDouble());

    // The counts: both stages in the history, the passes numbered on.
    const int heldPasses = held.value("passes").toInt(), releasedPasses = released.value("passes").toInt();
    const int heldIterations = held.value("iterations").toInt();
    const int releasedIterations = released.value("iterations").toInt();
    QVERIFY(heldPasses >= 1 && releasedPasses >= 1 && heldIterations >= 1 && releasedIterations >= 1);
    QCOMPARE(heldIterations+releasedIterations, int(trace.history.size()));
    QCOMPARE(diagnostics.value("seeds").toArray().first().toObject().value("iterations").toInt(),
             int(trace.history.size()));
    std::set<int> outers;
    for (const FitIteration &h : trace.history)
        outers.insert(h.outer);
    QCOMPARE(int(outers.size()), heldPasses+releasedPasses);
    QCOMPARE(trace.history[size_t(heldIterations)-1].outer, heldPasses-1);
    QCOMPARE(trace.history[size_t(heldIterations)].outer, heldPasses);
    QCOMPARE(trace.history[size_t(heldIterations)].iteration, 0);

    // The boundary, once, in its place among the texts.
    const QString boundary = QStringLiteral("Releasing the scale factors");
    QCOMPARE(texts.count(boundary), 1);
    const qsizetype at = texts.indexOf(boundary);
    const QRegularExpression passText(QStringLiteral("^Pass ([0-9]+), iteration ([0-9]+)$"));
    QString lastBefore, firstAfter;
    for (qsizetype i = 0; i < texts.size(); ++i) {
        const QRegularExpressionMatch match = passText.match(texts.at(i));
        if (!match.hasMatch())
            continue;
        const int pass = match.captured(1).toInt();
        if (i < at) {
            QVERIFY2(pass <= heldPasses, qPrintable(texts.at(i)));
            lastBefore = texts.at(i);
        } else {
            QVERIFY2(pass > heldPasses && pass <= heldPasses+releasedPasses, qPrintable(texts.at(i)));
            if (firstAfter.isEmpty())
                firstAfter = texts.at(i);
        }
    }
    const int lastHeldIteration = trace.history[size_t(heldIterations)-1].iteration+1;
    QCOMPARE(lastBefore, QStringLiteral("Pass %1, iteration %2").arg(heldPasses).arg(lastHeldIteration));
    QCOMPARE(texts.at(at+1), QStringLiteral("Integrating IMU factors"));
    QCOMPARE(firstAfter, QStringLiteral("Pass %1, iteration 1").arg(heldPasses+1));

    // The fit's account through the seams is the one the diagnostics wrote.
    const ScaleRelease &account = fixtureFit(name).fit.scaleRelease;
    QCOMPARE(account.held.rule, held.value("rule").toString().toStdString());
    QCOMPARE(account.held.passes, heldPasses);
    QCOMPARE(account.held.iterations, heldIterations);
    QVERIFY(account.held.objective == held.value("objective").toDouble());
    QVERIFY(account.released.has_value());
    QCOMPARE(account.released->rule, released.value("rule").toString().toStdString());
    QCOMPARE(account.released->passes, releasedPasses);
    QCOMPARE(account.released->iterations, releasedIterations);
    QVERIFY(account.released->objective == released.value("objective").toDouble());
    QVERIFY(account.kept);
    QVERIFY(account.reason.empty());
}

namespace {

/// The fallback to the held fit, as `run` reports it (item 1403): a success,
/// converged, its scale_release not kept for `reason`, the reported stopping
/// account, objective and iterations the held stage's, and the factors at
/// one, within a tenth of their tolerance, with the held stage's sigmas, each
/// below a hundredth of it.
void verifyHeldFallback(const InitializerRun &run, const QString &reason, const ImuNoise &noise)
{
    QVERIFY2(run.result.outcome == Fusion::Outcome::Succeeded, qPrintable(run.result.reason));
    QVERIFY(run.trace.converged);
    const QJsonObject release = run.diagnostics.value("scale_release").toObject();
    const QJsonObject held = release.value("held").toObject();
    qInfo() << "fallback: held" << held.toVariantMap() << "; released" << release.value("released").toVariant()
            << "; reason" << release.value("reason").toString();
    QCOMPARE(release.value("kept").toBool(true), false);
    QCOMPARE(release.value("reason").toString(), reason);
    QCOMPARE(held.value("rule").toString(), QStringLiteral("settled"));
    const QJsonObject stopping = run.diagnostics.value("stopping").toObject();
    QCOMPARE(stopping.value("rule").toString(), held.value("rule").toString());
    QCOMPARE(stopping.value("passes").toInt(), held.value("passes").toInt());
    QCOMPARE(run.trace.stopping.rule, held.value("rule").toString().toStdString());
    QVERIFY(run.diagnostics.value("objective").toDouble() == held.value("objective").toDouble());
    const QJsonObject seed = run.diagnostics.value("seeds").toArray().first().toObject();
    QCOMPARE(seed.value("converged").toBool(false), true);
    QCOMPARE(seed.value("iterations").toInt(), int(run.trace.history.size()));

    const QJsonObject scale = run.diagnostics.value("model").toObject().value("scale").toObject();
    for (const char *sensor : {"acc", "gyro"}) {
        const double tolerance = QLatin1String(sensor) == QLatin1String("acc")
            ? noise.accelerometer.sensitivityTolerance : noise.gyroscope.sensitivityTolerance;
        const QJsonArray factors = scale.value(QLatin1String(sensor)).toArray();
        const QJsonArray sigmas = scale.value(QLatin1String(sensor) + QLatin1String("_sigma")).toArray();
        QCOMPARE(factors.size(), 3);
        QCOMPARE(sigmas.size(), 3);
        for (int i = 0; i < 3; ++i) {
            QVERIFY2(std::abs(factors.at(i).toDouble()-1) <= .1*tolerance, sensor);
            QVERIFY2(sigmas.at(i).toDouble(1) < .01*tolerance, sensor);
        }
    }
}

} // namespace

void FusionKernelTest::releaseFailureFallsBackToTheHeldFit()
{
    // Items 1403 and 1410: the released stage forced to fail. The forcing is
    // allPrefixFitsFailFallsBack's, at the release boundary: the progress
    // function throws FitFailure there, with the account of a `cost
    // increased` (a rule the released stage can end under in production).
    // On scale_recording, whose release would move the x accelerometer factor
    // to about 1.02, the fit falls back to the held stage: converged, the
    // factors at one, scale_release not kept with the thrown rule as the
    // reason and no released account (no iteration ran). The published
    // channels are the held fit's, bit for bit: the pipeline's against the
    // reconstruction of the seam's own fit under the same forcing
    // (imuRateIsWhatTheFitPublishes' pattern).
    const QString boundary = QStringLiteral("Releasing the scale factors");
    const Checkpoint failingRelease(
        [boundary](const QString &text) {
            if (text == boundary) {
                Stopping s;
                s.rule = StopRule::kCostIncreased;
                throw FitFailure(std::string("Nonfinite or increasing optimizer cost"), s);
            }
        },
        [] { return false; });
    const QString name = QStringLiteral("scale_recording");
    const Fusion::Channels channels = toChannels(initializerFixture(name));
    const InitializerRun run = runInitializerFixture(name, Tuning{}, failingRelease);
    verifyHeldFallback(run, QStringLiteral("cost increased"), imuNoise(channels.imuConfiguration));
    const QJsonObject release = run.diagnostics.value("scale_release").toObject();
    QVERIFY(release.value("released").isNull());
    QCOMPARE(release.value("held").toObject().value("iterations").toInt(), int(run.trace.history.size()));

    // The seam: the same stages in the pipeline's order, under the same forcing.
    const Tuning tuning = pipelineTuning(name, Tuning{});
    const Samples window = windowOf(name, Tuning{});
    const Initialization init = initialize(window, tuning, failingRelease);
    const FitResult fit = fitFactorGraph(window, init.state, tuning, QString::fromLatin1(kFullFitPassFormat),
                                         failingRelease, gyroBiasModelFor(window));
    QVERIFY(fit.converged);
    QVERIFY(!fit.scaleRelease.kept);
    QCOMPARE(fit.scaleRelease.reason, std::string(StopRule::kCostIncreased));
    QVERIFY(!fit.scaleRelease.released.has_value());
    QVERIFY(fit.objective == fit.scaleRelease.held.objective);
    QCOMPARE(fit.stopping.rule, fit.scaleRelease.held.rule);
    const FitCovariance covariance = fitCovariance(fit, window.gnssTime.size());
    QVERIFY(covariance.computed);
    const ImuRateTrajectory out = reconstructAtImuRate(window, fit, tuning, &covariance);
    std::vector<double> widenings;
    for (const double factor : wideningFactors(window.gnssTime, fit.residuals, out.time))
        widenings.push_back(widening(factor));
    Fusion::Result expected;
    fillOutputChannels(out, widenings, prepareInput(channels).epoch, expected);
    QCOMPARE(fusionChannelNames().size(), 21);
    for (const QString &channel : fusionChannelNames()) {
        QVERIFY(!fusionChannel(run.result, channel).isEmpty());
        QVERIFY2(sameBitsEverywhere(fusionChannel(run.result, channel), fusionChannel(expected, channel)),
                 qPrintable(channel));
    }
}

void FusionKernelTest::divergenceEndsTheHeldStage_data()
{
    QTest::addColumn<double>("maxImuNrms");
    QTest::addColumn<double>("lower");
    QTest::addColumn<double>("upper");
    QTest::newRow("imu nrms bound zero") << 0. << .5 << 2.;
    QTest::newRow("scale range empty") << 10. << 1. << 1.;
}

void FusionKernelTest::divergenceEndsTheHeldStage()
{
    // Items 1402, 1404 and 1411: with the IMU normalized RMS bound at zero, or
    // the scale factors' range empty, no pass can be inside the bounds (the
    // comparisons are strict), so the held stage's first pass diverges and
    // ends the fit there: the solver failure of a fit that completed its
    // passes, with the reason naming `diverged`, the failure diagnostics'
    // completed-pass shape, the bounds in force in its stopping account, and
    // no released stage.
    QFETCH(double, maxImuNrms);
    QFETCH(double, lower);
    QFETCH(double, upper);
    Tuning tuning;
    tuning.divergenceMaxImuNrms = maxImuNrms;
    tuning.divergenceScaleRange = {lower, upper};
    QStringList texts;
    const Checkpoint collecting([&texts](const QString &text) { texts.append(text); }, {});
    PipelineTrace trace;
    const Fusion::Result result = runPipeline(toChannels(fusionFixture(QStringLiteral("coarse_maneuver"))), tuning,
                                              collecting, &trace);

    QCOMPARE(QString::fromLatin1(StopRule::kDiverged), QStringLiteral("diverged"));
    QVERIFY(result.outcome == Fusion::Outcome::SolverFailed);
    QCOMPARE(result.reason, QStringLiteral("Batch fusion did not converge (diverged); sensor fusion unavailable"));
    QVERIFY(!trace.converged);
    QCOMPARE(trace.stopping.rule, std::string(StopRule::kDiverged));
    QCOMPARE(trace.stopping.passes, 1);
    QVERIFY(!trace.history.empty());
    for (const FitIteration &h : trace.history)
        QCOMPARE(h.outer, 0);
    QVERIFY(!texts.contains(QStringLiteral("Releasing the scale factors")));

    const QJsonObject diagnostics = diagnosticsOf(result);
    QCOMPARE(diagnostics.keys(), kCompletedPassFailureKeys);
    QCOMPARE(diagnostics.value("failure").toString(), result.reason);
    const QJsonObject stopping = diagnostics.value("stopping").toObject();
    QCOMPARE(stopping.value("rule").toString(), QStringLiteral("diverged"));
    QCOMPARE(stopping.value("passes").toInt(), 1);
    QCOMPARE(stopping.value("divergence_max_imu_nrms").toDouble(-1), maxImuNrms);
    QCOMPARE(stopping.value("divergence_scale_range").toArray(), QJsonArray({lower, upper}));
    QVERIFY(diagnostics.value("quality").toObject().value("imu_nrms").isDouble());
    QVERIFY(allChannelsEmpty(result));
}

void FusionKernelTest::divergenceEndsTheReleasedStage()
{
    // Items 1403, 1404 and 1411: divergence in the released stage is the
    // fallback. On scale_recording the held stage's factors stay within a
    // few 1e-5 of one and the released stage moves the x accelerometer factor
    // to about 1.02, so a range of (0.5, 1.005) passes every pass of the
    // held stage and ends the released stage `diverged`, at its first pass or
    // a later one: the held fit is reported, converged, with the released
    // stage's account (its rule, its passes and the objective of the rebuilt
    // graph it diverged on) and `diverged` as the reason. The IMU normalized
    // RMS bound is not forced in the released stage alone: there is no
    // stage-specific bound to set, and the released stage starts from the
    // held stage's converged misfit, so no one bound passes every held pass
    // and refuses a released one on purpose. Both bounds end a stage through
    // the one rule, which divergenceEndsTheHeldStage shows for the IMU bound.
    const QString name = QStringLiteral("scale_recording");
    Tuning tuning;
    tuning.divergenceScaleRange = {.5, 1.005};
    const InitializerRun run = runInitializerFixture(name, tuning);
    verifyHeldFallback(run, QStringLiteral("diverged"), imuNoise(toChannels(initializerFixture(name)).imuConfiguration));
    const QJsonObject release = run.diagnostics.value("scale_release").toObject();
    const QJsonObject held = release.value("held").toObject(), released = release.value("released").toObject();
    QCOMPARE(released.value("rule").toString(), QStringLiteral("diverged"));
    QVERIFY(released.value("passes").toInt() >= 1);
    QVERIFY(released.value("iterations").toInt() >= 1);
    QVERIFY(released.value("objective").isDouble());
    QVERIFY(std::isfinite(released.value("objective").toDouble()));
    QCOMPARE(held.value("iterations").toInt()+released.value("iterations").toInt(), int(run.trace.history.size()));
    const QJsonObject stopping = run.diagnostics.value("stopping").toObject();
    QCOMPARE(stopping.value("divergence_scale_range").toArray(), QJsonArray({.5, 1.005}));
}

void FusionKernelTest::imuRateEndsAreTheFit_data()
{
    QTest::addColumn<QString>("name");
    for (const FusionFixture &fixture : fusionFixtures()) {
        if (fixture.expectSuccess)
            QTest::newRow(qPrintable(fixture.name)) << fixture.name;
    }
}

void FusionKernelTest::imuRateEndsAreTheFit()
{
    // Spec section 10, "The ends": at each interval's second fix the corrected
    // state is the fitted state to rounding, and P_n is the covariance of the
    // interval's IMU factor in the fit's reported graph bit for bit (the same
    // preintegration at the same bias with the same tuning, which also shows
    // that the observer changes nothing the steps integrate). Section 3: the
    // published axis is the window's IMU samples in [first fix, last fix).
    // Section 7: the summaries are the seam's maxima bit for bit. And what is
    // published is the seam's corrected state, bit for bit.
    QFETCH(QString, name);
    const WindowFit &f = fixtureFit(name);
    QVERIFY(f.fit.converged);
    const WindowSeams w = seamsOf(f);
    QCOMPARE(w.intervals.size(), f.window.gnssTime.size()-1);
    for (size_t k = 0; k < w.intervals.size(); ++k) {
        const IntervalReconstruction &r = w.intervals[k];
        QVERIFY2(sameStateToRounding(r.corrected.back(), fixState(f.fit, k+1)), qPrintable(QString::number(k)));
        const gtsam::Matrix factor = imuFactorCovariance(f.fit.graph, k);
        QCOMPARE(factor.rows(), Eigen::Index(9));
        QCOMPARE(factor.cols(), Eigen::Index(9));
        QVERIFY2(r.endCovariance == factor, qPrintable(QString::number(k)));
    }

    const ImuRateTrajectory out = reconstructAtImuRate(f.window, f.fit, f.tuning);
    QVERIFY(!out.time.empty());
    QVERIFY(out.time == samplesBetweenFirstAndLastFix(f.window));
    QCOMPARE(out.rotation.size(), out.time.size());
    QCOMPARE(out.position.size(), out.time.size());
    QCOMPARE(out.velocity.size(), out.time.size());
    QCOMPARE(out.acceleration.size(), out.time.size());
    for (size_t i = 0; i < out.time.size(); ++i) {
        const size_t e = edgeAt(w, out.time[i]);
        QVERIFY(w.edges[e] == out.time[i]);
        QVERIFY(out.rotation[i].matrix() == w.corrected[e].attitude().matrix());
        QVERIFY(out.position[i] == w.corrected[e].position());
        QVERIFY(out.velocity[i] == w.corrected[e].velocity());
    }

    double attitudeDeg = 0, velocity = 0, step = -1, stepTime = 0;
    for (const IntervalReconstruction &r : w.intervals) {
        attitudeDeg = std::max(attitudeDeg, r.mismatch.head<3>().norm()*180/kPi);
        velocity = std::max(velocity, r.mismatch.tail<3>().norm());
    }
    for (size_t j = 0; j < w.stepCorrection.size(); ++j) {
        if (w.stepCorrection[j].norm() > step) {
            step = w.stepCorrection[j].norm();
            stepTime = (w.edges[j]+w.edges[j+1])/2;
        }
    }
    qInfo() << name << ": largest endpoint correction" << out.maxEndpointCorrectionDeg << "deg, velocity mismatch"
            << out.maxVelocityMismatch << "m/s, step correction" << out.maxStepCorrection << "m/s^2 at"
            << out.maxStepCorrectionTime << "s";
    QVERIFY(out.maxEndpointCorrectionDeg == attitudeDeg);
    QVERIFY(out.maxVelocityMismatch == velocity);
    QVERIFY(out.maxStepCorrection == step);
    QVERIFY(out.maxStepCorrectionTime == stepTime);
}

void FusionKernelTest::imuRateSampleOnAFixIsPublishedOnce()
{
    // Sections 3 and 6 on a recording with dyadic times, so that IMU samples
    // fall exactly on the first fix (.25) and on an inner one (1.375); the
    // other two fixes fall between samples. The fitted states are the
    // predictions perturbed, so every interval has a mismatch. A sample on a
    // fix is published once, at the first edge of the interval the fix
    // starts, with the fitted state there; its acceleration takes the last
    // step of the interval before and the first of its own, except on the
    // first fix, which has only the step after it.
    using gtsam::imuBias::ConstantBias;
    WindowFit f;
    Samples &d = f.window;
    for (int i = 0; i <= 16; ++i) {
        d.imuTime.push_back(i*.125);
        d.gyro.emplace_back(.2, -.1+.02*i, .3);
        d.force.emplace_back(1+.1*i, -.5, -9.8+.05*i);
    }
    d.gnssTime = {.25, .8125, 1.375, 1.9375};
    f.tuning = tuningAt(12.5);
    f.tuning.maxGap = SampleContinuity::holeThreshold(d.imuTime);
    gtsam::Vector9 perturbation;
    perturbation << 1e-3, -2e-3, 1e-3, .01, -.02, .03, .05, -.04, .02;
    predictFits(f, ConstantBias(Vector3(.02, -.01, .03), Vector3(.001, 0, -.002)),
                gtsam::NavState(Rot3::RzRyRx(.1, -.2, .3), Vector3(1, 2, 3), Vector3(20, -3, 5)), perturbation);

    const WindowSeams w = seamsOf(f);
    QVERIFY(largestMismatch(w).velocity > 1e-2);
    const ImuRateTrajectory out = reconstructAtImuRate(d, f.fit, f.tuning);
    QVERIFY(out.time == samplesBetweenFirstAndLastFix(d));
    QCOMPARE(out.time.size(), size_t(14));   // samples 2..15
    QCOMPARE(std::count(out.time.begin(), out.time.end(), .25), std::ptrdiff_t(1));
    QCOMPARE(std::count(out.time.begin(), out.time.end(), 1.375), std::ptrdiff_t(1));

    for (const auto &[time, fix] : {std::pair<double, size_t>{.25, 0}, std::pair<double, size_t>{1.375, 2}}) {
        const size_t i = size_t(std::find(out.time.begin(), out.time.end(), time)-out.time.begin());
        QVERIFY(sameStateToRounding(gtsam::NavState(out.rotation[i], out.position[i], out.velocity[i]),
                                    fixState(f.fit, fix)));
        const size_t e = edgeAt(w, time);
        QCOMPARE(e, w.fixEdge[fix]);
        QCOMPARE(w.stepInterval[e], fix);
        if (fix)
            QCOMPARE(w.stepInterval[e-1], fix-1);
    }
    for (size_t i = 0; i < out.time.size(); ++i) {
        const Vector3 expected = expectedAcceleration(f, w, out, i);
        QVERIFY((out.acceleration[i]-expected).norm() <= 1e-12*(1+expected.norm()));
    }
}

void FusionKernelTest::imuRateMatchesHeldEndsGraph()
{
    // Spec section 10, "Equivalence", on coarse_maneuver: the one pass against
    // the dense graph with a state at every edge and the fix states and biases
    // held (heldEndsReference()). What the linearization costs is quadratic in
    // the mismatch, which on this fixture is a few 1e-7 m/s; under the
    // datasheet's densities the two differ by more, linearly: by 1.9e-4 (attitude),
    // 8.2e-3 (velocity) and 2.9e-3 (position) of the mismatch, which falls to
    // below 4e-6 with an accelerometer density twenty times the datasheet's.
    // The difference is that of one preintegration of many steps against a
    // chain of one-step factors, in which the accelerometer's noise no longer
    // dominates. The tolerance is linear in the mismatch, 2e-2 of it.
    const WindowFit &f = fixtureFit(QStringLiteral("coarse_maneuver"));
    QVERIFY(f.fit.converged);
    const WindowSeams w = seamsOf(f);
    const ImuRateTrajectory out = reconstructAtImuRate(f.window, f.fit, f.tuning);
    int iterations = 0;
    const NavStates reference = heldEndsReference(f, w, iterations);
    const Mismatch m = largestMismatch(w);
    const StateDifference diff = largestDifference(out, w, reference);
    qInfo() << "coarse_maneuver: largest mismatch: attitude" << m.attitude*180/kPi << "deg, velocity" << m.velocity
            << "m/s, position" << m.position << "m";
    qInfo() << "coarse_maneuver: against the held-ends graph (" << iterations << "iterations,"
            << w.edges.size() << "states): attitude" << diff.attitude*180/kPi << "deg (" << diff.attitude/m.attitude
            << "of the mismatch), velocity" << diff.velocity << "m/s (" << diff.velocity/m.velocity
            << "), position" << diff.position << "m (" << diff.position/m.position << ")";
    // Not vacuous: the fixture's mismatch is well above rounding.
    QVERIFY(m.velocity > 1e-7);
    QVERIFY(m.attitude > 1e-9);
    QVERIFY(diff.attitude <= 2e-2*m.attitude+1e-12);
    QVERIFY(diff.velocity <= 2e-2*m.velocity+1e-12);
    QVERIFY(diff.position <= 2e-2*m.position+1e-12);
}

void FusionKernelTest::imuRateMatchesHeldEndsGraphUnderRotation()
{
    // The same against a recording that rotates, because coarse_maneuver
    // barely does: 3 rad/s about a horizontal axis, IMU 25 Hz, GNSS 5 Hz,
    // exact readings, the true states as the fit. The mismatch is then the
    // integration's own discretization error. Velocity and position hold to
    // 5e-3 of the mismatch (the pass measures 5.7e-4 and 9.8e-4: the chain of
    // one-step factors of coarse_maneuver's test); applying the sharing
    // unmapped misses the velocity by 5-15 % of the mismatch here.
    //
    // The attitude mismatch is at rounding, so a bound relative to it means
    // nothing; the attitude differs from the reference by what tangent
    // preintegration over many steps differs from one-step factors chained,
    // not by linearization. Its bound is absolute: 5e-5 degrees, more than
    // ten times what the pass measures and more than ten times below what the
    // unmapped sharing gives (1.2e-3 degrees).
    //
    // The same recording at 13 Hz (FlySight's IMU rate) and 100 Hz: the
    // published acceleration against the true one, and the rotated reading
    // alone. The integration applies each reading at the attitude of the
    // middle of its step (the library alone would use the start), so what
    // the corrections carry into the published acceleration under rotation
    // is second order in the step's rotation.
    for (const double rate : {13., 25., 100.}) {
        const WindowFit f = tumbleFit(rate);
        const WindowSeams w = seamsOf(f);
        const Mismatch m = largestMismatch(w);
        const ImuRateTrajectory out = reconstructAtImuRate(f.window, f.fit, f.tuning);
        double published = 0, rotated = 0;
        for (size_t i = 0; i < out.time.size(); ++i) {
            const Vector3 truth = Tumble::acceleration(out.time[i]);
            const size_t sample = size_t(std::lower_bound(f.window.imuTime.begin(), f.window.imuTime.end(),
                                                          out.time[i])-f.window.imuTime.begin());
            published = std::max(published, (out.acceleration[i]-truth).norm());
            rotated = std::max(rotated, (out.rotation[i].rotate(f.window.force[sample])+kGravity-truth).norm());
        }
        qInfo() << "tumble at" << rate << "Hz: largest mismatch: attitude" << m.attitude*180/kPi << "deg, velocity"
                << m.velocity << "m/s, position" << m.position << "m; published acceleration vs truth" << published
                << "m/s^2, rotated reading alone" << rotated << "m/s^2";
        if (rate != 25.)
            continue;

        int iterations = 0;
        const NavStates reference = heldEndsReference(f, w, iterations);
        const StateDifference diff = largestDifference(out, w, reference);
        qInfo() << "tumble at 25 Hz: against the held-ends graph (" << iterations << "iterations,"
                << w.edges.size() << "states): attitude" << diff.attitude*180/kPi << "deg (bound 5e-5 deg),"
                << "velocity" << diff.velocity << "m/s (" << diff.velocity/m.velocity << "of the mismatch), position"
                << diff.position << "m (" << diff.position/m.position << ")";
        // Not vacuous: with the mid-step rotation the mismatch is a few mm/s,
        // still far above rounding.
        QVERIFY(m.velocity > 1e-3);
        QVERIFY(diff.velocity <= 5e-3*m.velocity+1e-12);
        QVERIFY(diff.position <= 5e-3*m.position+1e-12);
        QVERIFY(diff.attitude*180/kPi <= 5e-5);
    }
}

void FusionKernelTest::imuRateSharesByNoise()
{
    // Spec section 10, "Sharing by noise", on one interval of
    // boundarySamples(): at rest, identity attitude, no rotation, so the
    // velocity local coordinates are NED. The fitted end is the forward end
    // plus a vertical velocity dv and position dv T/2, a constant acceleration
    // error, for which the velocity correction is dv (t - t0) / T; vertical,
    // so that tilt does not couple (a tilt error moves the velocity
    // horizontally under a vertical force).
    using gtsam::imuBias::ConstantBias;
    const double dv = .05;
    const auto offsetEnd = [dv](const Samples &d, const Tuning &tuning) {
        WindowFit f;
        f.window = d;
        f.tuning = tuning;
        f.fit.values.insert(B(0), ConstantBias());
        f.fit.values.insert(X(0), gtsam::Pose3());
        f.fit.values.insert(V(0), Vector3(0, 0, 0));
        f.fit.values.insert(X(1), gtsam::Pose3());
        f.fit.values.insert(V(1), Vector3(0, 0, 0));
        const gtsam::NavState end = reconstructInterval(d, f.fit, tuning, 0).forward.back();
        const double duration = d.gnssTime[1]-d.gnssTime[0];
        f.fit.values.update(X(1), gtsam::Pose3(end.attitude(), Vector3(end.position()+Vector3(0, 0, dv*duration/2))));
        f.fit.values.update(V(1), Vector3(end.velocity()+Vector3(0, 0, dv)));
        return reconstructInterval(d, f.fit, tuning, 0);
    };
    // The vertical velocity correction at every edge.
    const auto corrections = [](const IntervalReconstruction &r) {
        std::vector<double> u;
        for (size_t j = 0; j < r.edges.size(); ++j)
            u.push_back(r.corrected[j].velocity().z()-r.forward[j].velocity().z());
        return u;
    };

    // Uniform noise: no signal change, so every step's covariance is the
    // density's and the noise per unit time is the same everywhere. The
    // correction grows strictly and is proportional to the elapsed time. Not
    // approximately: a position offset of exactly dv T/2 is the direction of
    // P_n's velocity column, whatever the integration covariance adds to the
    // position alone, so the sharing puts nothing on the position's
    // multiplier (measured 7e-16 of dv; bound 1e-9 of dv).
    const Samples uniform = boundarySamples(Vector3::Zero());
    const IntervalReconstruction r = offsetEnd(uniform, tuningAt(104));
    const std::vector<double> u = corrections(r);
    const double t0 = r.edges.front(), duration = r.edges.back()-t0;
    QCOMPARE(u.front(), 0.);
    double worst = 0;
    for (size_t j = 0; j < u.size(); ++j) {
        if (j)
            QVERIFY2(u[j] > u[j-1], qPrintable(QString::number(j)));
        worst = std::max(worst, std::abs(u[j]-dv*(r.edges[j]-t0)/duration));
    }
    qInfo() << "uniform noise: largest departure from proportional" << worst/dv << "of dv";
    QVERIFY(worst <= 1e-9*dv);

    // Noisy steps: the force steps by 1 m/s^2 (vertically) between samples 49
    // and 50, a change of slope of 1e4 m/s^4 at both, which raises the
    // sampling term of the three sample intervals touching them (48-49,
    // 49-50, 50-51): each step's variance there is about a hundred times the
    // density's. Their increments of the correction are larger than any
    // other step's.
    Samples noisy = boundarySamples(Vector3::Zero());
    for (size_t i = 50; i < noisy.force.size(); ++i)
        noisy.force[i] += Vector3(0, 0, 1);
    const IntervalReconstruction s = offsetEnd(noisy, tuningAt(104));
    const std::vector<double> v = corrections(s);
    std::vector<size_t> touching;
    for (const size_t sample : {48, 49, 50}) {
        const size_t step = size_t(std::find(s.edges.begin(), s.edges.end(), noisy.imuTime[sample])-s.edges.begin());
        QVERIFY(step+1 < s.edges.size());
        QCOMPARE(s.edges[step+1], noisy.imuTime[sample+1]);
        touching.push_back(step);
    }
    double smallestTouching = std::numeric_limits<double>::infinity(), otherwise = 0;
    for (size_t j = 0; j+1 < v.size(); ++j) {
        const double increment = std::abs(v[j+1]-v[j]);
        if (std::find(touching.begin(), touching.end(), j) != touching.end())
            smallestTouching = std::min(smallestTouching, increment);
        else
            otherwise = std::max(otherwise, increment);
    }
    qInfo() << "noisy steps: the smallest increment of the three" << smallestTouching
            << "m/s against at most" << otherwise << "m/s elsewhere";
    QVERIFY(smallestTouching > otherwise);
}

void FusionKernelTest::imuRateZeroMismatchIsForward()
{
    // Spec section 10, "Zero mismatch": fitted states that are the forward
    // predictions interval by interval, so d is zero (to rounding). The pass
    // is then the forward integration, every step correction is zero and the
    // published acceleration is the rotated reading R (f - b_a) + g. Several
    // intervals, a constant non-identity attitude, a non-zero accelerometer
    // bias, a force that changes piecewise-linearly, one fix on a sample.
    //
    // The recording does not rotate, and must not: under rotation a zero
    // mismatch leaves every c_j at minus the rotation lag, the difference
    // between the trapezoid of the edge-rotated readings the step correction
    // subtracts and the mid-step rotation the integration applies
    // (imuRateZeroMismatchUnderRotation).
    //
    // The tolerance on c: the velocities are about 20 m/s, their rounding
    // about 4e-15 m/s, divided by the shortest step (3 ms) 1.3e-12 m/s^2;
    // bound 1e-9.
    using gtsam::imuBias::ConstantBias;
    WindowFit f;
    Samples &d = f.window;
    for (int i = 0; i <= 200; ++i) {
        d.imuTime.push_back(i*.01);
        d.gyro.push_back(Vector3::Zero());
        d.force.push_back(Vector3(.5, -.3, -9.5)+Vector3(.02, -.01, .03)*std::min(i, 80)
                          +Vector3(-.03, .02, .01)*std::max(i-80, 0));
    }
    d.gnssTime = {.037, .5, 1.013, 1.49, 1.963};
    f.tuning = tuningAt(104);
    f.tuning.maxGap = SampleContinuity::holeThreshold(d.imuTime);
    const ConstantBias bias(Vector3(.05, -.08, .12), Vector3::Zero());
    predictFits(f, bias, gtsam::NavState(Rot3::RzRyRx(.3, -.2, 1.1), Vector3(10, -5, -300), Vector3(20, 5, 8)),
                gtsam::Vector9::Zero());

    const WindowSeams w = seamsOf(f);
    double mismatch = 0, largestC = 0;
    for (const IntervalReconstruction &r : w.intervals) {
        mismatch = std::max(mismatch, r.mismatch.cwiseAbs().maxCoeff());
        for (size_t j = 0; j < r.edges.size(); ++j)
            QVERIFY2(sameStateToRounding(r.corrected[j], r.forward[j]), qPrintable(QString::number(j)));
    }
    for (const Vector3 &c : w.stepCorrection)
        largestC = std::max(largestC, c.norm());
    const ImuRateTrajectory out = reconstructAtImuRate(d, f.fit, f.tuning);
    double largestDeparture = 0;
    for (size_t i = 0; i < out.time.size(); ++i) {
        const size_t sample = size_t(std::lower_bound(d.imuTime.begin(), d.imuTime.end(), out.time[i])-d.imuTime.begin());
        const Vector3 rotated = out.rotation[i].rotate(d.force[sample]-bias.accelerometer())+kGravity;
        largestDeparture = std::max(largestDeparture, (out.acceleration[i]-rotated).norm());
    }
    qInfo() << "zero mismatch: largest |d| component" << mismatch << ", largest |c|" << largestC
            << "m/s^2, published acceleration against the rotated reading" << largestDeparture << "m/s^2";
    QCOMPARE(w.intervals.size(), size_t(4));
    QVERIFY(mismatch <= 1e-12);
    QVERIFY(largestC <= 1e-9);
    QVERIFY(largestDeparture <= 1e-9);
}

void FusionKernelTest::imuRateZeroMismatchUnderRotation()
{
    // The rotating half of "zero mismatch": the tumble at 25 Hz, zero bias,
    // with fitted states that are the forward predictions interval by
    // interval, so d is zero to rounding and the pass is the forward
    // integration. The step corrections are then minus the rotation lag,
    // which the test forms from the readings and the corrected attitudes
    // alone: the trapezoid of the edge-rotated readings the step correction
    // subtracts, less the integration's own step, the midpoint reading turned
    // by half the step's rotation and then rotated by the attitude at the
    // step's first edge. That lag is second order in the step's rotation;
    // the test also forms the lag the library's start-of-step rotation would
    // leave, about |omega x f| dt / 2, and requires the integration's to be
    // below a tenth of it.
    //
    // The tolerance on c + lag: the velocities are about 35 m/s, their
    // rounding about 7e-15 m/s, divided by the shortest step (a 13 ms
    // part-step) 5e-13 m/s^2; bound 1e-9.
    using gtsam::imuBias::ConstantBias;
    WindowFit f = tumbleWindow(25);
    const Samples &d = f.window;
    const double t0 = d.gnssTime.front();
    predictFits(f, ConstantBias(), gtsam::NavState(Tumble::attitude(t0), Tumble::position(t0), Tumble::velocity(t0)),
                gtsam::Vector9::Zero());

    const WindowSeams w = seamsOf(f);
    double mismatch = 0;
    for (const IntervalReconstruction &r : w.intervals) {
        mismatch = std::max(mismatch, r.mismatch.cwiseAbs().maxCoeff());
        for (size_t j = 0; j < r.edges.size(); ++j)
            QVERIFY2(sameStateToRounding(r.corrected[j], r.forward[j]), qPrintable(QString::number(j)));
    }
    double largestLag = 0, largestStartLag = 0, largestDeparture = 0;
    for (size_t j = 0; j+1 < w.edges.size(); ++j) {
        const Rot3 from = w.corrected[j].attitude(), to = w.corrected[j+1].attitude();
        const double dt = w.edges[j+1]-w.edges[j], mid = (w.edges[j]+w.edges[j+1])/2;
        const Vector3 trapezoid = .5*(from.rotate(interpolateAt(d.imuTime, d.force, w.edges[j]))
                                      + to.rotate(interpolateAt(d.imuTime, d.force, w.edges[j+1])));
        const Vector3 midReading = interpolateAt(d.imuTime, d.force, mid);
        const Rot3 halfStep = Rot3::Expmap(interpolateAt(d.imuTime, d.gyro, mid)*(dt/2));
        const Vector3 integrated = from.rotate(halfStep.rotate(midReading));
        const Vector3 lag = trapezoid-integrated;
        largestLag = std::max(largestLag, lag.norm());
        largestStartLag = std::max(largestStartLag, (trapezoid-from.rotate(midReading)).norm());
        largestDeparture = std::max(largestDeparture, (w.stepCorrection[j]+lag).norm());
    }
    qInfo() << "zero mismatch under rotation: largest |d| component" << mismatch << ", largest rotation lag"
            << largestLag << "m/s^2 (the start-of-step rotation would leave" << largestStartLag
            << "), step correction plus lag at most" << largestDeparture << "m/s^2";
    QVERIFY(mismatch <= 1e-12);
    QVERIFY(largestStartLag > .1);
    QVERIFY(largestLag < .1*largestStartLag);
    QVERIFY(largestDeparture <= 1e-9);
}

void FusionKernelTest::imuRateAccelerationIntegratesToVelocity_data()
{
    imuRateEndsAreTheFit_data();
}

void FusionKernelTest::imuRateAccelerationIntegratesToVelocity()
{
    // Spec section 10, "Consistency": the published acceleration integrated
    // by the kernel's rule (the midpoint value of the piecewise-linear signal
    // over each step between samples) against the published velocity change,
    // over every run of 1, 2, 5 and 20 sample steps and over the whole axis.
    // The published acceleration spreads each correction over the two steps
    // beside a sample, so a run gains or loses a quarter step of the jump of
    // the corrections at each end: the bound from sample a to b is
    // dt_a/4 |c_before(a) - c_after(a)| + dt_b/4 |c_after(b) - c_before(b)|,
    // plus, for a sample step that contains a fix, dt/2 times the largest
    // difference between the corrections of the part-steps inside it, and
    // the kink of the rotated reading at the fix: the part-steps integrate
    // the bias-corrected reading rotated by the attitude at each edge,
    // through the fix's own, while the kernel's rule takes the straight line
    // between the samples, a difference of the length of the part-steps'
    // trapezoids against the sample step's. It is tight (the ratio is 1 to
    // 1e-5 on the worst runs), hence 1.01 x the bound + 1e-12 m/s. The
    // published acceleration is also checked to be the formula of section 6
    // over the seam's corrections.
    QFETCH(QString, name);
    const WindowFit &f = fixtureFit(name);
    QVERIFY(f.fit.converged);
    const WindowSeams w = seamsOf(f);
    const ImuRateTrajectory out = reconstructAtImuRate(f.window, f.fit, f.tuning);
    const size_t n = out.time.size();
    QVERIFY(n > 20);

    std::vector<size_t> edge(n);
    for (size_t i = 0; i < n; ++i) {
        edge[i] = edgeAt(w, out.time[i]);
        const Vector3 expected = expectedAcceleration(f, w, out, i);
        QVERIFY((out.acceleration[i]-expected).norm() <= 1e-12*(1+expected.norm()));
    }
    const Vectors &c = w.stepCorrection;
    // A sample on the window's first edge has one step beside it and no jump.
    const auto before = [&](size_t i) { return edge[i] ? c[edge[i]-1] : c[edge[i]]; };
    const auto after = [&](size_t i) { return c[edge[i]]; };

    // The reading at edge `e` divided by the fitted scale and bias-corrected,
    // rotated by the corrected attitude there, with the bias of the interval
    // of step `step` (the reconstruction's trapezoid of that step).
    const auto rotatedReading = [&](size_t e, size_t step) {
        const Vector3 reading = interpolateAt(f.window.imuTime, f.window.force, w.edges[e])
                                    .cwiseQuotient(f.fit.scale.head<3>());
        return Vector3(w.corrected[e].attitude().rotate(reading-w.intervals[w.stepInterval[step]].bias.accelerometer()));
    };

    // Per step between samples: the integral by the kernel's rule and the
    // fix term (the corrections' spread and the rotated reading's kink).
    Vectors integral(n-1);
    std::vector<double> fixTerm(n-1, 0.);
    for (size_t i = 0; i+1 < n; ++i) {
        const double dt = out.time[i+1]-out.time[i];
        integral[i] = interpolateAt(out.time, out.acceleration, (out.time[i]+out.time[i+1])/2)*dt;
        double spread = 0;
        Vector3 parts = Vector3::Zero();
        for (size_t q = edge[i]; q < edge[i+1]; ++q) {
            for (size_t p = edge[i]; p < edge[i+1]; ++p)
                spread = std::max(spread, (c[q]-c[p]).norm());
            parts += (w.edges[q+1]-w.edges[q])/2*(rotatedReading(q, q)+rotatedReading(q+1, q));
        }
        const Vector3 whole = dt/2*(rotatedReading(edge[i], edge[i])+rotatedReading(edge[i+1], edge[i+1]-1));
        fixTerm[i] = dt/2*spread + (edge[i+1]-edge[i] > 1 ? (parts-whole).norm() : 0.);
    }

    const std::vector<size_t> runs{1, 2, 5, 20, n-1};
    double worstRatio = 0, wholeError = 0;
    for (const size_t length : runs) {
        for (size_t a = 0; a+length < n; ++a) {
            const size_t b = a+length;
            Vector3 sum = Vector3::Zero();
            double bound = (out.time[a+1]-out.time[a])/4*(before(a)-after(a)).norm()
                         + (out.time[b]-out.time[b-1])/4*(after(b)-before(b)).norm();
            for (size_t i = a; i < b; ++i) {
                sum += integral[i];
                bound += fixTerm[i];
            }
            const double error = (sum-(out.velocity[b]-out.velocity[a])).norm();
            if (bound > 0)
                worstRatio = std::max(worstRatio, error/bound);
            if (length == n-1)
                wholeError = error;
            QVERIFY2(error <= 1.01*bound+1e-12,
                     qPrintable(QStringLiteral("run %1..%2: error %3, bound %4").arg(a).arg(b).arg(error).arg(bound)));
        }
    }
    qInfo() << name << ": worst error over bound" << worstRatio << ", whole axis error" << wholeError << "m/s";
}

void FusionKernelTest::imuRateIsWhatTheFitPublishes_data()
{
    imuRateEndsAreTheFit_data();
}

void FusionKernelTest::imuRateIsWhatTheFitPublishes()
{
    // Spec sections 6, 7 and 9: what the pipeline publishes is the IMU-rate
    // pass on the fit, bit for bit, not something like it. The twenty-one
    // channels of runPipeline() against reconstructAtImuRate() on this
    // executable's own fit of the fixture (fixtureFit(), the pipeline's
    // stages in its order), with its own covariance step composed and its
    // own widening, through fillOutputChannels() (clause 33 of the
    // specification of 1001-1065: the four accuracies are the fit's), and the
    // four numbers of the diagnostics against the pass's summaries. Section 6
    // and decision 12: the published time axis is the fixture's IMU samples in
    // [first fix, last fix) of the window, in the number imu_outputs says.
    QFETCH(QString, name);
    const Fusion::Channels channels = toChannels(fusionFixture(name));
    const Fusion::Result result = runPipeline(channels, Tuning{}, Checkpoint());
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
    const QJsonObject diagnostics = diagnosticsOf(result);

    const WindowFit &f = fixtureFit(name);
    QVERIFY(f.fit.converged);
    const FitCovariance covariance = fitCovariance(f.fit, f.window.gnssTime.size());
    QVERIFY(covariance.computed);
    const ImuRateTrajectory out = reconstructAtImuRate(f.window, f.fit, f.tuning, &covariance);
    std::vector<double> widenings;
    for (const double factor : wideningFactors(f.window.gnssTime, f.fit.residuals, out.time))
        widenings.push_back(widening(factor));
    Fusion::Result expected;
    fillOutputChannels(out, widenings, prepareInput(channels).epoch, expected);
    QCOMPARE(fusionChannelNames().size(), 21);
    for (const QString &channel : fusionChannelNames()) {
        QVERIFY(!fusionChannel(result, channel).isEmpty());
        QVERIFY2(sameBitsEverywhere(fusionChannel(result, channel), fusionChannel(expected, channel)),
                 qPrintable(channel));
    }
    QVERIFY(diagnostics.value("max_endpoint_correction_deg").toDouble() == out.maxEndpointCorrectionDeg);
    QVERIFY(diagnostics.value("max_velocity_mismatch_m_s").toDouble() == out.maxVelocityMismatch);
    QVERIFY(diagnostics.value("max_step_correction_m_s2").toDouble() == out.maxStepCorrection);
    QVERIFY(diagnostics.value("max_step_correction_time_s").toDouble() == out.maxStepCorrectionTime);

    verifyTimeAxis(result.time, expectedTimeAxis(channels, diagnostics), diagnostics);
}

void FusionKernelTest::imuRateAxisWhenGnssIsFasterThanImu()
{
    // Spec sections 6 and 10: the time axis is the IMU samples whatever the
    // GNSS rate, including a GNSS rate above the IMU's. 40 s level flight
    // under a horizontal acceleration that turns (so the heading is
    // observable), 25 degC; GNSS 25 Hz from .02 s, IMU 12.5 Hz (the rate it
    // states, with +/-16 g and +/-2000 deg/s), so every other fix interval
    // holds no IMU sample and is one integration step. The readings are the
    // exact force rounded onto the stated lattice, as a FlySight's are. The
    // fit succeeds and publishes the 499 IMU samples from .08 to 39.92 s,
    // every channel finite, and the reconstruction's account.
    const double base = 1700000000.;
    const double step = 16./32768*9.80665;   // the +/-16 g lattice, m/s^2
    const auto onLattice = [step](double value) { return std::round(value/step)*step; };
    const auto acceleration = [](double t) { return Vector3(.5*std::cos(.1*t), .5*std::sin(.1*t), 0); };
    const auto velocity = [](double t) { return Vector3(20+5*std::sin(.1*t), 10-5*std::cos(.1*t), 3); };
    const auto position = [](double t) {
        return Vector3(20*t-50*std::cos(.1*t)+50, 10*t-50*std::sin(.1*t), 3*t);
    };
    Fusion::Channels c;
    for (int k = 0; k <= 999; ++k) {
        const double t = .02+.04*k;
        const Vector3 p = position(t), v = velocity(t);
        c.gnssTime.append(base+t);
        c.north.append(p.x());
        c.east.append(p.y());
        c.down.append(p.z());
        c.velN.append(v.x());
        c.velE.append(v.y());
        c.velD.append(v.z());
        c.hAcc.append(1);
        c.vAcc.append(1.5);
        c.sAcc.append(.1);
    }
    for (int i = 0; i <= 500; ++i) {
        const double t = .08*i;
        const Vector3 force = acceleration(t)-kTestGravity;
        c.imuTime.append(base+t);
        c.ax.append(onLattice(force.x()));
        c.ay.append(onLattice(force.y()));
        c.az.append(onLattice(force.z()));
        c.wx.append(0);
        c.wy.append(0);
        c.wz.append(0);
        c.imuTemperature.append(kFixtureTemperatureDegC);
    }
    c.originIndex = 0;
    c.imuConfiguration = fixtureNoise(12.5).configuration;

    // Not vacuous: fix intervals without an IMU sample inside.
    const Samples recording = prepareInput(c).recording;
    int withoutSample = 0;
    for (size_t k = 0; k+1 < recording.gnssTime.size(); ++k) {
        const auto after = std::upper_bound(recording.imuTime.begin(), recording.imuTime.end(), recording.gnssTime[k]);
        if (after == recording.imuTime.end() || *after >= recording.gnssTime[k+1])
            ++withoutSample;
    }
    QVERIFY(withoutSample > 400);

    const Fusion::Result result = runPipeline(c, Tuning{}, Checkpoint());
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
    const QJsonObject diagnostics = diagnosticsOf(result);
    const QVector<double> axis = expectedTimeAxis(c, diagnostics);
    QCOMPARE(axis.size(), qsizetype(499));
    QVERIFY(std::abs(axis.front()-base-.08) < 1e-6);
    QVERIFY(std::abs(axis.back()-base-39.92) < 1e-6);
    verifyTimeAxis(result.time, axis, diagnostics);
    for (const QString &channel : fusionChannelNames()) {
        const QVector<double> &values = fusionChannel(result, channel);
        QCOMPARE(values.size(), axis.size());
        QVERIFY2(std::all_of(values.begin(), values.end(), [](double v) { return std::isfinite(v); }),
                 qPrintable(channel));
    }
    for (const char *key : {"max_endpoint_correction_deg", "max_velocity_mismatch_m_s", "max_step_correction_m_s2",
                            "max_step_correction_time_s"}) {
        const QJsonValue value = diagnostics.value(QLatin1String(key));
        QVERIFY2(value.isDouble() && std::isfinite(value.toDouble()), key);
    }
    qInfo() << "GNSS faster than IMU:" << withoutSample << "of" << recording.gnssTime.size()-1
            << "fix intervals without an IMU sample; endpoint correction"
            << diagnostics.value("max_endpoint_correction_deg").toDouble() << "deg, velocity mismatch"
            << diagnostics.value("max_velocity_mismatch_m_s").toDouble() << "m/s, step correction"
            << diagnostics.value("max_step_correction_m_s2").toDouble() << "m/s^2";
}

// ---- The accuracy (the specification of 1001-1065, section 7) -----------------------

namespace {

/// The success fixtures and the initializer's recordings: every fit whose
/// covariance the accuracy tests read.
const char *const kAccuracyFixtures[] = {"coarse_linear", "coarse_maneuver", "stationary_spin", "bridged_hole",
                                         "motion_start", "rest_throughout", "sacc_anchor", "drifting_bias",
                                         "scale_recording"};

/// The four accuracy channels, by golden column name.
const QStringList kAccuracyChannels{QStringLiteral("headingAcc"), QStringLiteral("tiltAcc"),
                                    QStringLiteral("accHAcc"), QStringLiteral("accDAcc")};

/// scale_recording with its GNSS accuracies at the standard deviations of its
/// noise (uniform, amplitudes .2 m and .03 m/s: .2/sqrt(3) and .03/sqrt(3)),
/// divided by `ratio` over [20, 40) s of the fixture's own time: a stretch
/// whose sigmas are understated by that ratio.
Fusion::Channels scaleRecordingAtItsNoise(double ratio)
{
    Fusion::Channels c = toChannels(initializerFixture(QStringLiteral("scale_recording")));
    for (qsizetype j = 0; j < c.gnssTime.size(); ++j) {
        const double t = c.gnssTime[j]-1700000000.;
        const double divide = t >= 20 && t < 40 ? ratio : 1.;
        c.hAcc[j] = .2/std::sqrt(3.)/divide;
        c.vAcc[j] = .2/std::sqrt(3.)/divide;
        c.sAcc[j] = .03/std::sqrt(3.)/divide;
    }
    return c;
}

/// The median of `values` (the upper one of an even count).
double medianOf(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    return values[values.size()/2];
}

} // namespace

void FusionKernelTest::covarianceMatchesJointMarginals()
{
    // Clause 25 (as settled), criterion 2: the covariance step's joint
    // covariance of every adjacent pair z_k = (x_k, x_k+1, B, T, S), from the
    // clique marginals of one factorization with the heading prior, against
    // the library's joint marginals of the reported graph (no prior): on
    // coarse_maneuver at every pair, on drifting_bias (1 Hz fixes, 201
    // states, where the covariance-form recursion fails) at nodes 0, N/2 and
    // N-2. Within 1e-6 relative (Frobenius); the prior moves a determined
    // heading by sigma^2 / (2 x 1000^2) of itself, far below that.
    for (const auto &[name, everyPair] : {std::pair<const char *, bool>{"coarse_maneuver", true},
                                          std::pair<const char *, bool>{"drifting_bias", false}}) {
        const WindowFit &f = fixtureFit(QLatin1String(name));
        QVERIFY2(f.fit.converged, name);
        const FitCovariance &c = fixtureCovariance(QLatin1String(name));
        QVERIFY2(c.computed, name);
        const size_t n = f.window.gnssTime.size();
        QCOMPARE(c.node.size(), n);
        QCOMPARE(c.next.size(), n-1);
        QCOMPARE(c.global.size(), n);
        std::vector<size_t> nodes;
        if (everyPair) {
            for (size_t k = 0; k+1 < n; ++k)
                nodes.push_back(k);
        } else {
            nodes = {0, n/2, n-2};
        }
        const gtsam::Marginals marginals(f.fit.graph, f.fit.values, gtsam::Marginals::QR);
        double worst = 0;
        for (const size_t k : nodes) {
            const gtsam::Matrix reference = jointOf(marginals, pairKeys(k), f.fit.values);
            const double difference = frobeniusRelative(c.pair(k), reference);
            worst = std::max(worst, difference);
            QVERIFY2(difference <= 1e-6, qPrintable(QStringLiteral("%1 pair %2: %3").arg(QLatin1String(name)).arg(k)
                                                        .arg(difference)));
        }
        qInfo() << name << ": the covariance step against the joint marginals at" << nodes.size()
                << "pairs: largest relative difference" << worst;
    }
}

void FusionKernelTest::firstNodeHeadingIsTheHeadingCheck()
{
    // Clauses 25 and 57, criterion 2: on every success fixture and every
    // initializer recording, the heading accuracy of the first node from the
    // covariance step equals the heading check (yawSigmaDeg() on the reported
    // graph, which has no prior) within 1e-5 relative, or both are at the
    // 180-degree cap; and so does the first published sample, unwidened,
    // within 1e-3, where it lies within one IMU interval of fix 0 (the step
    // chain from the fix adds what that part step carries).
    for (const char *name : kAccuracyFixtures) {
        const WindowFit &f = fixtureFit(QLatin1String(name));
        QVERIFY2(f.fit.converged, name);
        const FitCovariance &c = fixtureCovariance(QLatin1String(name));
        QVERIFY2(c.computed, name);
        const gtsam::Matrix3 R = f.fit.values.at<gtsam::Pose3>(X(0)).rotation().matrix();
        const double node = attitudeAccuracy(R*c.node[0].block<3, 3>(0, 0)*R.transpose()).heading;
        const double check = yawSigmaDeg(f.fit.graph, f.fit.values, X(0));
        const auto agrees = [&](double got, double relative) {
            return (got == kYawSigmaCapDeg && check == kYawSigmaCapDeg) || withinRelative(got, check, relative);
        };
        const ImuRateTrajectory &out = fixtureTrajectory(QLatin1String(name));
        QVERIFY(!out.headingAcc.empty());
        const double after = out.time.front()-f.window.gnssTime.front();
        qInfo() << name << ": first node heading" << node << "deg, the heading check" << check
                << "deg; first sample" << out.headingAcc.front() << "deg," << after << "s after fix 0";
        QVERIFY2(agrees(node, 1e-5), name);
        if (after < SampleContinuity::nominalInterval(f.window.imuTime))
            QVERIFY2(agrees(out.headingAcc.front(), 1e-3), name);
    }
}

void FusionKernelTest::sampleCovarianceMatchesTheEdgeGraph()
{
    // Clauses 26, 29 and 58, criterion 3: the composition at every tenth
    // sample of coarse_maneuver against a graph with a state at every
    // integration edge (heldEndsReference()'s construction with the fixes,
    // the biases, the slope and the scale free: GNSS factors at the fix
    // edges, a one-step scaled IMU factor across every step at its interval's
    // bias and the fit's scale, and the fit's three priors), solved to
    // convergence: the attitude covariance in the navigation frame and the
    // four unwidened accuracies, against the library's joint marginals of
    // (X(j), V(j), B(0), S(0)) at the sample's edge, within 1e-5 relative.
    using gtsam::imuBias::ConstantBias;
    const QString name = QStringLiteral("coarse_maneuver");
    const WindowFit &f = fixtureFit(name);
    QVERIFY(f.fit.converged);
    const ImuRateTrajectory &out = fixtureTrajectory(name);
    QCOMPARE(out.attitudeCovariance.size(), out.time.size());
    const WindowSeams w = seamsOf(f);

    const ConstantBias bias = f.fit.values.at<ConstantBias>(B(0));
    const GyroBiasModel &model = f.fit.biasModel;
    gtsam::NonlinearFactorGraph graph;
    gtsam::Values values;
    for (size_t j = 0; j < w.edges.size(); ++j) {
        values.insert(X(j), w.corrected[j].pose());
        values.insert(V(j), Vector3(w.corrected[j].velocity()));
    }
    values.insert(B(0), bias);
    values.insert(T(0), f.fit.gyroBiasSlope);
    values.insert(S(0), f.fit.scale);
    for (size_t k = 0; k < w.fixEdge.size(); ++k) {
        graph.emplace_shared<gtsam::GPSFactor>(X(w.fixEdge[k]), f.window.position[k],
                                               gtsam::noiseModel::Diagonal::Sigmas(f.window.positionSigma[k]));
        graph.emplace_shared<gtsam::PriorFactor<Vector3>>(V(w.fixEdge[k]), f.window.velocity[k],
                                                          gtsam::noiseModel::Diagonal::Sigmas(f.window.velocitySigma[k]));
    }
    for (size_t j = 0; j+1 < w.edges.size(); ++j) {
        const size_t k = w.stepInterval[j];
        gtsam::Matrix96 scaleJacobian;
        const auto pim = preintegrateImu(f.window, w.edges[j], w.edges[j+1],
                                         intervalBias(f.window, k, bias, f.fit.gyroBiasSlope, model), f.fit.scale,
                                         f.tuning.noise, ImuStepObserver(), &scaleJacobian);
        graph.emplace_shared<ScaledImuFactor>(X(j), V(j), X(j+1), V(j+1), B(0), T(0), S(0), pim,
                                              temperatureAtFix(f.window, k)-model.tRef, scaleJacobian, f.fit.scale);
    }
    // The bias, slope and scale priors, the reported graph's last three factors.
    for (size_t i = f.fit.graph.size()-3; i < f.fit.graph.size(); ++i)
        graph.push_back(f.fit.graph.at(i));
    gtsam::LevenbergMarquardtParams params;
    params.setLinearSolverType("MULTIFRONTAL_QR");
    gtsam::LevenbergMarquardtOptimizer optimizer(graph, values, params);
    int iterations = 0;
    for (; iterations < 100; ++iterations) {
        const double before = optimizer.error();
        optimizer.iterate();
        if (before-optimizer.error() <= 1e-14*std::max(1., before))
            break;
    }
    const gtsam::Values solution = optimizer.values();
    const gtsam::Marginals marginals(graph, solution, gtsam::Marginals::QR);
    const ConstantBias solvedBias = solution.at<ConstantBias>(B(0));
    const Vector3 solvedScale = solution.at<Vector6>(S(0)).head<3>();

    double worst[5] = {0, 0, 0, 0, 0};
    size_t compared = 0;
    for (size_t i = 0; i < out.time.size(); i += 10, ++compared) {
        const size_t j = edgeAt(w, out.time[i]);
        const size_t sample = size_t(std::lower_bound(f.window.imuTime.begin(), f.window.imuTime.end(), out.time[i])
                                     -f.window.imuTime.begin());
        // X(j) 0..5 (its rotation 0..2), V(j) 6..8, B(0) 9..14 (the
        // accelerometer's 9..11), S(0) 15..20 (the accelerometer's 15..17).
        const gtsam::Matrix joint = jointOf(marginals, {X(j), V(j), B(0), S(0)}, solution);
        gtsam::Matrix9 reference;
        const int at[3] = {0, 9, 15};
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b)
                reference.block<3, 3>(3*a, 3*b) = joint.block<3, 3>(at[a], at[b]);
        }
        const Rot3 attitude = solution.at<gtsam::Pose3>(X(j)).rotation();
        const gtsam::Matrix3 navigation = attitude.matrix()*reference.block<3, 3>(0, 0)*attitude.matrix().transpose();
        const AttitudeAccuracy expectedAttitude = attitudeAccuracy(navigation);
        const AccelerationAccuracy expectedAcceleration = accelerationAccuracy(
            reference, attitude, f.window.force[sample], solvedScale, solvedBias.accelerometer(), out.acceleration[i],
            f.tuning.noise.accelerometer.sampleSigma);
        const double differences[5] = {
            frobeniusRelative(out.attitudeCovariance[i], navigation),
            std::abs(out.headingAcc[i]-expectedAttitude.heading)/expectedAttitude.heading,
            std::abs(out.tiltAcc[i]-expectedAttitude.tilt)/expectedAttitude.tilt,
            std::abs(out.accHAcc[i]-expectedAcceleration.horizontal)/expectedAcceleration.horizontal,
            std::abs(out.accDAcc[i]-expectedAcceleration.vertical)/expectedAcceleration.vertical};
        for (int d = 0; d < 5; ++d) {
            worst[d] = std::max(worst[d], differences[d]);
            QVERIFY2(differences[d] <= 1e-5, qPrintable(QStringLiteral("sample %1, quantity %2: %3").arg(i).arg(d)
                                                            .arg(differences[d])));
        }
    }
    qInfo() << name << ": the composition against the edge graph (" << w.edges.size() << "states," << iterations
            << "iterations) at" << compared << "samples: attitude covariance" << worst[0] << ", heading" << worst[1]
            << ", tilt" << worst[2] << ", accHAcc" << worst[3] << ", accDAcc" << worst[4];
    QVERIFY(compared >= 50);
}

void FusionKernelTest::sampleOnAFixHasTheFixMarginal()
{
    // Clause 26, criterion 3: a sample exactly on a fix (sacc_anchor logs
    // 12.5 Hz against 1 Hz fixes on whole seconds, so every other fix is a
    // sample) has P_0 = 0 and Psi_0 = I: its attitude covariance is the fix's
    // marginal from the covariance step, rotated into the navigation frame,
    // and its accuracies are the formulas' on the fix's joint of attitude,
    // accelerometer bias and scale, to rounding.
    const QString name = QStringLiteral("sacc_anchor");
    const WindowFit &f = fixtureFit(name);
    QVERIFY(f.fit.converged);
    const FitCovariance &c = fixtureCovariance(name);
    QVERIFY(c.computed);
    const ImuRateTrajectory &out = fixtureTrajectory(name);
    const Vector3 accBias = f.fit.values.at<gtsam::imuBias::ConstantBias>(B(0)).accelerometer();
    size_t onFixes = 0;
    double worst = 0;
    for (size_t i = 0; i < out.time.size(); ++i) {
        const auto fix = std::lower_bound(f.window.gnssTime.begin(), f.window.gnssTime.end(), out.time[i]);
        if (fix == f.window.gnssTime.end() || *fix != out.time[i])
            continue;
        ++onFixes;
        const size_t k = size_t(fix-f.window.gnssTime.begin());
        const size_t sample = size_t(std::lower_bound(f.window.imuTime.begin(), f.window.imuTime.end(), out.time[i])
                                     -f.window.imuTime.begin());
        const Rot3 attitude = f.fit.values.at<gtsam::Pose3>(X(k)).rotation();
        const gtsam::Matrix3 navigation = attitude.matrix()*c.node[k].block<3, 3>(0, 0)*attitude.matrix().transpose();
        const AttitudeAccuracy expectedAttitude = attitudeAccuracy(navigation);
        const AccelerationAccuracy expectedAcceleration = accelerationAccuracy(
            fixAttitudeBiasScale(c, k), attitude, f.window.force[sample], f.fit.scale.head<3>(), accBias,
            out.acceleration[i], f.tuning.noise.accelerometer.sampleSigma);
        const double difference = frobeniusRelative(out.attitudeCovariance[i], navigation);
        worst = std::max(worst, difference);
        QVERIFY2(difference <= 1e-9, qPrintable(QStringLiteral("fix %1: %2").arg(k).arg(difference)));
        QVERIFY2(withinRelative(out.headingAcc[i], expectedAttitude.heading, 1e-9), qPrintable(QString::number(k)));
        QVERIFY2(withinRelative(out.tiltAcc[i], expectedAttitude.tilt, 1e-9), qPrintable(QString::number(k)));
        QVERIFY2(withinRelative(out.accHAcc[i], expectedAcceleration.horizontal, 1e-9), qPrintable(QString::number(k)));
        QVERIFY2(withinRelative(out.accDAcc[i], expectedAcceleration.vertical, 1e-9), qPrintable(QString::number(k)));
    }
    qInfo() << name << ":" << onFixes << "samples on a fix; largest relative difference of the attitude covariance"
            << worst;
    QVERIFY(onFixes > 100);
}

void FusionKernelTest::attitudeAccuracyFollowsTheNavigationFrame()
{
    // Clause 28, criterion 4: heading is the square root of the navigation
    // frame's vertical element and tilt of the sum of the two horizontal
    // ones, in degrees; the off-diagonal elements do not enter.
    const double degrees = 180/kPi;
    gtsam::Matrix3 navigation;
    navigation << 4e-4, 1e-4, -2e-5,
                  1e-4, 9e-4, 3e-5,
                  -2e-5, 3e-5, 1e-4;
    const AttitudeAccuracy a = attitudeAccuracy(navigation);
    QVERIFY(withinRelative(a.heading, std::sqrt(1e-4)*degrees, 1e-15));
    QVERIFY(withinRelative(a.tilt, std::sqrt(13e-4)*degrees, 1e-15));

    // The attitude is perturbed on the right, R Exp(phi) with phi in the
    // body frame, so its covariance enters the navigation frame as
    // R Sigma R^T. Pitched up a quarter turn, the body's x axis is the
    // vertical: the heading is the body x's sigma and the tilt the other two's.
    const Rot3 pitched = Rot3::Ry(kPi/2);
    const gtsam::Matrix3 body = Vector3(1e-6, 4e-6, 9e-6).asDiagonal();
    const AttitudeAccuracy p = attitudeAccuracy(pitched.matrix()*body*pitched.matrix().transpose());
    QVERIFY(withinRelative(p.heading, 1e-3*degrees, 1e-12));
    QVERIFY(withinRelative(p.tilt, std::sqrt(13e-6)*degrees, 1e-12));

    // The cap: 180 degrees for a variance above pi^2 rad^2, and for one that
    // is not a number, infinite or negative; each of the two alone (the
    // tilt's the sum of its two elements, here the north one with east zero).
    const double NaN = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    for (const double variance : {kPi*kPi*1.0001, 1e6, NaN, infinity, -1e-6}) {
        gtsam::Matrix3 m = navigation;
        m(2, 2) = variance;
        QCOMPARE(attitudeAccuracy(m).heading, kYawSigmaCapDeg);
        QCOMPARE(attitudeAccuracy(m).tilt, a.tilt);
        m = navigation;
        m(0, 0) = variance;
        m(1, 1) = 0;
        QCOMPARE(attitudeAccuracy(m).tilt, kYawSigmaCapDeg);
        QCOMPARE(attitudeAccuracy(m).heading, a.heading);
    }
    QCOMPARE(kYawSigmaCapDeg, 180.);
}

void FusionKernelTest::accelerationAccuracyFollowsItsPropagation()
{
    // Clauses 29, 35 and 58, criterion 4: the propagation through
    // a = R (f / s - b) + g on inputs with known answers, the accelerometer's
    // per-sample sigma zero unless stated.
    const double g = 9.80665;
    const gtsam::Matrix3 zero = gtsam::Matrix3::Zero();
    const Vector3 noBias = Vector3::Zero(), unitScale = Vector3::Ones();
    const Vector3 atRest(0, 0, -g);

    // The per-sample noise alone, isotropic.
    AccelerationAccuracy r = accelerationAccuracy(blockDiagonal(zero, zero, zero), Rot3(), atRest, unitScale, noBias,
                                                  Vector3(2, 0, 0), .003);
    QVERIFY(withinRelative(r.horizontal, .003, 1e-15));
    QVERIFY(withinRelative(r.vertical, .003, 1e-15));

    // The bias alone, rotated into the navigation frame: turned a quarter
    // turn about the vertical, the body's x axis points east and its y axis
    // south, so the north variance is the body y's and the east the body x's.
    const gtsam::Matrix3 biasVariance = Vector3(1e-4, 4e-4, 9e-4).asDiagonal();
    r = accelerationAccuracy(blockDiagonal(zero, biasVariance, zero), Rot3::Rz(kPi/2), atRest, unitScale, noBias,
                             Vector3(2, 0, 0), 0);
    QVERIFY(withinRelative(r.horizontal, .02, 1e-12));
    QVERIFY(withinRelative(r.vertical, .03, 1e-12));
    r = accelerationAccuracy(blockDiagonal(zero, biasVariance, zero), Rot3::Rz(kPi/2), atRest, unitScale, noBias,
                             Vector3(0, -2, 0), 0);
    QVERIFY(withinRelative(r.horizontal, .01, 1e-12));

    // The tilt alone: at rest, an attitude error about a horizontal axis
    // turns gravity's reaction into the horizontal, across that axis, by
    // g sigma. Level, a body x error moves the force east; turned a quarter
    // turn about the vertical, the body x axis points east and the force
    // moves north.
    const gtsam::Matrix3 aboutBodyX = Vector3(1e-6, 0, 0).asDiagonal();
    r = accelerationAccuracy(blockDiagonal(aboutBodyX, zero, zero), Rot3(), atRest, unitScale, noBias,
                             Vector3(0, 1, 0), 0);
    QVERIFY(withinRelative(r.horizontal, g*1e-3, 1e-12));
    QVERIFY(r.vertical <= 1e-15);
    r = accelerationAccuracy(blockDiagonal(aboutBodyX, zero, zero), Rot3::Rz(kPi/2), atRest, unitScale, noBias,
                             Vector3(1, 0, 0), 0);
    QVERIFY(withinRelative(r.horizontal, g*1e-3, 1e-12));

    // The scale alone: a reading f on a factor s moves by f / s^2 per unit of s.
    const Vector3 reading(3, -2, -9), scale(1.02, .99, 1.01), bias(.05, 0, 0);
    const gtsam::Matrix3 scaleVariance = Vector3(1e-4, 1e-4, 1e-4).asDiagonal();
    r = accelerationAccuracy(blockDiagonal(zero, zero, scaleVariance), Rot3(), reading, scale, bias,
                             Vector3(2.9, 0, 0), 0);
    QVERIFY(withinRelative(r.horizontal, 3/(1.02*1.02)*1e-2, 1e-12));
    QVERIFY(withinRelative(r.vertical, 9/(1.01*1.01)*1e-2, 1e-12));

    // Under a horizontal force a heading error moves the force across it,
    // never along it and never vertically: the directional accuracy is the
    // same whatever the heading variance, the cap's and beyond included.
    const Vector3 northForce(3, 0, -g);
    const gtsam::Matrix3 smallBias = Vector3(1e-4, 1e-4, 1e-4).asDiagonal();
    const AccelerationAccuracy level = accelerationAccuracy(blockDiagonal(zero, smallBias, zero), Rot3(), northForce,
                                                            unitScale, noBias, Vector3(3, 0, 0), .003);
    QVERIFY(withinRelative(level.horizontal, std::sqrt(1e-4+9e-6), 1e-12));
    for (const double headingVariance : {1e-2, 1., kPi*kPi, 1e6}) {
        const gtsam::Matrix3 heading = Vector3(0, 0, headingVariance).asDiagonal();
        const AccelerationAccuracy turned = accelerationAccuracy(blockDiagonal(heading, smallBias, zero), Rot3(),
                                                                 northForce, unitScale, noBias, Vector3(3, 0, 0), .003);
        QVERIFY2(turned.horizontal == level.horizontal, qPrintable(QString::number(headingVariance)));
        QVERIFY2(turned.vertical == level.vertical, qPrintable(QString::number(headingVariance)));
    }

    // The principal-value fallback: where the horizontal acceleration is
    // below its directional accuracy, the larger horizontal principal value;
    // where it has no horizontal part, the same.
    const gtsam::Matrix3 unequalBias = Vector3(.01, .04, 0).asDiagonal();
    const auto horizontal = [&](const Vector3 &published) {
        return accelerationAccuracy(blockDiagonal(zero, unequalBias, zero), Rot3(), atRest, unitScale, noBias,
                                    published, 0).horizontal;
    };
    QVERIFY(withinRelative(horizontal(Vector3(.15, 0, 0)), .1, 1e-12));
    QVERIFY(withinRelative(horizontal(Vector3(.1, 0, 0)), .1, 1e-12));
    QVERIFY(withinRelative(horizontal(Vector3(.05, 0, 0)), .2, 1e-12));
    QVERIFY(withinRelative(horizontal(Vector3(0, 0, -g)), .2, 1e-12));
    QVERIFY(withinRelative(horizontal(Vector3(0, .3, 0)), .2, 1e-12));

    // The yaw cap inside the propagation: a heading variance above pi^2
    // enters as pi^2. Under a 3 m/s^2 north force with no horizontal
    // acceleration published, the fallback reads 3 pi, not 3 sqrt(1e6);
    // below the cap the variance enters whole.
    for (const auto &[headingVariance, expected] : {std::pair<double, double>{4., 6.},
                                                    std::pair<double, double>{1e6, 3*kPi}}) {
        const gtsam::Matrix3 heading = Vector3(0, 0, headingVariance).asDiagonal();
        r = accelerationAccuracy(blockDiagonal(heading, zero, zero), Rot3(), northForce, unitScale, noBias,
                                 Vector3::Zero(), 0);
        QVERIFY2(withinRelative(r.horizontal, expected, 1e-12), qPrintable(QString::number(r.horizontal)));
        QVERIFY(r.vertical == 0);
    }
}

void FusionKernelTest::wideningWindowAndFactor()
{
    // Clause 31 (as settled), criterion 5, on hand-built residuals: the
    // window of a sample is the fixes within 2.5 s of it (both bounds
    // included) and at least the two around it; the factor is the window's
    // position and velocity residuals and the IMU residuals between its
    // fixes (an IMU residual's node is the later fix), over 6N - 9; the
    // priors are not the window's. Every number is an integer, so every sum
    // is exact and so is every factor.
    QCOMPARE(kWideningHalfWidthS, 2.5);
    std::vector<double> fixes;
    std::vector<FactorResidual> residuals;
    for (size_t k = 0; k <= 10; ++k) {
        fixes.push_back(double(k));
        residuals.push_back({"position", k, double(k), 1});
        residuals.push_back({"velocity", k, double(k), 2});
        if (k)
            residuals.push_back({"imu", k, double(k), 10.+double(k)});
    }
    residuals.push_back({"bias_prior", 0, 0, 1000});
    residuals.push_back({"slope_prior", 0, 0, 1000});
    residuals.push_back({"scale_prior", 0, 0, 1000});
    // 5.3: fixes 3..7 (N = 5), 5 x 3 + (14 + 15 + 16 + 17) over 21. 0.2:
    // fixes 0..2, 9 + 11 + 12 over 9. 2.5: fixes 0..5, both bounds, 18 +
    // (11 + ... + 15) over 27. 9.9: fixes 8..10, 9 + 19 + 20 over 9.
    QCOMPARE(wideningFactors(fixes, residuals, {5.3, .2, 2.5, 9.9}), std::vector<double>({77./21, 32./9, 83./27, 48./9}));

    // Fixes 10 s apart: none within 2.5 s, so the two around the sample
    // (N = 2, three degrees of freedom); a fix within reach is the same one.
    const std::vector<double> sparse{0, 10, 20};
    std::vector<FactorResidual> sparseResiduals;
    for (size_t k = 0; k < 3; ++k) {
        sparseResiduals.push_back({"position", k, sparse[k], 1});
        sparseResiduals.push_back({"velocity", k, sparse[k], 1});
        if (k)
            sparseResiduals.push_back({"imu", k, sparse[k], 5});
    }
    QCOMPARE(wideningFactors(sparse, sparseResiduals, {4, 12, 8.5, 0}), std::vector<double>({3., 3., 3., 3.}));

    // The widening: the square root where the factor exceeds one, one
    // otherwise: it never tightens.
    QCOMPARE(widening(4), 2.);
    QCOMPARE(widening(1), 1.);
    QCOMPARE(widening(.25), 1.);
    QCOMPARE(widening(0), 1.);
}

void FusionKernelTest::wideningIsOneAtTheModel()
{
    // Clause 59, criterion 5. On every committed fixture the widening is
    // exactly one: their noise is below the accuracies they state. And on
    // scale_recording with its GNSS accuracies at the standard deviations of
    // its noise, the residuals are at the model: every widening is at most
    // 1.25 and the median factor within [0.9, 1.1].
    for (const char *name : kAccuracyFixtures) {
        const WindowFit &f = fixtureFit(QLatin1String(name));
        const std::vector<double> factors =
            wideningFactors(f.window.gnssTime, f.fit.residuals, samplesBetweenFirstAndLastFix(f.window));
        const double largest = *std::max_element(factors.begin(), factors.end());
        qInfo() << name << ": largest widening factor" << largest;
        QVERIFY2(widening(largest) == 1, name);
    }
    const WindowFit f = fitOfChannels(scaleRecordingAtItsNoise(1));
    QVERIFY(f.fit.converged);
    const std::vector<double> factors =
        wideningFactors(f.window.gnssTime, f.fit.residuals, samplesBetweenFirstAndLastFix(f.window));
    double largest = 0;
    for (const double factor : factors)
        largest = std::max(largest, widening(factor));
    const double median = medianOf(factors);
    qInfo() << "scale_recording at its noise: median factor" << median << ", largest widening" << largest
            << "; position and velocity nrms" << f.fit.quality.positionNrms << f.fit.quality.velocityNrms;
    QVERIFY(largest <= 1.25);
    QVERIFY(median >= .9 && median <= 1.1);
}

void FusionKernelTest::wideningGrowsWithAnUnderstatedSigma()
{
    // Clauses 31 and 59, criterion 5: scale_recording at its noise with the
    // GNSS accuracies understated three times over [20, 40) s. Every sample
    // whose window lies inside the stretch, [22.5, 37.5) s, is widened by
    // 2.6 to 3.4, the median within 10 % of 3; every sample farther than
    // 2.5 s from the stretch by at most 1.25. And what is published is
    // widened by them: the success assembly (assembleSuccess(), the
    // pipeline's seam after the covariance step) on this fit gives, at every
    // sample, accHAcc and accDAcc equal to w times the unwidened accuracy of
    // the reconstruction (reconstructAtImuRate() with the fit's covariance)
    // and headingAcc and tiltAcc equal to min(180, w times it), bit for bit,
    // with w > 1 at hundreds of samples; and `accuracy.max_widening` and
    // `widened_samples` are those widenings' largest and count.
    const Fusion::Channels channels = scaleRecordingAtItsNoise(3);
    const double epoch = prepareInput(channels).epoch;
    const WindowFit f = fitOfChannels(channels);
    QVERIFY(f.fit.converged);
    const std::vector<double> times = samplesBetweenFirstAndLastFix(f.window);
    const std::vector<double> factors = wideningFactors(f.window.gnssTime, f.fit.residuals, times);
    std::vector<double> inside, outside;
    for (size_t i = 0; i < times.size(); ++i) {
        const double t = epoch+times[i]-1700000000.;
        if (t >= 22.5 && t < 37.5)
            inside.push_back(widening(factors[i]));
        else if (t < 17.5 || t >= 42.5)
            outside.push_back(widening(factors[i]));
    }
    QVERIFY(inside.size() > 300);
    QVERIFY(outside.size() > 500);
    const auto [insideLow, insideHigh] = std::minmax_element(inside.begin(), inside.end());
    const double median = medianOf(inside);
    const double outsideHigh = *std::max_element(outside.begin(), outside.end());
    qInfo() << "scale_recording understated three times over [20, 40) s: inside" << *insideLow << "to" << *insideHigh
            << "(median" << median << "), outside at most" << outsideHigh;
    QVERIFY(*insideLow >= 2.6 && *insideHigh <= 3.4);
    QVERIFY(std::abs(median-3) <= .3);
    QVERIFY(outsideHigh <= 1.25);

    QElapsedTimer published;
    published.start();
    const FitCovariance covariance = fitCovariance(f.fit, f.window.gnssTime.size());
    QVERIFY(covariance.computed);
    const ImuRateTrajectory out = reconstructAtImuRate(f.window, f.fit, f.tuning, &covariance);
    QVERIFY(out.time == times);
    const Fusion::Result result = assembleSuccess(prepareInput(channels), f.account, f.window, f.fit, f.tuning,
                                                  covariance);
    QVERIFY(result.outcome == Fusion::Outcome::Succeeded);
    QVector<double> headingAcc, tiltAcc, accHAcc, accDAcc;
    double largest = 1;
    int widened = 0;
    for (size_t i = 0; i < times.size(); ++i) {
        const double w = widening(factors[i]);
        headingAcc.append(std::min(kYawSigmaCapDeg, w*out.headingAcc[i]));
        tiltAcc.append(std::min(kYawSigmaCapDeg, w*out.tiltAcc[i]));
        accHAcc.append(w*out.accHAcc[i]);
        accDAcc.append(w*out.accDAcc[i]);
        largest = std::max(largest, w);
        widened += w > 1;
    }
    QVERIFY(sameBitsEverywhere(result.headingAcc, headingAcc));
    QVERIFY(sameBitsEverywhere(result.tiltAcc, tiltAcc));
    QVERIFY(sameBitsEverywhere(result.accHAcc, accHAcc));
    QVERIFY(sameBitsEverywhere(result.accDAcc, accDAcc));
    const QJsonObject accuracy = diagnosticsOf(result).value("accuracy").toObject();
    QVERIFY(accuracy.value("computed").toBool(false));
    QVERIFY(accuracy.value("max_widening").toDouble() == largest);
    QCOMPARE(accuracy.value("widened_samples").toInt(-1), widened);
    qInfo() << "published:" << widened << "of" << times.size() << "samples widened, largest" << largest << "; took"
            << published.elapsed() << "ms";
    QVERIFY(widened > 300);
    QVERIFY(largest > 2.6);
}

void FusionKernelTest::accuraciesFiniteAndPositive()
{
    // Clause 56, criterion 6: every sample's four published accuracies are
    // finite and positive on the four success fixtures and the four
    // initializer recordings of the specification; heading and tilt lie in
    // (0, 180].
    for (const char *name : {"coarse_linear", "coarse_maneuver", "stationary_spin", "bridged_hole", "motion_start",
                             "rest_throughout", "sacc_anchor", "drifting_bias"}) {
        const Fusion::Result &result = publishedRun(QLatin1String(name));
        QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, name);
        QVERIFY2(diagnosticsOf(result).value("accuracy").toObject().value("computed").toBool(), name);
        for (const QString &channel : kAccuracyChannels) {
            const QVector<double> &values = fusionChannel(result, channel);
            QCOMPARE(values.size(), result.time.size());
            const bool good = std::all_of(values.begin(), values.end(),
                                          [](double v) { return std::isfinite(v) && v > 0; });
            QVERIFY2(good, qPrintable(QStringLiteral("%1 %2").arg(QLatin1String(name), channel)));
            if (channel == QStringLiteral("headingAcc") || channel == QStringLiteral("tiltAcc"))
                QVERIFY(*std::max_element(values.begin(), values.end()) <= kYawSigmaCapDeg);
            qInfo() << name << channel << ": from" << *std::min_element(values.begin(), values.end()) << "to"
                    << *std::max_element(values.begin(), values.end());
        }
    }
}

void FusionKernelTest::gnssAccuracyScalingNeverLowersThem()
{
    // Clause 56, criterion 6: with every hAcc, vAcc and sAcc doubled and the
    // fit run again, no published accuracy of the four successes falls below
    // (1 - 1e-6) times its value.
    for (const char *name : {"coarse_linear", "coarse_maneuver", "stationary_spin", "bridged_hole"}) {
        const Fusion::Result &before = publishedRun(QLatin1String(name));
        Fusion::Channels channels = toChannels(fusionFixture(QLatin1String(name)));
        for (QVector<double> *sigmas : {&channels.hAcc, &channels.vAcc, &channels.sAcc}) {
            for (double &sigma : *sigmas)
                sigma *= 2;
        }
        const Fusion::Result after = runPipeline(channels, Tuning{}, Checkpoint());
        QVERIFY2(after.outcome == Fusion::Outcome::Succeeded, qPrintable(after.reason));
        for (const QString &channel : kAccuracyChannels) {
            const QVector<double> &a = fusionChannel(before, channel), &b = fusionChannel(after, channel);
            QCOMPARE(b.size(), a.size());
            double smallest = std::numeric_limits<double>::infinity();
            for (qsizetype i = 0; i < a.size(); ++i)
                smallest = std::min(smallest, b[i]/a[i]);
            qInfo() << name << channel << ": smallest ratio after doubling" << smallest;
            QVERIFY2(smallest >= 1-1e-6, qPrintable(QStringLiteral("%1 %2").arg(QLatin1String(name), channel)));
        }
    }
}

void FusionKernelTest::undeterminedHeadingIsCapped()
{
    // Clauses 35 and 60, criterion 7: on coarse_linear, whose heading the data
    // do not determine, the heading accuracy is 180 at every sample, and the
    // tilt and the acceleration accuracies (estimable, so independent of the
    // gauge) are finite and agree within 1e-6 relative with a gauge-fixed
    // reference: the linearized reported graph with a 1e-9 rad heading prior
    // on X(0), through the library's marginals of that linear graph, composed
    // at the samples by the same pass.
    const QString name = QStringLiteral("coarse_linear");
    const Fusion::Result &result = publishedRun(name);
    QVERIFY(result.outcome == Fusion::Outcome::Succeeded);
    QVERIFY(!result.headingAcc.isEmpty());
    QVERIFY(std::all_of(result.headingAcc.begin(), result.headingAcc.end(),
                        [](double v) { return v == kYawSigmaCapDeg; }));
    QCOMPARE(diagnosticsOf(result).value("accuracy").toObject().value("undetermined_heading_samples").toInt(-1),
             int(result.time.size()));

    const WindowFit &f = fixtureFit(name);
    gtsam::GaussianFactorGraph linear = *f.fit.graph.linearize(f.fit.values);
    const gtsam::Matrix3 R0 = f.fit.values.at<gtsam::Pose3>(X(0)).rotation().matrix();
    gtsam::Matrix prior = gtsam::Matrix::Zero(1, 6);
    prior.leftCols<3>() = (R0.transpose()*Vector3::UnitZ()).transpose()/1e-9;
    linear.push_back(std::make_shared<gtsam::JacobianFactor>(X(0), prior, gtsam::Vector1::Zero()));
    const gtsam::Marginals marginals(linear, f.fit.values, gtsam::Marginals::QR);
    const size_t n = f.window.gnssTime.size();
    FitCovariance reference;
    reference.computed = true;
    reference.node.resize(n);
    reference.next.resize(n-1);
    reference.global.resize(n);
    for (size_t k = 0; k+1 < n; ++k) {
        const gtsam::Matrix z = jointOf(marginals, pairKeys(k), f.fit.values);
        reference.node[k] = z.block<9, 9>(0, 0);
        reference.next[k] = z.block<9, 9>(0, 9);
        reference.global[k] = z.block<9, 15>(0, 18);
        reference.node[k+1] = z.block<9, 9>(9, 9);
        reference.global[k+1] = z.block<9, 15>(9, 18);
        reference.globals = z.block<15, 15>(18, 18);
    }
    const ImuRateTrajectory gauged = reconstructAtImuRate(f.window, f.fit, f.tuning, &reference);
    const ImuRateTrajectory &out = fixtureTrajectory(name);
    QCOMPARE(gauged.tiltAcc.size(), out.tiltAcc.size());
    double worst[3] = {0, 0, 0};
    for (size_t i = 0; i < out.time.size(); ++i) {
        const double got[3] = {out.tiltAcc[i], out.accHAcc[i], out.accDAcc[i]};
        const double expected[3] = {gauged.tiltAcc[i], gauged.accHAcc[i], gauged.accDAcc[i]};
        for (int q = 0; q < 3; ++q) {
            QVERIFY(std::isfinite(got[q]) && got[q] > 0);
            const double difference = std::abs(got[q]-expected[q])/expected[q];
            worst[q] = std::max(worst[q], difference);
            QVERIFY2(difference <= 1e-6, qPrintable(QStringLiteral("sample %1, quantity %2: %3")
                                                        .arg(i).arg(q).arg(difference)));
        }
        QCOMPARE(out.headingAcc[i], kYawSigmaCapDeg);
    }
    qInfo() << name << ": against the gauge-fixed reference: tilt" << worst[0] << ", accHAcc" << worst[1]
            << ", accDAcc" << worst[2] << "; first sample tilt" << out.tiltAcc.front() << "deg, accHAcc"
            << out.accHAcc.front() << ", accDAcc" << out.accDAcc.front() << "m/s^2";
}

void FusionKernelTest::covarianceFailureLeavesTheFitAsItIs()
{
    // Clause 34, criterion 8: a covariance step that fails is a result, not a
    // thrown error: computed from a copy of the converged fit whose values
    // carry a NaN, it is not computed and has no block. Given to the
    // success assembly (assembleSuccess(), the pipeline's seam after the
    // covariance step) with the real fit, it leaves the four accuracies
    // empty (which the registration's publish() leaves unset, and of which
    // the runner's --csv writes no column), says so in `accuracy`, nulls the
    // scale sigmas, and leaves
    // the seventeen channels and every other diagnostics key bit-identical to
    // the published run (publishedRun(): the same assembly on the same fit
    // with the covariance computed, as the pipeline runs it).
    const QString name = QStringLiteral("coarse_maneuver");
    const Fusion::Channels channels = toChannels(fusionFixture(name));
    const Fusion::Result &computed = publishedRun(name);
    QVERIFY(computed.outcome == Fusion::Outcome::Succeeded);
    QVERIFY(!computed.headingAcc.isEmpty());

    const WindowFit &f = fixtureFit(name);
    FitResult broken = f.fit;
    broken.values.update(V(0), Vector3(std::numeric_limits<double>::quiet_NaN(), 0, 0));
    const FitCovariance failed = fitCovariance(broken, f.window.gnssTime.size());
    QVERIFY(!failed.computed);
    QVERIFY(failed.node.empty() && failed.next.empty() && failed.global.empty());

    const Fusion::Result without = assembleSuccess(prepareInput(channels), f.account, f.window, f.fit, f.tuning, failed);
    QVERIFY(without.outcome == Fusion::Outcome::Succeeded);
    QVERIFY(without.reason.isEmpty());
    const QStringList names = fusionChannelNames();
    for (const QString &channel : names.mid(0, 17)) {
        QVERIFY(!fusionChannel(without, channel).isEmpty());
        QVERIFY2(sameBitsEverywhere(fusionChannel(without, channel), fusionChannel(computed, channel)),
                 qPrintable(channel));
    }
    for (const QString &channel : kAccuracyChannels)
        QVERIFY2(fusionChannel(without, channel).isEmpty(), qPrintable(channel));

    QJsonObject a = diagnosticsOf(computed), b = diagnosticsOf(without);
    const QJsonObject accuracy = b.value("accuracy").toObject();
    QCOMPARE(accuracy.value("computed").toBool(true), false);
    QCOMPARE(accuracy.value("failure").toString(),
             QStringLiteral("covariance unavailable: the factorization of the converged graph failed"));
    QCOMPARE(QString::fromLatin1(kCovarianceFailure), accuracy.value("failure").toString());
    QCOMPARE(accuracy.value("heading_prior_sigma_rad").toDouble(), 1000.);
    QCOMPARE(accuracy.value("widening_half_width_s").toDouble(), 2.5);
    for (const char *key : {"max_widening", "widened_samples", "undetermined_heading_samples"})
        QVERIFY2(accuracy.value(QLatin1String(key)).isNull(), key);
    QCOMPARE(a.value("accuracy").toObject().value("computed").toBool(false), true);
    QVERIFY(a.value("accuracy").toObject().value("failure").isNull());
    // Everything else, key for key, the two scale sigmas aside.
    for (QJsonObject *diagnostics : {&a, &b}) {
        diagnostics->remove("accuracy");
        QJsonObject model = diagnostics->value("model").toObject();
        QJsonObject scale = model.value("scale").toObject();
        const bool isFailed = diagnostics == &b;
        QCOMPARE(scale.value("acc_sigma").isNull(), isFailed);
        QCOMPARE(scale.value("gyro_sigma").isNull(), isFailed);
        scale.remove("acc_sigma");
        scale.remove("gyro_sigma");
        model.insert("scale", scale);
        diagnostics->insert("model", model);
    }
    QCOMPARE(b, a);
}

void FusionKernelTest::diagnosticsReportTheScaleSigma()
{
    // Clause 22, criterion 9: model.scale's acc_sigma and gyro_sigma are the
    // square roots of the diagonal of S(0)'s marginal covariance (the
    // library's, on the reported graph) within 1e-6 relative; `accuracy` has
    // exactly its keys; the limitations no longer say that no uncertainty is
    // published.
    const QString name = QStringLiteral("coarse_maneuver");
    const Fusion::Result &result = publishedRun(name);
    QVERIFY(result.outcome == Fusion::Outcome::Succeeded);
    const QJsonObject diagnostics = diagnosticsOf(result);
    const QJsonObject scale = diagnostics.value("model").toObject().value("scale").toObject();
    const QJsonArray acc = scale.value("acc_sigma").toArray(), gyro = scale.value("gyro_sigma").toArray();
    QCOMPARE(acc.size(), 3);
    QCOMPARE(gyro.size(), 3);
    const WindowFit &f = fixtureFit(name);
    const gtsam::Marginals marginals(f.fit.graph, f.fit.values, gtsam::Marginals::QR);
    const gtsam::Matrix covariance = marginals.marginalCovariance(S(0));
    for (int i = 0; i < 3; ++i) {
        QVERIFY2(withinRelative(acc.at(i).toDouble(), std::sqrt(covariance(i, i)), 1e-6),
                 qPrintable(QString::number(i)));
        QVERIFY2(withinRelative(gyro.at(i).toDouble(), std::sqrt(covariance(3+i, 3+i)), 1e-6),
                 qPrintable(QString::number(i)));
    }
    qInfo() << name << ": scale sigmas acc" << acc.at(0).toDouble() << acc.at(1).toDouble() << acc.at(2).toDouble()
            << "gyro" << gyro.at(0).toDouble() << gyro.at(1).toDouble() << gyro.at(2).toDouble();

    const QJsonObject accuracy = diagnostics.value("accuracy").toObject();
    QCOMPARE(accuracy.keys(), QStringList({"computed", "failure", "heading_prior_sigma_rad", "max_widening",
                                           "undetermined_heading_samples", "widened_samples",
                                           "widening_half_width_s"}));
    QCOMPARE(accuracy.value("computed").toBool(false), true);
    QVERIFY(accuracy.value("failure").isNull());
    QCOMPARE(accuracy.value("heading_prior_sigma_rad").toDouble(), kHeadingPriorSigmaRad);
    QCOMPARE(accuracy.value("widening_half_width_s").toDouble(), kWideningHalfWidthS);
    QCOMPARE(accuracy.value("max_widening").toDouble(), 1.);
    QCOMPARE(accuracy.value("widened_samples").toInt(-1), 0);
    QCOMPARE(accuracy.value("undetermined_heading_samples").toInt(-1), 0);
    QVERIFY(!diagnostics.value("limitations").toString().contains(QStringLiteral("no uncertainty")));
}

// ---- GNSS holes bridged by the IMU (the specification of 1301-1313) ---------------

namespace {

/// The entries of a fit's `input.gnss_holes`, seconds since the epoch.
std::vector<GnssHole> gnssHolesOf(const QJsonObject &diagnostics)
{
    std::vector<GnssHole> holes;
    for (const QJsonValue &entry : diagnostics.value("input").toObject().value("gnss_holes").toArray()) {
        const QJsonObject hole = entry.toObject();
        holes.push_back({hole.value("start_s").toDouble(-1), hole.value("length_s").toDouble(-1)});
    }
    return holes;
}

/// `a` and `b` are the same holes, bit for bit.
bool sameHoles(const std::vector<GnssHole> &a, const std::vector<GnssHole> &b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const GnssHole &x, const GnssHole &y) {
        return x.start == y.start && x.length == y.length;
    });
}

/// A published time read back as seconds since the epoch is that time to
/// rounding of the UTC addition: a sample on a fix is within this of it.
constexpr double kOnAFixS = 1e-6;

/// The published samples of `result` strictly inside `hole`: after the fix
/// before it and before the fix after it.
std::vector<qsizetype> samplesInside(const Fusion::Result &result, double epoch, const GnssHole &hole)
{
    std::vector<qsizetype> inside;
    for (qsizetype i = 0; i < result.time.size(); ++i) {
        const double t = result.time[i]-epoch;
        if (t > hole.start+kOnAFixS && t < hole.start+hole.length-kOnAFixS)
            inside.push_back(i);
    }
    return inside;
}

/// The published samples nearest the two fixes around `hole` from outside
/// it: the last on or before the fix before it, the first on or after the
/// fix after it.
std::pair<qsizetype, qsizetype> samplesBeside(const Fusion::Result &result, double epoch, const GnssHole &hole)
{
    qsizetype before = -1, after = -1;
    for (qsizetype i = 0; i < result.time.size(); ++i) {
        const double t = result.time[i]-epoch;
        if (t <= hole.start+kOnAFixS)
            before = i;
        if (after < 0 && t >= hole.start+hole.length-kOnAFixS)
            after = i;
    }
    return {before, after};
}

/// The epoch of a fit's diagnostics, `input.epoch_utc_s`.
double epochOf(const QJsonObject &diagnostics)
{
    return diagnostics.value("input").toObject().value("epoch_utc_s").toDouble();
}

/// The fixes `first`..`first + count - 1` of `f` removed from every per-fix
/// channel: a hole in the fixes with the IMU untouched.
void removeFixes(FusionFixture &f, qsizetype first, qsizetype count)
{
    for (QVector<double> *channel : {&f.gnssTime, &f.north, &f.east, &f.down, &f.velN, &f.velE, &f.velD,
                                     &f.hAcc, &f.vAcc, &f.sAcc})
        channel->remove(first, count);
}

/// The squared whitened residual of the `kind` factor at the first fix after
/// `hole`, from a fit's `residuals`, with that fix's node; -1 when there is none.
std::pair<double, int> residualAfter(const QJsonObject &diagnostics, const GnssHole &hole, const QString &kind)
{
    for (const QJsonValue &entry : diagnostics.value("residuals").toArray()) {
        const QJsonObject residual = entry.toObject();
        if (residual.value("kind").toString() == kind && residual.value("time_s").toDouble() > hole.start+kOnAFixS)
            return {residual.value("squared_whitened_error").toDouble(), residual.value("node").toInt(-1)};
    }
    return {-1, -1};
}

/// The variances of the position and of the velocity in a 9x9 covariance of
/// the IMU factor's order (attitude, position, velocity): the traces of the
/// two blocks, m^2 and (m/s)^2.
std::pair<double, double> positionVelocityTraces(const gtsam::Matrix9 &covariance)
{
    return {covariance.block<3, 3>(3, 3).trace(), covariance.block<3, 3>(6, 6).trace()};
}

/// The first edge of `r` that is an IMU sample of `window`, i.e. the first
/// sample the reconstruction publishes in its interval: edge 0 when the fix
/// that starts the interval is on a sample, edge 1 otherwise.
size_t firstPublishedEdge(const Samples &window, const IntervalReconstruction &r)
{
    return std::binary_search(window.imuTime.begin(), window.imuTime.end(), r.edges.front()) ? 0 : 1;
}

} // namespace

void FusionKernelTest::bridgedHoleFollowsTheTruth()
{
    // Items 1302 and 1307: bridged_hole is coarse_maneuver with a 2.6 s hole
    // in its fixes and the IMU continuous. Every IMU sample inside the hole is
    // published, and at each the published heading lies within three
    // headingAcc of the truth's 0, the tilt within three tiltAcc of 0, the
    // acceleration error's component along the published horizontal
    // acceleration within three accHAcc and its down component within three
    // accDAcc of the generating (1.5 - .4t, .8t, -.6 + .1t^2). Published
    // through assembleSuccess(), the pipeline's seam.
    //
    // The four published accuracies are bounded by global terms, so they need
    // not grow through a hole. The specification's "never below their values
    // at the fixes around it" is measured false here (docs/SENSOR_FUSION.md
    // section 8: heading and tilt move from the value before the hole to the
    // value after it, the accelerations' dip below both), so it is logged,
    // each channel's smallest and largest inside against the samples nearest
    // the two fixes, not asserted.
    //
    // The growth through the hole is in the position and velocity of the
    // sample covariance. Read through the reconstruction's per-interval seam
    // (reconstructInterval()), P_j of the hole's interval, the step chain's
    // covariance, is zero at the fix before the hole, never falls from one
    // edge to the next, is largest among the interval's published samples at
    // the last one inside the hole, ends at the IMU factor's own covariance,
    // and collapses at the fix after the hole, where the next interval's chain
    // starts again from the fitted state.
    const QString name = QStringLiteral("bridged_hole");
    const Fusion::Result &result = publishedRun(name);
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
    const QJsonObject diagnostics = diagnosticsOf(result);
    const double epoch = epochOf(diagnostics);
    const std::vector<GnssHole> holes = gnssHolesOf(diagnostics);
    QCOMPARE(holes.size(), size_t(1));
    const GnssHole &hole = holes.front();
    const std::vector<qsizetype> inside = samplesInside(result, epoch, hole);
    // 2.6 s of 100 Hz samples: 260, every one of them published.
    QCOMPARE(inside.size(), size_t(260));

    // Ratio of each error to its accuracy, and the smallest and largest
    // accuracy, per channel: heading, tilt, along the horizontal
    // acceleration, down.
    const double infinity = std::numeric_limits<double>::infinity();
    double ratio[4] = {0, 0, 0, 0}, largest[4] = {0, 0, 0, 0};
    double smallest[4] = {infinity, infinity, infinity, infinity};
    for (const qsizetype i : inside) {
        const double t = result.time[i]-1700000000.;
        const Vector3 truth(1.5-.4*t, .8*t, -.6+.1*t*t);
        const Vector3 published(result.accN[i], result.accE[i], result.accD[i]);
        const Vector3 error = published-truth;
        const Eigen::Vector2d horizontal = published.head<2>().normalized();
        const Rot3 attitude = Rot3::Quaternion(result.qw[i], result.qx[i], result.qy[i], result.qz[i]);
        const double errors[4] = {std::abs(angleDifference(result.yaw[i], 0)),
                                  std::acos(std::clamp(attitude.matrix()(2, 2), -1., 1.))*180/kPi,
                                  std::abs(error.head<2>().dot(horizontal)), std::abs(error.z())};
        const double accuracies[4] = {result.headingAcc[i], result.tiltAcc[i], result.accHAcc[i], result.accDAcc[i]};
        for (int c = 0; c < 4; ++c) {
            QVERIFY(std::isfinite(accuracies[c]) && accuracies[c] > 0);
            ratio[c] = std::max(ratio[c], errors[c]/accuracies[c]);
            largest[c] = std::max(largest[c], accuracies[c]);
            smallest[c] = std::min(smallest[c], accuracies[c]);
        }
    }
    const auto [before, after] = samplesBeside(result, epoch, hole);
    QVERIFY(before >= 0 && after > before);
    const QVector<double> *channels[4] = {&result.headingAcc, &result.tiltAcc, &result.accHAcc, &result.accDAcc};
    for (int c = 0; c < 4; ++c) {
        qInfo() << "bridged_hole:" << kAccuracyChannels[c] << "inside the hole: largest error / accuracy"
                << ratio[c] << ", accuracy from" << smallest[c] << "to" << largest[c] << "against"
                << (*channels[c])[before] << "before and" << (*channels[c])[after] << "after";
    }

    // The sample covariance of the hole's interval, k the fix before it, the
    // interval before it for comparison and the one after it for the
    // collapse. Measured before the assertions on the accuracies, so that
    // the log holds both whatever fails.
    const WindowFit &f = fixtureFit(name);
    const auto fix = std::find(f.window.gnssTime.begin(), f.window.gnssTime.end(), hole.start);
    QVERIFY(fix != f.window.gnssTime.end());
    const size_t k = size_t(fix-f.window.gnssTime.begin());
    QVERIFY(k >= 1 && k+2 < f.window.gnssTime.size());
    QVERIFY(f.window.gnssTime[k+1]-f.window.gnssTime[k] == hole.length);
    IntervalSensitivity sensitivity, nextSensitivity, ordinarySensitivity;
    const IntervalReconstruction r = reconstructInterval(f.window, f.fit, f.tuning, k, &sensitivity);
    const IntervalReconstruction next = reconstructInterval(f.window, f.fit, f.tuning, k+1, &nextSensitivity);
    reconstructInterval(f.window, f.fit, f.tuning, k-1, &ordinarySensitivity);
    const size_t n = r.edges.size()-1;
    QCOMPARE(sensitivity.covariance.size(), n+1);
    const auto [positionLast, velocityLast] = positionVelocityTraces(sensitivity.covariance[n-1]);
    const auto [positionEnd, velocityEnd] = positionVelocityTraces(sensitivity.covariance[n]);
    const auto [positionMiddle, velocityMiddle] = positionVelocityTraces(sensitivity.covariance[n/2]);
    const size_t firstNext = firstPublishedEdge(f.window, next);
    const auto [positionNext, velocityNext] = positionVelocityTraces(nextSensitivity.covariance[firstNext]);
    const auto [positionOrdinary, velocityOrdinary] = positionVelocityTraces(ordinarySensitivity.covariance.back());
    qInfo() << "bridged_hole: the hole's interval," << n << "steps, P_j as sigmas (the root of the trace): position"
            << std::sqrt(positionMiddle) << "m mid-hole," << std::sqrt(positionLast)
            << "m at the last sample inside," << std::sqrt(positionEnd) << "m at the fix after it (the factor's),"
            << std::sqrt(positionNext) << "m at the first sample after that fix; velocity" << std::sqrt(velocityMiddle)
            << "," << std::sqrt(velocityLast) << "," << std::sqrt(velocityEnd) << "," << std::sqrt(velocityNext)
            << "m/s; the interval before the hole ends at" << std::sqrt(positionOrdinary) << "m and"
            << std::sqrt(velocityOrdinary) << "m/s";
    qInfo() << "bridged_hole:" << inside.size() << "samples inside the hole of" << hole.length << "s from"
            << hole.start << "s";

    for (int c = 0; c < 4; ++c)
        QVERIFY2(ratio[c] <= 3, qPrintable(kAccuracyChannels[c]));
    const auto [position0, velocity0] = positionVelocityTraces(sensitivity.covariance.front());
    QCOMPARE(position0, 0.);
    QCOMPARE(velocity0, 0.);
    for (size_t j = 1; j <= n; ++j) {
        const auto [positionBefore, velocityBefore] = positionVelocityTraces(sensitivity.covariance[j-1]);
        const auto [position, velocity] = positionVelocityTraces(sensitivity.covariance[j]);
        QVERIFY2(position >= positionBefore && velocity >= velocityBefore, qPrintable(QString::number(j)));
    }
    QVERIFY(sensitivity.covariance[n] == r.endCovariance);
    // The interval publishes edges 0..n-1 (edge n is the fix after the hole,
    // published as the next interval's): the last of them is inside the hole.
    QVERIFY(r.edges[n-1] > hole.start && r.edges[n-1] < hole.start+hole.length);
    QVERIFY(positionLast > positionOrdinary && velocityLast > velocityOrdinary);
    QVERIFY(positionNext < positionLast/100 && velocityNext < velocityLast/100);
}

void FusionKernelTest::longHoleConverges()
{
    // Items 1301, 1303 and 1308: long_hole, a 30 s hole in the fixes (the
    // longest the fit bridges) with the IMU continuous, converges under the
    // production tuning. The hole's interval is one IMU factor like any
    // other, and the fix after it is an ordinary entry of `residuals`.
    // Logged: the iterations per pass, the largest of each accuracy inside the
    // hole against its median over the fit, and the squared whitened position
    // and velocity residuals at the fix after the hole.
    const QString name = QStringLiteral("long_hole");
    PipelineTrace trace;
    QElapsedTimer timer;
    timer.start();
    const Fusion::Result result = runPipeline(toChannels(initializerFixture(name)), Tuning{}, Checkpoint(), &trace);
    std::map<int, int> perPass;
    for (const FitIteration &iteration : trace.history)
        ++perPass[iteration.outer];
    QStringList passes;
    for (const auto &[pass, iterations] : perPass)
        passes.append(QString::number(iterations));
    qInfo() << "long_hole:" << trace.history.size() << "iterations, per pass" << passes.join(QStringLiteral(", "))
            << "; rule" << trace.stopping.rule.c_str() << "; outcome" << int(result.outcome)
            << qPrintable(result.reason) << "; took" << timer.elapsed() << "ms";
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
    QVERIFY(trace.converged);

    const QJsonObject diagnostics = diagnosticsOf(result);
    const double epoch = epochOf(diagnostics);
    const std::vector<GnssHole> holes = gnssHolesOf(diagnostics);
    QCOMPARE(holes.size(), size_t(1));
    const GnssHole &hole = holes.front();
    QCOMPARE(hole.length, 30.);
    const std::vector<qsizetype> inside = samplesInside(result, epoch, hole);
    QCOMPARE(inside.size(), size_t(2999));
    const QVector<double> *channels[4] = {&result.headingAcc, &result.tiltAcc, &result.accHAcc, &result.accDAcc};
    for (int c = 0; c < 4; ++c) {
        double largest = 0;
        for (const qsizetype i : inside) {
            QVERIFY(std::isfinite((*channels[c])[i]) && (*channels[c])[i] > 0);
            largest = std::max(largest, (*channels[c])[i]);
        }
        qInfo() << "long_hole:" << kAccuracyChannels[c] << "largest inside the hole" << largest
                << ", median over the fit" << medianOf(std::vector<double>(channels[c]->begin(), channels[c]->end()));
    }

    // One IMU factor per interval, the hole's included, and the fix after the
    // hole an ordinary fix of the residuals: node 150, after the 150 before it.
    const QJsonArray residuals = diagnostics.value("residuals").toArray();
    int imuResiduals = 0;
    for (const QJsonValue &entry : residuals)
        imuResiduals += entry.toObject().value("kind").toString() == QStringLiteral("imu");
    QCOMPARE(diagnostics.value("gnss_states").toInt(), 451);
    QCOMPARE(imuResiduals, 450);
    const auto [position, positionNode] = residualAfter(diagnostics, hole, QStringLiteral("position"));
    const auto [velocity, velocityNode] = residualAfter(diagnostics, hole, QStringLiteral("velocity"));
    const auto [imu, imuNode] = residualAfter(diagnostics, hole, QStringLiteral("imu"));
    const QJsonObject quality = diagnostics.value("quality").toObject();
    qInfo() << "long_hole: at the fix after the hole, squared whitened residuals: position" << position
            << ", velocity" << velocity << "; the hole's IMU factor" << imu << "; position nrms"
            << quality.value("position_nrms").toDouble() << ", velocity nrms" << quality.value("velocity_nrms").toDouble()
            << ", imu nrms" << quality.value("imu_nrms").toDouble();
    QCOMPARE(positionNode, 150);
    QCOMPARE(velocityNode, 150);
    QCOMPARE(imuNode, 150);
    QVERIFY(std::isfinite(position) && position >= 0);
    QVERIFY(std::isfinite(velocity) && velocity >= 0);
}

void FusionKernelTest::holeAboveTheCapIsRejected()
{
    // Items 1301 and 1308: the long-hole recording with a hole longer than
    // the longest interval the fit bridges is rejected before the fit, the
    // reason naming the interval, to two decimals, and the limit, with a
    // rejection's diagnostics. The limit is "longer than": a hole of exactly
    // 30 s passes the plan (here the fit is then forced to end on its first
    // iteration, a solver failure, so that the plan alone is judged;
    // longHoleConverges fits it under the production tuning), and one of
    // 31 s does not. The cap is the factor's span, not the continuity rule's
    // hole: the same recording without a hole and its fixes thinned to one
    // every 31 s, which the rule calls no hole (every interval is the
    // median), is rejected too, and thinned to one every 30 s it passes.
    const Fusion::Result sixty = rejectedBy(toChannels(longHole(60)));
    QVERIFY2(rejectedWith(sixty, QStringLiteral("GNSS fixes 60.00 s apart; fusion bridges at most 30 s")),
             qPrintable(sixty.reason));
    QCOMPARE(diagnosticsOf(sixty).keys(), QStringList({"algorithm", "failure"}));
    QVERIFY(allChannelsEmpty(sixty));
    const Fusion::Result thirtyOne = rejectedBy(toChannels(longHole(31)));
    QVERIFY2(rejectedWith(thirtyOne, QStringLiteral("GNSS fixes 31.00 s apart; fusion bridges at most 30 s")),
             qPrintable(thirtyOne.reason));

    Tuning oneIteration;
    oneIteration.relativeTolerance = -1;
    oneIteration.maxIterations = 1;
    oneIteration.maxPasses = 1;
    const Fusion::Result thirty = runPipeline(toChannels(longHole(30)), oneIteration, Checkpoint());
    QVERIFY2(thirty.outcome == Fusion::Outcome::SolverFailed, qPrintable(thirty.reason));

    // The fixes of the recording without a hole, one in every `step`.
    const auto thinned = [](int step) {
        Fusion::Channels c = toChannels(longHole(0));
        for (QVector<double> *channel : {&c.gnssTime, &c.north, &c.east, &c.down, &c.velN, &c.velE, &c.velD,
                                         &c.hAcc, &c.vAcc, &c.sAcc}) {
            QVector<double> kept;
            for (qsizetype i = 0; i < channel->size(); i += step)
                kept.append((*channel)[i]);
            *channel = kept;
        }
        return c;
    };
    const Fusion::Result sparse = rejectedBy(thinned(155));
    QVERIFY2(rejectedWith(sparse, QStringLiteral("GNSS fixes 31.00 s apart; fusion bridges at most 30 s")),
             qPrintable(sparse.reason));
    const Fusion::Result sparseAtTheCap = runPipeline(thinned(150), oneIteration, Checkpoint());
    QVERIFY2(sparseAtTheCap.outcome == Fusion::Outcome::SolverFailed, qPrintable(sparseAtTheCap.reason));
    qInfo() << "long-hole recording: 60 s" << sixty.reason << "; 31 s" << thirtyOne.reason << "; 30 s"
            << thirty.reason << "; fixes 31 s apart" << sparse.reason << "; 30 s apart" << sparseAtTheCap.reason;
}

void FusionKernelTest::sparsePiecesAreMerged()
{
    // Item 1304: the segment cutter merges any piece with fewer than three
    // fixes, not only the final one: into the piece before it, or into the
    // piece after it when it is the first; a middle piece of three fixes is a
    // segment however short; then the final piece's time rule as before.
    using Bounds = std::vector<std::pair<size_t, size_t>>;
    const auto axis = [](std::initializer_list<std::pair<int, int>> stretches) {
        std::vector<double> t;
        for (const auto &[first, last] : stretches) {
            for (int s = first; s <= last; ++s)
                t.push_back(s);
        }
        return t;
    };
    // 1 Hz, 60 s segments, 12 s minimum final piece. A middle piece of one
    // fix (70 s) ends the piece before it; so does one of two (70, 71 s).
    QCOMPARE(segmentBounds(axis({{0, 59}, {70, 70}, {130, 200}}), 60, 12),
             Bounds({{0, 60}, {61, 110}, {111, 131}}));
    QCOMPARE(segmentBounds(axis({{0, 59}, {70, 71}, {130, 200}}), 60, 12),
             Bounds({{0, 61}, {62, 111}, {112, 132}}));
    // A first piece of two fixes (0, 1 s) starts the piece after it.
    QCOMPARE(segmentBounds(axis({{0, 1}, {70, 200}}), 60, 12), Bounds({{0, 51}, {52, 111}, {112, 132}}));
    // Two sparse middle pieces in a row (70 s, 130 s) both end their predecessor.
    QCOMPARE(segmentBounds(axis({{0, 59}, {70, 70}, {130, 130}, {190, 260}}), 60, 12),
             Bounds({{0, 61}, {62, 111}, {112, 132}}));
    // A middle piece of exactly three fixes (70..72 s) stays, 2 s long.
    QCOMPARE(segmentBounds(axis({{0, 59}, {70, 72}, {130, 200}}), 60, 12),
             Bounds({{0, 59}, {60, 62}, {63, 112}, {113, 133}}));
    // A sparse final piece (60, 61 s) merges as before.
    QCOMPARE(segmentBounds(axis({{0, 61}}), 60, 12), Bounds({{0, 61}}));
    // A stretch of whole segment lengths without a fix (60..180 s) yields no
    // piece; the final piece 240..250 s is then short and merges.
    QCOMPARE(segmentBounds(axis({{0, 59}, {190, 250}}), 60, 12), Bounds({{0, 59}, {60, 120}}));
    // A first piece of two fixes followed only by sparse pieces: one piece.
    QCOMPARE(segmentBounds(axis({{0, 1}, {70, 70}, {130, 130}}), 60, 12), Bounds({{0, 3}}));
    // A window that is one piece is left alone, whatever its count.
    QCOMPARE(segmentBounds(axis({{0, 1}}), 60, 12), Bounds({{0, 1}}));

    // long_hole under 29.57 s segments: fixes at 0 .. 29.8 s and 59.8 ..
    // 119.8 s since the epoch, so the cut's second piece [29.57, 59.14) holds
    // the two fixes at 29.6 and 29.8 s before the hole, which end the first
    // segment; the pieces from 59.14 s on hold the 301 fixes after it, the
    // final one (118.4 .. 119.8 s) short and merged. Three segments, the
    // first ending at the fix before the hole and the second starting at the
    // fix after it, and the fit converges.
    Tuning tuning;
    tuning.segmentLength = 29.57;
    tuning.minFinalSegment = 12;
    const Samples window = windowOf(QStringLiteral("long_hole"), tuning);
    const double t0 = window.gnssTime.front();
    QCOMPARE(std::count_if(window.gnssTime.begin(), window.gnssTime.end(), [&](double t) {
                 return t >= t0+tuning.segmentLength && t < t0+2*tuning.segmentLength;
             }), std::ptrdiff_t(2));
    QCOMPARE(segmentBounds(window.gnssTime, tuning.segmentLength, tuning.minFinalSegment),
             Bounds({{0, 149}, {150, 294}, {295, 450}}));
    const InitializerRun run = runInitializerFixture(QStringLiteral("long_hole"), tuning);
    const std::vector<SegmentAccount> &segments = run.trace.initializer.segments;
    QCOMPARE(segments.size(), size_t(3));
    QCOMPARE(segments[0].firstFix, size_t(0));
    QCOMPARE(segments[0].lastFix, size_t(149));
    QCOMPARE(segments[1].firstFix, size_t(150));
    QCOMPARE(segments[1].lastFix, size_t(294));
    QCOMPARE(segments[2].firstFix, size_t(295));
    QCOMPARE(segments[2].lastFix, size_t(450));
    for (const SegmentAccount &s : segments) {
        qInfo() << "long_hole under 29.57 s segments: segment" << s.index << "fixes" << s.firstFix << "to" << s.lastFix
                << ", yaw sigma" << s.yawSigmaDeg << "deg, growth stop" << s.growthStop.c_str() << ", converged"
                << s.converged;
        QVERIFY(!s.fallback);
        QVERIFY(s.converged);
    }
    QVERIFY2(run.result.outcome == Fusion::Outcome::Succeeded, qPrintable(run.result.reason));
    QVERIFY(run.trace.converged);
}

void FusionKernelTest::gnssHolesInTheAudit()
{
    // Items 1305 and 1309: `input.gnss_holes` is in the diagnostics of every
    // success, those published through assembleSuccess() included: one
    // {start_s, length_s} per hole of the fitted window's GNSS axis, judged
    // by the continuity authority against that axis's own threshold, the fix
    // before the hole in seconds since the epoch, in time order; empty for a
    // window without one. A rejection's diagnostics have no such key. Each
    // step below stands on its own, so that one failing hides none of the
    // others.

    // The four golden fits: the array is the walk the plan's cap reads
    // (gnssHoles() of the fitted window), bit for bit, with its two keys.
    const std::pair<const char *, size_t> expected[] = {
        {"coarse_linear", 0}, {"coarse_maneuver", 0}, {"stationary_spin", 0}, {"bridged_hole", 1}};
    for (const auto &[name, count] : expected) {
        const Fusion::Result &result = publishedRun(QLatin1String(name));
        QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, name);
        const QJsonObject input = diagnosticsOf(result).value("input").toObject();
        QVERIFY2(input.value("gnss_holes").isArray(), name);
        const std::vector<GnssHole> holes = gnssHolesOf(diagnosticsOf(result));
        QCOMPARE(holes.size(), count);
        QVERIFY2(sameHoles(holes, gnssHoles(fixtureFit(QLatin1String(name)).window)), name);
        for (const QJsonValue &entry : input.value("gnss_holes").toArray())
            QCOMPARE(entry.toObject().keys(), QStringList({"length_s", "start_s"}));
    }

    // Against the fixtures' construction, the independent evidence: the hole
    // is the interval between the fixes the generator kept on either side of
    // the ones it removed, in seconds since the epoch (the first fix).
    // bridged_hole's is from the fix j = 11 to j = 24 of coarse_maneuver
    // (2.2 s and 4.8 s since the epoch, the fix j = 0 at -.163 s), long_hole's
    // from the fix at 29.9 s to the one at 59.9 s (29.8 s and 59.8 s since
    // the epoch, the first at .1 s). long_hole has its own fit, so its check
    // is a step of its own.
    const auto construction = [](const char *name, int before, double start, double length) {
        const FusionFixture f = fixtureNamed(QLatin1String(name));
        const QJsonObject diagnostics = diagnosticsOf(publishedRun(QLatin1String(name)));
        QVERIFY2(!diagnostics.isEmpty(), name);
        const double epoch = epochOf(diagnostics);
        QCOMPARE(epoch, f.gnssTime.front());
        const std::vector<GnssHole> holes = gnssHolesOf(diagnostics);
        QCOMPARE(holes.size(), size_t(1));
        const GnssHole &hole = holes.front();
        QVERIFY2(hole.start == f.gnssTime[before]-epoch, name);
        QVERIFY2(hole.length == (f.gnssTime[before+1]-epoch)-(f.gnssTime[before]-epoch), name);
        QVERIFY2(std::abs(hole.start-start) < 1e-6 && std::abs(hole.length-length) < 1e-6, name);
        qInfo() << name << ": gnss_holes start" << hole.start << "s, length" << hole.length << "s";
    };
    construction("bridged_hole", 11, 2.2, 2.6);
    construction("long_hole", 149, 29.8, 30);

    // Two holes, in time order: coarse_maneuver without the fixes j = 6..8
    // and j = 15..20, holes of .8 s from j = 5 and 1.4 s from j = 14.
    FusionFixture twoHoles = fusionFixture(QStringLiteral("coarse_maneuver"));
    removeFixes(twoHoles, 15, 6);
    removeFixes(twoHoles, 6, 3);
    const Fusion::Result result = runPipeline(toChannels(twoHoles), Tuning{}, Checkpoint());
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
    const std::vector<GnssHole> holes = gnssHolesOf(diagnosticsOf(result));
    QCOMPARE(holes.size(), size_t(2));
    const double epoch = twoHoles.gnssTime.front();
    QVERIFY(holes[0].start == twoHoles.gnssTime[5]-epoch);
    QVERIFY(holes[1].start == twoHoles.gnssTime[11]-epoch);
    QVERIFY(std::abs(holes[0].length-.8) < 1e-6);
    QVERIFY(std::abs(holes[1].length-1.4) < 1e-6);
    QVERIFY(holes[0].start+holes[0].length < holes[1].start);

    // A rejection: the algorithm and the reason, no input audit.
    QCOMPARE(failureDiagnostics(QStringLiteral("reason")).keys(), QStringList({"algorithm", "failure"}));
    const Fusion::Result rejected = rejectedBy(toChannels(fusionFixture(QStringLiteral("reject_imu_gap"))));
    QVERIFY(rejected.outcome == Fusion::Outcome::Rejected);
    QVERIFY(!diagnosticsOf(rejected).contains("input"));
}

FLYSIGHT_TEST_MAIN(FusionKernelTest)
#include "tst_fusion_kernel.moc"
