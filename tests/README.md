# FlySight Viewer tests

## What this is

Qt Test executables that link the `flysight_core` static library (session
data, calculations, import/export, logbook, session model, registries, unit
conversion). They need Qt Core, Gui and Test, GeographicLib, and Boost headers
only: no UI, no WebEngine, no KDDockWidgets, no QCustomPlot, and no Python.

The tests are not a standalone project. `tests/` is added by
`src/CMakeLists.txt` when `FLYSIGHT_BUILD_TESTS=ON` (default `OFF`), and every
test is registered with CTest. Test executables have no install rules, so
packages are the same whether or not the option is set.

| Test | Covers |
|------|--------|
| `tst_harness` | The test-support code itself: settings and logbook isolation, fixture builders |
| `tst_smoke` | End-to-end characterization of importer, session, calculations, exporter, logbook, and model (started as a pin of v2026.04.1; expectations a phase changed on purpose were rewritten with that phase) |
| `tst_calcregistry` | Calculation engine: value types, registration order and validation, family instances, private registries |
| `tst_calcengine` | Calculation engine: resolution, caching, dependency recording, invalidation across sessions, explicit policy, preferences, families, measurement layers |
| `tst_calcengine_safety` | Calculation engine: nested scopes, cycles, exceptions, re-entrancy guards |
| `tst_calcengine_oracle` | Calculation engine: randomized (seeded) sequences compared against a fresh evaluation |
| `tst_builtins_golden` | Every built-in calculation read through `SessionData` on the generated descent fixture, against hand-derived golden literals |
| `tst_builtins_engine` | The built-ins on a private registry and `FakeSessionState`: golden values, registration inventory, declared inputs only, multi-output groups, candidate order, the declared preference, interpolation family, altitude descriptor |
| `tst_session_engine` | `SessionData` on the engine with the real built-ins: run-once, invalidation, candidate replacement, overrides, preferences, the fresh-evaluation oracle, copy/move semantics |
| `tst_session_model_engine` | `SessionModel` + `AltitudeMarkerManager`: registry and preference broadcasts reaching `dependencyChanged`, coalescing, merges, rows surviving sort |
| `tst_schema_units` | The two tables behind the conversion layer: the schema table (`SCHEMA_VER` validation, which measurements each version corrects) and the unit normalization table (silent, identity for unknown text) |
| `tst_conversion_engine` | The conversion families on a private registry and `FakeSessionState`: legacy gyro correction, `SCHEMA_VER` 1 / 2 / absent / unsupported, unit normalization, schema-then-unit order, buffer sharing for identity conversions, the dependencies that make the choice follow the attribute |
| `tst_importer` | `DataImporter`: data stored exactly as recorded, nothing stamped, `SCHEMA_VER` and structural errors rejected without touching the target session, `$VAR` values kept verbatim, malformed rows skipped with one summary warning, FS1, custom columns, CRLF |
| `tst_source_layer` | Session-level acceptance for the source / effective split on real `SessionData`, importer, exporter, logbook and model: acceptance 1, 2, 4, 6 (load), 16; enumeration and source access never compute; lazy conversion; buffer sharing; exporter and merge use the source layer |

The `tst_calc*` tests drive `src/engine/` with synthetic calculations against
`FakeSessionState` / `FakePreferenceProvider` (`support/fakesessionstate.h`).
Each builds its own `CalculationRegistry`; none registers anything in
`CalculationRegistry::instance()`.

The built-in calculations are pinned by one golden table
(`support/builtinfixture.*`: `DescentFixture` generates a 296-row jump plus a
three-row sensor file; `goldenValues()` holds literals only). Rows marked
"captured" were recorded from the v2026.04.1 engine before the migration; all
others were derived by hand. `tst_builtins_engine` uses private registries;
`tst_builtins_golden`, `tst_session_engine` and `tst_session_model_engine` use
the process-wide registry through `TestEnvironment::registerBuiltIns()`, and
must leave it as they found it (altitude-marker registrations are removed in
`cleanup()`).

Ordinary reads (`getMeasurement`) return *effective* values: the recorded data
passed through the conversion layer, which `registerBuiltIns()` registers along
with the other built-ins. A test that asserts effective values must therefore
call `TestEnvironment::registerBuiltIns()`; without it no conversion family
exists and effective == source. Corrected gyro values are compared with an
absolute tolerance of `1e-9` (`62.5 * 1.14688` is not the double nearest
`71.68`); values whose conversion is exact (`1 g`, `1 gauss`, identities) are
compared with `==`. Test helpers that move data between sessions
(`DescentFixture::load`, `copyStoredState`) use the source accessors, so the
copies carry the data as recorded. Tests that count warnings install their
message handler after `registerBuiltIns()`.

## Configure / build / run

**Windows (superbuild).** Third-party dependencies must already be built in
`third-party/*-install` (see the top-level `README.md`).

```bash
cmake -G "Visual Studio 17 2022" -A x64 -B build -S . -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64" -DGOOGLE_MAPS_API_KEY="your-api-key" -DFLYSIGHT_BUILD_THIRD_PARTY=OFF -DFLYSIGHT_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure
```

The superbuild forwards `FLYSIGHT_BUILD_TESTS` to the application project, whose
build tree is `build/FlySightViewer-build`; that is the directory CTest runs
against. To stop building the tests, configure again with
`-DFLYSIGHT_BUILD_TESTS=OFF`.

**Windows (application project directly, as CI does):**

```bash
cmake -G "Visual Studio 17 2022" -A x64 -S src -B build-app -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64" -DFLYSIGHT_BUILD_TESTS=ON
cmake --build build-app --config Release
ctest --test-dir build-app -C Release --output-on-failure
```

**macOS / Linux** (single-configuration generators: no `--config`, no `-C`):

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DFLYSIGHT_BUILD_THIRD_PARTY=OFF -DFLYSIGHT_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build/FlySightViewer-build --output-on-failure
```

On Windows only the **Release** configuration is supported for tests: only a
Release `GeographicLib.dll` exists in `third-party/GeographicLib-install/bin`.

## Running one test / one function

```bash
ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure -R tst_smoke
ctest --test-dir build/FlySightViewer-build -C Release -N        # list tests without running them
```

CTest puts the Qt `bin` directory and `third-party/GeographicLib-install/bin`
on `PATH` for each test (CMake 3.22 or newer), so nothing has to be deployed
next to the executables. To run an executable directly, for example to select
one test function, add those two directories to `PATH` yourself:

```bash
PATH="/c/Qt/6.9.3/msvc2022_64/bin:$PWD/third-party/GeographicLib-install/bin:$PATH" QT_FORCE_STDERR_LOGGING=1 build/FlySightViewer-build/Release/tst_smoke.exe importFs2Sensor -v2
```

(Git Bash syntax; in PowerShell prepend the same two directories to
`$env:PATH` and set `$env:QT_FORCE_STDERR_LOGGING = "1"`.) The executables are
in the application build tree's per-configuration directory
(`build/FlySightViewer-build/Release/`). `QT_FORCE_STDERR_LOGGING=1` is needed
on Windows whenever the output is piped or captured: without a console, Qt Test
otherwise sends its log to the debugger and prints nothing. CTest sets it for
every test. All standard Qt Test command-line options work, e.g. `-functions`
to list the test functions.

## Isolation guarantees

Tests never touch your preferences or logbook. Every test executable creates
one `FlySightTest::TestEnvironment` in `main()` before any application
singleton exists. It:

- sets the organization to `FlySightTests` and the application name to the test
  class name, never the real `FlySight` / `FlySightViewer`;
- enables `QStandardPaths` test mode;
- creates one temporary directory per process; everything the test writes goes
  under it, and it is deleted when the process exits;
- makes INI the default `QSettings` format and redirects both user and system
  scope into that directory, so every default-constructed `QSettings`
  (`PreferencesManager`, `LogbookColumnStore`, `AltitudeMarkerManager`) reads
  and writes a throw-away INI file instead of the Windows registry;
- registers the preferences the core library reads with literal defaults, and
  points `general/logbookFolder` at a folder under the temporary directory
  (the application default, the Documents folder, is never used);
- **aborts the process with `qFatal`** if a probe `QSettings` is not an INI
  file under the temporary directory, or if the logbook directory is not under
  it. A misconfigured harness therefore fails loudly instead of writing to
  real user data.

`TestEnvironment::useFreshLogbook()` switches to a new empty logbook folder and
calls `LogbookManager::reset()`; `reopenLogbook()` calls `reset()` alone, which
simulates an application restart on the same folder.

## Writing a test

- One `QObject` test class per executable. Name the file and target
  `tst_<area>.cpp` / `tst_<area>` and register it in `tests/CMakeLists.txt`:

  ```cmake
  flysight_add_test(tst_<area> SOURCES tst_<area>.cpp)
  ```

  `flysight_add_test(<name> SOURCES ... [LIBS ...] [ENVIRONMENT VAR=value ...])`
  creates the executable, links `flysight_test_support` (and through it
  `flysight_core` and Qt Test), registers the CTest test with
  `QT_QPA_PLATFORM=offscreen` and `QT_FORCE_STDERR_LOGGING=1`, a 120 s timeout and the `core` label, and sets
  up DLL lookup on Windows.
- End the file with `FLYSIGHT_TEST_MAIN(YourTestClass)` (from `testmain.h`) and
  `#include "tst_<area>.moc"`. Never use `QTEST_MAIN` / `QTEST_GUILESS_MAIN`:
  they give no hook to isolate settings before the first singleton is touched.
  For the same reason, do not touch application singletons from global or
  static initializers in a test file.
- Get the environment with `FlySightTest::TestEnvironment::instance()`. Typical
  fixtures: `registerBuiltIns()` in `initTestCase()` (registers built-in
  attributes and calculations once per process; the UI-owned plots and markers
  are deliberately not registered), `useFreshLogbook()` and
  `resetPreferencesToDefaults()` in `init()`, `newTempDir()` for input and
  output files, and `waitForIdle(model)` to let a `SessionModel` finish its
  deferred saves and column fills.
- If a test exercises core code that reads a preference not yet registered,
  add the key and its literal default to
  `TestEnvironment::registerCorePreferences()`.
- Generate input files in code with `Fs2FileBuilder` / `Fs1FileBuilder`
  (`fixturebuilder.h`). Values are passed as text and written verbatim, so the
  test controls the exact bytes; nothing is added implicitly. `Fixtures::sensorFile()`
  and `Fixtures::trackFile()` are small canned files. Always include a
  `DEVICE_ID` so the importer does not search parent directories for
  `FLYSIGHT.TXT`.
- Use a fresh `DataImporter` per import when asserting on `getLastError()`.
- Expected values are literals, worked out independently. Never obtain an
  expectation by calling the code under test.
- When a test pins behavior that a planned change will alter, mark the
  expectation with

  ```cpp
  // BASELINE: <what> - changes in Phase <n>
  ```

  so it can be found with `git grep -n "BASELINE:" tests/`.

## Adding tests that need Python (Phase 7)

Not implemented yet. `pluginhost.cpp` has no UI dependencies but is not part of
`flysight_core`, because that would force Python onto every test. A test of
the embedded Python bridge compiles it directly and links the interpreter:
`flysight_add_test(tst_<area> SOURCES tst_<area>.cpp ${FLYSIGHT_SRC_DIR}/pluginhost.cpp LIBS pybind11::embed ENVIRONMENT <VAR=value> ...)`,
plus `add_dependencies(tst_<area> flysight_cpp_bridge)` so the bridge module
is built first, with `ENVIRONMENT` entries telling the interpreter where to
find the bridge module and the plugin directory. This works because `tests/` is
added from inside the `src` project, where `pybind11::embed`,
`Python::Python`, `flysight_cpp_bridge`, and
`flysight_msvc_fix_python_debug_autolink()` are all in scope.
