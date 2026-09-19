# =============================================================================
# FlySight Viewer test suite
# =============================================================================
#
# Not a standalone project: this directory is added by src/CMakeLists.txt when
# FLYSIGHT_BUILD_TESTS=ON, so every target of the application project
# (flysight_core, flysight_model, pybind11::embed, flysight_cpp_bridge, ...) is
# in scope. Application sources are never compiled into a test; tests link
# flysight_core through flysight_test_support.
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

# Application source directory, for later phases that must compile a single
# application source (e.g. pluginhost.cpp) directly into a test.
set(FLYSIGHT_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../src")

# ─────────────────────────────── shared test support
add_library(flysight_test_support STATIC
  support/testenvironment.cpp  support/testenvironment.h
  support/fixturebuilder.cpp   support/fixturebuilder.h
  support/fakesessionstate.cpp support/fakesessionstate.h
  support/builtinfixture.cpp   support/builtinfixture.h
  support/testmain.h
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
