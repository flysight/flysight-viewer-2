# Load Only Visible Sessions

## Overview

The application currently loads all sessions into memory on startup via a
background loader. With ~300 sessions this uses ~900 MB; scaling to 3,000
sessions (realistic for a coach) would use ~9 GB. Most of these sessions are
never viewed. This spec replaces the "load everything" model with on-demand
loading, an LRU cache, and a background worker that computes dirty column
values.

**Design goal:** Memory usage should be bounded by a configurable cache size,
not by the number of sessions in the logbook. Startup should be instant
regardless of logbook size.

## Dependencies

- **Depends on:** Background load/save workers (`docs/load-save-jobs.md`) —
  stub/loaded row model, `SessionRow`, `sessionRef()`, saver worker, dirty
  flag, high-water-mark progress tracking.
- **Context:** `docs/logbook-phase-2.md` describes the existing background
  loader, index format, cached column values, and the stub vs loaded session
  model.

## Concepts

### On-Demand Loading Only

The current background loader loads every session into memory after startup,
ordered by `lastAccessed`. This is removed entirely. Sessions are loaded only
when needed:

- When a session is made visible (for plotting)
- When a session is edited
- When a session's column values need recomputation (dirty column worker)

Startup populates stubs from the index and stops. No progress bar, no
background loading.

### LRU Cache with Visibility Pinning

Loaded sessions are held in a bounded LRU cache. The cache has two tiers:

- **Pinned:** Visible sessions (those being plotted/displayed) are always in
  memory. They cannot be evicted.
- **Unpinned:** Non-visible sessions fill remaining cache slots, ordered by
  how recently they were visible. When the cache is full, the least recently
  used unpinned session is evicted.

When a session becomes visible, it is loaded (if not already) and pinned.
When a session becomes non-visible, it is unpinned and enters the LRU pool.

Cache capacity is configurable via a preference (see below).

Dirty sessions (with unsaved changes) must be written to disk before eviction.
On application close, all dirty sessions must be flushed before exit (this
is already handled by the existing saver worker).

### Dirty Column Values

When a logbook column is added, edited, or changed, its cached values in the
index become stale for all sessions. Rather than loading every session to
recompute, the stale column values are marked dirty. A background worker
processes dirty values in idle time, loading sessions temporarily to compute
them.

The dirty state is the absence of a cached value for a column in the index.
When a column is added or changed, the cached values for that column are
removed for all sessions. The background worker scans for sessions with
missing column values and fills them in.

## Design

### Removing the Background Loader

The background loader (`m_loadTimer`, `m_loadQueue`, `startBackgroundLoader`,
`loadNextSession`, and associated progress signals/UI) is removed. The startup
progress bar is removed. The `lastAccessed` ordering logic is no longer needed
for loading.

### LRU Cache

The LRU cache tracks which non-visible loaded sessions to keep in memory.
When a loaded session is accessed (made visible, edited, or otherwise
touched via `sessionRef()`), it is bumped to the front of the LRU list.

When the number of loaded non-visible sessions exceeds the cache capacity,
the least recently used session is evicted. Eviction means: save to disk if
dirty, update cached column values from the in-memory data, then set the
session back to a stub (`session = std::nullopt`). The cached column values
remain, so the logbook table continues to display correctly.

If the number of pinned (visible) sessions exceeds the cache capacity, all
visible sessions remain in memory — the cache capacity applies only to the
unpinned pool.

### Cache Size Preference

Add a preference for the session cache size. A reasonable default is
something like 50. The preference should be in a logical location (e.g.,
Preferences > General or Preferences > Import).

### Dirty Column Worker

A background worker processes sessions with missing column values, using the
same QTimer-at-0ms-interval pattern as the existing saver worker.

**Tick processing:** On each tick, find a session with one or more missing
column values. If the session is already loaded (in cache), compute the
missing values from the in-memory data. If not loaded, load the session
temporarily, compute the values, and discard it (do not put it in the LRU
cache — these are temporary loads, not user-initiated). Cache the computed
values in the index. Re-arm the timer.

**Completion:** When no sessions have missing column values, stop the timer
and flush the index.

**Priority:** The saver worker has priority over the dirty column worker
(same as it currently has over the background loader). Dirty data in memory
is at risk; stale column values in the index are merely inconvenient.

**Progress bar:** Show a progress bar with high-water-mark tracking (same
pattern as the saver) while dirty column values are being processed. The
progress bar should include a cancel/stop button. Cancelling stops the
worker; the missing values remain missing in the index. They will be picked
up again later — either if the worker is restarted, or on next app launch.

### Column Changes

When columns change (add, edit, remove via `rebuildColumns`):

- For loaded sessions, compute the new/changed column values immediately
  from the in-memory data and cache them.
- For stub sessions, remove the cached values for the changed columns. This
  makes them "dirty" — the dirty column worker will fill them in.
- For removed columns, clean up any cached values and dirty state associated
  with the removed column definitions.
- Start/restart the dirty column worker.

### Index Rebuild

If the index is missing or corrupt on startup, all column values for all
sessions are missing. The dirty column worker handles this naturally — it
processes every session, loading each one to compute all column values. No
separate code path is needed.

## Interaction Scenarios

### Column added while dirty column worker is running

The new column's values are removed for all sessions. The worker continues
processing — when it reaches each session, it computes all missing columns
(both the previously-queued columns and the newly-added one) in a single
load.

### User makes a session visible while dirty column worker is running

The session is loaded on demand via `sessionRef()` and pinned in the cache.
The dirty column worker does not need to be interrupted — when it
eventually reaches that session, it finds it already loaded and computes
the values from the in-memory data without a disk load. Or, if the column
values were already computed during the on-demand load, it skips the session.

### Application close with dirty column values

Dirty sessions (unsaved changes) are flushed as they are now. Missing column
values in the index are harmless — they persist as gaps, and the dirty
column worker picks them up on next launch.

### Sort with missing column values

Sessions whose column values have not yet been computed will have missing
values for those columns. These should sort in a consistent, predictable way
(e.g., to the bottom).

## Notes for Implementer

### Temporary Loads in the Dirty Column Worker

The dirty column worker loads sessions that are not in the LRU cache
temporarily — it should not go through `sessionRef()`, which would put them
in the cache and potentially evict sessions the user actually cares about.
Load via `LogbookManager::loadSession()` directly, compute values, discard.

### Existing Saver Integration

The saver worker already handles dirty sessions and has priority. The dirty
column worker slots in below the saver in priority, taking the place of the
current background loader in the priority chain.

### Eviction Before Save

When evicting a session from the LRU cache, the session must be saved first
if it is dirty. This must also be guaranteed on application close.

## Definition of Done

1. Startup is instant — no background loading of sessions, no startup
   progress bar
2. Sessions are loaded on demand when made visible, edited, or accessed
3. Memory usage is bounded by the configured cache size plus visible sessions
4. Adding or editing a logbook column shows a progress bar while column
   values are computed in the background
5. The progress bar has a cancel button; cancelling leaves missing values
   to be computed later
6. Missing/corrupt index is rebuilt by the same dirty column worker — no
   separate code path
7. Dirty sessions are always saved before cache eviction and before app close
8. All existing functionality works correctly
9. Build passes with no errors
