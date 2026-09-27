# Phase 1: Solver dependencies

## Overview

This phase makes GTSAM 4.3a0 (with its bundled Eigen, **built without Boost**) and oneTBB 2022.1.0 pinned members of the third-party superbuild, next to GeographicLib and KDDockWidgets, and makes them available to the application build through GTSAM's exported CMake target `gtsam`. It deploys their runtime libraries with the application on Windows, macOS and Linux from the exported targets (never from file globs), caches the new install directories in CI, documents the build, and provides the reusable CMake helpers later phases need (64 MiB stack, test environment, a GTSAM-gated test function).

Two decisions by Michael, made after the plan was first written, shape this document (both are "let's try" decisions with a documented way back, see "Appendix: fallbacks"):

1. **GTSAM is pinned to the official repository**, `https://github.com/borglab/gtsam.git` at `814a734d68cbf5068a4bf20d63ba67c4935905bb` (4.3a0), not to the fork commit `8938b9f` the branch used.
2. **GTSAM is built without Boost on all three platforms** (`-DGTSAM_ENABLE_BOOST_SERIALIZATION=OFF -DGTSAM_USE_BOOST_FEATURES=OFF`). GTSAM then needs no Boost to build, its package config asks for none, and no Boost or ICU library is deployed. This phase adds **no** Boost requirement of any kind; the application's own, pre-existing header-only Boost use is untouched here (it ends in Phase 3, and Phase 10 removes Boost from the build, README and CI).

Nothing in the product links GTSAM after this phase. The only consumers are a QtTest smoke test and a tiny deployment probe, both gated by a new option. `flysight_model`, `flysight_core`, `flysight_test_support`, `FlySightViewer`, `flysight_cpp_bridge` and every existing test keep exactly the link lines they have today.

## Dependencies

- **Depends on:** None — can begin immediately.
- **Blocks:** Phase 8 (Fusion kernel port and golden parity), Phase 10.
- **Assumptions:**
  - Work happens on branch `sensor-fusion-jobs`; the implementation agent runs **no git command that changes repository state** (Commit Policy). Read-only git (`status`, `diff`, `log`, `show`, `ls-remote`, and `fetch` into a scratch repository *outside* this working tree) is fine.
  - On Michael's Windows machine these untracked trees already exist and are **never staged, never deleted, never cleaned**: `third-party/gtsam/`, `gtsam-build/`, `GTSAM-install/`, `oneTBB/`, `oneTBB-build/`, `oneTBB-install/`. `GTSAM-install` and `oneTBB-install` are a working Release install (VS 2022, TBB on) of the **fork commit built with Boost** (its `include/gtsam/config.h` has `GTSAM_ENABLE_BOOST_SERIALIZATION 1` and `GTSAM_USE_BOOST_FEATURES 1`, Boost 1.87 static). It is **not** the configuration this plan ships and is **not** the reference install for this phase or for the port. It stays on disk, untouched, for exactly one purpose: Phase 8's cross-check that the Boost-enabled and Boost-free builds give byte-identical fusion output.
  - **Local Boost-free install location (decided).** On Michael's machine the default install directories (`third-party/GTSAM-install`, `third-party/oneTBB-install`) are occupied by that old install, so the new build never goes there. The Boost-free upstream build is made once, in this phase, by a third-party-only superbuild in **`build-solver-deps/`** at the repository root, installing to **`build-solver-deps/GTSAM-install`** and **`build-solver-deps/oneTBB-install`** ("Manual Verification" step 1). `build*/` is git-ignored and is on the Commit Policy's never-stage list, so no new untracked path appears in `git status`. That directory is **kept** after this phase: it is the local solver install for phases 8, 9 and 10 and for every local application configure from now on, which passes `-DGTSAM_INSTALL_DIR=<repo>/build-solver-deps/GTSAM-install -DONETBB_INSTALL_DIR=<repo>/build-solver-deps/oneTBB-install` (the root project forwards them as `GTSAM_ROOT` / `ONETBB_ROOT`). Defaults, CI and fresh clones are unaffected: there the superbuild installs to `third-party/GTSAM-install` and `third-party/oneTBB-install` as usual.
  - Local toolchain: Visual Studio 2022, CMake 3.31.5, Qt 6.9.3 at `C:/Qt/6.9.3/msvc2022_64`, Boost 1.87 at `C:/Program Files/Boost/boost_1_87_0` (needed only by the application's header-only `boost::geometry` use until Phase 3; GTSAM does not use it), Python 3.13.3.
  - macOS and Linux cannot be verified before a push. Everything for those platforms is written carefully from the reference and flagged unverified; `Phase 1 fixup` commits after the first push are expected (overview, "Risks").

### Facts established during documentation (do not re-derive)

- **Revisions.** The donor branch `sensor_fusion_and_pitot` pinned both libraries as gitlinks: `third-party/gtsam` at `8938b9f158fa2f88ccfe3c31069a1452456b7eca` and `third-party/oneTBB` at `d3ad09cd7f69d3f50a3972bee9eb7fc8ee089b6e`. `sensor-fusion-clean-port` replaced the gitlinks with `ExternalProject` downloads pinned to the same two SHAs (`branch:cmake/SolverSuperbuild.cmake`).
- **`8938b9f` is not an upstream commit.** It exists only on Michael's fork `https://github.com/crwper/gtsam.git` (branch `fix/disable-js`), one commit on top of upstream `814a734d68cbf5068a4bf20d63ba67c4935905bb` ("Merge pull request #2017 from borglab/feature/city_relin", 2025-02-03). The fork commit changes exactly one line, `gtsam/3rdparty/GeographicLib/CMakeLists.txt` (`#add_subdirectory (js)`), and that directory is only entered when `GTSAM_INSTALL_GEOGRAPHICLIB=ON`. With `GTSAM_INSTALL_GEOGRAPHICLIB=OFF` (required below) the patch is never reached and the library sources of the two revisions are identical, which is why **this plan pins upstream `814a734…`** (Michael's decision) and keeps the fork only as a fallback. Read-only check: `git --git-dir=.git/modules/third-party/gtsam diff --stat 814a734 8938b9f`. The untracked `third-party/gtsam` tree is the fork revision, i.e. upstream plus that one line.
- **GTSAM's Boost-free build, read from the source tree (`third-party/gtsam`).**
  - Root `CMakeLists.txt` lines 57-63 define `GTSAM_ENABLE_BOOST_SERIALIZATION` and `GTSAM_USE_BOOST_FEATURES` (both default `ON`) and include `cmake/HandleBoost.cmake` **only** `if(GTSAM_ENABLE_BOOST_SERIALIZATION OR GTSAM_USE_BOOST_FEATURES)`. That file is the only place that calls `find_package(Boost …)`, forces `Boost_USE_STATIC_LIBS` on MSVC, and sets `GTSAM_BOOST_LIBRARIES`. With both options `OFF` it is never read, `GTSAM_BOOST_LIBRARIES` is empty, and `gtsam/CMakeLists.txt` line 138 (`target_link_libraries(gtsam PUBLIC ${GTSAM_BOOST_LIBRARIES})`) adds nothing.
  - `cmake/Config.cmake.in` lines 18-24 wrap `find_dependency(Boost …)` in `if (@GTSAM_ENABLE_BOOST_SERIALIZATION@ OR @GTSAM_USE_BOOST_FEATURES@)`; the generated `GTSAMConfig.cmake` therefore contains `if (OFF OR OFF)` and only `find_dependency(TBB 4.4 COMPONENTS tbb tbbmalloc)` runs.
  - `gtsam/config.h.in` lines 101-102 use `#cmakedefine01`, so the installed `config.h` always defines both macros, as `0` in this build.
  - What `GTSAM_USE_BOOST_FEATURES` switches in library code (tests aside): `base/timing.h` / `timing.cpp` (Boost timers versus `std::chrono`), `base/concepts.h` (Boost concept checks versus no-op macros), and a per-iteration timer in `nonlinear/LevenbergMarquardtOptimizer.cpp` that only feeds the `SUMMARY` verbosity printout. It also drops `inference/graph.h` / `graph-inl.h` (Boost.Graph; nothing else in the library includes them). `GTSAM_ENABLE_BOOST_SERIALIZATION` switches the `serialize` member templates and the `boost/serialization` includes in headers (`#if`-guarded throughout; `base/serialization.h` and `serializationTestHelpers.h` are guarded as whole files, so the MSVC precompiled header still compiles). A scan of every non-test, non-3rdparty file under `gtsam/` found no `#include <boost…>` or `boost::` use outside such a guard other than in `graph.h` / `graph-inl.h` and comments. None of this touches factor, preintegration, linearization, ordering or elimination code.
  - Bundled METIS and cephes do not use Boost. The default allocator with `GTSAM_WITH_TBB=ON` is `TBB` (`cmake/HandleAllocators.cmake`; `BoostPool` is only offered when TBB is off). `GTSAM_BUILD_UNSTABLE=OFF` keeps `gtsam_unstable` (which has further Boost-only parts) out of the build.
  - Headers the fusion code uses (`branch:src/batchfusion.h` / `.cpp`, `imugnssekf.cpp`): `gtsam/geometry/Rot3.h`, `gtsam/navigation/ImuFactor.h`, `gtsam/navigation/GPSFactor.h`, `gtsam/nonlinear/NonlinearFactorGraph.h`, `gtsam/nonlinear/Values.h`, `gtsam/nonlinear/LevenbergMarquardtOptimizer.h`, `gtsam/slam/PriorFactor.h`, `gtsam/inference/Symbol.h`. In all of them Boost appears only inside `#if GTSAM_ENABLE_BOOST_SERIALIZATION`. The branch's fusion sources contain no `boost` token at all (GTSAM 4.3 uses `std::shared_ptr` / `std::optional`), so they compile unchanged against a Boost-free GTSAM.
  - GTSAM's own CI builds this configuration on **Ubuntu only** (`.github/workflows/build-special.yml`, job `ubuntu-no-boost`); `INSTALL.md` (line 101) documents it for Windows. A Boost-free **shared MSVC** build and a Boost-free **macOS** build are not covered by upstream CI: the Windows one is verified locally in this phase, the macOS one only after a push.
- **oneTBB `d3ad09c`** is on `https://github.com/uxlfoundation/oneTBB.git` `master` (2025-02-07); its `version.h` says 2022.1.0.
- **The untracked `third-party/gtsam` and `third-party/oneTBB` have no `.git`**; they are plain source trees (`.git/modules/third-party/{gtsam,oneTBB}` are leftovers of the donor branch's submodules). `third-party/gtsam-build` was configured with the Visual Studio generator from that source, with `GTSAM_INSTALL_GEOGRAPHICLIB=ON`; that is why the existing `GTSAM-install` also contains `Geographic.lib`, a `geographiclib-config.cmake`, and a copy of the MSVC runtime DLLs in `bin/` (the concrete reason deployment must not glob that directory).
- **Exported targets.** *Read from the old Boost-enabled install* (`third-party/GTSAM-install/CMake/GTSAM-exports.cmake`): `gtsam` (SHARED) with `INTERFACE_LINK_LIBRARIES` `Boost::serialization;Boost::system;Boost::filesystem;Boost::thread;Boost::date_time;Boost::regex;Boost::timer;Boost::chrono;TBB::tbb;TBB::tbbmalloc;metis-gtsam-if;cephes-gtsam-if;gtsam_eigen3`, public definitions `_ENABLE_EXTENDED_ALIGNED_STORAGE;EIGEN_NO_STATIC_ASSERT`, `cxx_std_17`, and the bundled Eigen include directory through `gtsam_eigen3`; its `GTSAMConfig.cmake` calls `find_dependency(Boost 1.65 COMPONENTS …)` and `find_dependency(TBB 4.4 COMPONENTS tbb tbbmalloc)`. *Expected for the Boost-free build* (derived from the CMake sources above; the implementer confirms it against the new install and reports the actual value): `INTERFACE_LINK_LIBRARIES` = `TBB::tbb;TBB::tbbmalloc;metis-gtsam-if;cephes-gtsam-if;gtsam_eigen3` — the same list without the eight `Boost::` entries — the same definitions and features, and a `GTSAMConfig.cmake` whose only effective dependency lookup is TBB. The config directory is `<prefix>/CMake` on Windows and `<prefix>/lib/cmake/GTSAM` elsewhere; TBB's is `<prefix>/lib/cmake/TBB`.
- **Windows runtime set:** `gtsam.dll`, `metis-gtsam.dll`, `cephes-gtsam.dll` (GTSAM install `bin/`), `tbb12.dll`, `tbbmalloc.dll` (oneTBB install `bin/`). Unchanged by the Boost decision: Boost was static on MSVC in the old build and is absent in the new one.
- **Why Boost-free (background for the decision).** Homebrew's `boost` is 1.92 (2026-09); Boost 1.89 removed the compiled `boost_system` library, which a Boost-enabled GTSAM 4.3a0 requires as a component, so a Boost-enabled build would cap Boost at 1.88, pin macOS CI to `boost@1.85`, and ship shared Boost and ICU libraries on macOS and Linux (with an unresolved macOS deployment-target mismatch). All of that disappears with the two options `OFF`. Details of the abandoned design are in "Appendix: fallbacks".
- The branch was never built on macOS or Linux (`branch:docs/PORT_VALIDATION.md`, "Limits"). Its `flysight_solver_stack` handles MSVC and MinGW only.

## Tasks

### Task 1.1: Superbuild definitions for oneTBB and GTSAM

**Purpose:** Build and install the two libraries at pinned revisions with the options the reference used plus the two Boost switches set `OFF`, from the same superbuild that builds GeographicLib and KDDockWidgets.

**Files to create:**
- `cmake/SolverSuperbuild.cmake` — pinned revisions, options, `ext_oneTBB`, `ext_GTSAM`, clean targets.

**Files to modify:**
- `cmake/ThirdPartySuperbuild.cmake` — include the new file; extend `clean-third-party` and the status summary.
- `CMakeLists.txt` (root) — install-dir cache variables, `DEPENDS`, forwarding to the application build, option comments, summary lines.
- `third-party/CMakeLists.txt` — the two install-dir cache variables and summary lines for the standalone third-party build.

**Technical Approach:**

*Pinning mechanism (decided; see "Decisions Made").* The revisions are recorded as full commit SHAs in `cmake/SolverSuperbuild.cmake` and fetched by `ExternalProject_Add(GIT_REPOSITORY … GIT_TAG <sha>)`. No submodule is added and `.gitmodules` is not touched, so the Commit Policy's submodule clause does not come into play and nothing under `third-party/` is staged.

```
set(GTSAM_REPOSITORY  https://github.com/borglab/gtsam.git)
set(GTSAM_REVISION    814a734d68cbf5068a4bf20d63ba67c4935905bb)  # borglab/gtsam, 4.3a0 (2025-02-03)
set(ONETBB_REPOSITORY https://github.com/uxlfoundation/oneTBB.git)
set(ONETBB_REVISION   d3ad09cd7f69d3f50a3972bee9eb7fc8ee089b6e)  # uxlfoundation/oneTBB, 2022.1.0
```

`GIT_REPOSITORY` for GTSAM is the **official repository** (decided by Michael after planning). Keep repository and revision as one adjacent pair of `set()` lines per library so a pin is changed in one place. Put a comment above the GTSAM pair recording the fact from "Facts established": the fusion work was developed against fork commit `8938b9f…` of `https://github.com/crwper/gtsam.git`, which is this upstream commit plus one line of bundled-GeographicLib CMake (`#add_subdirectory (js)`) that only matters when `GTSAM_INSTALL_GEOGRAPHICLIB=ON`; this build sets it `OFF`, so upstream is used as is. **Fallback:** if upstream `814a734…` fails to configure or build for a reason that one-line patch fixes (an error under `gtsam/3rdparty/GeographicLib/js`), switch the pair to `https://github.com/crwper/gtsam.git` / `8938b9f158fa2f88ccfe3c31069a1452456b7eca`, change nothing else, and say so prominently in the implementation report. Any other upstream build failure is reported, not worked around by switching the pin.

*Structure of `cmake/SolverSuperbuild.cmake`* (start from `git show sensor-fusion-clean-port:cmake/SolverSuperbuild.cmake`, retyped with the header-comment style of `cmake/ThirdPartySuperbuild.cmake` lines 58-73 — a banner, what the library is for, and a "Key options" list explaining each flag):

1. `option(FLYSIGHT_BUILD_SOLVER_DEPS "Build GTSAM and oneTBB with the other third-party dependencies" ON)`. When `OFF`, the file defines nothing else and returns; the application build then uses whatever is already in `GTSAM_INSTALL_DIR` / `ONETBB_INSTALL_DIR`. This exists so a developer with working installs (Michael's machine: `build-solver-deps/…`) can keep `FLYSIGHT_BUILD_THIRD_PARTY=ON` for GeographicLib/KDDockWidgets without a 30-60 minute GTSAM rebuild, and so that an everyday build can never write into the default install directories, where his old Boost-enabled install lives.
2. Cache variables, each only if not already defined by the includer: `GTSAM_INSTALL_DIR` (`${THIRD_PARTY_DIR}/GTSAM-install`), `ONETBB_INSTALL_DIR` (`${THIRD_PARTY_DIR}/oneTBB-install`), and `GTSAM_SOURCE_DIR` / `ONETBB_SOURCE_DIR` (default empty: "existing source tree to build instead of downloading the pinned revision").
3. **No Boost handling.** The file does not include `BoostDiscovery.cmake`, does not read `BOOST_ROOT` / `BOOST_LIBRARYDIR`, and passes no Boost variable to either project (the branch's file did all three; do not port that part).
4. Platform forwarding list: `CMAKE_OSX_DEPLOYMENT_TARGET`, `CMAKE_OSX_ARCHITECTURES`, `CMAKE_OSX_SYSROOT`, each only when defined; `CMAKE_PREFIX_PATH` is **not** forwarded (neither library needs Qt).
5. If `CMAKE_VERSION VERSION_GREATER_EQUAL 4.0`, add `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` to **both** projects (GTSAM's bundled METIS declares `cmake_minimum_required(VERSION 3.0)`, which CMake 4 rejects; harmless for oneTBB).
6. `ExternalProject_Add(ext_oneTBB …)` then `ExternalProject_Add(ext_GTSAM … DEPENDS ext_oneTBB)`.

Common `ExternalProject` arguments, following `ext_GeographicLib` (`cmake/ThirdPartySuperbuild.cmake` lines 75-94): `CMAKE_GENERATOR "${CMAKE_GENERATOR}"`, `CMAKE_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}"` (never `-G`/`-A` in `CMAKE_ARGS`), `UPDATE_COMMAND ""`, `-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}`, `-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>`.

Source and build locations — these differ from GeographicLib on purpose:
- Download: `GIT_REPOSITORY`, `GIT_TAG ${…_REVISION}`, `GIT_SUBMODULES ""`, `SOURCE_DIR "${CMAKE_BINARY_DIR}/solver-sources/<name>"`. When `<X>_SOURCE_DIR` is non-empty: `SOURCE_DIR "${<X>_SOURCE_DIR}"` and `DOWNLOAD_COMMAND ""`.
- `BINARY_DIR "${CMAKE_BINARY_DIR}/oneTBB-build"` and `"${CMAKE_BINARY_DIR}/gtsam-build"`. **Not** `third-party/gtsam-build` / `third-party/oneTBB-build`: those exist on Michael's machine, configured from a different source path, and CMake refuses a build directory whose cached source differs.
- `INSTALL_DIR "${ONETBB_INSTALL_DIR}"` / `"${GTSAM_INSTALL_DIR}"`.

Exact options.

| Project | `CMAKE_ARGS` (in addition to the common ones) |
|---|---|
| `ext_oneTBB` | `-DBUILD_SHARED_LIBS=ON` `-DTBB_TEST=OFF` `-DTBB_EXAMPLES=OFF` `-DTBB_STRICT=OFF` `-DCMAKE_INSTALL_LIBDIR=lib` + platform list |
| `ext_GTSAM` | `-DBUILD_SHARED_LIBS=ON` `-DGTSAM_INSTALL_GEOGRAPHICLIB=OFF` `-DGTSAM_BUILD_EXAMPLES_ALWAYS=OFF` `-DGTSAM_BUILD_TESTS=OFF` `-DGTSAM_BUILD_UNSTABLE=OFF` `-DGTSAM_BUILD_PYTHON=OFF` `-DGTSAM_WITH_TBB=ON` `-DGTSAM_USE_SYSTEM_EIGEN=OFF` `-DGTSAM_BUILD_WITH_MARCH_NATIVE=OFF` `-DTBB_DIR=${ONETBB_INSTALL_DIR}/lib/cmake/TBB` `-DGTSAM_ENABLE_BOOST_SERIALIZATION=OFF` `-DGTSAM_USE_BOOST_FEATURES=OFF` `-DCMAKE_POSITION_INDEPENDENT_CODE=ON` `-DCMAKE_INSTALL_LIBDIR=lib` + platform list |

The two Boost options are the only deliberate departure from the configuration the branch was validated with. They are explicitly `OFF` on every platform; in the header comment's "Key options" list explain them in one line each (no Boost needed to build, to configure a consumer, or at run time; serialization and Boost timers are unused by FlySight). They are not expected to change any computed number (see "Facts established"), and Phase 8 proves that with a cross-check against the old Boost-enabled build before any golden output is accepted.

Every other GTSAM option stays at its default; these defaults are part of the numerical contract and must not be overridden: `GTSAM_USE_QUATERNIONS=OFF`, `GTSAM_POSE3_EXPMAP=ON`, `GTSAM_ROT3_EXPMAP=ON`, `GTSAM_TANGENT_PREINTEGRATION=ON`, `GTSAM_SLOW_BUT_CORRECT_BETWEENFACTOR=OFF`, `GTSAM_SLOW_BUT_CORRECT_EXPMAP=OFF`, `GTSAM_DEFAULT_ALLOCATOR=TBB`, `GTSAM_WITH_EIGEN_MKL=OFF`, `GTSAM_SUPPORT_NESTED_DISSECTION=ON`. No `-ffast-math`, `/fp:fast`, `-march`, or custom `CMAKE_CXX_FLAGS` are passed. `-DCMAKE_INSTALL_LIBDIR=lib` matters: it keeps the configs at `lib/cmake/...` on distributions that default to `lib64`.

7. Clean targets following lines 153-168 of `ThirdPartySuperbuild.cmake`: `clean-oneTBB` and `clean-GTSAM`, each removing its build directory under `${CMAKE_BINARY_DIR}` and its install directory. They never remove a source directory.

*`cmake/ThirdPartySuperbuild.cmake` edits (three small hunks):* `include("${CMAKE_CURRENT_LIST_DIR}/SolverSuperbuild.cmake")` after the two existing `ExternalProject_Add` calls; make `clean-third-party` depend on `clean-oneTBB clean-GTSAM` only `if(TARGET clean-GTSAM)`; add the two targets, "ext_GTSAM: depends on ext_oneTBB", and build-order step 2 to the status summary (only when the targets exist). Update the file's header comment list of libraries.

*Root `CMakeLists.txt` edits (each a separate small hunk, nothing reordered):*
- Option comment block (lines 55-74) and a summary line for `FLYSIGHT_BUILD_SOLVER_DEPS` and `FLYSIGHT_BUILD_FUSION_TESTS` (the latter is defined in Task 1.4; declare it here as `option(FLYSIGHT_BUILD_FUSION_TESTS "With FLYSIGHT_BUILD_TESTS: also build the GTSAM-linked tests" ON)` next to `FLYSIGHT_BUILD_PYTHON_TESTS`, line 80).
- `GTSAM_INSTALL_DIR` and `ONETBB_INSTALL_DIR` cache paths after `KDDW_INSTALL_DIR` (line 104), and in the "Path Variables" summary.
- In the `THIRD_PARTY_TARGETS` block (lines 143-150), following the existing `if(TARGET …)` pattern: append `ext_oneTBB` and `ext_GTSAM` when they exist.
- In `_APP_CMAKE_ARGS` (lines 156-166): `-DGTSAM_ROOT=${GTSAM_INSTALL_DIR}`, `-DONETBB_ROOT=${ONETBB_INSTALL_DIR}`, `-DFLYSIGHT_BUILD_FUSION_TESTS=${FLYSIGHT_BUILD_FUSION_TESTS}`. Nothing about Boost is forwarded or changed in the root project: the application keeps finding its header-only Boost exactly as on `master` (through `cmake/BoostDiscovery.cmake` in `src/CMakeLists.txt`) until Phase 10 removes it.

*`third-party/CMakeLists.txt`:* add the two install-dir cache variables after `KDDW_INSTALL_DIR` (line 47) and two lines each to the "Install Directories" / "Build Targets" summary. `FLYSIGHT_BUILD_SOLVER_DEPS` needs nothing here (the option lives in `SolverSuperbuild.cmake`).

**Acceptance Criteria:**
- [ ] `cmake/SolverSuperbuild.cmake` records both revisions as 40-character SHAs with the upstream repository URLs (`borglab/gtsam` at `814a734…`, `uxlfoundation/oneTBB` at `d3ad09c…`), or, only if the documented fallback was needed, the fork pair with the reason stated in the file comment and in the report; `.gitmodules` is unchanged (`git diff --stat -- .gitmodules` is empty) and `git status --porcelain` shows no new entry under `third-party/` other than `third-party/CMakeLists.txt` modified.
- [ ] Both SHAs are fetchable from the named upstream repositories (verified with a shallow fetch into a scratch repository outside the working tree; command in "Manual Verification").
- [ ] A superbuild configured with `-DFLYSIGHT_THIRD_PARTY_ONLY=ON` defines `ext_oneTBB`, `ext_GTSAM` (depending on `ext_oneTBB`), `clean-oneTBB`, `clean-GTSAM`; with `-DFLYSIGHT_BUILD_SOLVER_DEPS=OFF` it defines none of them and still configures.
- [ ] On Windows, building target `ext_GTSAM` into the `build-solver-deps/` install directories ("Manual Verification" step 1) succeeds and produces `bin/gtsam.dll`, `bin/metis-gtsam.dll`, `bin/cephes-gtsam.dll`, a `GTSAMConfig.cmake`, `include/gtsam/3rdparty/Eigen/`, `include/gtsam/config.h` containing `#define GTSAM_USE_TBB`, `#define GTSAM_ENABLE_BOOST_SERIALIZATION 0` and `#define GTSAM_USE_BOOST_FEATURES 0`, and no `Geographic*.lib`; the oneTBB install has `bin/tbb12.dll`, `bin/tbbmalloc.dll`, `lib/cmake/TBB/TBBConfig.cmake`.
- [ ] The `CMakeCache.txt` of that GTSAM build shows the option values in the table above (including both Boost options `OFF`) and the listed defaults unchanged, and contains no `Boost_INCLUDE_DIR` / `Boost_DIR` entry (GTSAM never looked for Boost). The GTSAM configure succeeds with `BOOST_ROOT` unset in the environment.
- [ ] In the new install, the `gtsam` target has no Boost in its link interface and the package config asks for none: `grep -i boost <GTSAM install>/CMake/GTSAM-exports*.cmake` finds nothing, and the only Boost text in `GTSAMConfig.cmake` is inside an `if (OFF OR OFF)` block. `dumpbin /dependents <GTSAM install>/bin/gtsam.dll` lists no `boost_*.dll`.
- [ ] No build or install directory used by the new targets is `third-party/gtsam-build`, `third-party/oneTBB-build`, `third-party/gtsam`, or `third-party/oneTBB` unless the user passes them explicitly.
- [ ] Root `CMakeLists.txt` forwards `GTSAM_ROOT`, `ONETBB_ROOT` and `FLYSIGHT_BUILD_FUSION_TESTS` to the `FlySightViewer` external project.
- [ ] `grep -n -i boost cmake/SolverSuperbuild.cmake` finds only the two `-DGTSAM_…BOOST…=OFF` arguments and comments about them; `git diff master -- CMakeLists.txt third-party/CMakeLists.txt cmake/ThirdPartySuperbuild.cmake` contains no line mentioning Boost.

**Complexity:** M

---

### Task 1.2: Application-side discovery and reusable helpers

**Purpose:** Find GTSAM through its exported target in the application build, derive the runtime-library set from that target, and provide the helper functions phases 8 and 9 call.

**Files to create:**
- `cmake/SolverDependencies.cmake` — discovery, runtime-target collection, helper functions.
- `cmake/solver_stack_linux.cpp` — the Linux half of `flysight_solver_stack` (see below).

**Files to modify:**
- `src/CMakeLists.txt` — two cache-path lines in the "Third-party root" block (after line 67), one entry in the conventions comment (line 35-36 list), and one `include(...)` line directly after `find_package(Boost REQUIRED)` (line 221; that Boost lookup is the application's own and is not touched here). Nothing else in this task.

**Technical Approach:**

Start from `git show sensor-fusion-clean-port:cmake/SolverDependencies.cmake`; keep its structure, retype with comments at the density of `cmake/DeployThirdPartyMacOS.cmake`.

*Discovery.*
- In `src/CMakeLists.txt`: `set(GTSAM_ROOT "${THIRD_PARTY_DIR}/GTSAM-install" CACHE PATH "GTSAM install prefix")` and `set(ONETBB_ROOT "${THIRD_PARTY_DIR}/oneTBB-install" CACHE PATH "oneTBB install prefix")`, per that file's own rule that third-party paths are defined once in that block.
- In `SolverDependencies.cmake`: `list(PREPEND CMAKE_PREFIX_PATH "${GTSAM_ROOT}" "${ONETBB_ROOT}")`; `find_package(GTSAM 4.3 CONFIG REQUIRED)`; `FATAL_ERROR` if `NOT TARGET gtsam`. **Nothing about Boost is set here** (the branch's `Boost_USE_STATIC_LIBS` line is not ported): a Boost-free GTSAM's config never looks for Boost.
- **Boost-free guard.** Before `find_package`, read `<GTSAM_ROOT>/include/gtsam/config.h` with `file(STRINGS … REGEX "define GTSAM_(ENABLE_BOOST_SERIALIZATION|USE_BOOST_FEATURES) ")` and `FATAL_ERROR` if either macro is `1`. The message says that this GTSAM was built with Boost, that FlySight requires the Boost-free build made by the superbuild (`cmake/SolverSuperbuild.cmake`), and how to point `GTSAM_ROOT` / `GTSAM_INSTALL_DIR` at it. This is what stops a configure on Michael's machine from silently using the old `third-party/GTSAM-install` (the default `GTSAM_ROOT`), and what keeps a stale CI cache from passing. After `find_package`, also `FATAL_ERROR` if the `gtsam` target's `INTERFACE_LINK_LIBRARIES` matches `Boost::`. Precede the `find_package` with an existence check that fails with an actionable message naming `GTSAM_ROOT`, `FLYSIGHT_BUILD_SOLVER_DEPS` and the README section when no `GTSAMConfig.cmake` is under `GTSAM_ROOT`.
- The include goes **after** `find_package(GeographicLib … NO_DEFAULT_PATH)` (line 211): an old `GTSAM-install` may contain its own `geographiclib-config.cmake`, and the prefix path must not be extended before GeographicLib is resolved.
- GTSAM is `REQUIRED` for the application configure from this phase on: deployment is derived from its targets, and the application links it from Phase 9. "Tests build without GTSAM" (spec section 3) is about link lines, which Task 1.4 guarantees.

*Runtime collection.* A recursive function over `INTERFACE_LINK_LIBRARIES` starting at `gtsam`, as on the branch: unwrap `$<LINK_ONLY:…>`, skip non-targets and other generator expressions, keep a visited list in a global property, and record a target when its `TYPE` is `SHARED_LIBRARY`, or `UNKNOWN_LIBRARY` whose imported location (any configuration) ends in `.so`, `.so.<version>`, `.dylib` or `.dll` (kept for robustness; with a Boost-free GTSAM every expected runtime target is a `SHARED_LIBRARY`). Resolve `ALIAS` targets with `ALIASED_TARGET` before reading properties. Result: list variable `FLYSIGHT_SOLVER_RUNTIME_TARGETS`, printed with `message(STATUS)`. Expected on every platform: exactly `gtsam;TBB::tbb;TBB::tbbmalloc;metis-gtsam;cephes-gtsam` (order not significant); `FATAL_ERROR` if any collected target's name starts with `Boost::`. After collection, `FATAL_ERROR` if the list lacks any of `gtsam`, `metis-gtsam`, `cephes-gtsam`, `TBB::tbb`, `TBB::tbbmalloc` — that would mean GTSAM was built static or without TBB, which breaks both deployment and parity.

*Helper functions (public names; later phases rely on them).*

`flysight_solver_stack(<target>)` — gives the **main thread** of an executable a 64 MiB (67108864 byte) stack. Documented in a comment as "for test executables that run a fit on their main thread; the application does not need it, its fits run on the job queue's worker thread (Phase 5)".
- MSVC: `target_link_options(<target> PRIVATE /STACK:67108864)`.
- MinGW: `-Wl,--stack,67108864`.
- Apple: `target_link_options(<target> PRIVATE "LINKER:-stack_size,0x4000000")` (hex, a multiple of the 16 KiB page size; valid for main executables only).
- Linux: no linker option controls the main thread's stack (it is bounded by `RLIMIT_STACK`, and glibc ignores `-z stack-size`). The helper adds `cmake/solver_stack_linux.cpp` to the target with `target_sources(<target> PRIVATE …)`. That file contains one `__attribute__((constructor))` function that reads `RLIMIT_STACK` and, if the soft limit is below 64 MiB, raises it to `min(64 MiB, hard limit)` with `setrlimit`. The kernel checks the current soft limit each time the main stack grows and reserves at least 128 MiB below the stack base at exec time, so the raise takes effect without re-exec. If the hard limit is lower, print one line to `stderr` and continue. The file compiles to nothing unless `__linux__` is defined, has no Qt or GTSAM dependency, and carries a comment explaining why a link flag cannot do this.
- Any other platform: `message(WARNING)` once.

`flysight_solver_test_environment(<test>)` — lets a CTest test find the solver libraries without copying them, following the `ENVIRONMENT_MODIFICATION` pattern of `flysight_add_test` (`tests/CMakeLists.txt` lines 84-97): for each runtime target, prepend `$<TARGET_FILE_DIR:<tgt>>` to `PATH` on Windows, to `LD_LIBRARY_PATH` on Linux, to `DYLD_LIBRARY_PATH` on macOS; de-duplicate is unnecessary. Needs CMake ≥ 3.22; below that, `message(WARNING)` exactly as the existing helper does. (On Linux this is needed because Ubuntu links with `--as-needed` and `RUNPATH`, so `libgtsam.so`'s own dependency on `libtbb.so.12` in a different prefix is not covered by the executable's build RPATH.)

`flysight_install_solver_runtime(<destination>)` — Task 1.3.

No `flysight_copy_solver_runtime` is ported: on `master` nothing is copied next to build-tree executables (tests use `PATH`, the application is run from its install directory), and the solver libraries follow the same rule.

**Acceptance Criteria:**
- [ ] With the Boost-free installs in `build-solver-deps/`, configuring the application prints a "Solver runtime targets:" status line containing exactly `gtsam`, `metis-gtsam`, `cephes-gtsam`, `TBB::tbb`, `TBB::tbbmalloc`, and no `Boost::` target.
- [ ] Configuring with `-DGTSAM_ROOT=<repo>/third-party/GTSAM-install` (the old Boost-enabled install, Michael's machine only) fails with the Boost-free guard's message. Nothing in that directory is modified by the attempt.
- [ ] `grep -n -i boost cmake/SolverDependencies.cmake` finds only the guard and its message.
- [ ] Configuring with `-DGTSAM_ROOT=<nonexistent>` fails with a message that names `GTSAM_ROOT` and how to build or skip the solver dependencies.
- [ ] `git diff master -- src/CMakeLists.txt` for this task shows only: two `set(... CACHE PATH ...)` lines, the conventions-comment entry, and one `include` line.
- [ ] `grep -n "gtsam\|GTSAM\|TBB" src/CMakeLists.txt` finds no `target_link_libraries` line; `flysight_model`, `flysight_core`, `FlySightViewer` and `flysight_cpp_bridge` link exactly what they link on `master`.
- [ ] `flysight_solver_stack`, `flysight_solver_test_environment` and `flysight_install_solver_runtime` exist as CMake functions with a usage comment each; `cmake/solver_stack_linux.cpp` compiles to an empty translation unit on Windows and macOS (it is only added on Linux, but must be harmless anywhere).
- [ ] GeographicLib is still found from `GEOGRAPHIC_ROOT` (the configure log's "GeographicLib found" path is under `GeographicLib-install`, not `GTSAM-install`).

**Complexity:** M

---

### Task 1.3: Runtime deployment on Windows, macOS and Linux

**Purpose:** Ship the solver runtime libraries inside the installed application on all three platforms, selected by exported target, so the install is ready for Phase 9 to link the application to `flysight_fusion` with no further deployment work.

**Files to modify:**
- `cmake/SolverDependencies.cmake` — `flysight_install_solver_runtime`.
- `src/CMakeLists.txt` — three one-line calls, one per platform block.

**Technical Approach:**

Call sites (install rules run in declaration order, so position matters):
- Windows: `flysight_install_solver_runtime(".")` directly after `include(".../DeployThirdPartyWindows.cmake")` (line 667).
- macOS: `flysight_install_solver_runtime("FlySightViewer.app/Contents/Frameworks")` after the `deploy_third_party_macos(...)` block (line 760) and **before** the "Fixing macOS rpaths" `install(CODE …)` (line 763). `cmake/fix_macos_rpaths.sh` then rewrites every dylib in `Frameworks` to `@rpath/<name>` ids and references (GTSAM's install names are absolute, `CMAKE_INSTALL_NAME_DIR=<prefix>/lib`), and the final signing pass signs them. No change to that script or to `DeployThirdPartyMacOS.cmake` is needed.
- Linux: `flysight_install_solver_runtime("${FLYSIGHT_APPDIR_USR}/lib")` directly after `include(".../DeployThirdPartyLinux.cmake" OPTIONAL)` (line 947), before `RunLinuxDeployQt` / `CreateAppImage`.

`DeployThirdParty{Windows,MacOS,Linux}.cmake`, `CreateAppDir.cmake`, `CreateAppImage.cmake` and `fix_macos_rpaths.sh` are **not modified**.

Behaviour of `flysight_install_solver_runtime(<destination>)`:

1. **Windows:** for each runtime target, `install(FILES "$<TARGET_FILE:<tgt>>" DESTINATION "<destination>")`. The generator expression selects the configuration being installed.
2. **Unix:** exactly one real file per library in the destination, named by the name the loader asks for. For `SHARED_LIBRARY` targets that is `$<TARGET_SONAME_FILE_NAME:<tgt>>` (e.g. `libgtsam.so.4`, `libtbb.so.12`, `libgtsam.4.dylib`); for `UNKNOWN_LIBRARY` targets (not expected; see Task 1.2) it is the file name of the fully resolved (`REALPATH`) imported location. Source is always the resolved real file, so no symlink and no second copy is installed (the branch installed both the versioned file and a soname copy; GTSAM is tens of megabytes). Resolving at configure time with `get_target_property(... IMPORTED_LOCATION_<CONFIG>)` / `IMPORTED_SONAME_<CONFIG>` + `get_filename_component(… REALPATH)` is preferred over generator expressions in `RENAME`; pick the configuration from `CMAKE_BUILD_TYPE`, falling back to the first entry of `IMPORTED_CONFIGURATIONS`, then to the unsuffixed property.
3. **No dependency closure.** With a Boost-free GTSAM the five libraries depend only on each other and on system libraries (libstdc++/libc++, libm, libpthread, libdl), so nothing is copied beyond the files selected by target, and there is no `file(GET_RUNTIME_DEPENDENCIES …)` step. (The abandoned Boost-enabled design needed a Boost/ICU closure here; it is described in "Appendix: fallbacks" and must not be implemented unless that fallback is taken.) The CI verification of Task 1.6 is what proves the set is closed.
4. **Linux only:** after step 2, `patchelf --set-rpath '$ORIGIN'` on each file this function installed (and only those), failing the install if `patchelf` is missing or returns non-zero — the same requirement `DeployThirdPartyLinux.cmake` line 174 already imposes. Do not port the branch's glob over all of `usr/lib/*.so*`.
5. Print one `message(STATUS)` per deployed file at configure time, in the style of `DeployThirdPartyWindows.cmake`.

Deployment is unconditional (not tied to `FLYSIGHT_BUILD_FUSION_TESTS`).

**Acceptance Criteria:**
- [ ] Windows, Release, after `cmake --install`: the install root contains `gtsam.dll`, `metis-gtsam.dll`, `cephes-gtsam.dll`, `tbb12.dll`, `tbbmalloc.dll`; it does not contain `tbbmalloc_proxy.dll`, `concrt140*.dll`, any `*d.dll` MSVC debug runtime, or `Geographic.lib`-era extras that came from `GTSAM-install/bin` (compare the install listing before and after this phase: the only new files are those five plus nothing else from this task).
- [ ] The deployment probe of Task 1.4, copied into the install root and run with `PATH` reduced to `C:\Windows\System32`, exits 0 (proves the deployed set is complete and loadable on Windows).
- [ ] `cmake/SolverDependencies.cmake` contains no `file(GLOB` over `GTSAM_ROOT` or `ONETBB_ROOT`.
- [ ] macOS and Linux branches of the function exist, follow steps 2 and 4 (no closure step, no `GET_RUNTIME_DEPENDENCIES`, no file-name pattern for Boost or ICU anywhere in the file), and are marked in a comment as unverified until the first CI run; the three call sites are the only `src/CMakeLists.txt` lines added by this task.
- [ ] `DeployThirdPartyWindows.cmake`, `DeployThirdPartyMacOS.cmake`, `DeployThirdPartyLinux.cmake`, `fix_macos_rpaths.sh`, `CreateAppDir.cmake`, `CreateAppImage.cmake` are unchanged.

**Complexity:** M

---

### Task 1.4: `FLYSIGHT_BUILD_FUSION_TESTS`, the smoke test, and the deployment probe

**Purpose:** Prove, in CTest and against the installed application, that the exported target compiles, links and runs, and establish the gated test function phases 8 and 9 add their tests with.

**Files to create:**
- `tests/tst_solver_smoke.cpp` — QtTest smoke test.
- `tests/solverprobe.h` — the shared, header-only GTSAM exercise (no Qt).
- `tests/solver_deploy_probe.cpp` — plain `main()` around the same exercise, for running inside an installed tree.

**Files to modify:**
- `tests/CMakeLists.txt` — one new gated block, placed after the embedded-Python block (after line 216) and before the cleanup audit.
- `tests/README.md` — option, label, the new test row, executable count.

**Technical Approach:**

*Gated block*, modelled on the `FLYSIGHT_BUILD_PYTHON_TESTS` block (`tests/CMakeLists.txt` lines 152-216):

```
option(FLYSIGHT_BUILD_FUSION_TESTS "Build and run the GTSAM-linked tests" ON)
if(FLYSIGHT_BUILD_FUSION_TESTS)
  # flysight_add_fusion_test(<name> SOURCES <files...> [LIBS <targets...>] [ENVIRONMENT <VAR=value...>])
  ...
endif()
```

`flysight_add_fusion_test` forwards its arguments to `flysight_add_test`, then calls `flysight_solver_stack(<name>)` and `flysight_solver_test_environment(<name>)`, sets `LABELS "core;fusion"` and `TIMEOUT 600`, and on MSVC adds `/bigobj` (GTSAM/Eigen templates). It does **not** add `gtsam` itself: callers pass what they link through `LIBS` (`gtsam` here, `flysight_fusion` in phases 8-9). The header comment of `tests/CMakeLists.txt` gains one sentence naming this as the only place a test may link GTSAM. Nothing outside the `if` block changes: `flysight_test_support` and `flysight_add_test` are untouched, so with the option `OFF` no test target references GTSAM.

*`tests/solverprobe.h`* — `namespace FlySightTest { struct SolverProbeResult { bool ok; double error; std::string detail; }; SolverProbeResult runSolverProbe(); std::size_t consumeStack(std::size_t bytes); }`, header-only inline:
- `runSolverProbe()`: build a small `gtsam::NonlinearFactorGraph` — a `PriorFactor<gtsam::Pose3>` on key 0, a chain of about ten `BetweenFactor<gtsam::Pose3>` with a known relative pose, and a second prior on the last key that agrees with the chain — perturb the initial `Values`, optimize with `gtsam::LevenbergMarquardtOptimizer` (default parameters), and report the final error and the distance of the last pose from the analytic answer. `ok` when the error is below `1e-9` and the translation is within `1e-6`. This exercises bundled-Eigen headers, the ABI definitions from the exported target, METIS ordering, TBB-parallel elimination, and DLL loading. Do not use anything from the fusion model; this is not a numerical fixture.
- `consumeStack(bytes)`: non-tail recursion in frames of a `volatile` 64 KiB buffer that is written end to end, marked never-inline, returning a checksum that the caller uses, until `bytes` have been consumed. Returns the bytes consumed.

*`tests/tst_solver_smoke.cpp`* — one class `TstSolverSmoke`, `FLYSIGHT_TEST_MAIN`, test functions:
- `gtsamIsTheRightBuild()`: `GTSAM_VERSION_STRING` from `<gtsam/config.h>` equals `"4.3a0"`; `GTSAM_USE_TBB` is defined (checked with `#ifdef`, `QFAIL` otherwise); `GTSAM_ENABLE_BOOST_SERIALIZATION == 0` and `GTSAM_USE_BOOST_FEATURES == 0` (both macros are always defined, as `0` or `1`; this is the test-level statement that the shipped configuration is the Boost-free one); `EIGEN_WORLD_VERSION.EIGEN_MAJOR_VERSION` is 3.4 (GTSAM's bundled Eigen, not a system one).
- `smallGraphOptimizes()`: `runSolverProbe().ok`, with `detail` in the failure message.
- `mainThreadHasSolverStack()`: `consumeStack(48 * 1024 * 1024)` returns normally. With a default stack (1 MiB on Windows, 8 MiB on macOS/Linux) this test crashes, which is the point: it is the only automated check of `flysight_solver_stack` on each platform.

Registered with `flysight_add_fusion_test(tst_solver_smoke SOURCES tst_solver_smoke.cpp solverprobe.h LIBS gtsam)`.

*`tests/solver_deploy_probe.cpp`* — `int main()`: calls `runSolverProbe()`, prints one line (`solver probe ok, error=…` or the detail) and returns 0/1. No Qt, no stack probe (it runs with the default stack on purpose; the tiny graph needs none). Target inside the same gated block: `add_executable(solver_deploy_probe solver_deploy_probe.cpp solverprobe.h)`, `target_link_libraries(solver_deploy_probe PRIVATE gtsam)`, `/bigobj` on MSVC, no `add_test`, no install rule. It is copied by hand (locally) or by the workflow (CI) into the installed tree and run there.

*`tests/README.md`:* add `FLYSIGHT_BUILD_FUSION_TESTS` to section 3 next to `FLYSIGHT_BUILD_PYTHON_TESTS`, the `fusion` label, a "Solver dependencies" table with the `tst_solver_smoke` row, a short paragraph on `solver_deploy_probe`, and bump the executable count in section 1 (add 1 to the current count, whatever it is; the probe is not a test). State that `ctest -LE fusion` is the GTSAM-free run.

**Acceptance Criteria:**
- [ ] With `-DFLYSIGHT_BUILD_TESTS=ON` (fusion tests default `ON`), `ctest -C Release -R tst_solver_smoke --output-on-failure` passes on Windows against the Boost-free installs in `build-solver-deps/`, with nothing copied next to the executable and with no Boost directory on `PATH`.
- [ ] Temporarily removing the `flysight_solver_stack` call makes `mainThreadHasSolverStack` crash on Windows (implementer confirms once, then restores the call; say so in the report).
- [ ] With `-DFLYSIGHT_BUILD_FUSION_TESTS=OFF`, the targets `tst_solver_smoke` and `solver_deploy_probe` do not exist and every other test builds and passes.
- [ ] In the generated build system no target other than `tst_solver_smoke` and `solver_deploy_probe` has `gtsam` on its link line (check: in the Visual Studio build tree, `grep -l "gtsam" --include=*.vcxproj -r <build>/FlySightViewer-build` lists only those two projects).
- [ ] The full existing suite still passes (`ctest -C Release --output-on-failure`), including `audit_cleanup`.
- [ ] `tests/README.md` documents the option, the label and the test.

**Complexity:** M

---

### Task 1.5: README build instructions

**Purpose:** Spec section 10: "README build instructions cover the new dependencies."

**Files to modify:**
- `README.md` — additive edits in existing sections; no new top-level section about fusion itself (Phase 10 owns feature documentation).

**Technical Approach:**

- Prerequisites table (line 76), "Boost Components" (lines 101-116), the `BOOST_ROOT` row (line 228) and the "Boost not found" troubleshooting entry: **left exactly as they are.** GTSAM adds no Boost requirement. The existing text describes the application's header-only `boost::geometry` use, which is still true until Phase 3 lands; Phase 10 removes the Boost prerequisite from the README altogether (overview, Integration Note 9).
- "Full Build" (line 139) and the options table (lines 214-219): name GTSAM and oneTBB; add `FLYSIGHT_BUILD_SOLVER_DEPS` (default `ON`) and `FLYSIGHT_BUILD_FUSION_TESTS` (default `ON`). Say that the first build downloads the pinned sources (git and network needed) into `<build>/solver-sources/` and that GTSAM takes tens of minutes.
- Path-variable table (lines 225-228): `GTSAM_INSTALL_DIR`, `ONETBB_INSTALL_DIR` (root), `GTSAM_ROOT`, `ONETBB_ROOT` (when configuring `src/` directly), `GTSAM_SOURCE_DIR`, `ONETBB_SOURCE_DIR` (build an existing source tree offline).
- A short "Solver dependencies (GTSAM, oneTBB)" subsection under the build options: pinned revisions live in `cmake/SolverSuperbuild.cmake`; how to bump one (edit the SHA; the CI cache key follows); `-DFLYSIGHT_BUILD_SOLVER_DEPS=OFF` reuses existing installs; GTSAM is built **without Boost** (`GTSAM_ENABLE_BOOST_SERIALIZATION=OFF`, `GTSAM_USE_BOOST_FEATURES=OFF`), so it needs no Boost to build or run and the application refuses a Boost-enabled GTSAM install at configure time; the application consumes the exported `gtsam` target, which supplies GTSAM's own Eigen headers — do not add another Eigen to the include path; Debug application builds need a Debug build of both libraries (`--config Debug`), since the exported targets only carry the configurations that were built.
- Install-location table (lines 258-259), clean targets (lines 266-301), project tree (lines 314-371), and the deployment table (line 394): add the two libraries, `clean-GTSAM` / `clean-oneTBB`, `SolverSuperbuild.cmake`, `SolverDependencies.cmake`, `solver_stack_linux.cpp`, and the deployed runtime set per platform.

**Acceptance Criteria:**
- [ ] Every option, cache variable, target and file introduced by tasks 1.1-1.4 appears in `README.md` with its default.
- [ ] The README states that GTSAM is built without Boost and names the two options; `git diff master -- README.md` adds no Boost requirement, version ceiling, component list or `boost@…` formula, and leaves the existing Boost lines untouched (Phase 10 removes them).
- [ ] The diff is additive within existing sections (no section reordered or rewritten), so it merges cleanly with other phases' README edits.

**Complexity:** S

---

### Task 1.6: CI workflow (committed separately as "Phase 1: CI (unverified)")

**Purpose:** Build, cache, verify and test the solver dependencies on all four runners through the existing workflow.

**Files to modify:**
- `.github/workflows/build.yml` — **the only path of this task, and the only path of this phase that the implementation agent reports under a separate "CI (unverified)" heading.** Everything else in the phase goes in the normal phase commit, including the macOS/Linux CMake code that is equally unverified.

**Technical Approach (each item is a small hunk; do not restructure steps):**

1. *Cache* (lines 92-110): add `cmake/SolverSuperbuild.cmake` to the hashed files; add `third-party/GTSAM-install` and `third-party/oneTBB-install` to `path`. Key format unchanged.
2. *Boost steps: untouched.* "Install Boost (Windows)" (lines 126-154), `brew install ninja boost` with its `BOOST_ROOT` lines (159-170), `libboost-all-dev` (line 246), the `BOOST_ROOT` / `BOOST_LIBRARYDIR` arguments of "Configure main application" and the "Verify Boost discovery" step stay exactly as on `master`. They serve the application's header-only Boost use, which exists until Phase 3; Phase 10 removes them. No `boost@1.85` pin, no Boost argument to the third-party configure step: GTSAM does not look for Boost. (Homebrew's current Boost, 1.92, is fine for header-only `boost::geometry`.)
3. *Verify third-party installations* (lines 395-447): add a GTSAM/oneTBB block that lists `lib/` (and `bin/` on Windows) and **fails** when neither `GTSAM-install/CMake/GTSAMConfig.cmake` nor `GTSAM-install/lib/cmake/GTSAM/GTSAMConfig.cmake` exists, or `oneTBB-install/lib/cmake/TBB/TBBConfig.cmake` is missing.
4. *Same step, Boost-free check:* fail unless `GTSAM-install/include/gtsam/config.h` contains `#define GTSAM_ENABLE_BOOST_SERIALIZATION 0` and `#define GTSAM_USE_BOOST_FEATURES 0`, and fail if `grep -ril "Boost::" GTSAM-install/CMake GTSAM-install/lib/cmake/GTSAM 2>/dev/null` finds a `GTSAM-exports*.cmake` file. (This also rejects a cache restored from a Boost-enabled build, should the key ever collide.)
5. *Configure main application* (lines 494-509): add `-DGTSAM_ROOT="${GITHUB_WORKSPACE}/third-party/GTSAM-install"` and `-DONETBB_ROOT="${GITHUB_WORKSPACE}/third-party/oneTBB-install"`. `FLYSIGHT_BUILD_FUSION_TESTS` defaults to `ON`, so the existing "Run tests" step runs `tst_solver_smoke` on every platform with no edit.
6. *Verify deployment output (Windows)* (line 585): add `gtsam.dll metis-gtsam.dll cephes-gtsam.dll tbb12.dll tbbmalloc.dll` to the critical-DLL loop. Then, on non-tag builds only (the probe exists only when tests are built): copy `solver_deploy_probe.exe` from the build tree into `build/install/`, run it with `PATH=/c/Windows/System32`, fail on non-zero, delete the copy.
7. *Verify deployment output (macOS)* (line 669): extend the loop to `libGeographic libgtsam libmetis-gtsam libcephes-gtsam libtbb libtbbmalloc`, and make a missing solver library or a remaining absolute non-system dependency in one of them an error (`exit 1`), not a warning.
8. *Verify deployment output (Linux)* (line 787): extend the loop to the same names (`.so*`), fail when one is missing, and run `ldd` on the deployed `libgtsam.so.*` with `LD_LIBRARY_PATH` unset: fail on any `not found`, fail when `libgtsam`'s `libtbb`, `libmetis-gtsam` or `libcephes-gtsam` dependencies resolve outside the AppDir, and fail when the `ldd` output names any `libboost` or `libicu` at all.
9. *Both Unix deployment checks (the macOS and Linux steps of items 7 and 8):* fail when the deployed tree contains any `libboost*` file (`find <bundle or AppDir> -name 'libboost*'` must print nothing). ICU is checked per library rather than per tree, because Qt's own deployment legitimately ships `libicu*` on Linux: none of the five solver libraries may reference `libicu` or `libboost` (`ldd` on Linux, `otool -L` on macOS, over each deployed solver library), and on macOS `Contents/Frameworks` must contain no `libicu*` that was not there before this phase (today: none).

No release-job, signing, notarization or artifact step changes.

**Acceptance Criteria:**
- [ ] The file is valid YAML (parse it locally, e.g. `python -c "import yaml,sys; yaml.safe_load(open('.github/workflows/build.yml'))"`), and every edited shell block passes `bash -n` when extracted.
- [ ] The diff touches only the places listed (items 1 and 3-9); no existing Boost line of the workflow is added, removed or edited (`git diff master -- .github/workflows/build.yml | grep -i boost` shows only the new GTSAM Boost-free checks and the `libboost*`/`libicu*` absence checks); step names, order and `if:` conditions of existing steps are unchanged.
- [ ] The implementation report lists `.github/workflows/build.yml` under its own heading "CI (unverified)" and nowhere else, and lists under "Unverified on macOS/Linux (in the phase commit)" the relevant parts of `cmake/SolverSuperbuild.cmake`, `cmake/SolverDependencies.cmake` and `cmake/solver_stack_linux.cpp`.

**Complexity:** M

## Testing Requirements

### Unit Tests
- New: `tests/tst_solver_smoke.cpp` (`gtsamIsTheRightBuild`, `smallGraphOptimizes`, `mainThreadHasSolverStack`), label `fusion`.
- No existing test changes. No entry is added to `tests/acceptance_map.txt` in this phase (that file still maps the previous plan's criteria and is checked by `audit_cleanup`; Phase 10 owns the new map, where spec acceptance 1 will cite `tst_solver_smoke` and the CI verification steps).

### Integration Tests
- The whole existing suite with `FLYSIGHT_BUILD_FUSION_TESTS=ON` and again configured `OFF` (fresh build directory or reconfigure): both pass; the second has no GTSAM-linked target.
- `solver_deploy_probe` run from inside the installed application directory with a scrubbed `PATH` (Windows, local and CI).
- CI on all four runners after the first push (not available to the implementer; expected follow-up is `Phase 1 fixup`).

### Manual Verification (Windows; use `build-solver-deps/` and new `build-phase1*` directories — `build*/` is git-ignored — never Michael's `build/`, never the default solver install directories)

The order matters: the Boost-free install has to exist before anything can be configured against it, so the long build comes first (start it as soon as Task 1.1 is written; run it in the background).

0. Revisions reachable, outside the working tree:
   `git init --bare <scratch>/probe.git`, then `git --git-dir=<scratch>/probe.git fetch --depth 1 https://github.com/borglab/gtsam.git 814a734d68cbf5068a4bf20d63ba67c4935905bb` and the same for `https://github.com/uxlfoundation/oneTBB.git d3ad09cd7f69d3f50a3972bee9eb7fc8ee089b6e`.
1. **The local Boost-free solver install** (30-60 minutes). With `BOOST_ROOT` / `BOOST_LIBRARYDIR` removed from the environment of this shell, to prove GTSAM needs neither:
   `cmake -G "Visual Studio 17 2022" -A x64 -B build-solver-deps -S . -DFLYSIGHT_THIRD_PARTY_ONLY=ON -DGTSAM_INSTALL_DIR=<repo>/build-solver-deps/GTSAM-install -DONETBB_INSTALL_DIR=<repo>/build-solver-deps/oneTBB-install`, then `cmake --build build-solver-deps --config Release --target ext_GTSAM`. This exercises the pinned download from `borglab/gtsam`. If the network is unavailable, do **not** substitute `third-party/gtsam` silently: it is the fork revision. Using `-DGTSAM_SOURCE_DIR=<repo>/third-party/gtsam -DONETBB_SOURCE_DIR=<repo>/third-party/oneTBB` is acceptable for a first local check only (library sources are identical), must be reported, and the install must be rebuilt from the pinned upstream download before the phase is declared done, because Phase 8's goldens are captured against this install. Check the Task 1.1 criteria on the result (`config.h` macros `0`, exports without `Boost::`, `dumpbin /dependents`). **Leave `build-solver-deps/` in place** when the phase ends and name it in the report.
2. Application against the Boost-free installs:
   `cmake -G "Visual Studio 17 2022" -A x64 -B build-phase1 -S . -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64" -DFLYSIGHT_BUILD_THIRD_PARTY=OFF -DFLYSIGHT_BUILD_TESTS=ON -DGTSAM_INSTALL_DIR=<repo>/build-solver-deps/GTSAM-install -DONETBB_INSTALL_DIR=<repo>/build-solver-deps/oneTBB-install`, `cmake --build build-phase1 --config Release`, `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`.
3. Deployment: list `build-phase1/install/`; confirm the five DLLs and that no `boost*.dll` is present; copy `solver_deploy_probe.exe` in, run with `PATH=C:\Windows\System32`, remove it; launch `FlySightViewer.exe` once to confirm the application still starts.
4. Guard: configure once more into a throwaway directory **without** the two `-D…_INSTALL_DIR` arguments. On Michael's machine the default `GTSAM_ROOT` is the old Boost-enabled install, and the configure must stop with the Boost-free guard's message (Task 1.2). Confirm afterwards that `third-party/GTSAM-install` is unmodified (`git status --porcelain --ignored third-party` unchanged; directory timestamps unchanged).
5. `-DFLYSIGHT_BUILD_FUSION_TESTS=OFF` and `-DFLYSIGHT_BUILD_SOLVER_DEPS=OFF` configure checks from tasks 1.1 and 1.4.

## Notes for Implementer

### Gotchas
- **Never run `clean-third-party`, `clean-GTSAM` or `clean-oneTBB` against the default install directories on Michael's machine**, and never build `ext_GTSAM` with the default `GTSAM_INSTALL_DIR` there: the old Boost-enabled `third-party/GTSAM-install` (with `third-party/oneTBB-install`) is the second solver of Phase 8's cross-check and cannot be reproduced exactly once overwritten. Always redirect to `build-solver-deps/…` as in Manual Verification step 1.
- **The old install is not a fallback for convenience.** Do not configure the application, the smoke test or the probe against `third-party/GTSAM-install` "because it is already built": it is a different configuration (Boost on) of a different pin (the fork). The Boost-free guard of Task 1.2 refuses it.
- Michael's everyday `build/` is a superbuild with `FLYSIGHT_BUILD_THIRD_PARTY=ON`. After this phase a plain `cmake --build build --config Release` there would download and build GTSAM into the default install directory, **overwriting the old install**. Put this, prominently, in your report so the orchestrator can relay it: before building `build/` again, reconfigure it with `-DFLYSIGHT_BUILD_SOLVER_DEPS=OFF -DGTSAM_INSTALL_DIR=<repo>/build-solver-deps/GTSAM-install -DONETBB_INSTALL_DIR=<repo>/build-solver-deps/oneTBB-install`.
- The pre-existing `GTSAM-install` was built with `GTSAM_INSTALL_GEOGRAPHICLIB=ON` and contains a second GeographicLib and MSVC runtime DLLs in `bin/`. That is why (a) the `SolverDependencies.cmake` include sits after GeographicLib discovery and (b) nothing may glob `GTSAM-install/bin`.
- GTSAM's exported targets carry only the configurations that were built (only `RELEASE` in a Release-only install such as `build-solver-deps/`). A Debug application build silently maps to the Release DLLs and mixes MSVC runtimes. Do not try to solve this in CMake; document it (Task 1.5). Verify in Release only.
- A Boost-free **shared** GTSAM on MSVC is documented by GTSAM (`INSTALL.md`) but not built by GTSAM's own CI (only `ubuntu-no-boost` is). If it fails to compile or link locally, report the exact error; do not patch GTSAM sources and do not quietly turn one of the two options back on. The way back is the whole fallback in "Appendix: fallbacks", and it is Michael's call.
- GTSAM's `excluded_headers` list for the Boost-free case uses a wrong path prefix, so `gtsam/base/serialization.h` and `serializationTestHelpers.h` may still be installed. They are empty under `#if GTSAM_ENABLE_BOOST_SERIALIZATION`; ignore them.
- The application's own `find_package(Boost REQUIRED)` (no components) still runs before the `SolverDependencies.cmake` include and may print CMake's CMP0167 "FindBoost is deprecated" warning with CMake ≥ 3.30, as it already does on `master`. It has nothing to do with GTSAM; leave it (Phase 10 removes the call).
- `ExternalProject` with `GIT_TAG <sha>` clones full history of GTSAM (a few hundred MB). `GIT_SHALLOW` does not work with a bare SHA; do not add it.
- Generator expressions are valid in `set_property(TEST … ENVIRONMENT_MODIFICATION …)` only for tests created with `add_test(NAME …)`, which `flysight_add_test` does.
- GTSAM compiles its own sources with `/WX` on MSVC. If a newer MSVC on the CI runner turns a new warning into an error, the fix belongs in a `Phase 1 fixup`. It must not change code generation (parity): silencing the specific warning with `/wd<nnnn>` is acceptable, optimisation or floating-point flags are not. GTSAM adds `/WX` through its own `GTSAM_COMPILE_OPTIONS_PRIVATE` list, so `CMAKE_COMPILE_WARNING_AS_ERROR=OFF` is probably not honoured; if no clean switch exists, stop and report rather than patching GTSAM sources.
- Likely first-push failures, in order of probability: a compile error in the Boost-free GTSAM on Apple Clang (not covered by GTSAM's CI); `TARGET_SONAME_FILE_NAME` / `IMPORTED_SONAME` on macOS containing an absolute or `@rpath/` prefix (take the file-name component); the Linux `RLIMIT_STACK` constructor (if `mainThreadHasSolverStack` fails only on Linux, the fallback is for fusion tests to run fits on a 64 MiB `QThread` there — a Phase 1 fixup, since it changes the helper's contract for phases 8-9).
- Shared files (`CMakeLists.txt`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`, `README.md`) are also edited by phases 2-9. Keep every hunk additive and self-contained; do not reflow, re-indent or reorder neighbouring lines; do not touch `src/calculations/builtincalculations.cpp`, `src/mainwindow.cpp`, `docs/CALCULATIONS.md` or `tests/acceptance_map.txt` at all.
- Do not add `flysight_fusion` or any source under `src/`: that is Phase 8.

### Decisions Made
- **Pinning by SHA in `cmake/SolverSuperbuild.cmake`, not by submodule.** Existing dependencies are not uniform (GeographicLib and QCustomPlot are vendored trees, KDDockWidgets and pybind11 are submodules), so either form is "consistent"; what is kept uniform is the superbuild shape (`ext_<Name>`, `third-party/<Name>-install`, `<NAME>_INSTALL_DIR`, clean target, CI cache). A submodule was rejected because: a gitlink cannot be created by an implementation agent (no state-changing git); `third-party/gtsam` and `third-party/oneTBB` already exist untracked without `.git`, exactly the paths the Commit Policy says are never staged, and stale `.git/modules/third-party/{gtsam,oneTBB}` would collide with `git submodule add`; CI checks out with `submodules: recursive` and `fetch-depth: 0`, which would clone all of GTSAM on every run, cache hit or not; and it is what the working reference does. The Commit Policy's submodule clause is conditional and simply does not apply.
- **GTSAM revision `814a734…` from the official `borglab/gtsam`** (Michael's decision, "let's try"). The branch used fork commit `8938b9f…`, whose only change is unreachable with `GTSAM_INSTALL_GEOGRAPHICLIB=OFF`; library sources are identical. Fallback: the fork pair, a one-line change (Task 1.1, Appendix A).
- **GTSAM is built without Boost on all platforms** (Michael's decision, "let's try"): `GTSAM_ENABLE_BOOST_SERIALIZATION=OFF`, `GTSAM_USE_BOOST_FEATURES=OFF`. This removes the Boost ≤ 1.88 ceiling, the `boost@1.85` CI pin, `Boost_USE_STATIC_LIBS` and `BOOST_ROOT` plumbing for GTSAM, all Boost/ICU runtime deployment on macOS and Linux, and the macOS deployment-target question. Every other option is the branch's, unchanged. Because this is a different configuration from the one the branch was validated with, Phase 8 captures its goldens against *this* build and cross-checks them byte for byte against the old Boost-enabled build before accepting them. Fallback: Appendix B.
- **The Boost-free requirement is enforced, not assumed:** a configure-time guard in `SolverDependencies.cmake`, two assertions in `tst_solver_smoke`, and CI checks on the install and on the deployed tree.
- **Local Boost-free installs live in `build-solver-deps/`**, not in the default `third-party/*-install` directories, which on Michael's machine hold the old Boost-enabled build that Phase 8 still needs. Defaults (and CI cache paths) are unchanged.
- **Boost in the application is not this phase's business.** The application's header-only Boost lookup, README text and CI steps are left exactly as on `master`; Phase 3 removes the last use, Phase 10 removes the lookup, the README prerequisite and the CI steps.
- **Solver build directories live under the superbuild's binary directory**, not under `third-party/`, to stay clear of the pre-existing `gtsam-build` / `oneTBB-build`.
- **`FLYSIGHT_BUILD_SOLVER_DEPS`** (default `ON`) added so existing installs can be reused without disabling the rest of the third-party build.
- **`FLYSIGHT_BUILD_FUSION_TESTS`** (default `ON`, like `FLYSIGHT_BUILD_PYTHON_TESTS`) and **`flysight_add_fusion_test`** are introduced here so phases 8 and 9 add tests with one call and one small hunk.
- **GTSAM is `REQUIRED` for the application configure** from this phase on; the GTSAM-free guarantee is about what targets link, enforced by an acceptance criterion.
- **No build-tree DLL copying** (the branch's `flysight_copy_solver_runtime` is dropped); tests use `PATH`/`LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH` through `flysight_solver_test_environment`, matching how `master` treats Qt and GeographicLib.
- **`flysight_solver_stack` on Linux raises `RLIMIT_STACK` from a static constructor**, because no link option can size the main thread's stack there; macOS uses `-stack_size`; MSVC uses `/STACK`. The application executable does not get the helper.
- **One real file per Unix library, named by soname, and no dependency closure** (none is needed without Boost), instead of the branch's two copies and its `usr/lib/*.so*` RPATH glob.
- **A non-Qt `solver_deploy_probe`** complements the QtTest smoke test, because a QtTest executable cannot run inside the installed tree (Qt Test is not deployed) and only a run inside that tree proves deployment.
- **CI does not touch Boost** in this phase (no `boost@1.85` pin).

### Open Questions
- **Resolved — macOS deployment target.** The question was whether bundled Homebrew Boost/ICU dylibs (built for the runner's macOS) would break fusion on macOS 12-14. With GTSAM built without Boost nothing from Homebrew is bundled; GTSAM and oneTBB are built by the superbuild with `CMAKE_OSX_DEPLOYMENT_TARGET` forwarded. The question returns only if the Boost fallback (Appendix B) is taken.
- If Michael prefers submodules for auditability, the change is confined to `SolverSuperbuild.cmake` (`SOURCE_DIR` instead of `GIT_*`), two gitlinks and `.gitmodules`, made by the orchestrator after moving the untracked source trees aside; GTSAM would then point at `borglab/gtsam`.

### Appendix: fallbacks

Both decisions above are "let's try". Neither fallback is taken by an implementation agent on its own judgment except where stated; the knowledge is kept here so that going back is cheap.

**A. Back to the fork pin.** Trigger: upstream `814a734…` fails to configure or build because of bundled GeographicLib's `js` subdirectory (the one thing the fork patches). Action, allowed without asking: in `cmake/SolverSuperbuild.cmake` set the GTSAM pair to `https://github.com/crwper/gtsam.git` / `8938b9f158fa2f88ccfe3c31069a1452456b7eca` and update the comment; nothing else changes (the CI cache key follows the file hash). Report it prominently. The branch's `borglab/gtsam` URL with the fork SHA is not a reliable download; the fork URL must be used with the fork SHA.

**B. Back to a Boost-enabled GTSAM.** Trigger: the Boost-free build cannot be made to work on some platform, or Phase 8's cross-check shows that it changes fusion output and Michael prefers the old configuration. This is **Michael's decision**; the agent stops and reports. What it entails (the design this document originally described):
- *Superbuild:* drop the two `=OFF` arguments (defaults are `ON`); in `SolverSuperbuild.cmake` cache `BOOST_ROOT` / `BOOST_LIBRARYDIR` from the environment when not defined, `include(BoostDiscovery.cmake)`, and pass `-DBOOST_ROOT=…` (and `-DBOOST_LIBRARYDIR=…` when set) to `ext_GTSAM`; forward both from the root project to the application (`_APP_CMAKE_ARGS`) and pass them to CI's third-party configure step.
- *Boost requirement:* **1.65-1.88** with compiled `serialization`, `system`, `filesystem`, `thread`, `program_options`, `date_time`, `timer`, `chrono`, `regex` (Boost 1.89 removed `boost_system`, which GTSAM 4.3a0 names as a component). Windows: prebuilt 1.87, static libraries (GTSAM's `HandleBoost.cmake` forces `Boost_USE_STATIC_LIBS ON` on MSVC, so no Boost DLL is deployed). macOS: `brew install boost@1.85`, `BOOST_ROOT=$(brew --prefix boost@1.85)`. Ubuntu 22.04: `libboost-all-dev` (1.74). README and CI must say all this, and Phase 10 must then **keep** `find_package(Boost)` / `BoostDiscovery.cmake` and the CI Boost steps.
- *Application discovery:* `if(MSVC) set(Boost_USE_STATIC_LIBS ON) endif()` before `find_package(GTSAM)` (it must match how GTSAM found Boost); remove the Boost-free guard and the two smoke-test assertions; expect CMake's CMP0167 FindBoost deprecation warning from GTSAM's `find_dependency(Boost …)`.
- *Runtime collection:* the `gtsam` link interface then carries eight `Boost::` targets; on Unix they are shared `UNKNOWN_LIBRARY` targets and are collected and deployed by soname.
- *Unix dependency closure:* shared Boost pulls libraries that are not CMake targets (`libboost_regex` needs ICU; `libboost_thread` / `filesystem` may need `libboost_atomic`). After installing the target-selected files, an `install(CODE …)` step runs `file(GET_RUNTIME_DEPENDENCIES LIBRARIES <installed solver files> …)` and copies every resolved dependency whose file name matches `^libboost_` or `^libicu` and is not already present (real file, under the referenced name); unresolved ones are warnings. Linux `patchelf --set-rpath '$ORIGIN'` covers those files too. CI's deployment checks then require, rather than forbid, in-tree `libboost_*`.
- *Open again:* Homebrew's Boost/ICU dylibs are built for the runner's macOS (15) while the application targets 12.0; bundling them may make fusion unusable on macOS 12-14. The alternative is building the needed Boost libraries from source in the superbuild with the deployment target.
- *Phase 8:* the goldens are re-captured against the Boost-enabled build (on Michael's machine the old `third-party/GTSAM-install` is exactly that), and the cross-check task is dropped.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria (Windows verified locally; macOS/Linux code present, reviewed against this document, and flagged unverified).
2. The full test suite passes on Windows with fusion tests on and off, and `tst_solver_smoke` passes against the Boost-free superbuild of the pinned upstream revisions in `build-solver-deps/` (downloaded, not built from the untracked fork tree), which is left in place for later phases.
3. The installed Windows application contains the five solver DLLs selected by target, and `solver_deploy_probe` runs inside it with a scrubbed `PATH`.
4. No target other than `tst_solver_smoke` and `solver_deploy_probe` links GTSAM; `flysight_model`, `flysight_core`, `flysight_test_support` and all existing tests are unchanged in what they link.
5. GTSAM was configured and built with no Boost available to it; the installed `config.h` has both Boost macros `0`; the `gtsam` target's link interface and the deployed tree contain no Boost library, and no solver library references ICU; the old `third-party/GTSAM-install` is untouched and was rejected by the guard.
6. The implementation report separates `.github/workflows/build.yml` ("CI (unverified)") from the phase-commit file list, lists no path under `third-party/` except `third-party/CMakeLists.txt`, and lists none of the pre-existing untracked trees.
7. Code follows the patterns of `cmake/ThirdPartySuperbuild.cmake`, `cmake/DeployThirdParty*.cmake` and `tests/CMakeLists.txt`; no TODOs or placeholder code remains.
