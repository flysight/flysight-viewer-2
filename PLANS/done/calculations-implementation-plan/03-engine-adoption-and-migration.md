# Phase 3: Engine adoption and built-in migration

## Overview

This phase makes the Phase 2 calculation engine the only thing that serves
calculated reads. `SessionData` implements `ISessionState`, owns a per-session
`CalculationEngine`, and resolves every `getAttribute` / `getMeasurement`
through it; all ~94 old per-value registrations in `src/calculations/*` become
67 registered calculations (multi-output groups, ordered per-sensor
candidates), synthesized interpolation becomes a `CalculationFamily`, altitude
markers register/unregister through the registry, the descent-pause preference
becomes a declared input, and the Python bridge is re-pointed at the registry
through minimal single-output adapters. `CalculatedValue`, `DependencyManager`,
and the direct cache setters are deleted.

**No data-model change happens here.** There is still no source/effective
split and no conversion layer (Phase 4): no source-conversion family is
registered, so a stored measurement resolves as a zero-copy passthrough of the
stored vector, `getUnit` still returns the stored unit text, and
`DataImporter` still converts `g` / `gauss` / `deg C` at import. At the end of
this phase every `// BASELINE:` pin in `tst_smoke` still passes unchanged and
every built-in output has the same numerical value as at `v2026.04.1`.

## Dependencies

- **Depends on:** Phase 2 (and Phase 1).
- **Blocks:** Phase 4 (and transitively 5-8).
- **Assumptions:**
  - Branch `schema-and-calculations`; `flysight_model` (Qt Core only, contains
    `src/engine/*`), `flysight_core`, the test harness, `FlySightTest::TestEnvironment`,
    `Fs2FileBuilder`, `FakeSessionState`, `FakePreferenceProvider` exist exactly
    as specified in `01-…` and `02-…`.
  - The engine API is used with Phase 2's names verbatim: `CalculationRegistry`
    (`registerCalculation`, `registerFamily`, `unregister`, `hasCandidateFor`,
    `candidatesFor`, `setPreferenceProvider`, `notifyPreferenceChanged`),
    `CalculationEngine` (`attribute`, `measurement`, `measurementUnit`,
    `attributeChanged`, `sourceMeasurementChanged`, `sourceUnitChanged`,
    `clear`, `rebind`, `setInvalidationListener`, `request`, `runCount`,
    `undeclaredReadCount`, `resultStatus`, `verifyAgainstFresh`),
    `CalculationDescriptor`, `CalculationFamily`, `CalcInput`,
    `CalculationResult`, `EvaluationContext`, `ISessionState`,
    `IPreferenceProvider`.
  - The process-wide `CalculationRegistry::instance()` is empty at the start of
    this phase (Phase 2 tests used private registries).
  - Line numbers: `src/sessiondata.h` and `src/mainwindow.cpp` are **baseline**
    (`git show v2026.04.1:<path>`); every other file cited is identical at the
    tag and on `master`.

## Migration strategy (read before the tasks)

The overview forbids the two engines from serving reads at the same time
across a task boundary. The order below therefore is:

1. **Tasks 3.1-3.2** - pin baseline values with the *old* engine; add the small
   engine/preferences additions this phase needs.
2. **Tasks 3.3-3.8** - write every new-style registration **side by side** with
   the old one. New registration functions are *overloads* taking a
   `CalculationRegistry &`; they are exercised only by tests, against
   `FakeSessionState` + a private registry. The application still calls the old
   no-argument overloads and the old engine serves every read. No dual path
   exists at run time: nothing enrols with the global registry because no
   `SessionData` owns an engine yet.
3. **Task 3.9 - the switch.** One task flips `SessionData` to the engine,
   deletes the old no-argument registration functions, and rewires startup.
   Before it the old engine serves all reads; after it the new one does.
4. **Tasks 3.10-3.12** - surface broadcasts in `SessionModel`, delete the old
   engine files, add the session-level test suite.

The tree compiles and all tests pass after each task.

### Calculation identity naming scheme

`CalculationId`s are dotted, lower-camel, never contain `'#'`:

| Prefix | Meaning |
|---|---|
| `builtin.attr.*` | `attributecalculations.cpp` |
| `builtin.gnss.<measurement>`, `builtin.imu.<m>`, `builtin.mag.<m>` | single-output measurements |
| `builtin.time.fit`, `builtin.time.utc.<SENSOR>`, `builtin.time.system.<SENSOR>` | `timecalculations.cpp` |
| `builtin.simplified.track` | simplified track |
| `builtin.wsp.*`, `builtin.sp.*` | WS-P / SP |
| `builtin.interpolation` | the interpolation **family** (instances `builtin.interpolation#<key>`) |
| `builtin.altitude.<attributeKey>` | e.g. `builtin.altitude._ALTITUDE_300_FT` |
| `plugin.attr.<index>.<name>`, `plugin.meas.<index>.<sensor>/<name>` | Python adapters; `<index>` = position in the SDK's `_attributes` / `_measurements` list |

### Inventory of calculations (authoritative for Tasks 3.3-3.7)

`A(x)` = `CalcInput::attribute(x)`, `M(s,n)` = `CalcInput::measurement(s,n)`,
`P(k)` = `CalcInput::preference(k)`. `_time` = `SessionKeys::Time`. Inputs are
listed in the order they must be declared (= the old dependency-list order, so
availability short-circuiting visits them in the same order). "Old lines"
refer to the file named in the section heading. **Row order = registration
order**; sections are registered in the order shown, which is the order of
`calculatedvalueregistry.cpp` 21-33.

**1. `attributecalculations.cpp`**

| Id | Inputs | Outputs | Old lines | Notes |
|---|---|---|---|---|
| `builtin.attr.analysisRange` | `M(GNSS,hMSL)`, `M(GNSS,_time)`, `P(import/descentPauseSeconds)` | `_ANALYSIS_START_TIME`, `_ANALYSIS_END_TIME` | 16-109 | Preference read at 18-19 was undeclared. Replaces two registrations + side-effect writes 81-82 + self-read 84. No descent found -> both unavailable. |
| `builtin.attr.exitTime` | `A(_ANALYSIS_START_TIME)`, `A(_ANALYSIS_END_TIME)`, `M(GNSS,velD)`, `M(GNSS,sAcc)`, `M(GNSS,accD)`, `M(GNSS,_time)` | `_EXIT_TIME` | 112-174 | |
| `builtin.attr.syncTime` | `A(_EXIT_TIME)` | `_SYNC_TIME` | 177-185 | passthrough |
| `builtin.attr.courseRef` | `A(_EXIT_TIME)` | `_COURSE_REF` | 188-196 | passthrough. Not merged with `syncTime`: unrelated outputs (spec 7.1). |
| `builtin.attr.manoeuvreStart` | `A(_EXIT_TIME)`, `A(_LANDING_TIME)`, `M(GNSS,velD)`, `M(GNSS,sAcc)`, `M(GNSS,_time)` | `_MANOEUVRE_START_TIME` | 200-258 | |
| `builtin.attr.flare` | `A(_EXIT_TIME)`, `A(_LANDING_TIME)`, `M(GNSS,hMSL)`, `M(GNSS,vAcc)`, `M(GNSS,_time)` | `_FLARE_START_TIME`, `_FLARE_END_TIME` | 263-370 | Replaces side effects 340-341 + self-read 343. |
| `builtin.attr.landingTime` | `A(_ANALYSIS_START_TIME)`, `A(_ANALYSIS_END_TIME)`, `A(_GROUND_ELEV)`, `M(GNSS,velD)`, `M(GNSS,velH)`, `M(GNSS,sAcc)`, `M(GNSS,hMSL)`, `M(GNSS,_time)` | `_LANDING_TIME` | 375-436 | |
| `builtin.attr.timeExtent.<S>` for `S` in `GNSS, BARO, HUM, MAG, IMU, TIME, VBAT` (this order) | `M(S,_time)` | `_START_TIME`, `_DURATION` | 439-480 | **Seven two-output candidates** (decision below). `_DURATION` alone is set unavailable when `max - min < 0`. |
| `builtin.attr.maxVelDTime` | `A(_MANOEUVRE_START_TIME)`, `A(_LANDING_TIME)`, `M(GNSS,velD)`, `M(GNSS,_time)` | `_MAX_VELD_TIME` | 483-523 | |
| `builtin.attr.maxVelHTime` | same with `M(GNSS,velH)` | `_MAX_VELH_TIME` | 526-566 | |
| `builtin.attr.groundElev` | `A(_ANALYSIS_END_TIME)`, `M(GNSS,hMSL)`, `M(GNSS,_time)` | `_GROUND_ELEV` | 572-607 | Fallback only; a stored `_GROUND_ELEV` wins by stored-first resolution. |

**2. `gnsscalculations.cpp`** - twenty single-output calculations
`builtin.gnss.<name>`, output `GNSS/<name>`, inputs exactly the old dependency
lists, in file order:

| `<name>` | Inputs | Old lines |
|---|---|---|
| `z` | `M(GNSS,hMSL)`, `A(_GROUND_ELEV)` | 15-42 |
| `velH` | `M velN, velE` | 45-71 |
| `vel` | `M velH, velD` | 74-100 |
| `accD` / `accN` / `accE` | `M velD|velN|velE`, `M(GNSS,time)` | 103-155 |
| `wcVel` | `M velN, velE, velD`, `A(_WIND_N)`, `A(_WIND_E)` | 158-194 |
| `course` | `M velN, velE`, `M(GNSS,_time)`, `A(_COURSE_REF)` | 197-258 |
| `courseRate` | `M course`, `M(GNSS,time)` | 261-276 |
| `glideRatio`, `diveAngle` | `M velH, velD` | 279-332 |
| `diveAngleRate` | `M diveAngle`, `M(GNSS,time)` | 335-350 |
| `accH` | `M accN, accE` | 355-378 |
| `wcVelH` | `M velN, velE`, `A(_WIND_N)`, `A(_WIND_E)` | 381-414 |
| `accAlongTrack`, `accCrossTrack` | `M accN, accE, accD, velN, velE, velD`, `A(_WIND_N)`, `A(_WIND_E)` | 417-536 |
| `lift`, `drag` | the six above + `M wcVel`, `M hMSL`, `A(_WIND_N)`, `A(_WIND_E)`, `A(_JUMPER_MASS)`, `A(_PLANFORM_AREA)` | 542-726 |
| `specificEnergy` | `M vel`, `M z` | 729-754 |
| `specificEnergyRate` | `M specificEnergy`, `M(GNSS,time)` | 757-772 |

Undeclared reads to remove: `session.getAttribute("_SESSION_ID")` inside
warnings at 61 and 90.

**3. `imucalculations.cpp`** - `builtin.imu.aTotal` (`M IMU ax, ay, az` ->
`IMU/aTotal`, 12-40), `builtin.imu.wTotal` (`M IMU wx, wy, wz` -> `IMU/wTotal`,
43-71). Undeclared `_SESSION_ID` reads at 30 and 61.
**4. `magcalculations.cpp`** - `builtin.mag.total` (`M MAG x, y, z` ->
`MAG/total`, 12-40). Undeclared `_SESSION_ID` read at 30.
`barocalculations.cpp`, `humcalculations.cpp`, `vbatcalculations.cpp` contain
no registrations.

**5. `timecalculations.cpp`**

| Id | Inputs | Outputs | Old lines | Notes |
|---|---|---|---|---|
| `builtin.time.fit` | `M(TIME,time)`, `M(TIME,tow)`, `M(TIME,week)` | `_TIME_FIT_A`, `_TIME_FIT_B` | 43-110 | Values stay **`QString`** (`QString::number(x,'g',17)`, 85-86). `hasMeasurement` checks 45-47 are dropped (see Gotchas). |
| `builtin.time.utc.GNSS` | `M(GNSS,time)` | `GNSS/_time` | 133-150 | passthrough (shares the buffer) |
| `builtin.time.utc.<S>`, `S` in `BARO, HUM, MAG, IMU, TIME, VBAT` | `M(S,time)`, `A(_TIME_FIT_A)`, `A(_TIME_FIT_B)` | `S/_time` | 115-130, 153-165 | `hasMeasurement` at 116 dropped; `systemTimeToUtc(session, ...)` at 123 replaced by a pure helper. |
| `builtin.time.system.GNSS` | `M(GNSS,time)`, `A(_TIME_FIT_A)`, `A(_TIME_FIT_B)` | `GNSS/_system_time` | 168-190 | unavailable when `a == 0` |
| `builtin.time.system.<S>`, same six sensors | `M(S,time)` | `S/_system_time` | 193-207 | passthrough |

Each `S/_time` is a *different* public name, so these are not competing
candidates; the only competing candidates among the built-ins are the seven
`timeExtent` calculations.

**6. `simplificationcalculations.cpp`** - `builtin.simplified.track`: inputs
`M(GNSS,lat)`, `M(GNSS,lon)`, `M(GNSS,hMSL)`, `M(GNSS,_time)`; outputs
`Simplified/lat`, `Simplified/lon`, `Simplified/hMSL`, `Simplified/_time`
(old 28-156; replaces four registrations + `setCalculatedMeasurement` 128-131).
Units are left empty.

**7. `wspcalculations.cpp`**

| Id | Inputs | Outputs | Old lines |
|---|---|---|---|
| `builtin.wsp.default.version` / `.topAlt` / `.bottomAlt` / `.task` | none | `_WSP_VERSION` = `"1.0"` / `_WSP_TOP_ALT` = `2500.0` / `_WSP_BOTTOM_ALT` = `1500.0` / `_WSP_TASK` = `"Time"` | 16-42 |
| `builtin.wsp.ref1Time` | `A(_EXIT_TIME)`, `M(GNSS,velD)`, `M(GNSS,_time)` | `_WSP_REF1_TIME` | 49-82 |
| `builtin.wsp.results` | `A(_WSP_TOP_ALT)`, `A(_WSP_BOTTOM_ALT)`, `A(_EXIT_TIME)`, `A(_GROUND_ELEV)`, `M(GNSS,z)`, `M(GNSS,lat)`, `M(GNSS,lon)`, `M(GNSS,_time)`, `M(GNSS,hAcc)`, `M(GNSS,vAcc)`, `A(_WSP_REF1_TIME)` | `_WSP_ENTRY_TIME`, `_WSP_EXIT_TIME`, `_WSP_ENTRY_LAT`, `_WSP_ENTRY_LON`, `_WSP_EXIT_LAT`, `_WSP_EXIT_LON`, `_WSP_TIME_RESULT`, `_WSP_DIST_RESULT`, `_WSP_SPEED_RESULT`, `_WSP_SEP_RESULT` | 86-350 |

`builtin.wsp.results` is a **partial-result** calculation: outputs are set as
they are found and everything not set is unavailable. The invalid-`QVariant`
"clear" writes (151-160, 191-197, 217, 259, 261) are simply *not setting* the
output. No entry crossing -> empty result; entry but no exit -> only the three
entry outputs; `timeResult <= 0` -> no speed; SEP only when `maxSep >= 0`.
All eleven inputs stay required, exactly as the old dependency list made them
(so the `ref1Var.canConvert` / size guards at 227 remain as defensive code).

**8. `spcalculations.cpp`**

| Id | Inputs | Outputs | Old lines |
|---|---|---|---|
| `builtin.sp.default.perfWindowHeight` / `.valWindowHeight` / `.breakoffAlt` | none | `7400.0/3.28084`, `3300.0/3.28084`, `5600.0/3.28084` (keep the expressions) | 17-38 |
| `builtin.sp.windowStart` | `A(_EXIT_TIME)`, `M(GNSS,velD)`, `M(GNSS,_time)`, `M(GNSS,z)` | `_SP_WINDOW_START_TIME`, `_SP_WINDOW_START_ALT` | 44-101 |
| `builtin.sp.results` | `A(_SP_PERF_WINDOW_HEIGHT)`, `A(_SP_VAL_WINDOW_HEIGHT)`, `A(_SP_BREAKOFF_ALT)`, `A(_EXIT_TIME)`, `A(_SP_WINDOW_START_TIME)`, `A(_SP_WINDOW_START_ALT)`, `M(GNSS,z)`, `M(GNSS,_time)`, `M(GNSS,vAcc)` | `_SP_WINDOW_END_TIME`, `_SP_BEST_START_TIME`, `_SP_BEST_END_TIME`, `_SP_SPEED_RESULT`, `_SP_MAX_SPEED_ACC` | 105-317 |

`builtin.sp.windowStart` is **one two-output calculation**; the self-reading
`_SP_WINDOW_START_ALT` recipe (89-101) and the side-effect writes (77-78, 83)
disappear. `builtin.sp.results` is partial like WS-P (`setAllNull` -> empty
result).

**9. Interpolation** - family `builtin.interpolation` (Task 3.7), registered
last among the built-ins.
**10. Altitude markers** - `builtin.altitude.<key>`: inputs
`A(_ANALYSIS_START_TIME)`, `A(_ANALYSIS_END_TIME)`, `M(GNSS,z)`,
`M(GNSS,_time)`; output `<key>` (`altitudemarkerfeature.cpp` 70-120).
Registered at run time after the built-ins.

Totals: 17 + 20 + 2 + 1 + 15 + 1 + 6 + 5 = **67** plain calculations + 1 family
(+ N altitude markers), replacing 94 old registrations.

The declared-input graph is acyclic (`_GROUND_ELEV` <- analysis end;
`_LANDING_TIME` <- analysis range + `_GROUND_ELEV`; `GNSS/z` <- `_GROUND_ELEV`;
nothing feeds back). The old engine's re-entrancy came only from helpers
reading their own key; that is gone. **No built-in may rely on cycle fallback.**

---

## Tasks

### Task 3.1: Golden characterization suite against the old engine

**Purpose:** Capture baseline values of every built-in output on a generated fixture *before* anything changes, so Tasks 3.3-3.9 can prove numerical equivalence.

**Files to create:**
- `tests/support/builtinfixture.h` / `.cpp` - fixture generator, golden table, helpers.
- `tests/tst_builtins_golden.cpp` (class `BuiltinsGoldenTest`).

**Files to modify:**
- `tests/CMakeLists.txt` - add the support files to `flysight_test_support`; `flysight_add_test(tst_builtins_golden SOURCES tst_builtins_golden.cpp)`.

**Technical Approach:**

In `namespace FlySightTest`:

```cpp
namespace DescentFixture {
    constexpr double T0 = 1704110400.0;                 // 2024-01-01T12:00:00Z
    Fs2FileBuilder trackFile(const QByteArray &sessionId = "descent");
    Fs2FileBuilder sensorFile(const QByteArray &sessionId = "descent");
    // Imports both files from a fresh temp dir with DataImporter and merges the sensor
    // session into the track session using only public API (setAttribute for keys the
    // target lacks, setMeasurement + setUnit for every measurement).
    FlySight::SessionData load();
}
struct GoldenValue { FlySight::DependencyKey name; bool available; QVariant attribute;
                     QVector<double> firstSamples; int sampleCount; double tolerance; };
QList<GoldenValue> goldenValues();                      // literals only
void copyStoredState(const FlySight::SessionData &from, FakeSessionState &to);  // attributeKeys/sensorKeys/measurementKeys/getUnit
```

**Track file** (`$VAR` `FIRMWARE_VER=v2023.09.22`, `SESSION_ID`, `DEVICE_ID=test-device`;
`GNSS` columns `time,lat,lon,hMSL,velN,velE,velD,hAcc,vAcc,sAcc,numSV`, units
`,deg,deg,m,m/s,m/s,m/s,m,m,m,`), 296 rows at 1 Hz, row `i` = 0..295, generated
with integer arithmetic and written as text:

- `time` = ISO `Z` text of `T0 + i` (`2024-01-01T12:00:00.000Z` + i s).
- `velD[i]`: 0 for i <= 9; `5*(i-9)` for 10..19; 50 for 20..73; 5 for 74..265
  except **-2 for 200..204**; 0 for 266..295.
- `hMSL[0..9] = 4000`, then `hMSL[i] = hMSL[i-1] - velD[i]` (gives 3725 at 19,
  1025 at 73, 100 at 265 and thereafter).
- `velN[i]` = 50 for i <= 73, 10 for 74..265, 1 for 266..295; `velE` = 0.
- `lat[i]` = `45 + i*0.0001` written with 4 decimals; `lon` = `-75`.
- `hAcc = 1.0`, `vAcc = 1.0`, `sAcc = 0.5`, `numSV = 12`.

**Sensor file** (same `$VAR`s), three rows per sensor at system time 10, 20, 30:
`TIME` (`time,tow,week`; units `s,s,`): tow 129610 / 129620 / 129630, week 2295
(so UTC = T0+10, +20, +30 and every sum in the fit is an exactly representable
integer); `IMU` (`time,wx,wy,wz,ax,ay,az,temperature`; `s,deg/s,deg/s,deg/s,g,g,g,deg C`):
wx 3/6/9, wy 4/8/12, wz 0, ax 0, ay 0, az 1, temperature 40; `MAG`
(`time,x,y,z,temperature`; gauss): x 3, y 4, z 0; `BARO` (`time,pressure,temperature`;
`s,Pa,deg C`): 90000, 20; `HUM` (`time,humidity,temperature`; `s,percent,deg C`):
50, 20; `VBAT` (`time,voltage`; `s,V`): 4.0. No `SCHEMA_VER`.

**Golden values** (hand-derived from the algorithms; with `ImportDescentPauseSeconds` = 30.0,
`ImportGroundReferenceMode` = "Automatic"). Attribute tolerances 1e-6 unless noted:

| Name | Expected |
|---|---|
| `_ANALYSIS_START_TIME` / `_ANALYSIS_END_TIME` | T0 / T0+295 |
| `_GROUND_ELEV` | 100.0 |
| `_EXIT_TIME`, `_SYNC_TIME`, `_COURSE_REF` | T0+9 (crossing at i=11, a=1, accD=5 -> `t11 - 10/5`) |
| `_LANDING_TIME` | T0+266 |
| `_MANOEUVRE_START_TIME` | T0+9 |
| `_FLARE_START_TIME` / `_FLARE_END_TIME` | T0+199 / T0+204 |
| `_MAX_VELD_TIME` / `_MAX_VELH_TIME` | T0+19 / T0+9 |
| `_START_TIME` / `_DURATION` | T0 / 295.0 (GNSS candidate) |
| `_TIME_FIT_A` / `_TIME_FIT_B` | `QString("1")` / `QString("1704110400")` (exact string compare) |
| `_WSP_VERSION`, `_WSP_TOP_ALT`, `_WSP_BOTTOM_ALT`, `_WSP_TASK` | "1.0", 2500.0, 1500.0, "Time" |
| `_WSP_REF1_TIME` | T0+20 |
| `_WSP_ENTRY_TIME` / `_WSP_EXIT_TIME` / `_WSP_TIME_RESULT` | T0+41.5 / T0+61.5 / 20.0 |
| `_WSP_ENTRY_LAT`, `_WSP_EXIT_LAT`, `_WSP_ENTRY_LON`, `_WSP_EXIT_LON` | 45.00415, 45.00615, -75, -75 (1e-9) |
| `_WSP_DIST_RESULT`, `_WSP_SPEED_RESULT` | **capture** (approximately 222.3 m and 11.1 m/s; assert the captured literal to 1e-6 and the approximation to 0.5) |
| `_WSP_SEP_RESULT` | 1.5381 |
| `_SP_PERF_WINDOW_HEIGHT`, `_SP_VAL_WINDOW_HEIGHT`, `_SP_BREAKOFF_ALT` | 2255.5199..., 1005.8399..., 1706.8799... (write as `7400.0/3.28084` etc. evaluated by hand to 17 digits, tolerance 1e-9) |
| `_SP_WINDOW_START_TIME` / `_SP_WINDOW_START_ALT` | T0+11 / 3885.0 |
| `_SP_WINDOW_END_TIME` | T0+57.3624 (tolerance 1e-3; capture the exact literal) |
| `_SP_BEST_START_TIME` / `_SP_BEST_END_TIME` / `_SP_SPEED_RESULT` | T0+18 / T0+21 / 50.0 |
| `_SP_MAX_SPEED_ACC` | 0.47140452079103168 |
| `_EXIT_TIME:GNSS/_time/hMSL` | 4000.0 |
| `_WSP_ENTRY_TIME:GNSS/_time/hMSL` | 2600.0 |
| `_NOPE:GNSS/_time/hMSL`, `_EXIT_TIME:GNSS/_time/nope`, `_EXIT_TIME:GNSS/hMSL` | unavailable |
| `GNSS/_time` | 296 samples; [0]=T0, [295]=T0+295 |
| `GNSS/z` | [0]=3900, [295]=0 |
| `GNSS/velH` [0]=50; `GNSS/vel` [19]=70.710678118654755; `GNSS/accD` [11]=5.0, [0]=0.0 | |
| `GNSS/course` | all 0.0; `GNSS/glideRatio`[19]=1.0; `GNSS/diveAngle`[19]=45.0 |
| `GNSS/wcVel`[19] = 70.710678118654755, `GNSS/wcVelH`[0] = 50.0 (wind 0) | |
| `GNSS/accN, accE, accH, courseRate, diveAngleRate, accAlongTrack, accCrossTrack, lift, drag, specificEnergy, specificEnergyRate` | **capture** sample [19] and the sample count (296) |
| `GNSS/_system_time` | `(utc - b) / a` with a = 1, b = T0: [0] = 0.0; [295] = 295.0 |
| `IMU/_time` (and `BARO`, `HUM`, `MAG`, `TIME`, `VBAT`) | {T0+10, T0+20, T0+30}; `IMU/_system_time` = {10, 20, 30} |
| `IMU/wTotal` | {5, 10, 15}  `// BASELINE: no gyro correction - Phase 4 scales by 1.14688` |
| `IMU/aTotal` | {9.80665 x3}; `MAG/total` = {0.0005 x3} (1e-15) |
| `Simplified/lat` | 2 samples {45.0, 45.0295}; `Simplified/_time` = {T0, T0+295}; `Simplified/hMSL` = {4000, 100}; `Simplified/lon` = {-75, -75} |

"**capture**" means: run the test once in this task with a temporary
`qDebug() << QString::number(v, 'g', 17)`, paste the printed literal into
`goldenValues()`, remove the print. This is the one sanctioned place where a
literal originates from the code under test, because the purpose is
characterization of the *old* engine. Every other row is asserted against the
hand-derived literal above; if a hand-derived value disagrees with the old
engine, stop and resolve the discrepancy (fix the table, not the code) before
continuing.

`tst_builtins_golden` in this task has one data-driven function
`sessionDataMatchesGolden` (`initTestCase` calls
`TestEnvironment::instance().registerBuiltIns()`): for each golden row read
through `SessionData::getAttribute` / `getMeasurement` and compare. Also
`storedEnumerationUnaffected`: after reading everything,
`hasMeasurement("GNSS","z")`, `hasAttribute("_EXIT_TIME")`, and
`hasMeasurement("Simplified","lat")` are false and `sensorKeys()` does not
contain `Simplified` (spec 4).

**Acceptance Criteria:**
- [ ] `tst_builtins_golden` passes against the **unmodified old engine** before Task 3.2 starts (record the passing run in the implementation summary; the orchestrator commits the phase as a whole, so do not commit it yourself).
- [ ] Every one of the 67 calculations' outputs appears in `goldenValues()` at least once (for `timeExtent` and `time.utc/system`, at least the GNSS and IMU instances).
- [ ] No expectation is computed at test time; `git grep -n "getAttribute\|getMeasurement" tests/support/builtinfixture.cpp` shows no use inside `goldenValues()`.
- [ ] `tst_smoke` and all Phase 2 tests still pass.

**Complexity:** M

---

### Task 3.2: Engine and preference additions needed by this phase

**Purpose:** Add the three small capabilities Phase 2 did not provide: enumeration of registrations, a safe "is this preference registered" query, and the `PreferencesManager` -> `IPreferenceProvider` adapter.

**Files to modify:**
- `src/engine/calculationregistry.h` / `.cpp` - add `QList<CalculationId> registeredIds() const;` (plain calculations, families, and conversion families, in sequence order) and `bool isFamily(const CalculationId &id) const;`.
- `src/preferences/preferencesmanager.h` - add `bool hasPreference(const QString &key) const { return m_preferences.contains(key); }`.
- `src/CMakeLists.txt` - add the two new files to `flysight_core`.
- `tests/tst_calcregistry.cpp` - one test for `registeredIds()` order after register / unregister / re-register, and `isFamily`.
- `tests/tst_harness.cpp` - `preferenceProviderAdapter` test (below).

**Files to create:**
- `src/preferences/enginepreferenceprovider.h` / `.cpp` (in `flysight_core`).

**Technical Approach:**

```cpp
class EnginePreferenceProvider : public QObject, public IPreferenceProvider {
    Q_OBJECT
public:
    // Idempotent. Sets the provider on the registry and connects
    // PreferencesManager::preferenceChanged -> registry.notifyPreferenceChanged(key).
    static void install(CalculationRegistry &registry = CalculationRegistry::instance());
    QVariant preferenceValue(const QString &key) const override;
};
```

- `preferenceValue`: `if (!prefs.hasPreference(key)) return QVariant(); return prefs.getValue(key);`
  - never call `getValue` on an unregistered key (it `Q_ASSERT`s, `preferencesmanager.h` 33-36).
- The singleton adapter object is a function-local static inside `install`;
  the connection is direct (same thread), so invalidation happens
  synchronously inside `PreferencesManager::setValue`, before it returns.
- `install()` for a non-global registry (tests) creates a second adapter
  owned by a static list; acceptable because registries in tests are few. Tests
  that use a private registry normally use `FakePreferenceProvider` instead.
- Not wired into `MainWindow` yet (Task 3.9). Wiring earlier would be harmless
  but pointless: no engine is enrolled.

`preferenceProviderAdapter` test: `install(localRegistry)`; a synthetic
calculation with input `P(import/descentPauseSeconds)` on a
`FakeSessionState` returns the pref value; `PreferencesManager::setValue(key, 5.0)`
delivers `{output}` to the engine's listener and the next read returns 5.0;
`preferenceValue("no/such/key")` is invalid and triggers no assert.

**Acceptance Criteria:**
- [ ] `registeredIds()` returns ids in sequence order; an id re-registered after `unregister` moves to the end.
- [ ] `EnginePreferenceProvider` lives in `flysight_core`; `flysight_model` still links Qt Core only and includes no `preferences/*` header.
- [ ] The adapter test passes; no other behavior changes; the application builds.

**Complexity:** S

---

### Task 3.3: Migrate time calculations (side by side)

**Purpose:** Provide new-style registrations for the time fit, `_time`, and `_system_time`, verified against the golden values on a fake session.

**Files to create:**
- `src/calculations/builtincalculations.h` / `.cpp` - the new entry point (replaces `calculatedvalueregistry.*` in Task 3.9).
- `tests/tst_builtins_engine.cpp` (class `BuiltinsEngineTest`).

**Files to modify:**
- `src/calculations/timecalculations.h` / `.cpp` - add overload `void registerTimeCalculations(CalculationRegistry &registry);` plus pure helpers.
- `src/CMakeLists.txt`, `tests/CMakeLists.txt`.

**Technical Approach:**

Entry point (in `flysight_core`):

```cpp
namespace FlySight {
// Registers every built-in calculation in the fixed order of the inventory. Engine registrations only.
void registerBuiltInCalculations(CalculationRegistry &registry = CalculationRegistry::instance());
// WS-P / SP marker groups and AttributeRegistry entries (UI metadata). Call once per process.
void registerBuiltInCalculationMetadata();          // added in Task 3.6
}
```

In this task `registerBuiltInCalculations` calls only
`Calculations::registerTimeCalculations(registry)`; Tasks 3.4-3.7 add calls **in
the final inventory order** (attribute, gnss, imu, mag, time, simplification,
wsp, sp, interpolation), not in the order the tasks are done.

Pattern for every migrated calculation (follow it in Tasks 3.4-3.7):

```cpp
CalculationDescriptor d;
d.id      = QStringLiteral("builtin.time.fit");
d.inputs  = { CalcInput::measurement("TIME","time"), CalcInput::measurement("TIME","tow"), CalcInput::measurement("TIME","week") };
d.outputs = { DependencyKey::attribute(SessionKeys::TimeFitA), DependencyKey::attribute(SessionKeys::TimeFitB) };
d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
    const auto fit = computeTimeFit(ctx.measurement("TIME","time"), ctx.measurement("TIME","tow"), ctx.measurement("TIME","week"));
    if (!fit) return CalculationResult::unavailable();
    return CalculationResult().setAttribute(SessionKeys::TimeFitA, QString::number(fit->a,'g',17))
                              .setAttribute(SessionKeys::TimeFitB, QString::number(fit->b,'g',17));
};
const bool ok = registry.registerCalculation(d);  Q_ASSERT(ok);
```

- Extract the numerical body into a **pure kernel** in an anonymous namespace
  or a `static` function - `std::optional<TimeFit> computeTimeFit(const QVector<double>&, const QVector<double>&, const QVector<double>&)`
  (old 55-83 verbatim) - and make the *old* lambda call the same kernel, so one
  copy of the arithmetic serves both engines until Task 3.9. Do the same for
  every multi-output / non-trivial calculation in later tasks; trivial
  single-output bodies may simply be duplicated (the old copy is deleted in 3.9).
- `_time` for non-GNSS sensors: read `a = ctx.attribute(TimeFitA).toDouble()`,
  `b = ...` once, then `result[i] = a * t[i] + b` - the same expression as
  `systemTimeToUtc` (19). `_system_time` for GNSS: `a == 0.0` -> unavailable,
  else `(utc - b) / a` (34).
- Keep the public helpers `systemTimeToUtc` / `utcToSystemTime(const SessionData&, double)`:
  they are UI helpers (`plotutils.cpp` 27/178, `measuretool.cpp` 35,
  `MapCursorDotModel.cpp` 339-340, `TrackMapModel.cpp` 125-126, `PlotWidget.cpp`
  1882) called *outside* any compute function. They must never be called from a
  compute function.
- A compute function never takes `SessionData`, never includes
  `preferencesmanager.h`, and reads only through `ctx`.

`tst_builtins_engine` (grows through Task 3.7): `init()` builds a private
`CalculationRegistry`, calls `registerBuiltInCalculations(registry)`, a
`FakePreferenceProvider` with `import/descentPauseSeconds = 30.0` set on the
registry, a `FakeSessionState` filled by `copyStoredState(DescentFixture::load(), fake)`,
and a `CalculationEngine`. Functions:
- `goldenOnEngine` - for each golden row with `registry.hasCandidateFor(name)`, compare `engine.attribute/measurement` to the literal.
- `noUndeclaredReads` - for each `registry.registeredIds()` that is not a family, `engine.request(id)`; assert `undeclaredReadCount() == 0`, `cycleCount() == 0`, and every `resultStatus(id)` is `Ok` or `MissingInput`.
- `inventory` - `registeredIds()` equals a literal `QStringList` (extended each task).

**Acceptance Criteria:**
- [ ] 15 ids registered by this task, in the inventory order; `inventory` pins them.
- [ ] `goldenOnEngine` covers `_TIME_FIT_A/B`, `*/_time`, `*/_system_time` and passes; `_TIME_FIT_A` is a `QString` equal to `"1"`.
- [ ] `runCount("builtin.time.fit") == 1` after reading `_TIME_FIT_B`, `_TIME_FIT_A`, `IMU/_time`, `MAG/_time`.
- [ ] `tst_builtins_golden` (old engine) still passes - the kernel extraction changed no value.
- [ ] The new overload contains no `SessionData` parameter and no `hasMeasurement` call.

**Complexity:** M

---

### Task 3.4: Migrate attribute calculations (side by side)

**Purpose:** New-style registrations for section 1 of the inventory, including the declared preference input.

**Files to modify:**
- `src/calculations/attributecalculations.h` / `.cpp` - add `void registerAttributeCalculations(CalculationRegistry &registry);`.
- `src/calculations/builtincalculations.cpp`, `tests/tst_builtins_engine.cpp`.

**Technical Approach:**

- Pure kernels (shared with the old lambdas until 3.9):
  `std::optional<std::pair<double,double>> computeAnalysisRange(const QVector<double> &hMSL, const QVector<double> &time, double timeout)` (old 24-87) and
  `std::optional<std::pair<double,double>> computeFlare(exitSec, landingSec, hMSL, vAcc, time)` (old 278-341).
  The old analysis-range lambda keeps reading `PreferencesManager` and passes
  the value into the kernel; the new compute passes
  `ctx.preference(PreferenceKeys::ImportDescentPauseSeconds).toDouble()`.
  Include `preferences/preferencekeys.h` for the key only.
- `builtin.attr.timeExtent.<S>`: one scan producing both outputs:
  `min`, `max` via `std::min_element` / `std::max_element` as today;
  `_START_TIME = min`; `_DURATION = max - min`, or `setUnavailable` when negative.
  Register in the loop order of `all_sensors` (439). Keep the `qWarning`s.
- All `canConvert<double>()` guards stay. Warnings stay but must not read
  anything from the session.
- The `_GROUND_ELEV` fallback stays an ordinary single candidate; user / import
  "Fixed" values are stored attributes and win by stored-first resolution.

Tests added to `tst_builtins_engine`: golden coverage of section 1;
`analysisRangeFollowsPreference` on a small fake: `GNSS/time` =
{0,10,20,30,40,50,60,70}, `GNSS/hMSL` = {1000,900,800,810,820,830,700,600} ->
with pref 30.0: start 0.0, end 70.0; after
`FakePreferenceProvider::set(registry, key, 5.0)`: listener receives a set
containing both outputs, start 45.0, end 70.0,
`runCount("builtin.attr.analysisRange") == 2`; `_GROUND_ELEV` = 600.0 in both.
`startTimeCandidateOrder`: with only `BARO/_time`-producing data the winner is
`builtin.attr.timeExtent.BARO` and `resultStatus("builtin.attr.timeExtent.GNSS") == MissingInput`.

**Acceptance Criteria:**
- [ ] 17 ids added in inventory order (7 `timeExtent` in sensor order).
- [ ] `git grep -n "PreferencesManager" -- src/calculations/attributecalculations.cpp` matches only inside the old no-argument overload (deleted in 3.9).
- [ ] The preference test passes with the literals above; with the provider's value unset, both outputs are unavailable and `resultStatus == MissingInput`.
- [ ] Old-engine golden test still passes.

**Complexity:** L

---

### Task 3.5: Migrate GNSS, IMU, and MAG measurements (side by side)

**Purpose:** Twenty-three single-output measurement calculations with the undeclared `_SESSION_ID` reads removed.

**Files to modify:**
- `src/calculations/gnsscalculations.*`, `imucalculations.*`, `magcalculations.*` - add `register…Calculations(CalculationRegistry &registry)` overloads.
- `src/calculations/builtincalculations.cpp`, `tests/tst_builtins_engine.cpp`.

**Technical Approach:**

- Mechanical translation: `session.getMeasurement(s, n)` -> `ctx.measurement(s, n)`,
  `session.getAttribute(k)` -> `ctx.attribute(k)`, `return std::nullopt` ->
  `return CalculationResult::unavailable()`, `return vec` ->
  `CalculationResult().setMeasurement("GNSS", name, vec)`.
  `computeDerivative` returns `std::optional`; map `nullopt` to unavailable.
- Delete `<< session.getAttribute("_SESSION_ID")` from the five size-mismatch
  warnings. **Do not declare `_SESSION_ID` as an input**: all declared inputs
  are required, the key does not exist (the real key is `SESSION_ID`), and the
  calculation would never run.
- Units: pass no unit (empty). `getUnit` for derived measurements is empty at
  baseline and stays so; Phase 4 owns effective units.
- The `if (!ok) windN = 0.0` fallbacks remain as written; they are reachable
  only for a stored non-numeric wind value, as today.
- To limit duplication a small file-local helper is allowed, e.g.
  `registerGnss(registry, name, inputs, fn)` where
  `fn(const EvaluationContext&) -> std::optional<QVector<double>>`.

**Acceptance Criteria:**
- [ ] 23 ids added in inventory order; golden coverage of every `GNSS/*`, `IMU/aTotal`, `IMU/wTotal`, `MAG/total` row passes.
- [ ] `git grep -n "_SESSION_ID" -- src/calculations` returns nothing in the new overloads (and nothing at all after 3.9).
- [ ] `noUndeclaredReads` passes with all 55 ids registered so far.

**Complexity:** M

---

### Task 3.6: Migrate simplified track, WS-P, and SP (side by side); split UI metadata

**Purpose:** The remaining multi-output groups, with partial results replacing "clear" writes, and the marker/attribute-registry side work separated from engine registration.

**Files to modify:**
- `src/calculations/simplificationcalculations.*`, `wspcalculations.*`, `spcalculations.*`.
- `src/calculations/builtincalculations.cpp`, `tests/tst_builtins_engine.cpp`.

**Technical Approach:**

- Kernels: `SimplifiedTrack simplifyTrack(lat, lon, hMSL, time)` (old 35-122,
  returning four vectors); `WspResults computeWspResults(...)` with
  `std::optional<double>` members for the ten outputs; `SpWindowStart` /
  `SpResults` likewise. The new compute copies each engaged optional into the
  `CalculationResult`; disengaged ones are left unset. The old lambdas are
  re-pointed at the same kernels (they translate a disengaged optional into
  the old invalid-`QVariant` write) so the old-engine golden test still guards
  the refactor.
- Move Groups C and D of `wspcalculations.cpp` (352-442) and
  `spcalculations.cpp` (319-394) verbatim into
  `Calculations::registerWspMetadata()` / `registerSpMetadata()`, called by the
  old no-argument functions for now and by
  `registerBuiltInCalculationMetadata()` (WS-P then SP). The new
  `register…(CalculationRegistry&)` overloads must not touch `MarkerRegistry`
  or `AttributeRegistry`, so they can run on private registries any number of
  times.
- Constant defaults: compute ignores `ctx`; no inputs.

Tests added: golden coverage; `wspPartial` - fake with the descent fixture but
stored `_WSP_BOTTOM_ALT = -50.0` (never crossed): `_WSP_ENTRY_TIME` = T0+41.5
available, `_WSP_EXIT_TIME`, `_WSP_TIME_RESULT`, `_WSP_DIST_RESULT`,
`_WSP_SPEED_RESULT`, `_WSP_SEP_RESULT` unavailable,
`resultStatus("builtin.wsp.results") == Ok`, `runCount == 1` across all ten
reads repeated twice. `spWindowStartIsOneRun` - reading `_SP_WINDOW_START_ALT`
then `_SP_WINDOW_START_TIME` gives `runCount("builtin.sp.windowStart") == 1`,
`cycleCount() == 0`. `simplifiedRunsOnce` - four outputs in any order, one run.

**Acceptance Criteria:**
- [ ] 12 ids added in inventory order (total 67); `inventory` pins the full list.
- [ ] No `QVariant()` "clear" value is ever passed to `CalculationResult::setAttribute` in these files.
- [ ] The three tests above and full golden coverage pass on the fake; old-engine golden still passes; marker groups `wsp` / `sp` and the seven attribute-registry entries are unchanged in the running application.

**Complexity:** L

---

### Task 3.7: Interpolation family and altitude-marker descriptor factory (side by side)

**Purpose:** Express synthesized interpolation as a parameterized calculation and prepare altitude markers for registry-based registration.

**Files to create:**
- `src/calculations/interpolationcalculations.h` / `.cpp` (`flysight_core`).

**Files to modify:**
- `src/altitudemarkerfeature.h` / `.cpp` - add `static CalculationDescriptor makeDescriptor(const QString &attributeKey, double thresholdMetres);` and `static CalculationId calculationId(const QString &attributeKey);`.
- `src/calculations/builtincalculations.cpp`, `src/CMakeLists.txt`, `tests/tst_builtins_engine.cpp`.

**Technical Approach:**

`Calculations::registerInterpolationFamily(CalculationRegistry &registry)`
registers `CalculationFamily{ id = "builtin.interpolation" }` whose
`instantiate(name)`:
1. returns `nullopt` unless `name.type == Attribute`;
2. parses `name.attributeKey` **exactly** as `sessiondata.cpp` 172-183: first
   `':'`; the remainder split on `'/'` must give exactly three parts; otherwise
   `nullopt`. Empty parts are allowed to instantiate (they are then simply
   unavailable), matching today;
3. returns a descriptor with `id` (= instanceKey) = the full key, inputs
   `A(timeAttr)`, `M(sensor,timeVector)`, `M(sensor,dataVector)`, one output
   (the name), and a compute that is `sessiondata.cpp` 186-216 verbatim
   (`canConvert<double>`, size check, `lower_bound`, the `cbegin`/`cend`
   rejection, `t2 == t1` guard).

The syntax and `SessionData::interpolationKey` (234-246) are unchanged, so
`sessionmodel.cpp` 235-249 / 1458-1466 / 1497-1504, `PlotWidget.cpp` 1235 and
`pluginhost.cpp` 403-410 need no edits. The family is registered **last** by
`registerBuiltInCalculations`, mirroring today's "recipes first, then
synthesis" fall-through (`sessiondata.cpp` 162-168); since no other
calculation outputs a name containing `':'`, order is immaterial in practice.
Each distinct key is one instance (`builtin.interpolation#<key>`) with its own
result and edges; an instance that cannot interpolate is **negatively cached**
(the old code re-parsed and re-searched on every read).

`AltitudeMarkerManager::makeDescriptor`: id
`"builtin.altitude." + attributeKey`, inventory section 10, compute = old
78-120 with the threshold captured.

**Decision - thresholds are baked at registration, not declared preference inputs.**
The attribute key encodes the value and unit (`_ALTITUDE_300_FT`), so the
threshold is part of the calculation's *identity*, not an input: a given id
always computes the same function, which keeps it pure. What preferences change
is *which* calculations exist, and that is handled by register/unregister
broadcasts. The altitude list also lives in a raw `QSettings` array
(`altitudemarkerfeature.cpp` 43-50), not in a `PreferencesManager` key, so it
could not be a declared preference input without new preference plumbing.

Tests added: golden rows for interpolation; `interpolationInstances` -
`runCountForInstance("builtin.interpolation#_EXIT_TIME:GNSS/_time/hMSL") == 1`
after three reads; storing `_EXIT_TIME = T0+41.5` returns a set containing that
key and the next read is 2600.0; a malformed key (`"a:b/c"`) has
`hasCandidateFor == false`. `altitudeDescriptor` - register
`makeDescriptor("_ALTITUDE_1000_M", 1000.0)` on the private registry: value
T0+71.5; with stored `_GROUND_ELEV = 0.0`: T0+78.0.

**Acceptance Criteria:**
- [ ] `registeredIds()` ends with `builtin.interpolation`, and `isFamily` is true for it.
- [ ] The three interpolation golden "unavailable" rows are unavailable and cached (`cachedState == Unavailable`), with no repeated state reads on re-read.
- [ ] `AltitudeMarkerManager::registerAll()` / `refresh()` behavior is unchanged in this task (still old engine).

**Complexity:** M

---

### Task 3.8: Python bridge - minimal single-output adapters

**Purpose:** Re-point `PluginHost` at `CalculationRegistry` and remove the direct cache setter, without doing Phase 7's work.

**Files to create:**
- `src/pluginsessionview.h` - header-only, Qt Core + `engine/evaluationcontext.h` only.

**Files to modify:**
- `src/pluginhost.cpp` - sections 4 and 5 (277-364).
- `src/sessiondata_bindings.cpp` - bind `PluginSessionView` instead of `SessionData`; delete the `setCalculatedMeasurement` binding (13-47).
- `src/CMakeLists.txt` - list `pluginsessionview.h` under `flysight_cpp_bridge`.
- `python_plugins/flysight_plugin_sdk.py` - no `setCalculatedMeasurement` reference exists today; add one sentence to the `AttributePlugin` / `MeasurementPlugin` docstrings: "`compute()` may read only the keys returned by `inputs()`; any other read makes the result unavailable."

**Technical Approach:**

Why a view object: under the new engine `compute` runs inside an evaluation,
where public `SessionData` reads are forbidden (Phase 2, Task 2.4 "Public
reads") and inputs are already resolved in the `EvaluationContext`. Also,
`flysight_model` is a static library linked into **both** the executable and
the `.pyd`, so the `.pyd` has its own copy of every static, including
`CalculationRegistry::instance()`; bridge code must therefore never construct a
`SessionData`/engine or reach the registry. A view over the context needs
neither.

```cpp
class PluginSessionView {                       // Python type name stays "SessionData"
public:
    explicit PluginSessionView(const EvaluationContext *ctx);
    void invalidate();                          // called by the host right after compute() returns
    QVector<double> getMeasurement(const QString &sensor, const QString &name) const;  // ctx->measurement, empty if invalidated
    QVariant        getAttribute(const QString &key) const;                            // ctx->attribute
    bool hasMeasurement(const QString &sensor, const QString &name) const;             // !getMeasurement(...).isEmpty()
    bool hasAttribute(const QString &key) const;                                       // getAttribute(key).isValid()
};
```

Bind it with `py::class_<PluginSessionView, std::shared_ptr<PluginSessionView>>(m, "SessionData")`
keeping the four Python method names and the existing return conversions
(`sessiondata_bindings.cpp` 50-101). The host creates
`std::make_shared<PluginSessionView>(&ctx)`, passes it to `compute`, and calls
`invalidate()` afterwards (also on the exception path - use a small scope
guard), so a plugin that stashes the object gets an inert view, never a
dangling pointer.

Registration: for plugin `i` of `_attributes` build a `CalculationDescriptor`
with id `plugin.attr.<i>.<name>`, `inputs` from the existing `.kind` decoding
(283-295; `Attribute` -> `CalcInput::attribute`, else `CalcInput::measurement`),
one output, and a compute that acquires the GIL, calls `compute(view)`, and
converts the return value exactly as today (304-323) into
`CalculationResult().setAttribute(name, value)` (`None` / unsupported type ->
`unavailable()`). Measurements likewise (`plugin.meas.<i>.<sensor>/<name>`,
358-362; the existing copy into a `QVector` already detaches from the NumPy
buffer). If `registerCalculation` returns false (duplicate id, `'#'` in a name,
output listed among its own inputs) log a `qWarning` naming the plugin and
continue.

Registration goes to `CalculationRegistry::instance()` from the executable.
`PluginHost::initialise` still runs before the built-ins (Task 3.9 keeps the
order), so plugin candidates still precede built-in candidates for the same
output. Between this task and 3.9 the old engine serves reads and plugin
calculations are not reachable; this is acceptable because **no plugin is
registered in the shipped tree** (`python_plugins/` contains only the SDK, and
its example classes at 208-260 are never registered).

**Deliberately left for Phase 7** (do not start them here): an explicit
`try/catch` with Python traceback logging in the adapter (today a Python
exception propagates as `py::error_already_set`, a `std::exception`, which the
engine's catch in `ensureResult` turns into status `Failed` - safe, but
untested until Phase 7); NumPy `ndim`/dtype validation and lifetime tests; the
multi-output plugin form; source access from Python; explicit rejection of
unknown dependency kinds; exposing `CalcInput` (preference inputs) to Python;
SDK/README documentation; the embedded-Python test target.

**Acceptance Criteria:**
- [ ] `git grep -n "setCalculatedMeasurement" -- src/sessiondata_bindings.cpp src/pluginhost.cpp python_plugins` returns nothing.
- [ ] `git grep -n "registerCalculatedAttribute\|registerCalculatedMeasurement\|sessiondata.h" -- src/pluginhost.cpp src/sessiondata_bindings.cpp` returns nothing.
- [ ] `flysight_cpp_bridge` links only `flysight_model` + Qt Core (unchanged) and builds; the application starts, imports the bridge and the SDK, and logs "Registered 0 attributes, 0 measurements".
- [ ] Manual check (not committed): a scratch plugin registering `DefaultDuration("GNSS")` under a new name (e.g. `_PY_DURATION`) is registered as `plugin.attr.0._PY_DURATION`; after Task 3.9 it returns 295.0 on the descent fixture, and a variant reading an undeclared measurement returns unavailable with one "undeclared read" warning.

**Complexity:** M

---

### Task 3.9: The switch - `SessionData` adopts the engine

**Purpose:** Make the new engine the only path for calculated reads and delete the old registration code, in one atomic step.

**Files to modify:**
- `src/sessiondata.h` / `.cpp` - rewrite as below.
- `src/calculations/*calculations.{h,cpp}` - delete every old no-argument `register…Calculations()` overload and the now-unused old lambdas; delete `barocalculations.*`, `humcalculations.*`, `vbatcalculations.*` (empty placeholders) and `calculatedvalueregistry.*`.
- `src/mainwindow.cpp` (baseline 48, 136-165, 586) - startup wiring.
- `src/altitudemarkerfeature.cpp` / `.h` - registry-based register / refresh.
- `src/dataimporter.cpp` - one call (below).
- `src/ui/docks/plot/PlotWidget.cpp` 1602 - registry query.
- `src/CMakeLists.txt` - source lists.
- `tests/support/testenvironment.cpp` - `registerBuiltIns()`.
- `tests/tst_builtins_golden.cpp` - unchanged logic; it now exercises the new engine through `SessionData`.

**Technical Approach:**

**`SessionData` after this phase** (public surface; `SessionKeys` unchanged):

```cpp
class SessionData : public ISessionState {
public:
    SessionData() = default;
    SessionData(const SessionData &other);                 // copies state only; NO engine, no cache, no listener
    SessionData(SessionData &&other) noexcept;             // steals state and engine; engine->rebind(this)
    SessionData &operator=(const SessionData &other);      // copies state; keeps own engine + listener, engine->clear()
    SessionData &operator=(SessionData &&other) noexcept;  // steals state and engine (own engine destroyed); rebind(this)
    ~SessionData() override;

    bool isVisible() const;  void setVisible(bool);
    QStringList attributeKeys() const;  bool hasAttribute(const QString&) const;          // stored only (spec 4)
    QVariant getAttribute(const QString &key) const;                                       // engine: stored-then-calculated
    QSet<DependencyKey> setAttribute(const QString &key, const QVariant &value);
    QSet<DependencyKey> removeAttribute(const QString &key);
    QStringList sensorKeys() const;  bool hasSensor(const QString&) const;
    QStringList measurementKeys(const QString&) const;  bool hasMeasurement(const QString&, const QString&) const;
    QVector<double> getMeasurement(const QString &sensor, const QString &name) const;      // engine: stored-then-calculated
    QSet<DependencyKey> setMeasurement(const QString &sensor, const QString &name, const QVector<double> &data);
    QSet<DependencyKey> setUnit(const QString &sensor, const QString &name, const QString &unit);   // was void
    QString getUnit(const QString &sensor, const QString &name) const;                     // stored unit text, as today
    QMap<QString,QString> units(const QString &sensor) const;
    QSet<DependencyKey> invalidateAllCalculations();       // engine->clear(); for code that wrote m_sensors directly
    CalculationEngine &calculationEngine() const;          // creates the engine if needed; for SessionModel and tests
    static QString interpolationKey(...);                  // unchanged

    // ISessionState - pure reads of m_attributes / m_sensors / m_units
    bool hasStoredAttribute(const QString&) const override;      QVariant storedAttribute(const QString&) const override;
    bool hasSourceMeasurement(const QString&, const QString&) const override;
    QVector<double> sourceMeasurement(const QString&, const QString&) const override;
    QString sourceUnit(const QString&, const QString&) const override;
private:
    bool m_visible = false;
    QMap<QString,QVariant> m_attributes;
    QMap<QString,QMap<QString,QVector<double>>> m_sensors;
    QMap<QString,QMap<QString,QString>> m_units;
    mutable std::unique_ptr<CalculationEngine> m_engine;   // lazily created, bound to CalculationRegistry::instance()
    friend class DataImporter;
};
```

Removed: `AttributeFunction`, `MeasurementFunction`, `MeasurementKey`,
`unregisterCalculatedAttribute`, `setCalculatedAttribute`,
`setCalculatedMeasurement`, `hasRegisteredCalculation`,
`registerCalculatedAttribute`, `registerCalculatedMeasurement`,
`addDependencies`, `m_calculatedAttributes`, `m_calculatedMeasurements`,
`m_dependencyManager`, `computeAttribute`, `synthesizeInterpolation`,
`computeMeasurement`, and the includes of `calculatedvalue.h` /
`dependencymanager.h` (baseline h 11, 13, 89-91, 106, 118-131, 138-144, 153).

Rules:
- **One read path.** `getAttribute(key)` = `calculationEngine().attribute(key)`;
  `getMeasurement` = `calculationEngine().measurement(...)`. Do not short-cut
  stored values in `SessionData`; the engine already resolves stored-first and
  returns the implicitly shared stored vector (no copy). With **no source
  conversion family registered in this phase**, a stored measurement is a
  passthrough and `getUnit` keeps reading `m_units` directly.
- **Mutations** change the map, then - only if `m_engine` exists - call
  `attributeChanged` / `sourceMeasurementChanged` / `sourceUnitChanged` and
  return its set. With no engine there is no cache: return the set containing
  just the changed name (the contract `SessionModel::updateAttribute` relies
  on). `setUnit` with an unchanged unit string still notifies (keep it simple).
- **Copy = state only.** `SessionData` is copied in `QList<SessionData>` during
  import (baseline `mainwindow.cpp` 573-587, 638-640), into
  `std::optional<SessionData>` rows (`sessionmodel.cpp` 546, 559), and by
  `LogbookManager::loadSession`'s return. A copy gets a cold engine on first
  calculated read. **Move carries the engine** (and with it the cache and the
  invalidation listener) and must call `rebind(this)`; this is what keeps a
  row's cache alive through `QVector<SessionRow>` growth and `std::sort`
  (`sessionmodel.cpp` sort). Both move operations are `noexcept`. A moved-from
  object has no engine and lazily gets a new one.
- Do **not** mark `SessionData` or `SessionRow` `Q_RELOCATABLE_TYPE`: the engine
  holds a pointer back to its `SessionData`.
- `DataImporter` writes `m_sensors` directly (baseline 148, 179, 219, 361). Add
  `sessionData.invalidateAllCalculations();` immediately before every `return`
  of `DataImporter::readFile` (baseline 29-83). With a fresh target session it
  is a no-op. Phase 4 removes the friendship.
- Baseline `mainwindow.cpp` 586 (`tempSessionData.getAttribute(GroundElev)` to
  "force" the calculation) is deleted: the value was only ever cached, never
  stored, and a copy no longer carries caches, so it has no effect.

**Startup** (baseline `mainwindow.cpp` 136-165) - order preserved, per the
overview decision that Python plugins precede built-ins:

```
registerBuiltInPlots(); registerBuiltInMarkers(); registerBuiltInAttributes();
... PluginHost::instance().initialise(pluginDir);        // plugin.* registered first
initializePreferences();
EnginePreferenceProvider::install();                     // NEW - after preferences are registered
registerBuiltInCalculations();                           // was CalculatedValueRegistry::instance()...
registerBuiltInCalculationMetadata();                    // WS-P / SP markers + attribute entries (same point as before)
m_altitudeMarkerManager = new AltitudeMarkerManager(model, this);  m_altitudeMarkerManager->registerAll();
```

All of this still happens before `LogbookManager::initialize()`, i.e. before
any session exists, so startup registration causes no broadcasts.

**Altitude markers** (`altitudemarkerfeature.cpp`): `registerAll()` and
`refresh()` share one routine that computes the desired key set from
preferences, then **diffs** against `m_registeredKeys`: for keys no longer
wanted `CalculationRegistry::instance().unregister(calculationId(key))`; for
new keys `registerCalculation(makeDescriptor(key, thresholdMetres))`; unchanged
keys are left alone (a colour-only change touches no registration and
invalidates nothing). Marker definitions, colour preference writes, and the
`state/markers/` cleanup (162-170) are unchanged. Remove
`#include "calculatedvalue.h"`. Each unregister/register broadcasts to every
enrolled engine, which is the fix for the stale-cache bug (old 153-156 flushed
no session).

**`hasRegisteredCalculation` consumer:** `PlotWidget.cpp` 1602 becomes
`CalculationRegistry::instance().hasCandidateFor(DependencyKey::attribute(meta.attributeKey))`.
`WingsuitPerformanceWidget.cpp` 416-417 and `SpeedSkydivingWidget.cpp` 330-332
only call `SessionModel::removeAttribute` and rely on the constant-default
calculations existing; no edit.

**`TestEnvironment::registerBuiltIns()`**: `registerBuiltInAttributes()`,
`EnginePreferenceProvider::install()`, `registerBuiltInCalculations()`,
`registerBuiltInCalculationMetadata()` - still behind the process-wide guard.

**CMake:** `flysight_core` loses `calculations/calculatedvalueregistry.*`,
`barocalculations.*`, `humcalculations.*`, `vbatcalculations.*`; gains nothing
new here. `flysight_model` still lists `calculatedvalue.*` /
`dependencymanager.*` until Task 3.11 (they compile standalone).

**Behavior notes to keep in mind (all value-neutral):**
- Unavailable results are now negatively cached; previously `std::nullopt`
  results were recomputed on every read (and their `qWarning`s repeated).
- A name whose calculated value is unavailable no longer falls through to a
  parse attempt as an interpolation key unless it actually has that syntax.

**Acceptance Criteria:**
- [ ] `tst_builtins_golden` passes unchanged (same literals) through `SessionData` on the new engine; `tst_smoke` passes with every `// BASELINE:` expectation unchanged; all Phase 2 tests and `tst_builtins_engine` pass.
- [ ] `git grep -nE "registerCalculated(Attribute|Measurement)|setCalculated(Attribute|Measurement)|hasRegisteredCalculation|unregisterCalculatedAttribute|addDependencies|CalculatedValueRegistry" -- src tests python_plugins` returns nothing.
- [ ] `git grep -nE "SessionData ?&" -- src/calculations` matches only the `systemTimeToUtc` / `utcToSystemTime` helper signatures; `git grep -n "PreferencesManager\|_SESSION_ID" -- src/calculations` returns nothing.
- [ ] A copied `SessionData` whose original had warm caches returns equal values and the original's `totalRunCount()` is unchanged by reads on the copy; a moved `SessionData` keeps `cachedState(name) == Available` and serves reads without new runs.
- [ ] The application builds and runs: import, plots, markers, logbook columns, WS-P / SP docks, and "Reset to default" on an editable marker bubble behave as before; with altitude markers enabled and a session plotted, adding/removing an altitude in preferences adds/removes the marker after the next UI refresh and logs no warning.

**Complexity:** L (split naturally into: `SessionData` rewrite; deletion of old overloads + CMake; startup + altitude manager + consumer; test-support update)

---

### Task 3.10: Surface broadcast invalidation in `SessionModel`; stop discarding merge invalidation

**Purpose:** Registry and preference changes invalidate loaded sessions through each engine's `InvalidationListener`; that must reach the UI as `dependencyChanged` + `dataChanged` and refresh cached logbook columns. Merges must emit the invalidation they already compute.

**Files to modify:**
- `src/sessionmodel.h` / `.cpp`.

**Technical Approach:**

Add to `SessionModel`:

```cpp
public:
    void flushPendingInvalidations();                     // synchronous delivery; used by tests and on shutdown
private:
    void attachSession(SessionRow &sr);                   // installs the engine listener for a loaded row
    void queueInvalidation(const QString &sessionId, const QSet<DependencyKey> &keys);
    void publishInvalidation(int row, const QSet<DependencyKey> &keys);   // dataChanged(row) + dependencyChanged per key
    QHash<QString, QSet<DependencyKey>> m_pendingInvalidations;
    bool m_invalidationFlushQueued = false;
```

- `attachSession(sr)`: `sr.session->calculationEngine().setInvalidationListener([this, id = sr.sessionId](const QSet<DependencyKey> &keys){ queueInvalidation(id, keys); });`
  Call it at **every** site that gives a row a session: `mergeSessions`
  (after 546 and after the row is appended at 565 - attach on the element in
  `m_rows`, not on the local `newRow`) and `sessionRef` (after 827/831, i.e.
  after the UUID -> `SESSION_ID` remap at 821-826 so the captured id is final).
  The listener captures the model and the id - never a row or session address,
  because rows move. Temporarily loaded sessions (`processNextDirtyColumn`
  1249-1262, the bulk-edit stub path ~1398-1421) are not attached.
- `queueInvalidation` unites the keys into `m_pendingInvalidations[id]` and,
  if not already queued, posts `flushPendingInvalidations` with
  `QMetaObject::invokeMethod(this, ..., Qt::QueuedConnection)`.
  **Why coalesce:** `AltitudeMarkerManager` issues one registry change per
  altitude and the registry has no batch API (Phase 2 decision), so a refresh
  with N altitudes and M loaded sessions would otherwise emit N x M bursts, each
  making every dock re-read. Deferring is safe: invalidation has already
  happened synchronously, so any read in between returns the new value; the
  signals only tell views to re-read.
- `flushPendingInvalidations`: swap the map out; for each id find the row
  (`getSessionRow`); skip if missing or not loaded (evicted/removed meanwhile);
  otherwise `publishInvalidation(row, keys)`, then **refresh cached columns**:
  `m_rows[row].cachedValues.clear();` and, after the loop, `startColumnWorker()`
  once. The existing idle `ColumnTask` recomputes from the in-memory session,
  writes `LogbookManager::setCachedValues`, and flushes the index on completion
  (ctor 101-112, `processNextDirtyColumn` 1229-1286). The session is **not**
  marked dirty and no save is scheduled: persistent state did not change.
  Finally `emit modelChanged()` once if anything was published.
- Refactor `setData` (489-493), `updateAttribute` (961-975), and
  `removeAttribute` (999-1013) to call `publishInvalidation` for their
  per-key emission so there is a single emitter. Their behavior (synchronous
  emission, `scheduleSave`) is unchanged; direct edits do not go through the
  queue because the engine returns their sets to the caller instead of the
  listener.
- `mergeSessions` (501-583): in the loaded-match branch unite the sets returned
  by `setAttribute` / `setMeasurement` (527-536) into a local
  `QHash<QString, QSet<DependencyKey>>`; after `endResetModel()` emit
  `dependencyChanged` for each. For the stub-replaced and new-row branches call
  `attachSession` only. **Nothing else about merge semantics changes here**
  (attribute overwrite, stub replacement, units not merged are Phase 6).
- `MainWindow` shutdown: call `model->flushPendingInvalidations()` before
  `flushDirtySessions()` only if convenient; pending signals at shutdown are
  harmless and may simply be dropped.

Known limitation, deliberately not addressed here: **unloaded** rows keep
`index.json` column values computed under the old preference value /
registration set (at baseline nothing was ever refreshed on a preference
change). Recorded under Open Questions for Phase 5's column-cache marker.

**Acceptance Criteria:**
- [ ] With two loaded rows, `PreferencesManager::setValue(ImportDescentPauseSeconds, 5.0)` followed by `flushPendingInvalidations()` (or one event-loop pass) emits `dependencyChanged(id, _ANALYSIS_START_TIME)` for each row whose analysis range had been read, emits `dataChanged` spanning that row, clears the row's `cachedValues`, and does not set `dirty`.
- [ ] Changing `AeroMass` or `ImportFixedElevation` emits no `dependencyChanged` at all.
- [ ] N registry changes inside one event-loop turn produce one `publishInvalidation` per session (spy count), containing the union of keys.
- [ ] A listener firing for a session that was evicted before the flush is ignored without warnings or crashes.
- [ ] `mergeSessions` into a loaded row emits `dependencyChanged` for the merged measurement names and their resolved dependents.
- [ ] `git grep -n "emit dependencyChanged" -- src/sessionmodel.cpp` shows the emission only inside `publishInvalidation` and the merge path.

**Complexity:** M

---

### Task 3.11: Delete the old engine

**Purpose:** "Remove the old mechanisms rather than leaving them alongside the new ones" (spec 12, acceptance 19).

**Files to delete:** `src/calculatedvalue.h`, `src/calculatedvalue.cpp`, `src/dependencymanager.h`, `src/dependencymanager.cpp`.

**Files to modify:**
- `src/CMakeLists.txt` - remove the four files from `flysight_model`; `flysight_model` = `sessiondata.*`, `dependencykey.h`, `engine/*`.
- `src/dependencykey.h` - delete the two dead `toDependencyKey` overloads (72-80). Nothing else in this header changes; `dependencykey_bindings.cpp` is untouched.
- `README.md` project-structure section and `tests/README.md` - replace mentions of the old classes, list the new test targets.

**Acceptance Criteria:**
- [ ] `git grep -nE "CalculatedValue|DependencyManager|calculatedvalue|dependencymanager|m_sideEffectKeys|m_activeCalculations|toDependencyKey|setCalculated(Attribute|Measurement)|registerCalculated(Attribute|Measurement)|unregisterCalculatedAttribute|addDependencies|hasRegisteredCalculation" -- src tests python_plugins CMakeLists.txt` returns **nothing**.
- [ ] `git grep -n "engine/" -- src/CMakeLists.txt` lists the engine sources once; no file is listed in two targets.
- [ ] Application, `flysight_cpp_bridge`, and all tests build and pass with `FLYSIGHT_BUILD_TESTS` ON; the application builds with it OFF.

**Complexity:** S

---

### Task 3.12: Session-level test suite

**Purpose:** Demonstrate acceptance items 9-11, 13, 15, 16 and the undeclared-read guarantee on real `SessionData` with the real built-ins and the global registry.

**Files to create:**
- `tests/tst_session_engine.cpp` (class `SessionEngineTest`) - `SessionData` only.
- `tests/tst_session_model_engine.cpp` (class `SessionModelEngineTest`) - `SessionModel`, `AltitudeMarkerManager`.

**Files to modify:** `tests/CMakeLists.txt`.

**Technical Approach:**

Both call `TestEnvironment::instance().registerBuiltIns()` in `initTestCase`
and `resetPreferencesToDefaults()` in `init()`. Sessions are built with the
Phase 1 builders (`DescentFixture::load()`) or programmatically through
`setAttribute` / `setMeasurement`. Run counts come from
`session.calculationEngine()`. "TIME data" below means `TIME/time` = {10,20,30},
`TIME/tow` = {129610,129620,129630}, `TIME/week` = {2295,2295,2295}
(fit a = 1, b = T0 = 1704110400).

`tst_session_engine`:

| Test | Scenario | Expected literals |
|---|---|---|
| `multiOutputRunsOnce` (acc. 9) | descent fixture; read `_TIME_FIT_B`, `_TIME_FIT_A`, `_TIME_FIT_B`; read the four `Simplified/*` in order `_time, lat, hMSL, lon` twice; read all ten WS-P outputs | `runCount("builtin.time.fit") == 1`, `runCount("builtin.simplified.track") == 1`, `runCount("builtin.wsp.results") == 1` |
| `declaredInputChangeRunsOnceMore` (9) | then `setMeasurement("TIME","tow",{129611,129621,129631})` | returned set contains `_TIME_FIT_A`, `_TIME_FIT_B`, `IMU/_time` (if read); `_TIME_FIT_B == "1704110401"`; fit `runCount == 2`; `runCount("builtin.simplified.track")` still 1 |
| `unrelatedChangeRunsNothing` (9) | `setAttribute("_DESCRIPTION","x")` | set == {`_DESCRIPTION`}; `totalRunCount()` unchanged after re-reading everything |
| `preferredSensorReplacesFallback` (acc. 10) | TIME data + `BARO/time` = {10,20,30}; read `_START_TIME`, `_DURATION` -> T0+10, 20.0; then `setMeasurement("GNSS","time",{1704110405.0,1704110406.0})` | returned set contains `_START_TIME` and `_DURATION`; next reads 1704110405.0 and 1.0; `runCount("builtin.attr.timeExtent.BARO") == 1`, `...GNSS == 1`; `verifyAgainstFresh` of both names is empty |
| `overrideOneOutput` (acc. 11) | TIME data + `BARO/time`; `setAttribute("_TIME_FIT_A","2")` | `_TIME_FIT_A == "2"`; `_TIME_FIT_B == "1704110400"`; `BARO/_time[0] == 1704110420.0`; `cycleCount() == 0`; fit `runCount == 1`; `removeAttribute` -> `_TIME_FIT_A == "1"`, fit `runCount` still 1, `BARO/_time[0] == 1704110410.0` |
| `overrideFlareStart` (11) | descent fixture; `setAttribute(_FLARE_START_TIME, T0+150)` | start T0+150, end T0+204, `runCount("builtin.attr.flare") == 1` |
| `declaredPreferenceInvalidates` (acc. 15) | 8-sample fixture of Task 3.4; listener installed on the engine; `PreferencesManager::setValue(ImportDescentPauseSeconds, 5.0)` | listener set contains `_ANALYSIS_START_TIME`; value 0.0 -> 45.0; `_GROUND_ELEV` 600.0; `GNSS/z` == {400,300,200,210,220,230,100,0} |
| `snapshotPreferencesDoNot` (15) | same session with `_JUMPER_MASS = 80.0`, `_PLANFORM_AREA = 2.0`, `_GROUND_ELEV = 50.0` stored; change `AeroMass` -> 90.0, `AeroArea` -> 3.0, `ImportFixedElevation` -> 10.0 | listener never called; attributes unchanged; `totalRunCount()` unchanged after re-reading `GNSS/z` (first sample 950.0) |
| `derivedWTotalFollowsSource` (acc. 16) | `IMU/wx,wy,wz` = {3},{4},{0} -> `IMU/wTotal` == {5.0}; set wx {6}, wy {8} -> {10.0}; then `setMeasurement("IMU","wTotal",{42.0})` | 42.0 with `runCount("builtin.imu.wTotal")` not incremented by the last read; `hasMeasurement("IMU","wTotal")` true only after the explicit set.  `// BASELINE: no gyro correction - Phase 4` on the 5.0 / 10.0 literals |
| `interpolatedAttributeFollows` (16) | TIME data, `IMU/time` = {10,20,30}, `IMU/wx` = {1,2,3}, `_M` = 1704110415.0 | `_M:IMU/_time/wx` == 1.5 (1e-9); set `IMU/wx` {10,20,30} -> 15.0; set `_M` 1704110425.0 -> 25.0; `_M` beyond the range -> unavailable |
| `noUndeclaredReadsAcrossBuiltIns` | descent fixture (all seven sensors); for every non-family id in `CalculationRegistry::instance().registeredIds()`: `calculationEngine().request(id)`; then read both interpolation golden keys | `undeclaredReadCount() == 0`; `cycleCount() == 0`; every `resultStatus(id)` is `Ok` (on the full fixture nothing is `MissingInput`); `scopeDepth() == 0` |
| `oracleOnFixture` | descent fixture; read all golden names; perform five edits (`_GROUND_ELEV` 0.0, remove it, `_EXIT_TIME` T0+30, `GNSS/velD` halved, pref 5.0) reading a rotating subset between edits | `verifyAgainstFresh(allGoldenNames)` empty after each edit |
| `copyAndMoveSemantics` | warm session `a`; `SessionData b = a;` `SessionData c = std::move(a);` | Task 3.9's copy/move criterion, with `_EXIT_TIME` == T0+9 on both |

`tst_session_model_engine` (constructs a `SessionModel`, `mergeSessions` two
copies of the descent fixture with session ids `"s1"`, `"s2"`; session 2 gets
stored `_GROUND_ELEV = 0.0` via `updateAttribute`):

| Test | Scenario | Expected |
|---|---|---|
| `twoSessionsOneRegistration` (acc. 13) | write the `altitudeMarkers` `QSettings` array `[1000]` **then** `setValue(AltitudeMarkersUnits, "Metric")`; `AltitudeMarkerManager mgr(&model); mgr.registerAll();` read `_ALTITUDE_1000_M` on both | s1 = T0+71.5, s2 = T0+78.0; one registration id `builtin.altitude._ALTITUDE_1000_M`; each session's `runCount` for it == 1 |
| `unregisterInvalidatesEverySession` (13) | empty the array, bump `AltitudeMarkersSize` (any `altitudeMarkers/` pref) to trigger `refresh()`; `flushPendingInvalidations()` | `dependencyChanged` spy has (`s1`, `_ALTITUDE_1000_M`) and (`s2`, ...); both reads now unavailable; `resultStatus(id) == nullopt` in both engines; `hasCandidateFor` false; no computation ran during the unregister (`totalRunCount()` unchanged until the re-read) |
| `reRegisterRestores` | put `[1000]` back, refresh | values return; spy shows the names again |
| `preferenceBroadcastReachesModel` (15) | Task 3.10's first two criteria | |
| `mergeEmitsDependencyChanged` | `mergeSessions({sensorOnlySessionFor_s1})` | spy contains (`s1`, `IMU/wx`) |
| `rowsSurviveSort` | read `_EXIT_TIME` on both rows, `model.sort(0)`, change the pause preference, flush | both rows still deliver `dependencyChanged`; values equal golden |

**Acceptance Criteria:**
- [ ] Both targets are registered with `flysight_add_test`, pass on Windows Release, and each acceptance number (9, 10, 11, 13, 15, 16) appears in a comment on at least one test function.
- [ ] Every expected value is a literal; the only computed comparison is `verifyAgainstFresh`.
- [ ] Tests leave the global registry as they found it (altitude registrations removed in `cleanup()`), and write nothing outside `TestEnvironment::rootPath()`.
- [ ] Temporarily registering a calculation that reads an undeclared input makes `noUndeclaredReadsAcrossBuiltIns` fail (checked once by hand, not committed).

**Complexity:** L

---

## Testing Requirements

### Unit Tests
- New: `tst_builtins_golden` (3.1), `tst_builtins_engine` (3.3-3.7), `tst_session_engine`, `tst_session_model_engine` (3.12); additions to `tst_calcregistry` and `tst_harness` (3.2).
- Existing: `tst_smoke` must pass **unchanged** - no `// BASELINE:` expectation moves in this phase. `tst_harness` `preferencesRegistered` unchanged. Phase 2's four engine suites unchanged apart from the `registeredIds` addition.
- `TestEnvironment::registerBuiltIns()` changes implementation in 3.9 (new entry points + `EnginePreferenceProvider::install()`).

### Integration Tests
- After every task: `cmake --build build --config Release` with `-DFLYSIGHT_BUILD_TESTS=ON`, then `ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure` - all green.
- After 3.9 and 3.11: also build with the option OFF.
- The grep criteria of Tasks 3.9 and 3.11 are part of the integration check (acceptance 19, engine part).

### Manual Verification
1. Launch; existing logbook shows cached columns; open a session: plots, markers (exit, landing, flare, max speeds), WS-P and SP docks show the same numbers as the `v2026.04.1` build for the same session (compare two or three real jumps side by side).
2. Drag an editable marker (e.g. manoeuvre start), confirm dependent markers/plots update; right-click its bubble -> "Reset to default" restores the calculated value.
3. WS-P dock: change top/bottom altitude, results update; "Restore FAI defaults" returns 2500 / 1500. Same for SP.
4. Preferences -> Import -> descent pause: change 30 -> 5 with a session plotted: analysis-dependent markers move **without re-import** (new behavior; acceptance 15). Change mass/area: nothing moves for existing sessions.
5. Preferences -> Altitude markers: add and remove an altitude and switch units with a session plotted; markers appear/disappear correctly, no stale marker values.
6. Logbook columns of type "measurement at marker" and "delta" still populate; sort by one of them.
7. Import a `TRACK.CSV` + `SENSOR.CSV` pair: one merged session, IMU/MAG/BARO plots against UTC time work (time fit).
8. Python console output still shows the bridge and SDK importing; "Registered 0 attributes, 0 measurements".

## Notes for Implementer

### Gotchas
- **All declared inputs are required.** Never "declare to be safe". Declaring `_SESSION_ID` (nonexistent) would disable the GNSS/IMU/MAG calculations entirely. Conversely every read in a compute must be declared, including preferences.
- **Declared order matters for tests** that pin dependency sets, because the availability pass short-circuits at the first missing input. Keep the old dependency-list order.
- **`hasMeasurement` inside the old time recipes (45-47, 116)** meant "stored, not derived". The new inputs accept a derived `TIME/time` too. No registered calculation outputs `*/time`, `TIME/tow`, or `TIME/week`, so behavior is identical; do not try to reproduce the stored-only check (there is no such declared-input kind for ordinary calculations).
- **Time-fit outputs are `QString`s.** Consumers call `toDouble()`. Do not "fix" this here; the golden test pins it.
- **Stored-but-empty measurements** (the importer creates `setMeasurement(sensor, col, {})` at header time, baseline 268) resolve as `Source` and are unavailable; they never fall through to a derived candidate. Same as today.
- **Public reads inside compute are forbidden** (engine asserts). `systemTimeToUtc(session, …)` and friends are UI helpers only. The Python view exists for exactly this reason.
- **Two copies of `flysight_model` statics exist** (executable and `.pyd`). Bridge code must only touch objects handed to it (`EvaluationContext` through `PluginSessionView`); it must never call `CalculationRegistry::instance()` or create a `SessionData`.
- **`SessionData` is now polymorphic and non-trivially movable.** Keep move operations `noexcept`; never declare it relocatable; never keep a `SessionData*` across a model reset/sort (unchanged rule).
- **Listener identity:** the listener travels with the engine on move and is dropped on copy. Any new code path that assigns `SessionRow::session` must call `attachSession`.
- **Registry lifetime:** `~CalculationRegistry` asserts that no engine is enrolled. No `SessionData` with an engine may have static storage duration. `SessionModel` dies with `MainWindow`, before static destruction.
- **Registration during evaluation is rejected** by the registry. `AltitudeMarkerManager::refresh()` runs from a `preferenceChanged` slot on the main thread, never inside a compute, so this cannot trigger - do not call `PreferencesManager::setValue` from a compute function.
- **`registerAll()` writes marker colour preferences** (138-140), which fire `preferenceChanged` -> `notifyPreferenceChanged`; no calculation declares those keys, so nothing is invalidated and no listener fires.
- **Copies are cold.** `PlotWidget::zoomToExtent(QVector<SessionData>)` (baseline `mainwindow.cpp` 636-641) now recomputes `_time` / analysis values on its copies once per import. Acceptable; do not restructure the UI API in this phase.
- **Do not add a fast path for stored values in `SessionData::get*`.** Phase 4 needs every measurement read to pass through the engine.
- `PreferencesManager::getValue` asserts in Debug on unregistered keys; the adapter must go through `hasPreference`.
- `DependencyKey`'s default constructor leaves `type` uninitialized; never default-construct one for comparison in tests.

### Decisions Made
- **Side-by-side overloads, one atomic switch (Task 3.9).** Satisfies "never two engines serving reads across a task boundary" while letting every migrated calculation be verified against golden values on `FakeSessionState` before the switch. No run-time dual path exists at any task boundary.
- **Golden values captured first, against the old engine,** on a fixture designed so that almost every value is hand-derivable; only geodesic distance and a few derivative-based samples are captured.
- **`_START_TIME` + `_DURATION` = one two-output calculation per sensor** (seven ordered candidates). One scan of one input produces both; at baseline both outputs always chose the same sensor because their recipes had identical dependencies, so merging changes no result.
- **`_SYNC_TIME` and `_COURSE_REF` stay separate; WS-P/SP constant defaults stay one calculation each** - unrelated outputs are not merged (spec 7.1).
- **WS-P results keep all eleven inputs required,** mirroring the old dependency list (no results without `hAcc`/`vAcc`/Ref1), to preserve behavior.
- **Interpolation = family `builtin.interpolation`, registered last,** syntax and `interpolationKey` unchanged, negative results cached.
- **Altitude thresholds baked into the registration; refresh diffs the key set.** Identity encodes the threshold; preference changes change which registrations exist.
- **`SessionData` owns a lazily created engine via `unique_ptr`; copy = state only, move = state + engine + `rebind`.** Lazy creation keeps temporary/import copies free and makes moved-from objects valid.
- **All reads go through the engine,** including stored values (zero-copy passthrough; no conversion family in this phase).
- **Broadcasts reach the UI through a per-row listener -> coalesced queued flush -> `publishInvalidation`** (`dataChanged` + `dependencyChanged`) **+ cleared `cachedValues` + `startColumnWorker()`**; no dirty flag, no save.
- **`mergeSessions`** only stops discarding the invalidation sets; semantics are Phase 6.
- **Preferences:** `EnginePreferenceProvider` in `flysight_core`, installed after `initializePreferences()`; `PreferencesManager::hasPreference` added. Mass, area, fixed elevation remain session attributes written at import.
- **Python:** `PluginSessionView` over `EvaluationContext`, bound under the Python name `SessionData`, shared-pointer holder + `invalidate()`; adapters registered as `plugin.attr.<i>.<name>` / `plugin.meas.<i>.<sensor>/<name>`; the C++ `SessionData` is no longer bound to Python.
- **Startup order preserved** (plugins before built-ins) per the overview; still flagged there for Michael's review.
- **Entry point renamed:** `registerBuiltInCalculations()` + `registerBuiltInCalculationMetadata()` in `src/calculations/builtincalculations.*`; `CalculatedValueRegistry` and the three empty placeholder files are deleted.
- **Engine additions requested of Phase 2's code (Task 3.2):** `CalculationRegistry::registeredIds()`, `CalculationRegistry::isFamily()`. Nothing in Phase 2 is contradicted.

### Open Questions

> Coordinator note: the per-row `cachedValues.clear()` in
> `flushPendingInvalidations` (Task 3.10) is superseded by Phase 5 Task 5.7's
> `checkCalculationEnvironment` / `invalidateColumns`; the unloaded-session
> staleness question below is resolved there (`calculationEnvironment`
> fingerprint).
- **Cached logbook columns of unloaded sessions** become stale when the descent-pause preference (or the altitude list) changes, since only loaded rows are refreshed in Task 3.10. At baseline nothing at all reacted to that preference. Suggest Phase 5 fold declared-preference values into (or alongside) the calculation-compatibility marker, or clear all stub `cachedValues` on a declared-preference change. Needs Michael's call; does not block this phase.
- `_WSP_DIST_RESULT` / `_WSP_SPEED_RESULT` and a handful of GNSS sample goldens are captured rather than hand-derived. If Michael wants strictly independent literals, compute them once with an external GeographicLib / spreadsheet and replace the captured values.

## Definition of Done

This phase is complete when:
1. All twelve tasks have passing acceptance criteria.
2. `tst_harness`, `tst_smoke` (BASELINE pins untouched), the four Phase 2 engine suites, `tst_builtins_golden`, `tst_builtins_engine`, `tst_session_engine`, and `tst_session_model_engine` pass via CTest on Windows Release; the application and `flysight_cpp_bridge` build with `FLYSIGHT_BUILD_TESTS` ON and OFF.
3. The grep criterion of Task 3.11 returns nothing: there is one cache, one dependency graph, one mutation path.
4. Every calculated read in the application is served by `CalculationEngine`; every compute function reads only through `EvaluationContext`; `undeclaredReadCount()` is zero across all built-ins on the full fixture.
5. Built-in outputs equal their `v2026.04.1` values on the golden fixture; the application behaves as at baseline except for the intended fixes (preference change re-evaluates the analysis range; altitude-marker changes invalidate loaded sessions; merges notify dependents).
6. Spec coverage: 3.3 (attribute/measurement resolution) - Tasks 3.9, 3.12; 4 (enumeration stays stored-only) - 3.1; 7.1-7.3 - 3.3-3.6, 3.12; 7.5 (registry change invalidates every session) - 3.9, 3.10, 3.12; 7.6 (interpolation) - 3.7; 7.8 - 3.2, 3.4, 3.10, 3.12; acceptance 9, 10, 11, 13, 15, 16 - 3.12; acceptance 19 (engine part) - 3.11.
7. No TODOs or placeholder code remain; nothing has been pushed.
