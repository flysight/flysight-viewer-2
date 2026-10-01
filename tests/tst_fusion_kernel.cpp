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
// bias (the custom factor's Jacobians, the section 6 cases), the
// solver-failure path and its diagnostics shapes, the IMU-rate reconstruction
// pass against a dense reference graph and its per-interval seam, the
// channels the fit publishes as that pass and their time axis), with the literal expectations of the reference's own
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
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/NonlinearEquality.h>
#include <gtsam/slam/PriorFactor.h>

#include "calculations/anglehelper.h"
#include "fusion/factorgraphfit.h"
#include "fusion/fusionoutput.h"
#include "fusion/fusionpipeline.h"
#include "fusion/fusionsamples.h"
#include "fusion/imuintegration.h"
#include "fusion/initializer.h"
#include "fusion/inputadapter.h"
#include "fusion/sensornoise.h"
#include "fusion/temperatureimufactor.h"
#include "fusion/trajectoryreconstruction.h"
#include "fusionfixtures.h"
#include "fusiongolden.h"
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
using gtsam::symbol_shorthand::B;
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
    const gtsam::PreintegratedImuMeasurements pim = preintegrateImu(d, start, end, bias, noise, observer);
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
        && result.roll.isEmpty() && result.yaw.isEmpty() && result.qw.isEmpty();
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
    tuning.maxGap = kImuGapMedians*medianInterval(prepareInput(channels).recording.imuTime);
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

InitializerRun runInitializerFixture(const QString &name, const Tuning &tuning,
                                     const Checkpoint &checkpoint = Checkpoint())
{
    InitializerRun run;
    run.result = runPipeline(toChannels(fixtureNamed(name)), tuning, checkpoint, &run.trace);
    run.diagnostics = diagnosticsOf(run.result);
    run.segments = run.diagnostics.value("initializer").toObject().value("segments").toArray();
    return run;
}

/// rest_throughout through the whole pipeline with the production tuning,
/// once per run of this executable: its fit is the longest of them, and two
/// tests read it (atRestPrefixStopsGrowing, dampingSaturationIsASolverFailure).
const InitializerRun &restThroughoutRun()
{
    static const InitializerRun run = runInitializerFixture(QStringLiteral("rest_throughout"), Tuning{});
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
};

/// A fixture's full fit through the internal seams, in the order of planFit()
/// and fitAndAssemble(): the prepared recording, the derived IMU gap limit,
/// the fitted window validated, the temperature model, the initializer and
/// the fit. The fits dominate this executable's time, so each is made once
/// per run; the caller checks convergence.
const WindowFit &fixtureFit(const QString &name)
{
    static std::map<QString, WindowFit> fits;
    const auto found = fits.find(name);
    if (found != fits.end())
        return found->second;
    WindowFit f;
    f.tuning = pipelineTuning(name, Tuning{});
    f.window = windowOf(name, Tuning{});
    const GyroBiasModel model = gyroBiasModelFor(f.window);
    const Initialization init = initialize(f.window, f.tuning);
    f.fit = fitFactorGraph(f.window, init.state, f.tuning, QString::fromLatin1(kFullFitPassFormat), Checkpoint(), model);
    return fits.emplace(name, std::move(f)).first->second;
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
/// without restating either, at its interval's bias under the fit's model.
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
    }
    for (size_t j = 0; j+1 < w.edges.size(); ++j) {
        const size_t k = w.stepInterval[j];
        const auto pim = preintegrateImu(f.window, w.edges[j], w.edges[j+1],
                                         intervalBias(f.window, k, bias, f.fit.gyroBiasSlope, model), f.tuning.noise);
        if (model.temperatureLinear)
            graph.emplace_shared<TemperatureImuFactor>(X(j), V(j), X(j+1), V(j+1), B(0), T(0), pim,
                                                       temperatureAtFix(f.window, k)-model.tRef);
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
    f.tuning.maxGap = kImuGapMedians*medianInterval(d.imuTime);
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
        if (const auto *temperature = dynamic_cast<const TemperatureImuFactor *>(factor.get()))
            return temperature->preintegratedMeasurements().preintMeasCov();
        if (const auto *stock = dynamic_cast<const gtsam::ImuFactor *>(factor.get()))
            return stock->preintegratedMeasurements().preintMeasCov();
    }
    return gtsam::Matrix();
}

/// The fixes and IMU samples of a WindowFit whose fitted states are each the
/// prediction of the one before, perturbed by `perturbation` (zero: the
/// forward integration exactly, interval by interval).
void predictFits(WindowFit &f, const gtsam::imuBias::ConstantBias &bias, const gtsam::NavState &first,
                 const gtsam::Vector9 &perturbation)
{
    f.fit.values.insert(B(0), bias);
    gtsam::NavState state = first;
    for (size_t k = 0; k < f.window.gnssTime.size(); ++k) {
        if (k) {
            const auto pim = preintegrateImu(f.window, f.window.gnssTime[k-1], f.window.gnssTime[k], bias, f.tuning.noise);
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
/// edge, the step after alone on the window's first edge.
Vector3 expectedAcceleration(const WindowFit &f, const WindowSeams &w, const ImuRateTrajectory &out, size_t i)
{
    const size_t e = edgeAt(w, out.time[i]);
    const size_t sample = size_t(std::lower_bound(f.window.imuTime.begin(), f.window.imuTime.end(), out.time[i])
                                 -f.window.imuTime.begin());
    const Vector3 correction = e ? Vector3((w.stepCorrection[e-1]+w.stepCorrection[e])/2) : w.stepCorrection[e];
    const Vector3 accBias = f.fit.values.at<gtsam::imuBias::ConstantBias>(B(0)).accelerometer();
    return out.rotation[i].rotate(f.window.force[sample]-accBias)+kGravity+correction;
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
    const auto pim = preintegrateImu(d, .037, .863, bias, tuning.noise);
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
    // Table 18, the accelerometer at ODR / 2, the step the sensitivity), bit
    // for bit: the same operations in the same order.
    const double g = 9.80665, radians = kPi/180;
    const struct { double rate, gyroBandwidth; } configurations[] = {{12.5, 4.2}, {26, 8.3}, {104, 33.0}};
    for (const auto &c : configurations) {
        const ImuNoise n = fixtureNoise(c.rate);
        QCOMPARE(n.configuration.accelFsG, 16.);
        QCOMPARE(n.configuration.gyroOdrHz, c.rate);

        const double accDatasheet = 110e-6*g, accStep = 16./32768*g, accBandwidth = c.rate/2;
        const double accSigma = std::sqrt(accDatasheet*accDatasheet*accBandwidth + accStep*accStep/12);
        const SensorNoise &a = n.accelerometer;
        QVERIFY(a.range == 16 && a.rate == c.rate);
        QVERIFY(a.datasheetDensity == accDatasheet);
        QVERIFY(a.bandwidth == accBandwidth);
        QVERIFY(a.step == accStep && a.latticeStep == accStep);
        QVERIFY(a.sampleSigma == accSigma);
        QVERIFY(a.density == accSigma*std::sqrt(1/c.rate));
        QVERIFY(a.sensitivityTolerance == .01);

        const double gyroDatasheet = 3.8e-3*radians, gyroStep = 70e-3*radians;
        const double gyroSigma = std::sqrt(gyroDatasheet*gyroDatasheet*c.gyroBandwidth + gyroStep*gyroStep/12);
        const SensorNoise &w = n.gyroscope;
        QVERIFY(w.range == 2000 && w.rate == c.rate);
        QVERIFY(w.datasheetDensity == gyroDatasheet);
        QVERIFY(w.bandwidth == c.gyroBandwidth);
        QVERIFY(w.step == gyroStep);
        QVERIFY(w.latticeStep == 70e-3);
        QVERIFY(w.sampleSigma == gyroSigma);
        QVERIFY(w.density == gyroSigma*std::sqrt(1/c.rate));
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
                QVERIFY(n.gyroscope.latticeStep == gyro.step);
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
                                             gtsam::imuBias::ConstantBias(), ImuNoise{}));
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
        const auto pim = preintegrateImu(one, 0, step, gtsam::imuBias::ConstantBias(), noise);
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

    const auto one = preintegrateImu(signalSamples({0, dt}, force, rate), 0, dt, gtsam::imuBias::ConstantBias(), noise);
    std::vector<double> fine;
    for (int j = 0; j <= 1000; ++j)
        fine.push_back(dt*j/1000);
    fine.back() = dt;
    const auto many = preintegrateImu(signalSamples(fine, force, rate), 0, dt, gtsam::imuBias::ConstantBias(), noise);

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
    for (const char *name : {"motion_start", "rest_throughout", "sacc_anchor", "drifting_bias"})
        fixtures.append(initializerFixture(QLatin1String(name)));
    QCOMPARE(fixtures.size(), 18);
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
    // diagnostics' JSON round-trips a double exactly); model holds the noise
    // and the gyro bias model; stopping reports the damping ceiling.
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
    QCOMPARE(model.keys(), QStringList({QStringLiteral("gyro_bias"), QStringLiteral("noise")}));
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
    const Rot3 forward = propagateAttitude(d, r, .037, .863, bg);
    const Rot3 backward = propagateAttitude(d, forward, .863, .037, bg);
    QVERIFY(Rot3::Logmap(forward.between(r)).norm() > 1e-3);
    QVERIFY(Rot3::Logmap(backward.between(r)).norm() < 1e-12);
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
        "algorithm", "anchor_time_s", "configuration", "dense_output", "end_s", "gnss_states", "imu_outputs",
        "initialization", "initializer", "input", "limitations", "max_endpoint_correction_deg",
        "max_seed_vs_selected_acceleration_m_s2", "max_seed_vs_selected_angle_deg", "max_step_correction_m_s2",
        "max_step_correction_time_s", "max_velocity_mismatch_m_s", "model", "objective",
        "orientation", "quality", "residuals", "seed_comparison_performed", "seeds", "selected_heading_deg",
        "start_s", "stationary_interval_s", "stopping"}));
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
    for (const char *name : {"motion_start", "rest_throughout", "sacc_anchor", "drifting_bias"}) {
        const FusionFixture a = initializerFixture(QLatin1String(name));
        const FusionFixture b = initializerFixture(QLatin1String(name));
        QCOMPARE(a.name, QLatin1String(name));
        QCOMPARE(b.name, a.name);
        QCOMPARE(a.originIndex, b.originIndex);
        QVERIFY(a.expectSuccess);
        // Every recording states its configuration: +/-16 g, +/-2000 deg/s and
        // a listed rate within 4 % of its sampling (12.5 Hz for the two that
        // log at 12.5 Hz, 26 Hz for the two at 25 Hz).
        QCOMPARE(a.accelFsG, 16.);
        QCOMPARE(a.gyroFsDegS, 2000.);
        QCOMPARE(a.accelOdrHz, b.accelOdrHz);
        QCOMPARE(a.gyroOdrHz, a.accelOdrHz);
        const bool slow = a.name == QStringLiteral("sacc_anchor") || a.name == QStringLiteral("drifting_bias");
        QCOMPARE(a.accelOdrHz, slow ? 12.5 : 26.);
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
    // iterations, under the datasheet's noise).
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
    QCOMPARE(diagnostics.value("algorithm").toString(), QStringLiteral("batch-temperature-bias-v6"));

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
        QVERIFY(std::isnan(s.lastPassMeanRelativeDecrease) && std::isnan(s.repreintegrationCostDifference));

        const QJsonObject diagnostics = failureDiagnostics(QString::fromLatin1(failure.failure), &s);
        QCOMPARE(diagnostics.keys(), QStringList({QStringLiteral("algorithm"), QStringLiteral("failure"),
                                                  QStringLiteral("stopping")}));
        QCOMPARE(diagnostics.value("algorithm").toString(), QStringLiteral("batch-temperature-bias-v6"));
        QCOMPARE(diagnostics.value("failure").toString(), QString::fromLatin1(failure.failure));
        const QJsonObject stopping = diagnostics.value("stopping").toObject();
        QCOMPARE(stopping.value("rule").toString(), QString::fromLatin1(failure.rule));
        QCOMPARE(stopping.value("passes").toInt(), 2);
        QVERIFY(stopping.value("last_pass_mean_relative_decrease").isNull());
        QVERIFY(stopping.value("repreintegration_cost_difference").isNull());
        QCOMPARE(stopping.value("lambda_upper_bound").toDouble(), 1e12);
        QCOMPARE(stopping.value("slow_tail").toObject().value("window").toInt(), 20);
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
    // `damping saturated` instead, with the failure diagnostics' shape of a
    // failed pass (no quality: no pass completed). The forcing makes every
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
    // depend on it. The three success fixtures, through the whole pipeline
    // (coarse_maneuver's four prefix fits, its segment fit and the full fit
    // among them), are the same bits under a 1e5 and the default 1e12
    // ceiling: every channel, every iteration of the trace and the
    // initializer's account; the diagnostics differ in the ceiling they
    // report and nowhere else.
    Tuning lowCeiling;
    lowCeiling.lambdaUpperBound = 1e5;
    for (const char *name : {"coarse_linear", "coarse_maneuver", "stationary_spin"}) {
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
    const auto pim = preintegrateImu(d, .037, .863, linearizedAt, fixtureNoise(104));
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
    const auto graph = buildFactorGraph(d, BiasLinearization{ConstantBias(), Vector3::Zero()}, model, tuningAt(104));
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
    fit.values.insert(X(1), gtsam::Pose3(propagateAttitude(d, Rot3(), .037, .863, intervalGyroBias), Vector3::Zero()));
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
    // The spec's "constant temperature" case: with T_k - T_ref exactly zero
    // at every fix the factor's H6 is zero, the slope's normal equation is
    // its prior's alone with a zero right-hand side, and every LM step
    // leaves it at 0.0. The bound is 1 % of the prior sigma, the margin
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
    QCOMPARE(diagnostics.value("algorithm").toString(), QStringLiteral("batch-temperature-bias-v6"));
    const QJsonObject gyroBias = diagnostics.value("model").toObject().value("gyro_bias").toObject();
    const QJsonArray b1 = gyroBias.value("b1_rad_s_per_degc").toArray();
    QCOMPARE(b1.size(), 3);
    for (const QJsonValue &component : b1)
        QVERIFY(std::abs(component.toDouble(1)) < .01*Tuning{}.gyroBiasSlopeSigma);
    // 2501 copies of 35: the sum 87535 and the quotient are exact.
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
    derived.noise = imuNoise(toChannels(f).imuConfiguration);
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
    f.tuning.maxGap = kImuGapMedians*medianInterval(d.imuTime);
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
    // datasheet's densities the two differ by more, linearly: by 2.5e-4 (attitude),
    // 7.8e-3 (velocity) and 2.9e-3 (position) of the mismatch, which falls to
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
    f.tuning.maxGap = kImuGapMedians*medianInterval(d.imuTime);
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

    // The bias-corrected reading at edge `e` rotated by the corrected
    // attitude there, with the bias of the interval of step `step` (the
    // reconstruction's trapezoid of that step).
    const auto rotatedReading = [&](size_t e, size_t step) {
        const Vector3 reading = interpolateAt(f.window.imuTime, f.window.force, w.edges[e]);
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
    // pass on the fit, bit for bit, not something like it. The seventeen
    // channels of runPipeline() against reconstructAtImuRate() on this
    // executable's own fit of the fixture (fixtureFit(), the pipeline's
    // stages in its order) through fillOutputChannels(), and the four
    // numbers of the diagnostics against the pass's summaries. Section 6 and
    // decision 12: the published time axis is the fixture's IMU samples in
    // [first fix, last fix) of the window, in the number imu_outputs says.
    QFETCH(QString, name);
    const Fusion::Channels channels = toChannels(fusionFixture(name));
    const Fusion::Result result = runPipeline(channels, Tuning{}, Checkpoint());
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
    const QJsonObject diagnostics = diagnosticsOf(result);

    const WindowFit &f = fixtureFit(name);
    QVERIFY(f.fit.converged);
    const ImuRateTrajectory out = reconstructAtImuRate(f.window, f.fit, f.tuning);
    Fusion::Result expected;
    fillOutputChannels(out, prepareInput(channels).epoch, expected);
    for (const QString &channel : fusionChannelNames()) {
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

FLYSIGHT_TEST_MAIN(FusionKernelTest)
#include "tst_fusion_kernel.moc"
