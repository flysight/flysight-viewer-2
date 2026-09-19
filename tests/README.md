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
| `tst_importer` | `DataImporter`: data stored exactly as recorded, nothing stamped, `SCHEMA_VER` and structural errors rejected without touching the target session, `$VAR` values kept verbatim, malformed rows skipped with one summary warning, FS1, custom columns, CRLF; `parseFile` carries nothing the file did not say (match id synthesized from the bytes but not stored), `applyCreationDefaults` is the one writer of import-time defaults and only fills absent keys, header-only `peekHeaderAttribute` |
| `tst_source_layer` | Session-level acceptance for the source / effective split on real `SessionData`, importer, exporter, logbook and model: acceptance 1, 2, 4, 6 (load), 16; enumeration and source access never compute; lazy conversion; buffer sharing; exporter and merge use the source layer |
| `tst_csvformat` | `CsvFormat`, the one definition of the on-disk text forms: shortest round-trip doubles (a 200 000-value bit-pattern sweep), `-0`, `nan` / `inf` / `-inf`, attribute values by `QVariant` type, line-break flattening, valid names and units |
| `tst_persistence_roundtrip` | Save / reload on the real importer, exporter and logbook: acceptance 5 (bit-identical samples, units and header attributes preserved, `SCHEMA_VER` only if recorded, effective values unchanged, second cycle byte-identical, independent of any cache) and acceptance 6 (a released logbook file is not rescaled, relabelled or stamped by a save; the `loadSession` backfill is additive and idempotent); non-finite samples, ragged sensors, unrepresentable text |
| `tst_logbook_index` | `LogbookManager`'s `index.json` column cache: the calculation-compatibility marker and environment fingerprint gate the cached values (acceptance 18 at the storage level), unsaved-column tracking and save ordering (an interrupted save never leaves a cached column that disagrees with the session file), orphan session files adopted, marks follow remap / remove / reset; the raw load with its failure reason, the legacy backfill as a separate step, identity-entry queries, a legacy flat index coming up as stubs without rewriting a session file |
| `tst_column_cache` | The same through `SessionModel`: upgrade discards and lazily recomputes (acceptance 18), an edit refreshes only the affected columns with a warm and a cold engine, merges and bulk edits, interrupted saves, environment changes (declared preference, altitude-marker registrations) discarding loaded and unloaded rows without saving, save failures, line breaks flattened at edit |
| `tst_session_merge` | `SessionMerge`, the pure plan-then-apply merge of spec 6.3 / 6.4 on programmatic sessions: absent attributes added, equal ones ignored, different header attributes conflict (all reported, sorted, with the delete-and-re-import hint), `_` attributes keep the session's value, the `n/a` device placeholder counts as absent, equality on the on-disk text form, columns replaced / added / kept with samples and unit together, bitwise column comparison (NaN, `-0`), the ragged rule, purity of `plan()`, the invalidation set of `apply()` |
| `tst_import_merge` | The import path (`SessionImport::importFiles` -> `SessionModel::mergeSessions`) against a temporary logbook: acceptance 3 (a rejected file leaves the session untouched), 7 (TRACK/SENSOR order independence loaded, unloaded and in one batch; conflicts change nothing; edits and unmatched measurements survive), 8 (the `SCHEMA_VER` escape hatch), 10 and 18 (merge parts); defaults only at creation, failed loads are errors, failed-load placeholders are never saved, identity stubs are matched, identical re-imports are no-ops |
| `tst_import_batch` | `SessionImport`: one result per file in input order with parse failures included, cancellation through the progress callback, and the text of the import-failure dialog with each file's error |
| `tst_python_bridge` | The Python plugin bridge through the real embedded interpreter and the real `flysight_cpp_bridge` module (acceptance 17, plugin half): effective reads in single-output plugins, declared-read diagnostics (`UndeclaredInputError`), source access matching C++, the multi-output form running once, exceptions and malformed output giving a clean unavailable result with negative caching, returned arrays copied, explicit key decoding with per-plugin rejection, plugin-before-built-in precedence, the bundled `imu_tilt.py` example. See "The embedded-Python bridge test" below |

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

Tests that use `LogbookManager::initialize()` together with cached column
values must call `TestEnvironment::registerBuiltIns()` first, as the
application does: the calculation-environment fingerprint captured at
`initialize()` is compared with the one of the next start, and registering in
between would make every reopen discard the cache. For the same reason set the
logbook columns (`LogbookColumnStore::setColumns`, which flushes the index)
before planting a hand-edited `index.json`. `tst_logbook_index` and
`tst_column_cache` remove whatever they register globally in `cleanup()`.

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
- A test of the import path imports through `SessionImport::importFiles` (the
  model decides between creation and merge). `DataImporter::importFile` is the
  "parse + create" convenience for tests that just need a session;
  `SessionModel::mergeSessions(QList<SessionData>)` adopts in-memory sessions
  as they are (no import-time defaults). Put TRACK / SENSOR pairs in one
  device-style folder (`<tmp>/24-01-01/12-00-00/`) when the description default
  matters. `index.json` gets fresh column ids on every flush, so "the index
  bytes are unchanged" also proves that nothing flushed it.

## The embedded-Python bridge test (`tst_python_bridge`)

`tst_python_bridge` is the one exception to "no Python": it boots the real
embedded interpreter, imports the real `flysight_cpp_bridge` module from the
build tree, and runs the SDK (`python_plugins/flysight_plugin_sdk.py`), the
test plugins in `tests/python_plugins/*.py`, and the bundled example
(`python_plugins/examples/imu_tilt.py`) against real `SessionData` objects. It
covers the plugin half of acceptance 17: effective reads in existing
single-output plugins, source access matching C++, the multi-output form
running once, exceptions and malformed output giving a clean unavailable
result, explicit key decoding, and registration-order precedence.
`pluginhost.cpp` and `pluginadapters.cpp` are not part of `flysight_core`
(that would force Python onto every test), so this target compiles them
directly and links `pybind11::embed`. No other test links Python.

**How it finds Python.** A test executable has no `python/` folder next to it,
so `PluginHost` takes its "system Python" branch, which honours the standard
environment variables. CTest sets all of them (see the block at the end of
`tests/CMakeLists.txt`); nothing comes from your `PATH` or profile, and
`PluginHost` has no test-only code path:

| Variable | Value | Why |
|---|---|---|
| `PYTHONHOME` | `sys.base_prefix` of the interpreter CMake found (`Python_EXECUTABLE`) | the standard library |
| `PYTHONPATH` | the directory of the built `flysight_cpp_bridge` (`build/FlySightViewer-build/Release`), then the site directory that holds NumPy | the bridge is imported before the plugin folder is on `sys.path`; NumPy may live in a venv or user site |
| `PATH` (Windows) | `+=` the directory of `python3XX.dll`, plus Qt and GeographicLib as for every test | DLL lookup |
| `PYTHONDONTWRITEBYTECODE` | `1` | nothing is written outside the test's temporary directory |

The plugin files are copied into a temporary directory first, because
`PluginHost` imports every `*.py` in the plugin folder.

**NumPy is required** in the build-time interpreter (the SDK imports it). If
`python -c "import numpy"` fails at configure time (or CMake is older than
3.22), CMake prints a warning and the test is registered as **disabled**:
`ctest` reports it as "Not Run (Disabled)" rather than omitting it, and the
target is still built so `pluginhost.cpp` stays compile-checked. Fix with
`python -m pip install numpy` and re-run CMake.
`-DFLYSIGHT_BUILD_PYTHON_TESTS=OFF` (default `ON`; forwarded by the root
`CMakeLists.txt`) removes the target entirely. Like every test on Windows it is
Release only.

**Running it outside CTest** means setting the three variables by hand (Git
Bash; adjust the Python and Qt paths):

```bash
PY="$(python -c 'import sys; print(sys.base_prefix)')"
NP="$(python -c 'import numpy, os; print(os.path.dirname(os.path.dirname(numpy.__file__)))')"
PYTHONHOME="$PY" \
PYTHONPATH="$(cygpath -w "$PWD/build/FlySightViewer-build/Release");$NP" \
PATH="$(cygpath -u "$PY"):/c/Qt/6.9.3/msvc2022_64/bin:$PWD/third-party/GeographicLib-install/bin:$PATH" \
PYTHONDONTWRITEBYTECODE=1 QT_FORCE_STDERR_LOGGING=1 \
build/FlySightViewer-build/Release/tst_python_bridge.exe -v2
```

**Adding a case.** There is one interpreter per process and
`PluginHost::initialise()` runs once, so every plugin file is loaded in
`initTestCase()`: add a file under `tests/python_plugins/` and a test function
in `tst_python_bridge.cpp`, never a second `initialise()`. Plugin calculations,
plots and markers stay registered for the life of the process: use unique
`_PY_*` / `py*` names and never assert that a registry is empty. Files are
imported in name order and the calculation ids contain the registration index,
so a new file that sorts before an existing one shifts the literal list in
`registrationOrderIsDeterministic` (name it to sort last, like `t_zdocs.py`).
Log assertions install their message handler inside the test function (after
`initTestCase`) and `cleanup()` removes it.

**Include order.** Include the pybind11 headers before any Qt header, wrapped
in `#pragma push_macro("slots")` / `#undef slots` / `#pragma pop_macro("slots")`
(Qt's `slots` macro breaks the Python headers), and mask `_DEBUG` around
`<Python.h>` on MSVC; copy the top of `tst_python_bridge.cpp`.
