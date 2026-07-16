# =============================================================================
# BundlePythonLinux.cmake
# =============================================================================
#
# Downloads and bundles python-build-standalone into the AppDir, providing a
# portable Python runtime that matches the build-time Python version for
# pybind11 compatibility.
#
# The module:
# 1. Downloads python-build-standalone from GitHub releases at configure time
# 2. Supports both x86_64 and aarch64 architectures
# 3. Installs Python to usr/share/python/ in the AppDir
# 4. Installs NumPy and copies python_src/ to site-packages
# 5. Copies libpython shared library to lib directory with proper symlinks
#
# =============================================================================

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

# =============================================================================
# Helper function for robust file download with retries
# =============================================================================
function(_download_with_retry_linux URL OUTPUT_PATH MAX_RETRIES)
    set(_retry_count 0)
    set(_download_success FALSE)

    while(NOT _download_success AND _retry_count LESS ${MAX_RETRIES})
        math(EXPR _retry_count "${_retry_count} + 1")
        message(STATUS "Download attempt ${_retry_count}/${MAX_RETRIES}: ${URL}")

        file(DOWNLOAD
            "${URL}"
            "${OUTPUT_PATH}"
            SHOW_PROGRESS
            STATUS _download_status
            TLS_VERIFY ON
            TIMEOUT 300  # 5 minute timeout per attempt
        )

        list(GET _download_status 0 _download_error)
        if(_download_error EQUAL 0)
            set(_download_success TRUE)
        else()
            list(GET _download_status 1 _download_message)
            message(WARNING "Download attempt ${_retry_count} failed: ${_download_message}")
            if(_retry_count LESS ${MAX_RETRIES})
                # Remove partial download
                file(REMOVE "${OUTPUT_PATH}")
                message(STATUS "Retrying in a moment...")
            endif()
        endif()
    endwhile()

    # Return success status via parent scope variable
    set(DOWNLOAD_FAILED FALSE PARENT_SCOPE)
    if(NOT _download_success)
        message(WARNING
            "Failed to download after ${MAX_RETRIES} attempts.\n"
            "URL: ${URL}\n"
            "Python bundling will be skipped."
        )
        set(DOWNLOAD_FAILED TRUE PARENT_SCOPE)
    endif()
endfunction()

# Ensure APPDIR variables are set
if(NOT DEFINED FLYSIGHT_APPDIR_PATH)
    set(FLYSIGHT_APPDIR_PATH "${CMAKE_INSTALL_PREFIX}/FlySightViewer.AppDir")
    set(FLYSIGHT_APPDIR_USR "${FLYSIGHT_APPDIR_PATH}/usr")
endif()

# =============================================================================
# Configuration
# =============================================================================

# Python version to bundle - derived from build-time Python for pybind11 compatibility
if(DEFINED FLYSIGHT_PYTHON_VERSION)
    set(BUNDLE_PYTHON_VERSION "${FLYSIGHT_PYTHON_VERSION}" CACHE STRING "Python version to bundle")
else()
    set(BUNDLE_PYTHON_VERSION "3.13" CACHE STRING "Python version to bundle")
    message(WARNING "BundlePythonLinux: Using default Python version. Call find_package(Python) first for consistency.")
endif()

if(DEFINED FLYSIGHT_PYTHON_FULL_VERSION)
    set(BUNDLE_PYTHON_FULL_VERSION "${FLYSIGHT_PYTHON_FULL_VERSION}" CACHE STRING "Full Python version")
else()
    set(BUNDLE_PYTHON_FULL_VERSION "3.13.14" CACHE STRING "Full Python version")
endif()

# python-build-standalone release tag
# Check https://github.com/astral-sh/python-build-standalone/releases for latest
# NOTE: This release must contain the Python version pinned in .github/workflows/build.yml
set(PBS_RELEASE_TAG "20260623" CACHE STRING "python-build-standalone release tag")

# Note: BUNDLE_PYTHON_VERSION and BUNDLE_PYTHON_FULL_VERSION define the Python to bundle.
# These should match PYTHON_STANDALONE_PYTHON_VERSION in BundlePythonMacOS.cmake.
# The version should also match the build-time Python for pybind11 compatibility.
# PBS_RELEASE_TAG should match PYTHON_STANDALONE_VERSION in BundlePythonMacOS.cmake.

# NumPy version (optional pin for reproducibility)
set(NUMPY_VERSION "" CACHE STRING "Specific NumPy version to install (empty for latest)")
if(NUMPY_VERSION)
    set(_numpy_spec "numpy==${NUMPY_VERSION}")
else()
    set(_numpy_spec "numpy")
endif()

# Detect architecture
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(PBS_ARCH "aarch64")
else()
    set(PBS_ARCH "x86_64")
endif()

# Validate and log architecture selection
message(STATUS "BundlePythonLinux: System processor: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "BundlePythonLinux: Selected PBS architecture: ${PBS_ARCH}")

# CI environment detection
if(DEFINED ENV{CI})
    message(STATUS "BundlePythonLinux: Running in CI environment")
    if(DEFINED ENV{GITHUB_ACTIONS})
        message(STATUS "BundlePythonLinux: GitHub Actions runner architecture: $ENV{RUNNER_ARCH}")
    endif()
endif()

# Build the download URL
# Format: cpython-{version}+{tag}-{arch}-unknown-linux-gnu-install_only_stripped.tar.gz
set(PBS_FILENAME "cpython-${BUNDLE_PYTHON_FULL_VERSION}+${PBS_RELEASE_TAG}-${PBS_ARCH}-unknown-linux-gnu-install_only_stripped.tar.gz")
set(PBS_URL "https://github.com/astral-sh/python-build-standalone/releases/download/${PBS_RELEASE_TAG}/${PBS_FILENAME}")

# =============================================================================
# Pre-download URL validation
# =============================================================================
# Validate Python version is plausibly available in python-build-standalone
if(NOT BUNDLE_PYTHON_FULL_VERSION MATCHES "^3\\.(1[0-9]|[89])\\.[0-9]+$")
    message(WARNING
        "Python version ${BUNDLE_PYTHON_FULL_VERSION} may not be available in python-build-standalone.\n"
        "Supported versions are typically 3.8.x through 3.13.x.\n"
        "Check https://github.com/astral-sh/python-build-standalone/releases for available versions."
    )
endif()

# Validate architecture
if(NOT PBS_ARCH MATCHES "^(x86_64|aarch64)$")
    message(FATAL_ERROR
        "Unsupported architecture '${PBS_ARCH}' for python-build-standalone.\n"
        "Supported architectures: x86_64, aarch64"
    )
endif()

# Note: PBS_RELEASE_TAG must correspond to a release that includes the requested Python version
# If the download fails with 404, check:
# 1. The release tag exists: https://github.com/astral-sh/python-build-standalone/releases
# 2. The Python version is included in that release
# 3. The architecture is supported for that Python version

# Local paths
set(PBS_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/python-standalone")
set(PBS_ARCHIVE_PATH "${PBS_DOWNLOAD_DIR}/${PBS_FILENAME}")
set(PBS_EXTRACT_DIR "${PBS_DOWNLOAD_DIR}/extracted")

# =============================================================================
# Download python-build-standalone at configure time
# =============================================================================

message(STATUS "BundlePythonLinux: Configuring Python ${BUNDLE_PYTHON_FULL_VERSION} bundling")
message(STATUS "  Architecture: ${PBS_ARCH}")
message(STATUS "  Download URL: ${PBS_URL}")

# Create download directory
file(MAKE_DIRECTORY "${PBS_DOWNLOAD_DIR}")

# Download the archive if it doesn't exist (with retry logic)
if(NOT EXISTS "${PBS_ARCHIVE_PATH}")
    message(STATUS "Downloading python-build-standalone...")
    _download_with_retry_linux("${PBS_URL}" "${PBS_ARCHIVE_PATH}" 3)
    if(DOWNLOAD_FAILED)
        message(WARNING "Python bundling will be skipped. AppImage may not work correctly.")
        set(FLYSIGHT_PYTHON_BUNDLING_AVAILABLE FALSE CACHE INTERNAL "")
        return()
    endif()
    message(STATUS "Download complete: ${PBS_ARCHIVE_PATH}")
else()
    message(STATUS "Using cached python-build-standalone: ${PBS_ARCHIVE_PATH}")
endif()

# Extract the archive if not already extracted
set(PBS_PYTHON_DIR "${PBS_EXTRACT_DIR}/python")
if(NOT EXISTS "${PBS_PYTHON_DIR}")
    message(STATUS "Extracting python-build-standalone...")
    file(MAKE_DIRECTORY "${PBS_EXTRACT_DIR}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf "${PBS_ARCHIVE_PATH}"
        WORKING_DIRECTORY "${PBS_EXTRACT_DIR}"
        RESULT_VARIABLE extract_result
    )
    if(extract_result)
        message(WARNING "Failed to extract python-build-standalone")
        set(FLYSIGHT_PYTHON_BUNDLING_AVAILABLE FALSE CACHE INTERNAL "")
        return()
    endif()
    message(STATUS "Extraction complete")
endif()

set(FLYSIGHT_PYTHON_BUNDLING_AVAILABLE TRUE CACHE INTERNAL "")
set(FLYSIGHT_PBS_PYTHON_DIR "${PBS_PYTHON_DIR}" CACHE INTERNAL "Path to extracted Python")

# =============================================================================
# Install Commands
# =============================================================================

# Install Python runtime to AppDir
install(CODE "
    message(STATUS \"Installing bundled Python runtime...\")

    set(PBS_PYTHON_DIR \"${PBS_PYTHON_DIR}\")
    set(APPDIR_USR \"${FLYSIGHT_APPDIR_USR}\")
    set(PYTHON_VERSION \"${BUNDLE_PYTHON_VERSION}\")

    # Create target directories
    file(MAKE_DIRECTORY \"\${APPDIR_USR}/share/python\")
    file(MAKE_DIRECTORY \"\${APPDIR_USR}/lib\")

    # Copy Python installation
    if(EXISTS \"\${PBS_PYTHON_DIR}\")
        # Copy the entire Python directory structure
        file(COPY \"\${PBS_PYTHON_DIR}/\"
             DESTINATION \"\${APPDIR_USR}/share/python\"
             PATTERN \"__pycache__\" EXCLUDE
             PATTERN \"*.pyc\" EXCLUDE
             PATTERN \"test\" EXCLUDE
             PATTERN \"tests\" EXCLUDE
             PATTERN \"idle_test\" EXCLUDE
        )
        message(STATUS \"Copied Python runtime to \${APPDIR_USR}/share/python\")

        # Copy libpython to lib directory
        file(GLOB LIBPYTHON_FILES \"\${PBS_PYTHON_DIR}/lib/libpython*.so*\")
        foreach(lib \${LIBPYTHON_FILES})
            get_filename_component(libname \"\${lib}\" NAME)
            # Copy file (preserving symlinks)
            execute_process(
                COMMAND cp -P \"\${lib}\" \"\${APPDIR_USR}/lib/\"
                RESULT_VARIABLE cp_result
            )
            if(NOT cp_result)
                message(STATUS \"Copied \${libname} to lib/\")
            endif()
        endforeach()

        # Also ensure libpython is in share/python/lib for PYTHONHOME
        file(GLOB LIBPYTHON_FILES \"\${APPDIR_USR}/share/python/lib/libpython*.so*\")
        foreach(lib \${LIBPYTHON_FILES})
            get_filename_component(libname \"\${lib}\" NAME)
            if(NOT EXISTS \"\${APPDIR_USR}/lib/\${libname}\")
                execute_process(
                    COMMAND cp -P \"\${lib}\" \"\${APPDIR_USR}/lib/\"
                )
            endif()
        endforeach()
    else()
        message(WARNING \"Python source directory not found: \${PBS_PYTHON_DIR}\")
    endif()
")

# Install NumPy into the bundled Python
install(CODE "
    message(STATUS \"Installing NumPy into bundled Python...\")

    set(APPDIR_USR \"${FLYSIGHT_APPDIR_USR}\")
    set(PYTHON_VERSION \"${BUNDLE_PYTHON_VERSION}\")
    set(SITE_PACKAGES \"\${APPDIR_USR}/share/python/lib/python\${PYTHON_VERSION}/site-packages\")
    set(BUNDLED_PYTHON \"\${APPDIR_USR}/share/python/bin/python3\")
    set(NUMPY_SPEC \"${_numpy_spec}\")

    if(EXISTS \"\${BUNDLED_PYTHON}\")
        # Verify pip is available
        execute_process(
            COMMAND \"\${BUNDLED_PYTHON}\" -m pip --version
            RESULT_VARIABLE _pip_check_result
            OUTPUT_VARIABLE _pip_version
            ERROR_QUIET
        )
        if(_pip_check_result)
            message(STATUS \"pip not available, attempting to bootstrap...\")
            # Try ensurepip first (available in python-build-standalone)
            execute_process(
                COMMAND \"\${BUNDLED_PYTHON}\" -m ensurepip --upgrade
                RESULT_VARIABLE _ensurepip_result
                OUTPUT_VARIABLE _ensurepip_output
                ERROR_VARIABLE _ensurepip_error
            )
            if(_ensurepip_result)
                message(WARNING \"ensurepip failed: \${_ensurepip_error}\")
            else()
                message(STATUS \"pip bootstrapped successfully via ensurepip\")
            endif()
        else()
            string(STRIP \"\${_pip_version}\" _pip_version)
            message(STATUS \"pip available: \${_pip_version}\")
        endif()

        # Use pip from the bundled Python to install NumPy
        message(STATUS \"Installing NumPy spec: \${NUMPY_SPEC}\")
        execute_process(
            COMMAND \"\${BUNDLED_PYTHON}\" -m pip install --target \"\${SITE_PACKAGES}\" \${NUMPY_SPEC}
            RESULT_VARIABLE pip_result
            OUTPUT_VARIABLE pip_output
            ERROR_VARIABLE pip_error
        )
        if(pip_result)
            message(WARNING \"Failed to install NumPy: \${pip_error}\")
        else()
            message(STATUS \"NumPy installed successfully\")
        endif()
    else()
        message(WARNING \"Bundled Python not found at \${BUNDLED_PYTHON}\")
    endif()
")

# Install python_src contents to site-packages
install(CODE "
    message(STATUS \"Installing FlySight Python SDK...\")

    set(APPDIR_USR \"${FLYSIGHT_APPDIR_USR}\")
    set(PYTHON_VERSION \"${BUNDLE_PYTHON_VERSION}\")
    set(SITE_PACKAGES \"\${APPDIR_USR}/share/python/lib/python\${PYTHON_VERSION}/site-packages\")
    set(PYTHON_SRC_DIR \"${CMAKE_CURRENT_SOURCE_DIR}/../python_plugins\")

    file(MAKE_DIRECTORY \"\${SITE_PACKAGES}\")

    # Copy plugin SDK files
    if(EXISTS \"\${PYTHON_SRC_DIR}\")
        file(GLOB PYTHON_FILES \"\${PYTHON_SRC_DIR}/*.py\")
        foreach(pyfile \${PYTHON_FILES})
            get_filename_component(filename \"\${pyfile}\" NAME)
            file(COPY \"\${pyfile}\" DESTINATION \"\${SITE_PACKAGES}\")
            message(STATUS \"Installed \${filename} to site-packages\")
        endforeach()
    endif()
")

# Ensure lib-dynload is accessible
install(CODE "
    message(STATUS \"Setting up lib-dynload paths...\")

    set(APPDIR_USR \"${FLYSIGHT_APPDIR_USR}\")
    set(PYTHON_VERSION \"${BUNDLE_PYTHON_VERSION}\")

    # The lib-dynload directory contains C extension modules
    set(LIB_DYNLOAD_SRC \"\${APPDIR_USR}/share/python/lib/python\${PYTHON_VERSION}/lib-dynload\")
    set(LIB_DYNLOAD_DST \"\${APPDIR_USR}/lib/python\${PYTHON_VERSION}/lib-dynload\")

    if(EXISTS \"\${LIB_DYNLOAD_SRC}\")
        # Create symlink for lib-dynload in the lib directory
        get_filename_component(LIB_DYNLOAD_DST_PARENT \"\${LIB_DYNLOAD_DST}\" DIRECTORY)
        file(MAKE_DIRECTORY \"\${LIB_DYNLOAD_DST_PARENT}\")

        if(NOT EXISTS \"\${LIB_DYNLOAD_DST}\")
            execute_process(
                COMMAND ln -sf \"\${LIB_DYNLOAD_SRC}\" \"\${LIB_DYNLOAD_DST}\"
            )
            message(STATUS \"Created lib-dynload symlink\")
        endif()
    endif()
")

# =============================================================================
# Verify bundled Python installation
# =============================================================================
install(CODE "
    message(STATUS \"Verifying bundled Python installation...\")
    set(PYTHON_EXE \"${FLYSIGHT_APPDIR_USR}/share/python/bin/python3\")

    # Test Python interpreter
    execute_process(
        COMMAND \"\${PYTHON_EXE}\" -c \"import sys; print(f'Python {sys.version}')\"
        RESULT_VARIABLE _verify_result
        OUTPUT_VARIABLE _verify_output
        ERROR_VARIABLE _verify_error
    )
    if(_verify_result)
        message(WARNING \"Bundled Python verification failed: \${_verify_error}\")
    else()
        string(STRIP \"\${_verify_output}\" _verify_output)
        message(STATUS \"  \${_verify_output}\")
    endif()

    # Test NumPy import
    execute_process(
        COMMAND \"\${PYTHON_EXE}\" -c \"import numpy; print(f'NumPy {numpy.__version__}')\"
        RESULT_VARIABLE _numpy_result
        OUTPUT_VARIABLE _numpy_output
        ERROR_VARIABLE _numpy_error
    )
    if(_numpy_result)
        message(WARNING \"NumPy import failed: \${_numpy_error}\")
    else()
        string(STRIP \"\${_numpy_output}\" _numpy_output)
        message(STATUS \"  \${_numpy_output}\")
    endif()

    # Test that site-packages is accessible
    execute_process(
        COMMAND \"\${PYTHON_EXE}\" -c \"import site; print(f'Site packages: {site.getsitepackages()}')\"
        RESULT_VARIABLE _site_result
        OUTPUT_VARIABLE _site_output
    )
    if(NOT _site_result)
        string(STRIP \"\${_site_output}\" _site_output)
        message(STATUS \"  \${_site_output}\")
    endif()

    message(STATUS \"Python bundle verification complete\")
")

message(STATUS "BundlePythonLinux.cmake: Python bundling configured")
message(STATUS "  Python will be installed to: ${FLYSIGHT_APPDIR_USR}/share/python")
