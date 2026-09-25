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
#     kernel, gestures only from the plot list's row delegate, a widget-free
#     core; the stationary-window initializer, its silent poll and the
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
#     results stale (items 401-442).
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

# audit_group(<slug>): the rules that follow belong to this group (see the header)
function(audit_group slug)
  set_property(GLOBAL APPEND PROPERTY AUDIT_GROUPS "${slug}")
endfunction()

# ─────────────────────────────── old engine
# m_sideEffectFrames: the sibling-frame scaffolding of sensor-fusion-clean-port
# (seventeen per-output registrations writing each other's results).
expect_none("old engine"
  "CalculatedValue\\b|calculatedvalue\\.|DependencyManager|dependencymanager|calculatedvalueregistry|CalculatedValueRegistry|m_sideEffectKeys|m_sideEffectFrames|m_activeCalculations|toDependencyKey"
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

# The logic of background work: the job queue, its model, the plot request
# component, and the fusion library.
set(FUSION_CORE "src/jobqueue.*" "src/jobmodel.*" "src/plotrequests.*" src/fusion)

# ─────────────────────────────── branch-mechanisms (acceptance 120)
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
expect_none("jobs never touch the idle scheduler" "[Ii]dle[Ss]cheduler" ${FUSION_CORE})
# m_pendingRebuildLevel is master's own and is not part of this rule.
expect_none("no plot rebuild guard" "m_rebuildingPlot|QScopedValueRollback" src/ui/docks/plot)
# Allow: none expected. A calculation outcome is reported by the plot row
# (badge and tooltip), never by a dialog, a message box or the status bar.
expect_none("no dialog or message box for a calculation outcome"
  "QMessageBox|QProgressDialog|QDialog|QErrorMessage|statusBar\\("
  ${FUSION_CORE} src/engine src/calculations src/ui/docks/plotselection)
# A rejection and a solver failure are results of the compute function, cached
# by the engine like any other; the registration never catches and stores one.
expect_none("no hand-cached failure" "catch *\\(" src/fusion/fusionregistration.cpp)

# ─────────────────────────────── naming
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
# Allow: these two counts pin the application's plot list to the list the
# tests use (tests/fusion/fusionsessions.cpp, fusionPlots()). Adding a plot
# means changing both, and the number here.
expect_count("seventeen fusion plots" "^ *\\{\"Sensor fusion\", " 17 src/mainwindow.cpp)
expect_count("six local-frame plots" "^ *\\{\"GNSS \\(Local frame\\)\", " 6 src/mainwindow.cpp)

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
expect_none("public and registration files are GTSAM-free" "#include <(gtsam|Eigen)"
  src/fusion/fusion.h src/fusion/fusionregistration.h src/fusion/fusionregistration.cpp)
# Allow: only the registration adapter may see the engine and the session keys.
expect_none("the kernel is pure"
  "#include [<\"](sessiondata|sessionmodel|engine/|jobqueue|plotrequests|preferences/|QApplication|QWidget|QtWidgets|QtGui)"
  src/fusion ":!src/fusion/fusionregistration.cpp" ":!src/fusion/fusionregistration.h")
expect_none("the kernel does not log" "qWarning|qInfo|qDebug|qCritical" src/fusion)
# flysight_core never references the fusion library; the application calls its
# one entry point next to the built-in registration.
expect_none("nobody but the application references the fusion library" "fusion/|Fusion::"
  src/calculations src/engine "src/sessionmodel.*" "src/jobqueue.*" "src/jobmodel.*" "src/plotrequests.*")
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

# ─────────────────────────────── gestures (acceptance 116)
# Only a gesture starts expensive work, and a gesture is an explicit call from
# the plot list's row delegate. MainWindow cannot be constructed in the test
# harness, so that nothing in it (start-up restore, profiles, the Plots menu)
# calls these is a text rule.
audit_group(gestures)
expect_only("gestures come from the row delegate only" "plotCheckedByUser|refreshPressed|cancelPressed"
  "^src/plotrequests\\.(h|cpp)$|^src/ui/docks/plotselection/PlotRowDelegate\\.(h|cpp)$" src)
# The opening parenthesis directly after the name keeps publishInvalidation(,
# publishEdges( and publishCalculationInvalidation( out of this rule.
expect_only("explicit work is prepared and published in one place" "[.>]prepare\\(|[.>]publish\\("
  "^src/jobqueue\\.cpp$|^src/engine/" src)
expect_only("no reader requests" "[.>]request\\("
  "^src/plotrequests\\.cpp$|^src/jobqueue\\.(cpp|h)$|^src/engine/" src)
# CalculationEngine::request() is the SYNCHRONOUS request: it runs the explicit
# calculation on the calling thread, which in the application is the GUI
# thread. Product code never calls it (tests do, and the engine's own files
# name it); explicit work runs only as a job, through JobQueue::request().
# The first rule names the ways src spells a session's engine
# (calculationEngine().request(, engine.request(, m_engine->request( ...) and
# does not match m_jobQueue->request(. The second closes the door on an alias
# (`auto &e = session.calculationEngine(); e.request(`): outside src/engine
# exactly one line calls any request( through an object, the job request in
# PlotRequests::requestTracks().
# Allow: none expected for the first rule. The count changes only when a second
# legitimate caller of JobQueue::request() appears, which is itself a design
# change (plotrequests.h: "the only caller").
expect_none("no synchronous explicit request in product code"
  "[Ee]ngine(\\(\\))? *(\\.|->) *request\\(" src ":!src/engine")
expect_count("one call of request( in product code: the job request" "[.>]request\\(" 1 src ":!src/engine")
expect_only("one call of request( in product code: the job request" "[.>]request\\("
  "^src/plotrequests\\.cpp$" src ":!src/engine")
# Allow: a future jobs dock is a pure view of JobQueue::model() and is added to
# the regex when it exists. Until then AppContext only carries the pointer.
expect_only("no jobs window, no view of the queue" "[Jj]ob[Qq]ueue|JobModel"
  "^src/ui/docks/AppContext\\.h$" src/ui)
# CalculationRegistry::explicitDependencies() is the one definition of
# "explicit-backed" (dependsOnExplicit() is its non-emptiness): plot rows ask
# dependsOnExplicit(), the logbook column cache asks explicitDependencies()
# through logbookColumnExplicitCalculations(). Neither tests the policy
# itself, and neither does the result store: what may be stored is decided by
# CalculationEngine::exportResult().
expect_only("one authority: explicit-backed" "EvaluationPolicy::Explicit"
  "^src/engine/|^src/fusion/fusionregistration\\.cpp$" src)

# ─────────────────────────────── widget-free-core
audit_group(widget-free-core)
expect_none("the logic components see no widget"
  "QtWidgets|#include <Q(Widget|TreeView|AbstractItemView|StyledItemDelegate|Application|ToolTip)>"
  "src/jobqueue.*" "src/jobmodel.*" "src/plotrequests.*" "src/plotmodel.*"
  src/ui/docks/plotselection/PlotRowLayout.h)

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
# patterns. `kWindowLength\b` and not `kWindowLength`: the kernel's
# kWindowLengthsMessage (fusionsamples.cpp) is a rejection reason, not a gate.
expect_none("the stationary-window detector is gone"
  "stationarywindow|StationaryWindow|assessStationaryWindow|bestStationaryWindow|kMaxMeanRate|imuGapLimit|kWindowLength\\b|kWindowGrid"
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
# Allow: none expected. The fusion document describes the model as it is:
# no stationary or candidate window, no coarse-only initializer, no frozen
# algorithm, no bias-shift settled test, no branch, twenty-two inputs. Say
# "the previous initializer", "a resting window", "the coarse attitude at the
# anchor" when the history must be mentioned.
expect_none("the fusion document describes the current model"
  "stationary window|candidate window|coarse initializer|frozen|bias shifts below|zero bias shift|sensor-fusion-clean-port|twenty-one"
  docs/SENSOR_FUSION.md)

# ─────────────────────────────── fusion-tooling (items 231, 233)
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
  "\\b(write|read|remove)CalculationRecords?\\("
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
expect_none("restoring is not requesting" "JobQueue|PlotRequests|[.>](request|prepare|publish)\\("
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

# ─────────────────────────────── leftover markers
expect_none("leftover markers" "BASELINE:|PHASE4-SWITCH" tests src)

# ─────────────────────────────── acceptance traceability
# tests/acceptance_map.txt: items 1-19 (the schema / engine specification),
# 101-120 (sensor fusion and plot-driven jobs, item = 100 + acceptance number),
# 201-247 (the sensor fusion improvements, item = 200 + requirement
# number), 301-350 (storing requested calculation results with the session,
# item = 300 + clause number) and 401-442 (stored results: validity that
# mirrors memory, item = 400 + clause number). Four line forms; see the head of
# the map.
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
            OR (item GREATER_EQUAL 401 AND item LESS_EQUAL 442)))
      _violation("[traceability] item ${item} is outside 1-19, 101-120, 201-247, 301-350 and 401-442: ${line}")
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
endif()

# ─────────────────────────────── verdict
if(VIOLATIONS)
  message(FATAL_ERROR "cleanup audit FAILED (${RULES} rules):${VIOLATIONS}")
endif()
message(STATUS "cleanup audit passed (${RULES} rules)")
