# Phase 2: Startup Optimization

## Overview

With Phase 1, the app loads every session CSV on startup — fine for small
logbooks, but unacceptably slow for thousands of sessions. This phase makes
startup time independent of logbook size by populating the table from a cached
index and loading session data in the background.

**Design goal:** The application should open about as quickly with 10,000
sessions in the logbook as it does with none.

## Dependencies

- **Depends on:** Phase 1 (persistence layer) — sessions on disk, basic
  `index.json`, `LogbookManager`, `DataExporter`, `DataImporter::readFile()`.
- **Context:** `docs/logbook-overview.md` describes the overall logbook design,
  including the index, startup sequence, and the relationship between
  `m_attributes`, `m_sensors`, and calculated values.

## Concepts

### Stubs vs Loaded Sessions

On startup, the model is populated entirely from the index. Each row is a
**stub** — it has cached column values from the index but no `SessionData`
object. A stub is enough to display the table but not enough to plot, compute
measurements, or access sensor data.

When any code path needs actual session data (e.g., `sessionRef()` is called),
the session's CSV is parsed synchronously and the stub is replaced with a fully
loaded `SessionData`. This on-demand loading is invisible to callers.

### Cached Column Values

The index caches the raw `QVariant`-compatible values (doubles, strings) that
the model's `data()` method would normally compute from a loaded `SessionData`.
These are **not** pre-formatted display strings — the model applies the same
formatting logic to cached values as to live values, producing identical display
output and sort behavior.

If a column's cached value differs from the live value (e.g., after an app
update changes a calculation), the cell updates when the session finishes
background loading. This is expected and harmless.

## Design

### Index Format

The index caches per-session, per-column values keyed by column UUID and
session ID.

```json
{
  "columns": {
    "a1b2c3d4": { "type": "SessionAttribute", "key": "_DESCRIPTION", ... },
    "e5f6a7b8": { "type": "MeasurementAtMarker", ... }
  },
  "sessions": {
    "SESSION_ID_1": {
      "lastAccessed": 1710900000.0,
      "values": {
        "a1b2c3d4": "Skydive",
        "e5f6a7b8": 56.7
      }
    }
  }
}
```

**Column UUIDs:** The UUIDs in `"columns"` are ephemeral — generated fresh each
time the index is flushed, solely to connect column definitions to per-session
values within the file. They are not persisted in `LogbookColumnStore` or
anywhere else in the application. On startup, the reader matches index columns
to live columns by comparing the column definition (type + parameters), then
uses the UUID to look up the corresponding cached value for each session. If a
live column has no match in the index (e.g., newly added), its cells show blank
until sessions load. Index columns with no live match are ignored.

**Session keys:** Sessions are keyed by SESSION_ID (not file UUID), consistent
with how the rest of the application identifies sessions. The file UUID remains
internal to `LogbookManager`.

**`lastAccessed` timestamp:** Per-session UTC timestamp (seconds since epoch),
updated on import and on visibility toggle (set to true). Used to order
background loading so that recently used sessions load first.

### SessionModel Changes

The model currently stores `QVector<SessionData>`. This changes to a structure
that supports rows with or without a loaded `SessionData`:

- Each row holds a session ID, the cached column value map (from the index),
  and an optional `SessionData`.
- `data()` for `DisplayRole` / `EditRole` / `SortRole`: if the session is
  loaded, compute the live value (current logic). If it's a stub, return the
  cached value for that column UUID, formatted through the same formatting path.
- `CheckStateRole`: all sessions default to unchecked (not visible), whether
  stub or loaded.
- `sessionRef()`: if the session is a stub, synchronously load the CSV (via
  `LogbookManager`), replace the stub with the loaded `SessionData`, remove
  the session from the background queue, and return the reference. This makes
  on-demand loading transparent to all existing callers.

### Background Loader

After the index populates the table, a background loader works through the
remaining unloaded sessions:

- **Queue:** A list of session IDs ordered by `lastAccessed` descending
  (most recently used first).
- **Processing:** A `QTimer` with 0ms interval processes one session per tick.
  Between ticks, the Qt event loop handles UI events, keeping the app
  responsive.
- **On completion of each session:** The loaded `SessionData` replaces the
  stub. The model emits `dataChanged` for that row (so the table updates any
  cells whose cached values differ from live values). The model does **not**
  emit `modelChanged` — see below.
- **When the queue is empty:** Stop the timer. Flush the index to disk (all
  cached values are now fresh).
- **On-demand loads** (via `sessionRef()`): Remove the session from the queue
  so it isn't loaded twice.

### Signal Design

A new signal `sessionLoaded(QString sessionId)` is emitted when a background
load completes. This is distinct from `modelChanged`, which continues to mean
"the set of things to draw may have changed" (visibility toggle, session
added/removed, attribute edit, etc.).

Docks (PlotWidget, TrackMapModel, etc.) connect to `modelChanged` as they do
today and are not affected by background loading. If a dock needs to react to
background loads (e.g., a visible session finishes loading), it can connect to
`sessionLoaded` and check whether the session is relevant.

In practice, visible sessions are always loaded on-demand (via `sessionRef()`)
before they become visible, so `sessionLoaded` during background loading
applies only to non-visible sessions and should not trigger plot/map rebuilds.

### Index Flushing

The index is written to disk:

- On the existing idle timer / deferred save path
- On clean shutdown
- After background loading completes (all cached values are fresh)

### Error Handling

If a session's CSV is missing or corrupt when the background loader (or
on-demand load) attempts to parse it, log a warning and leave the row as a
stub with its cached values. The session remains visible in the table but
cannot be plotted or edited.

## Startup Sequence

1. **Read index:** Populate the model with stubs from cached values. The table
   is fully populated and interactive immediately.
2. **Start background loader:** Queue all sessions ordered by `lastAccessed`
   descending. Process one per event-loop tick.
3. **User interaction:** Any action that calls `sessionRef()` on a stub
   triggers synchronous loading of that session. The session is removed from
   the background queue.

## Notes for Implementer

### `modelChanged` vs `sessionLoaded`

`modelChanged` is a catch-all signal connected to by PlotWidget,
TrackMapModel, MapCursorDotModel, LegendPresenter, and VideoWidget. All of
these perform full rebuilds in response. Background loading must **not** emit
`modelChanged` — only `dataChanged` (for the table row) and `sessionLoaded`.

### Threading

Main-thread idle processing (QTimer at 0ms) is the recommended approach. Each
CSV parse is fast (~1-5ms). With 10,000 sessions this means ~50 seconds of
background loading, but the UI remains responsive throughout. A worker thread
would add complexity without meaningful benefit.

## Definition of Done

This phase is complete when:

1. The app opens with the logbook table fully populated from the index, before
   any CSV files are parsed
2. Startup time with 10,000 sessions is comparable to startup with none
3. Background loading progressively replaces cached values with live values;
   cells update if values differ
4. Making a session visible (or any action requiring session data) loads it
   on-demand with no perceptible delay
5. Plots and maps are not rebuilt during background loading of non-visible
   sessions
6. The index is flushed with fresh values after background loading completes
7. All existing functionality works correctly
8. Build passes with no errors
