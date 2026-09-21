cmake_minimum_required(VERSION 3.18)

project(FlySightViewer VERSION 0.0.0 LANGUAGES CXX)

# Detect version from Git tags (overrides PROJECT_VERSION set above)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/GitVersion.cmake")

# ─────────────────────────────── version header
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.h"
    @ONLY
)

# ─────────────────────────────── global settings
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Install into <build>/install by default so devs don't need admin rights
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX
      "${CMAKE_BINARY_DIR}/install"
      CACHE PATH "Install path prefix." FORCE)
endif()

# =============================================================================
# Variable Definition Conventions
# =============================================================================
#
# Third-party path variables (defined once, cached, can be overridden):
#   THIRD_PARTY_DIR, QCUSTOMPLOT_ROOT, QCUSTOMPLOT_DIR,
#   GEOGRAPHIC_ROOT, KDDW_ROOT, BOOST_ROOT, BOOST_LIBRARYDIR
#   GTSAM_ROOT, ONETBB_ROOT (consumed by cmake/SolverDependencies.cmake)
#
# Build configuration variables:
#   GOOGLE_MAPS_API_KEY  - Google Maps JavaScript API key (CACHE STRING)
#   FLYSIGHT_BUILD_TESTS - Build the Qt Test suite under ../tests and register
#                          it with CTest (option, default OFF; defined next to
#                          the add_subdirectory call near the end of the
#                          target definitions)
#
# These are defined as CACHE PATH/STRING variables at the start of this file.
# Do not redefine these variables elsewhere - CMake's cache ensures
# command-line overrides work correctly.
#
# Platform-specific modules (cmake/Bundle*, cmake/Deploy*, cmake/Create*)
# may define their own CACHE variables, but should avoid redefining
# variables already defined here.
#
# When adding new third-party dependencies:
# 1. Add path variable in the "Third-party root" section below
# 2. Use the variable where needed without redefinition
# 3. Document the variable in this conventions block
#
# =============================================================================

# Third-party root (editable in Qt Creator / CMake cache)
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_LIST_DIR}/../third-party"
    CACHE PATH "Path to third-party dependencies")

set(QCUSTOMPLOT_ROOT "${THIRD_PARTY_DIR}/QCustomPlot"         CACHE PATH "QCustomPlot root")
set(QCUSTOMPLOT_DIR  "${QCUSTOMPLOT_ROOT}/QCustomPlot"        CACHE PATH "QCustomPlot sources dir")
set(GEOGRAPHIC_ROOT  "${THIRD_PARTY_DIR}/GeographicLib-install" CACHE PATH "GeographicLib install prefix")
set(KDDW_ROOT      "${THIRD_PARTY_DIR}/KDDockWidgets-install" CACHE PATH "KDDockWidgets install prefix")
set(GTSAM_ROOT     "${THIRD_PARTY_DIR}/GTSAM-install"         CACHE PATH "GTSAM install prefix")
set(ONETBB_ROOT    "${THIRD_PARTY_DIR}/oneTBB-install"        CACHE PATH "oneTBB install prefix")

# Google Maps API key for the embedded map view
set(GOOGLE_MAPS_API_KEY "" CACHE STRING "Google Maps JavaScript API key")

# Allow environment variable override for CI/CD
if(DEFINED ENV{KDDW_ROOT} AND NOT KDDW_ROOT)
    set(KDDW_ROOT "$ENV{KDDW_ROOT}")
endif()

# Make KDDW discoverable by find_package() without requiring external -DCMAKE_PREFIX_PATH=...
list(PREPEND CMAKE_PREFIX_PATH "${KDDW_ROOT}")

# KDDockWidgets CMake config location varies by platform
# Add common subdirectories to help find_package locate the config
set(_KDDW_CMAKE_PATHS
    "${KDDW_ROOT}/lib/cmake/KDDockWidgets-qt6"
    "${KDDW_ROOT}/lib64/cmake/KDDockWidgets-qt6"
    "${KDDW_ROOT}/share/cmake/KDDockWidgets-qt6"
)
foreach(_kddw_path ${_KDDW_CMAKE_PATHS})
    if(EXISTS "${_kddw_path}/KDDockWidgets-qt6Config.cmake")
        list(PREPEND CMAKE_PREFIX_PATH "${_kddw_path}/..")
        message(STATUS "Found KDDockWidgets config at: ${_kddw_path}")
        break()
    endif()
endforeach()

# Include cross-platform Boost discovery
# This sets BOOST_ROOT and BOOST_LIBRARYDIR with platform-appropriate defaults
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/BoostDiscovery.cmake")

# ─────────────────────────────── Qt
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets PrintSupport QuickWidgets Quick Qml WebEngineWidgets WebChannel Multimedia MultimediaWidgets)

if(QT_VERSION_MAJOR GREATER_EQUAL 6)
  # Nice-to-have; safe to omit if you prefer older style
  include(${Qt6_DIR}/Qt6Config.cmake OPTIONAL RESULT_VARIABLE _qt6cfg)
  if(_qt6cfg)
    qt_standard_project_setup()
  endif()
endif()

# ─────────────────────────────── Google Maps HTML template
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/map.html.in"
    "${CMAKE_CURRENT_BINARY_DIR}/resources/map.html"
    @ONLY
)

# ─────────────────────────────── Python / pybind11
# Full Python install for embedding + extension module build
find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development Development.Embed)

# Export Python version info for bundling modules
# This ensures bundled Python matches build-time Python for pybind11 compatibility
set(FLYSIGHT_PYTHON_VERSION "${Python_VERSION_MAJOR}.${Python_VERSION_MINOR}" CACHE INTERNAL "Python major.minor version")
set(FLYSIGHT_PYTHON_FULL_VERSION "${Python_VERSION}" CACHE INTERNAL "Python full version")

# =============================================================================
# Python Configuration Validation and Diagnostics
# =============================================================================
message(STATUS "")
message(STATUS "========================================")
message(STATUS "Python Configuration")
message(STATUS "========================================")
message(STATUS "  Interpreter: ${Python_EXECUTABLE}")
message(STATUS "  Version: ${Python_VERSION}")
message(STATUS "  Include dirs: ${Python_INCLUDE_DIRS}")
message(STATUS "  Libraries: ${Python_LIBRARIES}")
if(Python_Development.Embed_FOUND)
    message(STATUS "  Embed library: Found")
else()
    message(WARNING "  Embed library: NOT FOUND - embedding may fail")
endif()
message(STATUS "========================================")
message(STATUS "")

# Validate Python version is suitable for bundling
if(Python_VERSION_MAJOR LESS 3 OR (Python_VERSION_MAJOR EQUAL 3 AND Python_VERSION_MINOR LESS 8))
    message(FATAL_ERROR
        "Python ${Python_VERSION} is too old. Minimum required: 3.8\n"
        "pybind11 and the bundled Python require Python 3.8+"
    )
endif()

# Warn if Python version differs significantly from expected bundle version
# This can cause ABI incompatibility with pybind11 modules
set(_expected_bundle_minor "13")  # Update when changing default bundle version
if(NOT Python_VERSION_MINOR EQUAL ${_expected_bundle_minor})
    message(WARNING
        "Build-time Python ${Python_VERSION} differs from expected bundle version 3.${_expected_bundle_minor}.\n"
        "This may cause issues if python-build-standalone doesn't have version ${Python_VERSION}.\n"
        "The bundle will use Python ${Python_VERSION} - ensure it's available in python-build-standalone releases."
    )
endif()

add_subdirectory("${THIRD_PARTY_DIR}/pybind11"
                 "${CMAKE_BINARY_DIR}/pybind11-build")

# ─────────────────────────────── helpers
function(flysight_set_output_to_build_root tgt)
  # Make outputs predictable for multi-config generators (MSVC, Ninja Multi-Config)
  foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${cfg}" cfg_u)
    set_target_properties(${tgt} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_${cfg_u} "${CMAKE_BINARY_DIR}"
      LIBRARY_OUTPUT_DIRECTORY_${cfg_u} "${CMAKE_BINARY_DIR}"
      ARCHIVE_OUTPUT_DIRECTORY_${cfg_u} "${CMAKE_BINARY_DIR}"
    )
  endforeach()
endfunction()

function(flysight_msvc_fix_python_debug_autolink tgt)
  if(NOT MSVC)
    return()
  endif()

  # CPython headers auto-link pythonXY_d.lib when _DEBUG is defined.
  # Windows installers usually ship only pythonXY.lib (release import lib).
  set(_py_debug_lib "python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}_d.lib")

  target_link_options(${tgt}
    PRIVATE $<$<CONFIG:Debug>:/NODEFAULTLIB:${_py_debug_lib}>)

  # Ensure the release import lib is present in Debug too
  target_link_libraries(${tgt}
    PRIVATE $<$<CONFIG:Debug>:Python::Python>)
endfunction()

# ─────────────────────────────── third-party targets

# QCustomPlot as a real library target (instead of compiling into the exe)
add_library(qcustomplot STATIC
  "${QCUSTOMPLOT_DIR}/qcustomplot.cpp"
)
target_include_directories(qcustomplot
  PUBLIC  "${QCUSTOMPLOT_ROOT}"
  PRIVATE "${QCUSTOMPLOT_DIR}"
)
target_link_libraries(qcustomplot PUBLIC Qt${QT_VERSION_MAJOR}::Widgets)

# GeographicLib (prebuilt by superbuild)
find_package(GeographicLib REQUIRED
    PATHS "${GEOGRAPHIC_ROOT}/lib/cmake/GeographicLib"
    NO_DEFAULT_PATH)
message(STATUS "GeographicLib found: ${GeographicLib_VERSION}")
message(STATUS "  Libraries: ${GeographicLib_LIBRARIES}")

# Boost (header-only; boost::geometry used by simplification calculations)
# BOOST_ROOT and BOOST_LIBRARYDIR are set by cmake/BoostDiscovery.cmake with
# platform-appropriate defaults.

find_package(Boost REQUIRED)

# GTSAM and oneTBB (prebuilt by superbuild): exported target "gtsam", the
# runtime libraries to deploy, and the flysight_solver_* helpers. Must stay
# after the GeographicLib lookup above; the module explains why.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/SolverDependencies.cmake")

# KDDockWidgets (KDDW)
find_package(KDDockWidgets-qt6 CONFIG REQUIRED)

# Verify KDDockWidgets was found correctly
if(TARGET KDAB::kddockwidgets)
    get_target_property(_kddw_location KDAB::kddockwidgets LOCATION)
    get_target_property(_kddw_include_dirs KDAB::kddockwidgets INTERFACE_INCLUDE_DIRECTORIES)
    message(STATUS "KDDockWidgets found:")
    message(STATUS "  Library: ${_kddw_location}")
    message(STATUS "  Includes: ${_kddw_include_dirs}")
else()
    message(FATAL_ERROR "KDDockWidgets target KDAB::kddockwidgets not found after find_package succeeded. "
                        "Check KDDockWidgets-qt6Config.cmake for proper target export.")
endif()

# ─────────────────────────────── C++ model library
add_library(flysight_model STATIC
  sessiondata.cpp            sessiondata.h
  dependencykey.h

  # Calculation engine (Qt Core only): the one cache and dependency graph
  # behind every SessionData read
  engine/calctypes.h
  engine/sessionstate.h
  engine/calculationdescriptor.h
  engine/calculationresult.cpp     engine/calculationresult.h
  engine/evaluationcontext.cpp     engine/evaluationcontext.h
  engine/calculationregistry.cpp   engine/calculationregistry.h
  engine/calculationengine.cpp     engine/calculationengine.h
)
target_include_directories(flysight_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(flysight_model PUBLIC Qt${QT_VERSION_MAJOR}::Core)

# ─────────────────────────────── C++ core library
# Everything non-UI that sits on top of flysight_model: import/export, logbook,
# session model, registries, unit conversion, and the built-in calculations.
# It needs Qt Core + Gui (QColor) only, so test executables can link it without
# Widgets, WebEngine, KDDockWidgets, QCustomPlot, Multimedia, or Python.
# flysight_model stays separate because flysight_cpp_bridge links it and must
# not acquire these dependencies.
add_library(flysight_core STATIC
  dataimporter.cpp           dataimporter.h
  parsedfile.h
  # Import and merge rules: pure plan-then-apply merge, and the widget-free
  # import driver the main window calls
  sessionmerge.cpp           sessionmerge.h
  sessionimport.cpp          sessionimport.h
  dataexporter.cpp           dataexporter.h
  csvformat.cpp              csvformat.h
  logbookmanager.cpp        logbookmanager.h
  logbookcolumn.cpp          logbookcolumn.h
  sessionmodel.cpp           sessionmodel.h
  idlescheduler.cpp          idlescheduler.h
  attributeregistry.cpp      attributeregistry.h
  markerregistry.cpp         markerregistry.h
  plotregistry.cpp           plotregistry.h
  momentmodel.cpp            momentmodel.h
  altitudemarkerfeature.cpp  altitudemarkerfeature.h
  # Header-only Q_OBJECT singleton: must be listed in exactly one target so
  # AUTOMOC generates its meta-object once.
  preferences/preferencesmanager.h
  preferences/preferencekeys.h
  preferences/enginepreferenceprovider.cpp preferences/enginepreferenceprovider.h
  units/unitdefinitions.h
  units/unitconversion.h
  units/unitconverter.cpp    units/unitconverter.h
  # Conversion layer: source -> effective values (schema correction, then
  # unit normalization), registered with the engine as source conversions
  conversion/schematable.cpp       conversion/schematable.h
  conversion/sourceconversion.cpp  conversion/sourceconversion.h
  calculations/builtincalculations.cpp        calculations/builtincalculations.h
  calculations/registration.h
  calculations/interpolationcalculations.cpp  calculations/interpolationcalculations.h
  calculations/attributecalculations.cpp      calculations/attributecalculations.h
  calculations/attributeregistration.cpp     calculations/attributeregistration.h
  calculations/gnsscalculations.cpp           calculations/gnsscalculations.h
  calculations/imucalculations.cpp            calculations/imucalculations.h
  calculations/magcalculations.cpp            calculations/magcalculations.h
  calculations/timecalculations.cpp           calculations/timecalculations.h
  calculations/simplificationcalculations.cpp calculations/simplificationcalculations.h
  calculations/wspcalculations.cpp            calculations/wspcalculations.h
  calculations/spcalculations.cpp             calculations/spcalculations.h
  calculations/derivativehelper.cpp            calculations/derivativehelper.h
  calculations/isadensity.cpp                  calculations/isadensity.h
)
target_link_libraries(flysight_core
  PUBLIC  flysight_model Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui
  PRIVATE ${GeographicLib_LIBRARIES} Boost::boost)

# ─────────────────────────────── application
set(PROJECT_SOURCES
  main.cpp
  mainwindow.cpp             mainwindow.h          mainwindow.ui
  plotviewsettingsmodel.cpp  plotviewsettingsmodel.h
  plottool/pantool.h
  plottool/plottool.h
  plottool/selecttool.cpp    plottool/selecttool.h
  plottool/zoomtool.h
  plottool/setexittool.h     plottool/setexittool.cpp
  plottool/setsynctool.h     plottool/setsynctool.cpp
  plottool/setgroundtool.h   plottool/setgroundtool.cpp
  plottool/setcoursetool.h   plottool/setcoursetool.cpp
  plottool/measuretool.h     plottool/measuretool.cpp
  measuremodel.h
  preferences/preferencesdialog.h  preferences/preferencesdialog.cpp
  preferences/generalsettingspage.h preferences/generalsettingspage.cpp
  preferences/importsettingspage.h  preferences/importsettingspage.cpp
  preferences/legendsettingspage.h  preferences/legendsettingspage.cpp
  preferences/mapsettingspage.h     preferences/mapsettingspage.cpp
  preferences/markerssettingspage.h preferences/markerssettingspage.cpp
  preferences/altitudemarkerssettingspage.h preferences/altitudemarkerssettingspage.cpp
  preferences/addcolumndialog.cpp         preferences/addcolumndialog.h
  preferences/logbooksettingspage.cpp     preferences/logbooksettingspage.h
  preferences/plotssettingspage.h  preferences/plotssettingspage.cpp
  preferences/zoomsettingspage.h   preferences/zoomsettingspage.cpp
  preferences/aerodynamicssettingspage.h preferences/aerodynamicssettingspage.cpp
  crosshairmanager.h         crosshairmanager.cpp
  graphinfo.h
  plotutils.cpp              plotutils.h
  pluginhost.h               pluginhost.cpp
  pluginadapters.h           pluginadapters.cpp
  profile.cpp                profile.h
  profilemanager.cpp         profilemanager.h
  profilestatebridge.cpp     profilestatebridge.h
  manageprofilesdialog.cpp   manageprofilesdialog.h
  python_output_redirector.h
  plotmodel.cpp              plotmodel.h
  plotrangemodel.cpp         plotrangemodel.h
  markermodel.cpp            markermodel.h
  ui/docks/AppContext.h
  ui/docks/DockFeature.h
  ui/docks/DockRegistry.h    ui/docks/DockRegistry.cpp
  ui/docks/plotselection/PlotSelectionDockFeature.h    ui/docks/plotselection/PlotSelectionDockFeature.cpp
  ui/docks/markerselection/MarkerSelectionDockFeature.h    ui/docks/markerselection/MarkerSelectionDockFeature.cpp
  ui/docks/logbook/LogbookDockFeature.h    ui/docks/logbook/LogbookDockFeature.cpp
  ui/docks/logbook/LogbookView.h    ui/docks/logbook/LogbookView.cpp
  ui/docks/legend/LegendDockFeature.h    ui/docks/legend/LegendDockFeature.cpp
  ui/docks/legend/LegendWidget.h    ui/docks/legend/LegendWidget.cpp
  ui/docks/legend/LegendPresenter.h    ui/docks/legend/LegendPresenter.cpp
  ui/docks/legend/LegendTableModel.h    ui/docks/legend/LegendTableModel.cpp
  ui/docks/analysis/AnalysisMethodWidget.h
  ui/docks/analysis/AnalysisDockWidget.h    ui/docks/analysis/AnalysisDockWidget.cpp
  ui/docks/analysis/AnalysisDockFeature.h    ui/docks/analysis/AnalysisDockFeature.cpp
  ui/docks/analysis/StepCommitSpinBox.h
  ui/docks/analysis/WingsuitPerformanceWidget.h    ui/docks/analysis/WingsuitPerformanceWidget.cpp
  ui/docks/analysis/SpeedSkydivingWidget.h    ui/docks/analysis/SpeedSkydivingWidget.cpp
  ui/docks/video/VideoDockFeature.h    ui/docks/video/VideoDockFeature.cpp
  ui/docks/video/VideoWidget.h    ui/docks/video/VideoWidget.cpp
  ui/docks/map/MapDockFeature.h    ui/docks/map/MapDockFeature.cpp
  ui/docks/map/MapWidget.h    ui/docks/map/MapWidget.cpp
  ui/docks/map/MapBridge.h    ui/docks/map/MapBridge.cpp
  ui/docks/map/TrackMapModel.h    ui/docks/map/TrackMapModel.cpp
  ui/docks/map/MapCursorDotModel.h    ui/docks/map/MapCursorDotModel.cpp
  ui/docks/map/MapCursorProxy.h    ui/docks/map/MapCursorProxy.cpp
  ui/docks/map/MapPreferencesBridge.h    ui/docks/map/MapPreferencesBridge.cpp
  ui/docks/plot/PlotDockFeature.h    ui/docks/plot/PlotDockFeature.cpp
  ui/docks/plot/PlotWidget.h    ui/docks/plot/PlotWidget.cpp
  resources.qrc
)

if(WIN32)
  list(APPEND PROJECT_SOURCES FlySightViewer.rc)
endif()

if(APPLE)
  set(MACOSX_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/FlySightViewer.icns")
  set_source_files_properties(${MACOSX_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
  list(APPEND PROJECT_SOURCES ${MACOSX_ICON})
endif()

if(QT_VERSION_MAJOR GREATER_EQUAL 6)
  qt_add_executable(FlySightViewer WIN32 MANUAL_FINALIZATION ${PROJECT_SOURCES})
else()
  add_executable(FlySightViewer WIN32 ${PROJECT_SOURCES})
endif()

# Make the generated version.h header available
target_include_directories(FlySightViewer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# Embed configured map.html as a Qt resource so QWebEngineView can load it via
# qrc://, avoiding file:// origin restrictions on macOS.
qt_add_resources(FlySightViewer map_resource
    PREFIX "/"
    BASE "${CMAKE_CURRENT_BINARY_DIR}/resources"
    FILES "${CMAKE_CURRENT_BINARY_DIR}/resources/map.html"
)

# Profile resources — auto-discovered via glob.
# NOTE: Re-run CMake configure after adding/removing profile files.
file(GLOB PROFILE_RESOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/profiles/*.fvprofile")
list(APPEND PROFILE_RESOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/profiles/profiles.json")

qt_add_resources(FlySightViewer profile_resources
    PREFIX "/"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${PROFILE_RESOURCE_FILES}
)

target_link_libraries(FlySightViewer PRIVATE
  flysight_core
  flysight_model
  qcustomplot
  ${GeographicLib_LIBRARIES}

  KDAB::kddockwidgets

  Qt${QT_VERSION_MAJOR}::Widgets
  Qt${QT_VERSION_MAJOR}::PrintSupport
  Qt${QT_VERSION_MAJOR}::QuickWidgets
  Qt${QT_VERSION_MAJOR}::Quick
  Qt${QT_VERSION_MAJOR}::Qml
  Qt${QT_VERSION_MAJOR}::WebEngineWidgets
  Qt${QT_VERSION_MAJOR}::WebChannel
  Qt${QT_VERSION_MAJOR}::Multimedia
  Qt${QT_VERSION_MAJOR}::MultimediaWidgets

  pybind11::embed
  Boost::boost
)

flysight_set_output_to_build_root(FlySightViewer)
flysight_msvc_fix_python_debug_autolink(FlySightViewer)

# ─────────────────────────────── pybind11 bridge (.pyd)
pybind11_add_module(flysight_cpp_bridge
  cpp_bridge.cpp
  sessiondata_bindings.cpp
  pluginsessionview.h
)

target_link_libraries(flysight_cpp_bridge PRIVATE
  flysight_model
  Qt${QT_VERSION_MAJOR}::Core
)

flysight_set_output_to_build_root(flysight_cpp_bridge)
flysight_msvc_fix_python_debug_autolink(flysight_cpp_bridge)

if(QT_VERSION_MAJOR EQUAL 6)
  qt_finalize_executable(FlySightViewer)
endif()

# ─────────────────────────────── tests (opt-in)
# tests/ lives outside this source tree, hence the explicit binary directory.
# It is added from here (not as a standalone project) so that flysight_core,
# pybind11::embed, Python::Python and flysight_cpp_bridge are all in scope.
# enable_testing() must be called at the top of this build tree or
# `ctest --test-dir <build>` finds nothing.
option(FLYSIGHT_BUILD_TESTS "Build the FlySight Viewer test suite" OFF)
if(FLYSIGHT_BUILD_TESTS)
  enable_testing()
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../tests" "${CMAKE_BINARY_DIR}/tests")
endif()

# ─────────────────────────────── install rules
include(GNUInstallDirs)

# Define cmake module directory for deployment scripts
set(FLYSIGHT_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}/../cmake")

# On macOS, MACOSX_BUNDLE must be set BEFORE install(TARGETS) so that CMake
# uses the BUNDLE destination instead of RUNTIME. qt_add_executable with
# MANUAL_FINALIZATION does not set this automatically.
if(APPLE)
  set_target_properties(FlySightViewer PROPERTIES MACOSX_BUNDLE TRUE)
endif()

# On Windows, use a flat directory structure (all files in root)
# On other platforms, use standard GNU directory layout
if(WIN32)
  set(FLYSIGHT_INSTALL_BINDIR ".")
  set(FLYSIGHT_INSTALL_LIBDIR ".")
else()
  set(FLYSIGHT_INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}")
  set(FLYSIGHT_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
endif()

install(TARGETS FlySightViewer
  BUNDLE  DESTINATION .
  LIBRARY DESTINATION ${FLYSIGHT_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${FLYSIGHT_INSTALL_BINDIR}
)

# Platform-specific installation for pybind11 bridge module
# The module must be in site-packages for Python's import system to find it
if(APPLE)
    # macOS: Install to the app bundle's bundled Python site-packages
    # Path: FlySightViewer.app/Contents/Resources/python/lib/python3.XX/site-packages/
    install(TARGETS flysight_cpp_bridge
        LIBRARY DESTINATION "FlySightViewer.app/Contents/Resources/python/lib/python${Python_VERSION_MAJOR}.${Python_VERSION_MINOR}/site-packages"
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Linux: Installation handled via CreateAppDir.cmake with correct Python version path
    # See Task 8.8: The CreateAppDir.cmake will be updated to use site-packages
else()
    # Windows: Use the bundled Python's site-packages
    install(TARGETS flysight_cpp_bridge
        LIBRARY DESTINATION "python/Lib/site-packages"
        RUNTIME DESTINATION "python/Lib/site-packages"
    )
endif()

# Install Python SDK files to plugin directory (where PluginHost looks for it)
# On macOS, plugins go inside the app bundle so they're included in the DMG.
if(APPLE)
  set(_plugins_dest "FlySightViewer.app/Contents/Resources/python_plugins")
else()
  set(_plugins_dest "python_plugins")
endif()

install(FILES "${CMAKE_CURRENT_LIST_DIR}/../python_plugins/flysight_plugin_sdk.py"
              "${CMAKE_CURRENT_LIST_DIR}/../python_plugins/README.md"
  DESTINATION "${_plugins_dest}"
)

# The bundled example goes into a subdirectory on purpose: PluginHost imports
# only the *.py files directly inside the plugin directory, so an installed
# example is never auto-loaded. A user enables it by copying it one level up.
install(FILES "${CMAKE_CURRENT_LIST_DIR}/../python_plugins/examples/imu_tilt.py"
  DESTINATION "${_plugins_dest}/examples"
)


# ─────────────────────────────── Qt deployment (Qt 6.3+)
# On all platforms with Qt 6, use the built-in deployment API to bundle Qt libraries.
# We use the QML-aware variant (qt_generate_deploy_qml_app_script) because the app
# includes QML components (VideoSurface). This is a strict superset of
# qt_generate_deploy_app_script — it deploys all C++ runtime dependencies AND
# automatically discovers and deploys QML modules (QtMultimedia, etc.)
# by scanning QML source files for import statements.
if(QT_VERSION_MAJOR GREATER_EQUAL 6)
  qt_generate_deploy_qml_app_script(
    TARGET FlySightViewer
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
    DEPLOY_USER_QML_MODULES_ON_UNSUPPORTED_PLATFORM
    NO_COMPILER_RUNTIME
  )
  # On macOS, QML modules must be deployed inside the app bundle at
  # Contents/Resources/qml/ to match the qt.conf generated by macdeployqt.
  # Qt's default QT_DEPLOY_QML_DIR is "qml" (relative to install prefix),
  # which places them outside the bundle. Override it before the deploy script runs.
  if(APPLE)
    install(CODE "set(QT_DEPLOY_QML_DIR \"FlySightViewer.app/Contents/Resources/qml\")")
  endif()
  if(WIN32)
    install(CODE "set(QT_DEPLOY_BIN_DIR \".\")")
  endif()
  install(SCRIPT ${deploy_script})
endif()

# ─────────────────────────────── Qt Plugin and QML Module Deployment
# Include unified Qt path discovery module
include("${FLYSIGHT_CMAKE_DIR}/QtPathDiscovery.cmake")

# Discover Qt resource directories (plugins, QML modules)
if(QT_VERSION_MAJOR GREATER_EQUAL 6)
  find_qt6_resource_dirs()

  # ─────────────────────────────── Additional Qt Plugin Categories
  # qt_generate_deploy_app_script handles most plugins (platforms, imageformats, etc.)
  # but some may need manual deployment. This section handles any additional categories.
  #
  # Note: Most plugins (platforms, imageformats, iconengines) are already deployed
  # by qt_generate_deploy_app_script. Only add categories here if they are confirmed
  # to be missing from the deployment.
  #
  # To add a plugin category, add it to the list below:
  set(ADDITIONAL_QT_PLUGIN_CATEGORIES
    # imageformats  # Usually handled by Qt deploy
    # platforms     # Usually handled by Qt deploy
    # iconengines   # Usually handled by Qt deploy
    # tls           # May be needed for HTTPS map tiles
  )

  # Deploy additional plugin categories if specified
  foreach(_plugin_category ${ADDITIONAL_QT_PLUGIN_CATEGORIES})
    set(_plugin_src "${QT6_PLUGINS_DIR}/${_plugin_category}")

    if(EXISTS "${_plugin_src}")
      # Platform-specific destination
      if(WIN32)
        set(_plugin_dest "plugins/${_plugin_category}")
        set(_plugin_pattern "*.dll")
      elseif(APPLE)
        set(_plugin_dest "FlySightViewer.app/Contents/PlugIns/${_plugin_category}")
        set(_plugin_pattern "*.dylib")
      else()
        if(DEFINED FLYSIGHT_APPDIR_PATH)
          set(_plugin_dest "${FLYSIGHT_APPDIR_PATH}/usr/plugins/${_plugin_category}")
        else()
          set(_plugin_dest "plugins/${_plugin_category}")
        endif()
        set(_plugin_pattern "*.so")
      endif()

      install(DIRECTORY "${_plugin_src}"
        DESTINATION "${_plugin_dest}/.."
        FILES_MATCHING PATTERN "${_plugin_pattern}"
      )
      message(STATUS "Additional Qt plugin category ${_plugin_category} will be deployed")
    else()
      message(STATUS "Qt plugin category ${_plugin_category} not found (may not be needed)")
    endif()
  endforeach()

  # ─────────────────────────────── QtMultimedia QML Module Deployment
  # qt_generate_deploy_qml_app_script scans QML source files for import statements,
  # but VideoSurface.qml is embedded in a .qrc resource and not discovered at deploy
  # time. Manually deploy the QtMultimedia QML module so `import QtMultimedia` works.
  if(QT6_QML_DIR AND EXISTS "${QT6_QML_DIR}/QtMultimedia")
    if(APPLE)
      set(_qtmm_qml_dest "FlySightViewer.app/Contents/Resources/qml")
    elseif(WIN32)
      set(_qtmm_qml_dest "qml")
    else()
      if(DEFINED FLYSIGHT_APPDIR_PATH)
        set(_qtmm_qml_dest "${FLYSIGHT_APPDIR_PATH}/usr/qml")
      else()
        set(_qtmm_qml_dest "qml")
      endif()
    endif()

    install(DIRECTORY "${QT6_QML_DIR}/QtMultimedia"
      DESTINATION "${_qtmm_qml_dest}"
    )
    message(STATUS "QtMultimedia QML module will be deployed from ${QT6_QML_DIR}/QtMultimedia")
  else()
    message(WARNING "QtMultimedia QML module not found at ${QT6_QML_DIR}/QtMultimedia - video output may not work")
  endif()

  # ─────────────────────────────── QML Module & qt.conf Deployment
  # QML modules (e.g., QtMultimedia) are deployed automatically
  # by qt_generate_deploy_qml_app_script() above. It scans QML source files for
  # import statements and deploys the required modules. It also generates qt.conf.
  # No manual QML deployment or qt.conf generation is needed here.
endif()

# ─────────────────────────────── Windows Deployment
if(WIN32)
  # Bundle Python embeddable package
  include("${FLYSIGHT_CMAKE_DIR}/BundlePythonWindows.cmake")

  # Deploy third-party DLLs (GeographicLib, KDDockWidgets, Boost)
  include("${FLYSIGHT_CMAKE_DIR}/DeployThirdPartyWindows.cmake")
  flysight_install_solver_runtime(".")

  # Bundle MSVC runtime DLLs (vcruntime140.dll, msvcp140.dll, etc.) directly
  # into the application folder for app-local deployment. This is the Microsoft-
  # recommended approach for standalone/portable applications — it avoids
  # requiring users to run vc_redist.x64.exe. The Qt deploy script's compiler
  # runtime bundling is disabled via NO_COMPILER_RUNTIME above.
  if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
    include(InstallRequiredSystemLibraries)
  endif()
endif()

# ─────────────────────────────── macOS App Bundle Deployment
if(APPLE)
  # ==========================================================================
  # macOS RPATH Configuration (Single Source of Truth)
  # ==========================================================================
  # All RPATH settings for macOS are consolidated here. Do NOT set RPATH
  # properties in DeployThirdPartyMacOS.cmake or other modules.
  #
  # Key concepts:
  # - Library ID: The "name" of a library, should be @rpath/libname.dylib
  # - RPATH entry: Search paths for @rpath/ references
  # - @executable_path: Relative to the main executable
  # - @loader_path: Relative to the binary doing the loading
  # ==========================================================================

  # Option to control RPATH behavior for CI vs development builds
  option(FLYSIGHT_USE_INSTALL_RPATH_AT_BUILD "Use install RPATH during build (for CI)" OFF)

  # RPATH configuration for the main executable
  # @executable_path/../Frameworks allows the executable to find dylibs in the Frameworks directory
  set_target_properties(FlySightViewer PROPERTIES
    BUILD_RPATH "@executable_path/../Frameworks"
    INSTALL_RPATH "@executable_path/../Frameworks"
    BUILD_WITH_INSTALL_RPATH ${FLYSIGHT_USE_INSTALL_RPATH_AT_BUILD}
    MACOSX_RPATH TRUE
  )

  # Ensure sufficient Mach-O header padding for install_name_tool rewrites.
  # Without this, install_name_tool can fail if the new path is longer than
  # the original and there isn't enough room in the header to grow.
  target_link_options(FlySightViewer PRIVATE
    "$<$<PLATFORM_ID:Darwin>:-Wl,-headerpad_max_install_names>"
  )
  target_link_options(flysight_cpp_bridge PRIVATE
    "$<$<PLATFORM_ID:Darwin>:-Wl,-headerpad_max_install_names>"
  )

  # RPATH configuration for the pybind11 module
  # The module is installed to:
  #   FlySightViewer.app/Contents/Resources/python/lib/python3.XX/site-packages/flysight_cpp_bridge.cpython-3XX-darwin.so
  #
  # Path from module to Frameworks:
  #   site-packages/ -> python3.XX/ -> lib/ -> python/ -> Resources/ -> Contents/ -> Frameworks/
  #   That's 5 levels up: @loader_path/../../../../../Frameworks
  #
  # We also include @executable_path/../Frameworks as a fallback for when
  # the module is loaded from the main executable context.
  set_target_properties(flysight_cpp_bridge PROPERTIES
    BUILD_RPATH "@loader_path/../../../../../Frameworks;@executable_path/../Frameworks"
    INSTALL_RPATH "@loader_path/../../../../../Frameworks;@executable_path/../Frameworks"
    BUILD_WITH_INSTALL_RPATH ${FLYSIGHT_USE_INSTALL_RPATH_AT_BUILD}
    MACOSX_RPATH TRUE
    INSTALL_NAME_DIR "@rpath"
  )

  # Set app bundle properties (MACOSX_BUNDLE itself is set earlier,
  # before install rules, so that CMake uses BUNDLE DESTINATION)
  set_target_properties(FlySightViewer PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.flysight.viewer"
    MACOSX_BUNDLE_BUNDLE_NAME "FlySight Viewer"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_ICON_FILE "FlySightViewer.icns"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  )

  # Include macOS-specific deployment modules
  include("${FLYSIGHT_CMAKE_DIR}/BundlePythonMacOS.cmake" OPTIONAL)
  if(COMMAND bundle_python_macos)
    bundle_python_macos(TARGET FlySightViewer)
  endif()

  # Deploy third-party libraries (GeographicLib, KDDockWidgets)
  include("${FLYSIGHT_CMAKE_DIR}/DeployThirdPartyMacOS.cmake" OPTIONAL)
  if(COMMAND deploy_third_party_macos)
    deploy_third_party_macos(
      TARGET FlySightViewer
      GEOGRAPHIC_ROOT "${GEOGRAPHIC_ROOT}"
      KDDW_ROOT "${KDDW_ROOT}"
    )
  endif()
  flysight_install_solver_runtime("FlySightViewer.app/Contents/Frameworks")

  # Run rpath fixing script after installation
  install(CODE "
    message(STATUS \"Fixing macOS rpaths...\")
    set(FIX_SCRIPT \"${FLYSIGHT_CMAKE_DIR}/fix_macos_rpaths.sh\")
    if(EXISTS \"\${FIX_SCRIPT}\")
      execute_process(
        COMMAND chmod +x \"\${FIX_SCRIPT}\"
        RESULT_VARIABLE _chmod_result
      )
      if(NOT _chmod_result EQUAL 0)
        message(WARNING \"chmod failed on rpath fixing script: \${_chmod_result}\")
      endif()
      execute_process(
        COMMAND bash \"\${FIX_SCRIPT}\" \"\${CMAKE_INSTALL_PREFIX}/FlySightViewer.app\"
        RESULT_VARIABLE _fix_result
      )
      if(NOT _fix_result EQUAL 0)
        message(WARNING \"rpath fixing script returned: \${_fix_result}\")
      endif()
    else()
      message(STATUS \"Skipping rpath fix (script not found)\")
    endif()
  ")

  # ─────────────────────────────── Code signing
  # After all path-fixing scripts have run, sign every Mach-O binary in the
  # bundle. This is required on modern macOS — binaries with invalidated or
  # missing signatures cannot be loaded.
  #
  # Uses Mach-O content detection (not file extension) so nothing is missed:
  #   - .so files (Python pybind11 modules in Resources/python/)
  #   - .dylib files (QML plugins in Resources/qml/)
  #   - extensionless framework binaries (QtCore.framework/Versions/A/QtCore)
  #
  # Signing identity is controlled by FLYSIGHT_CODESIGN_IDENTITY:
  #   - Default ("-"): ad-hoc signing for development
  #   - "Developer ID Application: ...": distribution signing with timestamp
  #     and hardened runtime (required for notarization)
  #
  # For distribution builds, set at configure time:
  #   cmake -B build ... -DFLYSIGHT_CODESIGN_IDENTITY="Developer ID Application: Name (XXXXXXXXXX)"

  set(FLYSIGHT_CODESIGN_IDENTITY "-" CACHE STRING
    "macOS code signing identity. Use '-' for ad-hoc (development) or 'Developer ID Application: ...' for distribution."
  )

  set(FLYSIGHT_CODESIGN_ENTITLEMENTS "" CACHE FILEPATH
    "Path to entitlements plist for macOS code signing. Only used with distribution signing (non-ad-hoc)."
  )

  # Determine signing flags at configure time
  if(FLYSIGHT_CODESIGN_IDENTITY STREQUAL "-")
    set(_codesign_flags "--force --sign -")
    set(_codesign_entitlements_flags "")
    set(_codesign_mode "ad-hoc (development)")
  else()
    set(_codesign_flags "--force --sign \"${FLYSIGHT_CODESIGN_IDENTITY}\" --timestamp --options runtime")
    if(FLYSIGHT_CODESIGN_ENTITLEMENTS AND EXISTS "${FLYSIGHT_CODESIGN_ENTITLEMENTS}")
      set(_codesign_entitlements_flags "--entitlements \"${FLYSIGHT_CODESIGN_ENTITLEMENTS}\"")
      message(STATUS "macOS entitlements: ${FLYSIGHT_CODESIGN_ENTITLEMENTS}")
    else()
      set(_codesign_entitlements_flags "")
      if(NOT FLYSIGHT_CODESIGN_ENTITLEMENTS STREQUAL "")
        message(WARNING "Entitlements file not found: ${FLYSIGHT_CODESIGN_ENTITLEMENTS}")
      endif()
    endif()
    set(_codesign_mode "distribution (${FLYSIGHT_CODESIGN_IDENTITY})")
  endif()

  message(STATUS "macOS code signing: ${_codesign_mode}")

  install(CODE "
    set(_identity \"${FLYSIGHT_CODESIGN_IDENTITY}\")
    set(_entitlements_file \"${FLYSIGHT_CODESIGN_ENTITLEMENTS}\")
    set(_bundle \"\${CMAKE_INSTALL_PREFIX}/FlySightViewer.app\")

    if(_identity STREQUAL \"-\")
      message(STATUS \"Code signing bundle (ad-hoc for development)...\")
      set(_sign_cmd codesign --force --sign -)
      set(_sign_cmd_with_entitlements codesign --force --sign -)
    else()
      message(STATUS \"Code signing bundle (distribution: \${_identity})...\")
      set(_sign_cmd codesign --force --sign \"\${_identity}\" --timestamp --options runtime)
      if(_entitlements_file AND EXISTS \"\${_entitlements_file}\")
        set(_sign_cmd_with_entitlements codesign --force --sign \"\${_identity}\" --timestamp --options runtime --entitlements \"\${_entitlements_file}\")
      else()
        set(_sign_cmd_with_entitlements \${_sign_cmd})
      endif()
    endif()

    # Find all regular files in the bundle
    execute_process(
      COMMAND find \"\${_bundle}/Contents\" -type f
      OUTPUT_VARIABLE _all_files
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    string(REPLACE \"\\n\" \";\" _all_files \"\${_all_files}\")

    # Step 1: Sign each Mach-O binary (detected by content, not extension)
    # Individual binaries get the base signing command (no entitlements)
    set(_signed_count 0)
    foreach(_file \${_all_files})
      execute_process(
        COMMAND file \"\${_file}\"
        OUTPUT_VARIABLE _file_type
        OUTPUT_STRIP_TRAILING_WHITESPACE
      )
      if(_file_type MATCHES \"Mach-O\")
        execute_process(
          COMMAND \${_sign_cmd} \"\${_file}\"
          ERROR_QUIET
        )
        math(EXPR _signed_count \"\${_signed_count} + 1\")
      endif()
    endforeach()

    # Step 2: Sign the QtWebEngineProcess helper app with its own entitlements
    # Qt ships a separate entitlements file for the helper process
    set(_qtwe_helper \"\${_bundle}/Contents/Frameworks/QtWebEngineCore.framework/Versions/A/Helpers/QtWebEngineProcess.app\")
    set(_qtwe_entitlements \"\${_qtwe_helper}/Contents/Resources/QtWebEngineProcess.entitlements\")
    if(EXISTS \"\${_qtwe_helper}\" AND EXISTS \"\${_qtwe_entitlements}\")
      if(NOT _identity STREQUAL \"-\")
        execute_process(
          COMMAND codesign --force --sign \"\${_identity}\" --timestamp --options runtime --entitlements \"\${_qtwe_entitlements}\" \"\${_qtwe_helper}\"
          ERROR_QUIET
        )
        message(STATUS \"Signed QtWebEngineProcess.app with its own entitlements\")
      else()
        execute_process(COMMAND codesign --force --sign - \"\${_qtwe_helper}\" ERROR_QUIET)
      endif()
    endif()

    # Step 3: Sign .framework bundles (Apple requires frameworks signed as bundles)
    file(GLOB _frameworks \"\${_bundle}/Contents/Frameworks/*.framework\")
    list(LENGTH _frameworks _fw_count)
    foreach(_fw \${_frameworks})
      execute_process(COMMAND \${_sign_cmd} \"\${_fw}\" ERROR_QUIET)
    endforeach()

    # Step 4: Sign the top-level app bundle last (inside-out order)
    # The top-level bundle signing uses entitlements (for the main executable)
    execute_process(COMMAND \${_sign_cmd_with_entitlements} \"\${_bundle}\" ERROR_QUIET)

    message(STATUS \"Signed \${_signed_count} Mach-O binaries, \${_fw_count} frameworks, and the app bundle\")
  ")

  # Rename bundle for user-facing display: FlySightViewer.app -> FlySight Viewer.app
  # This keeps the binary name space-free (FlySightViewer) while giving the bundle
  # the human-readable name users expect in Finder and the DMG.
  install(CODE "
    set(_src \"\${CMAKE_INSTALL_PREFIX}/FlySightViewer.app\")
    set(_dst \"\${CMAKE_INSTALL_PREFIX}/FlySight Viewer.app\")
    if(EXISTS \"\${_src}\")
      if(EXISTS \"\${_dst}\")
        file(REMOVE_RECURSE \"\${_dst}\")
      endif()
      file(RENAME \"\${_src}\" \"\${_dst}\")
      message(STATUS \"Renamed bundle: FlySightViewer.app -> FlySight Viewer.app\")
    endif()
  ")
endif()

# ─────────────────────────────── Linux AppImage Deployment
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Linux convention: lowercase with hyphens for the executable name
  # Set RPATH for the executable to find libraries relative to its location
  set_target_properties(FlySightViewer PROPERTIES
    OUTPUT_NAME "flysight-viewer"
    INSTALL_RPATH "$ORIGIN/../lib"
    BUILD_WITH_INSTALL_RPATH FALSE
  )

  # Set RPATH for the pybind11 module
  set_target_properties(flysight_cpp_bridge PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib"
    BUILD_WITH_INSTALL_RPATH FALSE
  )

  # Python bundling for Linux (must be before CreateAppDir to define BUNDLE_PYTHON_VERSION)
  include("${FLYSIGHT_CMAKE_DIR}/BundlePythonLinux.cmake" OPTIONAL)

  # AppDir structure creation (uses BUNDLE_PYTHON_VERSION from BundlePythonLinux)
  include("${FLYSIGHT_CMAKE_DIR}/CreateAppDir.cmake" OPTIONAL)

  # Third-party library deployment
  include("${FLYSIGHT_CMAKE_DIR}/DeployThirdPartyLinux.cmake" OPTIONAL)
  flysight_install_solver_runtime("${FLYSIGHT_APPDIR_USR}/lib")

  # linuxdeployqt integration (optional enhancement)
  include("${FLYSIGHT_CMAKE_DIR}/RunLinuxDeployQt.cmake" OPTIONAL)

  # AppImage generation
  include("${FLYSIGHT_CMAKE_DIR}/CreateAppImage.cmake" OPTIONAL)
endif()

# ─────────────────────────────── packaging
# Determine architecture for package filename
if(APPLE)
  # CMAKE_SYSTEM_PROCESSOR is "arm64" on Apple Silicon, "x86_64" on Intel Macs
  set(FLYSIGHT_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(FLYSIGHT_ARCH "x64")
else()
  set(FLYSIGHT_ARCH "x86")
endif()

# Determine platform name for package filename
if(WIN32)
  set(FLYSIGHT_PLATFORM "Windows")
elseif(APPLE)
  set(FLYSIGHT_PLATFORM "macOS")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(FLYSIGHT_PLATFORM "Linux")
else()
  set(FLYSIGHT_PLATFORM "${CMAKE_SYSTEM_NAME}")
endif()

# Platform-specific generator selection
if(APPLE)
  # Use DMG on macOS for a professional installer experience
  set(CPACK_GENERATOR DragNDrop)
  set(CPACK_DMG_FORMAT UDZO)  # Compressed DMG
  set(CPACK_DMG_VOLUME_NAME "FlySight Viewer ${FLYSIGHT_VERSION_DISPLAY}")
  # Don't include top-level directory for DMG (app bundle goes directly in DMG)
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
else()
  # Use ZIP on Windows and Linux (Linux AppImage handled separately)
  set(CPACK_GENERATOR ZIP)
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
endif()

set(CPACK_PACKAGE_NAME "FlySightViewer")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "FlySightViewer-${FLYSIGHT_VERSION_DISPLAY}-${FLYSIGHT_PLATFORM}-${FLYSIGHT_ARCH}")
set(CPACK_PACKAGE_VENDOR "FlySight")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "FlySight GPS Data Viewer")

# Include LICENSE file if it exists
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../LICENSE")
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_LIST_DIR}/../LICENSE")
endif()

include(CPack)
