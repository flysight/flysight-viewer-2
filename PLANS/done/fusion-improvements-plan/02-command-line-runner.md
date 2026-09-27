# Phase 2: Command-line runner

## Overview

`fusion_runner` is a command-line program, built in the fusion-tests block and never shipped, that imports one recording's `TRACK.CSV` and `SENSOR.CSV` exactly as the application does (same parser, same conversion layer, same on-demand calculations), hands the kernel the channels the job queue would, prints the fit's diagnostics JSON on stdout, optionally writes the seventeen output channels as CSV, streams progress to stderr, and exits 0 only for `Succeeded` (spec section 8). It exists so that Michael can repeat the corpus comparison of spec section 12 on the product kernel after Phases 5 and 6, without the GUI, the logbook or the user's settings.

The phase has two halves: a small public addition to the fusion registration so that the tool and the engine assemble the kernel's input through one function, and the tool with its QtTest. Nothing numerical changes.

## Dependencies

- **Depends on:** None — can begin immediately.
- **Blocks:** None (Phase 7 documents the tool; Michael's manual corpus comparison uses it after Phases 5 and 6).
- **Assumptions:**
  - `master` as of the overview: `src/fusion/fusionregistration.cpp` holds `kFitInputs[]` (lines 62-80), `fitInputs()` (86-96), `channelsFrom(ctx)` (109-124), `kFitOutputs[]` (33-51) and `computeFit()` (147-164).
  - Phase 1 is documented and implemented in parallel and appends its own executable (`fusion_golden_capture`) to the same fusion block of `tests/CMakeLists.txt` and, since it links `flysight_fusion_test_support`, to the same `_FLYSIGHT_GTSAM_REACHERS` list of `cmake/SolverDependencies.cmake`. Both phases append; neither reorders (see Gotchas).
  - Verification build: `build-phase1/` (overview, "Verification commands"). Never rebuild `build/`.

## Tasks

### Task 2.1: Expose the fit's input table, channel assembly and output table from the registration header

**Purpose:** The runner must feed the kernel exactly what `computeFit()` feeds it, by construction rather than by copy: one table, one assembly function, used by the engine adapter and by the tool. Phase 6 adds `IMU/temperature` to `kFitInputs[]`; with this design that addition reaches the runner (its fit and its `--dump-inputs`) without a runner change.

**Files to modify:**
- `src/fusion/fusionregistration.h` — add three public declarations (below) and `#include "fusion/fusion.h"`, `<functional>`, `<QList>`, `<QString>`, `<QVariant>`, `<QVector>`; extend the header comment (the "only function of this library the application and the tests call" sentence now names the fit-registration function as the application's entry point and the three helpers as what the tests' tooling uses).
- `src/fusion/fusionregistration.cpp` — move `fitInputs()` out of the anonymous namespace into `Fusion::`; implement the reader-based `channelsFrom` as the one assembly; reduce the context overload to a two-line wrapper; add `fitOutputChannels()` over `kFitOutputs[]`.

**Technical Approach:**

Public API, in `namespace FlySight::Fusion`, next to `FitCalculationId`:

```cpp
/// The declared inputs of builtin.fusion.fit: the measurements, in the order
/// the kernel's Channels take them, then the four origin attributes
/// (_LOCAL_ORIGIN_INDEX, _LAT, _LON, _HMSL). One table serves the declaration,
/// the hand-over and the tooling (fusion_runner --dump-inputs).
QList<CalcInput> fitInputs();

/// Effective values, as any reader serves them.
using MeasurementReader = std::function<QVector<double>(const QString &sensor, const QString &name)>;
using AttributeReader   = std::function<QVariant(const QString &key)>;

/// The kernel's input assembled from effective values: every measurement of
/// fitInputs() into its Channels member, and the origin attributes (an origin
/// index that is not a number becomes -1, the kernel's "outside the GNSS
/// samples"). Field-by-field copies of implicitly shared vectors. The
/// registered calculation and fusion_runner both call this, so the two cannot
/// drift apart.
Channels channelsFrom(const MeasurementReader &measurement, const AttributeReader &attribute);

/// The seventeen measurement outputs of the fit in publication order, each
/// named as it is published under the Fusion sensor (_time, north, ..., qw),
/// with the array of `result` that holds it (empty arrays unless Succeeded).
struct FitOutputChannel {
    QString name;
    QVector<double> samples;
};
QList<FitOutputChannel> fitOutputChannels(const Result &result);
```

In the `.cpp`:
- `kFitInputs[]`, `FitInput`, `kFitOutputs[]`, `FitOutput` stay in the anonymous namespace, untouched.
- `Fusion::fitInputs()` is the existing function (86-96), now public; keep its comment ("Markers and preferences are not inputs").
- `Fusion::channelsFrom(measurement, attribute)` is the body of today's `channelsFrom(ctx)` (109-124) with `ctx.measurement(...)` replaced by `measurement(...)` and `ctx.attribute(...)` by `attribute(...)`, including the `isNumber ? originIndex : -1` rule and its comment.
- The anonymous-namespace `channelsFrom(const EvaluationContext &ctx)` becomes:
  ```cpp
  return Fusion::channelsFrom(
      [&ctx](const QString &sensor, const QString &name) { return ctx.measurement(sensor, name); },
      [&ctx](const QString &key) { return ctx.attribute(key); });
  ```
  `computeFit()` is otherwise unchanged (it still calls the context overload).
- `Fusion::fitOutputChannels(result)` iterates `kFitOutputs[]` and returns `{ QString::fromLatin1(output.name), result.*(output.samples) }` per row. `publish()` keeps iterating the table directly; do not reroute it.
- Keep the "no hand-cached failure" rule: no `catch` appears in this file (audit group `branch-mechanisms`); no GTSAM or Eigen include appears in either file (audit group `solver-confinement`); the header keeps including only engine and public fusion headers.

**Acceptance Criteria:**
- [ ] `fusionregistration.h` declares `fitInputs()`, `MeasurementReader`, `AttributeReader`, `channelsFrom(const MeasurementReader &, const AttributeReader &)`, `FitOutputChannel` and `fitOutputChannels(const Result &)` in `FlySight::Fusion`, and includes no header outside `engine/`, `fusion/fusion.h` and Qt Core / the standard library.
- [ ] `fusionregistration.cpp` has exactly one place that maps a `kFitInputs[]` row to a `Channels` member (the public `channelsFrom`); the context overload delegates to it; `computeFit()`'s behavior is unchanged.
- [ ] `fitInputs()` returns 21 entries on `master`: 17 measurements in `kFitInputs[]` order, then the four origin attributes in the order index, lat, lon, hmsl.
- [ ] `fitOutputChannels(result)` returns 17 entries named `_time, north, east, down, velN, velE, velD, accN, accE, accD, roll, pitch, yaw, qx, qy, qz, qw` in that order, each sharing the buffer of the corresponding `Result` array (the test checks `sameBitsEverywhere` against `fusionChannel(result, name)` of `tests/fusion/fusiongolden.h`).
- [ ] `tst_fusion_session`, `tst_fusion_jobs`, `tst_fusion_rows` and `tst_fusion_parity` pass unchanged (`registrationShape` still sees the three fusion ids last, in order).
- [ ] `audit_cleanup` passes.

**Complexity:** M

---

### Task 2.2: The runner's command line, process isolation and recording import

**Purpose:** A `QCoreApplication` process that reads its arguments, refuses bad invocations with a usage text, keeps the user's settings and logbook untouched by construction, and imports the two files into one `SessionData` the way `SessionModel::mergeSessions` does for a new session — without the model.

**Files to create:**
- `tests/fusion_runner.cpp` — the whole tool (one translation unit, like `tests/solver_deploy_probe.cpp`); this task writes `main()`, the argument handling, the isolation and `importRecording()`; Task 2.3 adds the fit and the outputs.

**Technical Approach:**

*Process shape.* `int main(int argc, char **argv)`: construct `QCoreApplication app(argc, argv)`; set `QCoreApplication::setOrganizationName("FlySightTools")`, `setOrganizationDomain("tools.flysight.invalid")`, `setApplicationName("fusion_runner")`; then, as a tripwire against any future `flysight_core` change, redirect default `QSettings` into a `QTemporaryDir` declared in `main()`'s scope: `QSettings::setDefaultFormat(QSettings::IniFormat)` and `QSettings::setPath(IniFormat, UserScope | SystemScope, <tempdir>/settings)` — the three calls of `tests/support/testenvironment.cpp` lines 151-153 (do not copy the `qFatal` probe). The guarantee itself is structural: the tool never references `PreferencesManager`, `LogbookManager`, `EnginePreferenceProvider`, `SessionModel` or `SessionImport`, so no `QSettings` object is ever constructed and the logbook folder is never resolved; the redirect only ensures that an accidental one could land nowhere but in a directory removed at exit. Install a Qt message handler that writes every Qt log message to stderr as one line prefixed `# ` (so a warning can never be mistaken for a progress text, and Windows never diverts it to the debugger).

*Registration.* Exactly the application's order (`src/mainwindow.cpp` lines 170-175): `FlySight::registerBuiltInCalculations();` then `FlySight::Fusion::registerFusionCalculations();`. Nothing else: no `registerBuiltInAttributes`, no `registerBuiltInCalculationMetadata`, no `EnginePreferenceProvider::install()`, no plugins. Without a preference provider the engine treats a preference input as unavailable (`src/engine/calculationengine.cpp` 520-527); no calculation on the fit's input path declares one (the only `CalcInput::preference` in `src/calculations` is `builtin.attr.analysisRange`, `attributecalculations.cpp:195`), which Task 2.5 pins with a test.

*Arguments.* Use `QCommandLineParser` with `parse(app.arguments())` — never `process()`, `showHelp()` or `showVersion()`, which choose the stream and exit 0. Options: `--csv <path>`, `--dump-inputs <path>`, `-h`/`--help`. Positional: either one folder or two file paths. Resolution: one positional → `<folder>/TRACK.CSV` and `<folder>/SENSOR.CSV` (exactly these names, upper case as the device writes them); two positionals → used as given, first is the track file, second the sensor file. Usage errors (unknown option, missing option value, zero or more than two positionals, a positional that is neither a directory nor a file) print one `error: ...` line followed by the usage text on stderr and exit 64. `--help` prints the usage text on stdout and exits 64 (Decisions Made). The usage text (write it as one literal; the test looks for the word `Usage:`):

```
Usage: fusion_runner [options] <folder>
       fusion_runner [options] <TRACK.CSV> <SENSOR.CSV>

Runs the sensor fusion fit on one recording, imported as FlySight Viewer
imports it, and prints the fit's diagnostics JSON on standard output.
Progress goes to standard error, one line per stage.

Options:
  --csv <path>          write the seventeen output channels as CSV (Succeeded only)
  --dump-inputs <path>  write the effective input channels the fit is given
  -h, --help            this text

Exit status: 0 Succeeded, 1 Rejected, 2 SolverFailed, 3 the recording could
not be imported, 4 an output file could not be written, 5 internal error,
64 usage error.
```

*Import* — `importRecording(const QStringList &paths, SessionData &session, QString &error)` (a function in the file; returns false with `error` set):
1. For each path, a fresh `DataImporter` (its error belongs to that file) and `ParsedFile file; importer.parseFile(path, file)`; on failure `error = path + ": " + importer.getLastError()`. This is what `SessionImport::importFiles` does (`src/sessionimport.cpp` 52-60).
2. All parsed files must carry the same match id (`file.sessionId`): the application would otherwise create two sessions and the fit would never see both files. Otherwise `error = "<path2> belongs to session '<id2>', <path1> to '<id1>'"`.
3. The session is the first file's data (`session = std::move(files[0].data)`), as `SessionModel::mergeSessions` creates a row from `file.data` (`src/sessionmodel.cpp` 601-612) — **without** `DataImporter::applyCreationDefaults()`: that function reads `PreferencesManager` (`src/dataimporter.cpp` 237) and writes only `SESSION_ID`, `DEVICE_ID`, `_DESCRIPTION`, `_IMPORT_TIME`, `_WIND_N/E`, `_JUMPER_MASS`, `_PLANFORM_AREA`, `_GROUND_ELEV`, none of which is an input of the fit or of any calculation the fit's inputs depend on (`fitInputs()`; `builtin.local.coordinates` reads GNSS only, `localcoordinatecalculations.cpp` 115-123; `builtin.time.utc.*` reads the sensor's `time` and the TIME fit, `timecalculations.cpp` 116-136).
4. Each further file: `const MergePlan plan = SessionMerge::plan(session, file.data)` (`src/sessionmerge.h` 65); `!plan.ok()` → `error = path + ": " + plan.errorWithHint()`; an empty plan is fine (nothing to add); otherwise `SessionMerge::apply(session, plan)`. This is the loaded-session branch of `mergeSessions` (`src/sessionmodel.cpp` 614-626).

The session's engine binds to `CalculationRegistry::instance()` on first read (`SessionData::calculationEngine()`), which is why the registrations come first.

**Acceptance Criteria:**
- [ ] `tests/fusion_runner.cpp` contains none of the identifiers `PreferencesManager`, `LogbookManager`, `EnginePreferenceProvider`, `SessionModel`, `SessionImport`, `applyCreationDefaults`, `JobQueue`; `QSettings` appears only in the redirect block; the only headers it includes from `src/` are `dataimporter.h`, `parsedfile.h`, `sessiondata.h`, `sessionmerge.h`, `csvformat.h`, `calculations/builtincalculations.h`, `engine/calctypes.h` (or `engine/calculationregistry.h`), `fusion/fusion.h` and `fusion/fusionregistration.h`.
- [ ] No GTSAM or Eigen header is included (audit rule "GTSAM headers: kernel and its tests only" stays green without editing its regex).
- [ ] `fusion_runner --help` prints a text containing `Usage:` on stdout and exits 64; `fusion_runner` with no arguments, with three positionals, or with `--bogus` prints `error:` and the usage on stderr and exits 64.
- [ ] A folder without `TRACK.CSV` or `SENSOR.CSV`, or an unparseable file, exits 3 with `<path>: <DataImporter error text>` on stderr (e.g. `Missing $DATA section`); two files with different `SESSION_ID`s exit 3 with a message naming both ids; a merge conflict exits 3 with `MergePlan::errorWithHint()`.
- [ ] Registration order is built-ins then fusion, once per process.

**Complexity:** M

---

### Task 2.3: The fit, its outputs and the exit status

**Purpose:** Run the kernel on the assembled channels with progress on stderr, print the diagnostics, write the optional files, and map the outcome to the exit status.

**Files to modify:**
- `tests/fusion_runner.cpp` — the second half of `main()`: `--dump-inputs`, channel assembly, `Fusion::run()`, stdout, `--csv`, exit codes.

**Technical Approach:**

*Order of operations after a successful import:*
1. `--dump-inputs <path>` (if given) is written **before** the fit, whatever the fit will say: one line per entry of `Fusion::fitInputs()`, in that order. A Measurement entry: `<sensor>/<name>` followed by `,` and the samples of `session.getMeasurement(sensor, name)` each formatted with `CsvFormat::formatDouble`, comma-separated (a measurement that is unavailable gives the label alone). An Attribute entry: `<key>,` followed by `CsvFormat::formatAttributeValue(session.getAttribute(key)).value_or(QString())`. UTF-8, `\n` line endings, written with `QFile` opened `WriteOnly | Truncate`. A write failure exits 4 with `error: could not write <path>: <QFile::errorString()>`. This is the tooling view of the table Task 2.1 exposes, so Phase 6's `IMU/temperature` appears in it automatically.
2. Channels: `Fusion::channelsFrom([&](sensor, name){ return session.getMeasurement(sensor, name); }, [&](key){ return session.getAttribute(key); })`. Effective values through `SessionData::getMeasurement` are the engine's resolution (`src/sessiondata.cpp` 143-148), the same values an `EvaluationContext` is filled from at prepare time, so this is what `computeFit()` gets. The runner does not reproduce the engine's availability gate: a recording missing a sensor gets empty channels and the kernel's own `Rejected` (Decisions Made).
3. `Fusion::run(channels, progress, {})` on the main thread (hence the 64 MiB stack of Task 2.4), with `progress = [](const QString &text) { <stderr> << text << '\n'; flush; }` and no cancel function (cancellation is not required; Ctrl-C ends the process). Wrap the call in `try { ... } catch (const std::exception &e)` → `error: <what()>` on stderr, exit 5 (this is `std::bad_alloc`, the one thing the kernel lets through; `fusion.h` 79-83).
4. stdout: `result.diagnosticsJson` verbatim followed by one `\n`, through a `QTextStream` on `stdout` (UTF-8), flushed. Nothing else ever goes to stdout after the arguments were accepted.
5. `--csv <path>` (if given) is written only for `Succeeded`: header line `_time,north,east,down,velN,velE,velD,accN,accE,accD,roll,pitch,yaw,qx,qy,qz,qw` (the names of `Fusion::fitOutputChannels()` joined by `,`, so the header is derived, not typed), then one line per sample with the seventeen values formatted by `CsvFormat::formatDouble` — shortest round-trip text, `.` decimal point whatever the locale, `nan`/`inf` for non-finite — comma-separated, `\n`, UTF-8, `QFile`. Not a `$FLYS` file: no `$` prefixes, no `$UNIT`. For any other outcome the path is not created or touched. A write failure exits 4.
6. Final stderr line, every outcome: `Succeeded`, `Rejected: <reason>` or `SolverFailed: <reason>` (`Result::reason`, which is also the diagnostics' `failure`). `Outcome::Cancelled` cannot occur without a cancel function; treat it as `error: cancelled without a cancel function`, exit 5.
7. Exit code: `Succeeded` 0, `Rejected` 1, `SolverFailed` 2 (3, 4, 5 and 64 as above). Define them as one `enum` at the top of the file with a comment that the usage text repeats them.

Stream discipline: stderr carries, in order, any `# `-prefixed Qt messages, the progress texts one per line, and the final outcome line; the test relies on exactly this (Task 2.5). Flush after every line so a long fit streams. On Windows the C runtime writes `\r\n`; readers must tolerate it.

**Acceptance Criteria:**
- [ ] On a success the stdout is exactly `Result::diagnosticsJson` + newline, and stderr's lines are the kernel's progress texts, in order, followed by `Succeeded`; exit 0.
- [ ] On a rejection stdout is the failure JSON (`{"algorithm":..., "failure":...}`), stderr ends with `Rejected: <reason>`, exit 1; a `--csv` path given with it does not exist afterwards.
- [ ] `--csv` writes the derived header line and `Result::time.size()` rows whose values reload bit-identically through `CsvFormat::parseDouble`.
- [ ] `--dump-inputs` writes one line per `fitInputs()` entry, in order, labelled `<sensor>/<name>` or `<key>`, values through `CsvFormat::formatDouble` / `formatAttributeValue`, and is written even when the fit is then rejected.
- [ ] Numbers are formatted only through `CsvFormat` (no `QString::number(double)`, `'g', 17`, `std::to_chars`, `QLocale`, `FloatingPointShortest` in the file).
- [ ] A `std::exception` out of the fit exits 5 with `error: ` on stderr; exit codes 0-5 and 64 are the only codes the program returns.

**Complexity:** M

---

### Task 2.4: Build targets and the GTSAM confinement lists

**Purpose:** Define the tool and its test in the fusion block, give the tool the solver stack, let the test find the tool, and register both with the configure-time link-confinement check, which otherwise fails the configure.

**Files to modify:**
- `tests/CMakeLists.txt` — the `FLYSIGHT_BUILD_FUSION_TESTS` block, after `solver_deploy_probe` (lines 439-443).
- `cmake/SolverDependencies.cmake` — `_FLYSIGHT_GTSAM_REACHERS` (lines 526-530).

**Technical Approach:**

In the fusion block, after the `solver_deploy_probe` lines (so that the block still ends with non-test executables; Phase 1 appends `fusion_golden_capture` in the same place — append after whichever is there):

```cmake
# Command-line runner: the fit on one recording imported as the application
# imports it (spec section 8). Not a test, not installed; run by hand with the
# Qt, GeographicLib and solver library directories on the path. Links the
# public headers of the fusion library only, never GTSAM.
add_executable(fusion_runner fusion_runner.cpp)
target_link_libraries(fusion_runner PRIVATE flysight_core flysight_fusion)
flysight_solver_stack(fusion_runner)
```

and, next to the other fusion tests (before the exact-mode section, so the `_exact` registration below sees it):

```cmake
# The runner, driven as a child process on fixtures written out as CSV
flysight_add_fusion_test(tst_fusion_runner SOURCES tst_fusion_runner.cpp
  LIBS flysight_fusion_session_support)
target_compile_definitions(tst_fusion_runner PRIVATE
  FLYSIGHT_FUSION_RUNNER="$<TARGET_FILE:fusion_runner>")
add_dependencies(tst_fusion_runner fusion_runner)   # $<TARGET_FILE> creates no build order
```

Inside `if(_fs_exact)` add `flysight_add_fusion_exact_test(tst_fusion_runner)` after the five existing lines: the test compares the tool's output with an in-process run through the golden comparator, and on the capture configuration that comparison is bit identity across the process boundary.

`flysight_add_fusion_test` gives the test the 64 MiB stack, `PATH`/`LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH` prepended with Qt, GeographicLib and the solver runtime directories (`flysight_add_test` 96-111 and `flysight_solver_test_environment`), labels `core;fusion`, timeout 600 s. The child process inherits that environment (Task 2.5 must not replace it), which is how the tool finds its DLLs under CTest.

`cmake/SolverDependencies.cmake`: append `fusion_runner tst_fusion_runner` to `_FLYSIGHT_GTSAM_REACHERS` (they reach `gtsam` through `flysight_fusion`'s private link). Do not touch `_FLYSIGHT_GTSAM_NAMERS` (neither names `gtsam`). The configure then prints `GTSAM link confinement: OK (<n> targets reach gtsam)` with `n` two higher than before.

`docs`/`tests/README.md`: not touched in this phase; Phase 7 documents the tool and adds it to the README's option table and section 4 notes.

**Acceptance Criteria:**
- [ ] `cmake --build build-phase1 --config Release` builds `fusion_runner` and `tst_fusion_runner`; the configure output contains `GTSAM link confinement: OK`.
- [ ] `fusion_runner` has no `install()` rule and is not in `PROJECT_SOURCES`; with `FLYSIGHT_BUILD_FUSION_TESTS=OFF` neither target exists.
- [ ] `fusion_runner`'s link line names `flysight_core` and `flysight_fusion` only (no `gtsam`, no `flysight_fusion_test_support`, no Qt Test).
- [ ] `ctest -N` lists `tst_fusion_runner` with labels `core;fusion` and, on the capture configuration, `tst_fusion_runner_exact`.
- [ ] `flysight_solver_stack(fusion_runner)` is applied (MSVC: `/STACK:67108864` on the link line).

**Complexity:** S

---

### Task 2.5: `tst_fusion_runner` — the tool as a child process on fixtures written as CSV

**Purpose:** Prove every clause of the contract from outside the process: same diagnostics as a direct `Fusion::run()` on the fixture, same diagnostics as the application's own import-and-fit path, CSV round trip, `--dump-inputs`, the legacy gyro correction, exit codes, and usage.

**Files to create:**
- `tests/tst_fusion_runner.cpp` — one `QObject` test class, `FLYSIGHT_TEST_MAIN(FusionRunnerTest)`, `#include "tst_fusion_runner.moc"` (tests/README.md section 8).

**Technical Approach:**

*Fixtures.* `initTestCase()`: `TestEnvironment::instance().registerBuiltIns(); registerFusionOnce();` (as `tst_fusion_session.cpp` 143-148). `init()`: `useFreshLogbook()`, `resetPreferencesToDefaults()`, snapshot `registeredIds()`; `cleanup()`: destroy any `SessionModel`, then compare the snapshot and `enrolledEngineCount() == 0`.

*Helpers (each returns `bool` or an empty-on-success `QString`, never asserts):*
- `struct ToolRun { int exitCode; QByteArray stdoutBytes; QStringList stderrLines; bool normalExit; }` and `ToolRun runTool(const QStringList &arguments)`: `QProcess` with `setProgram(QStringLiteral(FLYSIGHT_FUSION_RUNNER))`, the arguments, `start()`, `waitForStarted(30000)`, `waitForFinished(300000)`; do **not** call `setProcessEnvironment` (the inherited environment carries the DLL paths CTest prepared). Split stderr on `\n` and strip a trailing `\r` from every line; drop lines starting with `# ` only where a test says so (by default keep them: a Qt warning on the headless path is a finding).
- `QString writeRecording(const SessionData &session, const QString &dir)`: splits a session into two files by sensor and exports them with `DataExporter::exportSession` — `<dir>/TRACK.CSV` holds the stored attributes plus the `GNSS` and `Local` sensors, `<dir>/SENSOR.CSV` the same attributes plus `IMU` and `TIME`. Build each part as `tst_fusion_session.cpp`'s `withoutSensor()` does (copy `attributeKeys()` via `storedAttribute`, then `mergeSourceData` of the kept sensors of `sourceData()`). Both parts carry `SESSION_ID`, `DEVICE_ID` and (unless removed) `SCHEMA_VER`, so the match ids agree and the merge has no conflict. Directory from `TestEnvironment::instance().newTempDir("recording")`.
- `QJsonObject jsonOf(const QByteArray &stdoutBytes)` via `QJsonDocument::fromJson` (tolerates the trailing newline / CRLF); the test fails on a parse error with the raw bytes in the message.
- `bool readCsv(path, header, QHash<QString, QVector<double>> &columns)` with `CsvFormat::parseDouble` per field.
- `bool readDump(path, QList<QPair<QString, QStringList>> &lines)`: label before the first comma, the rest split on commas.

*Fixture choice.* Success cases use `coarse_maneuver` (6 s, 601 IMU samples, 33 fixes, non-zero gyro; `fusionfixtures.cpp` 102-114): the legacy-gyro check needs non-zero gyro, which `coarse_linear` has not. The rejection case uses `reject_too_few_fixes` (equal column lengths, so it can be exported; `reject_length` is ragged and `DataExporter` refuses it). The application-path case uses `naturalSession("n1")` (`fusionsessions.cpp` 234-276: GNSS lat/lon/hMSL, IMU device time, TIME pulses; it fits successfully, `naturalSessionEndToEnd`).

*Test functions:*

1. `noPreferenceOnTheFitPath` — premise of the model-free path: `CalculationRegistry::instance().staticDependencies(DependencyKey::measurement("Fusion", "_time")).preferences` is empty (and the same for `Fusion/roll`). If a future calculation on the path declares a preference this fails first and names the reason to revisit Task 2.2.

2. `outputTableMatchesGolden` — `fitOutputChannels(result)` on `Fusion::run(toChannels(fusionFixture("coarse_maneuver")))`: 17 entries, names equal `fusionChannelNames()`, samples `sameBitsEverywhere` with `fusionChannel(result, name)`.

3. `successMatchesDirectRun` — `sessionFromFixture(coarse_maneuver, "f1")` written as a recording (the fixture session stores `Local/*`, `IMU/_time`, the origin attributes and `SCHEMA_VER=2`, so the imported effective inputs are the fixture bit for bit: `fusionsessions.h` 38-47, `inputsAreBitIdenticalToFixture`). Run `fusion_runner <dir> --csv <dir>/out.csv --dump-inputs <dir>/in.txt`. Expect: `normalExit`, exit 0; `compareJson("diagnostics", jsonOf(stdout), QJsonDocument::fromJson(direct.diagnosticsJson.toUtf8()).object())` empty, where `direct = Fusion::run(toChannels(fixture), recordingProgress)` on the main thread; stderr's last line `Succeeded` and the lines before it equal the texts `recordingProgress` collected (a `QStringList` captured by the `ProgressFn`), in order; the CSV header equals `fusionChannelNames().join(",")`, row count equals `direct.time.size()`, every column `sameBitsEverywhere` with `fusionChannel(direct, name)`; the dump's labels, in order, equal `fitInputs()` rendered as `<sensor>/<name>` / `<key>`, and its `GNSS/_time`, `Local/north`, `IMU/wx` lines parse to the fixture's arrays bit for bit, `_LOCAL_ORIGIN_INDEX` to `fixture.originIndex`, `_LOCAL_ORIGIN_LAT` to `fixture.originLat`.

4. `matchesTheApplicationImportPath` — `naturalSession("n1")` written as a recording. Child: `fusion_runner <dir>/TRACK.CSV <dir>/SENSOR.CSV` (the two-path form), expect exit 0. In-process, the application's path: `SessionModel model; const SessionImport::BatchResult imported = SessionImport::importFiles(model, {track, sensor});` (`tst_fusion_jobs.cpp` 694-698 pattern), one imported id, `const SessionData *session = model.loadedSession(id)`, `session->calculationEngine().request(QString::fromLatin1(Fusion::FitCalculationId)).status == ResultStatus::Ok`, `waitForIdle(model)`; then `compareJson("diagnostics", jsonOf(stdout), QJsonDocument::fromJson(session->getAttribute(SessionKeys::FusionDiagnostics).toString().toUtf8()).object())` empty, and `Fusion/<name>` of the session `compareSamples`-equal to the tool's `--csv` columns for all seventeen. This is the proof of "imports exactly as the application does": the model-free path and `SessionImport` + `SessionModel` (creation defaults, logbook, column worker and all) give the kernel the same channels, and the runner runs the same kernel on them. Destroy the model before `cleanup()`'s registry check.

5. `rejectionExitsOne` — `reject_too_few_fixes` written as a recording; `fusion_runner <dir> --csv <dir>/out.csv`: exit 1; stdout JSON `compareJson`-equal to `Fusion::run(toChannels(fixture)).diagnosticsJson` (the `{algorithm, failure}` object); stderr's last line is `"Rejected: " + direct.reason`; `<dir>/out.csv` does not exist.

6. `legacySchemaScalesTheGyro` — the `coarse_maneuver` session with `removeAttribute("SCHEMA_VER")` before splitting (both files must lack it; a file that has it would be merged in). `fusion_runner <dir> --dump-inputs <dir>/in.txt`. Expect the dump's `IMU/wx`, `IMU/wy`, `IMU/wz` lines to parse to `fixture.wx[i] * 1.14688` etc., `sameBits` per sample (the conversion is exactly one multiply, `sourceconversion.cpp` 45-70, and `deg/s` is a unit identity), while `IMU/ax` and `GNSS/_time` stay bit-identical to the fixture; and stdout JSON `compareJson`-equal to `Fusion::run(scaled)` where `scaled = toChannels(fixture)` with the three gyro arrays multiplied by the same literal in the test — whatever the outcome (it may be `Succeeded` or `SolverFailed`; the exit code must agree with `scaled`'s outcome). The literal `1.14688` in a test is allowed (the audit counts it in `src` only; `tests/tst_schema_units.cpp` already spells it).

7. `usageAndImportFailures` — `--help`: exit 64, stdout contains `Usage:`; no arguments: exit 64, stderr contains `error:` and `Usage:`; `--bogus <dir>`: exit 64; a nonexistent folder: exit 3, stderr contains `TRACK.CSV`; a folder whose `SENSOR.CSV` is `$FLYS,1\n$VAR,SESSION_ID,x\n$COL,IMU,time,wx\n` (no `$DATA`; `tst_importer.cpp` 364) next to a valid `TRACK.CSV`: exit 3 and stderr contains `Missing $DATA section`; two files with different `SESSION_ID`s (`Fixtures::trackFile("a")` and `Fixtures::sensorFile("b")` from `fixturebuilder.h`): exit 3 and stderr names both ids.

Expected values are literals or a direct kernel run on the fixture's arrays, never a second call of the tool. Every fit here is a fixture fit of a second or so; the whole executable stays well under a minute.

**Acceptance Criteria:**
- [ ] `ctest --test-dir build-phase1/FlySightViewer-build -C Release -R tst_fusion_runner --output-on-failure` passes, and `-R tst_fusion_runner_exact` passes on Michael's machine (bit identity across the process boundary).
- [ ] The seven functions above exist and each of the checks listed is a `QVERIFY2`/`QCOMPARE` with the difference text (from `compareJson` / `compareSamples`) or the child's stderr in the failure message.
- [ ] The test never calls `QProcess::setProcessEnvironment` / `setEnvironment`, and never reads user settings (it runs under `FLYSIGHT_TEST_MAIN`).
- [ ] The legacy check compares against `fixture.wx[i] * 1.14688` bit for bit and would fail if the tool read source values instead of effective ones.
- [ ] The application-path check goes through `SessionImport::importFiles` on a `SessionModel` in the test's temporary logbook.
- [ ] `audit_cleanup` passes.

**Complexity:** L

---

## Testing Requirements

### Unit Tests
- New: `tests/tst_fusion_runner.cpp` (Task 2.5), registered with `flysight_add_fusion_test` and, on the capture configuration, as `tst_fusion_runner_exact`.
- Existing, must stay green unchanged: `tst_fusion_session` (`registrationShape`, `inputsAreBitIdenticalToFixture`, `naturalSessionEndToEnd`), `tst_fusion_jobs`, `tst_fusion_rows`, `tst_fusion_parity`, `tst_fusion_kernel`, and their `_exact` runs; `audit_cleanup`.

### Integration Tests
- `matchesTheApplicationImportPath` is the integration proof: the tool's diagnostics equal what `SessionImport` + `SessionModel` + the engine's explicit request produce for the same two files.
- Configure output: `GTSAM link confinement: OK (<n> targets reach gtsam)`; the whole fusion label: `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L fusion --output-on-failure`, then the full suite without `-L`.

### Manual Verification
1. Build: `cmake --build build-phase1 --config Release`.
2. Put the runtime directories on `PATH` (Git Bash): `PATH="/c/Qt/6.9.3/msvc2022_64/bin:$PWD/third-party/GeographicLib-install/bin:$PWD/build-solver-deps/GTSAM-install/bin:<oneTBB install>/bin:$PATH"` (the same directories CTest prepends; `flysight_solver_test_environment` derives the solver ones from the imported targets — check the `Solver runtime targets:` line of the configure output for what they are).
3. `build-phase1/FlySightViewer-build/Release/fusion_runner.exe --help` → usage, exit 64 (`echo $?`).
4. A reference recording of spec section 12, e.g. `fusion_runner.exe "TEMP/data/Data comp 5 - FS 2 - serie nr 2 - 014667 (test 10)/24-09-05/11-17-12" --csv out.csv`: progress lines stream on stderr; on today's kernel this recording is the known non-converging case (memory: fusion-convergence-case-11-17-12), so expect `SolverFailed: ...` and exit 2 with the failure JSON on stdout and no `out.csv`; on recording `17-26-24` (tests/README.md "Real recordings") expect exit 0, a JSON whose `gnss_states` is 9247 and `imu_outputs` 24411, and an `out.csv` with 24411 rows. Both files lack `SCHEMA_VER`, so the objective is the corrected-gyro one, not the branch's 65602.22 (README, same section).
5. `fusion_runner.exe <folder> > diag.json 2> log.txt; echo $?` shows that stdout holds only the JSON and stderr only progress plus the outcome line.

## Notes for Implementer

### Gotchas
- **Confinement list.** Forgetting `fusion_runner` / `tst_fusion_runner` in `_FLYSIGHT_GTSAM_REACHERS` fails the configure with "reaches GTSAM ... and is not one of". Phase 1 adds `fusion_golden_capture` to the same list and its `add_executable` to the same block; if both phases have landed, keep both entries — append, never reorder or replace.
- **`$<TARGET_FILE>` in a compile definition creates no dependency**; `add_dependencies(tst_fusion_runner fusion_runner)` is required or `ctest` can run a test against a stale or missing tool after a partial build.
- **Never `applyCreationDefaults()`** in the runner: it is the one import-side function that reads `PreferencesManager` (`dataimporter.cpp` 237) and would also search parent folders for `FLYSIGHT.TXT`.
- **Never `EnginePreferenceProvider::install()`**: it binds `PreferencesManager`, hence `QSettings`.
- **Windows line endings.** stdout/stderr of the child arrive with `\r\n`; strip `\r` per line before comparing. `QJsonDocument::fromJson` ignores the trailing whitespace. Files written by the tool use `\n` (they go through `QFile`, not a text-mode `FILE*`).
- **Do not replace the child's environment.** `QProcess` inherits the parent's; CTest put Qt, GeographicLib and the solver runtime on `PATH` (Windows) / `LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH`. `setProcessEnvironment()` would lose that and the tool would not start.
- **Fixture choice.** `reject_length` is ragged and cannot be exported; `coarse_linear` has zero gyro, so it cannot prove the 1.14688 scale. Use the fixtures named in Task 2.5.
- **Both files must lack `SCHEMA_VER`** in the legacy case: the merge adds any header key the session lacks, so removing it from one part only would still yield a schema-2 session.
- **Qt log messages** on the child's stderr: the handler prefixes them with `# `. `successMatchesDirectRun` expects none (its stderr must be exactly the progress texts and `Succeeded`); if a benign warning appears on some platform, find its cause rather than filtering — a warning on the headless import path is a defect of the path.
- **`QCommandLineParser::process()`** exits 0 on `--help` and prints to stdout on its own; use `parse()` and write the usage yourself so that the streams and codes are the tool's.
- **One number formatter.** The audit rule "one authority: number formatting" searches `src` only, but the tool follows it anyway: `CsvFormat::formatDouble` for every sample, `formatAttributeValue` for attributes. This is also what makes `--csv` locale-proof and bit-round-trippable (csvformat.h 18-30).
- **The fit runs on the main thread**: `flysight_solver_stack` is not optional (a large recording overflows the default stack in GTSAM's elimination).
- **Rejection semantics differ from the application in one respect**: the application never runs a fit whose declared inputs are unavailable (`ResultStatus::MissingInput`, reported as a blocker), while the tool hands empty channels to the kernel, which rejects with its own message (`Sensor fusion needs GNSS, IMU and shared UTC time conversion`, `inputadapter.cpp` 96). Acceptable for tooling; documented in Decisions Made.
- **`registerFusionOnce()`** in the test and `registerFusionCalculations()` in the tool are per process; nothing is ever unregistered.
- **`tests/README.md` and `docs/`** are Phase 7's; do not edit them here (the README option table will list `tst_fusion_runner` and `fusion_runner` then).

### Decisions Made
- **Import path (a), model-free.** `DataImporter::parseFile` + `SessionMerge::plan/apply` + the engine on a bare `SessionData`, with the channel assembly exposed from the registration header. Rationale: every piece is already model-free; the only preference read on the import side lives in `applyCreationDefaults()`, whose outputs are not on the fit's dependency path; the engine needs no preference provider because nothing on that path declares one (pinned by `noPreferenceOnTheFitPath`); and the runner then never constructs `PreferencesManager`, `LogbookManager` or a `SessionModel` at all — "never instantiated" rather than "redirected", which is the stronger of the two guarantees the overview allows. Path (b) would have needed the whole `TestEnvironment` machinery (preference registration, a logbook folder, the idle scheduler's saves) inside a tool. The equality of (a) with the application's path is proven by `matchesTheApplicationImportPath`, not assumed.
- **Settings tripwire in addition.** `main()` still redirects default `QSettings` into a `QTemporaryDir` (three calls, no probe) so that a future `flysight_core` change that constructs one cannot reach the user's registry/INI. Cheap, and it makes the "must not read or write the user's settings" clause hold structurally and defensively.
- **Channel assembly over readers** (`MeasurementReader`/`AttributeReader`), not over `EvaluationContext`: the context's constructor is private to the engine, so a public function over a context could not be called by a tool. The context overload stays as a two-line wrapper, so `computeFit()` and the tool share one body and one table.
- **Output table exposed too** (`fitOutputChannels`) rather than linking `flysight_fusion_test_support` for `fusionChannelNames()`/`fusionChannel()`: the assignment fixes the tool's link set to `flysight_core` and `flysight_fusion`, and the registration already owns "one table serves declaration and publication"; the tool becomes its third consumer.
- **`--dump-inputs` rather than the input audit** for the legacy-gyro proof: the audit (`inputadapter.cpp` 79-90) records counts, the epoch and the origin, not sample values, and cannot show a scale. The dump is the table of Task 2.1 rendered, so it also proves "the tool feeds exactly the fixture" and grows with Phase 6 automatically. It is written before the fit and independently of the outcome.
- **Exit codes**: 0 Succeeded, 1 Rejected, 2 SolverFailed, 3 import failure (parse, mismatched session ids, merge conflict, missing file), 4 output file not written, 5 internal (a `std::exception` out of the kernel, or `Cancelled` without a cancel function), 64 usage. `--help` also exits 64: the one rule "0 means a converged fit; nothing else ever returns 0" is the literal reading of spec section 8 and of "exit 0 only for Succeeded", and a corpus script never invokes `--help`. `--help` writes to stdout, an invocation error to stderr, so the two are still distinguishable.
- **Order on success**: dump, fit, JSON on stdout, then `--csv`; a CSV write failure therefore exits 4 with the JSON already printed (a script keeps the diagnostics and sees the code).
- **CSV shape**: comma-separated, derived header, no `$` prefixes, no units line — a plain CSV of the seventeen channels, not a FlySight file (the channels are calculated, and `DataExporter` writes source data only).
- **Missing sensors are the kernel's rejection**, not an emulation of the engine's availability gate: fewer moving parts in the tool, and the exit code (1) and message are still a function of the inputs.
- **Test comparisons through `compareJson`/`compareSamples`** (portable by default, bit-exact under `FLYSIGHT_FUSION_EXACT=1`), as the assignment requires; the `_exact` registration of the test makes the bit-identity claim across the process boundary a checked fact on the capture configuration. Test inputs are bit-identical to the fixtures by construction (exported fixture sessions store `Local/*`, `IMU/_time` and the origin attributes, which win over calculations), so no round-trip through lat/lon is needed for the fixture cases; the realistic lat/lon + TIME-fit path is covered by `naturalSession` against the application's own import.
- **Runner is one translation unit** in `tests/` (pattern `solver_deploy_probe.cpp`), not a library and not a new function in `src/sessionimport.*`: `SessionModel::mergeSessions` is by design the application's only import path (`sessionmodel.h` 91, audit "one import path"), and the tool's test needs no in-process copy of the import (it compares against the application path instead).
- **No README/docs edits** in this phase (Phase 7).

### Open Questions
- None.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria
2. All tests pass — `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L fusion --output-on-failure` (including `-L exact` on Michael's machine) and the full suite without `-L`, `audit_cleanup` included
3. Code follows patterns established in reference files (registration tables in `fusionregistration.cpp`, `solver_deploy_probe` for the executable, `tst_fusion_session` / `tst_fusion_jobs` for sessions and the import path, tests/README.md section 8 for the test)
4. No TODOs or placeholder code remains
5. The files reported for the commit are exactly: `src/fusion/fusionregistration.h`, `src/fusion/fusionregistration.cpp`, `tests/fusion_runner.cpp`, `tests/tst_fusion_runner.cpp`, `tests/CMakeLists.txt`, `cmake/SolverDependencies.cmake`
