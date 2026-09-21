# =============================================================================
# SolverSuperbuild.cmake
# =============================================================================
#
# ExternalProject targets for the solver dependencies of the sensor fusion:
# oneTBB and GTSAM. Included by ThirdPartySuperbuild.cmake, so both the root
# superbuild and the standalone third-party build get them.
#
# Unlike GeographicLib and KDDockWidgets, the sources are not part of this
# repository. Each library is pinned to a full commit SHA below and downloaded
# into the build tree (<build>/solver-sources/) the first time it is built,
# which needs git and network access. The build directories also live in the
# build tree; only the install directories are under third-party/.
#
# Build Order and Dependencies:
#   1. oneTBB - No dependencies
#   2. GTSAM  - Depends on oneTBB (GTSAM_WITH_TBB=ON)
#
# =============================================================================

option(FLYSIGHT_BUILD_SOLVER_DEPS
    "Build GTSAM and oneTBB with the other third-party dependencies" ON)

# With the option OFF nothing is defined here. The application build then uses
# whatever is already installed in GTSAM_INSTALL_DIR / ONETBB_INSTALL_DIR, and
# an everyday build can never write into those directories.
if(NOT FLYSIGHT_BUILD_SOLVER_DEPS)
    message(STATUS "FLYSIGHT_BUILD_SOLVER_DEPS is OFF - using existing GTSAM and oneTBB installs")
    return()
endif()

# =============================================================================
# Pinned Revisions
# =============================================================================
#
# One adjacent repository/revision pair per library. What gets built is decided
# here only, and the CI cache key hashes this file, so it follows a new pin.
#
# Other places state the version, or names that depend on it. Keep them in
# sync when a pin moves:
#   - cmake/SolverDependencies.cmake: the minimum version in
#     find_package(GTSAM 4.3 ...), and the list of required runtime targets
#     (gtsam, metis-gtsam, cephes-gtsam, TBB::tbb, TBB::tbbmalloc)
#   - tests/tst_solver_smoke.cpp: compares GTSAM_VERSION_STRING with "4.3a0"
#   - .github/workflows/build.yml: the deployment checks name the runtime files
#     (gtsam.dll, metis-gtsam.dll, cephes-gtsam.dll, tbb12.dll, tbbmalloc.dll;
#     the libgtsam / libmetis-gtsam / libcephes-gtsam / libtbb / libtbbmalloc
#     stems on macOS and Linux). "tbb12" carries oneTBB's binary version.
#   - README.md: the versions under "Solver dependencies (GTSAM, oneTBB)" and
#     the runtime file names in the deployment table
#   - tests/README.md (several places: the tst_solver_smoke row of the test
#     table, "Solver configuration of the goldens", and the
#     find_package(GTSAM 4.3 ...) of the capture harness) and
#     tests/data/fusion/capture.json ("solver"): the revision and versions the
#     fusion goldens were captured against
#   - the comments in this file that say 4.3a0 / 2022.1.0
#
# Moving the GTSAM pin changes the solver the fusion goldens were captured
# against. They must be re-validated before the new pin is accepted, by the
# procedure in tests/README.md, "Fusion golden parity": never edited to match.
# A oneTBB pin changes the threading runtime only, but the parity tests are
# the check that it did not change the results either.
#
# GTSAM: the fusion work was developed against commit
# 8938b9f158fa2f88ccfe3c31069a1452456b7eca of the fork
# https://github.com/crwper/gtsam.git. That commit is the upstream commit
# pinned below plus one line of bundled-GeographicLib CMake
# ("#add_subdirectory (js)"), which is only reached when
# GTSAM_INSTALL_GEOGRAPHICLIB=ON. This build sets that option OFF, so the
# library sources are identical and the official repository is used as is.
#

set(GTSAM_REPOSITORY  https://github.com/borglab/gtsam.git)
set(GTSAM_REVISION    814a734d68cbf5068a4bf20d63ba67c4935905bb)  # borglab/gtsam, 4.3a0 (2025-02-03)
set(ONETBB_REPOSITORY https://github.com/uxlfoundation/oneTBB.git)
set(ONETBB_REVISION   d3ad09cd7f69d3f50a3972bee9eb7fc8ee089b6e)  # uxlfoundation/oneTBB, 2022.1.0

# =============================================================================
# Path Variables
# =============================================================================
#
# The install directories are defined by the including project, which both
# entry points (root CMakeLists.txt, third-party/CMakeLists.txt) do before they
# include ThirdPartySuperbuild.cmake. There is no default here: the clean
# targets below remove these directories, so an empty value must never get
# this far.
#
# <X>_SOURCE_DIR is for offline development: point it at an existing source
# tree and that tree is built in place of the pinned download. It is never
# modified and never removed by a clean target.
#

foreach(_var GTSAM_INSTALL_DIR ONETBB_INSTALL_DIR)
    if("${${_var}}" STREQUAL "")
        message(FATAL_ERROR
            "${_var} is not set. Define it before including SolverSuperbuild.cmake, "
            "as the root CMakeLists.txt and third-party/CMakeLists.txt do.")
    endif()
endforeach()

if(NOT DEFINED GTSAM_SOURCE_DIR)
    set(GTSAM_SOURCE_DIR ""
        CACHE PATH "Existing GTSAM source tree to build instead of downloading the pinned revision")
endif()

if(NOT DEFINED ONETBB_SOURCE_DIR)
    set(ONETBB_SOURCE_DIR ""
        CACHE PATH "Existing oneTBB source tree to build instead of downloading the pinned revision")
endif()

# Build directories are under the superbuild's binary directory, not under
# third-party/ like GeographicLib's: a developer machine may already have
# third-party/gtsam-build or third-party/oneTBB-build configured from another
# source path, and CMake refuses a build directory whose cached source differs.
set(ONETBB_BINARY_DIR "${CMAKE_BINARY_DIR}/oneTBB-build")
set(GTSAM_BINARY_DIR "${CMAKE_BINARY_DIR}/gtsam-build")

# =============================================================================
# Arguments Shared by Both Projects
# =============================================================================

# Forward the macOS platform settings so the libraries are built for the same
# deployment target and architectures as the application. CMAKE_PREFIX_PATH is
# deliberately not forwarded: neither library needs Qt.
set(_solver_common_args)
foreach(_var CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_ARCHITECTURES CMAKE_OSX_SYSROOT)
    if(DEFINED ${_var})
        list(APPEND _solver_common_args "-D${_var}=${${_var}}")
    endif()
endforeach()

# GTSAM's bundled METIS declares cmake_minimum_required(VERSION 3.0), which
# CMake 4 rejects. Raising the policy floor avoids editing the pinned source;
# it is harmless for oneTBB.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
    list(APPEND _solver_common_args -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
endif()

# =============================================================================
# ext_oneTBB - Threading library used by GTSAM
# =============================================================================
#
# oneTBB parallelizes GTSAM's elimination and supplies its default allocator
# (tbbmalloc).
#
# Key options:
#   - BUILD_SHARED_LIBS=ON      : Build as shared libraries (tbb, tbbmalloc)
#   - TBB_TEST=OFF              : Skip tests
#   - TBB_EXAMPLES=OFF          : Skip examples
#   - TBB_STRICT=OFF            : Do not treat compiler warnings as errors
#   - CMAKE_INSTALL_LIBDIR=lib  : Keep TBBConfig.cmake at lib/cmake/TBB on
#                                 distributions that default to lib64
#

set(_onetbb_source_args
    GIT_REPOSITORY "${ONETBB_REPOSITORY}"
    GIT_TAG ${ONETBB_REVISION}
    GIT_SUBMODULES ""
    SOURCE_DIR "${CMAKE_BINARY_DIR}/solver-sources/oneTBB"
)
if(ONETBB_SOURCE_DIR)
    set(_onetbb_source_args SOURCE_DIR "${ONETBB_SOURCE_DIR}" DOWNLOAD_COMMAND "")
endif()

ExternalProject_Add(ext_oneTBB
    ${_onetbb_source_args}
    BINARY_DIR "${ONETBB_BINARY_DIR}"
    INSTALL_DIR "${ONETBB_INSTALL_DIR}"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}"
    CMAKE_ARGS
        -DBUILD_SHARED_LIBS=ON
        -DTBB_TEST=OFF
        -DTBB_EXAMPLES=OFF
        -DTBB_STRICT=OFF
        -DCMAKE_INSTALL_LIBDIR=lib
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        ${_solver_common_args}
    UPDATE_COMMAND ""
)

# =============================================================================
# ext_GTSAM - Factor-graph optimization (sensor fusion solver)
# =============================================================================
#
# GTSAM 4.3a0 performs the batch GNSS/IMU fit. The application consumes its
# exported CMake target "gtsam", which carries GTSAM's ABI definitions and its
# bundled Eigen headers (see SolverDependencies.cmake).
#
# Key options:
#   - BUILD_SHARED_LIBS=ON                   : Build as shared libraries (gtsam,
#                                              metis-gtsam, cephes-gtsam)
#   - GTSAM_INSTALL_GEOGRAPHICLIB=OFF        : Do not install GTSAM's bundled
#                                              GeographicLib next to ours
#   - GTSAM_BUILD_EXAMPLES_ALWAYS=OFF        : Skip examples
#   - GTSAM_BUILD_TESTS=OFF                  : Skip tests
#   - GTSAM_BUILD_UNSTABLE=OFF               : Skip gtsam_unstable
#   - GTSAM_BUILD_PYTHON=OFF                 : Skip the Python wrapper
#   - GTSAM_WITH_TBB=ON                      : Parallel elimination and the TBB
#                                              allocator (part of the numerical
#                                              configuration)
#   - GTSAM_USE_SYSTEM_EIGEN=OFF             : Use the Eigen bundled with GTSAM
#   - GTSAM_BUILD_WITH_MARCH_NATIVE=OFF      : Portable binaries, no -march=native
#   - TBB_DIR                                : Use the oneTBB built above
#   - GTSAM_ENABLE_BOOST_SERIALIZATION=OFF   : No Boost needed to build, to
#                                              configure a consumer, or at run
#                                              time; FlySight does not serialize
#                                              GTSAM objects
#   - GTSAM_USE_BOOST_FEATURES=OFF           : Same; only replaces Boost timers
#                                              and concept checks, which
#                                              FlySight does not use
#   - CMAKE_INSTALL_LIBDIR=lib               : Keep GTSAMConfig.cmake at
#                                              lib/cmake/GTSAM on distributions
#                                              that default to lib64
#
# Every other GTSAM option stays at its default. Those defaults (rotation and
# pose parameterization, tangent preintegration, allocator, no MKL) are part
# of the numerical contract of the fusion and must not be overridden here, and
# no floating-point, optimization or architecture flags are passed.
#

set(_gtsam_source_args
    GIT_REPOSITORY "${GTSAM_REPOSITORY}"
    GIT_TAG ${GTSAM_REVISION}
    GIT_SUBMODULES ""
    SOURCE_DIR "${CMAKE_BINARY_DIR}/solver-sources/gtsam"
)
if(GTSAM_SOURCE_DIR)
    set(_gtsam_source_args SOURCE_DIR "${GTSAM_SOURCE_DIR}" DOWNLOAD_COMMAND "")
endif()

ExternalProject_Add(ext_GTSAM
    ${_gtsam_source_args}
    BINARY_DIR "${GTSAM_BINARY_DIR}"
    INSTALL_DIR "${GTSAM_INSTALL_DIR}"
    DEPENDS ext_oneTBB
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}"
    CMAKE_ARGS
        -DBUILD_SHARED_LIBS=ON
        -DGTSAM_INSTALL_GEOGRAPHICLIB=OFF
        -DGTSAM_BUILD_EXAMPLES_ALWAYS=OFF
        -DGTSAM_BUILD_TESTS=OFF
        -DGTSAM_BUILD_UNSTABLE=OFF
        -DGTSAM_BUILD_PYTHON=OFF
        -DGTSAM_WITH_TBB=ON
        -DGTSAM_USE_SYSTEM_EIGEN=OFF
        -DGTSAM_BUILD_WITH_MARCH_NATIVE=OFF
        -DTBB_DIR=${ONETBB_INSTALL_DIR}/lib/cmake/TBB
        -DGTSAM_ENABLE_BOOST_SERIALIZATION=OFF
        -DGTSAM_USE_BOOST_FEATURES=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_INSTALL_LIBDIR=lib
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        ${_solver_common_args}
    UPDATE_COMMAND ""
)

# =============================================================================
# Clean Targets
# =============================================================================
#
# Remove the build and install directories of one library. A source directory
# is never removed: the downloaded sources stay in <build>/solver-sources/ and
# a user-supplied <X>_SOURCE_DIR is not ours to delete.
#
# Usage:
#   cmake --build build --target clean-oneTBB
#   cmake --build build --target clean-GTSAM
#

add_custom_target(clean-oneTBB
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${ONETBB_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${ONETBB_INSTALL_DIR}"
    COMMENT "Cleaning oneTBB build and install directories..."
)

add_custom_target(clean-GTSAM
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${GTSAM_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${GTSAM_INSTALL_DIR}"
    COMMENT "Cleaning GTSAM build and install directories..."
)
