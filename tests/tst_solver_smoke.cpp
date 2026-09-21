// Solver dependency smoke test.
//
// Proves that GTSAM's exported CMake target compiles, links and runs in a
// test executable, that the install it came from is the configuration
// FlySight ships (4.3a0, TBB, bundled Eigen, built without Boost), and that
// flysight_solver_stack() really gives the main thread its 64 MiB stack.
//
// Nothing from the fusion model is used here; see solverprobe.h. This is a
// fusion-gated test (FLYSIGHT_BUILD_FUSION_TESTS, label "fusion"): it is one
// of the few targets allowed to link GTSAM.

#include <cstddef>

#include <QtTest>

#include <Eigen/Core>
#include <gtsam/config.h>

#include "solverprobe.h"
#include "testmain.h"

using namespace FlySightTest;

class TstSolverSmoke : public QObject {
    Q_OBJECT

private slots:
    void gtsamIsTheRightBuild();
    void smallGraphOptimizes();
    void mainThreadHasSolverStack();
};

void TstSolverSmoke::gtsamIsTheRightBuild()
{
    QCOMPARE(QString::fromLatin1(GTSAM_VERSION_STRING), QStringLiteral("4.3a0"));

#ifndef GTSAM_USE_TBB
    QFAIL("GTSAM was built without TBB (GTSAM_USE_TBB is not defined)");
#endif

    // config.h always defines both macros, as 0 or 1. FlySight ships the
    // Boost-free configuration; cmake/SolverDependencies.cmake refuses any
    // other at configure time, and this is the same statement at test level.
    QCOMPARE(GTSAM_ENABLE_BOOST_SERIALIZATION, 0);
    QCOMPARE(GTSAM_USE_BOOST_FEATURES, 0);

    // The Eigen that reaches this translation unit must be the one bundled
    // with GTSAM (3.4), supplied by the exported target, not a system Eigen.
    QCOMPARE(EIGEN_WORLD_VERSION, 3);
    QCOMPARE(EIGEN_MAJOR_VERSION, 4);
}

void TstSolverSmoke::smallGraphOptimizes()
{
    const SolverProbeResult result = runSolverProbe();
    QVERIFY2(result.ok, result.detail.c_str());
}

void TstSolverSmoke::mainThreadHasSolverStack()
{
    // Default main-thread stacks are 1 MiB (Windows) or 8 MiB (macOS, Linux).
    // Without flysight_solver_stack() this call overflows the stack and the
    // process dies, which is the failure this test exists to produce.
    constexpr std::size_t kBytes = 48u * 1024u * 1024u;
    QVERIFY(consumeStack(kBytes) >= kBytes);
}

FLYSIGHT_TEST_MAIN(TstSolverSmoke)
#include "tst_solver_smoke.moc"
