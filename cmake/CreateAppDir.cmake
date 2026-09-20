# =============================================================================
# CreateAppDir.cmake
# =============================================================================
#
# Creates the Linux AppDir directory structure required for AppImage creation.
# This module sets up the AppRun launcher, desktop file, and proper directory
# hierarchy following the AppImage specification.
#
# AppDir Structure:
#   FlySightViewer.AppDir/
#   ├── AppRun                    (executable launcher script)
#   ├── FlySightViewer.desktop    (desktop entry file)
#   ├── FlySightViewer.png        (application icon)
#   └── usr/
#       ├── bin/                  (executables + qt.conf)
#       ├── lib/                  (shared libraries with RPATH $ORIGIN/../lib)
#       ├── plugins/              (Qt plugins - platforms, etc.)
#       ├── qml/                  (QML modules - QtMultimedia)
#       └── share/
#           ├── python/           (bundled Python runtime)
#           │   ├── bin/
#           │   └── lib/python3.XX/site-packages/
#           │       └── flysight_cpp_bridge.so  (pybind11 module)
#           └── applications/     (desktop files for XDG)
#
# Troubleshooting:
#   - "error while loading shared libraries": Check RPATH with patchelf --print-rpath
#   - "Qt plugin not found": Verify qt.conf paths and QT_PLUGIN_PATH in AppRun
#   - "Python import error": Check PYTHONPATH matches actual Python version
#   - "cannot execute binary file": Ensure AppRun is executable (chmod +x)
#
# Related files:
#   - cmake/DeployThirdPartyLinux.cmake - Library copying and RPATH
#   - cmake/BundlePythonLinux.cmake - Python runtime bundling
#   - cmake/CreateAppImage.cmake - AppImage generation
#
# =============================================================================

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

# Define the AppDir location within the install prefix
set(APPDIR_NAME "FlySightViewer.AppDir")
set(APPDIR_PATH "${CMAKE_INSTALL_PREFIX}/${APPDIR_NAME}")
set(APPDIR_USR "${APPDIR_PATH}/usr")

# Export these for use by other modules
set(FLYSIGHT_APPDIR_PATH "${APPDIR_PATH}" CACHE INTERNAL "Path to AppDir")
set(FLYSIGHT_APPDIR_USR "${APPDIR_USR}" CACHE INTERNAL "Path to AppDir/usr")

# =============================================================================
# AppRun Script
# =============================================================================
# The AppRun script is the entry point for the AppImage. It sets up the
# environment variables needed to locate libraries, Qt plugins, and Python.
# The Python version is substituted from BUNDLE_PYTHON_VERSION at configure time.

set(APPRUN_CONTENT [=[#!/bin/bash
# AppRun launcher for FlySight Viewer AppImage
# Sets up environment variables and launches the application

# Get the directory where AppRun is located (the AppDir root)
HERE="$(dirname "$(readlink -f "${0}")")"

# Library paths for Qt, third-party libs, and Python
export LD_LIBRARY_PATH="${HERE}/usr/lib:${HERE}/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH}"

# Qt plugin paths
export QT_PLUGIN_PATH="${HERE}/usr/plugins:${HERE}/usr/lib/qt6/plugins:${QT_PLUGIN_PATH}"

# Qt QML paths
export QML2_IMPORT_PATH="${HERE}/usr/qml:${HERE}/usr/lib/qt6/qml:${QML2_IMPORT_PATH}"

# Python environment for bundled runtime
# Note: Python version is substituted by CMake at configure time
if [ -d "${HERE}/usr/share/python" ]; then
    export PYTHONHOME="${HERE}/usr/share/python"
    export PYTHONPATH="${HERE}/usr/share/python/lib/python@BUNDLE_PYTHON_VERSION@/site-packages:${PYTHONPATH}"
fi

# XDG paths for desktop integration
export XDG_DATA_DIRS="${HERE}/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"

# Debug mode: set FLYSIGHT_DEBUG=1 to see library loading info
if [ -n "${FLYSIGHT_DEBUG}" ]; then
    echo "=== FlySight Viewer Debug Mode ==="
    echo "AppDir: ${HERE}"
    echo "LD_LIBRARY_PATH: ${LD_LIBRARY_PATH}"
    echo "QT_PLUGIN_PATH: ${QT_PLUGIN_PATH}"
    echo "QML2_IMPORT_PATH: ${QML2_IMPORT_PATH}"
    echo "PYTHONHOME: ${PYTHONHOME}"
    echo "PYTHONPATH: ${PYTHONPATH}"
    echo ""
    echo "=== Library directory contents ==="
    ls -la "${HERE}/usr/lib/" 2>/dev/null | head -20
    echo ""
    echo "=== Checking critical libraries ==="
    for lib in libkddockwidgets libGeographic; do
        found=$(find "${HERE}/usr/lib" -name "${lib}*.so*" 2>/dev/null | head -1)
        if [ -n "$found" ]; then
            echo "  $lib: $(basename $found)"
        else
            echo "  $lib: NOT FOUND"
        fi
    done
    echo ""
    echo "=== Checking Qt plugins ==="
    if [ -d "${HERE}/usr/plugins/platforms" ]; then
        echo "  Platform plugins: $(ls "${HERE}/usr/plugins/platforms/" 2>/dev/null)"
    else
        echo "  Platform plugins: MISSING"
    fi
    echo ""
    echo "=== Checking Python bundle ==="
    if [ -d "${HERE}/usr/share/python" ]; then
        echo "  Python bundle: Found"
        if [ -x "${HERE}/usr/share/python/bin/python3" ]; then
            echo "  Python version: $("${HERE}/usr/share/python/bin/python3" --version 2>&1)"
        fi
        BRIDGE=$(find "${HERE}/usr/share/python" -name "flysight_cpp_bridge*.so" 2>/dev/null | head -1)
        if [ -n "$BRIDGE" ]; then
            echo "  Bridge module: $(basename $BRIDGE)"
        else
            echo "  Bridge module: NOT FOUND"
        fi
    else
        echo "  Python bundle: MISSING"
    fi
    echo "=================================="
fi

# Launch the application
exec "${HERE}/usr/bin/flysight-viewer" "$@"
]=])

# =============================================================================
# Desktop Entry File
# =============================================================================
# The desktop file provides metadata for desktop integration.

set(DESKTOP_CONTENT [=[[Desktop Entry]
Type=Application
Name=FlySight Viewer
Comment=Data analysis and visualization for FlySight
Exec=flysight-viewer %F
Icon=FlySightViewer
Terminal=false
Categories=Science;DataVisualization;
MimeType=application/x-flysight;
Keywords=GPS;flight;skydiving;wingsuit;analysis;
]=])

# =============================================================================
# Install Commands
# =============================================================================

# Create AppDir directory structure at install time
install(CODE "
    message(STATUS \"Creating AppDir structure at: ${APPDIR_PATH}\")
    file(MAKE_DIRECTORY \"${APPDIR_PATH}\")
    file(MAKE_DIRECTORY \"${APPDIR_USR}/bin\")
    file(MAKE_DIRECTORY \"${APPDIR_USR}/lib\")
    file(MAKE_DIRECTORY \"${APPDIR_USR}/share/applications\")
    file(MAKE_DIRECTORY \"${APPDIR_USR}/share/python\")
    file(MAKE_DIRECTORY \"${APPDIR_USR}/plugins\")
    file(MAKE_DIRECTORY \"${APPDIR_USR}/qml\")
")

# Substitute Python version into AppRun content
# This ensures PYTHONPATH uses the same version as BUNDLE_PYTHON_VERSION
string(REPLACE "@BUNDLE_PYTHON_VERSION@" "${BUNDLE_PYTHON_VERSION}" APPRUN_CONTENT_CONFIGURED "${APPRUN_CONTENT}")

# Escape $ characters so bash variables survive CMake's install(CODE) substitution
# Without this, ${XDG_DATA_DIRS:-...} would be parsed as an invalid CMake variable
string(REPLACE "$" "\\$" APPRUN_CONTENT_ESCAPED "${APPRUN_CONTENT_CONFIGURED}")

# Write the AppRun script
install(CODE "
    set(APPRUN_CONTENT \"${APPRUN_CONTENT_ESCAPED}\")
    file(WRITE \"${APPDIR_PATH}/AppRun\" \"\${APPRUN_CONTENT}\")
    # Make AppRun executable
    execute_process(COMMAND chmod +x \"${APPDIR_PATH}/AppRun\")
    message(STATUS \"Created AppRun script with Python ${BUNDLE_PYTHON_VERSION}\")
")

# Write the desktop file
install(CODE "
    set(DESKTOP_CONTENT \"${DESKTOP_CONTENT}\")
    file(WRITE \"${APPDIR_PATH}/FlySightViewer.desktop\" \"\${DESKTOP_CONTENT}\")
    file(COPY \"${APPDIR_PATH}/FlySightViewer.desktop\"
         DESTINATION \"${APPDIR_USR}/share/applications\")
    message(STATUS \"Created desktop file\")
")

# =============================================================================
# Qt Configuration File (qt.conf)
# =============================================================================
# Helps Qt find plugins at runtime within the AppDir structure.
# Paths are relative to the binary directory (usr/bin/).

set(QTCONF_CONTENT "[Paths]
Prefix = ..
Plugins = plugins
Qml2Imports = qml
")

install(CODE "
    message(STATUS \"Creating qt.conf for AppDir...\")

    set(QTCONF_PATH \"${APPDIR_USR}/bin/qt.conf\")
    file(WRITE \"\${QTCONF_PATH}\" \"${QTCONF_CONTENT}\")

    message(STATUS \"  Created: \${QTCONF_PATH}\")
")

# Install the executable to AppDir/usr/bin
install(TARGETS FlySightViewer
    RUNTIME DESTINATION "${APPDIR_NAME}/usr/bin"
    COMPONENT AppImage
)

# Install the pybind11 bridge module to Python site-packages
# The module must be in site-packages for Python's import system to find it reliably
# BUNDLE_PYTHON_VERSION is set by BundlePythonLinux.cmake (included before this file)
if(NOT DEFINED BUNDLE_PYTHON_VERSION)
    set(BUNDLE_PYTHON_VERSION "3.13")
    message(WARNING "BUNDLE_PYTHON_VERSION not set, using default: ${BUNDLE_PYTHON_VERSION}")
endif()
install(TARGETS flysight_cpp_bridge
    LIBRARY DESTINATION "${APPDIR_NAME}/usr/share/python/lib/python${BUNDLE_PYTHON_VERSION}/site-packages"
    COMPONENT AppImage
)

# Install the icon (if it exists) to AppDir root
set(ICON_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/resources/FlySightViewer.png")
install(CODE "
    message(STATUS \"Installing application icon...\")

    set(ICON_SRC \"${ICON_SOURCE}\")
    set(APPDIR \"${APPDIR_PATH}\")

    if(EXISTS \"\${ICON_SRC}\")
        file(COPY \"\${ICON_SRC}\" DESTINATION \"\${APPDIR}\")
        message(STATUS \"  Installed icon from \${ICON_SRC}\")
    else()
        message(STATUS \"  Icon not found at \${ICON_SRC}, creating placeholder...\")

        # Create a minimal valid PNG placeholder
        # This is the binary content of a 1x1 blue PNG
        # Header: PNG signature (8 bytes) + IHDR chunk + IDAT chunk + IEND chunk
        # In production, replace with a proper 1024x1024 icon

        # For now, create an empty file and warn the user
        file(WRITE \"\${APPDIR}/FlySightViewer.png\" \"\")
        message(WARNING \"Created empty placeholder icon.\")
        message(STATUS \"Please provide a proper 1024x1024 PNG icon at:\")
        message(STATUS \"  ${CMAKE_CURRENT_SOURCE_DIR}/resources/FlySightViewer.png\")
    endif()

    # Also install to usr/share/icons for XDG compliance
    file(MAKE_DIRECTORY \"\${APPDIR}/usr/share/icons/hicolor/1024x1024/apps\")
    if(EXISTS \"\${APPDIR}/FlySightViewer.png\")
        file(COPY \"\${APPDIR}/FlySightViewer.png\"
             DESTINATION \"\${APPDIR}/usr/share/icons/hicolor/1024x1024/apps\")
    endif()
")

# Install plugin SDK and Python sources to site-packages
# Python version is set by BundlePythonLinux.cmake (included before this file).
# Provide fallback if that module wasn't included.
if(NOT DEFINED BUNDLE_PYTHON_VERSION)
    set(BUNDLE_PYTHON_VERSION "3.13")
endif()
install(DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../python_plugins/"
    DESTINATION "${APPDIR_NAME}/usr/share/python/lib/python${BUNDLE_PYTHON_VERSION}/site-packages"
    COMPONENT AppImage
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
    # site-packages is no place for an `examples` package or a README
    PATTERN "examples" EXCLUDE
    PATTERN "*.md" EXCLUDE
)

message(STATUS "CreateAppDir.cmake: AppDir structure will be created at install time")
message(STATUS "  AppDir path: ${APPDIR_PATH}")
