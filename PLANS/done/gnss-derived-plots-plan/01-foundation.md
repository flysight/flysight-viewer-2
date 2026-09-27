# Phase 1: Foundation

## Overview

This phase adds the foundational building blocks required by all subsequent phases: four new unit types, a shared derivative helper function, an ISA density model, three new session keys with their default attributes, two new aerodynamics preference keys, and the refactoring of the existing `accD` calculation to use the new shared derivative helper. Once complete, Phase 2 can register all new calculated measurements against these primitives.

## Dependencies

- **Depends on:** None -- can begin immediately
- **Blocks:** Phase 2 (Measurements)
- **Assumptions:** The existing codebase compiles and passes all tests on the current `master` branch before work begins.

## Tasks

### Task 1.1: Add New Unit Types

**Purpose:** Phase 2 measurements need four new unit types (Ratio, SpecificEnergy, SpecificPower, Coefficient) that do not currently exist in the unit system.

**Files to modify:**
- `src/units/unitdefinitions.h` -- Add four new type constants to `MeasurementTypes` namespace and four new entries to `getMeasurementTypeRegistry()`

**Technical Approach:**

1. In the `MeasurementTypes` namespace (lines 33-58), add four new constants after the existing `SpSpeedAcc` entry:

```cpp
inline const QString Ratio = QStringLiteral("ratio");
inline const QString SpecificEnergy = QStringLiteral("specific_energy");
inline const QString SpecificPower = QStringLiteral("specific_power");
inline const QString Coefficient = QStringLiteral("coefficient");
```

2. In `getMeasurementTypeRegistry()` (after the `SpSpeedAcc` entry around line 257), add four new registry entries. All four use identical Metric and Imperial specs (they are unit-system-independent):

| Type | SI Base Unit | Label (both systems) | Scale | Offset | Precision |
|------|-------------|---------------------|-------|--------|-----------|
| Ratio | (empty) | (empty) | 1.0 | 0.0 | 1 |
| SpecificEnergy | `"J/kg"` | `"kJ/kg"` | 0.001 | 0.0 | 1 |
| SpecificPower | `"W/kg"` | `"W/kg"` | 1.0 | 0.0 | 1 |
| Coefficient | (empty) | (empty) | 1.0 | 0.0 | 3 |

Follow the exact pattern used for `Count` (lines 197-204) for dimensionless types (Ratio, Coefficient) and the pattern used for `Rotation` (lines 142-149) for types with a label (SpecificEnergy, SpecificPower).

**Acceptance Criteria:**
- [ ] `MeasurementTypes::Ratio`, `MeasurementTypes::SpecificEnergy`, `MeasurementTypes::SpecificPower`, and `MeasurementTypes::Coefficient` are defined as `inline const QString` in the `MeasurementTypes` namespace
- [ ] Each new type has an entry in `getMeasurementTypeRegistry()` with both Metric and Imperial specs
- [ ] Ratio: empty label, scale 1.0, precision 1
- [ ] SpecificEnergy: label "kJ/kg", scale 0.001, precision 1
- [ ] SpecificPower: label "W/kg", scale 1.0, precision 1
- [ ] Coefficient: empty label, scale 1.0, precision 3
- [ ] Project compiles without errors

**Complexity:** S

---

### Task 1.2: Create Shared Derivative Helper

**Purpose:** Multiple new measurements (courseRate, diveAngleRate, accH, accN, accE, specificEnergyRate) require computing derivatives using the forward/centered/backward difference method. Currently this logic is duplicated inline in the `accD` calculation. A shared helper eliminates duplication and ensures consistency.

**Files to create:**
- `src/calculations/derivativehelper.h` -- Header declaring the helper function
- `src/calculations/derivativehelper.cpp` -- Implementation

**Files to modify:**
- `src/CMakeLists.txt` -- Add new source files to the build

**Technical Approach:**

1. Create `src/calculations/derivativehelper.h`:

```cpp
#ifndef DERIVATIVEHELPER_H
#define DERIVATIVEHELPER_H

#include <QVector>
#include <optional>

namespace FlySight {
namespace Calculations {

/**
 * @brief Compute the numerical derivative of values with respect to time.
 *
 * Uses forward difference for the first sample, centered difference for
 * interior samples, and backward difference for the last sample.
 *
 * @param values The dependent variable (e.g., velocity).
 * @param times  The independent variable (must be same size as values).
 * @return The derivative vector, or std::nullopt if input is invalid
 *         (empty, size mismatch, fewer than 2 points, or zero time gaps).
 */
std::optional<QVector<double>> computeDerivative(
    const QVector<double>& values,
    const QVector<double>& times);

} // namespace Calculations
} // namespace FlySight

#endif // DERIVATIVEHELPER_H
```

2. Create `src/calculations/derivativehelper.cpp` extracting the algorithm from `gnsscalculations.cpp` lines 106-167. The implementation must:
   - Return `std::nullopt` if `values` is empty, sizes mismatch, or fewer than 2 points
   - Use forward difference for index 0: `(v[1] - v[0]) / (t[1] - t[0])`
   - Use centered difference for indices 1..N-2: `(v[i+1] - v[i-1]) / (t[i+1] - t[i-1])`
   - Use backward difference for index N-1: `(v[N-1] - v[N-2]) / (t[N-1] - t[N-2])`
   - Return `std::nullopt` if any `dt == 0.0` (with a `qWarning`)
   - Include `<QVector>`, `<optional>`, and `<QDebug>` (for qWarning)

3. Add to `src/CMakeLists.txt` in the source file list, near the other `calculations/` entries (around line 336):
   ```
   calculations/derivativehelper.cpp            calculations/derivativehelper.h
   ```

**Acceptance Criteria:**
- [ ] `src/calculations/derivativehelper.h` declares `FlySight::Calculations::computeDerivative(const QVector<double>&, const QVector<double>&)` returning `std::optional<QVector<double>>`
- [ ] `src/calculations/derivativehelper.cpp` implements the forward/centered/backward difference algorithm matching the existing `accD` logic
- [ ] Returns `std::nullopt` for empty input, size mismatch, fewer than 2 points, or zero time deltas
- [ ] Files are added to `src/CMakeLists.txt`
- [ ] Project compiles without errors

**Complexity:** M

---

### Task 1.3: Refactor accD to Use Shared Derivative Helper

**Purpose:** Validate the new derivative helper by replacing the inline `accD` calculation with a call to it, eliminating code duplication.

**Files to modify:**
- `src/calculations/gnsscalculations.cpp` -- Replace lines 99-168 with a call to `computeDerivative`

**Technical Approach:**

1. Add `#include "derivativehelper.h"` to the includes at the top of `gnsscalculations.cpp`.

2. Replace the `accD` registration lambda (lines 106-167) with a simplified version:

```cpp
[](SessionData& session) -> std::optional<QVector<double>> {
    QVector<double> velD = session.getMeasurement("GNSS", "velD");
    QVector<double> time = session.getMeasurement("GNSS", "time");

    if (velD.isEmpty()) {
        qWarning() << "Cannot calculate accD due to missing velD";
        return std::nullopt;
    }

    return Calculations::computeDerivative(velD, time);
}
```

The dependency list and registration call remain identical.

**Acceptance Criteria:**
- [ ] The `accD` lambda calls `Calculations::computeDerivative(velD, time)` instead of implementing the algorithm inline
- [ ] The `accD` registration's dependency list is unchanged (`velD`, `time`)
- [ ] `accD` produces identical results as before (same algorithm, just factored out)
- [ ] Project compiles without errors

**Complexity:** S

---

### Task 1.4: Create ISA Density Model

**Purpose:** The lift and drag coefficient calculations in Phase 2 require air density as a function of altitude. A four-layer ISA model provides this.

**Files to create:**
- `src/calculations/isadensity.h` -- Header declaring the density function
- `src/calculations/isadensity.cpp` -- Implementation

**Files to modify:**
- `src/CMakeLists.txt` -- Add new source files

**Technical Approach:**

1. Create `src/calculations/isadensity.h`:

```cpp
#ifndef ISADENSITY_H
#define ISADENSITY_H

namespace FlySight {
namespace Calculations {

/**
 * @brief Compute air density using the International Standard Atmosphere model.
 *
 * Supports four layers:
 *   - Troposphere:     0-11 km,  lapse rate -6.5 K/km
 *   - Tropopause:     11-20 km,  isothermal at 216.65 K
 *   - Stratosphere 1: 20-32 km,  lapse rate +1.0 K/km
 *   - Stratosphere 2: 32-47 km,  lapse rate +2.8 K/km
 *
 * @param altitudeMSL Geometric altitude in metres above mean sea level.
 * @return Air density in kg/m^3. For altitudes outside 0-47 km, clamps to
 *         the nearest boundary layer.
 */
double isaDensity(double altitudeMSL);

} // namespace Calculations
} // namespace FlySight

#endif // ISADENSITY_H
```

2. Create `src/calculations/isadensity.cpp` implementing the four-layer model:

**Constants:**
- `T0 = 288.15` K (sea level temperature)
- `P0 = 101325.0` Pa (sea level pressure)
- `g = 9.80665` m/s^2
- `R = 287.058` J/(kg*K) (specific gas constant for dry air)

**Layer definitions (base altitude, base temperature, base pressure, lapse rate):**

| Layer | h_base (m) | T_base (K) | P_base (Pa) | Lapse (K/m) |
|-------|-----------|-----------|------------|-------------|
| Troposphere | 0 | 288.15 | 101325 | -0.0065 |
| Tropopause | 11000 | 216.65 | computed | 0.0 |
| Stratosphere 1 | 20000 | 216.65 | computed | 0.001 |
| Stratosphere 2 | 32000 | 228.65 | computed | 0.0028 |

For each layer, the base pressure is computed from the layer below at the transition altitude using the standard formulas:

- **Non-zero lapse rate** (L != 0): `P = P_base * (T / T_base) ^ (-g / (R * L))`
- **Isothermal** (L == 0): `P = P_base * exp(-g * (h - h_base) / (R * T_base))`

Density: `rho = P / (R * T)`

The function should:
- Clamp input altitude to [0, 47000] metres
- Determine which layer the altitude falls in
- Compute temperature and pressure at that altitude
- Return density = P / (R * T)

The layer base pressures can be computed at static initialization time or as `constexpr`/precomputed constants.

3. Add to `src/CMakeLists.txt`:
   ```
   calculations/isadensity.cpp                  calculations/isadensity.h
   ```

**Acceptance Criteria:**
- [ ] `isaDensity(0.0)` returns approximately 1.225 kg/m^3 (sea level standard density)
- [ ] `isaDensity(11000.0)` returns approximately 0.3639 kg/m^3
- [ ] `isaDensity(20000.0)` returns approximately 0.0880 kg/m^3
- [ ] `isaDensity(32000.0)` returns approximately 0.0132 kg/m^3 (within reasonable tolerance)
- [ ] Altitudes below 0 are clamped to 0; altitudes above 47000 are clamped to 47000
- [ ] Function is purely computational -- no dependencies on Qt or session state beyond basic types
- [ ] Files are added to `src/CMakeLists.txt`
- [ ] Project compiles without errors

**Complexity:** M

---

### Task 1.5: Add New Session Keys

**Purpose:** Phase 2 measurements depend on three new session attributes: `_WIND_N`, `_WIND_E` (placeholder wind components), and `_COURSE_REF` (course reference angle). These must be defined as session keys and registered with default values.

**Files to modify:**
- `src/sessiondata.h` -- Add three new key constants to `SessionKeys` namespace

**Technical Approach:**

Add the following constants to the `SessionKeys` namespace in `src/sessiondata.h`, grouped together with a comment (place them after the Speed Skydiving result keys, around line 77):

```cpp
// Wind and course reference keys
constexpr char WindN[] = "_WIND_N";
constexpr char WindE[] = "_WIND_E";
constexpr char CourseRef[] = "_COURSE_REF";
```

**Acceptance Criteria:**
- [ ] `SessionKeys::WindN`, `SessionKeys::WindE`, and `SessionKeys::CourseRef` are defined as `constexpr char[]` in the `SessionKeys` namespace
- [ ] Key strings are `"_WIND_N"`, `"_WIND_E"`, and `"_COURSE_REF"` respectively
- [ ] Project compiles without errors

**Complexity:** S

---

### Task 1.6: Register Course Reference Attribute

**Purpose:** `_COURSE_REF` needs to appear in the logbook UI so users can view and edit it. It must be registered with the `AttributeRegistry` following the same pattern as `GroundElev`.

**Files to modify:**
- `src/calculations/attributeregistration.cpp` -- Add registration call for `_COURSE_REF`

**Technical Approach:**

Add a new `reg.registerAttribute(...)` call in `registerBuiltInAttributes()` after the existing `GroundElev` registration (line 66). Follow the exact pattern from lines 59-66:

```cpp
reg.registerAttribute({
    QStringLiteral("Location"),
    QStringLiteral("Course Reference"),
    SessionKeys::CourseRef,
    AttributeFormatType::Double,
    true,
    MeasurementTypes::Angle
});
```

Key details:
- Category: `"Location"` (same as Ground Elevation)
- Format type: `Double` (numeric display with unit conversion)
- Editable: `true` (user can modify via logbook)
- Measurement type: `Angle` (uses the existing angle unit type for display)

**Acceptance Criteria:**
- [ ] `_COURSE_REF` appears in the attribute registry under category "Location"
- [ ] It is editable (`editable = true`)
- [ ] It uses `MeasurementTypes::Angle` for unit conversion
- [ ] It uses `AttributeFormatType::Double` for formatting
- [ ] Its display name is "Course Reference"
- [ ] Project compiles without errors

**Complexity:** S

---

### Task 1.7: Add Aerodynamics Preference Keys and Defaults

**Purpose:** The lift and drag coefficient calculations in Phase 2 need to read `aero/mass` and `aero/area` from preferences. The keys must be defined and registered with default values.

**Files to modify:**
- `src/preferences/preferencekeys.h` -- Add two new preference key constants
- `src/mainwindow.cpp` -- Register default values in `initializePreferences()`

**Technical Approach:**

1. In `src/preferences/preferencekeys.h`, add a new section after the Analysis Preferences section (before the closing `}` of the namespace, around line 138):

```cpp
// ============================================================================
// Aerodynamics Preferences
// ============================================================================
inline const QString AeroMass = QStringLiteral("aero/mass");
inline const QString AeroArea = QStringLiteral("aero/area");
```

2. In `src/mainwindow.cpp` `initializePreferences()`, add a new section after the Analysis Preferences registration (after line 1082):

```cpp
// ========================================================================
// Aerodynamics Preferences
// ========================================================================
prefs.registerPreference(PreferenceKeys::AeroMass, 1.0);
prefs.registerPreference(PreferenceKeys::AeroArea, 1.0);
```

**Acceptance Criteria:**
- [ ] `PreferenceKeys::AeroMass` is defined as `"aero/mass"`
- [ ] `PreferenceKeys::AeroArea` is defined as `"aero/area"`
- [ ] Default value for `AeroMass` is `1.0` (kg)
- [ ] Default value for `AeroArea` is `1.0` (m^2)
- [ ] Defaults are registered in `initializePreferences()` via `prefs.registerPreference()`
- [ ] Project compiles without errors

**Complexity:** S

---

## Testing Requirements

### Unit Tests

- No formal unit test framework appears to be in use in this project. If one exists, add tests for:
  - `computeDerivative()` with known input/output pairs (e.g., linear ramp should give constant derivative)
  - `computeDerivative()` edge cases: empty input, single point, zero time delta
  - `isaDensity()` at layer boundaries (0, 11000, 20000, 32000 m) against known ISA table values
  - `isaDensity()` clamping behavior for negative and >47000 m altitudes

### Integration Tests

- After Task 1.3, verify that the `accD` calculated measurement produces the same values as before the refactor by loading a known dataset and comparing output.

### Manual Verification

1. Build the project: `cmake --build build --config Release`
2. Launch the application and open an existing logbook with GNSS data
3. Verify "Course Reference" appears in the logbook under the "Location" category
4. Verify "Course Reference" is editable and accepts numeric input
5. Open any GNSS plot that shows vertical acceleration (`accD`) and confirm it still renders correctly (regression check for Task 1.3)

## Notes for Implementer

### Gotchas

- **ISA base pressures:** The base pressure for each layer above the troposphere must be computed from the layer below at the transition altitude. Pre-compute these values as constants (they are fixed by the ISA standard) to avoid runtime overhead and floating-point drift from repeated computation. Known values: P at 11 km = 22632.1 Pa, P at 20 km = 5474.89 Pa, P at 32 km = 868.019 Pa.
- **Derivative helper return type:** Use `std::optional<QVector<double>>` to match the existing pattern of calculated measurement lambdas. Do not use exceptions.
- **Empty labels for dimensionless units:** For Ratio and Coefficient, use `QString()` (empty string) for both the `siBaseUnit` and the `label` field in `UnitSpec`, matching the pattern used by `Count`.
- **CMakeLists.txt formatting:** The existing file uses aligned columns with spaces between `.cpp` and `.h` filenames. Match this formatting.
- **Include guard naming:** Use `DERIVATIVEHELPER_H` and `ISADENSITY_H` to match the project convention of uppercase filename with `_H` suffix.

### Decisions Made

- **Wind attributes not registered in AttributeRegistry:** `_WIND_N` and `_WIND_E` are placeholder session keys defaulting to 0.0. They are not registered in the `AttributeRegistry` (and thus won't appear in the logbook) because the overview describes them as placeholders for future per-track calculation. Only `_COURSE_REF` is registered for display.
- **Wind/CourseRef default values:** These keys default to `0.0` implicitly -- when a session's `getAttribute()` returns an invalid QVariant, the consuming measurement lambdas (in Phase 2) will use `toDouble(&ok)` and treat missing values as a computation failure. However, since these are session attributes (not preferences), they do not need explicit default registration in `initializePreferences()`. They will be set to `0.0` on sessions that need them, either at import time or by Phase 2/3 logic.
- **ISA clamping behavior:** Altitudes below 0 m clamp to sea-level density; altitudes above 47000 m clamp to the top of Stratosphere 2. This prevents NaN/undefined behavior for edge-case altitude data.

### Open Questions

- None. All architectural decisions are specified in the overview document.

## Definition of Done

This phase is complete when:
1. All seven tasks have passing acceptance criteria
2. The project compiles without errors (`cmake --build build --config Release`)
3. The existing `accD` measurement produces identical results after refactoring
4. New files follow the naming and structural patterns of existing code
5. No TODOs or placeholder code remains
