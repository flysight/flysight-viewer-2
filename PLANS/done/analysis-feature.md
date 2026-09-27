# Analysis Dock Feature Specification

## Overview

The Analysis Dock is a new dock widget that applies scoring and analysis methods to individual sessions. It takes a set of parameters plus session measurement data and produces a set of results. All parameters and results are session attributes (calculated or stored), so existing infrastructure — logbook columns, plots, markers — works automatically.

The first analysis method to implement is **Wingsuit Performance (WS-P)**.

## Core Concepts

### Analysis as a Lens

The analysis method is an operating mode of the dock, not a property of the session. The same track can be analyzed as Wingsuit Performance, Canopy Piloting, or any other method by switching the dropdown. The dock controls which method's parameters and results are displayed; a session can have stored parameters for multiple methods simultaneously without conflict (they occupy separate attribute namespaces).

### Attribute Lifecycle

Parameters and results follow this lifecycle:

1. **Default parameters are calculated attributes.** Every session has calculated default parameters for every registered analysis method (e.g., `_WS-P_TOP_ALT` defaults to 2500 m). These are ephemeral — never persisted, computed on demand.

2. **Results are always calculated attributes.** Given parameters (whether default or stored) and measurement data, results are deterministic. Results are never stored — they are always recomputed. Including an analysis version in the parameters guarantees determinism across software updates.

3. **Editing a parameter writes a stored attribute immediately.** When the user changes a parameter in the dock, it is written as a stored attribute on the session right away. The stored value shadows the calculated default. The dependency system invalidates and recalculates results automatically. There is no intermediate dock-local state and no Save button for parameters.

4. **"Reset to Defaults" removes stored parameters.** The dock provides a Reset to Defaults action that deletes stored parameter attributes for the current method. The session falls back to calculated defaults and results recalculate accordingly.

### No Save Button; Immediate Writes

The dock is an attribute editor. Changing a parameter writes it to the session immediately — the same as dragging a marker or editing a logbook cell. This keeps the data flow simple: everything goes through the attribute system, the dependency system handles invalidation, and there is no hidden dock-local state that could diverge from the session's actual attributes.

A session that has never been edited shows calculated defaults and calculated results. The moment the user changes any parameter, that parameter becomes a stored attribute. The dock should visually indicate which parameters are at their defaults vs. user-set (e.g., a subtle reset icon next to modified fields).

If a session has any stored (user-set) parameters for the current method, the dock should make this visible — e.g., the Reset to Defaults button becomes enabled, or a subtle indicator appears. This helps the user understand at a glance whether they're looking at default or customized analysis.

### Focused Session Binding

The dock displays data for the **focused session** — the current item in the logbook's QTreeView (the one with the focus rectangle, navigable via arrow keys). This is a new concept to add to SessionModel alongside the existing `hoveredSessionId`. A `focusedSessionId` with a `focusedSessionChanged` signal should be added, driven by `QItemSelectionModel::currentChanged` on the logbook's tree view.

This allows the user to click a session in the logbook and arrow through subsequent sessions, with the analysis dock updating for each one.

The analysis dock is a single-session tool. Multi-session aggregation (averages, scoreboards) is handled by other features consuming the attributes the analysis produces (logbook columns, cross-plots, legend, etc.).

## Architecture

### Dock Structure

```text
AnalysisDockFeature (DockFeature subclass)
  └─ AnalysisDockWidget
       ├─ Method selector (QComboBox at top)
       ├─ QStackedWidget (one page per method)
       │    ├─ WingsuitPerformanceWidget
       │    └─ (future methods)
       └─ Reset to Defaults button
```

Each method widget is a hand-coded panel (not generated from a template). This allows each method to have arbitrarily complex UI tailored to its domain. The dock shell handles the method selector, reset action, and focused session tracking.

### Integration Points

- **Calculated attributes**: Each method registers its default parameters and result calculations via `SessionData::registerCalculatedAttribute`.
- **Markers**: Each method registers markers for key points (e.g., window entry/exit) via MarkerRegistry. These markers appear on plots automatically.
- **Logbook columns**: Analysis results are available as session attributes and can be added as logbook columns by the user.
- **Dependency system**: Results depend on parameters; parameters depend on measurement data. Standard invalidation/recalculation applies.
- **AppContext**: The dock receives shared services through AppContext. A `focusedSessionId` accessor on SessionModel (or equivalent) will be needed.

### Self-Contained Analysis Methods

Each analysis method should act as a self-contained module. All of the method's attribute registrations (default parameters, calculated results), marker registrations, and UI code should be localized together. A developer working on a method should not need to search the codebase for scattered registrations. The method's initialization function (called at startup) should handle all registration in one place.

### Attribute Naming Convention

Analysis attributes should be namespaced by method. The planning agent should establish a clear, consistent convention. For reference, existing session keys use uppercase with underscores and a leading underscore for calculated values (e.g., `_EXIT_TIME`, `_GROUND_ELEV`).

## Wingsuit Performance (WS-P) Method

### Background

In FAI Wingsuit Performance competitions, a jump is scored on the performance between two altitude gates (a "competition window"). The default window is 2500 m to 1500 m above ground level (AGL). These altitudes can be adjusted (e.g., lowered due to weather). Three tasks are scored in separate rounds: Time (longest time through the window), Distance (greatest horizontal distance), and Speed (highest average horizontal speed = distance / time).

### Parameters

All parameters are session attributes (calculated defaults, overridable by stored values):

| Parameter | Description | Default |
| --------- | ----------- | ------- |
| Analysis version | Identifies the scoring algorithm version | Current version string |
| Top of window | Upper altitude gate, in meters AGL | 2500 |
| Bottom of window | Lower altitude gate, in meters AGL | 1500 |
| Task | Which task is being scored: Time, Distance, or Speed | Time |
| Round | Round number (integer, for organizational/logbook use) | 1 |

AGL is computed relative to the existing `_GROUND_ELEV` attribute.

**"Restore FAI Defaults" button:** Resets only the top and bottom altitude parameters to the FAI standard values (2500 m and 1500 m AGL) for the current analysis version. Does not affect task, round, or other parameters.

### Scoring Algorithm

Starting from `_EXIT_TIME` and working forward through the GNSS altitude data (converted to AGL using `_GROUND_ELEV`):

1. Find the first time the track crosses below the **top of window** altitude. This defines the **window entry point**. Interpolate to find the exact time and position (lat/lon) at the gate altitude.

2. Continuing forward, find the first time the track crosses below the **bottom of window** altitude. This defines the **window exit point**. Interpolate to find the exact time and position.

3. If the track does not cross both boundaries, there is no valid result — no time, distance, or speed results are produced. The dock should display an appropriate incomplete state.

The existing codebase uses interpolation for similar altitude-crossing calculations (e.g., exit detection). The planning agent should follow established patterns.

### Calculated Results

All results are calculated attributes, derived deterministically from parameters + GNSS measurement data:

| Result | Description |
| ------ | ----------- |
| Window entry time | Time (UTC seconds) at which the track crosses the top altitude gate |
| Window exit time | Time at which the track crosses the bottom altitude gate |
| Window entry position | Lat/lon at window entry (interpolated) |
| Window exit position | Lat/lon at window exit (interpolated) |
| Time result | Duration through the window (exit time minus entry time, in seconds) |
| Distance result | Geodesic distance on WGS84 between entry and exit positions, calculated using GeographicLib's inverse solution method (meters) |
| Speed result | Average horizontal speed through the window (distance / time) |
| Task score | The result corresponding to the selected task parameter |

The time, distance, and speed results should be registered in the attribute registry so they can be shown as logbook columns.

### Markers

WS-P should register two markers:

- **Window Top**: at the window entry time, showing where the track crosses the upper gate
- **Window Bottom**: at the window exit time, showing where the track crosses the lower gate

These should appear on plots as vertical lines (following existing marker patterns). They should be visible when the session has WS-P parameters.

### UI Layout

The WingsuitPerformanceWidget should include:

- **Top of window** and **bottom of window** altitude fields, in meters (not unit-aware — WS-P altitudes are always in meters per FAI rules).
- **"Restore FAI Defaults" button**: Resets top and bottom altitudes to 2500 m and 1500 m. Grayed out when the altitude parameters are already equal to the FAI default values; enabled otherwise.
- **Round number** field.
- **Results grid**: 3 rows, 2 columns. The first column contains radio buttons for selecting the task (Time, Distance, Speed). The second column displays the calculated result for each task:
  - Time: displayed in seconds with one decimal place (e.g., "32.4 s")
  - Distance: displayed in meters with no decimal places (e.g., "1523 m")
  - Speed: displayed in km/h with one decimal place (e.g., "169.2 km/h")

The radio buttons select the task parameter (which task this round is scored on). All three results are always shown regardless of which task is selected.

The planning agent has latitude in other layout details (spacing, grouping, widget types for the altitude/round fields, etc.).

## Future Considerations (Do Not Implement Now)

These are documented for architectural awareness only. The planning agent should ensure the design does not preclude these, but should not build infrastructure for them:

- **Additional analysis methods**: Speed Skydiving, Canopy Piloting, etc. — each would be a new widget in the stacked layout with its own parameters, results, and markers.
- **Map layers**: A separate feature for named reference geometry (DZ definitions, competition reference points). Will eventually feed into scoring as lane reference points.
- **Lane scoring**: Uses a ground reference point (from a map layer) and a calculated freefall reference point to define a lane. Penalty assignment is manual. Depends on the map layers feature.
- **Competition workflow**: Bulk parameter application via logbook selection, official/freeze indicators, scoreboard views derived from logbook data.
- **SEP (Spherical Error Probable)**: A GPS accuracy metric required by FAI (must be < 10 m). Should be added to WS-P results once the calculation is defined and FlySight data fields for accuracy are confirmed.
- **Method visibility preferences**: If the method list grows large, a preference to show/hide methods in the dropdown, potentially tied to a profile system.
