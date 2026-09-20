# =============================================================================
# FlySight Viewer test suite
# =============================================================================
#
# Not a standalone project: this directory is added by src/CMakeLists.txt when
# FLYSIGHT_BUILD_TESTS=ON, so every target of the application project
# (flysight_core, flysight_model, pybind11::embed, flysight_cpp_bridge, ...) is
# in scope. Tests link flysight_core through flysight_test_support rather than
# compiling application sources. The one exception is tst_python_bridge, which
# compiles pluginhost.cpp and pluginadapters.cpp directly: they belong to the
# application executable, not to flysight_core, because they need the embedded
# interpreter.
#
# Conventions:
#   - one QObject test class per executable
#   - file and target are named tst_<area>.cpp / tst_<area>
#   - every test is registered through flysight_add_test()
#   - tests have no install rules and keep their default output directory
#
# See tests/README.md for how to configure, build, and run.
# =============================================================================

if(NOT TARGET flysight_core)
  message(FATAL_ERROR "tests/ must be added from src/CMakeLists.txt with -DFLYSIGHT_BUILD_TESTS=ON")
endif()

find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

# Application source directory, for the test that must compile application
# sources directly (tst_python_bridge: pluginhost.cpp, pluginadapters.cpp).
set(FLYSIGHT_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../src")

# ─────────────────────────────── shared test support
add_library(flysight_test_support STATIC
  support/testenvironment.cpp  support/testenvironment.h
  support/fixturebuilder.cpp   support/fixturebuilder.h
  support/fakesessionstate.cpp support/fakesessionstate.h
  support/builtinfixture.cpp   support/builtinfixture.h
  support/oraclecatalogue.cpp  support/oraclecatalogue.h
  support/logbookprobe.cpp     support/logbookprobe.h
  support/testmain.h           support/testutil.h
)
target_include_directories(flysight_test_support PUBLIC "${CMAKE_CURRENT_LIST_DIR}/support")
target_link_libraries(flysight_test_support
  PUBLIC flysight_core Qt${QT_VERSION_MAJOR}::Test)

# ─────────────────────────────── helper
# flysight_add_test(<name> SOURCES <files...> [LIBS <targets...>] [ENVIRONMENT <VAR=value...>])
#
# Creates the executable, links the shared support library (and LIBS),
# registers it with CTest, and arranges for the DLLs it needs to be found on
# Windows without copying or deploying anything next to it.
function(flysight_add_test name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;ENVIRONMENT" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "flysight_add_test(${name}): SOURCES is required")
  endif()
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "flysight_add_test(${name}): unknown arguments: ${ARG_UNPARSED_ARGUMENTS}")
  endif()

  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE flysight_test_support ${ARG_LIBS})

  add_test(NAME ${name} COMMAND ${name})

  # offscreen is exported even though the tests run as QCoreApplication, so a
  # later test can switch its own main() to QGuiApplication without harness changes.
  #
  # QT_FORCE_STDERR_LOGGING: on Windows, Qt Test sends its plain-text log to
  # the debugger (OutputDebugString) instead of the standard streams whenever
  # no console is attached, which is always the case under CTest. Without this
  # `ctest --output-on-failure` would show nothing for a failing test.
  set(_env "QT_QPA_PLATFORM=offscreen" "QT_FORCE_STDERR_LOGGING=1")
  if(ARG_ENVIRONMENT)
    list(APPEND _env ${ARG_ENVIRONMENT})
  endif()
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT "${_env}"
    TIMEOUT 120
    LABELS core
  )

  if(WIN32)
    # Qt and GeographicLib are DLLs and nothing is deployed next to the test
    # executables (deployment happens only at install, for the application).
    # TARGET_FILE_DIR of Qt Core is Qt's bin directory for Debug and Release alike.
    set(_dll_dirs "$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>" "${GEOGRAPHIC_ROOT}/bin")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
      foreach(_d IN LISTS _dll_dirs)
        set_property(TEST ${name} APPEND PROPERTY
          ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_d}")
      endforeach()
    else()
      message(WARNING "CMake < 3.22: add Qt's bin and GeographicLib-install/bin to PATH before running ctest")
    endif()
  endif()
endfunction()

# ─────────────────────────────── tests
flysight_add_test(tst_harness SOURCES tst_harness.cpp)
flysight_add_test(tst_smoke   SOURCES tst_smoke.cpp)

# Calculation engine (synthetic calculations against a fake session state)
flysight_add_test(tst_calcregistry      SOURCES tst_calcregistry.cpp)
flysight_add_test(tst_calcengine        SOURCES tst_calcengine.cpp)
flysight_add_test(tst_calcengine_safety SOURCES tst_calcengine_safety.cpp)
flysight_add_test(tst_calcengine_oracle SOURCES tst_calcengine_oracle.cpp)

# Built-in calculations (golden values on the generated descent fixture)
flysight_add_test(tst_builtins_golden SOURCES tst_builtins_golden.cpp)

# Built-in calculations on the engine (fake session state, private registry)
flysight_add_test(tst_builtins_engine SOURCES tst_builtins_engine.cpp)

# SessionData / SessionModel on the engine (real built-ins, global registry)
flysight_add_test(tst_session_engine       SOURCES tst_session_engine.cpp)
flysight_add_test(tst_session_model_engine SOURCES tst_session_model_engine.cpp)

# Source layer and conversion layer: the schema and unit tables, the conversion
# families on a private registry, the importer, and the session-level suite
flysight_add_test(tst_schema_units      SOURCES tst_schema_units.cpp)
flysight_add_test(tst_conversion_engine SOURCES tst_conversion_engine.cpp)
flysight_add_test(tst_importer          SOURCES tst_importer.cpp)
flysight_add_test(tst_source_layer      SOURCES tst_source_layer.cpp)

# Persistence: the on-disk text forms, the exact save / reload round trip, the
# marker-gated index.json column cache, and the model's per-column refresh
flysight_add_test(tst_csvformat             SOURCES tst_csvformat.cpp)
flysight_add_test(tst_persistence_roundtrip SOURCES tst_persistence_roundtrip.cpp)
flysight_add_test(tst_logbook_index         SOURCES tst_logbook_index.cpp)
flysight_add_test(tst_column_cache          SOURCES tst_column_cache.cpp)

# Import and merge rules: the pure merge plan, the model's import path against
# a temporary logbook, and the widget-free import driver / failure text
flysight_add_test(tst_session_merge SOURCES tst_session_merge.cpp)
flysight_add_test(tst_import_merge  SOURCES tst_import_merge.cpp)
flysight_add_test(tst_import_batch  SOURCES tst_import_batch.cpp)

# Session-level idempotency oracle: randomized reads, edits, merges, preference
# and registry changes on real sessions against a fresh evaluation.
# QT_HASH_SEED=0 makes QSet / QHash iteration order reproducible, so a failure
# that depends on invalidation order reproduces from its seed. (FLYSIGHT_TEST_MAIN
# also fixes the seed in every test executable, so a run by hand reproduces too;
# the variable is kept so that the CTest run does not depend on that.)
flysight_add_test(tst_session_oracle SOURCES tst_session_oracle.cpp ENVIRONMENT "QT_HASH_SEED=0")
set_tests_properties(tst_session_oracle PROPERTIES TIMEOUT 300 LABELS "core;oracle")

# End-to-end workflows on the application's import path (acceptance 19)
flysight_add_test(tst_workflow SOURCES tst_workflow.cpp)

# ---- embedded-Python bridge test -----------------------------------------
# tst_python_bridge boots the real embedded interpreter, imports the real
# flysight_cpp_bridge module from the build tree, and loads the SDK, the test
# plugins under tests/python_plugins/ and the bundled example. It is the only
# target that links pybind11::embed / Python.
#
# Environment (all supplied by CTest, nothing is taken from the user's PATH or
# profile; see tests/README.md for running the executable by hand):
#   PYTHONHOME  = sys.base_prefix of the build-time interpreter (stdlib)
#   PYTHONPATH  = <dir of the built flysight_cpp_bridge> ; <site dir holding NumPy>
#   PATH (Win)  += the directory of pythonXY.dll (plus Qt / GeographicLib, as for every test)
#   PYTHONDONTWRITEBYTECODE=1 so nothing is written outside the test's temp dir
# A test executable has no python/ sibling, so PluginHost takes its "system
# Python" branch, which honours these variables. No test seam exists in PluginHost.
option(FLYSIGHT_BUILD_PYTHON_TESTS "Build and run the embedded-Python plugin bridge test" ON)
if(FLYSIGHT_BUILD_PYTHON_TESTS)
  # Build-time interpreter facts (Python_EXECUTABLE comes from src/CMakeLists.txt find_package(Python))
  execute_process(COMMAND "${Python_EXECUTABLE}" -c "import sys; print(sys.base_prefix)"
                  OUTPUT_VARIABLE _fs_py_home OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND "${Python_EXECUTABLE}" -c
                  "import numpy, os; print(os.path.dirname(os.path.dirname(numpy.__file__)))"
                  RESULT_VARIABLE _fs_numpy_rc OUTPUT_VARIABLE _fs_numpy_site
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  file(TO_CMAKE_PATH "${_fs_py_home}" _fs_py_home)
  file(TO_CMAKE_PATH "${_fs_numpy_site}" _fs_numpy_site)

  flysight_add_test(tst_python_bridge
    SOURCES tst_python_bridge.cpp
            "${FLYSIGHT_SRC_DIR}/pluginhost.cpp"
            "${FLYSIGHT_SRC_DIR}/pluginadapters.cpp"
    LIBS pybind11::embed
    ENVIRONMENT "PYTHONHOME=${_fs_py_home}" "PYTHONDONTWRITEBYTECODE=1")
  add_dependencies(tst_python_bridge flysight_cpp_bridge)
  flysight_msvc_fix_python_debug_autolink(tst_python_bridge)
  target_compile_definitions(tst_python_bridge PRIVATE
    FLYSIGHT_PYTHON_PLUGINS_DIR="${CMAKE_CURRENT_LIST_DIR}/../python_plugins"
    FLYSIGHT_TEST_PLUGINS_DIR="${CMAKE_CURRENT_LIST_DIR}/python_plugins"
    FLYSIGHT_BRIDGE_DIR="$<TARGET_FILE_DIR:flysight_cpp_bridge>")
  set_tests_properties(tst_python_bridge PROPERTIES LABELS "core;python" TIMEOUT 180)

  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
    # ENVIRONMENT_MODIFICATION, not ENVIRONMENT: the Windows separator ';' is
    # also CMake's list separator.
    set_property(TEST tst_python_bridge APPEND PROPERTY ENVIRONMENT_MODIFICATION
      "PYTHONPATH=set:$<TARGET_FILE_DIR:flysight_cpp_bridge>"
      "PYTHONPATH=path_list_append:${_fs_numpy_site}")
    if(WIN32)
      foreach(_d IN LISTS Python_RUNTIME_LIBRARY_DIRS)
        set_property(TEST tst_python_bridge APPEND PROPERTY ENVIRONMENT_MODIFICATION
          "PATH=path_list_prepend:${_d}")
      endforeach()
    endif()
  endif()

  if(APPLE)
    # Unverified (needs a CI run): let the test executable find libpython.
    set_target_properties(tst_python_bridge PROPERTIES BUILD_RPATH "${Python_LIBRARY_DIRS}")
  endif()

  if(NOT _fs_numpy_rc EQUAL 0 OR CMAKE_VERSION VERSION_LESS 3.22)
    message(WARNING "tst_python_bridge is DISABLED: NumPy is not importable by ${Python_EXECUTABLE} "
                    "(python -m pip install numpy) or CMake < 3.22.")
    set_tests_properties(tst_python_bridge PROPERTIES DISABLED TRUE)
  endif()
endif()

# ---- cleanup audit ---------------------------------------------------------
# "No old mechanism remains", "one authority per fact", and the acceptance
# traceability map (tests/acceptance_map.txt) as one repeatable command. A
# CMake script over `git grep`; registered only when this is a git checkout.
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../.git")
  add_test(NAME audit_cleanup COMMAND "${CMAKE_COMMAND}"
           "-DREPO=${CMAKE_CURRENT_LIST_DIR}/.." "-DGIT=${GIT_EXECUTABLE}"
           -P "${CMAKE_CURRENT_LIST_DIR}/audit/cleanup_audit.cmake")
  set_tests_properties(audit_cleanup PROPERTIES LABELS "audit" TIMEOUT 120)
endif()
