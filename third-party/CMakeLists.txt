# =============================================================================
# FlySight Viewer - Standalone Third-Party Build
# =============================================================================
#
# This CMakeLists.txt provides a standalone way to build all third-party
# dependencies without involving the root superbuild. It reuses the
# ExternalProject definitions from cmake/ThirdPartySuperbuild.cmake.
#
# Usage:
#   cd third-party
#   cmake -G "Visual Studio 17 2022" -A x64 -B build -S .
#   cmake --build build --config Release
#   cmake --build build --config Debug
#
# Clean:
#   cmake --build build --target clean-all
#   cmake --build build --target clean-third-party
#   cmake --build build --target clean-GeographicLib
#   cmake --build build --target clean-KDDockWidgets
#
# =============================================================================

cmake_minimum_required(VERSION 3.18)

project(FlySightViewer-ThirdParty LANGUAGES C CXX)

# Include the ExternalProject module for managing external builds
include(ExternalProject)

# =============================================================================
# Path Variables for Third-Party Dependencies
# =============================================================================
#
# Define paths that ThirdPartySuperbuild.cmake expects.
# When running from third-party/, CMAKE_CURRENT_SOURCE_DIR IS the third-party
# directory, so paths are relative to here.
#
# These paths match the existing directory structure.
#

set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    CACHE PATH "Root directory for third-party dependencies")

set(GEOGRAPHIC_INSTALL_DIR "${THIRD_PARTY_DIR}/GeographicLib-install"
    CACHE PATH "GeographicLib installation directory")

set(KDDW_INSTALL_DIR "${THIRD_PARTY_DIR}/KDDockWidgets-install"
    CACHE PATH "KDDockWidgets installation directory")

set(GTSAM_INSTALL_DIR "${THIRD_PARTY_DIR}/GTSAM-install"
    CACHE PATH "GTSAM installation directory")

set(ONETBB_INSTALL_DIR "${THIRD_PARTY_DIR}/oneTBB-install"
    CACHE PATH "oneTBB installation directory")

# =============================================================================
# Include ThirdPartySuperbuild.cmake
# =============================================================================
#
# Reuse ExternalProject definitions from the parent cmake/ directory.
# We set FLYSIGHT_CMAKE_DIR so ThirdPartySuperbuild.cmake can find
# helper scripts correctly.
#

# Tell ThirdPartySuperbuild.cmake where to find cmake/ directory
set(FLYSIGHT_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cmake"
    CACHE PATH "Directory containing FlySight CMake modules")

# Include the superbuild definitions
if(EXISTS "${FLYSIGHT_CMAKE_DIR}/ThirdPartySuperbuild.cmake")
    include("${FLYSIGHT_CMAKE_DIR}/ThirdPartySuperbuild.cmake")
else()
    message(FATAL_ERROR
        "ThirdPartySuperbuild.cmake not found at: ${FLYSIGHT_CMAKE_DIR}/ThirdPartySuperbuild.cmake\n"
        "This file is required for the standalone third-party build.\n"
        "Please ensure you are running from the correct directory.")
endif()

# =============================================================================
# Additional Targets for Standalone Build
# =============================================================================
#
# The clean-all target provides an alias for clean-third-party that matches
# the CLEAN.BAT naming convention.
#

add_custom_target(clean-all
    DEPENDS clean-third-party
    COMMENT "Cleaning all third-party build and install directories"
)

# =============================================================================
# Standalone Build Configuration Summary
# =============================================================================

message(STATUS "")
message(STATUS "========================================")
message(STATUS "Standalone Third-Party Build")
message(STATUS "========================================")
message(STATUS "")
message(STATUS "CMake Version:  ${CMAKE_VERSION}")
message(STATUS "CMake Generator: ${CMAKE_GENERATOR}")
if(CMAKE_GENERATOR_PLATFORM)
    message(STATUS "Generator Platform: ${CMAKE_GENERATOR_PLATFORM}")
endif()
message(STATUS "")
message(STATUS "Install Directories:")
message(STATUS "  GeographicLib: ${GEOGRAPHIC_INSTALL_DIR}")
message(STATUS "  KDDockWidgets: ${KDDW_INSTALL_DIR}")
message(STATUS "  GTSAM:         ${GTSAM_INSTALL_DIR}")
message(STATUS "  oneTBB:        ${ONETBB_INSTALL_DIR}")
message(STATUS "")
message(STATUS "Build Targets:")
message(STATUS "  ext_GeographicLib - Build GeographicLib")
message(STATUS "  ext_KDDockWidgets - Build KDDockWidgets")
if(TARGET ext_GTSAM)
    message(STATUS "  ext_oneTBB        - Build oneTBB")
    message(STATUS "  ext_GTSAM         - Build GTSAM (after ext_oneTBB)")
endif()
message(STATUS "")
message(STATUS "Clean Targets:")
message(STATUS "  clean-all           - Clean all third-party (alias for clean-third-party)")
message(STATUS "  clean-third-party   - Clean all third-party builds and installs")
message(STATUS "  clean-GeographicLib - Clean only GeographicLib")
message(STATUS "  clean-KDDockWidgets - Clean only KDDockWidgets")
message(STATUS "")
message(STATUS "Usage:")
message(STATUS "  Build all:    cmake --build build --config Release")
message(STATUS "  Build Debug:  cmake --build build --config Debug")
message(STATUS "  Clean all:    cmake --build build --target clean-all")
message(STATUS "")
message(STATUS "========================================")
message(STATUS "")
