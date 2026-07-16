# =============================================================================
# BundlePythonMacOS.cmake
# =============================================================================
#
# This module downloads and bundles python-build-standalone into a macOS
# .app bundle, providing a portable Python runtime that matches the build-time
# Python version for pybind11 compatibility.
#
# Usage:
#   include(BundlePythonMacOS)
#   bundle_python_macos(
#       TARGET FlySightViewer
#       PYTHON_VERSION "3.13"
#   )
#
# This module:
#   1. Downloads python-build-standalone from GitHub releases
#   2. Supports both arm64 (Apple Silicon) and x86_64 architectures
#   3. Installs Python to Contents/Resources/python/ in the app bundle
#   4. Copies libpython dylib to Contents/Frameworks/
#   5. Installs NumPy into the bundled site-packages
#   6. Copies python_src/ contents to site-packages
#
# =============================================================================

include_guard(GLOBAL)

# =============================================================================
# Helper function for robust file download with retries
# =============================================================================
function(_download_with_retry URL OUTPUT_PATH MAX_RETRIES)
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

    if(NOT _download_success)
        message(FATAL_ERROR
            "Failed to download after ${MAX_RETRIES} attempts.\n"
            "URL: ${URL}\n"
            "Please check network connectivity and try again."
        )
    endif()
endfunction()

# Python-build-standalone release information
# PBS_RELEASE_TAG corresponds to the release date from python-build-standalone
# NOTE: This release must contain the Python version pinned in .github/workflows/build.yml
set(PYTHON_STANDALONE_VERSION "20260623" CACHE STRING "python-build-standalone release date")

# Use build-time Python version for consistency with pybind11
# Fall back to default if called before find_package(Python)
if(DEFINED FLYSIGHT_PYTHON_FULL_VERSION)
    set(PYTHON_STANDALONE_PYTHON_VERSION "${FLYSIGHT_PYTHON_FULL_VERSION}" CACHE STRING "Python version in the standalone build")
else()
    set(PYTHON_STANDALONE_PYTHON_VERSION "3.13.14" CACHE STRING "Python version in the standalone build")
    message(WARNING "BundlePythonMacOS: Using default Python version. Call find_package(Python) first for consistency.")
endif()

# Base URL for python-build-standalone releases
set(PYTHON_STANDALONE_BASE_URL
    "https://github.com/astral-sh/python-build-standalone/releases/download/${PYTHON_STANDALONE_VERSION}"
)

# NumPy version (optional pin for reproducibility)
set(NUMPY_VERSION "" CACHE STRING "Specific NumPy version to install (empty for latest)")

# =============================================================================
# bundle_python_macos()
# =============================================================================
#
# Main function to set up Python bundling for macOS app bundles.
#
# Arguments:
#   TARGET          - The application target name (e.g., FlySightViewer)
#   PYTHON_VERSION  - Optional: Python version to bundle (default: 3.13)
#
function(bundle_python_macos)
    cmake_parse_arguments(
        PARSE_ARGV 0
        ARG
        ""
        "TARGET;PYTHON_VERSION"
        ""
    )

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "bundle_python_macos: TARGET argument is required")
    endif()

    # Resolve the actual output name (may differ from target name, e.g. "FlySight Viewer" vs "FlySightViewer")
    get_target_property(_output_name ${ARG_TARGET} OUTPUT_NAME)
    if(NOT _output_name)
        set(_output_name "${ARG_TARGET}")
    endif()

    # Bundle name for install destinations (relative to CMAKE_INSTALL_PREFIX)
    set(_bundle_name "${_output_name}.app")

    if(NOT ARG_PYTHON_VERSION)
        set(ARG_PYTHON_VERSION "3.13")
    endif()

    # Determine architecture
    if(CMAKE_OSX_ARCHITECTURES)
        set(_arch ${CMAKE_OSX_ARCHITECTURES})
    else()
        # Default to current system architecture
        execute_process(
            COMMAND uname -m
            OUTPUT_VARIABLE _arch
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    endif()

    # Map architecture to python-build-standalone naming
    if(_arch STREQUAL "arm64" OR _arch STREQUAL "aarch64")
        set(_pbs_arch "aarch64")
    elseif(_arch STREQUAL "x86_64" OR _arch STREQUAL "AMD64")
        set(_pbs_arch "x86_64")
    else()
        message(WARNING "bundle_python_macos: Unknown architecture '${_arch}', defaulting to x86_64")
        set(_pbs_arch "x86_64")
    endif()

    # Validate architecture against CMAKE_SYSTEM_PROCESSOR and provide diagnostic output
    message(STATUS "BundlePythonMacOS: System processor: ${CMAKE_SYSTEM_PROCESSOR}")
    message(STATUS "BundlePythonMacOS: OSX architectures: ${CMAKE_OSX_ARCHITECTURES}")
    message(STATUS "BundlePythonMacOS: Selected PBS architecture: ${_pbs_arch}")

    # Warn if cross-compiling and architecture might not match
    if(CMAKE_CROSSCOMPILING)
        message(WARNING "Cross-compiling detected. Ensure Python architecture (${_pbs_arch}) matches target.")
    endif()

    # Construct download URL
    # Format: cpython-X.Y.Z+YYYYMMDD-ARCH-apple-darwin-install_only.tar.gz
    set(_python_archive_name
        "cpython-${PYTHON_STANDALONE_PYTHON_VERSION}+${PYTHON_STANDALONE_VERSION}-${_pbs_arch}-apple-darwin-install_only.tar.gz"
    )
    set(_python_download_url "${PYTHON_STANDALONE_BASE_URL}/${_python_archive_name}")

    # =======================================================================
    # Pre-download URL validation
    # =======================================================================
    # Validate Python version is plausibly available in python-build-standalone
    if(NOT PYTHON_STANDALONE_PYTHON_VERSION MATCHES "^3\\.(1[0-9])\\.[0-9]+$")
        message(WARNING
            "Python version ${PYTHON_STANDALONE_PYTHON_VERSION} may not be available in python-build-standalone.\n"
            "Supported versions are typically 3.10.x through 3.14.x.\n"
            "Check https://github.com/astral-sh/python-build-standalone/releases for available versions."
        )
    endif()

    # Validate architecture
    if(NOT _pbs_arch MATCHES "^(x86_64|aarch64)$")
        message(FATAL_ERROR
            "Unsupported architecture '${_pbs_arch}' for python-build-standalone.\n"
            "Supported architectures: x86_64, aarch64"
        )
    endif()

    # Note: PBS_RELEASE_TAG must correspond to a release that includes the requested Python version
    # If the download fails with 404, check:
    # 1. The release tag exists: https://github.com/astral-sh/python-build-standalone/releases
    # 2. The Python version is included in that release
    # 3. The architecture is supported for that Python version

    # Download directory in the build tree
    set(_download_dir "${CMAKE_BINARY_DIR}/_python_standalone")
    set(_archive_path "${_download_dir}/${_python_archive_name}")
    set(_extract_dir "${_download_dir}/python")

    message(STATUS "")
    message(STATUS "----------------------------------------")
    message(STATUS "Python Bundling Configuration (macOS)")
    message(STATUS "----------------------------------------")
    message(STATUS "  Target:       ${ARG_TARGET}")
    message(STATUS "  Architecture: ${_pbs_arch}")
    message(STATUS "  Python:       ${PYTHON_STANDALONE_PYTHON_VERSION}")
    message(STATUS "  Download URL: ${_python_download_url}")
    message(STATUS "----------------------------------------")
    message(STATUS "")

    # Download python-build-standalone at configure time
    if(NOT EXISTS "${_extract_dir}/bin/python3")
        message(STATUS "Downloading python-build-standalone...")

        # Create download directory
        file(MAKE_DIRECTORY "${_download_dir}")

        # Download archive with retry logic
        if(NOT EXISTS "${_archive_path}")
            _download_with_retry("${_python_download_url}" "${_archive_path}" 3)
        endif()

        # Extract archive
        message(STATUS "Extracting python-build-standalone...")
        file(ARCHIVE_EXTRACT
            INPUT "${_archive_path}"
            DESTINATION "${_download_dir}"
        )

        # Verify extraction
        if(NOT EXISTS "${_extract_dir}/bin/python3")
            message(FATAL_ERROR "Failed to extract python-build-standalone - python3 not found")
        endif()

        message(STATUS "python-build-standalone ready at: ${_extract_dir}")
    else()
        message(STATUS "Using cached python-build-standalone at: ${_extract_dir}")
    endif()

    # Get major.minor version for library naming
    string(REGEX MATCH "^([0-9]+)\\.([0-9]+)" _version_match "${PYTHON_STANDALONE_PYTHON_VERSION}")
    set(_py_major_minor "${CMAKE_MATCH_1}${CMAKE_MATCH_2}")

    # =======================================================================
    # Install Python runtime to app bundle
    # =======================================================================

    # Install Python standard library and runtime to Contents/Resources/python/
    install(
        DIRECTORY "${_extract_dir}/"
        DESTINATION "${_bundle_name}/Contents/Resources/python"

        USE_SOURCE_PERMISSIONS
        PATTERN "*.pyc" EXCLUDE
        PATTERN "__pycache__" EXCLUDE
        PATTERN "test" EXCLUDE
        PATTERN "tests" EXCLUDE
        PATTERN "idle_test" EXCLUDE
        PATTERN "tkinter" EXCLUDE
        PATTERN "turtledemo" EXCLUDE
    )

    # Copy libpython dylib to Contents/Frameworks/
    # The dylib is typically at lib/libpython3.XX.dylib
    install(
        FILES "${_extract_dir}/lib/libpython${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.dylib"
        DESTINATION "${_bundle_name}/Contents/Frameworks"

    )

    # Also copy the symlink (libpython3.dylib -> libpython3.XX.dylib)
    if(EXISTS "${_extract_dir}/lib/libpython3.dylib")
        install(
            FILES "${_extract_dir}/lib/libpython3.dylib"
            DESTINATION "${_bundle_name}/Contents/Frameworks"
    
        )
    endif()

    # =======================================================================
    # Fix libpython dylib paths at install time
    # =======================================================================
    # The bundled libpython needs its library ID set to @rpath/libpython3.XX.dylib
    # so that other binaries can find it using RPATH references.

    set(_fix_libpython_script "${CMAKE_BINARY_DIR}/fix_libpython_macos.cmake")
    file(WRITE "${_fix_libpython_script}" "
# Fix libpython library ID for app bundle
set(BUNDLE_DIR \"\${CMAKE_INSTALL_PREFIX}/${_bundle_name}\")
set(FRAMEWORKS_DIR \"\${BUNDLE_DIR}/Contents/Frameworks\")

message(STATUS \"Fixing libpython dylib paths...\")

# Find all libpython dylibs (real files, not symlinks)
file(GLOB PYTHON_DYLIBS \"\${FRAMEWORKS_DIR}/libpython*.dylib\")

foreach(DYLIB \${PYTHON_DYLIBS})
    # Skip symlinks
    if(IS_SYMLINK \"\${DYLIB}\")
        continue()
    endif()

    get_filename_component(DYLIB_NAME \"\${DYLIB}\" NAME)
    message(STATUS \"  Processing: \${DYLIB_NAME}\")

    # Note: Do NOT strip the code signature before install_name_tool.
    # On arm64, codesign --remove-signature can corrupt __LINKEDIT.
    # install_name_tool works on signed binaries (warns but succeeds).

    # Set library ID to @rpath/libpython3.XX.dylib
    execute_process(
        COMMAND install_name_tool -id \"@rpath/\${DYLIB_NAME}\" \"\${DYLIB}\"
        RESULT_VARIABLE _id_result
        ERROR_VARIABLE _id_error
    )
    if(_id_result AND NOT _id_result EQUAL 0)
        message(STATUS \"    Note: Could not set id: \${_id_error}\")
    endif()

    # Verify the change
    execute_process(
        COMMAND otool -D \"\${DYLIB}\"
        OUTPUT_VARIABLE _new_id
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    # Extract just the ID line (second line of output)
    string(REGEX REPLACE \"^[^\\n]*\\n\" \"\" _new_id \"\${_new_id}\")
    message(STATUS \"    New ID: \${_new_id}\")

    # Fix any dependencies that might be absolute paths
    execute_process(
        COMMAND otool -L \"\${DYLIB}\"
        OUTPUT_VARIABLE _deps
        ERROR_QUIET
    )

    string(REGEX MATCHALL \"[^\\n\\t ]+\\\\.dylib\" DEP_LIST \"\${_deps}\")

    foreach(DEP \${DEP_LIST})
        # Skip system libraries and already-correct references
        if(DEP MATCHES \"^/usr/lib\" OR DEP MATCHES \"^/System/Library\" OR DEP MATCHES \"^@\")
            continue()
        endif()

        get_filename_component(DEP_NAME \"\${DEP}\" NAME)

        if(EXISTS \"\${FRAMEWORKS_DIR}/\${DEP_NAME}\")
            execute_process(
                COMMAND install_name_tool -change \"\${DEP}\" \"@rpath/\${DEP_NAME}\" \"\${DYLIB}\"
                ERROR_QUIET
            )
            message(STATUS \"    Fixed dep: \${DEP_NAME}\")
        endif()
    endforeach()

    # Ad-hoc re-sign after all path modifications (required on macOS 12+)
    execute_process(
        COMMAND codesign --force --sign - \"\${DYLIB}\"
        ERROR_QUIET
    )
endforeach()

message(STATUS \"libpython fixing complete\")
")
    install(SCRIPT "${_fix_libpython_script}")

    # =======================================================================
    # Install NumPy using pip at install time
    # =======================================================================

    # Determine NumPy package spec
    if(NUMPY_VERSION)
        set(_numpy_spec "numpy==${NUMPY_VERSION}")
    else()
        set(_numpy_spec "numpy")
    endif()

    # Create a script that installs NumPy into the bundled Python
    set(_install_numpy_script "${CMAKE_BINARY_DIR}/install_numpy_macos.cmake")
    file(WRITE "${_install_numpy_script}" "
# Install NumPy into bundled Python site-packages
set(BUNDLE_PYTHON_DIR \"\${CMAKE_INSTALL_PREFIX}/${_bundle_name}/Contents/Resources/python\")
set(PYTHON_EXE \"\${BUNDLE_PYTHON_DIR}/bin/python3\")
set(SITE_PACKAGES \"\${BUNDLE_PYTHON_DIR}/lib/python${CMAKE_MATCH_1}.${CMAKE_MATCH_2}/site-packages\")
set(NUMPY_SPEC \"${_numpy_spec}\")

message(STATUS \"Installing NumPy to bundled Python...\")
message(STATUS \"  Python: \${PYTHON_EXE}\")
message(STATUS \"  Site-packages: \${SITE_PACKAGES}\")
message(STATUS \"  NumPy spec: \${NUMPY_SPEC}\")

# Verify pip is available
execute_process(
    COMMAND \"\${PYTHON_EXE}\" -m pip --version
    RESULT_VARIABLE _pip_check_result
    OUTPUT_VARIABLE _pip_version
    ERROR_QUIET
)
if(_pip_check_result)
    message(STATUS \"pip not available, attempting to bootstrap...\")
    # Try ensurepip first (available in python-build-standalone)
    execute_process(
        COMMAND \"\${PYTHON_EXE}\" -m ensurepip --upgrade
        RESULT_VARIABLE _ensurepip_result
        OUTPUT_VARIABLE _ensurepip_output
        ERROR_VARIABLE _ensurepip_error
    )
    if(_ensurepip_result)
        message(WARNING \"ensurepip failed: \${_ensurepip_error}\")
    endif()
else()
    string(STRIP \"\${_pip_version}\" _pip_version)
    message(STATUS \"pip available: \${_pip_version}\")
endif()

execute_process(
    COMMAND \"\${PYTHON_EXE}\" -m pip install --upgrade pip
    RESULT_VARIABLE _pip_upgrade_result
)

execute_process(
    COMMAND \"\${PYTHON_EXE}\" -m pip install \${NUMPY_SPEC} --target \"\${SITE_PACKAGES}\"
    RESULT_VARIABLE _numpy_result
    OUTPUT_VARIABLE _numpy_output
    ERROR_VARIABLE _numpy_error
)
if(_numpy_result)
    message(WARNING \"NumPy installation failed: \${_numpy_error}\")
else()
    message(STATUS \"NumPy installed successfully\")
endif()
")
    install(SCRIPT "${_install_numpy_script}")

    # =======================================================================
    # Install Python SDK files to site-packages
    # =======================================================================

    # flysight_plugin_sdk.py from plugins directory
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/flysight_plugin_sdk.py")
        install(
            FILES "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/flysight_plugin_sdk.py"
            DESTINATION "${_bundle_name}/Contents/Resources/python/lib/python${CMAKE_MATCH_1}.${CMAKE_MATCH_2}/site-packages"
    
        )
    endif()

    # python_src/ directory contents if it exists
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../python_src")
        install(
            DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../python_src/"
            DESTINATION "${_bundle_name}/Contents/Resources/python/lib/python${CMAKE_MATCH_1}.${CMAKE_MATCH_2}/site-packages"
    
            PATTERN "*.pyc" EXCLUDE
            PATTERN "__pycache__" EXCLUDE
        )
    endif()

    # =======================================================================
    # Verify bundled Python installation
    # =======================================================================
    set(_verify_python_script "${CMAKE_BINARY_DIR}/verify_python_macos.cmake")
    file(WRITE "${_verify_python_script}" "
message(STATUS \"Verifying bundled Python installation...\")
set(PYTHON_EXE \"\${CMAKE_INSTALL_PREFIX}/${_bundle_name}/Contents/Resources/python/bin/python3\")

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
    install(SCRIPT "${_verify_python_script}")

    # =======================================================================
    # Store Python paths for other modules to use
    # =======================================================================

    set(BUNDLED_PYTHON_HOME
        "${_bundle_name}/Contents/Resources/python"
        CACHE INTERNAL "Path to bundled Python home in app bundle"
    )
    set(BUNDLED_PYTHON_LIBDIR
        "${_bundle_name}/Contents/Frameworks"
        CACHE INTERNAL "Path to bundled Python library in app bundle"
    )
    set(BUNDLED_PYTHON_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}"
        CACHE INTERNAL "Bundled Python major.minor version"
    )

endfunction()

# =============================================================================
# Convenience macro to check if we should bundle Python
# =============================================================================
macro(should_bundle_python_macos result_var)
    if(APPLE AND CMAKE_INSTALL_PREFIX)
        set(${result_var} TRUE)
    else()
        set(${result_var} FALSE)
    endif()
endmacro()
