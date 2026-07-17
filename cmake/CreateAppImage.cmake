# =============================================================================
# CreateAppImage.cmake
# =============================================================================
#
# Downloads appimagetool and generates the final AppImage file from the
# prepared AppDir structure.
#
# This module:
# 1. Downloads appimagetool if not present
# 2. Creates a custom target 'appimage' for AppImage generation
# 3. Handles environments without FUSE using APPIMAGE_EXTRACT_AND_RUN=1
# 4. Performs pre-flight verification of all critical components
#
# =============================================================================
# Troubleshooting AppImage Creation
# =============================================================================
#
# Common issues:
#
# 1. "AppImage fails to run with 'cannot open shared object file'"
#    - Check that RPATH is set correctly: patchelf --print-rpath path/to/binary
#    - Expected RPATH for executable: $ORIGIN/../lib
#    - Expected RPATH for bridge module: $ORIGIN/../../../../lib
#    - Run with FLYSIGHT_DEBUG=1 to see library paths
#
# 2. "FUSE not available" error when running AppImage
#    - Install fuse2: sudo apt-get install libfuse2
#    - Or run with: ./FlySightViewer.AppImage --appimage-extract-and-run
#
# 3. "Qt platform plugin could not be initialized"
#    - Verify libqxcb.so exists in usr/plugins/platforms/
#    - Check qt.conf exists in usr/bin/
#    - Verify XCB dependencies installed on host system
#
# 4. "Python import flysight_cpp_bridge failed"
#    - Verify bridge module is in usr/share/python/lib/pythonX.X/site-packages/
#    - Check PYTHONPATH in AppRun matches Python version
#    - Verify bridge module RPATH points to usr/lib/
#
# 5. AppImage size is unexpectedly large
#    - Check for duplicate libraries
#    - Verify only required libraries are bundled
#
# =============================================================================

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

# Ensure APPDIR variables are set
if(NOT DEFINED FLYSIGHT_APPDIR_PATH)
    set(FLYSIGHT_APPDIR_PATH "${CMAKE_INSTALL_PREFIX}/FlySightViewer.AppDir")
    set(FLYSIGHT_APPDIR_USR "${FLYSIGHT_APPDIR_PATH}/usr")
endif()

# =============================================================================
# Configuration
# =============================================================================

# appimagetool download configuration
# Detect architecture for correct download
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(APPIMAGETOOL_ARCH "aarch64")
else()
    set(APPIMAGETOOL_ARCH "x86_64")
endif()

set(APPIMAGETOOL_VERSION "continuous" CACHE STRING "appimagetool version to download")
set(APPIMAGETOOL_URL "https://github.com/AppImage/appimagetool/releases/download/${APPIMAGETOOL_VERSION}/appimagetool-${APPIMAGETOOL_ARCH}.AppImage")
set(APPIMAGETOOL_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/tools")
set(APPIMAGETOOL_PATH "${APPIMAGETOOL_DOWNLOAD_DIR}/appimagetool")

# Output AppImage configuration
set(APPIMAGE_OUTPUT_DIR "${CMAKE_BINARY_DIR}" CACHE PATH "Directory for generated AppImage")
set(APPIMAGE_FILENAME "FlySightViewer-${PROJECT_VERSION}-${APPIMAGETOOL_ARCH}.AppImage" CACHE STRING "Output AppImage filename")

# =============================================================================
# Download appimagetool at configure time
# =============================================================================

message(STATUS "CreateAppImage: Configuring AppImage generation")
message(STATUS "  Architecture: ${APPIMAGETOOL_ARCH}")
message(STATUS "  Output: ${APPIMAGE_OUTPUT_DIR}/${APPIMAGE_FILENAME}")

# Create tools directory
file(MAKE_DIRECTORY "${APPIMAGETOOL_DOWNLOAD_DIR}")

# Download appimagetool if not present
if(NOT EXISTS "${APPIMAGETOOL_PATH}")
    message(STATUS "Downloading appimagetool...")
    file(DOWNLOAD
        "${APPIMAGETOOL_URL}"
        "${APPIMAGETOOL_PATH}"
        SHOW_PROGRESS
        STATUS download_status
        TLS_VERIFY ON
    )
    list(GET download_status 0 download_error)
    if(download_error)
        list(GET download_status 1 download_message)
        message(WARNING "Failed to download appimagetool: ${download_message}")
        message(WARNING "AppImage generation will not be available")
        set(FLYSIGHT_APPIMAGETOOL_AVAILABLE FALSE CACHE INTERNAL "")
    else()
        # Make it executable
        execute_process(COMMAND chmod +x "${APPIMAGETOOL_PATH}")
        message(STATUS "appimagetool downloaded successfully")
        set(FLYSIGHT_APPIMAGETOOL_AVAILABLE TRUE CACHE INTERNAL "")
    endif()
else()
    message(STATUS "Using cached appimagetool: ${APPIMAGETOOL_PATH}")
    set(FLYSIGHT_APPIMAGETOOL_AVAILABLE TRUE CACHE INTERNAL "")
endif()

# =============================================================================
# Custom target for AppImage generation
# =============================================================================

if(FLYSIGHT_APPIMAGETOOL_AVAILABLE)
    # Create a script that will be run by the custom target
    set(APPIMAGE_SCRIPT "${CMAKE_BINARY_DIR}/create_appimage.sh")

    file(WRITE "${APPIMAGE_SCRIPT}" [=[#!/bin/bash
# AppImage creation script for FlySight Viewer
# Generated by CMake - do not edit directly

set -e

APPDIR="@FLYSIGHT_APPDIR_PATH@"
APPIMAGETOOL="@APPIMAGETOOL_PATH@"
OUTPUT_DIR="@APPIMAGE_OUTPUT_DIR@"
OUTPUT_NAME="@APPIMAGE_FILENAME@"
OUTPUT_PATH="${OUTPUT_DIR}/${OUTPUT_NAME}"

echo "=== Creating AppImage ==="
echo "AppDir: ${APPDIR}"
echo "Output: ${OUTPUT_PATH}"

# Verify AppDir exists
if [ ! -d "${APPDIR}" ]; then
    echo "ERROR: AppDir not found at ${APPDIR}"
    echo "Run 'cmake --install <build-dir>' first to create the AppDir"
    exit 1
fi

# Verify AppRun exists and is executable
if [ ! -x "${APPDIR}/AppRun" ]; then
    echo "ERROR: AppRun not found or not executable"
    exit 1
fi

# Verify desktop file exists
if [ ! -f "${APPDIR}/FlySightViewer.desktop" ]; then
    echo "ERROR: Desktop file not found"
    exit 1
fi

# Verify executable exists
if [ ! -x "${APPDIR}/usr/bin/flysight-viewer" ]; then
    echo "ERROR: flysight-viewer executable not found"
    exit 1
fi

# =============================================================================
# Pre-Flight Verification
# =============================================================================
# Check for all critical components before running appimagetool

echo ""
echo "=== Pre-flight verification ==="

# Verify critical libraries
echo "--- Verifying library dependencies ---"
LIB_DIR="${APPDIR}/usr/lib"

# Check third-party libraries
for lib in libkddockwidgets libGeographic; do
    found=$(find "${LIB_DIR}" -name "${lib}*.so*" 2>/dev/null | head -1)
    if [ -n "$found" ]; then
        echo "  Found: $(basename $found)"
    else
        echo "WARNING: ${lib} not found in ${LIB_DIR}"
    fi
done

echo ""
echo "--- Verifying Qt components ---"

# Check Qt XCB plugin (critical for X11 display)
XCB_PLUGIN="${APPDIR}/usr/plugins/platforms/libqxcb.so"
if [ -f "${XCB_PLUGIN}" ]; then
    echo "  Qt XCB plugin: Found"
else
    echo "WARNING: Qt XCB platform plugin not found!"
    echo "  Expected at: ${XCB_PLUGIN}"
    echo "  The AppImage may not run on most Linux systems."
fi

# Check qt.conf
if [ -f "${APPDIR}/usr/bin/qt.conf" ]; then
    echo "  qt.conf: Found"
else
    echo "WARNING: qt.conf not found in usr/bin/"
fi

# Check QML modules
for mod in QtMultimedia; do
    if [ -d "${APPDIR}/usr/qml/${mod}" ]; then
        echo "  QML ${mod}: Found"
    else
        echo "WARNING: QML module ${mod} not found"
    fi
done

echo ""
echo "--- Verifying Python bundle ---"

# Check Python bundle
PYTHON_DIR="${APPDIR}/usr/share/python"
if [ -d "${PYTHON_DIR}" ]; then
    echo "  Python bundle: Found"
    PYTHON_BIN="${PYTHON_DIR}/bin/python3"
    if [ -x "${PYTHON_BIN}" ]; then
        echo "  Python executable: OK"
    else
        echo "WARNING: Python executable not found or not executable"
    fi
else
    echo "WARNING: Python bundle not found at ${PYTHON_DIR}"
fi

# Check pybind11 bridge module
BRIDGE_SO=$(find "${PYTHON_DIR}" -name "flysight_cpp_bridge*.so" 2>/dev/null | head -1)
if [ -n "${BRIDGE_SO}" ]; then
    echo "  Bridge module: $(basename ${BRIDGE_SO})"
    # Check bridge RPATH if patchelf is available
    if command -v patchelf &> /dev/null; then
        BRIDGE_RPATH=$(patchelf --print-rpath "${BRIDGE_SO}" 2>/dev/null || true)
        if [ -n "${BRIDGE_RPATH}" ]; then
            echo "  Bridge RPATH: ${BRIDGE_RPATH}"
        fi
    fi
else
    echo "WARNING: pybind11 bridge module not found"
fi

echo ""
echo "=== Pre-flight verification complete ==="
echo ""

# Create output directory if needed
mkdir -p "${OUTPUT_DIR}"

# Set environment for AppImage extraction (works without FUSE)
export APPIMAGE_EXTRACT_AND_RUN=1

# Remove old AppImage if it exists
rm -f "${OUTPUT_PATH}"

# Record which appimagetool build we are running (continuous is a moving tag)
echo "appimagetool version:"
timeout 2m "${APPIMAGETOOL}" --version || echo "(--version failed with exit code $?)"

# Run appimagetool with a hard timeout so a hang fails the step with logs
# intact instead of starving the runner until it loses communication
echo "Running appimagetool..."
set +e
timeout 15m "${APPIMAGETOOL}" "${APPDIR}" "${OUTPUT_PATH}"
APPIMAGETOOL_RC=$?
set -e
if [ "${APPIMAGETOOL_RC}" -eq 124 ]; then
    echo "ERROR: appimagetool timed out after 15 minutes"
    exit 1
elif [ "${APPIMAGETOOL_RC}" -ne 0 ]; then
    echo "ERROR: appimagetool failed with exit code ${APPIMAGETOOL_RC}"
    exit 1
fi

# Verify output
if [ -f "${OUTPUT_PATH}" ]; then
    echo "=== AppImage created successfully ==="
    echo "Output: ${OUTPUT_PATH}"
    ls -lh "${OUTPUT_PATH}"

    # Make it executable
    chmod +x "${OUTPUT_PATH}"

    echo ""
    echo "To run the AppImage:"
    echo "  ${OUTPUT_PATH}"
    echo ""
    echo "Or extract and run (if FUSE is unavailable):"
    echo "  ${OUTPUT_PATH} --appimage-extract-and-run"
    echo ""
    echo "For debugging, run with:"
    echo "  FLYSIGHT_DEBUG=1 ${OUTPUT_PATH}"
else
    echo "ERROR: AppImage was not created"
    exit 1
fi
]=])

    # Configure the script with actual paths
    string(CONFIGURE "${APPIMAGE_SCRIPT}" APPIMAGE_SCRIPT_CONTENT @ONLY)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/create_appimage.sh"
         CONTENT "${APPIMAGE_SCRIPT_CONTENT}")

    # Make script executable at configure time
    # Note: file(GENERATE) runs at generation time, so we use install(CODE) for chmod
    install(CODE "
        execute_process(COMMAND chmod +x \"${CMAKE_BINARY_DIR}/create_appimage.sh\")
    " COMPONENT AppImage)

    # Add custom target for AppImage creation
    add_custom_target(appimage
        COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --config $<CONFIG>
        COMMAND bash "${CMAKE_BINARY_DIR}/create_appimage.sh"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Creating AppImage..."
        VERBATIM
    )

    # Make the appimage target depend on the main target
    add_dependencies(appimage FlySightViewer flysight_cpp_bridge)

    message(STATUS "CreateAppImage: 'appimage' target available")
    message(STATUS "  Build with: cmake --build <build-dir> --target appimage")
else()
    message(STATUS "CreateAppImage: appimagetool not available")
    message(STATUS "  AppImage generation disabled")
endif()

# =============================================================================
# Alternative: Install-time AppImage creation
# =============================================================================

# This provides an alternative way to create AppImage during install
# Useful for CI/CD pipelines that run 'cmake --install' directly

option(FLYSIGHT_CREATE_APPIMAGE_ON_INSTALL "Create AppImage during cmake --install" OFF)

if(FLYSIGHT_CREATE_APPIMAGE_ON_INSTALL AND FLYSIGHT_APPIMAGETOOL_AVAILABLE)
    install(CODE "
        message(STATUS \"Creating AppImage...\")

        set(APPDIR \"${FLYSIGHT_APPDIR_PATH}\")
        set(APPIMAGETOOL \"${APPIMAGETOOL_PATH}\")
        set(OUTPUT_DIR \"${APPIMAGE_OUTPUT_DIR}\")
        set(OUTPUT_NAME \"${APPIMAGE_FILENAME}\")
        set(OUTPUT_PATH \"\${OUTPUT_DIR}/\${OUTPUT_NAME}\")

        # Set environment for FUSE-less operation
        set(ENV{APPIMAGE_EXTRACT_AND_RUN} \"1\")

        # Run appimagetool
        execute_process(
            COMMAND \"\${APPIMAGETOOL}\" \"\${APPDIR}\" \"\${OUTPUT_PATH}\"
            RESULT_VARIABLE appimage_result
            OUTPUT_VARIABLE appimage_output
            ERROR_VARIABLE appimage_error
        )

        if(appimage_result EQUAL 0)
            message(STATUS \"AppImage created: \${OUTPUT_PATH}\")
            # Make it executable
            execute_process(COMMAND chmod +x \"\${OUTPUT_PATH}\")
        else()
            message(WARNING \"Failed to create AppImage:\")
            message(STATUS \"  \${appimage_error}\")
        endif()
    " COMPONENT AppImage)
endif()

message(STATUS "CreateAppImage.cmake: Configuration complete")
