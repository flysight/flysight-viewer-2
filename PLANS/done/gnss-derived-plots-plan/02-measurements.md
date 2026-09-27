# Phase 2: Measurements

## Overview

This phase registers all 16 new GNSS-derived calculated measurements using the foundation established in Phase 1 (derivative helper, ISA density model, session keys, preference keys, unit types). The measurements are organized into three groups: intermediate measurements (not plotted, used as dependencies), basic GNSS measurements, and advanced GNSS measurements. All registrations follow the existing pattern in `src/calculations/gnsscalculations.cpp`.

## Dependencies

- **Depends on:** Phase 1 (Foundation) -- requires derivative helper, ISA density model, session keys `WindN`/`WindE`/`CourseRef`, preference keys `AeroMass`/`AeroArea`, and new unit types
- **Blocks:** Phase 3 (UI & Tools) -- plot registration and menu items reference these measurement IDs
- **Assumptions:**
  - Phase 1 is fully implemented: `Calculations::computeDerivative()`, `Calculations::isaDensity()`, `SessionKeys::WindN`, `SessionKeys::WindE`, `SessionKeys::CourseRef`, `PreferenceKeys::AeroMass`, `PreferenceKeys::AeroArea` all exist
  - The `accD` registration in `gnsscalculations.cpp` already uses `Calculations::computeDerivative()` (Task 1.3)
  - Session attributes default to invalid QVariant when not explicitly set; lambdas must handle `toDouble(&ok)` failures

## Tasks

### Task 2.1: Register Intermediate Measurements (accN, accE, wcVel)

**Purpose:** Several advanced measurements depend on intermediate values that are not plotted directly. Registering `accN`, `accE`, and `wcVel` as calculated measurements makes them available through the dependency system.

**Files to modify:**
- `src/calculations/gnsscalculations.cpp` -- Add three new `registerCalculatedMeasurement` calls

**Technical Approach:**

Add the following three registrations inside `registerGnssCalculations()`, after the existing `accD` registration (line 167). Follow the same lambda pattern used by `accD` after Phase 1 refactoring.

**accN (northward acceleration):**
- Sensor: `"GNSS"`, measurement: `"accN"`
- Dependencies: `DependencyKey::measurement("GNSS", "velN")`, `DependencyKey::measurement("GNSS", "time")`
- Lambda: Fetch `velN` and `time`, return `Calculations::computeDerivative(velN, time)`. Return `std::nullopt` if `velN` is empty.

**accE (eastward acceleration):**
- Sensor: `"GNSS"`, measurement: `"accE"`
- Dependencies: `DependencyKey::measurement("GNSS", "velE")`, `DependencyKey::measurement("GNSS", "time")`
- Lambda: Fetch `velE` and `time`, return `Calculations::computeDerivative(velE, time)`. Return `std::nullopt` if `velE` is empty.

**wcVel (wind-corrected total speed):**
- Sensor: `"GNSS"`, measurement: `"wcVel"`
- Dependencies: `DependencyKey::measurement("GNSS", "velN")`, `DependencyKey::measurement("GNSS", "velE")`, `DependencyKey::measurement("GNSS", "velD")`, `DependencyKey::attribute(SessionKeys::WindN)`, `DependencyKey::attribute(SessionKeys::WindE)`
- Lambda:
  1. Fetch `velN`, `velE`, `velD`
  2. Read `windN` and `windE` from attributes via `session.getAttribute(SessionKeys::WindN).toDouble(&ok)` and `session.getAttribute(SessionKeys::WindE).toDouble(&ok)`. If either fails, default to `0.0` (wind is optional and defaults to zero).
  3. Validate sizes match, return `std::nullopt` if any are empty or mismatched
  4. For each sample: `sqrt((velN[i]-windN)^2 + (velE[i]-windE)^2 + velD[i]^2)`
  5. Return the result vector

**Acceptance Criteria:**
- [ ] `accN` is registered with dependencies on `velN` and `time`, uses `computeDerivative`
- [ ] `accE` is registered with dependencies on `velE` and `time`, uses `computeDerivative`
- [ ] `wcVel` is registered with dependencies on `velN`, `velE`, `velD`, `WindN` attribute, `WindE` attribute
- [ ] `wcVel` defaults wind to 0.0 when attributes are missing
- [ ] `wcVel` formula: `sqrt((velN-windN)^2 + (velE-windE)^2 + velD^2)`
- [ ] Project compiles without errors

**Complexity:** M

---

### Task 2.2: Register Basic GNSS Measurements (course, courseRate, glideRatio, diveAngle, diveAngleRate)

**Purpose:** Register the five basic GNSS measurements that appear in the "GNSS (Basic)" plot category.

**Files to modify:**
- `src/calculations/gnsscalculations.cpp` -- Add five new `registerCalculatedMeasurement` calls

**Technical Approach:**

Add the following five registrations inside `registerGnssCalculations()`, after the intermediate measurements from Task 2.1.

**course (unwrapped heading minus reference):**
- Sensor: `"GNSS"`, measurement: `"course"`
- Dependencies: `DependencyKey::measurement("GNSS", "velN")`, `DependencyKey::measurement("GNSS", "velE")`, `DependencyKey::attribute(SessionKeys::CourseRef)`
- Lambda:
  1. Fetch `velN`, `velE`. Return `std::nullopt` if either is empty or sizes mismatch.
  2. Read `courseRef` from `session.getAttribute(SessionKeys::CourseRef).toDouble(&ok)`. Default to `0.0` if missing.
  3. Compute raw heading for each sample: `rawDeg[i] = atan2(velE[i], velN[i]) * 180.0 / M_PI`
  4. Unwrap phase:
     - `course[0] = rawDeg[0]`
     - For each subsequent sample, compute `delta = rawDeg[i] - rawDeg[i-1]`. If `delta > 180.0`, subtract `360.0`. If `delta < -180.0`, add `360.0`. Then `course[i] = course[i-1] + delta`.
  5. Subtract `courseRef` from every element: `course[i] -= courseRef`
  6. Return the result vector

  Include `<cmath>` for `atan2` and use `M_PI` (or `std::numbers::pi` if C++20; `M_PI` is available via `<cmath>` in MSVC with `_USE_MATH_DEFINES` or already defined). The existing file already includes `<cmath>`.

**courseRate (rate of change of course):**
- Sensor: `"GNSS"`, measurement: `"courseRate"`
- Dependencies: `DependencyKey::measurement("GNSS", "course")`, `DependencyKey::measurement("GNSS", "time")`
- Lambda: Fetch `course` and `time`, return `Calculations::computeDerivative(course, time)`. Return `std::nullopt` if `course` is empty.

**glideRatio:**
- Sensor: `"GNSS"`, measurement: `"glideRatio"`
- Dependencies: `DependencyKey::measurement("GNSS", "velH")`, `DependencyKey::measurement("GNSS", "velD")`
- Lambda:
  1. Fetch `velH`, `velD`. Return `std::nullopt` if either is empty or sizes mismatch.
  2. For each sample: if `std::abs(velD[i]) < 1e-6`, set `result[i] = NaN` (`std::numeric_limits<double>::quiet_NaN()`). Otherwise, `result[i] = velH[i] / velD[i]`.
  3. Sign convention: positive when falling (velD > 0 in GNSS convention), which is the natural result of the division.
  4. Return the result vector

  Include `<limits>` for `std::numeric_limits`.

**diveAngle:**
- Sensor: `"GNSS"`, measurement: `"diveAngle"`
- Dependencies: `DependencyKey::measurement("GNSS", "velH")`, `DependencyKey::measurement("GNSS", "velD")`
- Lambda:
  1. Fetch `velH`, `velD`. Return `std::nullopt` if either is empty or sizes mismatch.
  2. For each sample: `result[i] = atan2(velD[i], velH[i]) * 180.0 / M_PI`
  3. 0 degrees = horizontal, 90 degrees = straight down (velD >> velH)
  4. Return the result vector

**diveAngleRate (rate of change of dive angle):**
- Sensor: `"GNSS"`, measurement: `"diveAngleRate"`
- Dependencies: `DependencyKey::measurement("GNSS", "diveAngle")`, `DependencyKey::measurement("GNSS", "time")`
- Lambda: Fetch `diveAngle` and `time`, return `Calculations::computeDerivative(diveAngle, time)`. Return `std::nullopt` if `diveAngle` is empty.

**Acceptance Criteria:**
- [ ] `course` uses atan2-based unwrapping: delta clamped to [-180, +180], accumulated from first raw value
- [ ] `course` subtracts the `CourseRef` attribute after unwrapping
- [ ] `course` defaults `courseRef` to 0.0 when missing
- [ ] `courseRate` uses `computeDerivative(course, time)`
- [ ] `glideRatio` returns `velH / velD`, with NaN when `|velD| < 1e-6`
- [ ] `diveAngle` returns `atan2(velD, velH)` in degrees
- [ ] `diveAngleRate` uses `computeDerivative(diveAngle, time)`
- [ ] All five measurements have correct dependency lists including both measurement and attribute dependencies
- [ ] Project compiles without errors

**Complexity:** L

---

### Task 2.3: Register Advanced GNSS Measurements -- Simple Derivatives and Wind-Corrected Speed (accH, wcVelH)

**Purpose:** Register the simpler advanced measurements that are direct derivatives or straightforward formulas.

**Files to modify:**
- `src/calculations/gnsscalculations.cpp` -- Add two new `registerCalculatedMeasurement` calls

**Technical Approach:**

Add the following two registrations after the basic measurements from Task 2.2.

**accH (horizontal acceleration):**
- Sensor: `"GNSS"`, measurement: `"accH"`
- Dependencies: `DependencyKey::measurement("GNSS", "velH")`, `DependencyKey::measurement("GNSS", "time")`
- Lambda: Fetch `velH` and `time`, return `Calculations::computeDerivative(velH, time)`. Return `std::nullopt` if `velH` is empty.

**wcVelH (wind-corrected horizontal speed):**
- Sensor: `"GNSS"`, measurement: `"wcVelH"`
- Dependencies: `DependencyKey::measurement("GNSS", "velN")`, `DependencyKey::measurement("GNSS", "velE")`, `DependencyKey::attribute(SessionKeys::WindN)`, `DependencyKey::attribute(SessionKeys::WindE)`
- Lambda:
  1. Fetch `velN`, `velE`. Return `std::nullopt` if either is empty or sizes mismatch.
  2. Read `windN` and `windE` from attributes. Default to `0.0` if missing.
  3. For each sample: `result[i] = sqrt((velN[i]-windN)^2 + (velE[i]-windE)^2)`
  4. Return the result vector

**Acceptance Criteria:**
- [ ] `accH` uses `computeDerivative(velH, time)`
- [ ] `wcVelH` formula: `sqrt((velN-windN)^2 + (velE-windE)^2)`
- [ ] `wcVelH` defaults wind to 0.0 when attributes are missing
- [ ] Both measurements have correct dependency lists
- [ ] Project compiles without errors

**Complexity:** S

---

### Task 2.4: Register Along-Track and Cross-Track Acceleration (accAlongTrack, accCrossTrack)

**Purpose:** Register the along-track and cross-track acceleration measurements used as inputs for lift and drag coefficients.

**Files to modify:**
- `src/calculations/gnsscalculations.cpp` -- Add two new `registerCalculatedMeasurement` calls

**Technical Approach:**

Add the following two registrations after Task 2.3's measurements.

**accAlongTrack (along-track acceleration):**
- Sensor: `"GNSS"`, measurement: `"accAlongTrack"`
- Dependencies: `DependencyKey::measurement("GNSS", "accN")`, `DependencyKey::measurement("GNSS", "accE")`, `DependencyKey::measurement("GNSS", "accD")`, `DependencyKey::measurement("GNSS", "velN")`, `DependencyKey::measurement("GNSS", "velE")`, `DependencyKey::measurement("GNSS", "velD")`, `DependencyKey::attribute(SessionKeys::WindN)`, `DependencyKey::attribute(SessionKeys::WindE)`
- Lambda:
  1. Fetch `accN`, `accE`, `accD`, `velN`, `velE`, `velD`. Return `std::nullopt` if any are empty.
  2. Validate all six vectors have equal size.
  3. Read `windN` and `windE` from attributes. Default to `0.0` if missing.
  4. For each sample i:
     - Wind-corrected velocity: `wcN = velN[i] - windN`, `wcE = velE[i] - windE`, `wcD = velD[i]`
     - Magnitude: `wcMag = sqrt(wcN*wcN + wcE*wcE + wcD*wcD)`
     - If `wcMag < 1e-9`, set along-track to `0.0` (avoid division by zero)
     - Else: unit vector `uN = wcN/wcMag`, `uE = wcE/wcMag`, `uD = wcD/wcMag`
     - Dot product: `dot = accN[i]*uN + accE[i]*uE + accD[i]*uD`
     - Along-track = `-dot` (negated so deceleration/drag gives positive values)
  5. Return the result vector

**accCrossTrack (cross-track acceleration):**
- Sensor: `"GNSS"`, measurement: `"accCrossTrack"`
- Dependencies: Same as `accAlongTrack` (all six measurement dependencies plus two wind attributes)
- Lambda:
  1. Fetch the same six measurement vectors and two wind attributes as `accAlongTrack`.
  2. For each sample i:
     - Compute `wcN`, `wcE`, `wcD`, `wcMag` as above
     - Compute `alongTrack` as above (the negated dot product)
     - Acceleration magnitude squared: `aMag2 = accN[i]^2 + accE[i]^2 + accD[i]^2`
     - Cross-track: `sqrt(max(0.0, aMag2 - alongTrack^2))` (clamped to avoid negative under sqrt due to floating-point)
  3. Return the result vector

  **Important:** `accCrossTrack` uses the pre-negation dot product value for the `aMag2 - along^2` calculation. Since negation does not change the square, using `alongTrack` (negated) is correct: `(-dot)^2 == dot^2`.

**Acceptance Criteria:**
- [ ] `accAlongTrack` computes `dot(a, normalize(wc))` and negates the result
- [ ] `accCrossTrack` computes `sqrt(|a|^2 - alongTrack^2)`, clamped to non-negative
- [ ] Both handle zero-magnitude wind-corrected velocity gracefully (no NaN/Inf)
- [ ] Both have eight dependencies: accN, accE, accD, velN, velE, velD, WindN attr, WindE attr
- [ ] Project compiles without errors

**Complexity:** M

---

### Task 2.5: Register Lift and Drag Coefficients (lift, drag)

**Purpose:** Register the lift and drag coefficient measurements, which combine cross-track/along-track accelerations with ISA density and user-configured mass/area preferences.

**Files to modify:**
- `src/calculations/gnsscalculations.cpp` -- Add two new `registerCalculatedMeasurement` calls and include headers for ISA density and preferences

**Technical Approach:**

Add the following includes at the top of `gnsscalculations.cpp` (if not already present from earlier tasks):
- `#include "isadensity.h"` (for `Calculations::isaDensity()`)
- `#include "../preferences/preferencesmanager.h"` (for `PreferencesManager::instance().getValue()`)
- `#include "../preferences/preferencekeys.h"` (for `PreferenceKeys::AeroMass`, `PreferenceKeys::AeroArea`)

Then add the following two registrations after Task 2.4's measurements.

**lift (lift coefficient):**
- Sensor: `"GNSS"`, measurement: `"lift"`
- Dependencies: `DependencyKey::measurement("GNSS", "accCrossTrack")`, `DependencyKey::measurement("GNSS", "wcVel")`, `DependencyKey::measurement("GNSS", "hMSL")`
- Lambda:
  1. Fetch `accCrossTrack`, `wcVel`, `hMSL`. Return `std::nullopt` if any are empty or sizes mismatch.
  2. Read mass and area from preferences at compute time:
     ```
     double mass = PreferencesManager::instance().getValue(PreferenceKeys::AeroMass).toDouble();
     double area = PreferencesManager::instance().getValue(PreferenceKeys::AeroArea).toDouble();
     ```
  3. For each sample i:
     - `rho = Calculations::isaDensity(hMSL[i])`
     - `mu = wcVel[i]` (wind-corrected total speed)
     - If `mu < 1e-9` or `rho < 1e-12` or `area < 1e-12`, set `result[i] = NaN`
     - Else: `result[i] = 2.0 * mass * accCrossTrack[i] / (rho * mu * mu * area)`
  4. Return the result vector

**drag (drag coefficient):**
- Sensor: `"GNSS"`, measurement: `"drag"`
- Dependencies: `DependencyKey::measurement("GNSS", "accAlongTrack")`, `DependencyKey::measurement("GNSS", "wcVel")`, `DependencyKey::measurement("GNSS", "hMSL")`
- Lambda: Same structure as `lift`, but uses `accAlongTrack` instead of `accCrossTrack`:
  - `result[i] = 2.0 * mass * accAlongTrack[i] / (rho * mu * mu * area)`

**Note:** Preferences are read inside the lambda at compute time, not captured at registration time. This ensures changes to aero/mass or aero/area take effect on recalculation without re-registering.

**Acceptance Criteria:**
- [ ] `lift` uses formula: `CL = 2 * m * accCrossTrack / (rho * mu^2 * A)`
- [ ] `drag` uses formula: `CD = 2 * m * accAlongTrack / (rho * mu^2 * A)`
- [ ] Both read `mass` and `area` from `PreferencesManager` at compute time
- [ ] Both use `Calculations::isaDensity(hMSL[i])` for per-sample density
- [ ] Both return NaN for samples where `wcVel` is near zero
- [ ] `lift` depends on `accCrossTrack`, `wcVel`, `hMSL`
- [ ] `drag` depends on `accAlongTrack`, `wcVel`, `hMSL`
- [ ] `#include "isadensity.h"`, `#include "../preferences/preferencesmanager.h"`, and `#include "../preferences/preferencekeys.h"` are added to includes
- [ ] Project compiles without errors

**Complexity:** M

---

### Task 2.6: Register Specific Energy Measurements (specificEnergy, specificEnergyRate)

**Purpose:** Register specific energy (kinetic + potential per unit mass) and its rate of change.

**Files to modify:**
- `src/calculations/gnsscalculations.cpp` -- Add two new `registerCalculatedMeasurement` calls

**Technical Approach:**

Add the following two registrations after Task 2.5's measurements.

**specificEnergy:**
- Sensor: `"GNSS"`, measurement: `"specificEnergy"`
- Dependencies: `DependencyKey::measurement("GNSS", "vel")`, `DependencyKey::measurement("GNSS", "z")`
- Lambda:
  1. Fetch `vel` and `z`. Return `std::nullopt` if either is empty or sizes mismatch.
  2. Use gravitational constant `g = 9.80665` (same as ISA model constant).
  3. For each sample: `result[i] = 0.5 * vel[i] * vel[i] + g * z[i]`
  4. Result is in J/kg (the unit type `specific_energy` handles display scaling to kJ/kg).
  5. Return the result vector

**specificEnergyRate:**
- Sensor: `"GNSS"`, measurement: `"specificEnergyRate"`
- Dependencies: `DependencyKey::measurement("GNSS", "specificEnergy")`, `DependencyKey::measurement("GNSS", "time")`
- Lambda: Fetch `specificEnergy` and `time`, return `Calculations::computeDerivative(specificEnergy, time)`. Return `std::nullopt` if `specificEnergy` is empty.

**Acceptance Criteria:**
- [ ] `specificEnergy` formula: `0.5 * vel^2 + g * z` where `g = 9.80665`
- [ ] `specificEnergy` depends on `vel` and `z` (both calculated measurements themselves)
- [ ] `specificEnergyRate` uses `computeDerivative(specificEnergy, time)`
- [ ] `specificEnergyRate` depends on `specificEnergy` and `time`
- [ ] Project compiles without errors

**Complexity:** S

---

### Task 2.7: Add Required Includes to gnsscalculations.cpp

**Purpose:** Ensure all new includes needed across Tasks 2.1-2.6 are present.

**Files to modify:**
- `src/calculations/gnsscalculations.cpp` -- Add missing `#include` directives

**Technical Approach:**

Add the following includes to the top of `gnsscalculations.cpp` (after the existing includes, before `using namespace FlySight`):

```cpp
#include "derivativehelper.h"       // Already added in Phase 1 Task 1.3
#include "isadensity.h"             // NEW: for Calculations::isaDensity()
#include "../preferences/preferencesmanager.h"  // NEW: for PreferencesManager::instance()
#include "../preferences/preferencekeys.h"      // NEW: for PreferenceKeys::AeroMass, AeroArea
#include <limits>                   // NEW: for std::numeric_limits (NaN in glideRatio)
```

The `"derivativehelper.h"` include should already be present from Phase 1 Task 1.3. The remaining three are new for Phase 2.

**Acceptance Criteria:**
- [ ] `#include "isadensity.h"` is present
- [ ] `#include "../preferences/preferencesmanager.h"` is present
- [ ] `#include "../preferences/preferencekeys.h"` is present
- [ ] `#include <limits>` is present
- [ ] No duplicate includes
- [ ] Project compiles without errors

**Complexity:** S

---

## Testing Requirements

### Unit Tests

- No formal unit test framework appears to be in use in this project. If one is added later, the following tests would be valuable:
  - `course` unwrapping: feed `velE`/`velN` values that produce headings crossing 360/0 boundary, verify continuous output
  - `glideRatio`: verify NaN at `velD = 0`, positive when `velD > 0`
  - `accAlongTrack`/`accCrossTrack`: verify with a known velocity/acceleration vector that the decomposition is correct and that `along^2 + cross^2 = |a|^2`
  - `lift`/`drag`: verify formula with known density, speed, and acceleration values

### Integration Tests

- After all tasks are complete, load a real GNSS dataset and verify that:
  - All 16 new measurements resolve without errors (no `std::nullopt` for valid data)
  - Measurements that depend on other calculated measurements (e.g., `courseRate` depends on `course`) resolve in the correct order via the dependency system

### Manual Verification

1. Build the project: `cmake --build build --config Release`
2. Launch the application and open a logbook with GNSS data
3. Use the debugger or add temporary logging to verify each new measurement produces non-empty output for a session with valid GNSS data
4. Verify `course` values are continuous (no 360-degree jumps) by temporarily plotting raw values
5. Verify `glideRatio` contains NaN at samples where vertical speed is near zero
6. Verify `accAlongTrack` is positive during deceleration phases (e.g., after deploying a parachute)
7. Verify that changing `_COURSE_REF` attribute on a session causes `course` to update (dependency invalidation)
8. Verify that `lift` and `drag` respond to changes in aero/mass and aero/area preferences (requires manual recalculation trigger)

## Notes for Implementer

### Gotchas

- **Registration order matters for readability, not execution:** The dependency system resolves calculations lazily, so registration order in `registerGnssCalculations()` does not affect correctness. However, registering in dependency order (intermediates first, then basic, then advanced) makes the code easier to follow.
- **Wind attribute defaults:** `_WIND_N` and `_WIND_E` may not be set on most sessions. The lambdas must handle this gracefully by defaulting to 0.0 (no wind correction). Use `toDouble(&ok)` and check `ok`, falling back to `0.0`. Do NOT return `std::nullopt` for missing wind -- this would make all wind-dependent measurements fail on every session.
- **CourseRef attribute default:** Similarly, `_COURSE_REF` may not be set. Default to `0.0` (no reference subtraction).
- **NaN propagation in glide ratio:** Plotting code should already handle NaN values (gaps in plot lines). Verify this assumption holds.
- **Cross-track floating-point safety:** When computing `sqrt(aMag2 - along^2)`, floating-point errors can make the argument slightly negative. Always clamp to `max(0.0, ...)` before taking the square root.
- **M_PI availability:** The file already includes `<cmath>` and MSVC provides `M_PI`. If there are compilation issues, define `_USE_MATH_DEFINES` before `<cmath>` or use a local constant `constexpr double PI = 3.14159265358979323846;`.
- **Preferences at compute time:** The `lift` and `drag` lambdas must call `PreferencesManager::instance().getValue()` inside the lambda body, not capture the values at registration time. This ensures preference changes take effect on recalculation.
- **accAlongTrack negation:** The dot product `dot(a, normalize(wc))` gives positive values when accelerating along the flight path. The spec requires negation so that drag (deceleration) gives positive values. This is intentional and matches aerodynamic convention.

### Decisions Made

- **Wind default to 0.0 rather than failing:** The overview says `_WIND_N` and `_WIND_E` are "placeholder" attributes defaulting to 0.0. Rather than returning `std::nullopt` when they are absent, the lambdas default to 0.0. This ensures wind-dependent measurements (wcVel, wcVelH, accAlongTrack, accCrossTrack, lift, drag) still produce output on sessions without explicit wind values.
- **Glide ratio NaN threshold:** Using `|velD| < 1e-6` as the threshold for producing NaN. This is small enough to avoid visible artifacts at typical GNSS data rates but prevents division by truly near-zero values.
- **Zero-speed handling in lift/drag:** When `wcVel < 1e-9`, the coefficient is set to NaN rather than 0 or infinity. This avoids misleading data at near-zero speed.
- **Task granularity:** Tasks are split by logical grouping (intermediates, basic, advanced-simple, along/cross-track, lift/drag, specific energy) rather than one task per measurement, keeping the total task count manageable while maintaining clear scope boundaries.

### Open Questions

- None. All formulas, sign conventions, and dependency declarations are specified in the overview document.

## Definition of Done

This phase is complete when:
1. All seven tasks have passing acceptance criteria
2. All 16 new measurements are registered: `accN`, `accE`, `wcVel`, `course`, `courseRate`, `glideRatio`, `diveAngle`, `diveAngleRate`, `accH`, `wcVelH`, `accAlongTrack`, `accCrossTrack`, `lift`, `drag`, `specificEnergy`, `specificEnergyRate`
3. The project compiles without errors (`cmake --build build --config Release`)
4. All new measurements produce valid output for sessions with complete GNSS data
5. Code follows the existing `registerCalculatedMeasurement` pattern in `gnsscalculations.cpp`
6. No TODOs or placeholder code remains
