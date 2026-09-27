# Feature Document vs. Current Implementation

Comparison of the vision in `PLANS/2026-02-24-features.md` against the current state of the codebase as of 2026-04-02.

## Sessions & Data Model
**Status: Fully Implemented**

The session model matches the vision closely. `SessionData` supports attributes (scalars), sensors (grouped measurements sharing a timebase), and measurements (per-sample vectors), all identified by string keys exactly as described. Calculated attributes and measurements are implemented with lazy evaluation and dependency tracking via `CalculatedValueRegistry` and `DependencyManager`.

## Calculations
**Status: Mostly Implemented**

- 12+ built-in calculation modules (GNSS, baro, IMU, WSP scoring, speed skydiving, etc.)
- Lazy evaluation with dependency management is working
- **Not yet implemented**: The "calculation dock" for long-running calculations with progress bars, stop buttons, and play-to-rerun UI. The current `IdleScheduler` handles background work but isn't user-facing in the way the document envisions.
- **Not yet implemented**: Multi-output calculations are mentioned as a limitation in the document -- unclear if this has been resolved.

## Centralized Definition of Plots, Calculations, etc.
**Status: Implemented**

`PlotRegistry`, `MarkerRegistry`, `CalculatedValueRegistry`, and the plugin system all follow the pattern of central registration with separate implementation.

## Plots & Plot Dock
**Status: Fully Implemented**

QCustomPlot-based plotting with multi-session display, color coding, plot enable/disable via the Plot Selection dock, keyboard shortcuts, and menu access. The Shift (multi-session cursor) and Ctrl (lock to session) modifiers described in the document are implemented in `CrosshairManager`.

The document notes these modifiers aren't yet in other docks (map, cross-plots) -- this is **still the case**.

## Unit Conversion
**Status: Fully Implemented**

`UnitConverter` with measurement types (so altitude vs. distance can use different units even though both are stored in meters), metric/imperial support, and an architecture ready for additional unit systems -- exactly as described.

## Markers
**Status: Mostly Implemented**

- Marker registry, selection dock, plot bubble annotations -- all working
- Draggable markers (user-editable position) -- implemented
- Plugin-defined markers -- implemented
- **Not yet implemented**: "Time from marker" x-axis mode where double-clicking a marker aligns all sessions at that marker. The document describes this as a key enhancement to the current "Time from exit" mode.
- **Not yet implemented**: The marker creation UI for defining markers by rules (plot equals reference value, min/max, time window relative to other markers). The document suggests a simple built-in UI plus Python for complex cases.
- **Partially addressed**: The document's musing about unifying "time of event" markers with "value at event" attributes -- the `MomentModel` system and the logbook's `MeasurementAtMarker` column type suggest progress here.

## Scoring & Analysis
**Status: Implemented**

Two competition scoring systems are fully built out:
- **Wingsuit Performance (WSP)** -- with altitude window, task selection (time/distance/speed), FAI defaults
- **Speed Skydiving (SP)** -- with performance/validation windows, breakoff altitude

The Analysis dock with method selector and stacked widgets matches the vision of scoring as "just another calculation."

## Logbook
**Status: Mostly Implemented**

- Session rows with attribute columns, visibility checkboxes -- working
- Configurable columns (session attributes, measurement-at-marker, deltas) -- working
- Persistent storage in a logbook directory with UUID-based CSV files and `index.json` -- working
- Cached column values for quick startup -- working
- **Not yet implemented**: Scatter plots from logbook columns (e.g., max vertical speed vs. max horizontal speed). The document describes this as a long-term analysis feature.
- **Not yet implemented**: Export/import of logbook data in user-friendly formats (JSON mentioned as an option).
- The document emphasizes robustness against corruption and easy backup -- the current file-based approach (CSV + JSON index) is reasonably robust but there's no explicit backup mechanism.

## Cross-Plots
**Status: Not Implemented**

This is one of the bigger missing features. The document envisions:
- Measurement vs. measurement line charts for visible sessions
- Attribute vs. attribute scatter charts
- Specific examples: **drag polar**, **speed polar** (vertical vs. horizontal speed), **wind circle** (2D velocity polar plot with circle fitting)
- Analysis overlays on cross-plots (max L/D lines, wind speed calculation)

None of these appear to exist in the codebase.

## Video Synchronization
**Status: Mostly Implemented**

- Video playback with frame stepping -- working
- Sync marker for aligning video to GPS time -- working (`SetSyncTool`)
- Video cursor in plot dock -- working
- Per-session video selection -- working
- Drag-and-drop video loading -- working
- **Not yet implemented**: The document's vision of the sync marker being draggable to scrub video position, or the video cursor always being drawn as a dashed line coinciding with the sync marker tip.

## Sharing Between Docks
**Status: Implemented**

The `MomentModel` / cursor system connects plot, map, legend, and video docks bidirectionally. Moving the cursor in any dock updates the others. This is one of the strongest areas of implementation.

## Map Dock
**Status: Mostly Implemented**

- Google Maps satellite view with track overlays -- working
- Cursor dot synchronization -- working
- Zoom/pan controls -- working
- **Not yet implemented**: Click-and-drag distance measurement tool in the map
- **Not yet implemented**: Map toolbar for tool mode selection (the document suggests paralleling the plot dock's tool system)
- **Not yet implemented**: Pinch gesture navigation (the document mentions this as a goal)
- The document suggests moving map type selection to preferences -- this is **done** (MapSettingsPage).

## Legend Dock
**Status: Fully Implemented**

Shows plot values at cursor position, responds to cursor from any dock (plot, video, map), can be positioned freely. Matches the vision.

## Layouts / Profiles
**Status: Implemented (as "Profiles")**

The document calls these "saved layouts." The implementation uses `ProfileManager` with `Profile` structs that store enabled plots/markers, reference marker, x-axis variable, zoom extents, logbook columns, dock layout (serialized), and analysis method selection. Save/load/manage via `ManageProfilesDialog` and a Profiles menu. This exceeds the minimum described in the document. **Not confirmed**: Whether export/import of profiles is supported.

## Embedded Python Interpreter
**Status: Implemented**

- `PluginHost` with pybind11 integration
- `AttributePlugin` for custom calculated attributes
- `MeasurementPlugin` for custom measurement arrays
- `SimplePlot` for registering new plot types
- `SimpleMarker` for custom markers
- Plugin SDK with dependency management and NumPy support

This matches the vision well. The document's desire for custom calculations, plots, and markers to be indistinguishable from built-in ones appears to be achieved.

## Preferences
**Status: Fully Implemented**

Hierarchical preference system with 9+ settings pages, QSettings persistence, and dynamic change signals. Matches the vision.

## Summary of Gaps

| Feature | Status |
|---|---|
| Cross-plots (drag polar, speed polar, wind circle) | **Not started** |
| Calculation dock (long-running calc management) | **Not started** |
| Time-from-marker x-axis alignment | **Not started** |
| Logbook scatter plots | **Not started** |
| Map distance measurement tool | **Not started** |
| Marker creation UI (rules-based) | **Not started** |
| Shift/Ctrl cursor modifiers in map dock | **Not started** |
| Draggable sync marker for video scrub | **Not started** |
| Profile export/import | **Unclear** |
| Logbook backup/export tooling | **Not started** |

The core architecture -- sessions, calculations, plots, markers, plugins, profiles, video sync, map integration, logbook persistence -- is solidly in place. The biggest gap is **cross-plots**, which the document describes as a natural extension of the existing infrastructure. The **calculation dock** for long-running computations is also unstarted but may not be needed until sensor fusion or similarly heavy calculations are added.
