# =============================================================================
# Cleanup audit: permanent invariants of the source tree, and the machine check
# of the acceptance traceability map.
#
#   - the mechanisms that were replaced (the per-value cache engine, the direct
#     cache setters, import-time unit conversion, the old plugin bridge) stay
#     removed rather than living alongside their replacements (acceptance 19);
#   - each fact has exactly one authority (one gyro factor, one schema-version
#     attribute name, one compatibility marker, one number formatter, one
#     emitter of dependencyChanged, one saver, one import path).
#
#   cmake -DREPO=<repository root> [-DGIT=<git executable>] -P cleanup_audit.cmake
#
# Needs only git. Every rule is a `git grep -E` over the working tree (tracked
# AND untracked, non-ignored files); nothing is compared with an earlier
# revision, so the result depends on the checked-out files alone. ALL violations
# are collected and reported together; the script fails if there is any.
#
# Adding a rule: one expect_none / expect_only / expect_count line below. This
# directory is excluded from every search, so a pattern never matches itself.
#
# Allowing a legitimate hit. The rules are text searches, so a correct change
# can trip one. Each rule that is likely to do so carries an "Allow:" comment
# saying what to edit. The general forms are:
#   expect_none   append a ":!path/to/file" exclusion to that rule's pathspec
#                 (or narrow the regex so it no longer matches the new text);
#   expect_only   add the file to the rule's allowed-file regex;
#   expect_count  change the expected number - and only if the fact really has
#                 gained a second authority, which is usually the bug.
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
# Allow: this matches ANY `.units(` / `->units(` call in src. A new, unrelated
# accessor of that name needs a ":!src/<file>" exclusion here (or another name).
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

# ─────────────────────────────── renamed-column conventions
# A source column and its converted value share one name; no "<name>_source" or
# "source:<name>" spelling may appear.
# Allow: a string literal that merely ends in `_source"` for another reason
# needs a ":!path" exclusion on this rule.
expect_none("alternate column names" "\"[A-Za-z]*_source\"|\"source:|_source\"" src python_plugins tests)

# ─────────────────────────────── one authority per fact
# Allow: these count LINES, comments included. Quoting the gyro factor or the
# "SCHEMA_VER" literal in a comment or log message in src trips the count -
# refer to the named constant instead. Raise a count only for a real second
# authority (and then ask whether it should exist).
expect_count("one authority: gyro factor" "1\\.14688" 1 src)
expect_only("one authority: gyro factor" "1\\.14688" "^src/conversion/schematable\\.cpp$" src)
expect_count("one authority: SCHEMA_VER literal" "\"SCHEMA_VER\"" 1 src)
expect_only("one authority: SCHEMA_VER literal" "\"SCHEMA_VER\"" "^src/conversion/schematable\\.h$" src)
expect_count("one authority: compatibility marker" "CalculationCompatibilityVersion *=" 1 src)
expect_only("one authority: number formatting" "FloatingPointShortest" "^src/csvformat\\.cpp$" src)
expect_none("one authority: number formatting" "<charconv>" src)

# ─────────────────────────────── nothing infers the schema
# Allow: the first rule bans file-name/date vocabulary (fileName, filePath,
# QFileInfo, QDate) from src/conversion and src/engine wholesale. Code there
# that needs such a type for a reason unrelated to choosing a schema takes a
# ":!src/<dir>/<file>" exclusion on that rule.
expect_none("no schema inference (conversion, engine)"
  "FIRMWARE_VER|FirmwareVer|fileName|filePath|QFileInfo|QDate" src/conversion src/engine)
expect_none("no schema inference (importer, merge, calculations)"
  "FIRMWARE_VER|FirmwareVer" src/dataimporter.cpp src/sessionmerge.cpp src/calculations)

# ─────────────────────────────── compute functions are pure
# Allow: a file under these directories that is NOT a compute function (for
# example registration glue that reads a preference default) takes a
# ":!src/<dir>/<file>" exclusion; a compute function never does.
expect_none("pure compute functions"
  "PreferencesManager|QSettings|QDateTime::current|std::rand|QRandomGenerator|_SESSION_ID"
  src/calculations src/conversion src/engine)

# ─────────────────────────────── only the conversion layer reads the source layer
# The registry refuses a source input anywhere but in a source conversion, and
# a descriptor has no field that says otherwise. The second rule finds code
# that declares a source input (or tests for one) outside the conversion layer
# and the engine.
# Allow: a test that builds source conversions on a private registry, or that
# proves a source input is refused, is added to the allowed-file regex. No file
# under src outside src/conversion and src/engine ever is.
expect_none("no source-input permission" "allowSourceInputs" src tests python_plugins)
expect_only("source inputs: conversion layer only"
  "CalcInput::source(Measurement|Unit)|Kind::Source(Measurement|Unit)|isSourceKind"
  "^src/conversion/|^src/engine/|^tests/tst_calcregistry\\.cpp$|^tests/tst_calcengine\\.cpp$|^tests/tst_conversion_engine\\.cpp$"
  src tests python_plugins)

# ─────────────────────────────── one mutation path, one emission path
# Allow: a new legitimate caller of exportSession( / mergeSessions( /
# importFile( is added to that rule's allowed-file regex. These match the bare
# call text, so an unrelated function of the same name trips them too - the
# same edit applies. "emit dependencyChanged" stays at exactly one line.
expect_count("one emitter" "emit dependencyChanged" 1 src)
expect_only("one emitter" "emit dependencyChanged" "^src/sessionmodel\\.cpp$" src)
expect_only("one saver" "exportSession\\(" "^src/logbookmanager\\.(cpp|h)$|^src/dataexporter\\.(cpp|h)$" src)
expect_only("one import path" "mergeSessions\\(" "^src/sessionmodel\\.(cpp|h)$|^src/sessionimport\\.(cpp|h)$" src)
expect_only("one import path" "importFile\\(" "^src/dataimporter\\.(cpp|h)$" src)
# Allow: none expected - the exporter and the merge must not read converted or
# calculated values. A differently-meant identifier containing one of these
# words needs the regex narrowed, not the file excluded.
expect_none("exporter and merge read stored state only"
  "getAttribute|getMeasurement|effectiveUnit|calculationEngine" src/dataexporter.cpp src/sessionmerge.cpp)

# ─────────────────────────────── leftover markers
expect_none("leftover markers" "BASELINE:|PHASE4-SWITCH" tests src)

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
