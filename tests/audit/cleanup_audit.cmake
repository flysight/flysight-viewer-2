# =============================================================================
# Cleanup audit (spec 12: "remove the old mechanisms rather than leaving them
# alongside the new ones"; spec 2: no new UI; acceptance 19: no remaining use of
# the old per-value cache engine or direct cache setters) and the machine check
# of the acceptance traceability map.
#
#   cmake -DREPO=<repository root> [-DGIT=<git executable>] -P cleanup_audit.cmake
#
# Needs only git. Every rule is a `git grep -E` (tracked AND untracked,
# non-ignored files) or a `git diff` against the baseline tag. ALL violations
# are collected and reported together; the script fails if there is any.
#
# Adding a rule: one expect_none / expect_only / expect_count line below. This
# directory is excluded from every search, so a pattern never matches itself.
# =============================================================================

cmake_minimum_required(VERSION 3.16)

if(NOT REPO)
  message(FATAL_ERROR "usage: cmake -DREPO=<repository root> [-DGIT=<git>] -P cleanup_audit.cmake")
endif()
if(NOT GIT)
  find_program(GIT git)
endif()
if(NOT GIT)
  message(FATAL_ERROR "cleanup audit: git not found")
endif()
get_filename_component(REPO "${REPO}" ABSOLUTE)

set(BASELINE_TAG "v2026.04.1")
set(VIOLATIONS "")
set(RULES 0)

# Always appended: the plugin README legitimately names removed APIs in its
# "what was removed" section, and this directory holds the patterns themselves.
set(ALWAYS_EXCLUDED ":!python_plugins/README.md" ":!tests/audit")
# Default pathspec
set(P src tests python_plugins cmake CMakeLists.txt)

function(_violation text)
  set(VIOLATIONS "${VIOLATIONS}\n  - ${text}" PARENT_SCOPE)
endfunction()

# _grep(<out-var> <regex> <pathspec>...): matching lines ("file:line:text"), as a list
function(_grep out regex)
  execute_process(
    COMMAND "${GIT}" -c core.quotepath=off grep --untracked -I -n -E -e "${regex}" -- ${ARGN} ${ALWAYS_EXCLUDED}
    WORKING_DIRECTORY "${REPO}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(rc GREATER 1)
    message(FATAL_ERROR "cleanup audit: git grep failed (${rc}) for '${regex}': ${stderr}")
  endif()
  # One list element per line; ';' and '[' ']' inside a line must not split or nest it
  string(REPLACE ";" "<semicolon>" stdout "${stdout}")
  string(REPLACE "[" "<" stdout "${stdout}")
  string(REPLACE "]" ">" stdout "${stdout}")
  string(REGEX REPLACE "\r?\n$" "" stdout "${stdout}")
  string(REGEX REPLACE "\r?\n" ";" hits "${stdout}")
  set(${out} "${hits}" PARENT_SCOPE)
endfunction()

# expect_none(<label> <regex> <pathspec>...)
function(expect_none label regex)
  math(EXPR RULES "${RULES} + 1")
  set(RULES ${RULES} PARENT_SCOPE)
  _grep(hits "${regex}" ${ARGN})
  foreach(hit IN LISTS hits)
    _violation("[${label}] ${hit}")
  endforeach()
  set(VIOLATIONS "${VIOLATIONS}" PARENT_SCOPE)
endfunction()

# expect_only(<label> <regex> <allowed-file-regex> <pathspec>...): every hit's file must match
function(expect_only label regex allowed)
  math(EXPR RULES "${RULES} + 1")
  set(RULES ${RULES} PARENT_SCOPE)
  _grep(hits "${regex}" ${ARGN})
  foreach(hit IN LISTS hits)
    string(REGEX REPLACE ":.*$" "" file "${hit}")
    if(NOT file MATCHES "${allowed}")
      _violation("[${label}] only allowed in ${allowed}: ${hit}")
    endif()
  endforeach()
  set(VIOLATIONS "${VIOLATIONS}" PARENT_SCOPE)
endfunction()

# expect_count(<label> <regex> <n> <pathspec>...): exactly n matching lines
function(expect_count label regex n)
  math(EXPR RULES "${RULES} + 1")
  set(RULES ${RULES} PARENT_SCOPE)
  _grep(hits "${regex}" ${ARGN})
  list(LENGTH hits count)
  if(NOT count EQUAL n)
    _violation("[${label}] expected exactly ${n} hit(s) of '${regex}', found ${count}: ${hits}")
  endif()
  set(VIOLATIONS "${VIOLATIONS}" PARENT_SCOPE)
endfunction()

# ─────────────────────────────── old engine
expect_none("old engine"
  "CalculatedValue\\b|calculatedvalue\\.|DependencyManager|dependencymanager|calculatedvalueregistry|CalculatedValueRegistry|m_sideEffectKeys|m_activeCalculations|toDependencyKey"
  ${P})

# ─────────────────────────────── old registration API and direct cache setters
expect_none("old registration / setters"
  "setCalculatedAttribute|setCalculatedMeasurement|registerCalculatedAttribute|registerCalculatedMeasurement|unregisterCalculatedAttribute|addDependencies|hasRegisteredCalculation|set_calculated|m_calculatedAttributes|m_calculatedMeasurements"
  ${P})

# ─────────────────────────────── import-time conversion
expect_none("import-time conversion" "toSI\\b|PHASE4-SWITCH" ${P})
expect_none("import-time conversion (importer)" "UnitConversion|unitconversion\\.h"
  src/dataimporter.cpp src/dataimporter.h)

# ─────────────────────────────── friends and back doors
expect_none("friend / back doors"
  "friend class DataImporter|invalidateAllCalculations|initializeFromDevice|loadAllSessions|scanSessionFiles\\b"
  src tests)

# ─────────────────────────────── ambiguous unit API (not tests: Fs1FileBuilder::units)
expect_none("ambiguous unit API" "\\bgetUnit\\(|[.>]units\\(" src)

# ─────────────────────────────── dead bridge code
expect_none("dead bridge"
  "bridgeimpl|bridge_impl|get_key_name|session_get_time_series|dependencykey_bindings" ${P})
expect_none("dead bridge (DependencyKey crossing)" "DependencyKey"
  python_plugins src/cpp_bridge.cpp "src/*_bindings.cpp")

# ─────────────────────────────── superseded commit 4668f48 (import-time gyro correction)
expect_none("superseded import-time correction"
  "GyroScaling|ImportGyroScaling|DataSchema\\b|dataSchemaVersion|SchemaVersion\\b"
  ${P} docs README.md)

# ─────────────────────────────── renamed-column conventions (spec 3.1)
expect_none("alternate column names" "\"[A-Za-z]*_source\"|\"source:|_source\"" src python_plugins tests)

# ─────────────────────────────── one authority per fact
expect_count("one authority: gyro factor" "1\\.14688" 1 src)
expect_only("one authority: gyro factor" "1\\.14688" "^src/conversion/schematable\\.cpp$" src)
expect_count("one authority: SCHEMA_VER literal" "\"SCHEMA_VER\"" 1 src)
expect_only("one authority: SCHEMA_VER literal" "\"SCHEMA_VER\"" "^src/conversion/schematable\\.h$" src)
expect_count("one authority: compatibility marker" "CalculationCompatibilityVersion *=" 1 src)
expect_only("one authority: number formatting" "FloatingPointShortest" "^src/csvformat\\.cpp$" src)
expect_none("one authority: number formatting" "<charconv>" src)

# ─────────────────────────────── nothing infers the schema
expect_none("no schema inference (conversion, engine)"
  "FIRMWARE_VER|FirmwareVer|fileName|filePath|QFileInfo|QDate" src/conversion src/engine)
expect_none("no schema inference (importer, merge, calculations)"
  "FIRMWARE_VER|FirmwareVer" src/dataimporter.cpp src/sessionmerge.cpp src/calculations)

# ─────────────────────────────── compute functions are pure
expect_none("pure compute functions"
  "PreferencesManager|QSettings|QDateTime::current|std::rand|QRandomGenerator|_SESSION_ID"
  src/calculations src/conversion src/engine)

# ─────────────────────────────── source-input opt-in (F2): the plugin host only
expect_only("source-input opt-in" "allowSourceInputs *= *" "^src/pluginadapters\\.cpp$|^src/engine/calculationdescriptor\\.h$" src)
expect_count("source-input opt-in (default)" "allowSourceInputs *= *false" 1 src/engine/calculationdescriptor.h)

# ─────────────────────────────── one mutation path, one emission path
expect_count("one emitter" "emit dependencyChanged" 1 src)
expect_only("one emitter" "emit dependencyChanged" "^src/sessionmodel\\.cpp$" src)
expect_only("one saver" "exportSession\\(" "^src/logbookmanager\\.(cpp|h)$|^src/dataexporter\\.(cpp|h)$" src)
expect_only("one import path" "mergeSessions\\(" "^src/sessionmodel\\.(cpp|h)$|^src/sessionimport\\.(cpp|h)$" src)
expect_only("one import path" "importFile\\(" "^src/dataimporter\\.(cpp|h)$" src)
expect_none("exporter and merge read stored state only"
  "getAttribute|getMeasurement|effectiveUnit|calculationEngine" src/dataexporter.cpp src/sessionmerge.cpp)

# ─────────────────────────────── leftover markers
expect_none("leftover markers" "BASELINE:|PHASE4-SWITCH" tests src)

# ─────────────────────────────── checks against the baseline tag
execute_process(COMMAND "${GIT}" rev-parse --verify --quiet "${BASELINE_TAG}^{commit}"
  WORKING_DIRECTORY "${REPO}" RESULT_VARIABLE tag_rc OUTPUT_QUIET ERROR_QUIET)

# _added_lines(<out-var> <pathspec>...): the added lines of `git diff <tag>`
function(_added_lines out)
  execute_process(COMMAND "${GIT}" -c core.quotepath=off diff --no-color "${BASELINE_TAG}" -- ${ARGN}
    WORKING_DIRECTORY "${REPO}" RESULT_VARIABLE rc OUTPUT_VARIABLE stdout ERROR_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "cleanup audit: git diff failed (${rc})")
  endif()
  string(REPLACE ";" "<semicolon>" stdout "${stdout}")
  string(REPLACE "[" "<" stdout "${stdout}")
  string(REPLACE "]" ">" stdout "${stdout}")
  string(REGEX REPLACE "\r?\n" ";" lines "${stdout}")
  set(added "")
  foreach(line IN LISTS lines)
    if(line MATCHES "^\\+" AND NOT line MATCHES "^\\+\\+\\+")
      list(APPEND added "${line}")
    endif()
  endforeach()
  set(${out} "${added}" PARENT_SCOPE)
endfunction()

if(tag_rc EQUAL 0)
  # No unfinished work was added
  math(EXPR RULES "${RULES} + 1")
  _added_lines(added src tests python_plugins ":!tests/audit")
  foreach(line IN LISTS added)
    if(line MATCHES "TODO|FIXME")
      _violation("[leftover markers] added line: ${line}")
    endif()
  endforeach()

  # No new UI (spec 2): the only UI-side files that changed at all
  math(EXPR RULES "${RULES} + 1")
  execute_process(COMMAND "${GIT}" -c core.quotepath=off diff --name-only "${BASELINE_TAG}" --
                          src/preferences src/mainwindow.ui src/qml src/resources.qrc src/ui
    WORKING_DIRECTORY "${REPO}" OUTPUT_VARIABLE changed ERROR_QUIET)
  string(REGEX REPLACE "\r?\n$" "" changed "${changed}")
  string(REGEX REPLACE "\r?\n" ";" changed "${changed}")
  set(allowed_ui
    src/preferences/preferencesmanager.h            # adds hasPreference()
    src/preferences/enginepreferenceprovider.h
    src/preferences/enginepreferenceprovider.cpp
    src/ui/docks/plot/PlotWidget.cpp)               # "reset to default" uses the registry query
  foreach(file IN LISTS changed)
    list(FIND allowed_ui "${file}" index)
    if(index EQUAL -1)
      _violation("[no new UI] ${file} changed since ${BASELINE_TAG}")
    endif()
  endforeach()

  # No preference was added
  math(EXPR RULES "${RULES} + 1")
  _added_lines(added src/preferences/preferencekeys.h)
  foreach(line IN LISTS added)
    _violation("[no new UI] preferencekeys.h: ${line}")
  endforeach()

  # No action, menu, dialog, label, status text or preference was added; the
  # one QMessageBox is the existing import-failure box.
  math(EXPR RULES "${RULES} + 1")
  _added_lines(added src/mainwindow.cpp src/ui)
  set(message_boxes 0)
  foreach(line IN LISTS added)
    if(line MATCHES "new QAction|addAction\\(|addMenu\\(|QDialog|QLabel|statusBar\\(\\)|QInputDialog|registerPreference\\(")
      _violation("[no new UI] added line: ${line}")
    endif()
    if(line MATCHES "QMessageBox::")
      math(EXPR message_boxes "${message_boxes} + 1")
    endif()
  endforeach()
  if(message_boxes GREATER 1)
    _violation("[no new UI] ${message_boxes} added QMessageBox:: lines (at most 1: the import-failure box)")
  endif()
else()
  message(STATUS "cleanup audit: tag ${BASELINE_TAG} not available - baseline diff checks skipped")
endif()

# ─────────────────────────────── acceptance traceability
math(EXPR RULES "${RULES} + 1")
set(map_file "${REPO}/tests/acceptance_map.txt")
if(NOT EXISTS "${map_file}")
  _violation("[traceability] tests/acceptance_map.txt is missing")
else()
  file(STRINGS "${map_file}" map_lines)
  set(items_seen "")
  foreach(line IN LISTS map_lines)
    string(STRIP "${line}" line)
    if(line STREQUAL "" OR line MATCHES "^#")
      continue()
    endif()
    if(NOT line MATCHES "^([0-9]+) +(tst_[a-z_]+) +([A-Za-z_0-9]+)$")
      _violation("[traceability] malformed line: ${line}")
      continue()
    endif()
    set(item "${CMAKE_MATCH_1}")
    set(target "${CMAKE_MATCH_2}")
    set(function "${CMAKE_MATCH_3}")
    list(APPEND items_seen "${item}")

    set(source "${REPO}/tests/${target}.cpp")
    if(NOT EXISTS "${source}")
      _violation("[traceability] item ${item}: tests/${target}.cpp does not exist")
      continue()
    endif()
    file(READ "${source}" text)
    string(FIND "${text}" "::${function}()" position)
    if(position EQUAL -1)
      _violation("[traceability] item ${item}: tests/${target}.cpp has no test function ${function}()")
    endif()
  endforeach()
  foreach(item RANGE 1 19)
    list(FIND items_seen "${item}" index)
    if(index EQUAL -1)
      _violation("[traceability] acceptance item ${item} has no line in tests/acceptance_map.txt")
    endif()
  endforeach()
endif()

# ─────────────────────────────── verdict
if(VIOLATIONS)
  message(FATAL_ERROR "cleanup audit FAILED (${RULES} rules):${VIOLATIONS}")
endif()
message(STATUS "cleanup audit passed (${RULES} rules)")
