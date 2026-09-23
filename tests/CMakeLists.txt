# =============================================================================
# FlySight Viewer test suite
# =============================================================================
#
# Not a standalone project: this directory is added by src/CMakeLists.txt when
# FLYSIGHT_BUILD_TESTS=ON, so every target of the application project
# (flysight_core, flysight_model, pybind11::embed, flysight_cpp_bridge, ...) is
# in scope. Tests link flysight_core through flysight_test_support rather than
# compiling application sources, with three exceptions, each of which compiles
# sources that belong to the application executable and not to flysight_core:
#   - tst_python_bridge: pluginhost.cpp and pluginadapters.cpp, which need the
#     embedded interpreter. The only test that links Python
#     (FLYSIGHT_BUILD_PYTHON_TESTS).
#   - tst_map_models: the two map models and their two helpers, which need Qt
#     Core and Gui only, so the test needs no Widgets, WebEngine or Quick.
#   - tst_plot_row_delegate: the plot list's row delegate. The only test that
#     links Qt Widgets (FLYSIGHT_BUILD_WIDGET_TESTS).
# The FLYSIGHT_BUILD_FUSION_TESTS block near the end of this file is the only
# place a test may link GTSAM.
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
# The engine library creates no thread; support/asyncdriver.h, which drives
# PreparedCalculation::compute() on a std::thread, does.
find_package(Threads REQUIRED)

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
  support/asyncdriver.h
  support/jobfixture.cpp      support/jobfixture.h
  support/plotfixture.cpp     support/plotfixture.h
)
target_include_directories(flysight_test_support PUBLIC "${CMAKE_CURRENT_LIST_DIR}/support")
# Threads::Threads is a usage requirement only: asyncdriver.h is header-only, so
# it is the including test that needs the thread library, not this archive.
# Stated once here, no test names it.
target_link_libraries(flysight_test_support
  PUBLIC flysight_core Qt${QT_VERSION_MAJOR}::Test
  INTERFACE Threads::Threads)

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
flysight_add_test(tst_calcengine_async  SOURCES tst_calcengine_async.cpp)
flysight_add_test(tst_calcengine_blockers SOURCES tst_calcengine_blockers.cpp)
flysight_add_test(tst_calcengine_restore SOURCES tst_calcengine_restore.cpp)

# Built-in calculations (golden values on the generated descent fixture)
flysight_add_test(tst_builtins_golden SOURCES tst_builtins_golden.cpp)

# Built-in calculations on the engine (fake session state, private registry)
flysight_add_test(tst_builtins_engine SOURCES tst_builtins_engine.cpp)
flysight_add_test(tst_time_fit SOURCES tst_time_fit.cpp)
flysight_add_test(tst_local_coordinates SOURCES tst_local_coordinates.cpp)
flysight_add_test(tst_simplified_track SOURCES tst_simplified_track.cpp)

# SessionData / SessionModel on the engine (real built-ins, global registry), and the stored results of explicit calculations
flysight_add_test(tst_session_engine       SOURCES tst_session_engine.cpp)
flysight_add_test(tst_session_model_engine SOURCES tst_session_model_engine.cpp)
flysight_add_test(tst_result_store SOURCES tst_result_store.cpp)

# Job queue and job model (synthetic explicit calculations; no GTSAM)
flysight_add_test(tst_jobqueue SOURCES tst_jobqueue.cpp)
flysight_add_test(tst_jobmodel SOURCES tst_jobmodel.cpp)

# Plot request logic (row state, gestures, continuation, cancel, pruning; no widgets)
flysight_add_test(tst_plot_requests SOURCES tst_plot_requests.cpp)

# Plot-list rows: pure geometry (no widgets) ...
flysight_add_test(tst_plot_row_layout SOURCES tst_plot_row_layout.cpp)

# ... and the delegate in an offscreen QTreeView. The only test that links
# Qt Widgets; it compiles the delegate from the application sources
# (pattern: tst_python_bridge). QT_QPA_PLATFORM=offscreen is already set by
# flysight_add_test().
option(FLYSIGHT_BUILD_WIDGET_TESTS "Build and run the tests that need Qt Widgets" ON)
if(FLYSIGHT_BUILD_WIDGET_TESTS)
  flysight_add_test(tst_plot_row_delegate
    SOURCES tst_plot_row_delegate.cpp
            "${FLYSIGHT_SRC_DIR}/ui/docks/plotselection/PlotRowDelegate.cpp"
            "${FLYSIGHT_SRC_DIR}/ui/docks/plotselection/PlotRowDelegate.h"
    LIBS Qt${QT_VERSION_MAJOR}::Widgets)
  set_tests_properties(tst_plot_row_delegate PROPERTIES LABELS "core;widgets")
endif()

# Source layer and conversion layer: the schema and unit tables, the conversion
# families on a private registry, the importer, and the session-level suite
flysight_add_test(tst_schema_units      SOURCES tst_schema_units.cpp)
flysight_add_test(tst_conversion_engine SOURCES tst_conversion_engine.cpp)
flysight_add_test(tst_importer          SOURCES tst_importer.cpp)
flysight_add_test(tst_source_layer      SOURCES tst_source_layer.cpp)

# Persistence: the on-disk text forms, the exact save / reload round trip, the
# marker-gated index.json column cache, the model's per-column refresh, and the
# stored calculation records
flysight_add_test(tst_csvformat             SOURCES tst_csvformat.cpp)
flysight_add_test(tst_persistence_roundtrip SOURCES tst_persistence_roundtrip.cpp)
flysight_add_test(tst_logbook_index         SOURCES tst_logbook_index.cpp)
flysight_add_test(tst_column_cache          SOURCES tst_column_cache.cpp)
flysight_add_test(tst_result_records        SOURCES tst_result_records.cpp)

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

# Map models on the simplified track (no track without a local origin). The
# two models and their two helpers are application sources that need Qt Core
# and Gui only, so they are compiled into the test; nothing here needs
# Widgets, WebEngine or Quick.
flysight_add_test(tst_map_models
  SOURCES tst_map_models.cpp
          "${FLYSIGHT_SRC_DIR}/ui/docks/map/TrackMapModel.cpp"
          "${FLYSIGHT_SRC_DIR}/ui/docks/map/MapCursorDotModel.cpp"
          "${FLYSIGHT_SRC_DIR}/plotrangemodel.cpp"
          "${FLYSIGHT_SRC_DIR}/plotutils.cpp")

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

# ---- GTSAM-linked (fusion) tests -------------------------------------------
# Everything that links GTSAM is defined inside this block, so with the option
# OFF no test target references GTSAM, and `ctest -LE fusion` is the GTSAM-free
# run of a build that has them. flysight_test_support and flysight_add_test()
# know nothing about the solver.
#
# The gtsam target, flysight_solver_stack() and flysight_solver_test_environment()
# come from cmake/SolverDependencies.cmake (included by src/CMakeLists.txt).
option(FLYSIGHT_BUILD_FUSION_TESTS "Build and run the GTSAM-linked tests" ON)
if(FLYSIGHT_BUILD_FUSION_TESTS)
  # flysight_add_fusion_test(<name> SOURCES <files...> [LIBS <targets...>] [ENVIRONMENT <VAR=value...>])
  #
  # flysight_add_test() plus what a test that runs the solver needs: a 64 MiB
  # main-thread stack, the solver libraries on the library search path, the
  # "fusion" label, a longer timeout, and /bigobj for GTSAM's and Eigen's
  # templates. It does not link GTSAM itself: the caller names what it links
  # in LIBS (gtsam, or a library that carries it).
  function(flysight_add_fusion_test name)
    flysight_add_test(${name} ${ARGN})
    flysight_solver_stack(${name})
    flysight_solver_test_environment(${name})
    set_tests_properties(${name} PROPERTIES LABELS "core;fusion" TIMEOUT 600)
    if(MSVC)
      target_compile_options(${name} PRIVATE /bigobj)
    endif()
  endfunction()

  # Solver dependencies: the exported target compiles, links, loads and solves
  flysight_add_fusion_test(tst_solver_smoke
    SOURCES tst_solver_smoke.cpp solverprobe.h
    LIBS gtsam)

  # Fusion kernel: golden regression against the product kernel (tests/README.md section 11)
  add_library(flysight_fusion_test_support STATIC
    fusion/fusionfixtures.cpp fusion/fusionfixtures.h
    fusion/fusiongolden.cpp   fusion/fusiongolden.h)
  target_include_directories(flysight_fusion_test_support PUBLIC "${CMAKE_CURRENT_LIST_DIR}/fusion")
  target_link_libraries(flysight_fusion_test_support PUBLIC flysight_fusion Qt${QT_VERSION_MAJOR}::Core)
  target_compile_definitions(flysight_fusion_test_support PUBLIC
    FLYSIGHT_FUSION_GOLDEN_DIR="${CMAKE_CURRENT_LIST_DIR}/data/fusion")
  if(NOT MSVC)
    target_compile_options(flysight_fusion_test_support PRIVATE -ffp-contract=off)  # fixture bits
  endif()
  flysight_add_fusion_test(tst_fusion_golden SOURCES tst_fusion_golden.cpp LIBS flysight_fusion_test_support)
  # fusion/fusiontrace.h is header-only (it reaches gtsam::Rot3) and is listed
  # for IDE visibility in the two targets that link gtsam and include it.
  flysight_add_fusion_test(tst_fusion_kernel SOURCES tst_fusion_kernel.cpp fusion/fusiontrace.h
    LIBS flysight_fusion_test_support gtsam)

  # Fusion as a registered calculation: sessions, engine, job queue (tests/README.md)
  add_library(flysight_fusion_session_support STATIC
    fusion/fusionsessions.cpp fusion/fusionsessions.h)
  target_link_libraries(flysight_fusion_session_support
    PUBLIC flysight_fusion_test_support flysight_test_support)
  flysight_add_fusion_test(tst_fusion_session SOURCES tst_fusion_session.cpp
    LIBS flysight_fusion_session_support)
  flysight_add_fusion_test(tst_fusion_jobs SOURCES tst_fusion_jobs.cpp
    LIBS flysight_fusion_session_support)

  # The plot-row script with the real fusion plots (acceptance 15, end to end)
  flysight_add_fusion_test(tst_fusion_rows SOURCES tst_fusion_rows.cpp
    LIBS flysight_fusion_session_support)

  # Stored fusion results: fit, unload / restart, restore (store-requested-calculations)
  flysight_add_fusion_test(tst_fusion_store SOURCES tst_fusion_store.cpp
    LIBS flysight_fusion_session_support)

  # The runner, driven as a child process on fixtures written out as CSV
  flysight_add_fusion_test(tst_fusion_runner SOURCES tst_fusion_runner.cpp
    LIBS flysight_fusion_session_support)
  target_compile_definitions(tst_fusion_runner PRIVATE
    FLYSIGHT_FUSION_RUNNER="$<TARGET_FILE:fusion_runner>")
  add_dependencies(tst_fusion_runner fusion_runner)   # $<TARGET_FILE> creates no build order

  # ---- bit-exact golden regression (label "exact") --------------------------
  # The same executables run a second time with FLYSIGHT_FUSION_EXACT=1, which
  # turns every golden comparison into bit equality (tests/README.md section
  # 11, "Tolerance policy"): the proof that a rebuild is bit-identical to the
  # capture. Bit identity is a fair demand only on the configuration the
  # goldens were captured with, which tests/data/fusion/capture.json records
  # (written by fusion_golden_capture): MSVC, x64, and the same compiler
  # major.minor (code generation is what decides the last bit; a Visual Studio
  # update may legitimately change it, and that is a reason to look and
  # re-capture, not a defect of the kernel) - and the Release configuration:
  # the goldens promise nothing about a Debug or RelWithDebInfo build, so the
  # exact tests exist for Release only (flysight_add_fusion_exact_test below).
  #
  #   AUTO  register the exact tests when the compiler matches capture.json,
  #         otherwise say why not in a STATUS line (never a red test for a
  #         compiler nobody validated)
  #   ON    register them whatever the compiler is
  #   OFF   never register them
  set(FLYSIGHT_FUSION_EXACT_TESTS "AUTO" CACHE STRING
      "Run the fusion golden tests a second time in bit-exact mode: AUTO (when the compiler matches tests/data/fusion/capture.json), ON or OFF")
  set_property(CACHE FLYSIGHT_FUSION_EXACT_TESTS PROPERTY STRINGS AUTO ON OFF)

  set(_fs_exact OFF)
  string(TOUPPER "${FLYSIGHT_FUSION_EXACT_TESTS}" _fs_exact_mode)
  if(NOT _fs_exact_mode MATCHES "^(AUTO|ON|OFF|TRUE|FALSE|YES|NO|1|0)$")
    message(FATAL_ERROR "FLYSIGHT_FUSION_EXACT_TESTS must be AUTO, ON or OFF, not '${FLYSIGHT_FUSION_EXACT_TESTS}'")
  endif()
  if(_fs_exact_mode STREQUAL "AUTO")
    # The captured compiler: "cl_version": "19.44.35220" -> 19.44
    file(READ "${CMAKE_CURRENT_LIST_DIR}/data/fusion/capture.json" _fs_capture)
    set(_fs_capture_cl "")
    if(_fs_capture MATCHES "\"cl_version\" *: *\"([0-9]+\\.[0-9]+)[0-9.]*\"")
      set(_fs_capture_cl "${CMAKE_MATCH_1}")
    endif()
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" _fs_this_cl "${CMAKE_CXX_COMPILER_VERSION}")
    if(_fs_capture_cl STREQUAL "")
      message(WARNING "Fusion exact tests not registered: tests/data/fusion/capture.json names no cl_version")
    elseif(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
      message(STATUS "Fusion exact tests not registered: the goldens were captured with 64-bit MSVC "
                     "${_fs_capture_cl} (capture.json); this is ${CMAKE_CXX_COMPILER_ID} "
                     "${CMAKE_CXX_COMPILER_VERSION}. Portable-tolerance parity still runs")
    elseif(NOT _fs_this_cl STREQUAL _fs_capture_cl)
      message(STATUS "Fusion exact tests not registered: compiler differs from capture.json "
                     "(MSVC ${CMAKE_CXX_COMPILER_VERSION}, goldens captured with ${_fs_capture_cl}). "
                     "Portable-tolerance parity still runs; -DFLYSIGHT_FUSION_EXACT_TESTS=ON forces them")
    else()
      set(_fs_exact ON)
    endif()
  elseif(FLYSIGHT_FUSION_EXACT_TESTS)
    set(_fs_exact ON)
  endif()

  # Release only, whatever the mode. With a multi-config generator (Visual
  # Studio) the configuration is chosen when ctest runs, so the tests are
  # restricted with CONFIGURATIONS: `ctest -C Release` runs them, `ctest -C
  # Debug` does not list them. With a single-config generator (Ninja,
  # Makefiles) CONFIGURATIONS is the wrong tool - it hides a test from a plain
  # `ctest` without -C, which is how a single-config tree is normally run - and
  # the configuration is known now: the tests are registered unrestricted when
  # CMAKE_BUILD_TYPE is Release, and not at all otherwise.
  get_property(_fs_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  set(_fs_exact_configurations "")
  if(_fs_exact AND _fs_multi_config)
    set(_fs_exact_configurations CONFIGURATIONS Release)
  elseif(_fs_exact AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "Fusion exact tests not registered: the goldens were captured from a Release build; "
                   "CMAKE_BUILD_TYPE is '${CMAKE_BUILD_TYPE}'. Portable-tolerance parity still runs")
    set(_fs_exact OFF)
  endif()

  # flysight_add_fusion_exact_test(<test>): CTest test <test>_exact, the
  # executable and whole environment of <test> plus FLYSIGHT_FUSION_EXACT=1.
  # (The 64 MiB stack is a link property of the executable.)
  function(flysight_add_fusion_exact_test name)
    add_test(NAME ${name}_exact COMMAND ${name} ${_fs_exact_configurations})
    get_test_property(${name} ENVIRONMENT _env)
    get_test_property(${name} ENVIRONMENT_MODIFICATION _modification)
    get_test_property(${name} TIMEOUT _timeout)
    list(APPEND _env "FLYSIGHT_FUSION_EXACT=1")
    set_tests_properties(${name}_exact PROPERTIES
      ENVIRONMENT "${_env}"
      TIMEOUT ${_timeout}
      LABELS "core;fusion;exact")
    if(_modification)
      set_property(TEST ${name}_exact PROPERTY ENVIRONMENT_MODIFICATION "${_modification}")
    endif()
  endfunction()

  if(_fs_exact)
    message(STATUS "Fusion exact tests registered for Release (FLYSIGHT_FUSION_EXACT_TESTS=${FLYSIGHT_FUSION_EXACT_TESTS}, "
                   "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}): ctest -C Release -L exact")
    # The two that decide bit-identity ...
    flysight_add_fusion_exact_test(tst_fusion_golden)
    flysight_add_fusion_exact_test(tst_fusion_kernel)
    # ... and, because they cost about a second each, the four that hold the
    # registered calculation, the job queue's worker thread, the plot rows and
    # the stored and restored results to the same goldens: the bits survive
    # the engine, the 64 MiB worker and the record file.
    flysight_add_fusion_exact_test(tst_fusion_session)
    flysight_add_fusion_exact_test(tst_fusion_jobs)
    flysight_add_fusion_exact_test(tst_fusion_rows)
    flysight_add_fusion_exact_test(tst_fusion_store)
    # ... and the runner, whose test compares the tool's output with an
    # in-process run through the golden comparator: on this configuration
    # that comparison is bit identity across the process boundary.
    flysight_add_fusion_exact_test(tst_fusion_runner)
  endif()

  # ---- three executables that are not tests and are not installed -----------
  # solver_deploy_probe is copied into an installed application tree and run
  # there to prove the deployed solver runtime is complete (tests/README.md
  # section 1). fusion_golden_capture runs the product kernel on the twelve
  # fixtures and writes the goldens of tests/data/fusion/ (the re-capture
  # procedure of tests/README.md section 11); it is run by hand with the DLL
  # directories on PATH, so it needs no flysight_solver_test_environment().
  add_executable(solver_deploy_probe solver_deploy_probe.cpp solverprobe.h)
  target_link_libraries(solver_deploy_probe PRIVATE gtsam)
  if(MSVC)
    target_compile_options(solver_deploy_probe PRIVATE /bigobj)
  endif()

  # The capture tool names gtsam (it includes fusion/fusionpipeline.h for the
  # trace seam and <gtsam/config.h> for the provenance), so it is listed in
  # _FLYSIGHT_GTSAM_NAMERS (cmake/SolverDependencies.cmake) and in the audit's
  # solver-confinement regex. FLYSIGHT_FUSION_GOLDEN_DIR, its default output
  # directory, comes from flysight_fusion_test_support. Fits run on the main
  # thread, hence the 64 MiB stack. The FLYSIGHT_CAPTURE_* definitions are
  # what capture.json records about the build that captured.
  add_executable(fusion_golden_capture fusion_golden_capture.cpp fusion/fusiontrace.h)
  target_link_libraries(fusion_golden_capture PRIVATE flysight_fusion_test_support gtsam)
  flysight_solver_stack(fusion_golden_capture)
  if(MSVC)
    target_compile_options(fusion_golden_capture PRIVATE /bigobj)
  endif()
  target_compile_definitions(fusion_golden_capture PRIVATE
    FLYSIGHT_CAPTURE_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
    FLYSIGHT_CAPTURE_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
    FLYSIGHT_CAPTURE_GENERATOR="${CMAKE_GENERATOR}"
    FLYSIGHT_CAPTURE_PLATFORM="${CMAKE_GENERATOR_PLATFORM}"
    FLYSIGHT_CAPTURE_CONFIGURATION="$<CONFIG>"
    FLYSIGHT_CAPTURE_CMAKE_VERSION="${CMAKE_VERSION}"
    FLYSIGHT_CAPTURE_GTSAM_DIR="${GTSAM_DIR}"
    FLYSIGHT_FUSION_FIXTURE_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}/fusion")

  # Command-line runner: the fit on one recording imported as the application
  # imports it (spec section 8). Not a test, not installed; run by hand with the
  # Qt, GeographicLib and solver library directories on the path. Links the
  # public headers of the fusion library only, never GTSAM.
  add_executable(fusion_runner fusion_runner.cpp)
  target_link_libraries(fusion_runner PRIVATE flysight_core flysight_fusion)
  flysight_solver_stack(fusion_runner)
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
