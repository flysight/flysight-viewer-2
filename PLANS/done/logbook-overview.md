# Logbook Persistence & Optimization

## Overview

The logbook provides persistent storage for imported FlySight sessions. It is the
source of truth for session data -- users may import sessions from many devices over
many years, and the original files may no longer be available.

## Design Goals

1. The logbook is stored in `<Documents>/FlySight Viewer/logbook/` so it is easily
   backed up by the user.on
2. Even with a large logbook (e.g., 10,000 entries), the application should open
   about as quickly as it does with no logbook at all.

## Storage Layout

```
<Documents>/FlySight Viewer/logbook/
  index.json
  sessions/
    <uuid>.csv
    <uuid>.csv
    ...
```

### Session Files (`sessions/<uuid>.csv`)

Each session is stored as a single file in FS2 format. The file contains
`SessionData::m_attributes` as `$VAR` lines and `SessionData::m_sensors` as sensor
data sections. Together, these represent the complete user-defined ground truth for
a session: imported data, user edits, and user-overridden marker positions. Purely
calculated values (`m_calculatedAttributes`, `m_calculatedMeasurements`) are not
stored; they are recomputed on demand.

Properties:

- **FS2 format**: The same format used by FlySight hardware. This makes session
  files self-documenting, importable into any FlySight Viewer instance, and readable
  by third-party tools.
- **UUID filenames**: Generated at import time. The original `SESSION_ID` (if any)
  is preserved as a `$VAR` attribute inside the file.
- **Logically write-once**: Session files are written at import time and not modified
  during normal use. If additional data for the same session is imported later (e.g.,
  from multiple files on the FlySight), the session file is updated with the merged
  data.

### Index (`index.json`)

The index is a startup cache that allows the logbook table to be fully populated
without parsing every session file. It contains the current logbook column
definitions and, per session, all valid attributes and cached column display values.

The index is **not** a source of truth. It is a best-effort snapshot of the last
known column values. On startup, values from the index are displayed immediately;
as sessions load in the background, live values replace cached ones. If they match
(the usual case), nothing visually changes. If they differ (e.g., after an app
update that changed a calculation), cells update as sessions finish loading.

The index is flushed to disk on an idle timer and on clean shutdown. If the
application crashes, the index may be stale; it is rebuilt progressively as
sessions load on the next startup.

## Startup Sequence

1. **Read index**: The logbook table is fully populated immediately from cached
   values.
2. **Load priority sessions**: Sessions that are currently plotted or visible are
   loaded first so that plots render quickly.
3. **Background loading**: Remaining sessions are loaded in the background. If a
   user action makes a specific session's data needed (e.g., a track is made
   visible, or a marker is dragged), that session is bumped to the front of the
   loading queue. This keeps user-perceived delay to a few milliseconds.

As each session finishes loading, its live attribute and column values replace the
cached values from the index.

## Prerequisite Cleanup

Before implementing persistence, two small changes are needed to ensure that
`m_attributes` and `m_sensors` contain only user-defined ground-truth data:

### 1. Stop using `m_attributes` as a calculation cache

Several multi-output calculations currently store their results via
`setAttribute()`, which places calculated values into `m_attributes` where they
are indistinguishable from user data. These should use a new
`setCalculatedAttribute()` method (mirroring the existing
`setCalculatedMeasurement()`) to store results in `m_calculatedAttributes` instead.

Affected calculations:

- **AnalysisRange** (`attributecalculations.cpp`): stores both
  `AnalysisStartTime` and `AnalysisEndTime` via `setAttribute()`.
- **Flare** (`attributecalculations.cpp`): stores both `FlareStartTime` and
  `FlareEndTime` via `setAttribute()`.
- **TimeFit** (`timecalculations.cpp`): stores both `TimeFitA` and `TimeFitB`
  via `setAttribute()`.

### 2. Move `_VISIBLE` out of `m_attributes`

Session visibility (`_VISIBLE`) is UI state, not session ground truth. It is
currently stored in `m_attributes` via `SessionData::setVisible()`. This should
be moved to `SessionModel`, which already owns the session list and handles the
visibility checkbox toggle.

## Development Phases

### Phase 1: Persistence Layer

- Write imported sessions to `sessions/<uuid>.csv` in FS2 format, serializing
  `m_attributes` and `m_sensors`.
- On startup, load sessions from the logbook folder instead of requiring re-import.
- Write and read `index.json` for cached attributes and column values.

### Phase 2: Startup Optimization

- Populate the logbook table from the index before session data is fully loaded.
- Load plotted/visible sessions first.
- Load remaining sessions in the background with priority bumping for user
  interactions that need specific session data.
