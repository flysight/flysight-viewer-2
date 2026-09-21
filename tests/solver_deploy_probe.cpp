// Deployment probe for the solver runtime libraries.
//
// A plain executable (no Qt, no test framework) around the same GTSAM
// exercise as tst_solver_smoke. It is not a CTest test and is never
// installed: it is copied into an installed application tree and run there
// with a minimal PATH, where it can only start if every solver library it
// needs was deployed next to it. A QtTest executable cannot do this, because
// Qt Test is not part of the deployed application.
//
// It runs with the default main-thread stack on purpose; the tiny graph of
// the probe needs no more.

#include <cstdio>

#include "solverprobe.h"

int main()
{
    const FlySightTest::SolverProbeResult result = FlySightTest::runSolverProbe();
    if (!result.ok) {
        std::printf("solver probe FAILED: %s\n", result.detail.c_str());
        return 1;
    }
    std::printf("solver probe ok, error=%g\n", result.error);
    return 0;
}
