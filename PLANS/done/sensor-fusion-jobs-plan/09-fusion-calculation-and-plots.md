# Phase 9: Fusion calculation, derived values, and plots

## Overview

This phase joins the three tracks. It registers the batch GNSS/IMU fit of Phase 8 as **one explicit multi-output calculation** titled "Sensor fusion" (`Fusion/...` and `_FUSION_DIAGNOSTICS`), as a thin adapter from the engine's `EvaluationContext` to `Fusion::Channels` and from `Fusion::Result` to a `CalculationResult`; registers the on-demand derived values `Fusion/accH` and `Fusion/_system_time`; registers the seventeen "Sensor fusion" plots; and links the application to `flysight_fusion`, calling its single registration entry point next to the built-in registration.

It then proves, with the real fit, spec acceptance 5, 6, 7, 8, 9, 10, 11 and 12 at session level (`tst_fusion_session`) and through the job queue (`tst_fusion_jobs`). It also closes Phase 5's open question: a logbook column whose value depends on an explicit calculation is never written into the `index.json` column cache, because explicit results are not persisted (spec section 2).

Nothing of the branch's mechanism is ported: no per-output registrations, no sibling writes, no thread-local guard, no modal dialog, no nested event loop, no message box, no hand-cached failure (spec section 2, acceptance 20).

## Dependencies

- **Depends on:** Phase 2 (local coordinates, `SessionKeys::LocalOrigin*`), Phase 4 (async request, blockers, `title`, `setReason`, progress facility), Phase 5 (`JobQueue`, `JobModel`, `jobfixture.h`), Phase 8 (`flysight_fusion`, `fusion/fusion.h`, fixtures, goldens).
- **Blocks:** Phase 10.
- **Assumptions:**
  - Phase 8's public API is exactly `FlySight::Fusion::{Channels, Outcome, Result, ProgressFn, CancelFn, run}` in `src/fusion/fusion.h`; `run()` throws nothing except `std::bad_alloc`; checkpoints call `progress(text)` then `cancelRequested()`, in that order, at three kinds of boundary only.
  - Phase 8's test support exists: `tests/fusion/fusionfixtures.h/.cpp` (`FlySightTest::FusionFixture`, `fusionFixture(name)`), `tests/fusion/fusiongolden.h/.cpp` (`loadFusionGolden`, `compareSamples`, `compareJson`, `toChannels`, `exactParityRequested`), the library `flysight_fusion_test_support`, goldens under `tests/data/fusion/`.
  - Phase 4's `CalculationDescriptor::title`, `CalculationResult::setReason()`, `EvaluationContext::progress()`, `CalculationProgress`, `CalculationCancelled`, `prepare()`, `blockers()`, `readiness()`, `resultDetail()`, `preparedCount()` and `tests/support/asyncdriver.h` exist as documented.
  - Phase 5's `JobQueue`, `JobModel`, `SessionModel::publishCalculationInvalidation()` and `tests/support/jobfixture.h` (`waitIdle`) exist as documented. The end-state mapping puts `PublishOutcome::detail` (the result's reason) into `JobRecord::reason`.
  - Phase 2's `builtin.local.coordinates` publishes `Local/north|east|down|velN|velE|velD` and the four origin attributes (`double` lat/lon/hMSL, `qlonglong` index) and `SessionKeys::LocalOriginLat|Lon|Hmsl|Index` exist in `src/sessiondata.h`.
  - Phase 1's `flysight_add_fusion_test`, `FLYSIGHT_BUILD_FUSION_TESTS`, and `flysight_install_solver_runtime` (already called on all three platforms, unconditionally) exist.
  - Phases 6 and 7 are **not** assumed. Nothing here needs the plot request component or the plot-list view; with Phase 7 absent the fusion plots are simply never computed in the application. If Phase 7 is open at the same time, `src/mainwindow.cpp` is a shared file (Commit Policy, plan-specific notes).
  - The implementer runs no git command that changes repository state.

### Facts established during documentation (do not re-derive)

- **Units.** On `master` effective `IMU/ax|ay|az` are m/s^2 (the conversion layer multiplies recorded `g` by 9.80665, `docs/DATA_SCHEMA.md` section 6) and effective `IMU/wx|wy|wz` are deg/s. That is exactly what `Fusion::Channels` expects; the deg-to-rad conversion (`wx * radians`) is inside the kernel's input adapter (Phase 8). **The Phase 9 adapter performs no arithmetic at all**: it copies values.
- **Legacy gyro scaling.** For a recording without `SCHEMA_VER`, `master`'s conversion layer multiplies the gyro channels by 1.14688 (`docs/DATA_SCHEMA.md` section 4). The branch (based on `v2026.04.1`) did not. A legacy real recording therefore gives different fusion numbers on `master` than those recorded in `branch:docs/PORT_VALIDATION.md`. This is correct ("effective values only", spec section 6) and matters only for the optional real-recording check (Task 9.8).
- **A result may carry an available attribute while its measurement outputs are unavailable.** `CalculationResult` (`src/engine/calculationresult.h` lines 16-19): "Any declared output that is not set is unavailable". Phase 4's synthetic `expA` does exactly this (`EA_DIAG = "rejected"`, `EA1`/`EA2` unset, reason set) and Phase 4 Task 4.5 asserts "its diagnostics attribute output is `Available`" while the others are `NotProduced`. No engine change is needed.
- **The plot x axis.** `PlotWidget` reads y first (`src/ui/docks/plot/PlotWidget.cpp` line 530) and then `session.getMeasurement(sensorID, m_xVariable)` for the plot's own sensor (line 540). `m_xVariable` is `_time` or `_system_time`. The legend and the measure tool do the same (`src/plottool/measuretool.cpp` lines 301, 395). So `Fusion/_time` (an output of the fit) and `Fusion/_system_time` (derived on demand) are everything the readers need; no reader changes.
- **Stored data always wins** (`docs/CALCULATIONS.md` section 5; Phase 4 "blockers" step 2): a measurement with source data is served by the conversion layer, not by a registered calculation. Tests use this to build sessions whose effective inputs are bit-identical to a Phase 8 fixture (Task 9.6).
- **Column caching.** `SessionModel::computeColumnValues()` (`src/sessionmodel.cpp` lines 1950-1984) is called from exactly two places, both of which feed the cache (`rebuildColumns`, line 174, and `fillMissingColumns`, line 1417). Live display of loaded rows (`data()`, lines 356-366) and sorting (lines 1996-2018) do not use it. A cached value is present-but-invalid for an unavailable column (`sr.cachedValues[i] = values.value(...)`), which is already how "unavailable" is cached.
- `SessionModel::updateAttribute()` (lines 1160-1200) is the application's edit path; it accepts any attribute key, so tests change the declared input `_LOCAL_ORIGIN_INDEX` through it.
- `DataExporter` refuses a sensor whose columns have unequal lengths (`src/dataexporter.cpp` line 118). Fixture sessions that go into a `SessionModel` (which saves them) must have equal-length columns per sensor; see Task 9.6.
- macOS: `cmake/fix_macos_rpaths.sh` `fix_executable()` rewrites every non-system dependency of the main executable that exists in `Frameworks` to `@rpath/...`, so the application's new reference to `libgtsam` needs no script change.

## Design summary (binding for all tasks)

### Names

| Thing | Name |
|---|---|
| Files | `src/fusion/fusionregistration.h`, `src/fusion/fusionregistration.cpp` (inside `flysight_fusion`) |
| Entry point | `void FlySight::Fusion::registerFusionCalculations(CalculationRegistry &registry = CalculationRegistry::instance());` |
| Explicit calculation | id `builtin.fusion.fit`, title `"Sensor fusion"`, policy `Explicit` |
| Derived | `builtin.fusion.accH`, `builtin.fusion.systemTime` (mirrors `builtin.local.systemTime`), policy `OnDemand` |
| Sensor | the string literal `"Fusion"` (as `"GNSS"`, `"Local"`, `"Simplified"`; no constant) |
| Attribute key | `SessionKeys::FusionDiagnostics[] = "_FUSION_DIAGNOSTICS"` in `src/sessiondata.h` |
| Id constants | in `fusionregistration.h`: `inline constexpr char FitCalculationId[] = "builtin.fusion.fit";` (for Phase 10 and application code; tests use literals) |

Ids keep `master`'s `builtin.<area>.<thing>` convention: they are registered by the application itself, not by a plugin. Nothing is named EKF.

### Declared inputs of `builtin.fusion.fit` (this order; all required)

```
GNSS/_time
Local/north  Local/east  Local/down  Local/velN  Local/velE  Local/velD
GNSS/hAcc    GNSS/vAcc   GNSS/sAcc
IMU/_time
IMU/ax  IMU/ay  IMU/az  IMU/wx  IMU/wy  IMU/wz
_LOCAL_ORIGIN_INDEX  _LOCAL_ORIGIN_LAT  _LOCAL_ORIGIN_LON  _LOCAL_ORIGIN_HMSL
```

Seventeen `CalcInput::measurement` and four `CalcInput::attribute`: exactly the members of `Fusion::Channels`, in member order. No preference, no marker, no `GNSS/lat`, no `TIME/...`, no `_TIME_FIT_*`: those are transitive and the engine tracks them. No `SourceMeasurement` (effective values only).

### Outputs of `builtin.fusion.fit`

`Fusion/_time, north, east, down, velN, velE, velD, accN, accE, accD, roll, pitch, yaw, qx, qy, qz, qw` (`DependencyKey::measurement("Fusion", ...)`, in this order) and `DependencyKey::attribute(SessionKeys::FusionDiagnostics)`.

### Outcome mapping

| `Fusion::Outcome` | What the compute function does |
|---|---|
| `Succeeded` | returns a result with all seventeen measurements (no unit argument, as every derived measurement on `master`) and `_FUSION_DIAGNOSTICS` = `result.diagnosticsJson` (a `QString`) |
| `Rejected`, `SolverFailed` | returns a result with **only** `_FUSION_DIAGNOSTICS` set, and `setReason(result.reason)`. The measurements are unset, therefore unavailable. The engine caches it like any other result (`ResultStatus::Ok`), the queue ends the job Succeeded with that reason, blocker inspection reports `NotProduced` with that detail |
| `Cancelled` | `throw CalculationCancelled();` |
| `std::bad_alloc` from `run()` or from copying | not caught: propagates to the engine, which classifies it `ResourceExhausted` on the asynchronous path |

There is no `try`/`catch` in `fusionregistration.cpp`.

## Tasks

### Task 9.1: Registration files, key, and library wiring

**Purpose:** Give `flysight_fusion` its single registration entry point and the engine types it needs, without letting GTSAM leak anywhere else.

**Files to create:**
- `src/fusion/fusionregistration.h` — declares the entry point and `FitCalculationId`. Includes `"engine/calculationregistry.h"` (needed for the default argument). Doc comment in the shape of `src/calculations/simplificationcalculations.h`: lists the three ids, their outputs, and states "the only function the application and the tests call; `flysight_core` never references it".
- `src/fusion/fusionregistration.cpp` — Tasks 9.2 and 9.3.

**Files to modify:**
- `src/sessiondata.h` — one line in `namespace SessionKeys` after Phase 2's local-origin keys, under the comment `// Sensor fusion diagnostics (calculated; compact JSON)`: `constexpr char FusionDiagnostics[] = "_FUSION_DIAGNOSTICS";`.
- `src/CMakeLists.txt` — in Phase 8's `flysight_fusion` block only: add `fusion/fusionregistration.cpp fusion/fusionregistration.h` to the source list and change the link line to `target_link_libraries(flysight_fusion PUBLIC flysight_model Qt${QT_VERSION_MAJOR}::Core PRIVATE gtsam)`. Update the block's comment: "Pure kernel plus its registration (`fusion/fusionregistration.*`, the only file here that sees the engine)".

**Technical Approach:**
- `flysight_model` (Qt Core only) supplies the engine headers and `SessionKeys`. `flysight_core` is **not** linked: `calculations/registration.h` (`addCalculation`) and `calculations/anglehelper.h` are header-only and reached through the existing `src/` include directory.
- `fusionregistration.cpp` includes `fusion/fusion.h`, `fusion/fusionregistration.h`, `calculations/registration.h`, `engine/calculationprogress.h`, `sessiondata.h` (for `SessionKeys` only), `<cmath>`. It includes no GTSAM header and no internal `src/fusion/` header.
- Do **not** register `_FUSION_DIAGNOSTICS` in the `AttributeRegistry` (`src/calculations/attributeregistration.cpp`): like `_TIME_FIT_A/B` and `_LOCAL_ORIGIN_*` it is not a user-facing logbook column.

**Acceptance Criteria:**
- [ ] `grep -n "#include" src/fusion/fusionregistration.*` shows no GTSAM, Eigen, Widgets, `sessionmodel`, `jobqueue`, or preference header.
- [ ] `grep -rn "sessiondata\|engine/" src/fusion` matches only `fusionregistration.h/.cpp` (Phase 8's kernel purity is intact).
- [ ] `flysight_core`, `flysight_model`, `flysight_test_support` and `flysight_cpp_bridge` have unchanged link lines; `grep -rn "fusion" src/calculations src/engine src/sessionmodel.* src/jobqueue.*` finds no reference to the fusion library.
- [ ] Scoped "no EKF" check (overview, Integration Note 4): in `src/` and in `tests/*.cpp` / `tests/*.h`, no identifier, file name, calculation id, sensor or attribute contains `ekf` (any case) or `_IMU_GNSS_EKF`, and there is no `posN` / `posE` / `posD` output. A bare `grep -rni "ekf" src tests` is not the test: `SP_MediaSeekForward` in `src/ui/docks/video/VideoWidget.cpp` matches it, and `tests/README.md` legitimately names the branch's files in the golden-capture procedure.

**Complexity:** S

---

### Task 9.2: The explicit "Sensor fusion" calculation

**Purpose:** Spec section 6: one explicit, titled, multi-output, pure calculation whose rejections and solver failures are cached results and whose cancellation abandons the run.

**Files to modify:** `src/fusion/fusionregistration.cpp`.

**Technical Approach:**

Anonymous-namespace helpers, small and named for intent (style of `src/calculations/localcoordinatecalculations.cpp` / `attributecalculations.cpp`):

- `QList<CalcInput> fitInputs()` and `QList<DependencyKey> fitOutputs()` — the lists of the design summary. Keep the seventeen output names in one file-local table `{ name, QVector<double> Fusion::Result::*member }` (the one idea worth keeping from the branch's `fusion_outputs`), used for both the declaration and the publication so they cannot drift:
  `_time→time, north, east, down, velN, velE, velD, accN, accE, accD, roll, pitch, yaw, qx, qy, qz, qw`.
- `Fusion::Channels channelsFrom(const EvaluationContext &ctx)` — field-by-field copies (`QVector` is implicitly shared: no sample is copied). Attributes: `originLat|Lon|HMSL = ctx.attribute(...).toDouble()`; `originIndex`: `bool ok = false; qlonglong v = ctx.attribute(SessionKeys::LocalOriginIndex).toLongLong(&ok); channels.originIndex = ok ? v : -1;` (a stored, hand-edited, non-numeric value becomes the kernel's "Local origin index outside GNSS samples" rejection instead of silently meaning fix 0). No arithmetic, no validation, no unit handling: all of that is the kernel's and is under golden test.
- `CalculationResult publish(const Fusion::Result &fit)` — the outcome table above for `Succeeded` / `Rejected` / `SolverFailed`.
- The compute function:
  1. `const Channels channels = channelsFrom(ctx);`
  2. `CalculationProgress &facility = ctx.progress();`
  3. `const Result fit = run(channels, [&facility](const QString &t) { facility.report(t); }, [&facility] { return facility.isCancelled(); });`
  4. `if (fit.outcome == Outcome::Cancelled) throw CalculationCancelled();`
  5. `return publish(fit);`
  It captures nothing, holds no static or mutable state (Phase 4: explicit compute functions must be re-entrant), and logs nothing (it may run on the worker; Phase 4: "log nothing from compute").
- Descriptor: `d.id = FitCalculationId; d.title = QCoreApplication::translate("Fusion", "Sensor fusion"); d.policy = EvaluationPolicy::Explicit;` then `Calculations::addCalculation(registry, d)`.
- On the synchronous `request()` path `ctx.progress()` is `CalculationProgress::none()`: text is dropped and cancellation never happens, so both paths run the same `run(channels, ...)` on the same values (acceptance 7).

**Acceptance Criteria:**
- [ ] `registry.title("builtin.fusion.fit") == "Sensor fusion"`; the instance has 21 inputs in the listed order, 18 outputs, policy `Explicit`.
- [ ] `fusionregistration.cpp` contains no `catch`, no `static` local, no `qWarning`/`qInfo`/`qDebug` inside the explicit compute function, and no reference to `SessionData`, `CalculationEngine`, `CalculationRegistry::instance()` inside any compute function.
- [ ] Every criterion of Tasks 9.7 and 9.8 that names this calculation passes.

**Complexity:** M

---

### Task 9.3: Derived on-demand calculations `Fusion/accH` and `Fusion/_system_time`

**Purpose:** Spec section 6 "Derived values": separate on-demand calculations that declare fusion outputs as inputs, unavailable until fusion publishes and appearing through ordinary invalidation.

**Files to modify:** `src/fusion/fusionregistration.cpp`.

**Technical Approach:**

| Id | Inputs (order) | Output | Definition |
|---|---|---|---|
| `builtin.fusion.accH` | `Fusion/accN`, `Fusion/accE` | `Fusion/accH` | per sample `std::sqrt(accN[i]*accN[i] + accE[i]*accE[i])` (the branch's expression, operand order kept); sizes differ → `CalculationResult::unavailable()` |
| `builtin.fusion.systemTime` | `Fusion/_time`, `_TIME_FIT_A`, `_TIME_FIT_B` | `Fusion/_system_time` | `a = attribute(TimeFitA).toDouble()`, `b = attribute(TimeFitB).toDouble()`; `a == 0.0` → unavailable; per sample `(time[i] - b) / a`; any non-finite result → the whole output unavailable |

- `accH` follows `registerImuMagnitude` in `src/calculations/imucalculations.cpp` (lines 13-52) without its warnings (both inputs come from one bundle; an empty input never reaches compute).
- `_system_time` mirrors `builtin.time.system.GNSS` in `src/calculations/timecalculations.cpp` (lines 127-152): the same inverse-fit expression the branch used through `utcToSystemTime` (`(utc - b) / a`), plus the branch's non-finite guard. It is **not** a passthrough of `IMU/time`: fused timestamps are a subset of the IMU samples expressed in UTC, and the branch defined the axis as the inverse fit of `Fusion/_time`.
- Both are `OnDemand`, registered after the fit inside `registerFusionCalculations`, so `registeredIds()` order is `builtin.fusion.fit`, `builtin.fusion.accH`, `builtin.fusion.systemTime`.
- No `Fusion/_time` passthrough is needed (it is an output of the fit), and nothing else is: see "The plot x axis" in Facts. For Phase 6's rule "y available implies x available": on a real recording `IMU/_time` exists only through the time fit, so whenever the fit published, `_TIME_FIT_A/B` exist and `a != 0` (a zero slope makes `IMU/_time` constant, which the kernel rejects).

**Acceptance Criteria:**
- [ ] Before a request both outputs are unavailable and `blockers()` of each reports `Blocked` by `builtin.fusion.fit` only; after publication both are available, aligned with `Fusion/_time`, without any request of their own (`runCount` 1 each after being read).
- [ ] `accH[i]` equals `std::sqrt(accN[i]*accN[i] + accE[i]*accE[i])` bit for bit; with stored `_TIME_FIT_A = "1"`, `_TIME_FIT_B = "1699999900"`, `_system_time[i] == _time[i] - 1699999900.0` exactly.

**Complexity:** S

---

### Task 9.4: Application wiring: link, registration call, seventeen plots

**Purpose:** Make the calculation and its plots exist in the application (spec section 6 last paragraph, Decision 2).

**Files to modify:**
- `src/CMakeLists.txt` — one line: add `flysight_fusion` to `target_link_libraries(FlySightViewer PRIVATE ...)` directly after `flysight_model` (line 423).
- `src/mainwindow.cpp` — three small hunks: (1) `#include "fusion/fusionregistration.h"` next to the `calculations/builtincalculations.h` include; (2) directly after `registerBuiltInCalculations();` (line 169) and before `registerBuiltInCalculationMetadata();`:
  ```cpp
  // Sensor fusion lives in its own library (the only one that links GTSAM);
  // it is explicit, so registering it costs nothing until a plot asks for it.
  Fusion::registerFusionCalculations();
  ```
  (3) the plot rows below.

**Technical Approach:**

*Registration order.* After every built-in, including the interpolation family. No fusion output has another candidate, and `_FUSION_DIAGNOSTICS` cannot match the interpolation pattern (no `:`), so order is unobservable; plugins still come first. `src/main.cpp` and `registerBuiltInCalculations()` are not touched (`flysight_core` must not reference the library).

*Plots.* In `MainWindow::registerBuiltInPlots()`, one contiguous block after the `{"IMU", "Temperature", ...}` row and before `// Category: Magnetometer` (the branch's position), preceded by the comment `// Category: Sensor fusion (explicit: computed on request from the plot list; same fixed NED frame as "GNSS (Local frame)")`. Category string exactly `"Sensor fusion"`; sensor `"Fusion"`. Names, units, measurement types and order are the branch's (`branch:src/mainwindow.cpp` lines 992-1008), with `posN/posE/posD` renamed:

| # | Plot name | Units | Measurement | measurementType | Colour |
|---|---|---|---|---|---|
| 1 | North position | `m` | `north` | `distance` | `fromHsl(  0, S_dk, L_dw)` |
| 2 | East position | `m` | `east` | `distance` | `fromHsl(120, S_dk, L_dc)` |
| 3 | Down position | `m` | `down` | `distance` | `fromHsl(240, S_dk, L_db)` |
| 4 | North velocity | `m/s` | `velN` | `speed` | `fromHsl(  0, S, L_w)` |
| 5 | East velocity | `m/s` | `velE` | `speed` | `fromHsl(120, S, L_c)` |
| 6 | Down velocity | `m/s` | `velD` | `vertical_speed` | `fromHsl(240, S, L_b)` |
| 7 | North acceleration | `m/s^2` | `accN` | `acceleration` | `fromHsl(320, S, L_w)` |
| 8 | East acceleration | `m/s^2` | `accE` | `acceleration` | `fromHsl(  0, S, L_w)` |
| 9 | Down acceleration | `m/s^2` | `accD` | `acceleration` | `fromHsl( 40, S, L_w)` |
| 10 | Horizontal acceleration | `m/s^2` | `accH` | `acceleration` | `fromHsl( 20, S, L_w)` |
| 11 | Roll | `deg` | `roll` | `angle` | `fromHsl(  0, S_dk, L_dw)` |
| 12 | Pitch | `deg` | `pitch` | `angle` | `fromHsl(120, S_dk, L_dc)` |
| 13 | Yaw | `deg` | `yaw` | `angle` | `fromHsl(240, S_dk, L_db)` |
| 14 | Quaternion X | (empty) | `qx` | `ratio` | `fromHsl(  0, S, L_w)` |
| 15 | Quaternion Y | (empty) | `qy` | `ratio` | `fromHsl(120, S, L_c)` |
| 16 | Quaternion Z | (empty) | `qz` | `ratio` | `fromHsl(240, S, L_b)` |
| 17 | Quaternion W | (empty) | `qw` | `ratio` | `fromHsl(  0, 0, L_g)` |

The four acceleration colours are the branch's verbatim. The branch's `Qt::darkRed/darkGreen/darkBlue/darkGray` are expressed in the function's muted palette, exactly as Phase 2 did for "GNSS (Local frame)" and for the reason recorded in the palette's own comments.

*Deployment: what remains.* Nothing. `flysight_fusion` is static with `PRIVATE gtsam`, so CMake puts `$<LINK_ONLY:gtsam>` on the executable's link line (libraries only; no GTSAM include directory or definition reaches any application translation unit). The executable now imports the GTSAM / METIS / Cephes / TBB runtime, which Phase 1's `flysight_install_solver_runtime` already installs on all three platforms; the macOS rpath script and the Linux `$ORIGIN/../lib` RPATH already cover the executable. The application executable does **not** get `flysight_solver_stack`: fits run on the queue's 64 MiB worker. macOS and Linux are unverified until a push; a missing library there is a `Phase 1 fixup`.

*Must not be added:* any dialog, message box, `QProgressDialog`, `sensorFusionIsRunning`, idle-scheduler pause, or plot-widget guard.

**Acceptance Criteria:**
- [ ] The application builds and starts from its install directory on Windows; `CalculationRegistry::instance().contains("builtin.fusion.fit")` after start-up (check in a debugger or by the plot list behaviour once Phase 7 exists).
- [ ] The plot list shows a "Sensor fusion" category with the seventeen plots in the order above between "IMU" and "Magnetometer"; checking one draws nothing, shows no dialog, and blocks nothing.
- [ ] `git diff -- src/mainwindow.cpp` shows exactly three hunks (include, call, plot block); `git diff -- src/CMakeLists.txt` for this phase shows the `flysight_fusion` block edit of Task 9.1 and one added link line.
- [ ] No application translation unit's compile line contains a GTSAM include directory (inspect `FlySightViewer.vcxproj`).

**Complexity:** S

---

### Task 9.5: Explicit-backed logbook columns are not cached (resolves Phase 5's open question)

**Purpose:** Spec section 2 puts "persisting fusion results across application restarts" out of scope, and the column cache's own invariant says a cached value is the column's value for the session **as it is on disk**. A column such as "`Fusion/roll` at exit" must therefore never put a fused number into `index.json`: after a restart the loaded session reads unavailable until fusion is requested again, and a stub showing a number that vanishes when the row is loaded would be wrong.

**Files to modify:**
- `src/sessionmodel.cpp` — a file-local predicate and one check in `computeColumnValues()`.
- `src/sessionmodel.h` — extend the "CACHED COLUMN VALUES" paragraph of the class comment (three sentences).
- `tests/tst_column_cache.cpp` — one new test function (GTSAM-free).

**Technical Approach:**
- Anonymous-namespace function in `sessionmodel.cpp`:
  `bool dependsOnExplicitCalculation(const DependencyKey &name)` — true when any name in `CalculationRegistry::instance().staticDependencies(name).names` has a candidate (`candidatesFor(n)`, plus `sourceConversionsFor` for measurements, as `staticDependencies` itself walks them) whose `descriptor->policy == EvaluationPolicy::Explicit`. A pure function of the registrations; `staticDependencies` is memoized by the registry; no engine, no session, nothing runs. This is the same predicate as Phase 6's "explicit-backed" (06, 'Backed by an explicit calculation'), and the two must agree on the binding definition: **any name in the static dependency closure has a candidate with explicit policy, looking through source conversions.** Phase 10 Task 10.2 unifies the two if that is small (overview, Integration Note 10).
- In `computeColumnValues()`, at the top of the per-column loop: if any name of `columnNames(col)` `dependsOnExplicitCalculation`, leave `value` invalid and do not read the session for that column. Comment: "An explicit result is never persisted, so the value for the on-disk state is 'unavailable' whatever happens to be published in memory right now."
- Nothing else changes. Loaded rows still display the live value (they do not use the cache); `publishCalculationInvalidation()` still leaves cached columns alone (Phase 5) - which is now exactly right, because the cached value does not depend on publication. When a row is evicted its fusion column goes blank, which is what a reload would show.
- **No `CalculationCompatibilityVersion` bump.** The rule in `docs/CALCULATIONS.md` section 9 lists `computeColumnValues`, but the bump is for changes that "can alter the value that any existing session yields for any logbook column". Before this phase no explicit calculation was registered, so no existing column changes value. Registering the fusion calculations changes the environment fingerprint, which discards and lazily refills the cache once, as for any added registration.
- Test `explicitBackedColumnIsNeverCached` in `tst_column_cache.cpp`: register on the global registry (and unregister at the end of the function, before `cleanup()`'s registry comparison) a test-local explicit calculation `test.explicit` with input attribute `_DESCRIPTION` and output attribute `X_OUT = "computed"`; enable a `SessionAttribute` column on `X_OUT` next to the three standard ones; merge one session; `waitForIdle`. Assert the cached value of that column is present and invalid; `session.calculationEngine().request("test.explicit")` on the loaded row, then edit `_DESCRIPTION` through `updateAttribute` (which invalidates the column) and `waitForIdle`: the cached value is again present and invalid, `indexValue(sessionId, column)` on disk is not `"computed"`, while `model.data(index, Qt::DisplayRole)` of the loaded row shows the live value after a fresh request. Restore the column set as the other functions of that file do.

**Acceptance Criteria:**
- [ ] `explicitBackedColumnIsNeverCached` passes; every existing function of `tst_column_cache`, `tst_logbook_index`, `tst_session_model_engine`, `tst_workflow`, `tst_session_oracle` passes unchanged.
- [ ] With the fusion calculations registered, a `MeasurementAtMarker` column on `Fusion/roll` has a present, invalid cached value before and after a published fit (asserted in `tst_fusion_jobs::columnOnFusionOutputIsNotCached`, Task 9.8).
- [ ] `CalculationCompatibilityVersion` is unchanged by this phase.

**Complexity:** S

---

### Task 9.6: Test support: fixtures as sessions

**Purpose:** Turn Phase 8's deterministic fixtures into real `SessionData` objects whose effective inputs are bit-identical to the fixture, so session-level results can be compared with the committed goldens, plus one "natural" session that exercises the real input chain.

**Files to create:**
- `tests/fusion/fusionsessions.h` / `tests/fusion/fusionsessions.cpp` — namespace `FlySightTest`.

**Files to modify:**
- `tests/CMakeLists.txt` — inside `if(FLYSIGHT_BUILD_FUSION_TESTS)`, after Phase 8's two tests, one additive hunk:
  ```cmake
  # Fusion as a registered calculation: sessions, engine, job queue (tests/README.md)
  add_library(flysight_fusion_session_support STATIC
    fusion/fusionsessions.cpp fusion/fusionsessions.h)
  target_link_libraries(flysight_fusion_session_support
    PUBLIC flysight_fusion_test_support flysight_test_support)
  flysight_add_fusion_test(tst_fusion_session SOURCES tst_fusion_session.cpp
    LIBS flysight_fusion_session_support Threads::Threads)
  flysight_add_fusion_test(tst_fusion_jobs SOURCES tst_fusion_jobs.cpp
    LIBS flysight_fusion_session_support Threads::Threads)
  ```
  (`find_package(Threads)` comes from Phase 4.) Neither test names `gtsam`.

**Technical Approach:**

```cpp
/// Registers the fusion calculations on the global registry once per process
/// (after TestEnvironment::registerBuiltIns(), as the application does).
void registerFusionOnce();

/// Stored source data such that the 21 declared inputs read back bit-identical
/// to the fixture ("stored data always wins"):
///   GNSS/time (unit "s")            -> GNSS/_time through builtin.time.utc.GNSS
///   GNSS/hAcc, vAcc ("m"), sAcc ("m/s")
///   Local/north|east|down ("m"), Local/velN|velE|velD ("m/s")
///   IMU/_time ("s"), IMU/ax|ay|az ("m/s^2"), IMU/wx|wy|wz ("deg/s")
///   stored attributes _LOCAL_ORIGIN_INDEX (qlonglong), _LOCAL_ORIGIN_LAT|LON|HMSL (double),
///   _TIME_FIT_A = "1", _TIME_FIT_B = "1699999900", SESSION_ID = sessionId, DEVICE_ID = "fusion-test"
FlySight::SessionData sessionFromFixture(const FusionFixture &fixture, const QString &sessionId);

/// The same without any IMU column (acceptance 11).
FlySight::SessionData sessionWithoutImu(const FusionFixture &fixture, const QString &sessionId);

/// A recording as the importer would leave it: GNSS lat/lon/hMSL/velN/velE/velD/
/// hAcc/vAcc/sAcc/time, IMU time/ax../wz, TIME time/tow/week; nothing under Local,
/// no stored origin, no stored fit. Retyped from the branch's synthetic()
/// (branch:tests/fusion_session_regression.cpp): epoch 1700000000, 200 fixes at
/// .1 + i*.2 s with lat = lon = 0, hMSL = 100, zero velocity, hAcc = vAcc = 1,
/// sAcc = .1; IMU system time 100 + i*.01, i = 0..4000, az = -9.80665, the rest 0;
/// TIME pulses {100, 120, 140} with tow / tow+20 / tow+40.
FlySight::SessionData naturalSession(const QString &sessionId);

QStringList fusionMeasurementNames();   ///< the 17 literal names, in output order
```

- Use `setSourceMeasurement(sensor, name, samples, unit)`. The listed units are all identity rows of the unit table (`docs/DATA_SCHEMA.md` section 6), and the sessions carry no `SCHEMA_VER`-dependent column except the gyro: **give fixture sessions the stored attribute `SCHEMA_VER = 2`** so `IMU/wx|wy|wz` are read literally (without it the conversion layer multiplies them by 1.14688 and nothing matches the goldens). `naturalSession` does the same.
- `fusionsessions.cpp` must assert its own premise once per session in debug and the tests assert it explicitly (`inputsAreBitIdenticalToFixture`, Task 9.7).
- Only fixtures with equal-length columns per sensor may become sessions in a `SessionModel` (the saver refuses ragged sensors): `coarse_linear`, `coarse_maneuver`, `stationary_spin`, `reject_nonfinite`, `reject_time_order`, `reject_imu_gap`, `reject_gnss_gap`, `reject_sigma`, `reject_origin`. Do not use `reject_too_few_fixes`, `reject_coverage`, `reject_length` as sessions unless all columns of a sensor are truncated together (they are for the first two; `reject_length` is not usable).
- If `naturalSession` takes more than about 20 s to fit in Release on the development machine, lower its IMU rate to 25 Hz (`100 + i*.04`, `i = 0..1000`) and say so in the file comment; its expectations are structural (Task 9.7), not golden.

**Acceptance Criteria:**
- [ ] `fusionsessions.*` includes no GTSAM header and no internal `src/fusion/` header (only `fusion/fusionregistration.h`).
- [ ] With `-DFLYSIGHT_BUILD_FUSION_TESTS=OFF` none of the three new targets exists and every other test builds and passes.

**Complexity:** M

---

### Task 9.7: `tst_fusion_session` - the calculation on real sessions and engines

**Purpose:** Acceptance 5, 6, 7, 8 (second half), 9, 11, 12 and the cancellation boundary through the adapter, on real `SessionData` engines bound to the global registry, with the fit on the test's main thread (64 MiB through `flysight_add_fusion_test`).

**Files to create:** `tests/tst_fusion_session.cpp` — class `FusionSessionTest`, `FLYSIGHT_TEST_MAIN`, `#include "tst_fusion_session.moc"`. Fixture pattern of `tests/tst_session_engine.cpp` (`initTestCase`: `registerBuiltIns()` then `registerFusionOnce()`; `init`/`cleanup`: registry snapshot compared, `enrolledEngineCount() == 0`).

**Technical Approach:** Expected values are goldens (through `loadFusionGolden` / `compareSamples` / `compareJson`, in the mode chosen by `FLYSIGHT_FUSION_EXACT`) and literals; never a second call of the code under test, except where sync-versus-async is the rule being tested. "Reads of everything" below means: the 17 measurements, `Fusion/accH`, `Fusion/_system_time`, `_FUSION_DIAGNOSTICS`, and the interpolated logbook value `SessionData::interpolationKey(SessionKeys::ExitTime, "Fusion", SessionKeys::Time, "roll")` with a stored `_EXIT_TIME` inside the fit.

| Function | Asserts |
|---|---|
| `registrationShape` | ids present in order; title; 21 inputs in order; 18 outputs; policies; `candidatesFor(Fusion/accH)` is `builtin.fusion.accH` only |
| `inputsAreBitIdenticalToFixture` | for `coarse_maneuver`: each of the 17 effective input vectors `sameBits` with the fixture; the four attributes equal |
| `readsNeverRunTheFit` (acceptance 5) | on `coarse_linear`: reads of everything in a scrambled order, three rounds, interleaved with `blockers()` and `readiness()` calls: every value unavailable; `runCount("builtin.fusion.fit") == 0`; `totalRunCount()` does not change after the first round; `preparedCount() == 0`; `resultStatus` is absent or `NotRequested`; `verifyAgainstFresh` of the names is empty; `DataExporter::toBytes(session)` contains no `Fusion` text and runs nothing |
| `requestRunsOnceAndPublishesTogether` (acceptance 6; data: three success fixtures) | `request("builtin.fusion.fit")` returns `Ok`; its `invalidated` contains every name read while unrequested (incl. `accH`, `_system_time`, the interpolated attribute); all 17 outputs available, equal length, matching the golden channels; diagnostics match the golden by `compareJson`; `accH` per Task 9.3; `_system_time[i] == _time[i] - 1699999900.0`; the interpolated value is valid; `runCount == 1`; a second `request` returns the cached status with `runCount == 1`; 100 rounds of sibling reads run nothing; for `stationary_spin` `|yaw.last - yaw.first| > 360` and no adjacent step above 180 |
| `asyncMatchesSync` (acceptance 7; data: `coarse_linear`, `coarse_maneuver`, `reject_nonfinite`) | two sessions from one fixture: `request()` in one, `prepare()` + `computeOn(ComputeMode::Inline, ...)` + `publish()` in the other: every output `sameBits`, `_FUSION_DIAGNOSTICS` strings equal, `resultStatus`, `resultDetail`, `runCount`, `undeclaredReadCount() == 0` equal; `PublishOutcome::detail` equals the reason; `RecordingProgress` received at least "Starting fit" on success fixtures |
| `changeAfterPublicationDropsEverything` (acceptance 8, second half) | after a published fit and reads of everything: `setMeasurement("IMU", "az", <one sample changed>)` returns a set containing all 17 names, `_FUSION_DIAGNOSTICS`, `Fusion/accH`, `Fusion/_system_time` and the interpolated attribute; all read unavailable; `readiness() == Ready`; `runCount` still 1 until a new `request`, then 2 and a different `accD`. Editing `_EXIT_TIME`, `_ANALYSIS_START_TIME`, `_GROUND_ELEV` invalidates no `Fusion` measurement and leaves `runCount` unchanged (markers do not affect the fit) |
| `rejectionIsACachedResult` (acceptance 9; data: `reject_nonfinite`, `reject_imu_gap`, `reject_gnss_gap`, `reject_sigma`, `reject_origin`) | `request` returns `Ok`; 17 measurements, `accH`, `_system_time` unavailable; `_FUSION_DIAGNOSTICS` available, parses to exactly `{algorithm, failure}`, `failure` equals the golden's; `resultDetail() == failure`; `blockers(Fusion/roll)` and `blockers(Fusion/accH)` are `NotProduced` with `status Ok` and that detail; `blockers(_FUSION_DIAGNOSTICS)` is `Available`; second `request` runs nothing (`runCount == 1`); `prepare()` returns `AlreadyValid`. Then for `reject_sigma`: restoring `GNSS/sAcc` from `coarse_linear` makes `readiness() == Ready`, blockers `Blocked`, and a new request succeeds and matches the `coarse_linear` golden (`runCount == 2`) |
| `cancelStopsAtNextBoundary` (acceptance 10, adapter half; rows n = 1, 2, 3, 4 on `coarse_maneuver`) | a test-local `CalculationProgress` whose `isCancelled()` returns true from its n-th call: `compute()` returns `Kind::Cancelled`; exactly n texts were reported (no further boundary was reached); `publish()` returns `Discarded / Cancelled`; outputs unavailable; nothing cached; `readiness() == Ready`; a following uncancelled request matches the golden |
| `missingInputsAreNotApplicable` (acceptance 11; rows: `sessionWithoutImu`; `naturalSession` with every `hAcc = 10` (no origin); `naturalSession` without the TIME sensor (no `IMU/_time`)) | `readiness() == MissingInput` with no blockers; `prepare()` is `NothingToRun / MissingInput`; `request()` returns `MissingInput`; `blockers()` of all 17 plot names is `NotApplicable`; `runCount == 0` |
| `blockersReportFusion` (acceptance 12) | on an unrequested session `blockers()` of `Fusion/accH`, `Fusion/_system_time`, `Fusion/roll` and the interpolated attribute is `Blocked` with exactly one blocker: `registrationId "builtin.fusion.fit"`, empty `instanceOutput`, `title "Sensor fusion"`; `runCount == 0` afterwards; after publication all four are `Available` with empty lists |
| `naturalSessionEndToEnd` | `naturalSession`: prepare computes the on-demand inputs (`runCount("builtin.local.coordinates") == 1`, `runCount("builtin.time.fit") == 1`); the fit succeeds; `Fusion/_time` is a contiguous subsequence of `IMU/_time` (`sameBits`); all outputs aligned; diagnostics `input.origin_index == 0`, `gnss_states == 200`; `_system_time` within `1e-6` of the matching `IMU/time` samples; changing `TIME/tow` by +1 drops the result (the transitive input reaches it) |
| `twoSessionsAreIndependent` | two sessions requested one after the other share the descriptor; each result matches its own golden; invalidating one leaves the other published |

**Acceptance Criteria:**
- [ ] Every function exists and passes in Release on Windows in portable mode and with `FLYSIGHT_FUSION_EXACT=1` on the capture machine; total run time under 120 s (the CTest timeout for fusion tests is 600 s).
- [ ] The test includes `fusion/fusionregistration.h` and no other `src/fusion/` header; it links no `gtsam` directly.
- [ ] `cleanup()` verifies the registry is as `init()` found it.

**Complexity:** L

---

### Task 9.8: `tst_fusion_jobs` - the real fit through the job queue

**Purpose:** Acceptance 6, 7, 8 (first half), 9, 10, 11 through `JobQueue` on a real `SessionModel`, with the fit on the queue's 64 MiB worker; the column-cache rule on a real fusion column; the optional real-recording check.

**Files to create:** `tests/tst_fusion_jobs.cpp` — class `FusionJobsTest`. Fixture pattern of Phase 5's `tests/tst_jobqueue.cpp` (fresh logbook, `resetPreferencesToDefaults`, `LogbookManager::initialize()`, a `SessionModel`, a `JobQueue`; destroy queue, then model; registry snapshot). Sessions enter through `model.mergeSessions(QList<SessionData>)` and `waitForIdle(model)`.

**Technical Approach:**

*Determinism without a gate.* The real compute function cannot be held by a semaphore. Use Phase 5's ordering guarantee instead: progress posts are delivered on the main thread in order and **before** the `QThread::finished` event of the same job. A slot connected to `JobQueue::jobProgress` that acts on the *first* text of the job ("Starting fit") therefore runs while the job is still `Running` from the queue's point of view - even if the worker has already returned - and whatever it does (edit an input, cancel) is seen by `finishRun()`. No sleeps, no timing assumptions. Disconnect the slot after its first action.

| Function | Asserts |
|---|---|
| `jobPublishesAllOutputsTogether` (acceptance 6) | `coarse_maneuver`: reads of everything first; `request(id, "builtin.fusion.fit")` is `Created`; `waitIdle`; job `Succeeded`, empty reason, `resultStatus Ok`, `calculationTitle "Sensor fusion"`, non-empty `progressText`; a `dependencyChanged` spy holds the names read before, for that session only; outputs match the golden; `accH` and `_system_time` available unrequested; second request is `NothingToDo`; `runCount == 1` |
| `queueMatchesSynchronousRequest` (acceptance 7) | sessions `a` and `b` from `stationary_spin`: `a` through the queue, `b` through `calculationEngine().request()` on the main thread: all 17 outputs and the diagnostics `sameBits` / equal |
| `inputChangeDuringFitSupersedes` (acceptance 8, first half) | `coarse_maneuver` (origin index 3): on the first progress text call `model.updateAttribute(id, "_LOCAL_ORIGIN_INDEX", qlonglong(4))`; `waitIdle`; job `Superseded`, reason "Inputs changed"; outputs unavailable; `resultStatus` absent or `NotRequested`; `runCount == 0`; `readiness() == Ready`; a new request is `Created`, succeeds, and its diagnostics have `input.origin_index == 4` |
| `rejectedRecordingIsSucceededJob` (acceptance 9) | `reject_origin`: job `Succeeded`, `resultStatus Ok`, `reason == "Local origin index outside GNSS samples"` (also equal to the golden's `failure`); measurements unavailable; diagnostics available; re-request `NothingToDo`, `runCount == 1`; `updateAttribute(id, "_LOCAL_ORIGIN_INDEX", qlonglong(0))` then request: `Created`, `Succeeded`, outputs match the `coarse_linear` golden |
| `cancelDuringFitThenNextJobStarts` (acceptance 10) | jobs for `s1` (`stationary_spin`) and `s2` (`coarse_linear`); on `s1`'s first progress text `queue.cancel(job1)`; `waitIdle` (60 s): job1 `Cancelled` "Cancelled", nothing published (outputs unavailable, no `dependencyChanged` for `s1`'s outputs from a publication, `resultStatus` not set), `readiness(s1) == Ready`; job2 `Succeeded` with `startedAt >= job1.finishedAt`; re-request of `s1` succeeds and matches the golden. Log with `qInfo` the time from cancel to job end and job1's last progress text (evidence of a boundary stop; not asserted: see Gotchas) |
| `noImuSessionCannotHaveAJob` (acceptance 11) | `sessionWithoutImu`: `request` returns `MissingInput`, `job == 0`, `model()->rowCount() == 0`, `!isSessionPinned`; `blockers()` of all 17 plot names `NotApplicable` |
| `readersNeverStartAFit` (acceptance 5, model level) | a `MeasurementAtMarker` logbook column on `Fusion/roll` at `_EXIT_TIME` enabled; sessions merged; `waitForIdle(model)` (column worker, saver): the job model stays empty, `runCount("builtin.fusion.fit") == 0` on every loaded session, `model.data()` of the column is empty |
| `columnOnFusionOutputIsNotCached` | the same column: after a published fit and an `_EXIT_TIME` edit plus `waitForIdle`, the live cell shows a number, `rowAt(row).cachedValues` holds a present invalid value, and `indexValue(id, column)` on disk is not a number (Task 9.5) |
| `shutdownDuringFit` | `stationary_spin` running (first progress seen through `QSignalSpy::wait`), one more queued; `queue.shutdown()` returns; both `Cancelled` "Application closing"; nothing published; destroying queue then model does not crash |
| `realRecordingCheck` | optional, below |

*Optional real-recording check (not CI).* `realRecordingCheck` does `QSKIP` unless the environment variable `FLYSIGHT_FUSION_RECORDING` names a folder containing `TRACK.CSV` and `SENSOR.CSV`. It imports both with `DataImporter` (as `tst_workflow` does), merges the session, requests the fit through the queue, waits up to 30 minutes, requires `Succeeded`, aligned outputs and `Fusion/_time` ⊂ `IMU/_time`, and logs `objective`, `gnss_states`, `imu_outputs`, biases and elapsed time with `qInfo`. Run the executable directly (not under CTest: the 600 s timeout). Comparison with `branch:docs/PORT_VALIDATION.md` (recording 17-26-24: objective 65602.22485051976, 9247 GNSS states, 24411 outputs): the counts should match as they are; the **objective matches only if the gyro input is the branch's**, i.e. with a copy of `SENSOR.CSV` that has `$VAR,SCHEMA_VER,2` added after `$FLYS,1` (the documented escape hatch), because the branch predates the legacy gyro correction. With the unmodified legacy file a different objective is expected and correct. Document this in `tests/README.md`; assert nothing about those numbers in code.

**Acceptance Criteria:**
- [ ] All functions pass in Release on Windows, 20 consecutive runs, under 300 s per run; `realRecordingCheck` skips when the variable is unset.
- [ ] No `QTest::qSleep`, `QThread::sleep`, or `qWait(n)` used as a delay.
- [ ] `cleanup()` verifies the registry and `enrolledEngineCount() == 0`.

**Complexity:** L

---

### Task 9.9: Documentation and traceability

**Purpose:** Keep the name listings, test README and acceptance map in step, with small additive edits. (`docs/SENSOR_FUSION.md` and user documentation are Phase 10.)

**Files to modify:**
- `docs/CALCULATIONS.md` — (1) section 5, after the registration-order sentence, one sentence: "The application then registers sensor fusion from its own library (`Fusion::registerFusionCalculations`, `src/fusion/fusionregistration.cpp`); `flysight_core` does not know it." (2) section 8, one sentence: "`builtin.fusion.fit` (title "Sensor fusion") is the first explicit calculation." (3) append one new section after the last existing one, numbered 17 whichever of sections 12-16 (Phases 4-7) have landed yet (overview, Integration Note 2): **17. Sensor fusion as a registered calculation** - the three ids, the 21 inputs, the 18 outputs, the outcome table of this document, "no arithmetic in the adapter", how progress and cancellation reach the kernel, the derived values' definitions, and the rule of Task 9.5.
- `docs/DATA_SCHEMA.md` — (1) section 5, one sentence after the enumeration paragraph: the purely derived sensors `Local`, `Simplified` and `Fusion` never appear in enumeration or in saved files, and `Fusion` additionally reads unavailable until sensor fusion has been requested for the session. (2) section 11, one paragraph: a column that depends on an explicitly requested calculation is cached as unavailable, because such results are not saved; loaded rows show the live value.
- `tests/README.md` — executable count +2; two rows in the fusion table (Phase 8's); one paragraph "Fusion sessions" (`fusionsessions.h`, the stored-data trick, `SCHEMA_VER = 2`); the optional real-recording check with `FLYSIGHT_FUSION_RECORDING` and the gyro-scaling caveat.
- `tests/acceptance_map.txt` — append to the `# SFJ` comment block (Phase 4's convention; create the block header as Phase 4's document gives it if absent, which is Phase 2's form, 02 Task 2.7). The `# SFJ n` comment lines are converted to audited `100 + n` lines by Phase 10 (overview, Integration Note 1):
  ```
  # SFJ 5  tst_fusion_session readsNeverRunTheFit
  # SFJ 5  tst_fusion_jobs readersNeverStartAFit
  # SFJ 6  tst_fusion_session requestRunsOnceAndPublishesTogether
  # SFJ 6  tst_fusion_jobs jobPublishesAllOutputsTogether
  # SFJ 7  tst_fusion_session asyncMatchesSync
  # SFJ 7  tst_fusion_jobs queueMatchesSynchronousRequest
  # SFJ 8  tst_fusion_jobs inputChangeDuringFitSupersedes
  # SFJ 8  tst_fusion_session changeAfterPublicationDropsEverything
  # SFJ 9  tst_fusion_session rejectionIsACachedResult
  # SFJ 9  tst_fusion_jobs rejectedRecordingIsSucceededJob
  # SFJ 10 tst_fusion_session cancelStopsAtNextBoundary
  # SFJ 10 tst_fusion_jobs cancelDuringFitThenNextJobStarts
  # SFJ 11 tst_fusion_session missingInputsAreNotApplicable
  # SFJ 11 tst_fusion_jobs noImuSessionCannotHaveAJob
  # SFJ 12 tst_fusion_session blockersReportFusion
  ```

**Acceptance Criteria:**
- [ ] `audit_cleanup` passes.
- [ ] Edits to the four shared files are additive except the README count.

**Complexity:** S

## Testing Requirements

### Unit Tests
- New (label `fusion`): `tst_fusion_session`, `tst_fusion_jobs`; support `tests/fusion/fusionsessions.*`.
- New, GTSAM-free: `tst_column_cache::explicitBackedColumnIsNeverCached`.
- Unchanged and passing: everything else, in particular `tst_builtins_engine` (its inventory is of `registerBuiltInCalculations()` only, which this phase does not touch), `tst_fusion_parity`, `tst_fusion_kernel`, `tst_jobqueue`, `tst_jobmodel`, `tst_calcengine_async`.

### Integration Tests
- Full suite with `FLYSIGHT_BUILD_FUSION_TESTS=ON`, and configured `OFF`: both pass; with `OFF`, `ctest -LE fusion` equals the whole suite and no test target links GTSAM.
- `FLYSIGHT_FUSION_EXACT=1` with `-R tst_fusion` on the capture machine: session-level outputs are bit-identical to the goldens (the adapter adds no arithmetic).
- Build and install the application; it starts from the install directory with `PATH` reduced to the system directories.

### Manual Verification
1. `cmake --build build --config Release`, `ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure`. (`build` here stands for this phase's own build directory, configured as in Phase 8's Manual Verification step 1, i.e. with `-DGTSAM_INSTALL_DIR=<repo>/build-solver-deps/GTSAM-install -DONETBB_INSTALL_DIR=<repo>/build-solver-deps/oneTBB-install`: the Boost-free solver the goldens were captured against. The old Boost-enabled `third-party/GTSAM-install` is never used; overview, Integration Note 14.)
2. Start the application: "Sensor fusion" category with seventeen plots; first start refills logbook columns once (environment fingerprint changed).
3. **Acceptance 19** (needs Phase 7 in the tree; otherwise hand this step to Phase 10): with a TRACK+SENSOR recording visible, check "Roll"; while the job runs, pan and zoom other plots, hide and show a track, edit a description and drag a marker: the UI stays responsive, no dialog appears, the roll curve appears when the job ends; drag the exit marker afterwards: the curve stays (markers are not inputs).
4. Optional real-recording check (Task 9.8) with and without the `SCHEMA_VER,2` copy.
5. `git grep -n "QProgressDialog\|sensorFusionIsRunning\|QMessageBox" src/fusion src/jobqueue.cpp` finds nothing.

## Notes for Implementer

### Gotchas
- **Never run the real fit on `asyncdriver.h`'s `StdThread` / `QtThread` modes**: those threads have default stacks (1 MiB on Windows). Use `ComputeMode::Inline` (the test's main thread has 64 MiB) or the job queue's worker.
- Fixture sessions need `SCHEMA_VER = 2`, or the gyro channels are scaled by 1.14688 and nothing matches.
- The adapter must not "help": no size checks, no finiteness checks, no unit conversion, no epoch handling. Every such rule is in the kernel with the branch's order and messages, and is what the goldens test.
- A non-finite `Local/...` sample anywhere (an invalid GNSS fix) rejects the whole recording ("Nonfinite Local/north"). That is the branch's behaviour and is preserved (Phase 8 gotchas); it is a result with a reason, not a missing input.
- A missing origin (no fix under 10 m) is `MissingInput`, never a job and never a failure badge (Phase 8 decision; acceptance 11).
- `CalculationCancelled` must be thrown from the compute function itself, after `run()` has returned `Cancelled`; never let the kernel's `FusionCancelled` escape and never throw from the progress lambdas.
- `request()` on the synchronous path needs no open evaluation: call it only at top level in tests.
- `cancelDuringFitThenNextJobStarts` cannot assert *when* the kernel stopped: if the worker finished before the cancel was processed the job still ends Cancelled (Phase 5: cancel wins). The boundary stop itself is proven deterministically by `cancelStopsAtNextBoundary` and by Phase 8's `cancelAtEachKindOfBoundary`.
- `updateAttribute` compares old and new `QVariant`s; pass `qlonglong` values for `_LOCAL_ORIGIN_INDEX`.
- Registering the fusion calculations on the global registry in `initTestCase()` schedules an environment check in any live `SessionModel`; create models after registration.
- Shared files (`src/CMakeLists.txt`, `tests/CMakeLists.txt`, `src/mainwindow.cpp`, `src/sessiondata.h`, `docs/CALCULATIONS.md`, `docs/DATA_SCHEMA.md`, `tests/README.md`, `tests/acceptance_map.txt`): one insertion point each as named, no reflow. Report every touched path; the orchestrator commits.

### Decisions Made
- **Ids** `builtin.fusion.fit`, `builtin.fusion.accH`, `builtin.fusion.systemTime`; **entry point** `Fusion::registerFusionCalculations(registry)`, called in `MainWindow` directly after `registerBuiltInCalculations()` and in tests through `registerFusionOnce()` after `registerBuiltIns()`. `TestEnvironment` is not changed (it must not link GTSAM).
- **`flysight_fusion` links `flysight_model` only**, not `flysight_core`; helper headers from `src/calculations/` are header-only.
- **`SessionKeys::FusionDiagnostics` lives in `sessiondata.h`**, where `master` keeps every calculated attribute key (Phase 2 precedent).
- **Rejected and SolverFailed are published identically**: diagnostics attribute set, measurements unset, `setReason(reason)`. No engine change was needed for "attribute available, measurements unavailable".
- **No units reported on `Fusion` measurements**, like every derived measurement on `master` and like `Local`.
- **`Fusion/_system_time` is the inverse time fit of `Fusion/_time`** with the branch's non-finite guard, mirroring `builtin.time.system.GNSS`.
- **Plot colours**: the branch's acceleration colours verbatim; its dark named colours expressed in the muted palette, as Phase 2 did.
- **Phase 5's open question is resolved here** (Task 9.5): explicit-backed columns are cached as unavailable; the predicate is a registration-only function local to `sessionmodel.cpp`; no compatibility-marker bump.
- **`# SFJ n` comment lines** in the acceptance map, following Phases 4 and 5.
- **Solver failure is not session-tested**: `run()` has no public seam to force it; Phase 8 tests the outcome at kernel level and the adapter handles it in the same branch as `Rejected`.
- **`std::bad_alloc` is verified by inspection** (no `catch` in the adapter); the engine and queue halves are tested with synthetic calculations in Phases 4 and 5.
- **Deterministic mid-run actions use the first `jobProgress` delivery**, relying on Phase 5's documented event ordering, instead of a gate inside the compute function.

### Open Questions
- None blocking. For Phase 10: (a) Phase 6 defines the same "explicit-backed" predicate privately; consider moving one implementation into `CalculationRegistry` and using it from both. (b) Acceptance-map numbering (`# SFJ n` versus Phases 2/8's `100 + n`) is reconciled by Phase 10, which converts every `# SFJ n` comment into an audited `100 + n` line (overview, Integration Note 1). (c) Phase 8's purity grep (`sessiondata|engine/` in `src/fusion`) must exempt `fusionregistration.*` in the final audit. (d) Manual acceptance 19 and the end-to-end row test with the real fusion plots need Phase 7.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria
2. All tests pass with fusion tests on and off, plus `audit_cleanup`; `tst_fusion_session` and `tst_fusion_jobs` also pass with `FLYSIGHT_FUSION_EXACT=1` on the capture machine
3. Code follows patterns established in reference files (`localcoordinatecalculations.cpp`, `imucalculations.cpp`, `timecalculations.cpp`, `tst_session_engine.cpp`, `tst_jobqueue.cpp`)
4. No TODOs or placeholder code remains
5. `flysight_fusion` is still the only product target that names `gtsam` (the test-side targets `tst_solver_smoke`, `solver_deploy_probe` and `tst_fusion_kernel` also name it; other fusion tests get it through `flysight_fusion`; overview, Integration Note 6); `flysight_core`, `flysight_model` and every non-fusion test are GTSAM-free; nothing is named EKF, by the scoped check of Task 9.1 (overview, Integration Note 4), not by a bare `grep -rni "ekf" src tests`
6. None of the branch's modal dialog, re-entrancy guard, scheduler pause, rebuild guard, message box, or hand-cached failure exists
7. The completion report lists every created and modified path, shared files called out
