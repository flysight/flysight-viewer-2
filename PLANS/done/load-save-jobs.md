# Background Load/Save Workers

## Overview

After importing 300+ sessions, the application freezes for ~20 seconds while
writing merged CSV files to disk. The data is already in memory and the UI
could be fully functional during this time. This spec replaces the synchronous
save loop with a background worker that processes dirty sessions one at a time
via the event loop, mirroring the existing background loader pattern.

**Design goal:** After an import completes, the user should be able to interact
with the application immediately. Saving to disk happens in the background.

## Dependencies

- **Depends on:** Phase 2 (startup optimization) — background loader, stub/loaded
  row model, `SessionRow`, `sessionRef()`, `LogbookManager`.
- **Context:** `docs/logbook-phase-2.md` describes the existing background loader,
  stub vs loaded sessions, and the signal design (`modelChanged` vs
  `sessionLoaded`).

## Concepts

### Workers Driven by Row State

Rather than managing explicit job queues, two workers react to flags on
`SessionRow`:

- **Loader worker:** Processes rows where `session` is `std::nullopt` (stubs).
  This is the existing background loader.
- **Saver worker:** Processes rows that are marked dirty — their in-memory
  `SessionData` has not yet been persisted to disk.

Both workers use the same mechanism: a `QTimer` at 0ms interval, processing
one session per event-loop tick. Between ticks the UI remains responsive.

When an import adds 300 sessions, `mergeSessions` creates loaded rows and marks
them dirty. The saver worker picks them up. When a marker drag modifies a
session's attributes, the same dirty flag is set and the same worker handles it.
No separate queue or job object is needed — the flags *are* the queue.

### Priority

1. **On-demand loading** (synchronous `sessionRef()`) always takes implicit
   priority — it runs inline when the user interacts with a stub.
2. **Saver worker** runs next. Dirty data exists only in memory and is at risk
   if the application crashes. Persisting it takes priority over loading stubs,
   which can always be re-read from disk.
3. **Loader worker** runs when there are no dirty sessions to save.

When the saver has work, the loader pauses. When the saver finishes, the loader
resumes from where it left off.

### High-Water-Mark Progress Tracking

Each worker tracks a high-water mark to provide meaningful progress bars. When
a batch of work arrives (e.g., 335 sessions become dirty from an import), the
worker snapshots the count as the total for the current "wave." The progress
bar draws against that total, counting down as sessions are processed.

If new work trickles in during a wave (e.g., a marker drag dirtying one
session), the total is incremented. When the wave drains to zero, the
high-water mark resets so the next wave starts fresh.

## Design

### Dirty Flag on SessionRow

Add a `dirty` flag to `SessionRow`. A row is dirty when its in-memory
`SessionData` has changes that are not yet persisted to disk. The flag is set
by:

- `mergeSessions` — newly imported sessions need to be written to the logbook
  directory.
- Any code path that modifies a loaded session's data (marker drag, attribute
  edit, etc.) — currently managed by `m_dirtySessions` / `scheduleSave()`.

The flag is cleared when the saver worker successfully writes the session to
disk.

### Saver Worker

The saver worker mirrors the existing loader worker pattern:

- **Tick processing:** On each timer tick, find the next dirty row, call
  `LogbookManager::saveSession()`, clear the dirty flag, update cached column
  values, and re-arm the timer.
- **Completion:** When no dirty rows remain, flush the index to disk and stop
  the timer.
- **Interaction with loader:** While the saver is active, the loader timer is
  not armed. When the saver completes, re-arm the loader if stubs remain.

### Replacing `flushDirtySessions`

The current synchronous `flushDirtySessions` path (which saves all dirty
sessions in one shot) is replaced by the saver worker for normal operation.
The only place that needs a synchronous flush is application shutdown — on
close, drain all remaining dirty sessions before exiting.

### Progress UI

Two stacked progress bars at the bottom of the logbook dock:

- **Saver bar** (top): visible when dirty sessions exist. Label like
  "Saving sessions..." with a progress bar driven by the high-water mark.
- **Loader bar** (bottom): visible when stubs exist. Label like
  "Loading sessions..." with a progress bar driven by its own high-water mark.
  This is the existing progress bar, relocated into the new layout.

When a worker has no work, its bar is hidden. Typically only one bar is visible
at a time, but both may briefly appear during transitions (e.g., saver finishes
and loader resumes). Having two separate bars avoids confusing jumps when
switching between save and load work.

## Interaction Scenarios

### Marker drag during background save

The marker drag modifies `SessionData` attributes and marks the session dirty.
If the saver hasn't reached that session yet, it saves the current
(already-modified) data — correct. If the saver already saved it, the dirty
flag is set again and the saver picks it up on a subsequent tick.

### New import during background save

`mergeSessions` adds new loaded rows marked dirty. The saver's high-water mark
increases to account for the new work. The saver continues processing without
interruption.

### New import during background load

`mergeSessions` adds new loaded rows marked dirty. The loader pauses while the
saver drains the new dirty sessions. The loader then resumes.

### Application close during background work

Synchronously drain all remaining dirty sessions before exiting. Stubs do not
need to be loaded — they are already persisted on disk.

## Notes for Implementer

### Loader Ordering

The existing loader orders stubs by `lastAccessed` descending (most recently
used first). The saver does not need a particular ordering — just drain all
dirty rows.

### Index Flushing

Flush the index to disk when the saver worker drains to zero and on application
close.

## Definition of Done

1. After importing 300+ sessions, the UI is interactive immediately — the
   progress bar shows saving progress while the user can browse, plot, and
   interact with sessions
2. Sessions are persisted to disk in the background without blocking the UI
3. The saver worker has priority over the loader worker; the loader pauses
   while saves are in progress
4. Progress bars accurately reflect remaining work using high-water-mark
   tracking
5. Marker drags and other edits during background saving work correctly
6. Application close flushes all unsaved sessions synchronously before exit
7. All existing functionality works correctly
8. Build passes with no errors
