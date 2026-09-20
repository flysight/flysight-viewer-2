# =============================================================================
# FlySight Viewer - Superbuild CMakeLists.txt
# =============================================================================
#
# This is the root CMakeLists.txt for the FlySight Viewer superbuild system.
# It orchestrates building all third-party dependencies and the main application
# using CMake's ExternalProject module for build isolation.
#
# Usage Examples:
#   # Build everything (third-party + application):
#   cmake -G "Visual Studio 17 2022" -A x64 -B build -S .
#   cmake --build build --config Release
#
#   # Build only third-party dependencies:
#   cmake -G "Visual Studio 17 2022" -A x64 -B build -S . -DFLYSIGHT_THIRD_PARTY_ONLY=ON
#   cmake --build build --config Release
#
#   # Skip third-party (use pre-built dependencies):
#   cmake -G "Visual Studio 17 2022" -A x64 -B build -S . -DFLYSIGHT_BUILD_THIRD_PARTY=OFF
#   cmake --build build --config Release
#
#   # Also build the test suite, then run it (see tests/README.md):
#   cmake -G "Visual Studio 17 2022" -A x64 -B build -S . -DFLYSIGHT_BUILD_TESTS=ON
#   cmake --build build --config Release
#   ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure
#
# =============================================================================

cmake_minimum_required(VERSION 3.18)

project(FlySightViewer-Superbuild LANGUAGES C CXX)

# Include the ExternalProject module for managing external builds
include(ExternalProject)

# =============================================================================
# Generator Configuration
# =============================================================================

# Provide information about the expected generator for Windows development
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    message(STATUS "Using Visual Studio generator: ${CMAKE_GENERATOR}")
    if(CMAKE_GENERATOR_PLATFORM)
        message(STATUS "Target platform: ${CMAKE_GENERATOR_PLATFORM}")
    endif()
else()
    message(STATUS "Using generator: ${CMAKE_GENERATOR}")
    message(STATUS "Note: Visual Studio 2022 is the primary generator for Windows development")
endif()

# =============================================================================
# Build Options
# =============================================================================
#
# These options control which parts of the build system are active.
#
# FLYSIGHT_BUILD_THIRD_PARTY: Build third-party dependencies (GeographicLib, KDDockWidgets)
#   - Set to OFF to use pre-built dependencies from third-party/*-install directories
#
# FLYSIGHT_BUILD_APP: Build the main FlySight Viewer application
#   - Set to OFF to skip building the application (useful for dependency builds only)
#
# FLYSIGHT_THIRD_PARTY_ONLY: Convenience option to build only third-party dependencies
#   - When ON, automatically disables FLYSIGHT_BUILD_APP
#   - Equivalent to: -DFLYSIGHT_BUILD_THIRD_PARTY=ON -DFLYSIGHT_BUILD_APP=OFF
#
# FLYSIGHT_BUILD_TESTS: Build the Qt Test suite under tests/ as part of the application build
#   - Forwarded to the application project (src/CMakeLists.txt), which owns the option
#   - Run with: ctest --test-dir <build>/FlySightViewer-build -C Release --output-on-failure
#
# FLYSIGHT_BUILD_PYTHON_TESTS: With FLYSIGHT_BUILD_TESTS, also build tst_python_bridge (default ON)
#   - Forwarded to the application project; tests/CMakeLists.txt owns the option
#   - Needs NumPy in the build-time Python; without it the test is registered but disabled
#

option(FLYSIGHT_BUILD_THIRD_PARTY "Build third-party dependencies" ON)
option(FLYSIGHT_BUILD_APP "Build the main FlySight Viewer application" ON)
option(FLYSIGHT_THIRD_PARTY_ONLY "Build only third-party dependencies (skip application)" OFF)
option(FLYSIGHT_BUILD_TESTS "Build the FlySight Viewer test suite" OFF)
option(FLYSIGHT_BUILD_PYTHON_TESTS "With FLYSIGHT_BUILD_TESTS: also build the embedded-Python plugin bridge test" ON)

# Handle option interaction: THIRD_PARTY_ONLY implies no app build
if(FLYSIGHT_THIRD_PARTY_ONLY)
    set(FLYSIGHT_BUILD_APP OFF)
    message(STATUS "FLYSIGHT_THIRD_PARTY_ONLY is ON - application build disabled")
endif()

# =============================================================================
# Path Variables for Third-Party Dependencies
# =============================================================================
#
# These paths define where third-party dependencies are located and installed.
# They match the existing directory structure created by BUILD.BAT.
# Users can override these via CMake cache or command line.
#

set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party"
    CACHE PATH "Root directory for third-party dependencies")

set(GEOGRAPHIC_INSTALL_DIR "${THIRD_PARTY_DIR}/GeographicLib-install"
    CACHE PATH "GeographicLib installation directory")

set(KDDW_INSTALL_DIR "${THIRD_PARTY_DIR}/KDDockWidgets-install"
    CACHE PATH "KDDockWidgets installation directory")

# =============================================================================
# Third-Party Superbuild
# =============================================================================
#
# Include the third-party build definitions if enabled.
# The ThirdPartySuperbuild.cmake file defines ExternalProject_Add calls
# for GeographicLib and KDDockWidgets.
#

if(FLYSIGHT_BUILD_THIRD_PARTY)
    set(_THIRD_PARTY_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThirdPartySuperbuild.cmake")
    if(EXISTS "${_THIRD_PARTY_CMAKE}")
        message(STATUS "Including third-party superbuild configuration")
        include("${_THIRD_PARTY_CMAKE}")
    else()
        message(WARNING "Third-party superbuild file not found: ${_THIRD_PARTY_CMAKE}")
        message(WARNING "Third-party dependencies will not be built.")
        message(WARNING "This file will be created in Phase 2 of the build system implementation.")
    endif()
else()
    message(STATUS "Third-party build disabled - using pre-built dependencies")
endif()

# =============================================================================
# Main Application
# =============================================================================
#
# Add the main FlySight Viewer application as an ExternalProject.
# This invokes the existing src/CMakeLists.txt which handles the actual
# application build, Qt integration, and all application-specific targets.
#

if(FLYSIGHT_BUILD_APP)
    message(STATUS "Application build enabled")

    # Collect third-party targets for DEPENDS clause
    set(THIRD_PARTY_TARGETS "")
    if(FLYSIGHT_BUILD_THIRD_PARTY)
        if(TARGET ext_GeographicLib)
            list(APPEND THIRD_PARTY_TARGETS ext_GeographicLib)
        endif()
        if(TARGET ext_KDDockWidgets)
            list(APPEND THIRD_PARTY_TARGETS ext_KDDockWidgets)
        endif()
    endif()

    # Build the main FlySight Viewer application
    # Forward CMAKE_PREFIX_PATH so the inner build can find Qt and other
    # packages (e.g., MacPorts installs Qt6 modules to /opt/local/libexec/qt6
    # which is not on the default search path).
    set(_APP_CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DTHIRD_PARTY_DIR=${THIRD_PARTY_DIR}
        -DGEOGRAPHIC_ROOT=${GEOGRAPHIC_INSTALL_DIR}
        -DKDDW_ROOT=${KDDW_INSTALL_DIR}
        # Forwarded unconditionally so that turning the option back OFF also
        # reaches the inner cache.
        -DFLYSIGHT_BUILD_TESTS=${FLYSIGHT_BUILD_TESTS}
        -DFLYSIGHT_BUILD_PYTHON_TESTS=${FLYSIGHT_BUILD_PYTHON_TESTS}
    )
    if(CMAKE_PREFIX_PATH)
        list(APPEND _APP_CMAKE_ARGS "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}")
    endif()
    if(Python_ROOT_DIR)
        list(APPEND _APP_CMAKE_ARGS "-DPython_ROOT_DIR=${Python_ROOT_DIR}")
    endif()
    if(Python_EXECUTABLE)
        list(APPEND _APP_CMAKE_ARGS "-DPython_EXECUTABLE=${Python_EXECUTABLE}")
    endif()
    if(FLYSIGHT_CODESIGN_IDENTITY)
        list(APPEND _APP_CMAKE_ARGS "-DFLYSIGHT_CODESIGN_IDENTITY=${FLYSIGHT_CODESIGN_IDENTITY}")
    endif()
    if(GOOGLE_MAPS_API_KEY)
        list(APPEND _APP_CMAKE_ARGS "-DGOOGLE_MAPS_API_KEY=${GOOGLE_MAPS_API_KEY}")
    endif()
    if(FLYSIGHT_VERSION_OVERRIDE)
        list(APPEND _APP_CMAKE_ARGS "-DFLYSIGHT_VERSION_OVERRIDE=${FLYSIGHT_VERSION_OVERRIDE}")
    endif()

    ExternalProject_Add(FlySightViewer
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src"
        BINARY_DIR "${CMAKE_BINARY_DIR}/FlySightViewer-build"
        INSTALL_DIR "${CMAKE_BINARY_DIR}/install"
        CMAKE_GENERATOR "${CMAKE_GENERATOR}"
        CMAKE_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}"
        CMAKE_ARGS ${_APP_CMAKE_ARGS}
        BUILD_ALWAYS TRUE
        DEPENDS ${THIRD_PARTY_TARGETS}
    )
else()
    message(STATUS "Application build disabled")
endif()

# =============================================================================
# Top-Level Install Rules
# =============================================================================
#
# The application is built and installed by ExternalProject into
# ${CMAKE_BINARY_DIR}/install/ during the build step. These rules forward
# that installation to the user-specified --prefix so that
# `cmake --install build --prefix dist` works as expected.
#

if(FLYSIGHT_BUILD_APP)
    install(DIRECTORY "${CMAKE_BINARY_DIR}/install/"
        DESTINATION "."
        USE_SOURCE_PERMISSIONS
    )
endif()

# =============================================================================
# Build Configuration Summary
# =============================================================================

message(STATUS "")
message(STATUS "========================================")
message(STATUS "FlySight Viewer Superbuild Configuration")
message(STATUS "========================================")
message(STATUS "")
message(STATUS "CMake Version:              ${CMAKE_VERSION}")
message(STATUS "CMake Generator:            ${CMAKE_GENERATOR}")
if(CMAKE_GENERATOR_PLATFORM)
    message(STATUS "Generator Platform:         ${CMAKE_GENERATOR_PLATFORM}")
endif()
message(STATUS "")
message(STATUS "Build Options:")
message(STATUS "  FLYSIGHT_BUILD_THIRD_PARTY: ${FLYSIGHT_BUILD_THIRD_PARTY}")
message(STATUS "  FLYSIGHT_BUILD_APP:         ${FLYSIGHT_BUILD_APP}")
message(STATUS "  FLYSIGHT_THIRD_PARTY_ONLY:  ${FLYSIGHT_THIRD_PARTY_ONLY}")
message(STATUS "  FLYSIGHT_BUILD_TESTS:       ${FLYSIGHT_BUILD_TESTS}")
message(STATUS "  FLYSIGHT_BUILD_PYTHON_TESTS: ${FLYSIGHT_BUILD_PYTHON_TESTS}")
message(STATUS "")
message(STATUS "Path Variables:")
message(STATUS "  THIRD_PARTY_DIR:       ${THIRD_PARTY_DIR}")
message(STATUS "  GEOGRAPHIC_INSTALL_DIR: ${GEOGRAPHIC_INSTALL_DIR}")
message(STATUS "  KDDW_INSTALL_DIR:      ${KDDW_INSTALL_DIR}")
message(STATUS "")
message(STATUS "========================================")
message(STATUS "")
