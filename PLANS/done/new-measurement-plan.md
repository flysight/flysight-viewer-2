# Feature Specification: New GNSS-Derived Plots

## Overview

Add 12+ new GNSS-derived plots to the FlySight viewer, covering course, glide ratio, dive angle, accelerations, aerodynamic coefficients, and specific energy. This also includes a "Set Course" tool (analogous to "Set Ground"), an "Aerodynamics" preferences page, and a reorganization of the GNSS plot category.

## New Calculated Measurements

All new measurements use sensorID `"GNSS"`. Where derivatives are needed, use the same forward/centered/backward difference method currently used for vertical acceleration (`accD`), but factored out into a shared helper function so all derivative calculations use a single implementation.

### Intermediate Measurements (not plotted, needed by other calculations)

| measurementID | Formula | Dependencies |
|---------------|---------|-------------|
| `accN` | d(velN)/dt | velN, time |
| `accE` | d(velE)/dt | velE, time |
| `wcVel` | `sqrt((velN-windN)² + (velE-windE)² + velD²)` | velN, velE, velD, windN attr, windE attr |

### GNSS (Basic) Measurements

| measurementID | Display Name | Formula | Unit Type | Dependencies |
|---------------|-------------|---------|-----------|-------------|
| `course` | Course | Continuous (unwrapped) `atan2(velE, velN)` in degrees, minus courseRef attribute | `angle` | velN, velE, courseRef attr |
| `courseRate` | Course rate | d(course)/dt | `rotation` | course, time |
| `glideRatio` | Glide ratio | `velH / velD` (NaN when velD ≈ 0) | `ratio` (new, dimensionless, 1 dp) | velH, velD |
| `diveAngle` | Dive angle | `atan2(velD, velH)` in degrees (0° = horizontal, 90° = straight down) | `angle` | velH, velD |
| `diveAngleRate` | Dive angle rate | d(diveAngle)/dt | `rotation` | diveAngle, time |

**Course unwrapping**: Compute `atan2(velE[i], velN[i])` in degrees for each sample to get a raw heading. Then unwrap: for each consecutive pair, compute the delta; if the delta exceeds +180°, subtract 360°; if it's below -180°, add 360°. Accumulate the corrected deltas starting from the first raw value to produce a continuous signal (e.g., 359→360→361, not 359→0). The courseRef attribute is subtracted after unwrapping.

**Glide ratio sign**: Positive when falling (velD > 0 in GNSS convention), negative when rising.

### GNSS (Advanced) Measurements

| measurementID | Display Name | Formula | Unit Type | Dependencies |
|---------------|-------------|---------|-----------|-------------|
| `accH` | Horizontal acceleration | d(velH)/dt | `acceleration` | velH, time |
| `wcVelH` | Wind-corrected horizontal speed | `sqrt((velN-windN)² + (velE-windE)²)` | `speed` | velN, velE, windN attr, windE attr |
| `accAlongTrack` | Along-track acceleration | See below | `acceleration` | accN, accE, accD, velN, velE, velD, windN attr, windE attr |
| `accCrossTrack` | Cross-track acceleration | See below | `acceleration` | (same as accAlongTrack) |
| `lift` | Lift coefficient | See below | `coefficient` (new, dimensionless, 3 dp) | accCrossTrack, wcVel, hMSL; reads mass & area from prefs |
| `drag` | Drag coefficient | See below | `coefficient` | accAlongTrack, wcVel, hMSL; reads mass & area from prefs |
| `specificEnergy` | Specific energy | `0.5 * vel² + g * z` (J/kg) | `specific_energy` (new, display as kJ/kg, scale 0.001, 1 dp) | vel, z |
| `specificEnergyRate` | Specific energy rate | d(specificEnergy)/dt | `specific_power` (new, W/kg, 1 dp) | specificEnergy, time |

**Along-track / cross-track acceleration**: Uses the wind-corrected velocity vector `wc = (velN-windN, velE-windE, velD)` as the reference direction. The acceleration vector is `a = (accN, accE, accD)`. Along-track = `dot(a, normalize(wc))`, negated so that deceleration (drag) gives positive values. Cross-track = `sqrt(|a|² - alongTrack²)`, always non-negative.

**Lift and drag coefficients**:
- `CL = 2 * m * accCrossTrack / (ρ * μ² * A)`
- `CD = 2 * m * accAlongTrack / (ρ * μ² * A)`
- Where `m` = mass (from preferences), `A` = planform area (from preferences), `μ` = wcVel (wind-corrected total speed), `ρ` = air density from ISA standard atmosphere model at the measured hMSL altitude.

**ISA density model**: Implement a four-layer International Standard Atmosphere model to support altitudes up to ~150,000 ft:
- Troposphere: 0–11 km, lapse rate -6.5 K/km
- Tropopause: 11–20 km, isothermal at 216.65 K
- Stratosphere 1: 20–32 km, lapse rate +1.0 K/km
- Stratosphere 2: 32–47 km, lapse rate +2.8 K/km
- Constants: T₀=288.15 K, P₀=101325 Pa, g=9.80665 m/s², R=287.058 J/(kg·K)

**Existing accD**: Refactor the existing vertical acceleration calculation to use the shared derivative helper.

## New Calculated Attributes

| Session Key | Display Name | Default Value | Notes |
|-------------|-------------|---------------|-------|
| `_WIND_N` | Wind North | `0.0` | Placeholder; will be calculated per-track in future |
| `_WIND_E` | Wind East | `0.0` | Placeholder; will be calculated per-track in future |
| `_COURSE_REF` | Course Reference | `0.0` | Set via "Set Course" tool; subtracted from course measurement |

Register `_COURSE_REF` in the attribute display registry (category "Location", editable, angle type) so it appears in the logbook.

## Plot Selection Dock Reorganization

Rename the existing `"GNSS"` category to `"GNSS (Basic)"` and move `accD` (Vertical acceleration) to a new `"GNSS (Advanced)"` category.

### GNSS (Basic) — plot order:
1. Elevation
2. Horizontal speed
3. Vertical speed
4. Total speed
5. Course
6. Course rate
7. Glide ratio
8. Dive angle
9. Dive angle rate
10. Horizontal accuracy
11. Vertical accuracy
12. Speed accuracy
13. Number of satellites

### GNSS (Advanced) — plot order:
1. Horizontal acceleration
2. Vertical acceleration (moved from Basic)
3. Wind-corrected horizontal speed
4. Along-track acceleration
5. Cross-track acceleration
6. Lift coefficient
7. Drag coefficient
8. Specific energy
9. Specific energy rate

## Plots Menu

Update the Plots menu to this layout (hotkeys in parentheses):

```
Independent Variable (submenu, unchanged)
---
Elevation (E)
---
Horizontal Speed (H)
Vertical Speed (V)
Total Speed (S)
---
Course (C)
Course Rate (Shift+C)
---
Glide Ratio (G)
Dive Angle (A)
Dive Angle Rate (Shift+A)
---
Horizontal Accuracy (Shift+H)
Vertical Accuracy (Shift+V)
Speed Accuracy (Shift+S)
---
Number of Satellites (Shift+N)
---
Lift Coefficient (L)
Drag Coefficient (D)
```

Existing hotkeys (E, H, V, S, Shift+H, Shift+V, Shift+S, Shift+N) are unchanged.

## Default Plot Colors

Use the original FlySight Viewer default colors where a plot existed in the original viewer:

| Plot | Default Color |
|------|--------------|
| Course | `Qt::cyan` |
| Course rate | `Qt::darkCyan` |
| Glide ratio | `Qt::darkCyan` |
| Dive angle | `Qt::magenta` |
| Dive angle rate | `Qt::darkYellow` |
| Specific energy | `Qt::darkGreen` |
| Specific energy rate | `Qt::darkBlue` |
| Lift coefficient | `Qt::darkGreen` |
| Drag coefficient | `Qt::darkBlue` |

For plots without originals (horizontal acceleration, wind-corrected horizontal speed, along-track acceleration, cross-track acceleration), choose visually distinct colors following the existing palette conventions.

Reference for original colors: `TEMP/plotvalue.h`

## Set Course Tool

Add a "Set Course" tool following the exact pattern of the existing "Set Ground" tool:

- **Menu**: "Set C**o**urse" in Tools menu, hotkey `O`, checkable, in the tool action group
- **Behavior**: On click, interpolate the raw course measurement at the cursor position for each traced session. Store the result as the `_COURSE_REF` attribute. Revert to the primary tool after click.
- **Marker**: Register a "Course" marker — category "Reference", short label "Crs", editable, default enabled. Use the existing marker registration pattern.
- **Tool enum**: Add `SetCourse` to the PlotWidget Tool enum and wire it up like SetGround.

## Aerodynamics Preferences Page

Add a new "Aerodynamics" page to the Preferences dialog (after the existing Logbook page), following the pattern of existing settings pages (e.g., ImportSettingsPage).

### Preference Keys and Defaults

| Key | Display Name | Default | Range | Precision |
|-----|-------------|---------|-------|-----------|
| `aero/mass` | Mass | 1.0 kg | 0.1–500 | 1 dp |
| `aero/area` | Planform area | 1.0 m² | 0.01–100 | 2 dp |

Default of 1.0 for both gives normalized (per-unit) figures. Users can enter actual values for true aerodynamic coefficients.

The lift and drag coefficient calculations read these preferences at compute time (not registration time).

## New Unit Types

| Type constant | Key string | Display | Scale | Precision |
|---------------|-----------|---------|-------|-----------|
| `Ratio` | `"ratio"` | (dimensionless) | 1.0 | 1 |
| `SpecificEnergy` | `"specific_energy"` | kJ/kg | 0.001 | 1 |
| `SpecificPower` | `"specific_power"` | W/kg | 1.0 | 1 |
| `Coefficient` | `"coefficient"` | (dimensionless) | 1.0 | 3 |

All four are the same in both Metric and Imperial systems.
