# Phase 7: Python plugin bridge

## Overview

This phase finishes the Python side of the calculation engine. `AttributePlugin`
and `MeasurementPlugin` stay the simple single-output forms they are today, now
with effective reads, declared-input enforcement that is visible (and
debuggable) from Python, copied NumPy results, and clean failure behavior. A
small multi-output form (`CalculationPlugin`) is added with one bundled
example, plugins get explicit source access with C++ semantics, dependency
kinds are decoded through one explicit table, and the whole bridge is exercised
by a test target that boots the real embedded interpreter and imports the real
`flysight_cpp_bridge` module.

Nothing here changes what ordinary users see: no plugin is auto-loaded in the
shipped tree before or after this phase.

## Dependencies

- **Depends on:** Phase 4 (and 1-3).
- **Blocks:** Phase 8.
- **Runs in parallel with:** Phases 5 and 6. Do **not** edit
  `src/dataexporter.*`, `src/logbookmanager.*`, `src/sessionmodel.*`,
  `src/dataimporter.*`, or `src/mainwindow.cpp`. Shared files that this phase
  *does* edit (`src/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/README.md`, and - shared
  with Phase 5 Task 5.5 - `src/engine/calculationregistry.*`,
  `tests/tst_calcregistry.cpp`)
  must be edited as self-contained appended blocks so merges stay trivial.
- **Assumptions (state at the start of this phase):**
  - Branch `schema-and-calculations`. The plugin-related files
    (`src/pluginhost.*`, `src/*_bindings.cpp`, `src/cpp_bridge.cpp`,
    `src/bridgeimpl.h`, `src/python_output_redirector.h`,
    `python_plugins/flysight_plugin_sdk.py`, `cmake/*`, `.github/workflows/build.yml`)
    are identical at `v2026.04.1` and on `master`; line numbers cited for them
    are valid on either, **before** Phase 3's edits. Phase 3 Task 3.8 has since
    rewritten `pluginhost.cpp` sections 4-5 (old 277-364) and
    `sessiondata_bindings.cpp`; cite-by-line for those regions refers to the
    baseline text so you can recognize what was replaced.
  - Phase 3 Task 3.8 left: `src/pluginsessionview.h` (header-only
    `PluginSessionView` over `const EvaluationContext *`, bound in the bridge
    under the Python name `SessionData` with a `std::shared_ptr` holder and
    `invalidate()`), single-output adapters registered with
    `CalculationRegistry::instance()` under ids `plugin.attr.<i>.<name>` and
    `plugin.meas.<i>.<sensor>/<name>`, the `setCalculatedMeasurement` binding
    deleted, inputs still decoded by "`Attribute`, else measurement", no
    `try/catch` around `compute`, no output validation.
  - Engine names used verbatim from Phase 2: `CalculationDescriptor`
    (`id`, `inputs`, `outputs`, `policy`, `compute`), `CalcInput` (kinds
    `Attribute`, `Measurement`, `Preference`, `SourceMeasurement`, `SourceUnit`),
    `CalculationResult` (`setAttribute`, `setMeasurement(sensor, name, values, unit)`,
    `unavailable()`), `EvaluationContext` (`attribute`, `measurement`,
    `measurementUnit`, `sourceMeasurement`, `sourceUnit`), `ResultStatus`
    (`Ok`, `MissingInput`, `Failed`, `UndeclaredRead`, `InvalidOutput`, ...),
    `CalculationEngine` (`runCount`, `runCountForInstance`, `resultStatus`,
    `undeclaredReadCount`, `lastUndeclaredRead`, `cachedState`),
    `CalculationRegistry` (`registerCalculation`, `hasCandidateFor`,
    `registeredIds`).
  - Phase 4 names: `SessionData::getMeasurement` (effective),
    `effectiveUnit`, `sourceMeasurement`, `sourceUnit`, `hasSourceMeasurement`,
    `setSourceMeasurement`; conversion families `builtin.conversion.schema` /
    `builtin.conversion.default`; ordinary `EvaluationContext::measurement`
    reads are already effective.
  - The registry currently **rejects** `SourceMeasurement` / `SourceUnit`
    inputs on anything not registered through `registerSourceConversion`
    (Phase 2 Task 2.3). Task 7.1 adds the opt-in this phase needs.
  - **Two copies of `flysight_model` statics exist** - one in the executable
    (or test executable), one in `flysight_cpp_bridge` (`.pyd` / `.so`). Code
    compiled into the bridge must never call `CalculationRegistry::instance()`,
    construct a `SessionData` or engine, or rely on any static. It may only
    operate on objects handed to it (`EvaluationContext` through
    `PluginSessionView`). Everything in this document respects that.
  - Startup order (Phase 3 Task 3.9): `PluginHost::initialise()` runs before
    `registerBuiltInCalculations()`, so plugin candidates precede built-in
    candidates for the same output.
  - Test harness per Phase 1: `flysight_add_test(<name> SOURCES ... LIBS ... ENVIRONMENT ...)`,
    `FLYSIGHT_TEST_MAIN`, `TestEnvironment` (`newTempDir`, `registerBuiltIns`),
    `Fs2FileBuilder`; `tests/` is added from inside the `src` project so
    `pybind11::embed`, `Python::Python`, `flysight_cpp_bridge`,
    `flysight_msvc_fix_python_debug_autolink()` and the `Python_*` variables
    are in scope.

## Design summary (read before the tasks)

### Python-visible surface after this phase

| Thing | Name | Notes |
|---|---|---|
| Key type (SDK, pure Python) | `Key` - frozen dataclass `(kind: str, sensor: str = "", name: str = "")` | hashable / comparable by value; replaces the bridge-bound `DependencyKey` |
| Key helpers | `attr(name)`, `meas(sensor, name)`, **new** `source(sensor, name)` | `attr` / `meas` call sites are unchanged |
| Kind constants | `KIND_ATTRIBUTE = "attribute"`, `KIND_MEASUREMENT = "measurement"`, `KIND_SOURCE = "source"` | |
| Single-output forms | `AttributePlugin`, `MeasurementPlugin`, `register_attribute`, `register_measurement` | unchanged API |
| Multi-output form | **new** `CalculationPlugin`, `register_calculation`, SDK list `_calculations` | ids `plugin.calc.<i>.<name>` |
| Session view (Python type name stays `SessionData`) | `getMeasurement`, `getAttribute`, `hasMeasurement`, `hasAttribute`, **new** `effectiveUnit`, `sourceMeasurement`, `sourceUnit`, `hasSourceMeasurement` | every read must be declared |
| Exceptions (bridge module) | `flysight_cpp_bridge.UndeclaredInputError` (subclass of `RuntimeError`); stale view -> `RuntimeError` | re-exported by the SDK |
| Removed | `flysight_cpp_bridge.DependencyKey`, `SessionData.setCalculatedMeasurement` (already gone in Phase 3), the unregistered `Default*` example classes in the SDK | |

### Key decoding table (the only place kinds are interpreted)

`Key.kind` must be an exact Python `str`. Anything else, any other string, or a
missing / non-`str` / empty required field is a **registration error for that
plugin** (Task 7.3).

| `kind` | Required fields | In `inputs()` becomes | Allowed in `outputs()` |
|---|---|---|---|
| `"attribute"` | `name` non-empty, `sensor == ""` | `CalcInput::attribute(name)` | yes -> `DependencyKey::attribute(name)` |
| `"measurement"` | `sensor`, `name` non-empty | `CalcInput::measurement(sensor, name)` | yes -> `DependencyKey::measurement(sensor, name)` |
| `"source"` | `sensor`, `name` non-empty | **two** inputs, in this order: `CalcInput::sourceMeasurement(sensor, name)`, `CalcInput::sourceUnit(sensor, name)` | no (error) |
| `"preference"` | - | error: `preference inputs are not available to plugins` | no |
| anything else | - | error: `unknown dependency kind '<repr>'` | no |

Preference inputs are deliberately **not** exposed (the spec does not require
it; nothing bundled needs it). The kind is named in the table only so the
error message is specific.

### Failure model (one rule)

A plugin calculation either publishes a fully validated bundle or nothing.

| Situation | Python sees | Engine status | Log |
|---|---|---|---|
| `compute` raises | - | `Failed` | one `qWarning` with plugin label + `error_already_set::what()` (message, traceback with file/line); plus the engine's own one-line failure warning |
| Output malformed (Task 7.5 / 7.6 rules) | - | `Failed` | one `qWarning` naming plugin, output, and reason |
| Undeclared read | `UndeclaredInputError` raised at the offending line | `UndeclaredRead` (even if the plugin swallows the exception and returns a value) | engine's undeclared-read warning + one adapter `qWarning` with the traceback |
| View used after `compute` returned | `RuntimeError` | n/a | none |
| `None` (single-output) / `None` or missing entry (bundle) / non-finite attribute | - | `Ok`, that output unavailable | none |

All of these are negatively cached by the engine with the declared inputs as
dependencies: **the plugin is not called again until a declared input
changes**. No exception ever propagates into Qt code; no Python object
outlives the GIL scope that created it except the plugin instances themselves
(Task 7.5 "Lifetime").

---

## Tasks

Order: 7.1 (engine opt-ins) -> 7.2 (test target, so every later task can add
cases) -> 7.3 -> 7.4 -> 7.5 -> 7.6 -> 7.7 -> 7.8 -> 7.9. The application builds
and all tests pass after each task. `PluginHost` initialises once per process,
so `tst_python_bridge` loads **all** test plugin files in `initTestCase`; a
task that adds plugin files adds the matching test functions in the same task.

### Task 7.1: Engine opt-ins needed by the bridge

**Purpose:** Let a plugin calculation declare source inputs (so source reads are tracked and invalidated like everything else) and let the view ask whether a read is declared without triggering the violation path.

**Files to modify:**
- `src/engine/calculationdescriptor.h` - add `bool allowSourceInputs = false;` to `CalculationDescriptor`.
- `src/engine/calculationregistry.cpp` - validation rule change.
- `src/engine/evaluationcontext.h` / `.cpp` - add `bool isDeclared(const CalcInput &input) const;`.
- `tests/tst_calcregistry.cpp`, `tests/tst_calcengine.cpp` - new cases.

**Technical Approach:**

- `allowSourceInputs` header comment: "Opt-in for calculations that read the
  recorded (source) layer explicitly. Set only by the Python plugin host for
  plugins that declare `source()` inputs. Built-in calculations must never set
  it: only the conversion layer and explicit plugin source access depend on
  source nodes."
- Registry rule (Phase 2 Task 2.3 "Validation"): a `SourceMeasurement` /
  `SourceUnit` input is accepted iff the calculation is registered through
  `registerSourceConversion` **or** `descriptor.allowSourceInputs` is true.
  Everything else in validation is unchanged. `CalculationFamily` gets no flag.
- No engine change is needed for evaluation: `ensureResult`'s availability
  pass already handles source kinds generically ("note the leaf; read
  `state`"), and `sourceMeasurementChanged` / `sourceUnitChanged` already seed
  those leaves. Source nodes are leaves, so no cycle can arise.
- `EvaluationContext::isDeclared(input)`: const, returns whether `input`
  (`operator==` on `CalcInput`) is in the descriptor's `inputs`. It records
  nothing, warns nothing, and does not mark the evaluation. It exists because
  an empty `sourceUnit` is a legitimate declared value, so "empty return" cannot
  be used to detect an undeclared read.

Tests:
- `tst_calcregistry::sourceInputsOptIn` - plain descriptor with
  `CalcInput::sourceMeasurement("S","m")`: rejected with the flag false
  (existing behavior), accepted with the flag true.
- `tst_calcengine::optInSourceRead` - flagged calculation, inputs
  `sourceMeasurement(S,m)`, `sourceUnit(S,m)`, output attr `SRC0` = first source
  sample; with a doubling conversion family registered (as in
  `sourceConversionHook`) and source `{1,2,3}` unit `"raw"`: `SRC0 == 1`
  (not 2); `fake.setMeasurement(engine, "S","m",{5,6,7},"raw")` returns a set
  containing `SRC0`; next read `5`, `runCount == 2`; `setUnit` also invalidates
  it; with the source measurement absent the status is `MissingInput` and
  `runCount == 0`.
- `tst_calcengine::isDeclaredIsSilent` - inside a compute, `isDeclared` of an
  undeclared input returns false and afterwards `undeclaredReadCount() == 0`
  and the result status is `Ok`.

**Acceptance Criteria:**
- [ ] `git grep -n "allowSourceInputs" -- src` hits only `engine/calculationdescriptor.h`, `engine/calculationregistry.cpp`, and (after Task 7.5) `pluginadapters.cpp`.
- [ ] The Phase 2 test asserting that a plain calculation with a source input is rejected still passes unchanged.
- [ ] The three new tests pass; `flysight_model` still links Qt Core only.

**Complexity:** S

---

### Task 7.2: Embedded-Python test target and environment

**Purpose:** Provide the real-bridge test executable (acceptance 17) first, so each later task lands with its tests.

**Files to create:**
- `tests/tst_python_bridge.cpp` (class `PythonBridgeTest`)
- `tests/python_plugins/t_single.py` (first test plugin file; more files are added by later tasks)

**Files to modify:**
- `tests/CMakeLists.txt` - one appended block (below).
- `src/pluginhost.h` / `.cpp` - add `bool isInitialised() const;`.

**Technical Approach:**

**How the application finds Python and the bridge today** (`pluginhost.cpp`
71-217): if `<applicationDirPath>/python` exists (installed layout) it builds a
PEP-587 `PyConfig` with `home` = that directory and explicit
`module_search_paths` (the bridge lives in `python/Lib/site-packages`, installed
by `src/CMakeLists.txt` 454-471); otherwise it constructs a default
`py::scoped_interpreter()` - the **system Python**, which honors `PYTHONHOME`
and `PYTHONPATH`. The bridge is imported (195) **before** the plugin directory
is put on `sys.path` (236), so the bridge must be reachable through the
interpreter's own search path. A test executable in `<build>/tests/<cfg>/` has
no `python/` sibling, so it always takes the system-Python branch; the test
environment therefore supplies `PYTHONHOME`, `PYTHONPATH`, and (Windows) the
Python DLL directory. No production seam or environment variable is added to
`PluginHost` for this.

**`tests/CMakeLists.txt` block** (append at the end of the file):

```cmake
# ---- Phase 7: embedded-Python bridge test --------------------------------
option(FLYSIGHT_BUILD_PYTHON_TESTS "Build and run the embedded-Python plugin bridge test" ON)
if(FLYSIGHT_BUILD_PYTHON_TESTS)
  # Build-time interpreter facts (Python_EXECUTABLE comes from src/CMakeLists.txt find_package(Python))
  execute_process(COMMAND "${Python_EXECUTABLE}" -c "import sys; print(sys.base_prefix)"
                  OUTPUT_VARIABLE _fs_py_home OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND "${Python_EXECUTABLE}" -c
                  "import numpy, os; print(os.path.dirname(os.path.dirname(numpy.__file__)))"
                  RESULT_VARIABLE _fs_numpy_rc OUTPUT_VARIABLE _fs_numpy_site
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  file(TO_CMAKE_PATH "${_fs_py_home}" _fs_py_home)
  file(TO_CMAKE_PATH "${_fs_numpy_site}" _fs_numpy_site)

  flysight_add_test(tst_python_bridge
    SOURCES tst_python_bridge.cpp
            "${FLYSIGHT_SRC_DIR}/pluginhost.cpp"
            "${FLYSIGHT_SRC_DIR}/pluginadapters.cpp"      # added by Task 7.3; omit until it exists
    LIBS pybind11::embed
    ENVIRONMENT "PYTHONHOME=${_fs_py_home}" "PYTHONDONTWRITEBYTECODE=1")
  add_dependencies(tst_python_bridge flysight_cpp_bridge)
  flysight_msvc_fix_python_debug_autolink(tst_python_bridge)
  target_compile_definitions(tst_python_bridge PRIVATE
    FLYSIGHT_PYTHON_PLUGINS_DIR="${CMAKE_CURRENT_LIST_DIR}/../python_plugins"
    FLYSIGHT_TEST_PLUGINS_DIR="${CMAKE_CURRENT_LIST_DIR}/python_plugins")
  set_tests_properties(tst_python_bridge PROPERTIES LABELS "core;python" TIMEOUT 180)

  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
    set_property(TEST tst_python_bridge APPEND PROPERTY ENVIRONMENT_MODIFICATION
      "PYTHONPATH=set:$<TARGET_FILE_DIR:flysight_cpp_bridge>"
      "PYTHONPATH=path_list_append:${_fs_numpy_site}")
    if(WIN32)
      foreach(_d IN LISTS Python_RUNTIME_LIBRARY_DIRS)
        set_property(TEST tst_python_bridge APPEND PROPERTY ENVIRONMENT_MODIFICATION
          "PATH=path_list_prepend:${_d}")
      endforeach()
    endif()
  endif()

  if(NOT _fs_numpy_rc EQUAL 0 OR CMAKE_VERSION VERSION_LESS 3.22)
    message(WARNING "tst_python_bridge is DISABLED: NumPy is not importable by ${Python_EXECUTABLE} "
                    "(python -m pip install numpy) or CMake < 3.22.")
    set_tests_properties(tst_python_bridge PROPERTIES DISABLED TRUE)
  endif()
endif()
```

Notes on the block:
- `PYTHONPATH` is set through `ENVIRONMENT_MODIFICATION` (not `ENVIRONMENT`)
  because its Windows separator `;` is also CMake's list separator.
  `$<TARGET_FILE_DIR:flysight_cpp_bridge>` is `${CMAKE_BINARY_DIR}` because of
  `flysight_set_output_to_build_root(flysight_cpp_bridge)` (`src/CMakeLists.txt` 418).
  The NumPy parent directory is appended explicitly so a venv or user-site
  NumPy still resolves when `PYTHONHOME` is the base prefix.
- On Windows the bridge `.pyd` depends on `Qt6Core.dll`. Python >= 3.8 does not
  search `PATH` for an extension module's dependencies, but the test executable
  has already loaded `Qt6Core.dll`, and the loader reuses a loaded module by
  name - the same reason the application works. Nothing to do.
- **Python development components are `REQUIRED` by `src/CMakeLists.txt` 116**,
  so they are never "unavailable" in a configured tree. The only optional
  ingredient is NumPy in the *build-time* interpreter (the SDK imports it at
  module level); without it the test is registered but `DISABLED`, so
  `ctest` reports "Not Run (Disabled)" instead of silently omitting it. The
  target is still built, which keeps `pluginhost.cpp` compile-checked.
  `-DFLYSIGHT_BUILD_PYTHON_TESTS=OFF` removes the target entirely. The root
  superbuild `CMakeLists.txt` must declare and forward this option to the
  `src` ExternalProject exactly as Phase 1 does for `FLYSIGHT_BUILD_TESTS`
  (otherwise `=OFF` is silently ignored on the documented Windows build path).
- Windows: Release only (Phase 1 rule; additionally `python3xx_d.lib` is
  normally absent).
- macOS (unverified, cannot be checked without a push): if the test cannot
  locate `libpython`, add under `if(APPLE)`
  `set_target_properties(tst_python_bridge PROPERTIES BUILD_RPATH "${Python_LIBRARY_DIRS}")`.

**`PluginHost::isInitialised()`**: true iff the interpreter booted, the bridge
imported, and the SDK imported (set a `bool m_ready` at the point where
`pluginhost.cpp` today reaches section 3). `initialise()` stays idempotent
(`if (m_interp) return;`).

**`tst_python_bridge.cpp` skeleton.** Include order matters: include
`<pybind11/embed.h>` and `<pybind11/numpy.h>` with the `slots` push/pop dance of
`pluginhost.cpp` 21-36 **before** any Qt header, then Qt / FlySight headers.
`FLYSIGHT_TEST_MAIN(PythonBridgeTest)`.

`initTestCase()`:
1. `dir = TestEnvironment::instance().newTempDir("plugins")`.
2. Copy into `dir`: `FLYSIGHT_PYTHON_PLUGINS_DIR/flysight_plugin_sdk.py`, every
   `FLYSIGHT_TEST_PLUGINS_DIR/*.py`, and (from Task 7.7)
   `FLYSIGHT_PYTHON_PLUGINS_DIR/examples/imu_tilt.py`. A temp directory is used
   because `PluginHost` imports **every** `*.py` in the plugin directory and
   because CPython would otherwise write `__pycache__` into the source tree.
   The test calls `initialise(dir)` directly; `FLYSIGHT_PLUGINS` is only read by
   `MainWindow` and is not needed.
3. `PluginHost::instance().initialise(dir)`; `QVERIFY(PluginHost::instance().isInitialised())`
   (a failure here is a hard failure, not a skip: CMake promised the environment).
4. **Then** `TestEnvironment::instance().registerBuiltIns()` - plugins before
   built-ins, exactly like `MainWindow`.

Fixture (file-local `bridgeSensorFile(sessionId)` using `Fs2FileBuilder`):
`$VAR` `FIRMWARE_VER=v2023.09.22`, `SESSION_ID=<id>`, `DEVICE_ID=test-device`;
`IMU` columns `time,wx,wy,wz,ax,ay,az,temperature`, units
`s,deg/s,deg/s,deg/s,g,g,g,deg C`, rows `3,62.5,-125,0,1,0,1,40` and
`4,62.5,-125,0,0,0,1,40`. No `SCHEMA_VER`. Helper `loadFixture()` writes it to
a fresh temp dir and imports it with a fresh `DataImporter` into a fresh
`SessionData` (fresh engine => per-test run counts). `near(a,b)` =
`qAbs(a-b) <= 1e-9`.

`t_single.py` (existing-style plugins, written exactly as a pre-existing plugin
would be - `inputs()` + `compute(session)` + `session.getMeasurement/getAttribute`):

| Class | Registration | Inputs | Returns |
|---|---|---|---|
| `PyWx0(AttributePlugin)` | name `_PY_WX0` | `meas("IMU","wx")` | `float(np.array(session.getMeasurement("IMU","wx"), float)[0])` |
| `PyWx2(MeasurementPlugin)` | sensor `IMU`, name `pyWx2`, `units = "deg/s"` | `meas("IMU","wx")` | `np.array(session.getMeasurement("IMU","wx"), float) * 2` |
| `PyFirmware(AttributePlugin)` | `_PY_FW` | `attr("FIRMWARE_VER")` | `session.getAttribute("FIRMWARE_VER")` |
| module level | `register_plot(SimplePlot("PyTest","Py Wx2","deg/s","#112233","IMU","pyWx2","rotation"))`, `register_marker(SimpleMarker("PyTest","Py marker","PyM","#445566","_PY_WX0",[("IMU","_time","wx")]))` | | |

Test functions added in this task:
- `bootsRealBridge` - `py::module_::import("flysight_cpp_bridge")` succeeds and its `__file__` starts with the build-tree bridge directory (proves the *real* module is used, not a stub).
- `singleOutputPluginsReadEffectiveValues` (acc. 17) - `_PY_WX0` near `71.68`; `IMU/pyWx2` has 2 samples near `143.36`; `_PY_FW == "v2023.09.22"`; `sourceMeasurement("IMU","wx") == {62.5, 62.5}` (the plugin saw corrected data, the source is untouched); each plugin's `runCount == 1` after three reads of each output.
- `pluginOutputsAreNotEnumerated` - `hasMeasurement("IMU","pyWx2")` and `hasAttribute("_PY_WX0")` false after the reads.
- `plotAndMarkerRegistrationUnaffected` - `PlotRegistry::instance().allPlots()` contains a `PlotValue` with `plotName == "Py Wx2"` and `measurementType == "rotation"`; `MarkerRegistry::instance()->allMarkers()` contains `attributeKey == "_PY_WX0"` with one measurement triple `("IMU","_time","wx")`.
- `secondInitialiseIsNoOp` - `initialise(otherTempDir)` changes nothing.

**Acceptance Criteria:**
- [ ] With NumPy present, `ctest --test-dir build/FlySightViewer-build -C Release -R tst_python_bridge --output-on-failure` passes from a shell whose `PATH` contains neither Qt nor Python.
- [ ] With NumPy absent the configure step warns and `ctest -N` lists the test as disabled; with `-DFLYSIGHT_BUILD_PYTHON_TESTS=OFF` the target does not exist.
- [ ] No other test target links `pybind11::embed` or Python.
- [ ] The test writes nothing outside `TestEnvironment::rootPath()` (`PYTHONDONTWRITEBYTECODE=1`, temp plugin dir).
- [ ] No environment variable or test-only code path was added to `pluginhost.cpp` other than `isInitialised()`.

**Complexity:** M

---

### Task 7.3: Value-typed keys in the SDK and explicit decoding in the host

**Purpose:** Replace "anything that is not an attribute is a measurement" (`pluginhost.cpp` baseline 283-295, 337-349) with one explicit decoding table, make keys usable as dict keys for bundles, and turn every per-plugin registration problem into a logged rejection of that plugin only.

**Files to create:**
- `src/pluginadapters.h` / `.cpp` - decoding, marshalling, and adapter construction (compiled into `FlySightViewer` and `tst_python_bridge`; **never** into the bridge module).
- `tests/python_plugins/t_badkeys.py`

**Files to modify:**
- `python_plugins/flysight_plugin_sdk.py`
- `src/pluginhost.h` / `.cpp`
- `src/cpp_bridge.cpp` - remove the `register_dependencykey` declaration and call.
- `src/CMakeLists.txt` - add `pluginadapters.cpp pluginadapters.h` to `PROJECT_SOURCES` next to `pluginhost` (282); remove `dependencykey_bindings.cpp` from `pybind11_add_module(flysight_cpp_bridge ...)` (407-411).

**Files to delete:**
- `src/dependencykey_bindings.cpp`

**Technical Approach:**

**SDK** (`flysight_plugin_sdk.py`): delete `from flysight_cpp_bridge import DependencyKey`
(58) and add

```python
KIND_ATTRIBUTE   = "attribute"
KIND_MEASUREMENT = "measurement"
KIND_SOURCE      = "source"

@dataclass(frozen=True)
class Key:
    kind:   str
    sensor: str = ""
    name:   str = ""          # attribute key when kind == "attribute"

def attr(name: str) -> Key:                 return Key(KIND_ATTRIBUTE, "", name)
def meas(sensor: str, name: str) -> Key:    return Key(KIND_MEASUREMENT, sensor, name)
def source(sensor: str, name: str) -> Key:  return Key(KIND_SOURCE, sensor, name)
```

Replace `List[DependencyKey]` annotations with `List[Key]`. Also in this task,
delete the unregistered example classes (208-260: `DefaultStartTime`,
`DefaultDuration`, `DefaultExitTime`, `DefaultTime`) and the then-unused
`datetime` import (54). They collide with built-in output names
(`_START_TIME`, `_DURATION`, `_EXIT_TIME`, `{sensor}/_time`), have never been
registered, and rely on the ISO-string rule that Task 7.5 removes. Two of them
reappear as documentation examples under non-colliding names (Task 7.9).

**Bridge:** the `DependencyKey` binding is deleted. `src/dependencykey.h` is
untouched (it is the engine's public-name type).

**Host decoding** (`pluginadapters.h`, namespace `FlySight::PluginBridge`):

```cpp
struct PluginError : std::runtime_error { using std::runtime_error::runtime_error; };   // message is user-facing

enum class KeyKind { Attribute, Measurement, Source };
struct DecodedKey { KeyKind kind; QString sensor; QString name; QString display() const; };   // "attribute _X" / "measurement IMU/wx" / "source IMU/wx"

DecodedKey            decodeKey(pybind11::handle key);                       // throws PluginError
QList<CalcInput>      decodeInputs(pybind11::handle inputsResult, bool *usesSource);  // list/tuple of Key; "source" -> 2 inputs
QList<DependencyKey>  decodeOutputs(pybind11::handle outputsResult);         // attribute / measurement only; non-empty
QString               pluginLabel(pybind11::handle plugin);                  // "<module>.<qualname>"
```

`decodeKey` implements the decoding table above *literally*: fetch `kind` with
`py::getattr(key, "kind", py::none())`; require `PyUnicode_CheckExact`; compare
against the three constants with `if / else if`; final `else` throws
`PluginError("unknown dependency kind '<repr(kind)>'")` (or the specific
preference message). `sensor` / `name` must be exact `str`; emptiness rules per
the table. A key object without those attributes (e.g. an `int`, or a legacy
object) throws `PluginError("dependency key <repr> is not a flysight_plugin_sdk.Key")`.
`inputs()` / `outputs()` must return a `list` or `tuple`; anything else throws.
**There is no default branch that produces a key.**

**Per-plugin registration.** Restructure `pluginhost.cpp` sections 4-5 into one
helper used for all three SDK lists:

```cpp
template <typename Fn> void registerEach(py::handle sdkList, const char *what, Fn registerOne);
// for index i, object plugin: try { registerOne(i, plugin); report.registeredIds << id; }
// catch (const PluginBridge::PluginError &e)  -> reject(label, e.what())
// catch (const py::error_already_set &e)      -> reject(label, e.what())      // e.g. inputs() raised
// catch (const std::exception &e)             -> reject(label, e.what())
```

`reject` = `qWarning().noquote() << "[PluginHost] Plugin" << label << "rejected:" << reason;`
plus an entry in the report. Other plugins continue. A `false` from
`CalculationRegistry::registerCalculation` (duplicate id, `'#'` in a name,
output listed among its own inputs, duplicate outputs) becomes
`PluginError("registration refused by the calculation registry (see previous warning)")`.
The label is computed defensively (`"<unknown>"` if even that fails).

Add to `PluginHost`:

```cpp
struct PluginLoadReport {
    QStringList registeredIds;     // calculation ids, in registration order
    QStringList rejected;          // "<label>: <reason>"
    QStringList failedImports;     // plugin file names whose import raised
};
const PluginLoadReport &report() const;
```

The summary line (baseline 418-422) reports registered / rejected counts per
kind instead of raw list lengths.

Registration order is fixed and documented: all `_attributes`, then all
`_measurements`, then all `_calculations` (Task 7.6); inside a list, plugin
files are imported in `QDir` name order and registrations keep call order.
Ids keep Phase 3's scheme (`<i>` = index in the SDK list, rejected plugins
still consume their index).

`t_badkeys.py` (each registered with `register_attribute` unless noted):

| Class | Defect | Expected rejection reason contains |
|---|---|---|
| `PyBogusKind` (`_PY_BOGUS`) | `inputs()` -> `[Key("bogus","IMU","wx")]` | `unknown dependency kind 'bogus'` |
| `PyPrefKind` (`_PY_PREF`) | `[Key("preference","","general/units")]` | `preference inputs are not available` |
| `PyIntKey` (`_PY_INTKEY`) | `[42]` | `is not a flysight_plugin_sdk.Key` |
| `PyNonStrKind` (`_PY_NONSTR`) | `[Key(1,"IMU","wx")]` | `unknown dependency kind` |
| `PyEmptyField` (`_PY_EMPTY`) | `[meas("IMU","")]` | `empty` |
| `PyInputsRaises` (`_PY_INRAISE`) | `inputs()` raises `RuntimeError("nope")` | `nope` |
| `PyNoName` | no `name` attribute | `name` |
| `PyGoodNeighbour` (`_PY_GOOD`) | none; returns `1.0` | - registered |

Test functions: `unknownKindRejectsPluginOnly` - for each bad class
`report().rejected` has exactly one entry starting with its label and
containing the text above; `CalculationRegistry::instance().hasCandidateFor(DependencyKey::attribute("_PY_BOGUS"))`
(etc.) is false; `_PY_GOOD == 1.0`; the process did not crash and
`t_single`'s plugins are registered. `registrationOrderIsDeterministic` -
`report().registeredIds` equals a literal `QStringList` (extended by later
tasks).

**Acceptance Criteria:**
- [ ] `git grep -n "DependencyKey" -- python_plugins src/cpp_bridge.cpp src/*_bindings.cpp ':!python_plugins/README.md'` returns nothing; `src/dependencykey_bindings.cpp` no longer exists; `src/dependencykey.h` is unchanged.
- [ ] `pluginadapters.cpp` contains exactly one function that maps a kind to a `CalcInput`/`DependencyKey`, and it has no fall-through default.
- [ ] `git grep -n "class Default" -- python_plugins` returns nothing.
- [ ] Existing call sites `meas(...)` / `attr(...)` work unchanged (`t_single.py` untouched, still passing).
- [ ] A rejected plugin never leaves a partially registered calculation (`registeredIds` and `hasCandidateFor` agree).
- [ ] Application starts with an empty plugin directory and logs the summary with zeros.

**Complexity:** M

---

### Task 7.4: Session view - declared-read diagnostics, source access, effective unit

**Purpose:** Give plugins source access with C++ semantics and make the declared-inputs rule fail loudly *in Python*, at the offending line, with the plugin and key named.

**Files to modify:**
- `src/pluginsessionview.h` (header-only; Qt Core + `engine/evaluationcontext.h` + `<stdexcept>` only)
- `src/sessiondata_bindings.cpp`
- `src/pluginhost.cpp` / `src/pluginadapters.cpp` - pass the label to the view.
- `python_plugins/flysight_plugin_sdk.py` - `from flysight_cpp_bridge import UndeclaredInputError` (re-export).

**Files to create:**
- `tests/python_plugins/t_view.py`

**Technical Approach:**

```cpp
struct UndeclaredInputError : std::runtime_error { using std::runtime_error::runtime_error; };
struct StaleSessionViewError : std::runtime_error { using std::runtime_error::runtime_error; };

class PluginSessionView {
public:
    PluginSessionView(const EvaluationContext *ctx, const QString &pluginLabel);   // label = calculation id + " (" + module.qualname + ")"
    void invalidate();

    // effective layer - key must be a declared attr()/meas() input
    QVector<double> getMeasurement(const QString &sensor, const QString &name) const;
    QVariant        getAttribute(const QString &key) const;
    QString         effectiveUnit(const QString &sensor, const QString &name) const;      // ctx->measurementUnit
    bool            hasMeasurement(const QString &sensor, const QString &name) const;     // true for a declared input
    bool            hasAttribute(const QString &key) const;

    // source layer - key must be a declared source() input; never computes, never falls back
    QVector<double> sourceMeasurement(const QString &sensor, const QString &name) const;  // ctx->sourceMeasurement
    QString         sourceUnit(const QString &sensor, const QString &name) const;         // ctx->sourceUnit
    bool            hasSourceMeasurement(const QString &sensor, const QString &name) const;
};
```

Every accessor does, in this order:
1. If invalidated: `throw StaleSessionViewError("SessionData is only valid inside compute()")`.
2. Build the matching `CalcInput`; if `!ctx->isDeclared(input)`: call the
   matching `ctx` accessor once and discard the result (this is what flags the
   evaluation as `UndeclaredRead` and makes the engine count and warn - Phase 2
   Task 2.2), then
   `throw UndeclaredInputError("<label> read undeclared <kind> <sensor>/<name>; add <helper>('<sensor>', '<name>') to inputs()")`
   where `<helper>` is `meas`, `attr`, or `source`.
3. Return the `ctx` value. `has*` return `true` at this point: every declared
   input is available whenever `compute` runs (spec 7.1: all declared inputs
   are required).

Source semantics match C++ exactly: `sourceMeasurement` / `sourceUnit` return
the recorded samples and recorded unit text. Because all declared inputs are
required, a plugin that declares `source("IMU","wTotal")` on a session whose
`wTotal` is purely derived **does not run**: its outputs are unavailable with
status `MissingInput` and the derived value is never substituted. That is the
Python form of "asking for the source of a measurement that has no source data
reports absence" (spec 4); `hasSourceMeasurement` exists for API symmetry and
for documentation examples, and raises like every other accessor when the key
is undeclared.

Bindings (`sessiondata_bindings.cpp`): keep
`py::class_<PluginSessionView, std::shared_ptr<PluginSessionView>>(m, "SessionData")`;
bind the eight methods with the camelCase names above and keyword names
`sensorKey`, `measurementKey`, `key` as today. Changes to return conversion:
- `getMeasurement` / `sourceMeasurement` return a **new 1-D `float64` NumPy
  array that owns a copy** of the samples (`py::array_t<double>(n)` + `memcpy`),
  not a Python list: a Python list of boxed floats costs ~32 bytes per sample
  for IMU-rate data, and an array is what every plugin converts to anyway
  (`np.array(x, float)` keeps working). Never expose the `QVector` buffer
  without copying - it is implicitly shared with the source layer and the cache.
- `getAttribute` keeps today's rule (baseline 63-82): invalid -> `None`;
  convertible to a number (including numeric-looking strings, which is how
  attributes loaded from files arrive) -> `float`; other string -> `str`;
  otherwise `None`. Replace the deprecated `v.type() == QVariant::String` with
  `v.typeId() == QMetaType::QString`.
- `effectiveUnit` / `sourceUnit` -> `str`.
- In `cpp_bridge.cpp` (or the bindings file) register the exceptions:
  `py::register_exception<UndeclaredInputError>(m, "UndeclaredInputError", PyExc_RuntimeError);`
  `StaleSessionViewError` needs no registration (pybind11 maps
  `std::runtime_error` to `RuntimeError`).

The view's inline methods execute inside the bridge module; they touch only the
`EvaluationContext` they were given, so the two-registry constraint holds.

`t_view.py`:

| Class | Form | Inputs | Behavior |
|---|---|---|---|
| `PyUndeclared` | attr `_PY_UNDECL` | `meas("IMU","wx")` | reads `session.getMeasurement("IMU","wy")` |
| `PyUndeclaredSwallowed` | attr `_PY_UNDECL_SW` | `meas("IMU","wx")` | `try: session.getAttribute("FIRMWARE_VER") except Exception: pass; return 1.0` |
| `PyUndeclaredSource` | attr `_PY_UNDECL_SRC` | `meas("IMU","wx")` | reads `session.sourceMeasurement("IMU","wx")` (effective declared, source not) |
| `PySrcOfDerived` | attr `_PY_SRC_WTOTAL` | `source("IMU","wTotal")` | returns `1.0` (must never run) |
| `PyDerivedWTotal` | attr `_PY_EFF_WTOTAL0` | `meas("IMU","wTotal")` | first sample |
| `PyStash` | attr `_PY_STASH` | `meas("IMU","wx")` | stores `session` in module global `stash`; returns `1.0`; module function `poke_stale()` returns `True` iff `stash.getMeasurement("IMU","wx")` raises `RuntimeError` |

(The source-equals-C++ plugin needs the multi-output form and is added in Task 7.6.)

Test functions:
- `undeclaredReadDiagnostic` - `_PY_UNDECL` unavailable; `resultStatus(id) == UndeclaredRead`; `undeclaredReadCount() == 1`; `lastUndeclaredRead().first` ends with `._PY_UNDECL` and `.second == CalcInput::measurement("IMU","wy")`; a message-handler capture contains exactly one message with all of `UndeclaredInputError`, `_PY_UNDECL`, `IMU/wy`, `meas('IMU', 'wy')`; a second read adds no message and no run.
- `swallowedUndeclaredReadStillFails` - `_PY_UNDECL_SW` unavailable, status `UndeclaredRead`.
- `undeclaredSourceRead` - `_PY_UNDECL_SRC` unavailable; `lastUndeclaredRead().second == CalcInput::sourceMeasurement("IMU","wx")`.
- `sourceOfDerivedNameIsAbsent` (acc. 17) - `_PY_EFF_WTOTAL0` near `160.2813526271849`; `_PY_SRC_WTOTAL` unavailable with `resultStatus == MissingInput` and `runCount == 0`; C++ `session.hasSourceMeasurement("IMU","wTotal")` false, `sourceMeasurement` empty, `sourceUnit` `""`.
- `staleViewRaises` - after reading `_PY_STASH`, calling `t_view.poke_stale()` through pybind11 returns `True`.

**Acceptance Criteria:**
- [ ] `pluginsessionview.h` includes no `sessiondata.h`, no registry / engine header other than `engine/evaluationcontext.h` (+ `calctypes.h` transitively), and defines no static data.
- [ ] No binding returns memory owned by a `QVector`.
- [ ] The five tests pass; all Task 7.2 / 7.3 tests still pass.
- [ ] `git grep -n "setCalculated\|set_calculated" -- src python_plugins tests ':!python_plugins/README.md'` returns nothing (the README's "What was removed" section names the removed API on purpose).

**Complexity:** M

---

### Task 7.5: Single-output adapters - marshalling, exceptions, lifetime

**Purpose:** Make the adapters that Phase 3 left minimal into the final ones: validated and copied results, contained exceptions, GIL-safe object lifetime.

**Files to modify:**
- `src/pluginadapters.h` / `.cpp`, `src/pluginhost.cpp`
- `python_plugins/flysight_plugin_sdk.py` (docstrings of `AttributePlugin.compute` / `MeasurementPlugin.compute` only)

**Files to create:**
- `tests/python_plugins/t_results.py`

**Technical Approach:**

```cpp
namespace FlySight::PluginBridge {
// Throws PluginError on malformed values. Return false = "no value" (None / non-finite attribute).
bool toAttributeValue(pybind11::handle value, QVariant *out);
bool toMeasurementValues(pybind11::handle value, QVector<double> *out);

CalculationDescriptor makeAttributeAdapter(int index, pybind11::object plugin);     // plugin.attr.<i>.<name>
CalculationDescriptor makeMeasurementAdapter(int index, pybind11::object plugin);   // plugin.meas.<i>.<sensor>/<name>
}
```

**Attribute return typing** (checked in this order):

| Python value | Result |
|---|---|
| `None` | unavailable |
| `bool` (`PyBool_Check`, tested **before** int) | malformed |
| `str` (`PyUnicode_Check`) | `QVariant(QString)` verbatim |
| `float`, `int`, or any `numbers.Real` instance (covers `np.float64`, `np.float32`, `np.int64`; `numbers.Real` is imported once and cached in the holder described below) | `QVariant(double)` via `PyFloat_AsDouble`; non-finite -> unavailable (treated as "no value", no warning) |
| anything else (list, tuple, dict, ndarray, bytes, complex, ...) | malformed |

**The ISO-datetime-string -> UTC-seconds rule (baseline `pluginhost.cpp` 309-321)
is dropped.** A string is a string. Time-valued attributes are UTC seconds as
`float`, like every built-in (`_START_TIME`, `_EXIT_TIME`, ...), so markers and
the interpolation family work; the README shows `dt.timestamp()`. Rationale:
silent type changes based on string shape are surprising
(`QDateTime::fromString("2024-01-01", Qt::ISODateWithMs)` is valid), and the
only users were the deleted example classes.

**Measurement return** (`toMeasurementValues`):
1. `None` -> unavailable.
2. `str` / `bytes` -> malformed (before NumPy would try to parse them).
3. `py::array a = py::array::ensure(value)`; null (clear the Python error) -> malformed.
4. `a.dtype().kind()` must be `'f'`, `'i'`, or `'u'`; `'b'` (bool), `'c'`, `'O'`, `'U'`, `'S'`, `'M'`, `'m'`, `'V'` -> malformed `"dtype <name> is not numeric"`.
5. `a.ndim() != 1` -> malformed `"expected a 1-D array, got <n>-D"` (covers scalars / 0-D and 2-D).
6. `auto d = py::array_t<double, py::array::c_style | py::array::forcecast>::ensure(a)`; null -> malformed.
7. `out->resize(n); memcpy(out->data(), d.data(), n * sizeof(double))`. The copy
   is complete before the adapter returns; nothing in the result references
   NumPy memory. Lists, tuples, `float32`, integer arrays, and non-contiguous
   slices are therefore all accepted; NaN samples are allowed (gaps).
8. An empty 1-D array is not malformed; the engine normalizes it to unavailable.

**Length is not validated.** The adapter cannot know the sensor's time vector
unless the plugin declared it, and reading it undeclared would violate the
engine's rule. This matches the built-ins. The consumer side already protects
itself (`PlotWidget.cpp` 523-524 skips a graph whose x and y sizes differ).
The README states the expectation: one value per sample of `<sensor>/_time`.

`MeasurementPlugin.units` (class attribute, `None` or `str`) is read once at
registration and passed as the `unit` argument of
`CalculationResult::setMeasurement`, so `SessionData::effectiveUnit` reports it.
`AttributePlugin.units` remains informational (there is no attribute unit
concept in the engine).

**Adapter body** (identical skeleton for both forms; `inputs()` is called once,
at registration):

```
compute = [holder, label, outputKey, unit](const EvaluationContext &ctx) -> CalculationResult {
    py::gil_scoped_acquire gil;
    auto view = std::make_shared<PluginSessionView>(&ctx, label);
    ViewGuard guard(view);                       // calls view->invalidate() on every exit path
    try {
        py::object out = holder->plugin.attr("compute")(view);
        ... toAttributeValue / toMeasurementValues ...           // may throw PluginError
        return value ? CalculationResult().set...(...) : CalculationResult::unavailable();
    } catch (py::error_already_set &e) {
        const bool undeclared = e.matches(holder->undeclaredInputError);
        qWarning().noquote() << "[PluginHost]" << label << "raised:" << e.what();   // message + traceback, once
        if (undeclared) return CalculationResult::unavailable();  // engine publishes UndeclaredRead
        throw PluginError(...first line of e.what()...);           // engine publishes Failed
    } catch (const PluginError &e) {
        qWarning().noquote() << "[PluginHost]" << label << "returned malformed output:" << e.what();
        throw;                                                       // engine publishes Failed
    }
}
```

- The `py::error_already_set` is caught, logged, and destroyed **inside** the
  GIL scope; what crosses into the engine is a plain `std::runtime_error`
  subclass holding no Python reference. The engine's `catch` in `ensureResult`
  (Phase 2 Task 2.4 step 6) turns it into status `Failed` with an empty bundle
  and caches it against the declared inputs.
- Logging goes through `qWarning` with `e.what()` (pybind11 3.0 includes the
  exception type, message, and an "At:" traceback with file and line). Do not
  call `PyErr_Print()`; the error has already been fetched.
- Descriptors set `allowSourceInputs = usesSource` (from `decodeInputs`).
- A plugin whose output key equals a built-in output (e.g. `IMU/aTotal`) is
  legal: registration order makes it the first candidate.

**Lifetime.** The compute lambdas live in `CalculationRegistry::instance()`, a
function-local static whose destruction order relative to the
`PluginHost` singleton (which owns the `py::scoped_interpreter`) is not
guaranteed - in the application the host is constructed first and destroyed
last, in a test it can be the other way round. A `py::object` destroyed after
`Py_Finalize` crashes. Therefore the lambdas capture a
`std::shared_ptr<PyPluginHolder>`:

```cpp
struct PyPluginHolder { pybind11::object plugin, undeclaredInputError, numbersReal; };
std::shared_ptr<PyPluginHolder> makeHolder(pybind11::object plugin);
// deleter: if (Py_IsInitialized()) { py::gil_scoped_acquire g; delete p; }
//          else { p->plugin.release(); p->undeclaredInputError.release(); p->numbersReal.release(); delete p; }
```

No raw `py::object` may be captured by value in anything stored outside
`pluginhost.cpp`'s stack. The interpreter's main thread keeps the GIL for the
life of the process (nothing calls `PyEval_SaveThread`), so
`gil_scoped_acquire` is re-entrant bookkeeping today; it is kept so a future
worker-thread evaluation (spec 7.7) does not need adapter changes.

`t_results.py`:

| Class | Output | Returns | Expected |
|---|---|---|---|
| `PyRaises` (attr form is enough here; bundle variant in 7.6) | `_PY_RAISE` , input `meas("IMU","wz")` | `raise ValueError("boom")` if `wz[0] == 0` else `1.0` | see test |
| `PyTwoD` | `IMU/pyTwoD` | `np.zeros((2, 2))` | Failed |
| `PyScalarMeas` | `IMU/pyScalar` | `3.0` | Failed (0-D) |
| `PyStrArray` | `IMU/pyStrArr` | `np.array(["a", "b"])` | Failed |
| `PyBoolArray` | `IMU/pyBoolArr` | `np.array([True, False])` | Failed |
| `PyStrMeas` | `IMU/pyStrMeas` | `"1.5"` | Failed |
| `PyBoolAttr` | `_PY_BOOL` | `True` | Failed |
| `PyListAttr` | `_PY_LIST` | `[1, 2]` | Failed |
| `PyNanAttr` | `_PY_NAN` | `float("nan")` | unavailable, status `Ok` |
| `PyNoneAttr` | `_PY_NONE` | `None` | unavailable, status `Ok` |
| `PyIntAttr` / `PyNpFloatAttr` / `PyNpIntAttr` / `PyStrAttr` | `_PY_INT` / `_PY_NPF` / `_PY_NPI` / `_PY_STR` | `3` / `np.float64(2.5)` / `np.int64(7)` / `"2024-01-01T00:00:00Z"` | `3.0`, `2.5`, `7.0` (all `QMetaType::Double`), and the **string unchanged** (`QMetaType::QString`) |
| `PyListMeas` / `PyF32Meas` / `PyStrideMeas` | `IMU/pyList` / `IMU/pyF32` / `IMU/pyStride` | `[1, 2]` / `np.array([1.5, 2.5], dtype=np.float32)` / `np.arange(4.0)[::2]` | `{1,2}`, `{1.5,2.5}`, `{0,2}` |
| `PyKeepsBuffer` | `IMU/pyKeep`, input `meas("IMU","wx")` | module-global `buf = np.array([1.0, 2.0, 3.0])`, returned as-is; module function `poke()` does `buf[:] = -1.0`, then `del` + `gc.collect()` | `{1,2,3}` before and after `poke()`, `runCount == 1` |
| `PyShadowATotal` | `IMU/aTotal`, input `meas("IMU","ax")` | `np.full(len(ax), 42.0)` | see test |

Test functions:
- `exceptionYieldsCleanUnavailable` (acc. 17) - `_PY_RAISE` unavailable; `resultStatus == Failed`; the captured log has exactly one message containing `ValueError: boom` and `t_results.py`; three more reads: `runCount == 1`, no new message (negative caching); `_PY_WX0` (another plugin) still near `71.68`; `session.setSourceMeasurement("IMU","wz",{1.0,1.0},"deg/s")` returns a set containing `_PY_RAISE`; next read `== 1.0`, `runCount == 2`; `calculationEngine().scopeDepth() == 0`.
- `malformedOutputsAreUnavailable` (data-driven over the Failed rows) - unavailable, `resultStatus == Failed`, one warning naming the plugin, `runCount == 1` after repeated reads.
- `acceptedReturnTypes` - the `Ok` rows with the literals above, including `QVariant::typeId()` checks.
- `returnedArrayIsCopied` - `IMU/pyKeep == {1,2,3}`; call `t_results.poke()`; re-read `{1,2,3}`; `runCount == 1`.
- `pluginBeatsBuiltinStoredBeatsBoth` - fixture session: `IMU/aTotal == {42.0, 42.0}`, `runCount("builtin.imu.aTotal") == 0`; second session whose file also has a recorded `aTotal` column (`m/s^2`, values `7, 7`): `IMU/aTotal == {7.0, 7.0}` and the plugin's `runCount == 0`.
- `measurementPluginUnit` - `session.effectiveUnit("IMU","pyWx2") == "deg/s"`.

**Acceptance Criteria:**
- [ ] No `py::object` is captured by value in any lambda stored in the registry (`git grep -n "\[plugin\]" -- src` returns nothing).
- [ ] No Python exception type crosses into `engine/*`: the only exception the adapters let escape is `PluginBridge::PluginError`.
- [ ] `git grep -n "ISODate\|QDateTime" -- src/pluginhost.cpp src/pluginadapters.cpp` returns nothing.
- [ ] The test process exits with code 0 and no crash at static destruction, in both orders of first touching `CalculationRegistry::instance()` and `PluginHost::instance()` (checked once by hand by calling `registerBuiltIns()` first in a scratch run; not committed).
- [ ] All tests listed pass.

**Complexity:** L

---

### Task 7.6: Multi-output plugin form

**Purpose:** Replace the removed cache setter with the engine's native shape: one computation returning a bundle of declared outputs.

**Files to modify:**
- `python_plugins/flysight_plugin_sdk.py`
- `src/pluginadapters.h` / `.cpp`, `src/pluginhost.cpp`

**Files to create:**
- `tests/python_plugins/t_multi.py`

**Technical Approach:**

SDK:

```python
class CalculationPlugin:
    """One computation, several declared outputs (see README)."""
    name:  str                      # optional; defaults to the class name. Used in the id plugin.calc.<i>.<name>
    units: Dict[Key, str] = {}      # optional unit label per *measurement* output

    def inputs(self)  -> List[Key]: return []
    def outputs(self) -> List[Key]: raise NotImplementedError      # attr() and/or meas() keys, at least one
    def compute(self, session) -> Optional[Dict[Key, object]]: raise NotImplementedError

_calculations: List[CalculationPlugin] = []
def register_calculation(plugin: CalculationPlugin) -> None: _calculations.append(plugin)
```

No `policy` attribute is exposed: spec 7.7 migrates nothing to explicit mode
and there is no UI that could request a plugin calculation. All plugin
calculations are `EvaluationPolicy::OnDemand`.

Host: `CalculationDescriptor makeCalculationAdapter(int index, py::object plugin)`:
- `name` = `getattr(plugin, "name", type(plugin).__name__)`; id `plugin.calc.<i>.<name>`.
- `outputs` from `decodeOutputs` (non-empty; `"source"` or unknown kinds are
  `PluginError`s; duplicates and output-equals-input are refused by the
  registry and surface as a rejection).
- `units`: must be a `dict` whose keys decode to declared *measurement*
  outputs and whose values are `str`; anything else is a `PluginError` at
  registration.
- Compute: same skeleton as Task 7.5. Bundle rules:
  - `None` -> every output unavailable, status `Ok`.
  - Not a `dict` (`PyDict_Check`) -> malformed.
  - Each dict key is decoded with `decodeKey` and must equal a declared output;
    an undecodable or **undeclared key makes the whole result malformed**.
  - Value `None`, or a declared output with no entry -> that output
    unavailable (a **partial result**, status `Ok`).
  - Attribute values through `toAttributeValue`, measurement values through
    `toMeasurementValues` (+ the registered unit); any malformed value makes
    the whole result malformed.
  - Build the complete `CalculationResult` in a local and return it only if
    every entry validated. Nothing is published from a partially validated
    bundle, and the engine publishes the bundle atomically.
- Registered after `_attributes` and `_measurements`.

`t_multi.py`:

| Class | Inputs | Outputs | Returns |
|---|---|---|---|
| `PyGyroStats` | `meas IMU wx, wy, wz` | `attr("_PY_W_MAX")`, `attr("_PY_W_MIN")`, `meas("IMU","pyWNorm")`; `units = {meas("IMU","pyWNorm"): "deg/s"}` | max / min over all three columns; `sqrt(wx^2+wy^2+wz^2)` |
| `PyPartial` | `meas IMU wx` | `_PY_PART_A`, `_PY_PART_B`, `_PY_PART_C` | `{attr("_PY_PART_A"): 1.0, attr("_PY_PART_B"): None}` |
| `PySourceProbe` | `source("IMU","ax")`, `meas("IMU","ax")` | `meas("IMU","pySrcAx")`, `attr("_PY_SRC_UNIT")`, `attr("_PY_EFF_UNIT")`, `attr("_PY_EFF_AX0")`, `attr("_PY_HAS_SRC")` | source samples, `sourceUnit`, `effectiveUnit`, first effective sample, `1.0 if session.hasSourceMeasurement("IMU","ax") else 0.0` |
| `PyBundleRaises` | `meas IMU wz` | `_PY_BR_A`, `_PY_BR_B` | builds `{A: 1.0}` then `raise ValueError("late boom")` if `wz[0] == 0`, else `{A: 1.0, B: 2.0}` |
| `PyWrongKey` | `meas IMU wx` | `_PY_WK_A` | `{attr("_PY_WK_A"): 1.0, attr("_PY_WK_OTHER"): 2.0}` |
| `PyNotDict` | `meas IMU wx` | `_PY_ND_A` | `5.0` |
| `PyBadMember` | `meas IMU wx` | `_PY_BM_A`, `meas("IMU","pyBmM")` | `{A: 1.0, M: np.zeros((2,2))}` |
| `PySourceOutput` | - | `outputs()` -> `[source("IMU","wx")]` | rejected at registration |
| `PyNoOutputs` | - | `[]` | rejected at registration |

Test functions:
- `multiOutputRunsOnce` (acc. 17) - read `IMU/pyWNorm`, `_PY_W_MIN`, `_PY_W_MAX`, then all three again in another order: `_PY_W_MAX` near `71.68`, `_PY_W_MIN` near `-143.36`, `IMU/pyWNorm` two samples near `160.2813526271849`, `effectiveUnit == "deg/s"`; `runCount(id) == 1`. `setSourceMeasurement("IMU","wy",{0.0,0.0},"deg/s")` returns a set containing all three names that were read; after re-reading all three `runCount == 2`, `_PY_W_MIN == 0.0`; an unrelated `setAttribute("_DESCRIPTION","x")` causes no run.
- `partialBundle` - `_PY_PART_A == 1.0`; `_PY_PART_B`, `_PY_PART_C` unavailable; `resultStatus == Ok`; `runCount == 1`.
- `sourceAccessMatchesCpp` (acc. 17) - `IMU/pySrcAx` equals `session.sourceMeasurement("IMU","ax")` element-wise (`{1.0, 0.0}`); `_PY_SRC_UNIT == session.sourceUnit("IMU","ax") == "g"`; `_PY_EFF_UNIT == session.effectiveUnit("IMU","ax") == "m/s^2"`; `_PY_EFF_AX0 == 9.80665`; `_PY_HAS_SRC == 1.0`. Then `setSourceMeasurement("IMU","ax",{2.0,0.0},"g")`: the returned set contains `IMU/pySrcAx`; the next read is `{2.0, 0.0}` (source reads are tracked dependencies).
- `bundleExceptionPublishesNothing` (acc. 17) - both `_PY_BR_A` and `_PY_BR_B` unavailable although `A` had been placed in the dict; status `Failed`; `runCount == 1` after repeated reads; after `wz` changes to `{1,1}` both are available (`1.0`, `2.0`) with `runCount == 2`.
- `malformedBundles` (data-driven: `PyWrongKey`, `PyNotDict`, `PyBadMember`) - **every** declared output unavailable (for `PyBadMember` including the valid `_PY_BM_A`), status `Failed`, one warning each.
- `badOutputDeclarationsRejected` - `PySourceOutput` and `PyNoOutputs` appear in `report().rejected`.

**Acceptance Criteria:**
- [ ] `git grep -n "policy" -- python_plugins` returns nothing.
- [ ] The bundle is validated completely before any `CalculationResult` is returned (one return statement on the success path).
- [ ] All listed tests pass; `registrationOrderIsDeterministic`'s literal list now ends with the `plugin.calc.*` ids.

**Complexity:** M

---

### Task 7.7: Bundled example plugin and packaging

**Purpose:** Ship the one multi-output example the spec asks for, without auto-loading anything in production, and make sure the shipped file is the one the tests run.

**Files to create:**
- `python_plugins/examples/imu_tilt.py`

**Files to modify:**
- `src/CMakeLists.txt` 481-483 (SDK install rule) - also install `examples/imu_tilt.py` to `${_plugins_dest}/examples` and `python_plugins/README.md` (Task 7.9) to `${_plugins_dest}`.
- `cmake/CreateAppDir.cmake` 275-280 - add `PATTERN "examples" EXCLUDE` and `PATTERN "*.md" EXCLUDE` (this rule copies the whole `python_plugins/` tree into the AppImage's **site-packages**, where an `examples` package and a README do not belong).
- `tests/tst_python_bridge.cpp` - copy the example into the temp plugin dir (already anticipated in Task 7.2) and add the test below.

**Technical Approach:**

**Location decision.** `PluginHost` imports every `*.py` **directly inside** the
plugin directory (`QDir::Files`, non-recursive; baseline 264-272), and on
Windows / macOS only files named in the install rule are deployed. An example
placed at `python_plugins/imu_tilt.py` and installed would run for every user
on every session. It therefore lives in `python_plugins/examples/`, which is
never scanned. A user enables it by copying it one level up (or pointing
`FLYSIGHT_PLUGINS` at a folder containing it and the SDK). The test loads it by
copying it next to the SDK in its temp plugin directory.
(`cmake/BundlePythonLinux.cmake` 337 globs only top-level `*.py`, so it is
unaffected. `cmake/BundlePythonMacOS.cmake` 438-444 refers to a non-existent
`../plugins/` path and is dead; leave it.)

**Content** - genuinely one computation, several outputs, no name collisions
with built-ins (`IMU/aTotal`, `IMU/wTotal` are the only built-in IMU outputs):

```
class ImuTilt(CalculationPlugin):
    name = "ImuTilt"
    units = { meas("IMU","tiltPitch"): "deg", meas("IMU","tiltRoll"): "deg" }
    inputs  -> meas("IMU","ax"), meas("IMU","ay"), meas("IMU","az")
    outputs -> meas("IMU","tiltPitch"), meas("IMU","tiltRoll"), attr("_IMU_PEAK_ACCEL")
    compute -> g = sqrt(ax^2+ay^2+az^2) (computed once, used by all three outputs)
               pitch = degrees(arctan2(-ax, sqrt(ay^2+az^2))); roll = degrees(arctan2(ay, az))
               peak  = float(g.max()) ; returns the three-entry dict; None if the arrays are empty or lengths differ
register_calculation(ImuTilt())
register_plot(SimplePlot("IMU (examples)", "Tilt pitch", "deg", "#8E24AA", "IMU", "tiltPitch", "angle"))
register_plot(SimplePlot("IMU (examples)", "Tilt roll",  "deg", "#3949AB", "IMU", "tiltRoll",  "angle"))
```

Module docstring: what it demonstrates (declared inputs, effective reads in
m/s^2 regardless of the file's `g`, bundle return, partial/None), how to enable
it, and a pointer to the README. Keep it under ~60 lines.

Test `bundledExampleRuns` (acc. 17, "with one example"): on the fixture
(effective `ax = {9.80665, 0}`, `ay = {0, 0}`, `az = {9.80665, 9.80665}`):
`IMU/tiltPitch` near `{-45.0, 0.0}`; `IMU/tiltRoll` near `{0.0, 0.0}`;
`_IMU_PEAK_ACCEL` near `13.868697431446114`; `effectiveUnit("IMU","tiltPitch") == "deg"`;
`runCount` of the `...ImuTilt` id `== 1` across all reads; the two plots are in
`PlotRegistry`.

**Acceptance Criteria:**
- [ ] `python_plugins/` top level contains only `flysight_plugin_sdk.py` and `README.md`; after `cmake --install`, `<install>/python_plugins/` contains the SDK, the README, and `examples/imu_tilt.py`; launching the installed app logs zero registered calculations.
- [ ] Copying `examples/imu_tilt.py` into `<install>/python_plugins/` and restarting adds "Tilt pitch" / "Tilt roll" to the plot list and they plot for a session with IMU data.
- [ ] `bundledExampleRuns` passes against the file in `python_plugins/examples/` (not a copy kept under `tests/`).

**Complexity:** S

---

### Task 7.8: Dead-code removal and audit

**Purpose:** Spec 12 / acceptance 19 for the bridge: no direct cache setter, no dead bridge header, no second key type.

**Files to delete:**
- `src/bridgeimpl.h` (declares `get_key_name` / `session_get_time_series`, "implemented in bridge_impl.cpp" - no such file exists, nothing includes it, it is in no CMake source list).

**Files to modify:**
- `src/pluginhost.h` - fix the stale comment at 19-21 ("before you register your built-in CalculatedValues") to describe the real contract: call once, after `QCoreApplication` exists, **before** `registerBuiltInCalculations()` so plugin candidates precede built-ins; remove the unused `namespace FlySight { class SessionData; }` forward declaration.
- `src/cpp_bridge.cpp` - update the module docstring / comments; keep `PythonOutputRedirector_CPP`.

**Acceptance Criteria:**
- [ ] `git grep -nE "setCalculated|set_calculated|bridgeimpl|bridge_impl|get_key_name|session_get_time_series" -- src python_plugins tests cmake CMakeLists.txt ':!python_plugins/README.md'` returns nothing.
- [ ] `git grep -n "DependencyKey" -- python_plugins ':!python_plugins/README.md'` returns nothing; the bridge module exports exactly `SessionData`, `PythonOutputRedirector_CPP`, `UndeclaredInputError` (check with `dir(flysight_cpp_bridge)` in `bootsRealBridge`, ignoring dunder names).
- [ ] `git grep -n "sessiondata.h" -- src/pluginhost.cpp src/pluginadapters.cpp src/sessiondata_bindings.cpp src/cpp_bridge.cpp src/pluginsessionview.h` returns nothing (the bridge never sees the C++ `SessionData`).
- [ ] Application and all tests build and pass with `FLYSIGHT_BUILD_TESTS` ON; the application builds with it OFF.

**Complexity:** S

---

### Task 7.9: Plugin documentation

**Purpose:** Spec 10, second bullet: document source versus effective access, the attribute conflict rule, and the plugin changes in the plugin README and SDK docstrings.

**Files to create:**
- `python_plugins/README.md`

**Files to modify:**
- `python_plugins/flysight_plugin_sdk.py` - module docstring and class docstrings.
- `tests/README.md` - replace the Phase 1 placeholder section "Adding tests that need Python (Phase 7)" with the real instructions (append-only elsewhere).

**Technical Approach:**

**Location decision:** `python_plugins/README.md`, next to the SDK, because it
is installed with it (Task 7.7) and is what a plugin author opens first.
`docs/DATA_SCHEMA.md` belongs to Phase 8; this README links to it and does not
duplicate it. The root `README.md` is left to Phase 8 (it is also being edited
by the parallel track).

README sections (keep each short, with one runnable snippet where marked):
1. **What a plugin is / where plugins live** - default folder per platform (`<app>/python_plugins`, macOS `Contents/Resources/python_plugins`), `FLYSIGHT_PLUGINS` override, every top-level `*.py` is imported at startup, files load in name order, `examples/` is not loaded.
2. **The simple forms** *(snippet)* - `AttributePlugin` (`_PY_DURATION` from `meas("GNSS","_time")`, returning seconds as `float`) and `MeasurementPlugin` (`units`, one value per sample of the sensor's time vector; length is not checked by the host, mismatched plots are skipped). These two snippets are the rewritten former `DefaultDuration` / `DefaultTime` examples.
3. **The declared-inputs rule** - `inputs()` is read once at startup; **all declared inputs are required** - the plugin runs only when every one is available, so do not declare "just in case"; to support two alternative inputs register two plugins for the same output (order = preference); any read of an undeclared key raises `UndeclaredInputError` naming the key and the fix, and the result is unavailable even if the exception is caught; `compute` must be a pure function of its inputs (no clock, randomness, files, globals); the `session` object is valid only during `compute`.
4. **Effective versus source values** *(snippet)* - `getMeasurement` returns what Viewer itself uses: schema-corrected, SI-normalized values under the recorded names (legacy gyro x 1.14688, `g` -> m/s^2, `gauss` -> T); `effectiveUnit`; `source(sensor, name)` + `sourceMeasurement` / `sourceUnit` / `hasSourceMeasurement` return exactly what the file recorded, never compute, never fall back to a derived value; a plugin that declares the source of a name that has no recorded data does not run. Link: `docs/DATA_SCHEMA.md`.
5. **Multi-output calculations** *(snippet = a trimmed `imu_tilt.py`)* - `outputs()`, dict keyed by the same `attr()` / `meas()` keys, `None` or a missing entry = that output unavailable, undeclared key or malformed value = nothing is published, `units`, the computation runs once per session however many outputs are read.
6. **Return types** - the two tables from Task 7.5 in prose form; times are UTC seconds (`datetime.timestamp()`); **strings are never parsed as dates** (changed); `bool` is rejected; NaN/inf attribute = no value; arrays are copied, so reusing or mutating a returned buffer is safe; reads return NumPy arrays that are private copies.
7. **Errors** - an exception or malformed output gives an unavailable result, the traceback is written once to the application log (`[PluginHost] ... raised:`), other plugins are unaffected, and the plugin is **not called again until one of its declared inputs changes**; registration problems (unknown key kind, bad `outputs()`, duplicate id) reject that plugin only and are logged at startup.
8. **Precedence** - resolution order for a name is: recorded data / stored attribute first, then plugins in registration order, then built-ins. A plugin that declares a built-in output (e.g. `IMU/aTotal`) replaces it whenever the plugin's inputs are available; it can never override recorded data or a value the user has set.
9. **What was removed** - `session.setCalculatedMeasurement(...)` (the direct cache setter): plugins *return* results, they do not publish them; use `CalculationPlugin` for several outputs. `flysight_cpp_bridge.DependencyKey`: use `attr()` / `meas()` / `source()`. The `Default*` example classes. Preference inputs are not available to plugins.
10. **Header attributes and the conflict rule** (one paragraph) - attributes read from a file's `$VAR` lines (`FIRMWARE_VER`, `SCHEMA_VER`, `DEVICE_ID`, ...) reach plugins through `getAttribute` exactly as recorded, numeric-looking text arriving as `float`; an absent attribute is "no value" (a plugin declaring it simply does not run) - never assume a default, in particular for `SCHEMA_VER`, whose interpretation belongs to the conversion layer; when a second file is merged into a session, a header attribute with a *different* value makes that import fail rather than overwrite, so a plugin can rely on header attributes being single-valued per session. Details: `docs/DATA_SCHEMA.md`.
11. **Plots and markers** - pointer to the `SimplePlot` / `SimpleMarker` docstrings (unchanged).
12. **Testing a plugin** - pointer to `tests/README.md` and `tests/python_plugins/`.

SDK docstrings: module docstring lists five extension points and the
source/effective distinction in two sentences; `Key`, `attr`, `meas`, `source`
get one-line docstrings; `AttributePlugin` / `MeasurementPlugin` /
`CalculationPlugin` docstrings state inputs rule, return types, and error
behavior in 5-8 lines each and point to the README. Fix the existing wrong
docstring "Return a single QVariant-compatible value or small NumPy array"
(84-85): attributes are `float | int | str | None`. Fix `SimpleMarker`'s
`measurements` description (182) to say `(sensor, time_vector, data_vector)`
triples, matching `pluginhost.cpp` 403-410.

`tests/README.md`: how `tst_python_bridge` finds Python (`PYTHONHOME`,
`PYTHONPATH` to the build-tree bridge, DLL dir on Windows - all set by CTest),
the NumPy requirement and the DISABLED behavior,
`FLYSIGHT_BUILD_PYTHON_TESTS`, how to run it outside CTest (the three variables
to set by hand), the one-interpreter-per-process rule (all plugin files are
loaded in `initTestCase`; add a file under `tests/python_plugins/` and a test
function, never a second `initialise`), and the `slots` include-order rule.

**Acceptance Criteria:**
- [ ] Every snippet in `python_plugins/README.md` is exercised: either it is `examples/imu_tilt.py` or an equivalent class exists in `tests/python_plugins/` (name the file in an HTML comment under the snippet).
- [ ] The README states each of: source vs effective access, declared-inputs rule, multi-output form, return types, error behavior, registration-order precedence, removal of the cache setter, the attribute conflict rule with a link to `docs/DATA_SCHEMA.md`.
- [ ] `python -c "import ast,sys; ast.parse(open('python_plugins/flysight_plugin_sdk.py').read())"` succeeds and the SDK contains no reference to `DependencyKey`, `setCalculatedMeasurement`, or ISO strings as return values.
- [ ] No mention of gyro-scaling preferences, schema stamping, or import-time correction.

**Complexity:** M

---

## Testing Requirements

### Unit Tests
- New target `tst_python_bridge` (Tasks 7.2-7.7) - the only target linking `pybind11::embed`. Consolidated case list, with the acceptance-17 clauses marked:

| Test | Task | Acc. 17 clause |
|---|---|---|
| `bootsRealBridge`, `secondInitialiseIsNoOp`, `plotAndMarkerRegistrationUnaffected`, `pluginOutputsAreNotEnumerated` | 7.2 | real bridge |
| `singleOutputPluginsReadEffectiveValues` | 7.2 | existing single-output plugins, effective reads (62.5 -> 71.68 within 1e-9) |
| `unknownKindRejectsPluginOnly`, `registrationOrderIsDeterministic` | 7.3 | - (spec 8 last bullet) |
| `undeclaredReadDiagnostic`, `swallowedUndeclaredReadStillFails`, `undeclaredSourceRead`, `staleViewRaises` | 7.4 | - (spec 7.1) |
| `sourceOfDerivedNameIsAbsent` | 7.4 | source access matches C++ (absence) |
| `exceptionYieldsCleanUnavailable` | 7.5 | Python exception -> clean unavailable, others unaffected, no re-run until input change |
| `malformedOutputsAreUnavailable`, `acceptedReturnTypes`, `returnedArrayIsCopied`, `measurementPluginUnit`, `pluginBeatsBuiltinStoredBeatsBoth` | 7.5 | - (spec 8 bullet 4; overview "Registration order") |
| `multiOutputRunsOnce`, `partialBundle` | 7.6 | multi-output runs once |
| `sourceAccessMatchesCpp` | 7.6 | source access matches C++ (values + unit) |
| `bundleExceptionPublishesNothing`, `malformedBundles`, `badOutputDeclarationsRejected` | 7.6 | never a partial publication |
| `bundledExampleRuns` | 7.7 | "with one example" |

- Additions to existing suites: `tst_calcregistry::sourceInputsOptIn`, `tst_calcengine::optInSourceRead`, `tst_calcengine::isDeclaredIsSilent` (Task 7.1).
- Every expected number is a literal (`71.68`, `143.36`, `-143.36`, `160.2813526271849`, `13.868697431446114`, `-45.0`, `9.80665`, ...); corrected-gyro values are compared with `1e-9` tolerance (Phase 4 rule: `62.5 * 1.14688` is `71.67999999999999`).
- Log assertions use `qInstallMessageHandler`, installed **after** `initTestCase` finished and removed in `cleanup()`.
- All earlier suites pass unchanged.

### Integration Tests
- After every task: build with `-DFLYSIGHT_BUILD_TESTS=ON`; `ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure`; after 7.3, 7.7, 7.8 also build with the option OFF and run `cmake --install` once to check the `python_plugins/` layout.
- The grep criteria of Tasks 7.1, 7.3, 7.4, 7.5, 7.8 are part of the check (acceptance 19, bridge part).
- **CI** (`.github/workflows/build.yml`; only relevant if Phase 1's optional "Run tests" step was adopted): the workflow installs CPython 3.13 with `actions/setup-python` (301-306) but **no NumPy** in that interpreter (NumPy is only pip-installed into the *bundled* runtime at install time). Add one step directly after "Setup Python": `python -m pip install numpy`. Without it the test is reported as disabled and CI stays green. Cost: one extra small executable per platform and < 5 s of run time; no effect on packaging (tests have no install rules). Like all workflow edits this cannot be verified without a push: make it a separate commit and tell Michael it is unverified. macOS `libpython` lookup for the test executable is the most likely thing to need a follow-up (see Task 7.2).

### Manual Verification
1. Normal build and install; launch: log shows the bridge and SDK importing and `Registered 0 ...`; no behavior change.
2. Copy `examples/imu_tilt.py` next to the SDK in `build/install/python_plugins/`; relaunch; "Tilt pitch" / "Tilt roll" plots appear and plot for an imported `SENSOR.CSV`; values look like degrees.
3. Put a scratch plugin with a `raise` in `compute` in the folder: the app keeps working, one traceback appears in the log when its output is first needed, and it does not repeat while scrolling / replotting.
4. Scratch plugin reading an undeclared measurement: the log names the plugin, the key, and the `meas(...)` line to add.
5. Scratch plugin with `Key("bogus", ...)`: rejected at startup with a clear message; other plugins load.
6. Set `FLYSIGHT_PLUGINS` to an empty folder: app starts, warns that the SDK could not be imported, no crash (existing behavior).
7. Quit the application with a plugin loaded: clean exit, no crash dialog.

## Notes for Implementer

### Gotchas
- **Two registries.** Anything compiled into `flysight_cpp_bridge` (`cpp_bridge.cpp`, `sessiondata_bindings.cpp`, and the inline methods of `pluginsessionview.h` when called from Python) runs against the bridge's private copy of every `flysight_model` static. It may call methods on the `EvaluationContext` it was handed and nothing else. `pluginadapters.cpp` must never be added to the bridge target; `sessiondata_bindings.cpp` must never be added to the executable.
- **Interpreter is once per process.** `initialise()` returns immediately the second time; there is no teardown. All test plugin files must exist in the directory before the first call. Global registry state (plugin calculations, plots, markers) persists for the life of the test process - choose unique `_PY_*` / `py*` names and never assert that a registry is empty.
- **Plugins before built-ins in the test**, or `pluginBeatsBuiltinStoredBeatsBoth` measures the wrong thing. `TestEnvironment::registerBuiltIns()` is guarded process-wide, so call it exactly once, after `initialise`.
- **`slots` macro.** Include pybind11 headers before Qt headers, wrapped in `#pragma push_macro("slots") / #undef slots / pop`, and mask `_DEBUG` around `<Python.h>` on MSVC - copy `pluginhost.cpp` 21-36.
- **`bool` is an `int` in Python** and `np.bool_` arrays forcecast silently to 0/1: check bool before int, and check `dtype.kind` before `forcecast`.
- **`py::array::ensure` / `array_t::ensure` return a null handle and leave a Python error set** on failure; call `PyErr_Clear()` before throwing `PluginError`.
- **Never keep a `py::error_already_set` alive outside the GIL scope** and never let it reach the engine: log, convert, destroy.
- **`Key` must stay a frozen dataclass** (hash + eq by value); bundle lookup depends on it. Do not add mutable fields.
- **`source()` expands to two `CalcInput`s.** `lastUndeclaredRead()` and `dependenciesOf` tests must expect `sourceMeasurement` and `sourceUnit` separately.
- **An empty recorded unit is a legitimate value**; never use "empty string" to detect an undeclared `sourceUnit` read - that is what `isDeclared` is for.
- **Returned NumPy arrays from reads are copies.** Do not try to be clever with `py::capsule`-backed zero-copy views of `QVector` storage: the buffer is shared with the source layer and the engine cache, and a writeable view would let a plugin corrupt recorded data.
- **Windows `PYTHONPATH` contains `;`.** Use `ENVIRONMENT_MODIFICATION` (CMake >= 3.22) as specified; do not put it in `flysight_add_test(... ENVIRONMENT ...)`.
- **Install-time NumPy is not build-time NumPy.** `cmake/BundlePythonWindows.cmake` installs NumPy into the embeddable runtime under `<build>/python-embed/`; the test deliberately does not use that runtime (its `._pth` file puts CPython in isolated mode, which ignores `PYTHONPATH`, so the build-tree bridge could not be found).
- **Linux packaging quirk (pre-existing, not fixed here):** the AppImage installs the SDK into site-packages and does not create `<appdir>/usr/bin/python_plugins`; the default plugin directory therefore does not exist there. Only the `EXCLUDE` patterns are added in this phase.
- `QDir::entryInfoList({"*.py"}, QDir::Files)` also imports `flysight_plugin_sdk.py` itself as a "plugin" (a harmless re-import of an already imported module); leave it.
- Parallel phases edit `tests/CMakeLists.txt` and `tests/README.md`: keep this phase's edits in one appended block / one replaced section.

### Decisions Made
- **Python face of `CalcInput` = pure-Python `Key` dataclass in the SDK**, decoded by one explicit table in the host; the bridge-bound `DependencyKey` class is deleted. Rationale: value semantics (needed for bundle dict keys) for free, no C++ binding to keep in sync, the C++ `DependencyKey` public-name type stays untouched, `attr()` / `meas()` call sites unchanged. There are no third-party plugins, so the type change is free.
- **Unknown / malformed kind = registration error for that plugin**: logged with the plugin label, plugin skipped, others continue; never a crash, never a default.
- **Preference inputs are not exposed to Python**; `"preference"` is rejected with a specific message.
- **Source access is a declared input kind** (`source(sensor, name)` -> `SourceMeasurement` + `SourceUnit`), enabled by a new descriptor opt-in `allowSourceInputs` that only the plugin host sets. Spec 7.5 says only the conversion layer depends on source nodes; spec 8 requires plugin source access "with the same semantics as C++". Correct invalidation (spec 7.4) of a calculation that reads the source is only possible if the read is a tracked dependency, so the opt-in is the narrowest reconciliation: built-ins still cannot declare source inputs, source nodes are leaves (no cycle), and the read never computes. **Flagged for Michael's review.**
- **Absence of source data = the plugin does not run** (`MissingInput`), because all declared inputs are required; it never falls back to a derived value. `hasSourceMeasurement` is kept for symmetry with C++.
- **Undeclared read is Python-visible**: `UndeclaredInputError` raised at the offending call with plugin id, key, and the fix; the evaluation is flagged first, so swallowing the exception cannot publish a result. Status `UndeclaredRead`.
- **Stale view raises `RuntimeError`** (Phase 3 returned empty values; raising is easier to debug).
- **Reads return NumPy arrays (copies)** instead of Python lists.
- **Attribute return typing**: `None | float | int | numbers.Real | str`; `bool` rejected; non-finite = no value; **ISO-string-to-seconds rule dropped**; `getAttribute`'s numeric-looking-string -> `float` rule kept (it is how file attributes become usable numbers).
- **Measurement returns**: any 1-D real numeric array-like, copied by `memcpy` before the adapter returns; `ndim != 1`, bool / complex / object / string dtypes, `str`, `bytes` are malformed; empty = unavailable; **length not validated** (unknowable without an undeclared read; consumers already skip mismatches).
- **Malformed output and exceptions -> status `Failed`** through a `PluginError` thrown from the adapter (the engine's existing exception path gives atomic non-publication and negative caching). The engine's `InvalidOutput` status is not relied on: the adapter validates keys itself so the log can name the plugin.
- **Multi-output form**: `CalculationPlugin` / `register_calculation` / `_calculations`, ids `plugin.calc.<i>.<name>`, dict bundle keyed by `Key`, `None`/missing = partial, undeclared key or bad member = nothing published, static `units` dict, **no `policy`**.
- **Registration order**: `_attributes`, `_measurements`, `_calculations`; files in name order; plugins before built-ins (overview decision, still flagged there); stored data beats both. Documented in the README.
- **Lifetime**: `shared_ptr<PyPluginHolder>` with a `Py_IsInitialized()`-checking deleter instead of a shutdown ordering contract; no edit to `main.cpp` / `mainwindow.cpp`.
- **New file pair `src/pluginadapters.*`** for decoding / marshalling / adapters; `pluginhost.cpp` keeps interpreter boot, discovery, plots, markers.
- **Example**: `python_plugins/examples/imu_tilt.py` (`ImuTilt`: `IMU/tiltPitch`, `IMU/tiltRoll`, `_IMU_PEAK_ACCEL`), installed but **not auto-loaded**; tests run the shipped file.
- **Docs**: `python_plugins/README.md` + SDK docstrings; root `README.md` and `docs/DATA_SCHEMA.md` left to Phase 8.
- **Test environment**: system-Python branch of `PluginHost`, configured entirely by CTest (`PYTHONHOME` = build interpreter's `sys.base_prefix`, `PYTHONPATH` = build-tree bridge dir + NumPy's site dir, Windows `PATH` += `Python_RUNTIME_LIBRARY_DIRS`); no production seam added. Disabled (not omitted) when NumPy or CMake 3.22 is missing; `FLYSIGHT_BUILD_PYTHON_TESTS=OFF` omits it.
- **Engine additions requested of Phase 2's code**: `CalculationDescriptor::allowSourceInputs`, `EvaluationContext::isDeclared`. Nothing in Phases 2-4 is contradicted; the Phase 2 test that a plain calculation with a source input is rejected still holds for the default flag value.

### Open Questions
- **`allowSourceInputs`** relaxes the letter of spec 7.5 ("nothing else does") to satisfy spec 8. If Michael prefers the strict reading, the alternative is to drop Python source access to "unit text and samples of a declared *measurement* input, read untracked" - which breaks the idempotency invariant and is not recommended.
- The precedence **plugins before built-ins** remains the overview's flagged decision; this phase documents and tests it (`pluginBeatsBuiltinStoredBeatsBoth`). Reversing it later means moving one call in `MainWindow` and flipping that test.
- macOS / Linux CI behavior of `tst_python_bridge` (libpython lookup, `PYTHONHOME` with a framework build) is unverified until a push.

## Definition of Done

This phase is complete when:
1. All nine tasks have passing acceptance criteria.
2. `tst_python_bridge` and every earlier suite pass via CTest on Windows Release; the application and `flysight_cpp_bridge` build with `FLYSIGHT_BUILD_TESTS` ON and OFF; `cmake --install` produces `python_plugins/{flysight_plugin_sdk.py, README.md, examples/imu_tilt.py}`.
3. Spec coverage: 8 bullet 1 - Tasks 7.2, 7.4, 7.5; bullet 2 - 7.1, 7.4, 7.6 (`sourceAccessMatchesCpp`, `sourceOfDerivedNameIsAbsent`); bullet 3 - 7.6, 7.7, 7.8; bullet 4 - 7.5, 7.6 (`returnedArrayIsCopied`, `exceptionYieldsCleanUnavailable`, `bundleExceptionPublishesNothing`, `malformed*`); bullet 5 - 7.3; 4 (source never computes / absence) - 7.4; 7.1 (undeclared read is an error) - 7.4; 7.2 (unavailable cached, atomic publication) - 7.5, 7.6; 7.3 (order, stored precedence) - 7.5; 7.5 (exception leaves no partial result) - 7.5, 7.6; 7.7 (no plugin policy) - 7.6; 10 bullet 2 - 7.9; 12 (remove old mechanisms) - 7.3, 7.8; acceptance 17 - the marked tests; acceptance 19 (bridge part) - grep criteria of 7.4 and 7.8.
4. The bridge module exposes no way to write to the engine, the registry, or a session; plugins only return values.
5. Code follows the patterns in the reference files; no TODOs or placeholder code remain; files owned by Phases 5 and 6 are untouched; nothing has been pushed.
