// Linux half of flysight_solver_stack() (cmake/SolverDependencies.cmake).
//
// GTSAM's elimination of a large factor graph overflows a default 8 MiB stack,
// so an executable that runs a fit on its main thread needs 64 MiB. On Windows
// and macOS the linker sizes the main thread's stack (/STACK, -stack_size).
// Linux has no such link option: the main thread's stack is not allocated up
// front, it grows on demand up to the RLIMIT_STACK soft limit, and glibc
// ignores "-z stack-size".
//
// What does work is raising the soft limit from inside the process. The kernel
// checks the current soft limit each time the main stack grows, and at exec
// time it leaves a gap below the stack base that is large enough for the
// stack to grow to this size, so the new limit takes effect immediately and
// no re-exec is needed. A constructor function does it before main() runs.
//
// This file is added to a target only on Linux. Anywhere else it compiles to
// an empty translation unit. It depends on nothing but libc.
//
// Unverified until the first CI run on Linux.

#ifdef __linux__

#include <sys/resource.h>

#include <cstdio>

namespace {

constexpr rlim_t kSolverStackBytes = 64ull * 1024ull * 1024ull;

__attribute__((constructor)) void raiseMainThreadStackLimit()
{
    rlimit limit{};
    if (getrlimit(RLIMIT_STACK, &limit) != 0)
        return;

    // RLIM_INFINITY compares greater than any finite size, so an unlimited
    // stack is left alone.
    if (limit.rlim_cur >= kSolverStackBytes)
        return;

    // An unprivileged process may raise its soft limit up to the hard limit
    // and no further.
    rlim_t wanted = kSolverStackBytes;
    if (limit.rlim_max != RLIM_INFINITY && limit.rlim_max < wanted)
        wanted = limit.rlim_max;

    if (wanted < kSolverStackBytes) {
        std::fprintf(stderr,
                     "solver stack: the hard stack limit (%llu bytes) is below 64 MiB; "
                     "large fits on the main thread may overflow the stack\n",
                     static_cast<unsigned long long>(limit.rlim_max));
    }

    limit.rlim_cur = wanted;
    if (setrlimit(RLIMIT_STACK, &limit) != 0)
        std::perror("solver stack: setrlimit(RLIMIT_STACK)");
}

} // namespace

#endif // __linux__
