# Phase 1: Baseline branch, core library, and test harness

## Overview

This phase creates the working branch from tag `v2026.04.1`, moves every non-UI
model / IO / calculation source out of the `FlySightViewer` executable into a
linkable static library (`flysight_core`), and adds an isolated Qt Test harness
(`tests/`) that is built through an opt-in CMake option and registered with
CTest. It ships a small characterization ("smoke") suite that pins *baseline*
behavior, so that Phases 2-8 can change expectations deliberately. No
application behavior changes in this phase.

## Dependencies

- **Depends on:** None - can begin immediately.
- **Blocks:** Phases 2-8 (everything).
- **Assumptions:**
  - Tag `v2026.04.1` exists locally (verified: it resolves to `7ed7382`).
  - The working tree is clean apart from the untracked `experiments/` and
    `third-party/{gtsam,GTSAM-install,gtsam-build,oneTBB,oneTBB-build,oneTBB-install}/`
    directories, which are harmless and must be left alone.
  - Third-party dependencies are already built in `third-party/*-install`
    (GeographicLib is a **shared** library: `third-party/GeographicLib-install/bin/GeographicLib.dll`).
  - Toolchain: Visual Studio 2022, Qt 6.9.3 (`C:/Qt/6.9.3/msvc2022_64`), Boost 1.87,
    Python 3.13, CMake 3.31 locally (project minimum is 3.18).
  - `src/CMakeLists.txt`, `CMakeLists.txt`, `cmake/*`, `.github/workflows/build.yml`
    and every file named in the library boundary below **except**
    `src/dataimporter.*`, `src/dataexporter.cpp`, `src/logbookmanager.cpp`,
    `src/sessiondata.h`, `src/preferences/preferencekeys.h` are identical at the
    tag and on `master`; line numbers for them are valid on either. For the
    excepted files, line numbers below are **baseline** (`git show v2026.04.1:<path>`).

### Build-structure facts the implementer must know first

- The repository root `CMakeLists.txt` is a **superbuild**: it builds
  third-party libraries and then builds `src/` as an `ExternalProject`
  (`CMakeLists.txt` 167-176) into `build/FlySightViewer-build`, forwarding a
  hand-picked list of cache variables (`_APP_CMAKE_ARGS`, 141-165).
- `src/CMakeLists.txt` is the real project (`project(FlySightViewer ...)`). CI
  (`.github/workflows/build.yml` step "Configure main application", ~468-495)
  configures `-S src` directly with Ninja and never uses the superbuild for the app.
- Therefore "wired into the top-level build" means: the option is defined in
  `src/CMakeLists.txt` (so CI and direct builds see it) **and** declared and
  forwarded by the root superbuild (so `cmake --build build --config Release`
  builds the tests when it was configured with the option).

---

## Tasks

### Task 1.1: Create the working branch from `v2026.04.1`

**Purpose:** Establish the baseline every later phase builds on, without carrying commit `4668f48`.

**Files to modify:** none.

**Technical Approach:**

- Branch name: **`schema-and-calculations`** (matches the spec file name
  `PLANS/schema-and-calculations.md` and the repo's un-prefixed branch naming,
  e.g. `sensor-fusion-clean-port`).
- Commands (Git Bash or PowerShell, from the repo root). **The orchestrator
  runs steps 1-2 before this phase starts** (see "Commit Policy" in
  `00-overview.md`); the implementation agent runs only the read-only checks
  in steps 3-4 and stops if they fail:
  1. `git status --short` - confirm only the untracked `experiments/` and
     `third-party/...` entries listed above. If tracked files are modified, stop and ask
     (sole exception: `.claude/prompts/implementation-orchestrator.md`, handled by the
     "Tooling commit" rule in the overview's Commit Policy).
  2. `git switch -c schema-and-calculations v2026.04.1`
  3. `git log --oneline -1` must print `7ed7382 Surface notarization rejections and retry stapling`.
  4. `git diff --stat v2026.04.1` must be empty.
- Expected side effects of the switch: tracked `tests/` (three files from
  `4668f48`) and `docs/DATA_SCHEMA.md` disappear from the working tree - that is
  correct; `tests/` is re-created from scratch in Task 1.4. `PLANS/` and `TEMP/`
  are excluded through `.git/info/exclude` and survive. `build*/` and `dist/`
  are ignored by `.gitignore` and survive (stale `build-import-tests/` etc. may
  be ignored).
- The only remote is **`upstream`**; there is no `origin`. **Do not push** and do
  not set an upstream tracking branch unless Michael asks. Do not cherry-pick,
  merge, or rebase anything from `master`. Ideas from `master:tests/*` may be
  read with `git show master:tests/<file>` and re-typed; nothing concerning
  `ImportGyroScaling`, `GyroScaling`, `DataSchema`, `SchemaVersion`/`SCHEMA_VER`
  stamping or `dataSchemaVersion` may be reproduced.
- Do not commit, stage, or create the branch yourself: per the "Commit Policy" section of
  `00-overview.md` the orchestrator creates the branch before this phase
  starts and commits the phase on ACCEPT. Verify the branch (criteria below)
  and report every file you created, modified, or deleted.

**Acceptance Criteria:**
- [ ] `git rev-parse --abbrev-ref HEAD` prints `schema-and-calculations`.
- [ ] `git merge-base HEAD v2026.04.1` equals `git rev-parse v2026.04.1^{commit}`, and `git log v2026.04.1..HEAD` contains no commit from `master` (`4668f48` absent).
- [ ] `git grep -n "ImportGyroScaling\|GyroScaling\|dataSchemaVersion\|SchemaVersion"` returns nothing.
- [ ] Nothing has been pushed (`git branch -vv` shows no upstream for the new branch).

**Complexity:** S

---

### Task 1.2: Remove the two unused Widgets includes that block a UI-free library

**Purpose:** `dataimporter.cpp` and `sessionmodel.cpp` include `<QMessageBox>` without using it; that single include is the only thing tying the core sources to Qt Widgets.

**Files to modify:**
- `src/dataimporter.cpp` - delete line 13, `#include <QMessageBox>` (baseline numbering).
- `src/sessionmodel.cpp` - delete line 6, `#include <QMessageBox>`.

**Technical Approach:**

Findings from the include audit (baseline), which define the boundary used in Task 1.3:

| Source | Non-Qt-Core things it pulls in | Verdict |
|---|---|---|
| `dataimporter.cpp` | `preferences/preferencesmanager.h`, `preferencekeys.h`, `units/unitconversion.h`; `<QMessageBox>` **unused** (no `QMessageBox` token anywhere else in the file) | core after include removal |
| `dataexporter.cpp` | Qt Core only | core |
| `logbookmanager.cpp` | `dataimporter.h`, `dataexporter.h`, `logbookcolumn.h`, preferences | core |
| `logbookcolumn.cpp` | `attributeregistry.h`, `markerregistry.h`, `plotregistry.h`, `QSettings`, preferences | core |
| `sessionmodel.cpp` | `attributeregistry.h`, `logbookmanager.h`, `units/unitconverter.h`, preferences; `<QMessageBox>` **unused**; `QAbstractTableModel` is Qt Core | core after include removal |
| `idlescheduler.*` | `QTimer` (Core) | core |
| `attributeregistry.*`, `plotregistry.*` | `QColor` (**Qt Gui**) in `plotregistry.h` | core, needs Gui |
| `markerregistry.cpp` | `momentmodel.h` (mirrors markers into `MomentModel`), `QColor` | core, drags in `momentmodel` |
| `momentmodel.cpp` | `sessionmodel.h`, `dependencykey.h`, `QColor` | core (no UI) |
| `units/unitconverter.cpp` | preferences only | core |
| `units/unitconversion.h`, `units/unitdefinitions.h` | header-only, Core | core |
| `calculations/simplificationcalculations.cpp` | `boost/geometry`, `GeographicLib/LocalCartesian.hpp` | core, needs Boost headers + GeographicLib |
| `calculations/wspcalculations.cpp` | `GeographicLib/Geodesic.hpp`, `markerregistry.h`, `attributeregistry.h`, `QColor` | core, needs GeographicLib + Gui |
| `calculations/spcalculations.cpp` | `markerregistry.h`, `attributeregistry.h`, `QColor` | core, needs Gui |
| `calculations/attributecalculations.cpp` | preferences | core |
| `calculations/gnss/imu/mag/time/baro/hum/vbat`, `derivativehelper`, `isadensity`, `calculatedvalueregistry`, `attributeregistration` | Core (+ `attributeregistry.h`, `unitdefinitions.h`) | core |
| `altitudemarkerfeature.cpp` | `sessionmodel.h`, `markerregistry.h`, preferences, `QSettings`, `QColor` | core (Phase 3 must test its dynamic registration) |
| `pluginhost.cpp` | Python / pybind11 embed, `plotregistry.h`, `markerregistry.h`, `python_output_redirector.h`; **no UI includes** | stays in the executable (see Task 1.3, "Room for Phase 7") |

Nothing in the core set references `PluginHost`, `QApplication`, `QWidget`,
KDDockWidgets, WebEngine, QCustomPlot, or a `:/` resource. `gnsscalculations.cpp`
does **not** use GeographicLib; `sessionmodel.cpp` does use `UnitConverter`,
`AttributeRegistry`, `LogbookManager`, and `LogbookColumnStore`, all of which
are in the core set.

**Acceptance Criteria:**
- [ ] `git grep -n QMessageBox -- src/dataimporter.cpp src/sessionmodel.cpp` returns nothing.
- [ ] The application still compiles (verified in Task 1.3).

**Complexity:** S

---

### Task 1.3: Create the `flysight_core` static library and slim the executable

**Purpose:** Make the engine-adjacent code linkable by test executables without Widgets, WebEngine, KDDockWidgets, QCustomPlot, Multimedia, or Python.

**Files to modify:**
- `src/CMakeLists.txt` - add `Gui` to the Qt component list; keep `flysight_model`; add `flysight_core`; remove the moved entries from `PROJECT_SOURCES`; link `flysight_core` into `FlySightViewer`; add the test option/subdirectory hook (the hook itself is specified in Task 1.4).

**Technical Approach:**

1. **Two-tier layout (decision).** Keep the existing `flysight_model` target
   (`src/CMakeLists.txt` 234-241) exactly as it is - `sessiondata`,
   `dependencymanager`, `calculatedvalue`, Qt Core only - because
   `flysight_cpp_bridge` (407-416) links it and must not acquire Qt Gui /
   GeographicLib link dependencies or a second copy of more singletons. The
   only edit to it: add `dependencykey.h` to its source list (it is currently
   listed only under the executable at line 278; header-only, cosmetic, keeps it
   visible in the IDE under the right target).

2. **New target**, placed immediately after `flysight_model`:

   ```cmake
   add_library(flysight_core STATIC
     dataimporter.cpp dataimporter.h
     dataexporter.cpp dataexporter.h
     logbookmanager.cpp logbookmanager.h
     logbookcolumn.cpp logbookcolumn.h
     sessionmodel.cpp sessionmodel.h
     idlescheduler.cpp idlescheduler.h
     attributeregistry.cpp attributeregistry.h
     markerregistry.cpp markerregistry.h
     plotregistry.cpp plotregistry.h
     momentmodel.cpp momentmodel.h
     altitudemarkerfeature.cpp altitudemarkerfeature.h
     preferences/preferencesmanager.h        # header-only Q_OBJECT: must be listed for AUTOMOC
     preferences/preferencekeys.h
     units/unitdefinitions.h
     units/unitconversion.h                  # not listed anywhere today; add it here
     units/unitconverter.cpp units/unitconverter.h
     calculations/<all 15 .cpp/.h pairs currently at lines 326-340>
   )
   target_link_libraries(flysight_core
     PUBLIC  flysight_model Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui
     PRIVATE ${GeographicLib_LIBRARIES} Boost::boost)
   ```

   - The target must be declared **after** `find_package(GeographicLib ...)`
     (207) and `find_package(Boost REQUIRED)` (217); the existing position of
     `flysight_model` (after 232) satisfies that.
   - Include directories: `flysight_model` already exports
     `${CMAKE_CURRENT_SOURCE_DIR}` PUBLIC (240), which `flysight_core` inherits;
     the sources use `"../sessiondata.h"`-style and `"preferences/..."`-style
     includes that both resolve from there. No extra include dir is needed.
     GeographicLib/Boost are `PRIVATE` because only `.cpp` files include them;
     for a static library CMake still propagates them as link-only
     dependencies to consumers, which is what tests need.
   - `CMAKE_AUTOMOC` is already ON globally (17). `preferencesmanager.h` is a
     header-only `Q_OBJECT` singleton; it must appear in exactly **one**
     target's sources (this one) or its moc output is either missing or
     duplicated. Remove it from `PROJECT_SOURCES`.
   - Do **not** call `flysight_set_output_to_build_root` on the library.
   - Add `Gui` to the component list at line 97 (it is already found
     transitively through `Widgets`; listing it makes the dependency explicit).
     Do not add `Test` there - that belongs to `tests/CMakeLists.txt`.

3. **Executable.** Delete from `PROJECT_SOURCES` (244-342) every entry now in
   `flysight_core` or `flysight_model`: lines 247-248 (`sessionmodel`,
   `idlescheduler`), 250-251 (`dataimporter`, `dataexporter`), 262-263
   (`preferencesmanager.h`, `preferencekeys.h`), 276-277 (`units/*`), 278
   (`dependencykey.h`), 283-285 (`plotregistry`, `attributeregistry`,
   `logbookcolumn`), 287 (`logbookmanager`), 294 (`markerregistry`), 296
   (`momentmodel`), 297 (`altitudemarkerfeature`), 326-340 (`calculations/*`).
   Everything else (main, mainwindow, plot tools, preference **pages/dialogs**,
   `pluginhost`, `profile*`, `plotmodel`, `markermodel`, `plotutils`,
   `crosshairmanager`, all `ui/docks/*`, `resources.qrc`) stays.
   Add `flysight_core` as the first item of the executable's
   `target_link_libraries` (382-401); leave every other entry, including
   `flysight_model`, `${GeographicLib_LIBRARIES}` and `Boost::boost`, untouched
   so the final link line is a superset of today's.

4. **Bridge module.** `flysight_cpp_bridge` is untouched and keeps linking only
   `flysight_model` + Qt Core.

5. **If a core source fails to compile without Widgets** (a hidden include the
   audit missed): remove the include if unused; if it is used, do not add
   Widgets to the library - stop and record it under Open Questions. The audit
   found no such case.

6. **Room for Phase 7 (do not build now).** `pluginhost.cpp` and the
   `*_bindings.cpp` files stay where they are. Because `pluginhost.cpp` has no
   UI includes, Phase 7 can create an embedded-Python test by compiling
   `${FLYSIGHT_SRC_DIR}/pluginhost.cpp` directly into a test executable and
   linking `pybind11::embed`, using the `SOURCES`/`LIBS`/`ENVIRONMENT`
   arguments of `flysight_add_test()` (Task 1.4) and a `DEPENDS`-style
   `add_dependencies(<test> flysight_cpp_bridge)`. `tests/` is added with
   `add_subdirectory` *inside* the `src` project precisely so that
   `pybind11::embed`, `Python::Python`, `flysight_cpp_bridge`, and
   `flysight_msvc_fix_python_debug_autolink()` are all in scope there.
   Phase 1 must not move `pluginhost` into a library: that would force
   Python onto every test target.

**Acceptance Criteria:**
- [ ] `src/CMakeLists.txt` defines `flysight_core` as `STATIC` with exactly the source set above; `flysight_model`'s sources are unchanged except for the added `dependencykey.h`.
- [ ] `flysight_core` links only `flysight_model`, Qt Core, Qt Gui, GeographicLib, and `Boost::boost` - no Widgets, PrintSupport, Quick, Qml, WebEngine, WebChannel, Multimedia, KDDockWidgets, qcustomplot, pybind11, or Python (`cmake --graphviz` or inspection of the target's `LINK_LIBRARIES`).
- [ ] No source file appears in both `PROJECT_SOURCES` and a library.
- [ ] `cmake --build build --config Release` succeeds with zero new warnings about duplicate moc symbols; `build/install/FlySightViewer.exe` launches, shows the existing logbook, imports a file, plots it, and loads Python plugins exactly as before.
- [ ] `flysight_cpp_bridge`'s link inputs are unchanged.
- [ ] No `.cpp`/`.h` file under `src/` changed other than the two include deletions of Task 1.2 and the `LogbookManager::reset()` addition of Task 1.5.

**Complexity:** M

---

### Task 1.4: Add the `FLYSIGHT_BUILD_TESTS` option, `tests/` subdirectory, and CTest wiring

**Purpose:** Provide an opt-in test build that works from the superbuild, from a direct `-S src` configure, and in CI, with Windows DLL resolution handled by CTest.

**Files to modify:**
- `CMakeLists.txt` (root) - declare and forward the option.
- `src/CMakeLists.txt` - declare the option, `enable_testing()`, `add_subdirectory`.

**Files to create:**
- `tests/CMakeLists.txt`

**Technical Approach:**

1. **Option name: `FLYSIGHT_BUILD_TESTS`, default `OFF`.** OFF by default so
   release/packaging builds, install rules, CPack, and the Qt deploy script are
   byte-for-byte unaffected and Qt Test is not required to build the app.

2. **Root `CMakeLists.txt`:** next to the existing options (63-65) add
   `option(FLYSIGHT_BUILD_TESTS "Build the FlySight Viewer test suite" OFF)`;
   in the `_APP_CMAKE_ARGS` block (141-165) append
   `-DFLYSIGHT_BUILD_TESTS=${FLYSIGHT_BUILD_TESTS}` unconditionally (so turning
   it back OFF also propagates); add the option to the usage comment block
   (9-20), the options comment (49-61), and the summary printout (213-216).

3. **`src/CMakeLists.txt`:** place this after line 423 (the
   `qt_finalize_executable` block, i.e. after `flysight_core`, `pybind11`, and
   `flysight_cpp_bridge` all exist) and before the install rules (425):

   ```cmake
   option(FLYSIGHT_BUILD_TESTS "Build the FlySight Viewer test suite" OFF)
   if(FLYSIGHT_BUILD_TESTS)
     enable_testing()
     add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../tests" "${CMAKE_BINARY_DIR}/tests")
   endif()
   ```

   `enable_testing()` must be in `src/CMakeLists.txt` (the top of that build
   tree), not only in `tests/`, or `ctest --test-dir <build>` finds nothing.
   The explicit binary dir is mandatory because `../tests` is outside the
   source tree. Document the new option in the conventions comment block (31-54).

4. **`tests/CMakeLists.txt`** (not a standalone project - no `project()` call;
   unlike `master:tests/CMakeLists.txt`, sources are **not** compiled directly
   into tests, they come from `flysight_core`):
   - Guard: `if(NOT TARGET flysight_core) message(FATAL_ERROR "tests/ must be added from src/CMakeLists.txt with -DFLYSIGHT_BUILD_TESTS=ON") endif()`.
   - `find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)`.
   - `set(FLYSIGHT_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../src")` for later phases.
   - `add_library(flysight_test_support STATIC support/...)` (Task 1.5), linking
     `PUBLIC flysight_core Qt6::Test`, with `PUBLIC` include dir
     `${CMAKE_CURRENT_LIST_DIR}/support`.
   - A helper, used for every test now and later:

     ```cmake
     # flysight_add_test(<name> SOURCES <files...> [LIBS <targets...>] [ENVIRONMENT <VAR=value...>])
     ```

     It must: `add_executable(<name> ${SOURCES})`; link
     `flysight_test_support` + `LIBS`; `add_test(NAME <name> COMMAND <name>)`;
     set test property `ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${ENVIRONMENT}"`
     (pattern from `master:tests/CMakeLists.txt` last line); set
     `TIMEOUT 120`; set `LABELS core`; and apply the Windows DLL path below.
     Test executables are left in their default per-config output directory
     (`<build>/tests/Release/`); do **not** call
     `flysight_set_output_to_build_root` on them and do **not** add install rules.
   - **Windows DLL resolution.** Test executables need `Qt6Core.dll`,
     `Qt6Gui.dll`, `Qt6Test.dll`, and `GeographicLib.dll` at run time; nothing
     is deployed next to them (deployment happens only at install, for the app).
     Resolve via CTest, not by copying DLLs:

     ```cmake
     if(WIN32)
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
     ```

     (`add_test(NAME ...)` tests evaluate generator expressions in properties.)
     A `windeployqt` step is deliberately not used: the tests run as
     `QCoreApplication` and need no platform plugin (see Task 1.5), and PATH
     prepending keeps Debug/Release Qt DLL selection automatic because
     `TARGET_FILE_DIR` is the same `bin` for both. Debug-config tests
     additionally need a Debug GeographicLib (`GeographicLib_d.dll`); only
     Release is a supported test configuration on Windows - say so in the README.
   - Register the Phase 1 suite: `flysight_add_test(tst_harness SOURCES tst_harness.cpp)` and
     `flysight_add_test(tst_smoke SOURCES tst_smoke.cpp)` (Tasks 1.5, 1.6).
   - Naming convention for all later phases: one QObject test class per
     executable, file and target `tst_<area>.cpp` / `tst_<area>`.

5. **How to run** (goes into the README, Task 1.7):
   - Superbuild: configure as in `README.md` plus `-DFLYSIGHT_BUILD_TESTS=ON`,
     build with `cmake --build build --config Release`, then
     `ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure`.
   - Direct / CI: `ctest --test-dir build -C Release --output-on-failure` after
     configuring `-S src ... -DFLYSIGHT_BUILD_TESTS=ON`.

**Acceptance Criteria:**
- [ ] With the option absent or `OFF`, `tests/` is never entered, Qt Test is not searched for, and the generated app build is unchanged (no new targets in `build/FlySightViewer-build`).
- [ ] With `-DFLYSIGHT_BUILD_TESTS=ON` passed to the **root** configure, the inner cache (`build/FlySightViewer-build/CMakeCache.txt`) contains `FLYSIGHT_BUILD_TESTS:BOOL=ON` and the normal build command builds `tst_harness` and `tst_smoke`.
- [ ] `ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure` runs both tests from a shell whose `PATH` does **not** contain Qt or GeographicLib, and both pass.
- [ ] `ctest -N` lists exactly the tests registered through `flysight_add_test`, each with `QT_QPA_PLATFORM=offscreen` in its environment.
- [ ] No test target has an install rule; `cmake --install` output is identical with the option ON or OFF.

**Complexity:** M

---

### Task 1.5: Shared test-support library (isolation, temp logbook, fixture builder)

**Purpose:** Guarantee that no test can read or write the user's preferences (Windows registry `HKCU\Software\FlySight\FlySightViewer`) or logbook (`Documents/FlySight Viewer/logbook`), and give every later phase one way to generate input files.

**Files to create:**
- `tests/support/testenvironment.h` / `.cpp` - process-wide isolation + logbook control.
- `tests/support/testmain.h` - `FLYSIGHT_TEST_MAIN` macro.
- `tests/support/fixturebuilder.h` / `.cpp` - FS2 / FS1 file builders and file helpers.
- `tests/tst_harness.cpp` - tests of the support code itself.

**Files to modify:**
- `src/logbookmanager.h` / `src/logbookmanager.cpp` - add one public method `void reset();`.

**Technical Approach:**

Everything lives in `namespace FlySightTest`.

1. **Why isolation needs care here.**
   - `PreferencesManager` (`src/preferences/preferencesmanager.h` 18-21, 72-74)
     is a Meyers singleton whose `QSettings m_settings` is default-constructed
     on the **first** `instance()` call, binding to whatever
     organization/application/format is current at that moment. `main.cpp`
     16-18 uses `FlySight` / `flysight.ca` / `FlySightViewer`; on Windows the
     default format is the registry.
   - `LogbookColumnStore` (`src/logbookcolumn.cpp` 144, 180) and
     `AltitudeMarkerManager` (`src/altitudemarkerfeature.cpp` 43, 165) create
     their own default `QSettings`.
   - `registerPreference()` **writes** the default into `QSettings` when the key
     is missing (26-29), and `getValue()` on an unregistered key hits
     `Q_ASSERT(false)` (33-36).
   - `LogbookManager::logbookDirectory()` (baseline `logbookmanager.cpp`
     130-135) is `<GeneralLogbookFolder preference> + "/FlySight Viewer/logbook"`,
     re-read on every call; `sessionsDirectory()` (137-142) `mkpath`s it. The
     preference's app default is `QStandardPaths::DocumentsLocation`
     (baseline `mainwindow.cpp` 1025-1026), which `QStandardPaths` test mode does
     **not** redirect on Windows. `LogbookManager` is a singleton holding
     `m_sessionIdToUuid`, `m_lastAccessed`, `m_cachedValues`, `m_hasIndexData`,
     `m_deferredScan`, `m_scannedUuids` with no way to clear them.
   - `UnitConverter`'s constructor reads `GeneralUnits`
     (`src/units/unitconverter.cpp` 10-20) and `SessionModel`'s constructor
     touches `LogbookColumnStore`, `UnitConverter`, and registers
     `LogbookCacheSize` (`src/sessionmodel.cpp` 18-131) - so preferences must be
     registered before a `SessionModel` is constructed.

2. **`class TestEnvironment`** (non-copyable; exactly one per process, created
   in `main` before any application singleton is touched):

   ```cpp
   class TestEnvironment {
   public:
       explicit TestEnvironment(const QString &testName);  // performs all isolation steps
       ~TestEnvironment();
       static TestEnvironment &instance();                 // asserts one exists

       QString rootPath() const;        // the process-wide QTemporaryDir
       QString settingsPath() const;    // <root>/settings
       QString logbookFolder() const;   // value of general/logbookFolder
       QString logbookDir() const;      // logbookFolder() + "/FlySight Viewer/logbook"
       QString sessionsDir() const;     // logbookDir() + "/sessions"
       QString indexPath() const;       // logbookDir() + "/index.json"
       QString newTempDir(const QString &prefix = {});  // fresh subdir of rootPath()

       void useFreshLogbook();          // new empty logbook folder + LogbookManager::reset()
       void reopenLogbook();            // LogbookManager::reset() keeping the folder (simulates restart)
       void registerCorePreferences();  // idempotent; called by the constructor
       void registerBuiltIns();         // once per process: attributes + built-in calculations
       void resetPreferencesToDefaults();
   };
   ```

   Constructor steps, in this order:
   1. `QCoreApplication::setOrganizationName("FlySightTests")`,
      `setOrganizationDomain("tests.flysight.invalid")`,
      `setApplicationName(testName)` - never the real names.
   2. `QStandardPaths::setTestModeEnabled(true)`.
   3. Create the root `QTemporaryDir`; fail hard (`qFatal`) if invalid.
   4. `QSettings::setDefaultFormat(QSettings::IniFormat)` and
      `QSettings::setPath(QSettings::IniFormat, scope, settingsPath())` for
      **both** `QSettings::UserScope` and `QSettings::SystemScope` (the second
      stops fallback reads of machine-wide settings). Pattern:
      `master:tests/dataimporter_test.cpp` `main()`.
   5. **Guard:** construct `QSettings probe;` and `qFatal` unless
      `probe.format() == QSettings::IniFormat` and
      `QDir::cleanPath(probe.fileName())` starts with `rootPath()`. This is the
      tripwire that makes "never touches the user's preferences" verifiable.
   6. `registerCorePreferences()` then `useFreshLogbook()`.
   7. **Guard:** `qFatal` unless `logbookDir()` starts with `rootPath()`.

   `registerCorePreferences()` registers, with these literal defaults (mirror of
   baseline `mainwindow.cpp` 1017-1115, restricted to keys the core library
   reads): `GeneralUnits`="Metric"; `GeneralLogbookFolder`=`<root>/logbook-0`
   (never Documents); `ImportGroundReferenceMode`="Automatic";
   `ImportFixedElevation`=0.0; `ImportDescentPauseSeconds`=30.0;
   `ImportHideOthersOnImport`=false; `AeroMass`=1.0; `AeroArea`=1.0.
   (`LogbookCacheSize`, `LogbookColumnsVersion`, and the `AltitudeMarkers*` keys
   self-register in `sessionmodel.cpp` 119, `logbookcolumn.cpp` 113,
   `altitudemarkerfeature.cpp` 20-23.) Per-plot / per-marker colour keys are UI
   preferences and are not registered. `resetPreferencesToDefaults()` calls
   `setValue(key, getDefaultValue(key))` for those keys, except that
   `GeneralLogbookFolder` keeps its current test value.

   `useFreshLogbook()` sets `GeneralLogbookFolder` to a new
   `<root>/logbook-<n>` via `PreferencesManager::setValue`, then
   `LogbookManager::instance().reset()`. It does not call `initialize()`; tests
   call that explicitly, as `MainWindow` does (baseline `mainwindow.cpp` 168-170).

   `registerBuiltIns()` calls `FlySight::registerBuiltInAttributes()`
   (`src/calculations/attributeregistration.h`) and
   `CalculatedValueRegistry::instance().registerBuiltInCalculations()` behind a
   process-wide `static bool` - registrations are global statics and must not be
   duplicated. It deliberately does **not** reproduce
   `MainWindow::registerBuiltInPlots/Markers` (UI-owned, baseline
   `mainwindow.cpp` 895-1015); tests that need a marker or plot register their own.

3. **`LogbookManager::reset()`** (the only functional source addition in this
   phase; not called by the application): clears `m_sessionIdToUuid`,
   `m_lastAccessed`, `m_cachedValues`, `m_scannedUuids` and sets
   `m_hasIndexData = m_deferredScan = false`. Header comment: "Drops all
   in-memory index state so the next initialize() re-reads the current logbook
   folder. Used by tests to simulate an application restart." Touches no files.

4. **`FLYSIGHT_TEST_MAIN(TestClass)`** in `testmain.h` expands to a `main` that
   constructs `QCoreApplication app(argc, argv)`, then
   `FlySightTest::TestEnvironment env(QStringLiteral(#TestClass))`, then
   `TestClass tc; return QTest::qExec(&tc, argc, argv);`. `QCoreApplication`
   (not `QApplication`) is sufficient: `QColor`/`QAbstractTableModel`/`QTimer`
   need no GUI application, and avoiding a platform plugin removes the Windows
   `platforms/` deployment problem. `QT_QPA_PLATFORM=offscreen` is still set by
   CTest so that a later test may switch its own `main` to `QGuiApplication`
   without harness changes. Never use `QTEST_MAIN`/`QTEST_GUILESS_MAIN` in this
   project: they construct the application but give no hook before singletons.
   Each test file ends with `#include "tst_<name>.moc"`.

5. **Event-loop helper** in `testenvironment.h`:
   `bool waitForIdle(FlySight::SessionModel &model, int timeoutMs = 5000);` -
   spins the event loop (`QSignalSpy` on `IdleScheduler::schedulerIdle`, see
   `src/idlescheduler.h`) until the model's scheduler reports idle; returns
   false on timeout. Later phases use it for deferred saves and column fills.

6. **Fixture builder** (`fixturebuilder.h`). Values are passed as **text**, so a
   test controls the exact bytes in the file (required for the round-trip and
   precision tests of Phase 5) and expected values stay literals:

   ```cpp
   class Fs2FileBuilder {
   public:
       Fs2FileBuilder &flysVersion(const QByteArray &v);               // default "1" -> "$FLYS,1"
       Fs2FileBuilder &var(const QByteArray &key, const QByteArray &value);   // "$VAR,key,value", in call order
       Fs2FileBuilder &sensor(const QByteArray &name,
                              const QList<QByteArray> &columns,
                              const QList<QByteArray> &units);        // "$COL,.." + "$UNIT,.."; units may be shorter/empty -> no $UNIT line if empty
       Fs2FileBuilder &row(const QByteArray &sensor, const QByteArray &csvValues); // "$IMU,3,-125,..."
       Fs2FileBuilder &rawHeaderLine(const QByteArray &line);          // emitted verbatim before $DATA (malformed-input tests)
       Fs2FileBuilder &rawDataLine(const QByteArray &line);            // emitted verbatim after $DATA
       Fs2FileBuilder &lineEnding(const QByteArray &eol);              // default "\n"; "\r\n" allowed
       QByteArray toBytes() const;   // $FLYS, $VARs, $COL/$UNIT per sensor in call order, $DATA, rows in call order
       bool write(const QString &path) const;
   };

   class Fs1FileBuilder {            // "time,lat,lon,hMSL,..." format (dataimporter.cpp 61, 139-186)
   public:
       Fs1FileBuilder &columns(const QList<QByteArray> &names);  // must start time,lat,lon,hMSL for sniffing
       Fs1FileBuilder &units(const QList<QByteArray> &units);
       Fs1FileBuilder &row(const QByteArray &csvValues);
       QByteArray toBytes() const;
       bool write(const QString &path) const;
   };

   bool writeFile(const QString &path, const QByteArray &contents);   // from master:tests/dataimporter_test.cpp
   QByteArray readFileBytes(const QString &path);

   namespace Fixtures {   // canned, deliberately tiny, used by smoke tests and later phases
       Fs2FileBuilder sensorFile(const QByteArray &sessionId = "test-session");
       Fs2FileBuilder trackFile(const QByteArray &sessionId = "test-session");
   }
   ```

   `Fixtures::sensorFile()` contents (adapted from
   `master:tests/dataimporter_test.cpp` `sensorFile()`, gyro columns
   deliberately out of order, **no `SCHEMA_VER`**):
   `$VAR FIRMWARE_VER=v2023.09.22, SESSION_ID=<id>, DEVICE_ID=test-device`;
   `IMU`: columns `time,wy,ax,wz,wx,temperature`, units `s,deg/s,g,deg/s,deg/s,deg C`,
   row `3,-125,1,0,62.5,40`; `MAG`: columns `time,x,y,z,temperature`, units
   `s,gauss,gauss,gauss,deg C`, row `3,1,0,-0.5,40`.
   `Fixtures::trackFile()`: same three `$VAR`s; `GNSS`: columns
   `time,lat,lon,hMSL,velN,velE,velD,hAcc,vAcc,sAcc,numSV`, units
   `,deg,deg,m,m/s,m/s,m/s,m,m,m,`, three rows with ISO `Z` timestamps
   (`2024-01-01T12:00:00.000Z`, `.200Z`, `.400Z`) and plain literal values.
   Both always carry `DEVICE_ID` so `DataImporter::extractDeviceId`
   (baseline 368-422) never walks parent directories looking for `FLYSIGHT.TXT`.

7. **`tst_harness.cpp`** (class `HarnessTest`) verifies the support code:
   - `settingsAreIsolated`: `QSettings().fileName()` is under `rootPath()` and is
     an INI file; organization is `FlySightTests`.
   - `logbookIsIsolated`: `logbookDir()` is under `rootPath()`; after
     `LogbookManager::instance().initialize()` on a fresh logbook,
     `hasIndexData()` is false and `hasDeferredScan()` is true with empty
     `scannedUuids()`.
   - `freshLogbookChangesFolder`: two `useFreshLogbook()` calls give different folders.
   - `fs2BuilderBytes`: `toBytes()` of a small builder equals an expected literal `QByteArray`.
   - `fs1BuilderBytes`: same for FS1.
   - `preferencesRegistered`: `getValue` of each core key returns the literal default above.

**Acceptance Criteria:**
- [ ] `tst_harness` passes under CTest.
- [ ] Running the full test suite creates/modifies no key under `HKCU\Software\FlySight` and no file under the user's `Documents/FlySight Viewer` (verify once manually: note the registry key's and folder's last-modified time before and after).
- [ ] Constructing `TestEnvironment` with the INI redirect sabotaged (e.g. temporarily commenting out `setDefaultFormat`) aborts with `qFatal` - checked once by hand, not committed.
- [ ] `LogbookManager::reset()` exists, is not called anywhere under `src/`, and is the only change to `logbookmanager.*`.
- [ ] No test-support header includes any Widgets, WebEngine, KDDockWidgets, or pybind11 header.
- [ ] `fixturebuilder` output contains exactly the lines requested - no implicit `$VAR`, no reformatting of numeric text.

**Complexity:** L (split naturally into: environment + macro; `LogbookManager::reset`; fixture builder; `tst_harness`)

---

### Task 1.6: Baseline smoke (characterization) suite

**Purpose:** Prove the harness can drive importer, session, calculations, exporter, logbook, and model end to end, and pin today's behavior so later phases change it on purpose.

**Files to create:**
- `tests/tst_smoke.cpp` (class `SmokeTest`, `FLYSIGHT_TEST_MAIN(SmokeTest)`).

**Technical Approach:**

Every expectation is a literal. Each test function that encodes behavior a
later phase will change carries the comment
`// BASELINE: <what> - changes in Phase <n>` so the later implementer finds it
with one grep. `initTestCase()` calls `TestEnvironment::instance().registerBuiltIns()`;
`init()` calls `useFreshLogbook()` and `resetPreferencesToDefaults()`.

| Test | Steps | Expected (baseline) |
|---|---|---|
| `importFs2Sensor` | write `Fixtures::sensorFile()` to `<newTempDir>/sensor.csv`; `DataImporter::importFile` | returns true; `sensorKeys()` contains `IMU`,`MAG`; `IMU/time`=3.0; `IMU/wx`=62.5, `wy`=-125.0, `wz`=0.0 (**BASELINE: no gyro correction - Phase 4 -> 71.68 / -143.36**); `IMU/ax`=9.80665 with `getUnit`="m/s^2" (**BASELINE: converted at import and unit relabelled - Phase 4**); `MAG/x`=0.0001, unit "T"; `IMU/temperature`=40.0, unit "degC"; `hasAttribute("SCHEMA_VER")` false; `FIRMWARE_VER`="v2023.09.22"; `SESSION_ID`="test-session"; `DEVICE_ID`="test-device"; input file bytes unchanged |
| `importAppliesCreationDefaults` | same import | `_DESCRIPTION`="sensor.csv" (file name, per `getDescription` baseline 449-483); `_IMPORT_TIME` present and > 0; `_WIND_N`=`_WIND_E`=0.0; `_JUMPER_MASS`=`_PLANFORM_AREA`=1.0; `_GROUND_ELEV` absent as a stored attribute in "Automatic" mode |
| `importFs2Track` | `Fixtures::trackFile()` | `GNSS/hMSL`, `velN` equal the literals; `GNSS/time` first value = 1704110400.0, third = 1704110400.4 (compare with `qAbs(diff) < 1e-6`) |
| `importFs1` | `Fs1FileBuilder` with `time,lat,lon,hMSL,velN,velE,velD,hAcc,vAcc,sAcc,heading,cAcc,gpsFix,numSV` + unit line + 2 rows | sensor `GNSS` exists with the literal values; `SESSION_ID` is a 32-char hex MD5 (synthesized, baseline 108-113) |
| `rejectsUnknownFormat` | file starting `hello` | `importFile` false; `getLastError()`="Unknown file format"; session has no sensors |
| `derivedMeasurement` | import sensor file | `getMeasurement("IMU","wTotal")` has one element equal to 139.7542486... computed by hand: `sqrt(62.5^2+125^2+0^2)` = 139.75424859373686 (compare to 1e-9) (**BASELINE - Phase 4 scales by 1.14688**); `hasMeasurement("IMU","wTotal")` is false |
| `exportReloadRoundTrip` | `DataExporter::exportSession` to a temp path; `DataImporter::readFile` into a new session | same sensor/measurement key sets; values equal within 1e-12; units `m/s^2`, `T`, `degC`, `deg/s`; file starts with `$FLYS,1\n`; file contains no `SCHEMA_VER`; a second export of the reloaded session is byte-identical to the first |
| `logbookSaveReload` | `LogbookManager::initialize()`; `saveSession(session)`; `flushIndex()`; `reopenLogbook()`; `initialize()`; `loadSession("test-session")` | `saveSession` true; exactly one `*.csv` in `sessionsDir()`; `indexPath()` exists; after reopen `hasIndexData()` true; loaded optional has value; `IMU/wx`=62.5; `FIRMWARE_VER` preserved; nothing exists outside `rootPath()` |
| `modelMergeSavesToTempLogbook` | construct `SessionModel`; `mergeSessions({track})`; `waitForIdle(model)` (or `flushDirtySessions()`) | `rowCount()`==1; `getSessionRow("test-session")`==0; a session CSV exists under `sessionsDir()` |
| `modelMergesTrackAndSensor` | `mergeSessions({track})` then `mergeSessions({sensor})` with equal `SESSION_ID` | still one row; `sessionRef(0)` has sensors `GNSS`, `IMU`, `MAG` (**BASELINE: merge rules change in Phase 6; keep this test minimal - do not assert on attribute overwrite behavior**) |

Do not assert anything about gyro-scaling preferences, schema stamping, or
`dataSchemaVersion` (none exist at baseline). Do not test UI classes.

**Acceptance Criteria:**
- [ ] `tst_smoke` passes under CTest on Windows Release.
- [ ] Every expected number in the file is a literal; no expectation is obtained by calling the code under test.
- [ ] `git grep -n "BASELINE:" tests/` lists each expectation that Phases 4-6 are known to change (gyro values, import-time unit conversion/relabel, derived `wTotal`, merge behavior).
- [ ] All files the suite writes are under `TestEnvironment::rootPath()`.

**Complexity:** M

---

### Task 1.7: Documentation and optional CI hook

**Purpose:** Tell humans and CI how to build and run the tests; satisfy the spec's "document how to run the tests" at harness level.

**Files to create:**
- `tests/README.md`

**Files to modify:**
- `README.md` (baseline version) - "Build Options" table: add `FLYSIGHT_BUILD_TESTS` (`OFF`, "Build the Qt Test suite under `tests/`; run with CTest"); add a short "Running the tests" subsection pointing to `tests/README.md`; update the project-structure section to list `tests/` and mention the `flysight_model` / `flysight_core` libraries.
- `.github/workflows/build.yml` - optional, minimal (see below).

**Technical Approach:**

`tests/README.md` sections:
1. *What this is* - Qt Test executables linked against `flysight_core`; no UI, no WebEngine, no Python (until Phase 7).
2. *Configure / build / run* - Windows superbuild commands (copy the configure line from `README.md` "Application Only Build" and append `-DFLYSIGHT_BUILD_TESTS=ON`; build with `cmake --build build --config Release`; run `ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure`); the direct `-S src` variant; macOS/Linux equivalents without `--config`/`-C`.
3. *Running one test / one function* - `ctest -R tst_smoke`, and running the executable directly (`tst_smoke.exe importFs2Sensor -v2`) with the note that outside CTest on Windows the Qt `bin` and `third-party/GeographicLib-install/bin` directories must be on `PATH`. Release only on Windows.
4. *Isolation guarantees* - temp INI settings, test organization name, temp logbook folder, the `qFatal` guards; "tests never touch your preferences or logbook".
5. *Writing a test* - `FLYSIGHT_TEST_MAIN`, `TestEnvironment`, `Fs2FileBuilder`/`Fs1FileBuilder`, `flysight_add_test()`, the `tst_<area>` naming rule, literals-only expectations, the `// BASELINE:` convention.
6. *Adding tests that need Python (Phase 7)* - one paragraph: pass `SOURCES ${FLYSIGHT_SRC_DIR}/pluginhost.cpp`, `LIBS pybind11::embed`, and `ENVIRONMENT` entries; not implemented yet.

Do not copy the "Coverage includes ..." paragraph from `master:tests/README.md` (it describes the superseded gyro-scaling feature).

**CI (keep minimal):** in `.github/workflows/build.yml`, add
`-DFLYSIGHT_BUILD_TESTS=ON` to the "Configure main application" `cmake` command
and insert one step between "Build main application" and "Install application":

```yaml
      - name: Run tests
        shell: bash
        run: ctest --test-dir build --output-on-failure -C Release
```

The Ninja single-config build ignores `-C`. `install-qt-action` already puts
Qt's `bin` on `PATH`, and the `ENVIRONMENT_MODIFICATION` property covers
GeographicLib on Windows; on Linux/macOS the build-tree RPATH covers it
(note: macOS sets `BUILD_RPATH` only on `FlySightViewer`; if the macOS test step
fails to locate `libGeographicLib`, set
`BUILD_RPATH "${GEOGRAPHIC_ROOT}/lib"` on test targets inside
`flysight_add_test` under `if(APPLE)`). Tests add no install rules, so
packaging, signing, and notarization steps are unaffected. Because a workflow
change can only be validated by pushing, and pushing is not part of this phase,
make the edit as its own commit and tell Michael it is unverified; if he prefers,
leave CI untouched and record that in the phase report.

**Acceptance Criteria:**
- [ ] `tests/README.md` exists with the six sections; every command in it was executed once on Windows and works verbatim.
- [ ] `README.md` lists `FLYSIGHT_BUILD_TESTS` and links to `tests/README.md`; no mention of gyro scaling or `docs/DATA_SCHEMA.md` is introduced.
- [ ] If the workflow was edited: the diff is limited to one added configure flag and one added step, in a separate commit.

**Complexity:** S

---

## Testing Requirements

### Unit Tests
- New: `tst_harness` (support-code self-tests, Task 1.5) and `tst_smoke` (baseline characterization, Task 1.6). No tests exist at baseline, so none need updating.

### Integration Tests
- Configure the superbuild twice in the existing `build/` directory - once with `-DFLYSIGHT_BUILD_TESTS=ON`, once with `OFF` - and confirm the inner cache follows and the app builds both times.
- `ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure` from a clean shell (no Qt on `PATH`): 2/2 pass.
- Optional: a direct `cmake -S src -B build-core-tests ... -DFLYSIGHT_BUILD_TESTS=ON` configure mirrors CI.

### Manual Verification
1. Build with the normal command; launch `build/install/FlySightViewer.exe`.
2. Existing logbook appears with cached column values; open a session; plots, map, markers, legend render.
3. Import a FlySight 2 `TRACK.CSV` + `SENSOR.CSV` pair; confirm a single merged session; restart and confirm it persisted.
4. Preferences dialog opens; change units Metric/Imperial and see logbook columns reformat (exercises `UnitConverter` -> `SessionModel` across the new library boundary).
5. Altitude markers appear when enabled in preferences (exercises `AltitudeMarkerManager` from the library).
6. Python plugins under `python_plugins/` still load (bridge module untouched).
7. Note the modification time of `Documents/FlySight Viewer/logbook/index.json` and export `HKCU\Software\FlySight` before and after a full `ctest` run: unchanged.

## Notes for Implementer

### Gotchas
- **Header-only `Q_OBJECT`.** `preferencesmanager.h` must be listed in exactly one target. Forgetting it gives unresolved `PreferencesManager::staticMetaObject`; listing it twice gives duplicate symbols.
- **Singleton order.** The first `PreferencesManager::instance()` call freezes the `QSettings` backend. Nothing in a test binary may touch an application singleton before `TestEnvironment` is constructed - including global/static initializers in test files.
- **`getValue` asserts on unregistered keys** in Debug. Any new core code path a test exercises may need its key added to `registerCorePreferences()`.
- **`QStandardPaths` test mode does not redirect Documents on Windows** - which is why the logbook folder preference is always set explicitly and guarded.
- **Global registrations are per process and cannot be undone at baseline** (`CalculatedValue::s_methods`, `AttributeRegistry`, `MarkerRegistry`). Call `registerBuiltIns()` only through the guarded helper; keep one test class per executable.
- **`MarkerRegistry` <-> `MomentModel` <-> `SessionModel`** form an include cycle at the `.cpp` level; that is fine inside one static library but is the reason `momentmodel` must move with them.
- **Static-library link order** is handled by CMake because `flysight_core` declares `flysight_model` as a dependency; do not list raw `.lib` paths.
- **Out-of-tree `add_subdirectory`** requires the explicit binary directory argument.
- **`BUILD_ALWAYS TRUE`** on the ExternalProject means the inner install step runs on every build; tests must never gain install rules or they would land in `build/install` and in release packages.
- **GeographicLib is a DLL on Windows** and only a Release build is present in `third-party/GeographicLib-install/bin`; Debug test runs are unsupported.
- `DataImporter::m_lastError` is not cleared at entry (baseline); in tests use a fresh `DataImporter` per import when asserting on error text.
- `importFile` on a fixture without `DEVICE_ID` walks up parent directories for `FLYSIGHT.TXT` (read-only, but non-hermetic); the canned fixtures always include `DEVICE_ID`.
- `src/CMakePresets.json` hard-codes Qt 6.9.2 and is unused by the documented workflow; leave it alone.

### Decisions Made
- **Branch:** `schema-and-calculations`, created locally from `v2026.04.1`, not pushed.
- **Library:** new `flysight_core` (STATIC) layered on the unchanged `flysight_model`, rather than growing `flysight_model`, so the pybind11 bridge module's link closure and singleton footprint stay exactly as released. Later phases put engine code that `SessionData` depends on into `flysight_model` and everything else into `flysight_core`; if Phase 3 makes `SessionData` need `PreferencesManager` directly, that phase moves `preferencesmanager.h` down into `flysight_model`.
- **Boundary:** includes `MomentModel`, the three registries, `AltitudeMarkerManager`, `UnitConverter`, and `attributeregistration` in addition to the items the assignment listed, because the include audit shows they are required by (or needed to test) `SessionModel`, `LogbookColumn`, WSP/SP calculations, and Phase 3's altitude-marker work. `PluginHost` and the bindings stay in the executable/bridge.
- **Qt modules for the library:** Core + Gui (for `QColor`); no Widgets. The only blockers were two unused `<QMessageBox>` includes.
- **Option:** `FLYSIGHT_BUILD_TESTS`, default OFF, defined in `src/CMakeLists.txt` and declared/forwarded by the root superbuild.
- **Tests are in-tree targets of the `src` project** (not a standalone project as on `master`), linking the library instead of recompiling sources.
- **Windows DLLs via CTest `ENVIRONMENT_MODIFICATION`** (CMake >= 3.22) rather than `windeployqt` or DLL copying; tests run as `QCoreApplication` so no platform plugin is needed, with `QT_QPA_PLATFORM=offscreen` still exported for future GUI-application tests.
- **Core preferences are registered by test support with literal defaults** instead of extracting a shared function from `MainWindow::initializePreferences`, to keep application source changes in this phase to two include deletions plus `LogbookManager::reset()`.
- **`LogbookManager::reset()`** is added as the single test seam, because the singleton otherwise cannot simulate a restart or a second logbook within one process (needed by acceptance items 5-8 and 18 later).
- **Fixture values are text**, not doubles, so tests own the exact bytes written.
- **CI:** one flag plus one `ctest` step, optional and committed separately because it cannot be verified without pushing.

### Open Questions
- Should CI run the tests from Phase 1 onward, or only once the suite is meaningful (Phase 2+)? The document defaults to adding the step but leaves the final call to Michael since it requires a push to validate.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria.
2. `tst_harness` and `tst_smoke` pass via CTest on Windows Release, and the application builds with `FLYSIGHT_BUILD_TESTS` both ON and OFF.
3. The application behaves identically to `v2026.04.1` (manual verification list above).
4. The user's preferences and logbook are demonstrably untouched by a full test run.
5. Code follows the patterns in the reference files; no TODOs or placeholder code remain; nothing from commit `4668f48` has been carried over; nothing has been pushed.
