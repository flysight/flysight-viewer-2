# =============================================================================
# Cleanup audit: permanent invariants of the source tree, and the machine check
# of the acceptance traceability map.
#
#   - the mechanisms that were replaced (the per-value cache engine, the direct
#     cache setters, import-time unit conversion, the old plugin bridge) stay
#     removed rather than living alongside their replacements (acceptance 19);
#   - each fact has exactly one authority (one gyro factor, one schema-version
#     attribute name, one compatibility marker, one number formatter, one
#     emitter of dependencyChanged, one saver, one import path);
#   - the mechanisms of sensor-fusion-clean-port that have no successor stay
#     absent (acceptance 120), and the structure that replaced them stays in
#     place: one worker thread and no locks, GTSAM confined to the fusion
#     kernel, work started only by the demand layer, through the executor, a
#     widget-free core; the stationary-window initializer, its silent poll and the
#     constant-bias algorithm strings retired by the sensor fusion improvements
#     stay absent, and the fusion tools stay isolated (items 212, 218, 231,
#     233, 234, 247);
#   - the stored results of requested calculations live in the logbook's
#     cache/ folder, never in the session file: they are named, written, read,
#     restored and deleted in one place each, and the documents describe them
#     (items 301-350);
#   - a stored result goes stale only when the same result in memory would be
#     dropped or its code changed: a record carries no environment
#     fingerprint, the plug-in code identity is computed in one place at
#     start-up, only an owner destroyed at shutdown removes registrations as
#     teardown, and no document says that unrelated changes make stored
#     results stale (items 401-442);
#   - what is switched on is the request: one widget-free demand layer is the
#     only caller of the executor, nothing below it knows it, the views only
#     read it, "pending" never reaches the model or the index, the executor
#     has one bound of jobs on a below-normal worker, no default profile
#     carries a column over a requested output, and no refresh, cancel, queue
#     or plot-request logic remains (items 501-563);
#   - a fact is computed by the component that owns it and announced by it:
#     the session model alone computes a column's requested calculations
#     (with the index, below it), the demand layer keeps one memory and one
#     walk in parts of its own, and the executor's unused signals and queries
#     stay gone (items 601-662);
#   - background work is shown in one place and a failure once per
#     recording: the status bar presents the scheduler's tasks and the demand
#     layer's progress and failures, the logbook row a recording's failures,
#     and one drawing of the warning glyph remains, the style's; the
#     per-source presentation, the indicators, the clock and the logbook's
#     progress line stay gone, in code and documents (items 701-754);
#   - a default that stands in for a value the user has not set is a
#     calculation, and every constant one is registered by one helper;
#     neither the importer nor the legacy backfill writes wind; one type owns
#     the orientation vocabulary; the sensor fusion category is the eight
#     plots of the tests' mirror, and no removed fusion plot and no
#     local-frame plot remains in code or documents (items 801-863).
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
# A pathspec that matches no file is not an error for git grep, so a misspelt
# path passes silently: plant a hit once to prove a new rule works.
#
# Rule groups. audit_group(<slug>) at the head of a block of rules names the
# group; tests/acceptance_map.txt cites groups as "<item> audit <slug>", and a
# line that cites an unknown group is a violation.
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

# Word boundaries. The rules run on Windows, Linux and macOS, and git grep -E
# uses the platform's regex library: the GNU escapes (\b, \<, \w, \s, ...) work
# on the first two and match nothing on macOS, where a rule using them checks
# nothing (or, for expect_count, fails). A rule spells a boundary with these
# groups instead: a hit is a matching line, so the extra character matched
# does not count. _grep refuses a pattern with a GNU escape.
set(WB_START "(^|[^A-Za-z0-9_])")    # before a name: the line's start or a non-word character
set(WB_END "([^A-Za-z0-9_]|$)")      # after a name: a non-word character or the line's end

function(_violation text)
  set(VIOLATIONS "${VIOLATIONS}\n  - ${text}" PARENT_SCOPE)
endfunction()

# _grep(<out-var> <regex> <pathspec>...): matching lines ("file:line:text"), as a list
function(_grep out regex)
  if(regex MATCHES "\\\\[bBwWsSdD<>]")
    message(FATAL_ERROR "cleanup audit: '${regex}' uses a GNU regex escape, which macOS "
                        "git grep does not support; spell a word boundary with WB_START / WB_END")
  endif()
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

# audit_group(<slug>): the rules that follow belong to this group (see the header)
function(audit_group slug)
  set_property(GLOBAL APPEND PROPERTY AUDIT_GROUPS "${slug}")
endfunction()

# ─────────────────────────────── old engine
# m_sideEffectFrames: the sibling-frame scaffolding of sensor-fusion-clean-port
# (seventeen per-output registrations writing each other's results).
expect_none("old engine"
  "CalculatedValue${WB_END}|calculatedvalue\\.|DependencyManager|dependencymanager|calculatedvalueregistry|CalculatedValueRegistry|m_sideEffectKeys|m_sideEffectFrames|m_activeCalculations|toDependencyKey"
  ${P})

# ─────────────────────────────── old registration API and direct cache setters
expect_none("old registration / setters"
  "setCalculatedAttribute|setCalculatedMeasurement|registerCalculatedAttribute|registerCalculatedMeasurement|unregisterCalculatedAttribute|addDependencies|hasRegisteredCalculation|set_calculated|m_calculatedAttributes|m_calculatedMeasurements"
  ${P})

# ─────────────────────────────── import-time conversion
expect_none("import-time conversion" "toSI${WB_END}|PHASE4-SWITCH" ${P})
expect_none("import-time conversion (importer)" "UnitConversion|unitconversion\\.h"
  src/dataimporter.cpp src/dataimporter.h)

# ─────────────────────────────── friends and back doors
expect_none("friend / back doors"
  "friend class DataImporter|invalidateAllCalculations|initializeFromDevice|loadAllSessions|scanSessionFiles${WB_END}"
  src tests)

# ─────────────────────────────── ambiguous unit API (not tests: Fs1FileBuilder::units)
# Allow: this matches ANY `.units(` / `->units(` call in src. A new, unrelated
# accessor of that name needs a ":!src/<file>" exclusion here (or another name).
expect_none("ambiguous unit API" "${WB_START}getUnit\\(|[.>]units\\(" src)

# ─────────────────────────────── dead bridge code
expect_none("dead bridge"
  "bridgeimpl|bridge_impl|get_key_name|session_get_time_series|dependencykey_bindings" ${P})
expect_none("dead bridge (DependencyKey crossing)" "DependencyKey"
  python_plugins src/cpp_bridge.cpp "src/*_bindings.cpp")

# ─────────────────────────────── superseded commit 4668f48 (import-time gyro correction)
expect_none("superseded import-time correction"
  "GyroScaling|ImportGyroScaling|DataSchema${WB_END}|dataSchemaVersion|SchemaVersion${WB_END}"
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

# ─────────────────────────────── constant defaults (items 835-839, 851)
# A default that stands in for a value the user has not set is a registered
# calculation. A constant one is registered by Calculations::addConstantDefault
# and by nothing else, so one search finds them all; the importer and the
# legacy backfill write no wind.
audit_group(constant-defaults)
# A compute that ignores its context is a constant, and only the helper writes
# one. Allow: a calculation that genuinely needs no context but is not an
# attribute default names its parameter (and says why it ignores it), or
# registers through the helper; never add a file to the allowed regex.
expect_only("one authority: constant defaults" "\\]\\(const EvaluationContext *&\\)"
  "^src/calculations/attributecalculations\\.h$" src)
# The local copies the helper replaced. Allow: none; the helper is the one way.
expect_none("the replaced default helpers stay gone" "register(Sp|Wsp)Default" src tests)
# Wind is a constant default, not an import default or a backfill. Allow: none;
# a wind the user sets is stored by the logbook's edit, not by these files.
expect_none("no wind default in the importer or the backfill" "_WIND_|WindN|WindE"
  src/dataimporter.cpp src/dataimporter.h src/logbookmanager.cpp src/logbookmanager.h)

# ─────────────────────────────── orientation (item 821)
# The orientation type (Fusion::Orientation) is the one place that spells the
# vocabulary of the orientation attribute: its choices, its constant default
# and the attitude's parser all come from it. Tests and documents spell tokens
# legitimately, so only src is searched. Allow: none expected; code that needs
# a token asks the type (a comment names an orientation in words, "forward +y,
# up +z", or by its label). Never add a file to the allowed regex.
audit_group(orientation)
expect_only("one authority: orientation tokens" "[+-][xyz],[+-][xyz]"
  "^src/fusion/orientation\\.(cpp|h)$" src)

# The recording-wide local frame is the only projection: the simplified track
# (and anything else that needs metres) consumes Local/..., never its own.
# Allow: a second legitimate user of LocalCartesian is added to the
# allowed-file regex - after asking why it cannot read Local/... instead.
audit_group(local-projection)
expect_only("one authority: local projection" "LocalCartesian"
  "^src/calculations/localcoordinatecalculations\\.cpp$" src)
expect_none("simplified track: shared frame only" "GeographicLib|boost"
  "src/calculations/simplificationcalculations.*")

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
  src/calculations src/conversion src/engine src/fusion)

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
  "^src/conversion/|^src/engine/|^tests/tst_calcregistry\\.cpp$|^tests/tst_calcengine\\.cpp$|^tests/tst_calcengine_oracle\\.cpp$|^tests/tst_calcengine_restore\\.cpp$|^tests/tst_conversion_engine\\.cpp$"
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

# =============================================================================
# Sensor fusion as an explicit calculation, with plot-driven background jobs
# (acceptance items 101-120). The rules below keep the mechanisms of the branch
# sensor-fusion-clean-port that have no successor out of the tree, and keep the
# structure that replaced them in place.
# =============================================================================

# The demand layer's files: the component (the reconciler) and its parts.
set(DEMAND_LAYER "src/calculationdemand.*" "src/demandstate.*" "src/demandfill.*" "src/demandsettleclock.*")
set(DEMAND_FILES "(calculationdemand|demandstate|demandfill|demandsettleclock)")

# The logic of background work: the executor, its model, the demand layer, and
# the fusion library.
set(FUSION_CORE "src/jobqueue.*" "src/jobmodel.*" ${DEMAND_LAYER} src/fusion)

# ─────────────────────────────── branch-mechanisms (acceptance 120; item 739)
# The branch ran the fit inside a getter, behind an application-modal progress
# dialog with a nested event loop, and needed a thread-local re-entrancy guard,
# an idle-scheduler pause and a plot-widget rebuild guard to survive that.
# Nothing runs inside a getter any more, so none of them has a successor.
audit_group(branch-mechanisms)
# Allow: the import progress dialog in mainwindow.cpp is the only modal
# progress in the application (the #include and the dialog: two lines). A
# COMMENT that names the class trips the count - reword the comment. A new
# modal progress dialog needs a better reason than "a calculation is slow".
expect_only("modal progress is the import dialog only" "QProgressDialog" "^src/mainwindow\\.cpp$" src)
expect_count("modal progress is the import dialog only" "QProgressDialog" 2 src)
# Allow: none expected. A nested event loop is how a slow calculation blocks
# the application while pretending not to.
expect_none("no nested event loop" "QEventLoop|processEvents" src)
expect_none("no re-entrancy guard, no 'a calculation is running' flag"
  "thread_local|sensorFusionIsRunning|fusionRunning" src tests)
# Allow: the idle scheduler knows nothing about calculations or jobs. If it
# ever must, that is a design change to discuss, not an exclusion to add.
expect_none("the idle scheduler is never paused for a calculation"
  "[Ff]usion|[Jj]ob[Qq]ueue|calculations/|IsRunning" src/idlescheduler.cpp src/idlescheduler.h)
# The executor and the kernel never touch the idle scheduler. (The demand
# layer is not in this rule: its column fill is a scheduler task.)
expect_none("jobs never touch the idle scheduler" "[Ii]dle[Ss]cheduler"
  "src/jobqueue.*" "src/jobmodel.*" src/fusion)
# m_pendingRebuildLevel is master's own and is not part of this rule.
expect_none("no plot rebuild guard" "m_rebuildingPlot|QScopedValueRollback" src/ui/docks/plot)
# Allow: none expected. A calculation outcome is reported by the status bar's
# warning and the logbook row, which present the demand layer; never by a
# dialog or a message box, and the kernel, the calculations and the plot list
# never use the status bar.
expect_none("no dialog or message box for a calculation outcome"
  "QMessageBox|QProgressDialog|QDialog|QErrorMessage|statusBar\\("
  ${FUSION_CORE} src/engine src/calculations src/ui/docks/plotselection)
# A rejection and a solver failure are results of the compute function, cached
# by the engine like any other; the registration never catches and stores one.
expect_none("no hand-cached failure" "catch *\\(" src/fusion/fusionregistration.cpp)

# ─────────────────────────────── naming (items 120, 801, 803, 852, 862, 863)
# The algorithm is a batch factor-graph fit and nothing is named after a
# filter; the branch's sensor and output names are gone.
# Allow: tests/README.md is excluded because its section 10 spells these
# patterns when it describes this rule. The patterns are case-sensitive on
# purpose (SP_MediaSeekForward contains the three letters in lower case after
# an upper-case S; "[Ee]kf" does not match it).
audit_group(naming)
set(NAMING_PATHS src tests docs python_plugins cmake CMakeLists.txt README.md
    ":!tests/README.md")
expect_none("nothing is named after a filter" "EKF|[Ee]kf" ${NAMING_PATHS})
expect_none("branch output names are gone" "posN|posE|posD|_IMU_GNSS_EKF|ImuGnssEkf" ${NAMING_PATHS})
# Allow: this count pins the application's plot list to the list the tests
# use (tests/fusion/fusionsessions.cpp, fusionPlots()). Adding a plot means
# changing both, and the number here (PLANS/fusion-accuracy.md will).
expect_count("eight fusion plots" "^ *\\{\"Sensor fusion\", " 8 src/mainwindow.cpp)
# The local frame is the input of sensor fusion and of the simplified track,
# and a column's source; it has no plots of its own. Allow: none expected in
# code or tests (tests/README.md is excluded as above).
expect_none("no local-frame plots" "GNSS \\(Local frame\\)|localFramePlots" src tests ":!tests/README.md")
# A plot row names its measurement and then its type; the fit's own channels
# have no row. The trailing `, "` matches a row's measurement followed by its
# type, never a calculation's ("Fusion", "down"). Allow: none expected; a fit
# channel is read as a measurement (a column, a plug-in input), not plotted.
expect_none("the removed fusion plots stay out of the registry"
  "\"Fusion\", *\"(north|east|down|velN|velE|velD|accN|accE|roll|pitch|yaw|q[xyzw])\", *\"" src)
# Allow: describe the fit's outputs as outputs or measurements; name no removed
# category or plot.
expect_none("the documents describe the eight fusion plots"
  "[Ss]eventeen( real)? plots|GNSS \\(Local frame\\)|Quaternion [WXYZ]|quaternion plots" docs README.md)

# ─────────────────────────────── solver-confinement
# Text half of "only the code that needs GTSAM links it". The link half is
# checked at configure time by flysight_assert_solver_confinement()
# (cmake/SolverDependencies.cmake), which sees real link closures.
audit_group(solver-confinement)
# Allow: a new kernel file under src/fusion is already allowed. A new TEST or
# tool that needs GTSAM types is added to the regex and to the FUSION block of
# tests/CMakeLists.txt (and, if it links gtsam itself, to _FLYSIGHT_GTSAM_NAMERS
# in cmake/SolverDependencies.cmake); nothing else under src ever is.
expect_only("GTSAM headers: kernel and its tests only" "#include <gtsam/"
  "^src/fusion/|^tests/(tst_solver_smoke\\.cpp|solverprobe\\.h|solver_deploy_probe\\.cpp|tst_fusion_kernel\\.cpp|fusion_golden_capture\\.cpp|README\\.md)$"
  src tests cmake)
# oneTBB is GTSAM's thread pool; one adapter drives its workers' priority
# (fusion/solverthreads.h). Allow: none expected; a new use of oneTBB belongs
# in that adapter.
expect_only("oneTBB: the solver-threads adapter only" "#include [<\"](oneapi/)?tbb/"
  "^src/fusion/solverthreads\\.cpp$" src tests)
expect_none("public and registration files are GTSAM-free" "#include <(gtsam|Eigen)"
  src/fusion/fusion.h src/fusion/fusionregistration.h src/fusion/fusionregistration.cpp
  src/fusion/orientation.h src/fusion/orientation.cpp)
# Allow: only the registration adapter may see the engine and the session keys.
expect_none("the kernel is pure"
  "#include [<\"](sessiondata|sessionmodel|engine/|jobqueue|${DEMAND_FILES}|preferences/|QApplication|QWidget|QtWidgets|QtGui)"
  src/fusion ":!src/fusion/fusionregistration.cpp" ":!src/fusion/fusionregistration.h")
expect_none("the kernel does not log" "qWarning|qInfo|qDebug|qCritical" src/fusion)
# flysight_core never references the fusion library; the application calls its
# one entry point next to the built-in registration.
expect_none("nobody but the application references the fusion library" "fusion/|Fusion::"
  src/calculations src/engine "src/sessionmodel.*" "src/jobqueue.*" "src/jobmodel.*" ${DEMAND_LAYER})
# Narrow and case-sensitive on purpose: the GTSAM_..._BOOST_... option and
# macro names and the "Boost::" test of the Boost-free guard in
# cmake/SolverDependencies.cmake must not match.
expect_none("no Boost in FlySight sources or build"
  "#include <boost/|boost::[a-z]|find_package\\(Boost|find_dependency\\(Boost|Boost::boost|BoostDiscovery"
  src tests cmake CMakeLists.txt third-party/CMakeLists.txt)

# ─────────────────────────────── one-worker
# One thread owns all state; the worker owns captured inputs and nothing else.
# Do not add locks to make shared access safe: remove the sharing.
# Allow: these match comments too. Prose such as "any thread" is fine; the
# class names are not - reword the comment.
audit_group(one-worker)
expect_only("one place creates a thread" "QThread|std::thread|QtConcurrent|QThreadPool|std::async"
  "^src/jobqueue\\.(cpp|h)$" src)
expect_none("no locks"
  "QMutex|QReadWriteLock|QWaitCondition|QSemaphore|std::mutex|std::shared_mutex|std::condition_variable" src)
expect_only("one atomic: the cancel flag" "std::atomic|QAtomic" "^src/jobqueue\\.cpp$" src)

# ─────────────────────────────── gestures (items 116, 306, 501, 519, 527, 534, 543, 544, 562, 605, 608, 625, 628, 629, 651, 659, 661, 737, 740)
# Only the demand layer starts requested calculations, and it derives what to
# start from what is switched on; nothing is a gesture. MainWindow cannot be
# constructed in the test harness, so that nothing in it (start-up restore,
# profiles, the Plots menu) offers or cancels work is a text rule.
audit_group(gestures)
expect_none("no gesture entry points" "plotCheckedByUser|refreshPressed|cancelPressed"
  src tests ":!tests/README.md")
# The opening parenthesis directly after the name keeps publishInvalidation(,
# publishEdges( and publishCalculationInvalidation( out of this rule.
expect_only("explicit work is prepared and published in one place" "[.>]prepare\\(|[.>]publish\\("
  "^src/jobqueue\\.cpp$|^src/engine/" src)
expect_only("no reader requests" "[.>]request\\(" "^src/engine/" src)
# CalculationEngine::request() is the SYNCHRONOUS request: it runs the explicit
# calculation on the calling thread, which in the application is the GUI
# thread. Product code never calls it (tests do, and the engine's own files
# name it); explicit work runs only as a job, through JobQueue::offer() from
# CalculationDemand, which keeps the executor's chosen next job equal to what
# the checked plots need for the visible sessions. The first rule names the
# ways src spells a session's engine (calculationEngine().request(,
# engine.request(, m_engine->request( ...); together with "no reader requests"
# it closes the door on an alias (`auto &e = session.calculationEngine();
# e.request(`). The offer is made on exactly one line of product code, and
# only the demand layer withdraws the chosen next job; no product code cancels
# a job (JobQueue::cancel() is kept for the jobs dock, a later view of the job
# history, and its doc comment says so).
# Allow: none expected. The count changes only when a second legitimate caller
# of JobQueue::offer() appears, which is itself a design change
# (calculationdemand.h: "the only caller"). The choice stays in the reconciler,
# not the fill or the settle clock. A comment that quotes these calls names
# them without the member-access prefix.
expect_none("no synchronous explicit request in product code"
  "[Ee]ngine(\\(\\))? *(\\.|->) *request\\(" src ":!src/engine")
expect_count("one call of offer( in product code: the demand layer" "[.>]offer\\(" 1 src)
expect_only("one call of offer( in product code: the demand layer" "[.>]offer\\("
  "^src/calculationdemand\\.cpp$" src)
expect_only("the chosen next job is withdrawn by the demand layer only" "[.>]withdrawChosenNext\\("
  "^src/calculationdemand\\.cpp$" src)
expect_none("no product code cancels a job" "([Jj]ob[Qq]ueue|m_queue|executor)(->|\\.)cancel\\(" src)
# The cancel operation stays for the jobs dock, a later view of the job
# history; the rule above keeps it without a product caller. Allow: none
# expected; removing it is a decision about the jobs dock, not this rule.
expect_count("cancel is kept for the jobs dock" "bool cancel\\(JobId" 1 src/jobqueue.h)
# The executor lost what no product code used: the idle and queued signals,
# the query of both active jobs, and the busy-period bookkeeping behind the
# idle signal. The job model's rowsInserted, isIdle(), runningJob(),
# chosenNextJob() and the records carry the same facts. Allow: none
# expected; a jobs dock that needs one brings it back with its first caller
# (tests/README.md is excluded: section 10 spells these names).
expect_none("the executor has no idle or queued signal and no two-job query"
  "jobQueued|activeJobs\\(|JobQueue::idle${WB_END}|announceIdleIfIdle|m_idleAnnounced|AfterEnd"
  src tests ":!tests/README.md")
expect_none("the executor announces no idle()" "${WB_START}idle\\(\\)" src)
# The demand layer is the only offerer (the rules above), so the chosen next
# job is always its own: it keeps no memory of its own offer and withdraws
# the chosen next job whenever its choice finds nothing. Allow: none expected.
expect_none("the demand layer keeps no memory of its own offer" "m_offeredJob|withdrawOwnOffer"
  src tests ":!tests/README.md")
# Allow: a future jobs dock is a pure view of JobQueue::model() and is added to
# the regex when it exists. Until then AppContext only carries the pointer.
expect_only("no jobs window, no view of the queue" "[Jj]ob[Qq]ueue|JobModel"
  "^src/ui/docks/AppContext\\.h$" src/ui)
# CalculationRegistry::explicitDependencies() is the one definition of
# "explicit-backed" (dependsOnExplicit() is its non-emptiness): the demand
# layer asks dependsOnExplicit() for plots, the logbook column cache asks
# explicitDependencies()
# through logbookColumnExplicitCalculations(); the session model hands each
# enabled column's requested calculations to the demand layer
# (columnRequestedCalculations()), and the demand layer asks
# explicitDependencies() for a plot's (group demand). Neither tests the policy
# itself, and neither does the result store: what may be stored is decided by
# CalculationEngine::exportResult().
expect_only("one authority: explicit-backed" "EvaluationPolicy::Explicit"
  "^src/engine/|^src/fusion/fusionregistration\\.cpp$" src)

# ─────────────────────────────── widget-free-core
audit_group(widget-free-core)
expect_none("the logic components see no widget"
  "QtWidgets|#include <Q(Widget|TreeView|AbstractItemView|StyledItemDelegate|Application|ToolTip|Style[A-Za-z]*|HeaderView)>"
  "src/jobqueue.*" "src/jobmodel.*" ${DEMAND_LAYER} "src/plotmodel.*")

# =============================================================================
# Sensor fusion improvements (acceptance items 201-247): the segmented
# initializer replaced the stationary-window detector and the single-anchor
# initial attitude, the stopping rule and the temperature model retired the
# constant-bias algorithm strings, and two tools (fusion_golden_capture,
# fusion_runner) joined the fusion-tests block. The rules below keep the retired
# mechanisms out of the tree, the kernel's boundaries and its one
# integrateMeasurement call in place, the fusion document current, and the
# tools isolated and uninstalled.
# =============================================================================

# ─────────────────────────────── fusion-model (items 212, 218, 234, 247)
audit_group(fusion-model)
# Allow: tests/README.md is excluded because its section 10 spells these
# patterns. `kWindowLength` only at a word end (WB_END): the kernel's
# kWindowLengthsMessage (fusionsamples.cpp) is a rejection reason, not a gate.
expect_none("the stationary-window detector is gone"
  "stationarywindow|StationaryWindow|assessStationaryWindow|bestStationaryWindow|kMaxMeanRate|imuGapLimit|kWindowLength${WB_END}|kWindowGrid"
  src tests cmake docs README.md CMakeLists.txt ":!tests/README.md")
# Allow: none expected. Preparation neither reports nor asks; the first
# boundary of a run is "Starting fit" (fusionprogress.h).
expect_none("the silent poll is gone" "pollCancel" src tests)
expect_none("the anchor-attitude initializer is gone"
  "InitialAttitude|initialAttitude\\(|kInitialHeadingDeg|attitudeFromStationaryWindow" src tests)
# Allow: none expected. The goldens say batch-temperature-bias-v3; a hit under
# tests/data/fusion means a stale capture (re-capture, tests/README.md section 11).
expect_none("the retired algorithm strings are gone" "batch-shared-bias-v[12]"
  src tests docs README.md ":!tests/README.md")
# Allow: the branch the kernel was ported from is history: the historical
# sentence of tests/README.md section 11, its section 1 audit row, appendix B,
# and the provenance comment at the head of tst_fusion_kernel.cpp (the literal
# expectations of the reference's self-test). A new historical note is added
# to the allowed-file regex; a new mechanism never is. The count in
# tests/README.md rises only with a new historical note there.
expect_only("the branch is history only" "sensor-fusion-clean-port"
  "^tests/README\\.md$|^tests/tst_fusion_kernel\\.cpp$"
  src tests docs cmake CMakeLists.txt README.md)
expect_count("the branch is history only" "sensor-fusion-clean-port" 4 tests/README.md)
# Allow: these count LINES, comments included. Exactly three checkpoint( call
# sites: "Starting fit" (fusion.cpp), the pass iteration and "Integrating IMU
# factors" (factorgraphfit.cpp), the three kinds of boundary. A new boundary
# kind is added to the run() comment of fusion.h, to docs/SENSOR_FUSION.md
# section 7 and to this count together; a comment that spells `checkpoint(`
# is reworded instead. The per-step covariance is written into the shared
# preintegration parameters before the one `.integrateMeasurement(` call of
# imuintegration.cpp (the member-call form, so the comment above the call
# that names the function does not count).
expect_count("three kinds of boundary" "checkpoint\\(" 3 src/fusion)
expect_count("one integrateMeasurement, per-step covariance" "[.>]integrateMeasurement\\(" 1
  src/fusion/imuintegration.cpp)
# Allow: none expected; tests/README.md spells the call. The step model has
# one author: preintegrateImu() integrates every IMU step and sets its
# covariance, and whatever else needs a step reads it from there (the
# reconstruction through the observer, the tests' one-step reference factors
# by preintegrating one step); no other kernel file and no test integrates a
# step or sets a sensor covariance, and the reconstruction's transition is the
# library's update() on a copy, not the static tangent update. A second reader
# of the step model takes it from preintegrateImu() instead of restating it.
expect_only("the step model has one author"
  "[.>]integrateMeasurement\\(|UpdatePreintegrated|(accelerometer|gyroscope|integration)Covariance"
  "^src/fusion/imuintegration\\.cpp$|^tests/README\\.md$"
  src tests)
# Allow: none expected. The fusion document describes the model as it is:
# no stationary or candidate window, no coarse-only initializer, no frozen
# algorithm, no bias-shift settled test, no branch, twenty-two inputs. Say
# "the previous initializer", "a resting window", "the coarse attitude at the
# anchor" when the history must be mentioned.
expect_none("the fusion document describes the current model"
  "stationary window|candidate window|coarse initializer|frozen|bias shifts below|zero bias shift|sensor-fusion-clean-port|twenty-one"
  docs/SENSOR_FUSION.md)

# ─────────────────────────────── fusion-tooling (items 231, 233, 848)
audit_group(fusion-tooling)
# Allow: none expected. The runner imports model-free (DataImporter::parseFile,
# SessionMerge, the engine on a bare SessionData) and never names the
# preferences singleton, the logbook manager, the engine's preference
# provider, the session model, the application's import driver, the
# import-time defaults or the job queue. QSettings is not in the pattern: the
# runner's defensive redirect block names it on purpose.
expect_none("the runner never touches the logbook or settings"
  "PreferencesManager|LogbookManager|EnginePreferenceProvider|SessionModel|SessionImport|applyCreationDefaults|JobQueue"
  tests/fusion_runner.cpp)
# Allow: none expected. Numbers become text through CsvFormat only
# (formatDouble, formatAttributeValue), so a --csv file reloads bit for bit
# and the goldens are locale-proof.
expect_none("one number formatter in the tools"
  "QString::number\\(|FloatingPointShortest|std::to_chars|QLocale|'g', 17"
  tests/fusion_runner.cpp tests/fusion_golden_capture.cpp)
# Allow: none expected. The three executables of the fusion-tests block are
# built with the tests and never shipped.
expect_none("the fusion tools are not installed"
  "install\\(.*(fusion_runner|fusion_golden_capture|solver_deploy_probe)"
  src tests cmake CMakeLists.txt)
# Allow: a new internal header under src/fusion is added to the pattern; a
# new test that needs the kernel's seams is added to the allowed-file regex
# (and to the GTSAM regex of solver-confinement). The runner includes
# fusion/fusion.h and fusion/fusionregistration.h only; the capture tool and
# tests/fusion/fusiontrace.h include fusion/fusionpipeline.h, the trace seam,
# which is not in the pattern.
expect_only("the tools see the public header or the trace seam only"
  "#include \"fusion/(factorgraphfit|initializer|imuintegration|inputadapter|fusionsamples|fusionoutput|fusionprogress|trajectoryreconstruction|temperatureimufactor|samplestatistics)\\.h\""
  "^src/fusion/|^tests/tst_fusion_kernel\\.cpp$"
  src tests)

# =============================================================================
# Storing requested calculation results with the session (acceptance items
# 301-350): the result of an explicit calculation is kept in a record file in
# the logbook's cache/ folder, never in the session file. The rules below keep the file names and the file
# I/O in the record format and the logbook manager, the store as the one caller
# of that I/O and of the engine's export / restore, restoring separate from
# requesting, the session file ignorant of records, the result version one
# literal, and the documents current.
# =============================================================================

# ─────────────────────────────── stored-results (items 304, 305, 316, 317, 326, 327, 330, 333, 334, 346, 348, 349)
audit_group(stored-results)
# Allow: none expected. The extension and the magic are file-local constants of
# calculationrecord.cpp; everything else asks calculationRecordExtension() or the
# name functions. Header comments are not searched (a comment may quote the
# extension); a .cpp comment that quotes it is reworded.
expect_only("one authority: the record file extension" "\"\\.?fvresult\""
  "^src/calculationrecord\\.cpp$" "src/*.cpp")
# Allow: a new user of the record file names is the logbook manager or nobody.
expect_only("record file names: the record format and the logbook manager only"
  "recordFileName\\(|parseRecordFileName\\(|calculationRecordExtension\\(|calculationRecordPath\\(|calculationRecordFileNames\\("
  "^src/calculationrecord\\.(cpp|h)$|^src/logbookmanager\\.(cpp|h)$" src)
# Allow: none expected. Records are written on an Ok install, read at a load and
# deleted on an input change or when stale - all by the result store; the
# logbook manager deletes them itself with their session and as strays.
expect_only("records are written, read and removed by the result store"
  "${WB_START}(write|read|remove)CalculationRecords?\\("
  "^src/logbookmanager\\.(cpp|h)$|^src/calculationresultstore\\.(cpp|h)$" src)
expect_only("one result store, owned by the session model" "CalculationResultStore"
  "^src/calculationresultstore\\.(cpp|h)$|^src/sessionmodel\\.(cpp|h)$" src)
# Allow: none expected. exportResult() / restoreResult() have one product caller.
expect_only("only the result store exports and restores results" "exportResult\\(|restoreResult\\("
  "^src/engine/|^src/calculationresultstore\\.(cpp|h)$" src)
expect_only("one explicit-result listener, installed by the session model" "setExplicitResultListener\\("
  "^src/engine/|^src/sessionmodel\\.cpp$" src)
# Records are read for a session being loaded into a row and, since
# stored-results validity 9.1, for the column worker's temporary copy of an
# unloaded session - nowhere else (not the bulk edit's temporary load). The
# session model has exactly two call sites: restoreStoredResults() (every path
# that installs a session into a row) and restoreForColumnWorker(), which only
# processNextDirtyColumn() calls (its definition and that call: two lines).
# Allow: none expected. A new reader of records is a change of the
# specification, not of this rule.
expect_only("stored results are restored at a load or by the column worker only" "restoreSession\\("
  "^src/calculationresultstore\\.(cpp|h)$|^src/sessionmodel\\.cpp$" src)
expect_count("two restore call sites: a row's load and the column worker's copy"
  "m_resultStore\\.restoreSession\\(" 2 src)
expect_count("the column worker's copy is restored from its step only" "restoreForColumnWorker\\(" 2
  src/sessionmodel.cpp)
# Restoring is not requesting. Allow: none expected; a comment that names the
# queue is reworded.
expect_none("restoring is not requesting" "JobQueue|CalculationDemand|[.>](request|offer|prepare|publish)\\("
  "src/calculationresultstore.*")
# The session file is the recording: nothing on its path knows a record exists.
# Allow: none expected.
expect_none("the session file knows nothing of stored results"
  "CalculationRecord|calculationrecord|CalculationResultStore|StoredCalculationResult|exportResult|fvresult"
  src/dataexporter.cpp src/dataexporter.h src/dataimporter.cpp src/dataimporter.h
  src/sessionmerge.cpp src/sessionmerge.h src/csvformat.cpp src/csvformat.h "src/sessiondata.*")
# The engine layer knows nothing above it: the code stamps are added by the
# record format, and the store, the logbook and the session model call the
# engine, never the reverse. Allow: none expected. The engine includes only
# itself, ../dependencykey.h and ../csvformat.h.
expect_none("the engine includes nothing of the calculations, the records, the store, the logbook, the model or fusion"
  "#include +[\"<](\\.\\./)*(calculations/|fusion/|logbook|sessionmodel|calculationrecord|calculationresultstore)"
  src/engine)
expect_none("stored results are widget-free"
  "QtWidgets|#include [<\"]Q(Widget|Application|MessageBox|Dialog)|#include \"(\\.\\./)?ui/"
  "src/calculationrecord.*" "src/calculationresultstore.*" "src/engine/storedcalculationresult.*")
# Fusion's result version is its kernel's algorithm string, spelled once.
# Allow: none expected. A changed algorithm changes the one literal; a comment
# or test in src that quotes it names Fusion::Algorithm instead.
expect_count("one authority: the fusion algorithm string" "batch-temperature-bias-v3" 1 src)
expect_only("one authority: the fusion algorithm string" "batch-temperature-bias-v3"
  "^src/fusion/fusion\\.h$" src)
# The compatibility rule names the result version, in the code and in the note.
# Allow: reword the sentence, never duplicate it; the count is 1 in each file.
expect_count("the bump rule names the result version" "CalculationDescriptor::resultVersion" 1
  src/calculations/builtincalculations.h)
expect_count("the bump rule names the result version (docs)" "Bump it, or the result version of the calculation concerned" 1
  docs/CALCULATIONS.md)
# Allow: tests/README.md is excluded because its section 10 describes this rule.
# Say what is kept instead of what used to be lost.
expect_none("no text says requested results are not kept"
  "[Rr]esults are kept in memory only|[Rr]esults are not saved|explicit results are never saved|[Aa]n explicit result is never (persisted|saved)|cached as present and invalid|never cached for unloaded ones"
  src tests docs python_plugins README.md ":!tests/README.md")

# =============================================================================
# Stored results: validity that mirrors memory (acceptance items 401-442): a
# stored result goes stale when the same result in memory would be dropped, and
# when the code that computed it changes. What a result looked up is part of
# its record; what else is registered is not. The rules below keep the
# environment fingerprint out of the record, the plug-in code identity in one
# place and computed once at start-up, teardown removals at shutdown only, the
# environment check away from records, and the documents current.
# =============================================================================

# ─────────────────────────────── result-validity (items 409, 410, 413, 415, 417, 429, 439, 441)
audit_group(result-validity)
# Allow: none expected. What else is registered never makes a record stale, so
# neither the record format, the store nor the snapshot names the calculation
# environment (the logbook column cache does, per column:
# calculationEnvironmentDigest() and logbookColumnEnvironment()). The camelCase
# spelling only: a comment may still say "environment fingerprint" in words.
expect_none("a record carries no environment fingerprint" "calculationEnvironment|CalculationEnvironment"
  "src/calculationrecord.*" "src/calculationresultstore.*" "src/engine/storedcalculationresult.*")
# Allow: none expected. The digest is computed by plugincodeidentity.cpp and
# asked for by the plug-in host at start-up (tests build stand-in identities
# with the same functions; tests are not searched).
expect_only("one authority: the plug-in code identity" "pluginCodeIdentity\\(|readPluginCodeFiles\\("
  "^src/plugincodeidentity\\.(cpp|h)$|^src/pluginhost\\.(cpp|h)$" src)
# Allow: a new owner of registrations that declares a result version (a new
# explicit built-in, say) is added to the allowed-file regex. The plug-in
# adapters build descriptors; the host stamps every plug-in registration in
# registerEach(). `==` comparisons do not match.
expect_only("result versions are declared by the engine, the fusion registration and the plug-in host"
  "[.>]resultVersion *=[^=]"
  "^src/engine/|^src/fusion/fusionregistration\\.cpp$|^src/pluginhost\\.cpp$" src)
# A removal marked as teardown reports no drop, so the records of the results
# it drops survive: only an owner destroyed at shutdown uses it, today the
# altitude-marker manager's destructor. Allow: a new owner that unregisters in
# its destructor is added to both rules (and to docs/CALCULATIONS.md 15.8);
# a runtime change never is.
expect_only("teardown removals only at shutdown" "Removal::Teardown"
  "^src/engine/|^src/altitudemarkerfeature\\.cpp$" src)
expect_count("teardown removals only at shutdown" "Removal::Teardown" 1 src ":!src/engine")
# Allow: none expected. Plug-in registrations live as long as the process.
expect_none("the plug-in host never unregisters" "unregister\\("
  "src/pluginhost.*" "src/pluginadapters.*")
# Plug-in loading stays a start-up operation: MainWindow initialises the host
# once, and nothing reloads plug-ins or watches their files. Allow: a file
# watcher for something other than plug-ins gets a ":!path" exclusion in the
# third rule.
expect_count("plug-ins are loaded once, at start-up" "[.>]initialise\\(" 1
  src ":!src/pluginhost.cpp" ":!src/pluginhost.h")
expect_only("plug-ins are loaded once, at start-up" "[.>]initialise\\("
  "^src/mainwindow\\.cpp$" src ":!src/pluginhost.cpp" ":!src/pluginhost.h")
expect_none("nothing watches the plug-in files" "QFileSystemWatcher" src)
# The environment check touches no record: its whole-session marking existed
# only because records carried the environment fingerprint. A record is
# unconfirmed only after a failed write or removal, or when the store skips it.
# Allow: none expected.
expect_none("the environment check touches no record" "markCalculationRecordsUnconfirmed" src tests)
expect_only("records are skipped by the result store" "markCalculationRecordSkipped\\("
  "^src/logbookmanager\\.(cpp|h)$|^src/calculationresultstore\\.(cpp|h)$" src)
# The bump rule of the amended specification, in the code; the note's copy is
# counted by the stored-results group. Allow: reword around it, never
# duplicate it; the old wording does not come back.
expect_count("the bump rule covers what a requested calculation reads"
  "Bump it, or the result version of the calculation concerned" 1
  src/calculations/builtincalculations.h)
expect_none("the old bump-rule wording is gone" "Bump it, or the calculation's result version"
  src docs README.md)
# Allow: tests/README.md is excluded because its section 10 describes this rule.
# Say what a stored result depends on instead.
expect_none("no text says an unrelated change makes stored results stale"
  "makes every (record|stored result) stale|of a preference that calculations read|marker, the environment fingerprint"
  src tests docs README.md ":!tests/README.md")

# =============================================================================
# Demand-driven requested calculations (acceptance items 501-563): what is
# switched on - checked plots for the visible sessions, enabled logbook columns
# for every session - is the request. One widget-free demand layer derives what
# to compute and is the only caller of the executor (group gestures). The rules
# below keep everything below it ignorant of it, the views read-only, the
# model, the index and the scheduler free of jobs and of "pending", one bound,
# a below-normal worker, the default profiles free of columns over requested
# outputs, and no refresh, cancel, queue or plot-request logic in the code or
# the documents.
# =============================================================================

# ─────────────────────────────── demand (items 506, 508, 513, 515, 518, 527, 529, 530, 533-535, 538, 540-544, 546, 547, 563, 601-605, 607, 612, 613, 615, 616, 622, 624, 627, 631, 636, 641, 644, 646, 647, 649-652, 659-662, 701, 702, 706, 708, 723-728, 732, 736-744, 752-754, 853)
audit_group(demand)
# Allow: a new view that presents the demand layer is added to the allowed-file
# regex; nothing below the demand layer (the executor, the session model, the
# scheduler, the logbook) ever is. Elsewhere a comment says "the demand layer".
# Its views are the logbook's view, dock feature and cell delegate, the status
# bar, and the plot widget and the legend (which ask isMerelyUncomputed() and
# isNotYetComputed() about a value they could not read); the plot list is not
# one of them.
expect_only("only the application and its views know the demand layer" "CalculationDemand"
  "^src/${DEMAND_FILES}\\.(cpp|h)$|^src/mainwindow\\.(cpp|h)$|^src/ui/docks/AppContext\\.h$|^src/ui/docks/logbook/(LogbookView|LogbookCellDelegate)\\.(cpp|h)$|^src/ui/docks/logbook/LogbookDockFeature\\.cpp$|^src/ui/docks/plot/PlotWidget\\.cpp$|^src/ui/docks/legend/LegendPresenter\\.cpp$|^src/ui/statusbar/StatusBarFeature\\.(cpp|h)$"
  src)
# Allow: none expected. The layers below the demand layer never include it
# (so they can use none of its types).
expect_none("nothing below the demand layer includes it" "#include [\"<](\\.\\./)*${DEMAND_FILES}\\.h"
  "src/jobqueue.*" "src/jobmodel.*" "src/sessionmodel.*" "src/idlescheduler.*" "src/logbookmanager.*"
  "src/logbookcolumn.*" "src/plotmodel.*" "src/profilestatebridge.*" "src/calculationresultstore.*"
  src/engine)
# Allow: none expected. The views read progress, failures and the pending
# cells; a pass, the load step and the settle seams belong to the demand
# layer and to tests.
expect_none("the views only read the demand layer" "[.>](flush|runLoadStep|endInputSettleWaits|setInputSettleDelay)\\("
  src/ui "src/mainwindow.*")
# Allow: none expected. Nothing the demand views paint is a control: the base
# classes handle every click and key (tooltips come from helpEvent /
# viewportEvent, which this rule does not name). Say "no event of its own" in
# a comment instead of naming a handler.
expect_none("the demand views handle no event of their own"
  "editorEvent|mouse(Press|Release|DoubleClick|Move)Event|keyPressEvent"
  "src/ui/docks/logbook/LogbookCellDelegate.*" "src/ui/statusbar/StatusBarFeature.*")
# Pending is a presentation of demand: the demand layer answers it and the cell
# delegate paints it; the model, its cached values and index.json never see it.
# Allow: none expected.
expect_only("pending is the view's presentation of demand" "isCellPending|showsPending|pendingText\\(|pendingToolTip\\("
  "^src/${DEMAND_FILES}\\.(cpp|h)$|^src/ui/docks/logbook/LogbookCellDelegate\\.(cpp|h)$" src)
# Allow: none expected. The model knows pinned ids only; the logbook, the
# column store, the plot model and the scheduler know nothing about jobs.
# Comments say "the executor".
expect_none("the model, the logbook and the scheduler know nothing of the executor"
  "JobQueue|JobModel|jobqueue\\.h|jobmodel\\.h"
  "src/sessionmodel.*" "src/logbookmanager.*" "src/logbookcolumn.*" "src/idlescheduler.*" "src/plotmodel.*")
# Allow: none expected. The idle scheduler runs the steps of registered tasks;
# the load step is one more task and tells it nothing.
expect_none("the idle scheduler learns nothing about jobs or demand" "[Dd]emand|[Cc]alculation|[Jj]ob|[Ee]xecutor"
  src/idlescheduler.cpp src/idlescheduler.h)
# The load step is the demand layer's task: the enum names it, the status
# bar maps it to the computations (the fill is never an item of its own), and
# the session model never registers or runs it. Allow: none expected.
expect_only("the load step is the demand layer's scheduler task" "ColumnFillTask"
  "^src/(calculationdemand|demandfill)\\.(cpp|h)$|^src/sessionmodel\\.h$|^src/ui/statusbar/StatusBarFeature\\.cpp$" src)
# Hidden loads for column demand go through the one entry that loads without
# showing and pins under the corrected id. Allow: none expected.
expect_only("hidden loads go through loadPinnedSession" "loadPinnedSession\\("
  "^src/(calculationdemand|demandfill)\\.(cpp|h)$|^src/sessionmodel\\.(cpp|h)$" src)
# The demand layer reads record names only; loading, restoring and reading
# records belong to the session model and the result store. Allow: reword a
# comment that names one of these calls ("loaded the way showing it would").
expect_none("the demand layer never loads a session or reads a record itself"
  "sessionRef\\(|loadSession\\(|readCalculationRecord|calculationRecordIds\\(|restoreSession\\(|restoreStoredResults\\("
  ${DEMAND_LAYER})
# The walk and the pass never load or pin: the fill does, and only it.
# Allow: none expected.
expect_only("the demand layer loads and pins through its fill only"
  "[.>](loadPinnedSession|pinSession|unpinSession)\\(" "^src/demandfill\\.cpp$" ${DEMAND_LAYER})
# The fill and the settle clock never call the executor (the fill learns of
# a shutdown through its owner's hook). Allow: none expected.
expect_none("the fill and the settle clock never call the executor"
  "[.>](offer|withdrawChosenNext|runningJob|chosenNextJob|publishingJob|isShutDown|isIdle|job)\\("
  "src/demandfill.*" "src/demandsettleclock.*")
# They expose nothing of the walk, and present nothing: session ids in,
# session ids and times out. Allow: none expected.
expect_none("the fill and the settle clock know nothing of the walk"
  "BlockerReport|blockers\\(|TrackCondition|RowStabilityGuard|PairMemory|LearnedFact|DemandProgress|SessionFailures|FailedCalculation"
  "src/demandfill.*" "src/demandsettleclock.*")
# One bound of simultaneous jobs, and the load bound follows it: the running
# job's session and the chosen next job's (docs/CALCULATIONS.md 16.8).
# Allow: change the number only together with the executor's run slots.
expect_count("one bound of simultaneous jobs" "kMaxRunningJobs *=[^=]" 1 src)
expect_count("the load bound follows the executor's bound"
  "kMaxHeldSessions *= *JobQueue::kMaxRunningJobs *\\+ *1" 1 src/demandfill.h)
# Allow: none expected. The worker runs below normal priority
# (docs/CALCULATIONS.md 15.5).
expect_count("the worker runs below normal priority" "start\\(QThread::LowPriority\\)" 1 src/jobqueue.cpp)
# The executor is not a queue. Allow: none expected (tests/README.md is not
# searched: its section 10 spells these names).
expect_none("the executor keeps no queue" "oldestQueued|cancelUnwantedQueued|cancelSession\\(|cancelAll\\(|RequestResult"
  src docs README.md)
expect_none("the plot request logic is gone"
  "PlotRequests|plotrequests|plotRequests|PlotRowState|PlotTrackCondition|tst_plot_requests"
  src tests docs cmake CMakeLists.txt README.md ":!tests/README.md")
# No refresh and no cancel for requested calculations anywhere: work follows
# demand (docs/COMPUTED_PLOTS.md section 5).
# Allow: say "there is no refresh and no cancel"; never name the old controls.
expect_none("no refresh or cancel control in code or documents"
  "[Rr]efresh (icon|control|gesture)|press(es|ed|ing)? (the )?refresh|[Cc]ancel (control|icon)|circled x"
  src docs README.md)
expect_none("the plot rows' controls are gone"
  "drawRefreshGlyph|drawCancelGlyph|Control::(Refresh|Cancel)|controlHit|controlCount\\(|controlRect\\("
  src tests ":!tests/README.md")
# Applying a profile that carries a column over a requested output computes it
# for the whole logbook, so no default profile carries one
# (docs/COMPUTED_PLOTS.md section 4). Allow: a
# new requested calculation adds its sensor or attribute names to the pattern.
expect_none("no default profile carries a column over a requested output"
  "\"(sensorID|attributeKey|markerAttributeKey|marker2AttributeKey)\": *\"(Fusion|_FUSION)"
  src/resources/profiles)

# ─────────────────────────────── calculation refinements (items 601-662)
# A column's requested calculations are computed by the registry-side
# authority (logbookColumnExplicitCalculations(), logbookcolumn.*) for two
# owners only: the session model, the one source of each enabled column's
# closure and requested calculations (the demand layer reads
# columnRequestedCalculations() and columnDependencyClosure()), and the
# logbook index, which sits below the model, runs before any model exists
# and decides the validity of cached values over records. The demand layer
# computes a plot's requested calculations and closure from the registry
# (the model knows nothing of plots), one call each; comments name the
# registry's functions as CalculationRegistry::name(). Allow: none expected.
expect_only("one computation of a column's requested calculations: the session model and the index"
  "logbookColumnExplicitCalculations\\("
  "^src/logbookcolumn\\.(cpp|h)$|^src/sessionmodel\\.(cpp|h)$|^src/logbookmanager\\.(cpp|h)$" src)
expect_count("the demand layer asks the registry for a plot's requested calculations only"
  "[.>]explicitDependencies\\(" 1 ${DEMAND_LAYER})
expect_count("the demand layer computes a plot's closure only" "[.>]staticDependencies\\(" 1 ${DEMAND_LAYER})
# One display name of a session: SessionModel::sessionDisplayName(). The
# demand layer and the executor read it and compute none. Allow: none
# expected; a comment says "the display name".
expect_none("one display name of a session: the session model's" "SessionKeys::Description|_DESCRIPTION"
  ${DEMAND_LAYER} "src/jobqueue.*" "src/jobmodel.*")
# The index announces a reason it learns (a record change); the demand
# layer reads reasons only when it looks up a session's record set.
# Allow: none expected.
expect_count("the demand layer reads a record's reason in one place" "[.>]calculationRecordReason\\(" 1
  ${DEMAND_LAYER})
# The demand layer observes no display change of the model: a bulk edit
# reaches it as the dependency change the session model publishes, and
# the column worker's display changes reach nothing. A new fact the demand
# layer needs is announced by its owner (a signal of its own), never
# inferred from dataChanged. Allow: one line, the plot model's check-state
# connection in calculationdemand.cpp (connect(m_plotModel,
# &QAbstractItemModel::dataChanged, ...)); any second "::dataChanged" in
# the demand layer, whatever its variable or line wrapping, trips the count.
# The session model's old slot name, onSessionDataChanged, is forbidden with
# the replaced machinery below.
expect_count("the demand layer observes no display change of the model" "::dataChanged" 1
  ${DEMAND_LAYER})
# One walk over the rows per pass, under one guard. Allow: none expected.
expect_count("the one walk reads the rows under one guard" "RowStabilityGuard +[A-Za-z_]+\\(" 1
  ${DEMAND_LAYER})
# What the one walk and the one pair memory replaced stays gone: the fill's
# ending step, the per-cell memory, the second walk and its column tables,
# the dirty flags, the demand layer's own display name, its display-change
# slot and the fields no view read. Allow: none expected (tests/README.md is
# excluded: section 10 spells these names).
expect_none("the demand layer's replaced machinery stays gone"
  "isFillEnding|m_fillEnding|[Ss]ettlement|m_settled|CellKey|ColumnWalk|walkColumns|ColumnInfo|plotCandidates|inspectUnderGuard|inspectedPlots|syncColumns|m_columnReports|buildState|finishState|rebuildRelevantNames|m_relevantNames|m_columnsDirty|rowDisplayName|onSessionDataChanged|JobFailed|CalculationDemand::(buildToolTip|kToolTipListLimit|kMaxHeldSessions)|\\.(settling|waiting)${WB_END}"
  src tests ":!tests/README.md")
# ─────────────────────────────── one status bar for background work (items 701-754)
# No view keeps a progress label, a cluster of glyphs or an animation clock:
# progress is the status bar's bar, and nothing turns. Allow: none expected
# (tests/README.md is excluded: its section 10 lists these names).
expect_none("no view keeps a label, a cluster or a clock"
  "progressLabel|clusterRect|syncAnimation|WorkingAnimation|followDemand|workingClock|frameAdvanced"
  src tests ":!tests/README.md")
# Each view that holds the demand layer weakly learns of its end itself: the
# logbook's cells turn plain, and the status bar shows no computation and no
# warning. Allow: a new view that holds the demand layer is added.
expect_only("each view that holds the demand layer learns of its end itself" "&QObject::destroyed"
  "^src/ui/docks/logbook/LogbookCellDelegate\\.cpp$|^src/ui/statusbar/StatusBarFeature\\.cpp$" src/ui)
# The demand layer presents progress, failures and the pending cells: the
# per-source state, its track and condition types, its counts, lists and
# tooltip, the queries that returned it, its signals and the lists of working
# ids are gone, and no test reads them either (a test asserts through the
# values and the executor). The logbook cell delegate's own showsWarning(index)
# is not the state's showsWarning(). Allow: none expected (tests/README.md is
# excluded: its section 10 lists these names).
expect_none("the per-source presentation stays gone"
  "DemandState|DemandTrack|DemandCondition|kToolTipListLimit|buildToolTip|workingPlotIds|workingColumnIds|plotStateChanged|columnStateChanged|statesChanged|${WB_START}(plotState|columnState)\\(|sessionIdsOf|wantedCount|doneCount|waitingCount|failedCount|showsWarning\\(\\)"
  src tests ":!tests/README.md")
# Counts that no view reads are not kept: the walk tallies no track per
# source, keeps no list of running or failed tracks and builds no text of its
# own. Allow: none expected.
expect_none("the demand layer keeps no per-source state"
  "m_columnStates|m_states${WB_END}|addTrack|runningCount|jobFailure|calculationTitles|isWorking\\(|isPlain\\(|toolTip${WB_END}"
  ${DEMAND_LAYER})
# The plot rows' and column headers' glyphs, their shared painting, geometry
# and hover helper, and the header view that drew them are gone with the
# per-source presentation. Allow: none expected (tests/README.md is excluded:
# its section 10 lists these names).
expect_none("the per-source indicators and their plumbing stay gone"
  "DemandIndicator|drawWarningGlyph|drawWorkingGlyph|drawDemandGlyph|[Gg]lyphMetrics|glyphColor|showIndicatorToolTip|repaintWhenDemandDestroyed|PlotRowDelegate|PlotRowLayout|layoutPlotRow|LogbookHeaderView|indicatorRect|toolTipForSection"
  src tests ":!tests/README.md")
# One drawing of the warning glyph: the style's standard icon, in the status
# bar and on the logbook row. Allow: none expected; a comment says "the
# style's warning icon".
expect_only("one drawing of the warning glyph: the style's" "SP_MessageBoxWarning"
  "^src/ui/statusbar/StatusBarFeature\\.cpp$|^src/ui/docks/logbook/LogbookCellDelegate\\.cpp$" src)
# The plot list presents nothing of the demand layer: a plot row over a
# requested calculation looks as any other row, with the tree's own
# delegate. Allow: none expected.
expect_none("the plot list presents nothing of the demand layer"
  "[Cc]alculation[Dd]emand|demandstate|DemandState|setItemDelegate" src/ui/docks/plotselection)
# One place shows background work: the status bar names the scheduler's
# tasks and the computations, and it alone follows the scheduler's reports;
# the main window makes it. The logbook presents no task progress, and the
# fill reports none of its own (the status bar shows the computations for
# it). Allow: none expected; a comment names a label without its opening
# quote, and a signal without the class qualifier.
expect_only("the status bar names background work" "\"(Saving|Loading|Updating) sessions|\"Computing (columns|results)"
  "^src/ui/statusbar/StatusBarFeature\\.cpp$" src)
expect_only("the status bar is the one view of the scheduler's tasks" "IdleScheduler::(activeTaskChanged|progressChanged|schedulerIdle)"
  "^src/ui/statusbar/StatusBarFeature\\.cpp$" src)
expect_only("the main window makes the status bar" "new StatusBarFeature" "^src/mainwindow\\.cpp$" src)
expect_none("the logbook presents no task progress" "QProgressBar|IdleScheduler|minimumSizeHint|cancelRequested"
  src/ui/docks/logbook)
expect_none("the fill reports no progress of its own" "Progress\\{|[Hh]igh[-]?[Ww]ater" "src/demandfill.*")
# The documents describe the refined demand layer and the status bar: the
# removed executor API, the per-cell memory, the per-source presentation and
# its indicators, clock and progress line stay out of docs/ and the README.
# Not "badge" alone: DATA_SCHEMA.md says "no badge" of legacy files. Allow:
# say "the pair memory", "a remembered failure", "the scheduler completes
# it", "the status bar", "the row warning", "progress", "failures".
expect_none("the documents describe the refined demand layer"
  "jobQueued|activeJobs\\(|${WB_START}idle\\(\\)|[Ss]ettlement|progressLabel|isFillEnding|one clock per view|lose work only by stepping|under the same label|arc with \"k of n\"|triangle with a number|CalculationDemand::(kMaxHeldSessions|kToolTipListLimit|buildToolTip)|plotState|columnState|workingPlotIds|workingColumnIds|statesChanged|DemandState|DemandTrack|DemandCondition|buildToolTip|kToolTipListLimit|jobFailure|showsWarning|isPlain|isWorking\\(|WorkingAnimation|followDemand|workingClock|[Ww]orking[- ]indicator|DemandIndicator|PlotRowDelegate|PlotRowLayout|LogbookHeaderView|glyphMetrics|drawDemandGlyph|showIndicatorToolTip|repaintWhenDemandDestroyed|progress line|[Ww]arning badge|badges|turning arc"
  docs README.md)

# ─────────────────────────────── leftover markers
expect_none("leftover markers" "BASELINE:|PHASE4-SWITCH" tests src)

# ─────────────────────────────── acceptance traceability
# tests/acceptance_map.txt: items 1-19 (the schema / engine specification),
# 101-120 (sensor fusion and plot-driven jobs, item = 100 + acceptance number),
# 201-247 (the sensor fusion improvements, item = 200 + requirement
# number), 301-350 (storing requested calculation results with the session,
# item = 300 + clause number), 401-442 (stored results: validity that
# mirrors memory, item = 400 + clause number), 501-563 (demand-driven
# requested calculations, item = 500 + clause number), 601-662
# (calculation refinements, item = 600 + clause number), 701-754 (one
# status bar for background work, item = 700 + clause number) and 801-863
# (sensor fusion plots, attitude and the orientation attribute, item = 800 +
# clause number). Four line forms; see the head of the map.
math(EXPR RULES "${RULES} + 1")
set(map_file "${REPO}/tests/acceptance_map.txt")
if(NOT EXISTS "${map_file}")
  _violation("[traceability] tests/acceptance_map.txt is missing")
else()
  get_property(audit_groups GLOBAL PROPERTY AUDIT_GROUPS)
  set(manual_file "${REPO}/tests/README.md")
  set(workflow_file "${REPO}/.github/workflows/build.yml")
  set(manual_text "")
  set(workflow_text "")
  if(EXISTS "${manual_file}")
    file(READ "${manual_file}" manual_text)
  endif()
  if(EXISTS "${workflow_file}")
    file(READ "${workflow_file}" workflow_text)
  endif()

  file(STRINGS "${map_file}" map_lines)
  set(items_seen "")        # every item with a line of any kind
  set(items_automated "")   # items with at least one test or audit line
  foreach(line IN LISTS map_lines)
    string(STRIP "${line}" line)
    if(line STREQUAL "" OR line MATCHES "^#")
      continue()
    endif()

    if(line MATCHES "^([0-9]+) +manual +M([0-9]+)$")
      set(item "${CMAKE_MATCH_1}")
      string(FIND "${manual_text}" "**M${CMAKE_MATCH_2} " position)
      if(position EQUAL -1)
        _violation("[traceability] item ${item}: tests/README.md has no manual step **M${CMAKE_MATCH_2}")
      endif()
    elseif(line MATCHES "^([0-9]+) +ci +([^ ]+)$")
      set(item "${CMAKE_MATCH_1}")
      string(FIND "${workflow_text}" "${CMAKE_MATCH_2}" position)
      if(position EQUAL -1)
        _violation("[traceability] item ${item}: .github/workflows/build.yml does not contain '${CMAKE_MATCH_2}'")
      endif()
    elseif(line MATCHES "^([0-9]+) +audit +([a-z-]+)$")
      set(item "${CMAKE_MATCH_1}")
      list(FIND audit_groups "${CMAKE_MATCH_2}" index)
      if(index EQUAL -1)
        _violation("[traceability] item ${item}: no audit rule group '${CMAKE_MATCH_2}' (groups: ${audit_groups})")
      else()
        list(APPEND items_automated "${item}")
      endif()
    elseif(line MATCHES "^([0-9]+) +(tst_[a-z_]+) +([A-Za-z_0-9]+)$")
      set(item "${CMAKE_MATCH_1}")
      set(target "${CMAKE_MATCH_2}")
      set(function "${CMAKE_MATCH_3}")
      set(source "${REPO}/tests/${target}.cpp")
      if(NOT EXISTS "${source}")
        _violation("[traceability] item ${item}: tests/${target}.cpp does not exist")
      else()
        file(READ "${source}" text)
        string(FIND "${text}" "::${function}()" position)
        if(position EQUAL -1)
          _violation("[traceability] item ${item}: tests/${target}.cpp has no test function ${function}()")
        else()
          list(APPEND items_automated "${item}")
        endif()
      endif()
    else()
      _violation("[traceability] malformed line: ${line}")
      continue()
    endif()

    list(APPEND items_seen "${item}")
    if(NOT ((item GREATER_EQUAL 1 AND item LESS_EQUAL 19) OR (item GREATER_EQUAL 101 AND item LESS_EQUAL 120)
            OR (item GREATER_EQUAL 201 AND item LESS_EQUAL 247) OR (item GREATER_EQUAL 301 AND item LESS_EQUAL 350)
            OR (item GREATER_EQUAL 401 AND item LESS_EQUAL 442) OR (item GREATER_EQUAL 501 AND item LESS_EQUAL 563)
            OR (item GREATER_EQUAL 601 AND item LESS_EQUAL 662) OR (item GREATER_EQUAL 701 AND item LESS_EQUAL 754)
            OR (item GREATER_EQUAL 801 AND item LESS_EQUAL 863)))
      _violation("[traceability] item ${item} is outside 1-19, 101-120, 201-247, 301-350, 401-442, 501-563, 601-662, 701-754 and 801-863: ${line}")
    endif()
  endforeach()

  foreach(item RANGE 1 19)
    list(FIND items_seen "${item}" index)
    if(index EQUAL -1)
      _violation("[traceability] acceptance item ${item} has no line in tests/acceptance_map.txt")
    endif()
  endforeach()
  # Manual and ci lines never stand alone
  foreach(item RANGE 101 120)
    list(FIND items_automated "${item}" index)
    if(index EQUAL -1)
      _violation("[traceability] acceptance item ${item} has no resolving test or audit line in tests/acceptance_map.txt")
    endif()
  endforeach()
  foreach(item RANGE 201 247)
    list(FIND items_automated "${item}" index)
    if(index EQUAL -1)
      _violation("[traceability] acceptance item ${item} has no resolving test or audit line in tests/acceptance_map.txt")
    endif()
  endforeach()
  foreach(item RANGE 301 350)
    list(FIND items_automated "${item}" index)
    if(index EQUAL -1)
      _violation("[traceability] acceptance item ${item} has no resolving test or audit line in tests/acceptance_map.txt")
    endif()
  endforeach()
  foreach(item RANGE 401 442)
    list(FIND items_automated "${item}" index)
    if(index EQUAL -1)
      _violation("[traceability] acceptance item ${item} has no resolving test or audit line in tests/acceptance_map.txt")
    endif()
  endforeach()
  foreach(item RANGE 501 563)
    list(FIND items_automated "${item}" index)
    if(index EQUAL -1)
      _violation("[traceability] acceptance item ${item} has no resolving test or audit line in tests/acceptance_map.txt")
    endif()
  endforeach()
  foreach(item RANGE 601 662)
    list(FIND items_automated "${item}" index)
    if(index EQUAL -1)
      _violation("[traceability] acceptance item ${item} has no resolving test or audit line in tests/acceptance_map.txt")
    endif()
  endforeach()
  foreach(item RANGE 701 754)
    list(FIND items_automated "${item}" index)
    if(index EQUAL -1)
      _violation("[traceability] acceptance item ${item} has no resolving test or audit line in tests/acceptance_map.txt")
    endif()
  endforeach()
  foreach(item RANGE 801 863)
    list(FIND items_automated "${item}" index)
    if(index EQUAL -1)
      _violation("[traceability] acceptance item ${item} has no resolving test or audit line in tests/acceptance_map.txt")
    endif()
  endforeach()
endif()

# ─────────────────────────────── verdict
if(VIOLATIONS)
  message(FATAL_ERROR "cleanup audit FAILED (${RULES} rules):${VIOLATIONS}")
endif()
message(STATUS "cleanup audit passed (${RULES} rules)")
