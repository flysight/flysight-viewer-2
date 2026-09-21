# =============================================================================
# ThirdPartySuperbuild.cmake
# =============================================================================
#
# This file defines ExternalProject targets for the third-party dependencies:
# GeographicLib and KDDockWidgets, and (through SolverSuperbuild.cmake) the
# solver dependencies oneTBB and GTSAM. It implements a superbuild pattern that
# builds these libraries with proper dependency handling.
#
# This replaces the Windows-specific BUILD.BAT with a portable, cross-platform
# CMake solution.
#
# Build Order and Dependencies:
#   1. GeographicLib  - No dependencies
#   2. KDDockWidgets  - No dependencies, can build in parallel
#   3. oneTBB, GTSAM  - See SolverSuperbuild.cmake (GTSAM depends on oneTBB)
#
# Each library is built in the configuration specified by CMAKE_BUILD_TYPE.
# For local development requiring both Debug and Release, run the build twice
# with different CMAKE_BUILD_TYPE values.
#
# =============================================================================

# =============================================================================
# Generator Detection and Configuration
# =============================================================================
#
# Detect the generator type to configure ExternalProject arguments correctly.
# Visual Studio generators require the -A (architecture) argument, while
# other generators (Ninja, Makefiles) do not.
#

get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

# Generator settings for ExternalProject
# Note: We use CMAKE_GENERATOR and CMAKE_GENERATOR_PLATFORM as ExternalProject
# parameters rather than passing -G/-A via CMAKE_ARGS. This avoids the
# "Multiple -A options not allowed" error that occurs when both the parent
# project's platform and an explicit -A argument are passed.

# =============================================================================
# Source Directory Variables
# =============================================================================
#
# Define paths to the third-party source directories.
# These use THIRD_PARTY_DIR which is defined in the root CMakeLists.txt.
#

set(GEOGRAPHIC_SOURCE_DIR "${THIRD_PARTY_DIR}/GeographicLib")
set(KDDW_SOURCE_DIR "${THIRD_PARTY_DIR}/KDDockWidgets")

# Verify source directories exist
foreach(_src_dir GEOGRAPHIC_SOURCE_DIR KDDW_SOURCE_DIR)
    if(NOT EXISTS "${${_src_dir}}")
        message(WARNING "Third-party source directory not found: ${${_src_dir}}")
        message(WARNING "Please ensure git submodules are initialized: git submodule update --init --recursive")
    endif()
endforeach()

# =============================================================================
# ext_GeographicLib - Geographic coordinate conversions
# =============================================================================
#
# GeographicLib provides geodesic computations and coordinate conversions
# (e.g., WGS84 to local Cartesian) used by the path simplification system.
#
# Key options:
#   - BUILD_SHARED_LIBS=ON     : Build as shared library
#   - GEOGRAPHICLIB_LIB_TYPE=SHARED : Explicit shared library type
#   - BUILD_BOTH_LIBS=OFF      : Only build one library type
#   - BUILD_DOCUMENTATION=OFF  : Skip documentation
#   - BUILD_MANPAGES=OFF       : Skip man pages
#   - BUILD_TOOLS=OFF          : Skip command-line tools
#   - BUILD_TESTING=OFF        : Skip tests
#

ExternalProject_Add(ext_GeographicLib
    SOURCE_DIR "${GEOGRAPHIC_SOURCE_DIR}"
    BINARY_DIR "${THIRD_PARTY_DIR}/GeographicLib-build"
    INSTALL_DIR "${GEOGRAPHIC_INSTALL_DIR}"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}"
    CMAKE_ARGS
        -DBUILD_SHARED_LIBS=ON
        -DGEOGRAPHICLIB_LIB_TYPE=SHARED
        -DBUILD_BOTH_LIBS=OFF
        -DBUILD_DOCUMENTATION=OFF
        -DBUILD_MANPAGES=OFF
        -DBUILD_TOOLS=OFF
        -DBUILD_TESTING=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
)

# =============================================================================
# ext_KDDockWidgets - KDE Docking Widget Framework
# =============================================================================
#
# KDDockWidgets provides advanced docking functionality for Qt applications.
# It has no dependencies on the other third-party libraries.
#
# Key options:
#   - KDDockWidgets_QT6=ON       : Build for Qt6
#   - KDDockWidgets_EXAMPLES=OFF : Skip example builds
#   - KDDockWidgets_TESTS=OFF    : Skip test builds
#   - KDDockWidgets_DOCS=OFF     : Skip documentation generation
#
# Note: Uses an out-of-source build directory matching the pattern from
#   BUILD.BAT (builds in third-party/KDDockWidgets-build, not in the
#   source tree). This avoids potential issues with in-source builds.
#

set(KDDW_BINARY_DIR "${THIRD_PARTY_DIR}/KDDockWidgets-build")

# Forward CMAKE_PREFIX_PATH so KDDockWidgets can find Qt on non-standard prefixes
set(_KDDW_CMAKE_ARGS
    -DKDDockWidgets_QT6=ON
    -DKDDockWidgets_EXAMPLES=OFF
    -DKDDockWidgets_TESTS=OFF
    -DKDDockWidgets_DOCS=OFF
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
)
if(CMAKE_PREFIX_PATH)
    list(APPEND _KDDW_CMAKE_ARGS "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}")
endif()

ExternalProject_Add(ext_KDDockWidgets
    SOURCE_DIR "${KDDW_SOURCE_DIR}"
    BINARY_DIR "${KDDW_BINARY_DIR}"
    INSTALL_DIR "${KDDW_INSTALL_DIR}"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}"
    CMAKE_ARGS ${_KDDW_CMAKE_ARGS}
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
)

# =============================================================================
# ext_oneTBB and ext_GTSAM - Solver dependencies
# =============================================================================
#
# Defined in their own file, together with their pinned revisions and the
# FLYSIGHT_BUILD_SOLVER_DEPS option. With that option OFF the file defines no
# target, which is why everything below tests for the targets.
#

include("${CMAKE_CURRENT_LIST_DIR}/SolverSuperbuild.cmake")

# =============================================================================
# Clean Targets
# =============================================================================
#
# Convenience targets to clean individual third-party builds or all of them.
# These replicate the functionality of CLEAN.BAT in a cross-platform way.
#
# Usage:
#   cmake --build build --target clean-GeographicLib
#   cmake --build build --target clean-KDDockWidgets
#   cmake --build build --target clean-third-party   (cleans all)
#

add_custom_target(clean-GeographicLib
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${THIRD_PARTY_DIR}/GeographicLib-build"
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${GEOGRAPHIC_INSTALL_DIR}"
    COMMENT "Cleaning GeographicLib build and install directories..."
)

add_custom_target(clean-KDDockWidgets
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${KDDW_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${KDDW_INSTALL_DIR}"
    COMMENT "Cleaning KDDockWidgets build and install directories..."
)

add_custom_target(clean-third-party
    DEPENDS clean-GeographicLib clean-KDDockWidgets
    COMMENT "Cleaning all third-party build and install directories..."
)

if(TARGET clean-GTSAM)
    add_dependencies(clean-third-party clean-oneTBB clean-GTSAM)
endif()

# =============================================================================
# Build Status Summary
# =============================================================================
#
# Display information about the configured third-party builds.
#

message(STATUS "")
message(STATUS "----------------------------------------")
message(STATUS "Third-Party Superbuild Configuration")
message(STATUS "----------------------------------------")
message(STATUS "")
message(STATUS "ExternalProject Targets:")
message(STATUS "  ext_GeographicLib -> ${GEOGRAPHIC_INSTALL_DIR}")
message(STATUS "  ext_KDDockWidgets -> ${KDDW_INSTALL_DIR}")
if(TARGET ext_GTSAM)
    message(STATUS "  ext_oneTBB        -> ${ONETBB_INSTALL_DIR}")
    message(STATUS "  ext_GTSAM         -> ${GTSAM_INSTALL_DIR}")
endif()
message(STATUS "")
message(STATUS "Dependency Graph:")
message(STATUS "  ext_GeographicLib : (no dependencies)")
message(STATUS "  ext_KDDockWidgets: (no dependencies)")
if(TARGET ext_GTSAM)
    message(STATUS "  ext_oneTBB       : (no dependencies)")
    message(STATUS "  ext_GTSAM        : depends on ext_oneTBB")
endif()
message(STATUS "")
message(STATUS "Build Order:")
message(STATUS "  1. ext_GeographicLib and ext_KDDockWidgets (in parallel)")
if(TARGET ext_GTSAM)
    message(STATUS "  2. ext_oneTBB, then ext_GTSAM (independent of step 1)")
endif()
message(STATUS "")
message(STATUS "Clean Targets Available:")
message(STATUS "  clean-GeographicLib, clean-KDDockWidgets, clean-third-party")
if(TARGET clean-GTSAM)
    message(STATUS "  clean-oneTBB, clean-GTSAM")
endif()
message(STATUS "")
message(STATUS "----------------------------------------")
message(STATUS "")
