# Background computation per recording: the Compute attribute

Date: 2026-10-04.
Status: specification for planning. Not an implementation plan. Small enough
to be implemented directly by one agent in one phase.
Baseline: branch `store-requested-calculations` at `3260e24`; the committed
code is authoritative.
Related: `src/calculationdemand.h/.cpp` (the reconciler: the walk, the track
conditions, the pair memory, the choice), `src/demandfill.h` (the column
fill), `src/demandstate.h` (progress, failures, pending cells),
`src/jobqueue.h/.cpp` (the executor, `cancel(JobId)`),
`src/logbookmanager.h/.cpp` (the index: `records`, `recordReasons`),
`src/sessionmodel.h/.cpp` (Choice cells, `startBulkEdit`),
`src/ui/docks/logbook/LogbookCellDelegate.h` (the pending mark, the row
warning), `src/ui/docks/logbook/LogbookView.h` ("Set ..."),
`src/attributeregistry.h` and `src/calculations/attributeregistration.cpp`
(the built-in attribute definitions), `src/fusion/fusionregistration.cpp`
(the Orientation definition and its constant default, the pattern to
follow), `docs/CALCULATIONS.md` sections 15 and 16, `docs/COMPUTED_PLOTS.md`,
`docs/DATA_SCHEMA.md` sections 9 and 11, `docs/SENSOR_FUSION.md` section 2,
`tests/audit/cleanup_audit.cmake` (group `gestures`: "no product code cancels
a job").

## 1. Motivation

Requested calculations follow demand: a checked fusion plot or an enabled
column over a fusion output wants a result for every track it covers, and
the demand layer computes what is missing, one recording at a time, for as
long as the plot stays checked or the column enabled. There is no way to
leave one recording out. A logbook of a hundred recordings with a column
over a fusion output fits all hundred; a recording the user knows will not
converge, or does not care about, is fitted all the same, and a recording
whose fit is known to take an hour holds the worker while the recordings
the user is looking at wait behind it. The only remedies today are to
uncheck the plot or disable the column, which drop every recording at once.

The remedy is a per-recording switch, stored with the recording as the
Orientation is: on, the recording takes part in background computation as
it does today; off, nothing is computed for it in the background, a fit
already running for it is stopped, and what was computed earlier is kept
and shown. The switch is read in one place, the demand layer, which is the
only starter of requested calculations, so it covers the sensor fusion fit
and every background calculation added later without a switch of its own.

## 2. Principles

- **One switch, one reader.** The attribute is read by the demand layer
  alone, when it decides what is wanted. The engine, the executor, the
  column worker, the stores and the kernel know nothing of it. A future
  requested calculation is covered by construction.
- **Demand, not data.** The attribute is not an input of any calculation.
  Changing it never invalidates a result, stored or in memory, never starts
  a settle wait, and never makes a running job stale. It changes what is
  wanted, exactly as checking a plot or showing a track does, and the
  running job is stopped for that reason alone.
- **Off is an explicit intent.** Hiding a track is momentary, and a running
  fit for it finishes and is kept (the demand specification). Switching a
  recording off is a decision about that recording: the fit running for it
  is stopped at its next boundary, and nothing of it is published or
  stored.
- **What exists is shown.** A stored result of a recording that is switched
  off is restored and drawn like any other. Restoring is not demand.
- **Nothing happens by default.** The attribute is absent from every
  recording on disk today, and absent means on. No file is rewritten, no
  index is discarded, no stored result is dropped; the compatibility
  marker and the algorithm strings are untouched.
- **On-demand calculations are not gated.** An instant calculation (a
  value derived from stored outputs, a column that needs no computing) is
  computed for a recording that is switched off as it is for any other.

## 3. The attribute

- **Key, type, values.** A session attribute `_COMPUTE` (`SessionKeys`), a
  Choice of two tokens, `on` (label "On") and `off` (label "Off"), in that
  order. Its definition is a built-in of the core (category "Session",
  display name "Compute", editable), registered beside the other built-in
  definitions, not by the fusion library: the switch is the demand layer's,
  not the fit's.
- **Storage.** Stored with the session like the Orientation (DATA_SCHEMA
  section 9): a `$VAR,_COMPUTE,<token>` line, written when the user sets
  it, never removed by an edit, saved verbatim. A recording without the
  line reads the constant default `on`, through a constant default of the
  attribute (as `builtin.default._ORIENTATION` serves the Orientation), so
  the column shows "On" for every recording that has never been set and
  the session file is not touched by showing it. A hand-edited token
  outside the list is kept and shown as written; for the demand layer any
  token but `off` is on.
- **Not an input.** No calculation declares it. Setting it emits the
  model's ordinary change for the attribute's name; the demand layer
  observes that change as a change of demand (a pass), and nothing else
  reacts: no result depends on the name, so the engine invalidates nothing
  but the attribute's own value, the executor's running job is not stale,
  and no settle wait starts.
- **The column.** "Compute", header tooltip "compute results for this
  recording in the background", a Choice column like the Orientation: its
  cell shows the label of the effective token, is edited in place with the
  list, and the logbook's context menu offers "Set Compute..." for the
  selected recordings with the same list, applied as a bulk edit; a bulk
  edit of a recording that is not loaded takes the existing stub path (edit,
  save, index) without loading it. The column is not in the default column
  set; the user adds it (Add Column, "Session").
- **The import preference.** Preferences > Import gains "Compute newly
  imported recordings in the background", on by default, beside the
  orientation. It works as the orientation's does: a recording imported
  while it is off is written with `$VAR,_COMPUTE,off`, a fact of the import
  the user can change per recording afterwards; while it is on nothing is
  written, and the recording reads the default. It serves the user who
  wants positive control: new recordings compute nothing until switched on
  one by one or with "Set Compute...". Changing the preference touches no
  recording already imported.

## 4. What changes in the demand layer

- **An excluded track.** A track (a session of a source, the demand
  specification's term) whose session reads `off` is **excluded**: a new
  condition beside `Done`, `Failed`, `NotApplicable`, `Waiting` and
  `Running`, decided before any other, for loaded sessions and sessions
  that are not loaded alike, for plot tracks and column tracks alike. An
  excluded track is never a candidate, never counted in progress, never an
  entry of the failures, and never pending. A session all of whose tracks
  are excluded is, to the demand layer, a session that wants nothing: not
  loaded by the column fill, not held, not offered.
- **Stored results still serve.** The engine restores a stored result of an
  excluded session when it is loaded, as today; its plots draw, its columns
  fill from the record, and its derived values compute on demand. A stored
  rejection or solver failure of an excluded session is not listed in the
  failures and shows no row warning: the warning is about what is wanted,
  and nothing is wanted of the recording. Switching it on again lists the
  failure as before, without computing.
- **The cell of an excluded track.** A requested column's cell of an
  excluded session that has no value reads "excluded", muted like the
  pending mark and with the tooltip "Not computed: background computation
  is switched off for this recording", where it would otherwise read "···"
  or be blank. A value always wins: the cell of an excluded session with a
  stored result shows the value. It is a presentation of demand, like the
  pending mark: never a cached value, never written to the index, and
  sorting treats the cell as it treats a pending one.
- **Switching off.** A pass runs. Every waiting pair of the session leaves
  demand: a chosen next job for it is withdrawn ("No longer needed", as
  today). The **running job** for it, if any, is asked to stop through the
  executor's cancellation: it ends Cancelled with the reason "Switched off
  for this recording" at its next boundary, publishes nothing and stores
  nothing (cancel wins over a late result, as today), and the next job
  starts after the worker is joined. The demand layer is the executor's one
  product caller of `cancel()`; the executor's lifecycle, its first-writer
  rule for the pending end and its shutdown are unchanged. A held session
  whose last pending cell was excluded releases its hold and leaves the
  hidden pool by ordinary eviction. The pair memory is not touched: a
  failure remembered for the session is still remembered, and an exclusion
  is not a memory, it is read from the attribute at every pass.
- **Switching on.** A pass runs; the session's tracks are classified as any
  other's, and its missing results enter demand under the ordinary
  priority, from the start: a fit that was stopped runs again from its
  beginning. A failure remembered for the pair in this run still keeps it
  from being offered, as it would any other pair.
- **The plot widget's "no data" warning** keeps asking the engine and
  stays silent for an excluded track whose result is missing, because the
  engine still reports the name `Blocked`; nothing changes there.
- **Priority, the settle wait, the fill's bound, the executor's limits and
  the status bar's form** are unchanged. The status bar's counts and lists
  simply do not contain excluded sessions, and the fill never loads one.

## 5. Sessions that are not loaded

The demand layer decides an excluded track without loading the session:
loading it would be the background work the switch forbids. For a loaded
session the attribute is read from the session; for one that is not loaded
it is read from the logbook index, which records, per session, whether the
attribute is `off`: a fact about what is on disk, derived, optional and
additive, beside `records` and `recordReasons` (DATA_SCHEMA section 11).
The index learns it wherever it reads or writes the session's file: the
import, a save (the bulk edit's stub path included), a load, and the column
worker's temporary copy. An index without the entry means on, which is what
every existing logbook reads. The model already announces a bulk edit of a
recording that is not loaded under the attribute's own name (CALCULATIONS
16.3), so the edit takes effect at the next pass without a load.

The session file is the authority; the index entry is a cache of it. An
index that is missing or discarded is rebuilt as it is today: the filename
scan, then the column worker's background pass over every session file,
which relearns the entry. Until the worker reaches a session whose
attribute is `off`, the entry is absent and the session reads as on, so
the column fill may load it once as a hidden session; the loaded session
then reads `off` from its own attribute, no job starts for it, and its hold
is released. The recovery costs at most one hidden load per such
recording, never a fit.

## 6. Tests

- A session switched off is not computed when a plot is checked while it is
  visible, when it is shown while a plot is checked, or when a column over a
  requested output is enabled; the other sessions are; progress never counts
  it; its column cell reads "excluded" and is not pending.
- Switching a session off while its job is running ends the job Cancelled
  with the reason, publishes and stores nothing, releases its hold, and the
  next candidate runs; switching off while its pair is the chosen next job
  withdraws it; switching off while the session settles after an edit
  leaves nothing waiting.
- Switching a session on creates demand for its missing results and no
  other; a stopped fit runs again from its start.
- A session switched off with a stored result restores and draws it, fills
  the column from it, and is not loaded again for it; a stored rejection of
  a session switched off is not listed among the failures and shows no row
  warning, and is listed again when the session is switched on.
- Setting the attribute invalidates no stored result and no cached value
  over a requested output, starts no settle wait and makes no running job
  stale; the compatibility marker and the algorithm string are unchanged
  (the goldens and the stored-result tests prove it).
- The column fill never loads a session that is switched off; a logbook
  whose unloaded sessions are all off creates no load and no job when the
  column is enabled; the index records the attribute on the bulk edit's stub
  path and reports it without a load.
- The column: the default shows "On" without a write; an edit stores a
  token and refuses others; "Set Compute..." bulk-edits the selected
  sessions; a hand-edited token is shown as written and read as on. The
  Orientation column's tests are the model.
- The import preference: a recording imported while it is off carries the
  `off` line and is excluded at once, for a column that is enabled and a
  plot that is checked alike; one imported while it is on carries no line
  and reads the default; the preference's page round-trips the setting.
- A session whose attribute is `off` on disk but absent from the index (an
  index discarded or written by an earlier build) is loaded at most once by
  the fill and never fitted.
- A manual step: a logbook with a long-running recording, the recording
  switched off while it is being computed, the status bar's count, the
  cell, the row and the plot observed; switched on again; the whole-logbook
  column fill with half the recordings off.
- The cleanup audit's rule that no product code cancels a job is restated:
  the demand layer is the one product caller. The acceptance map opens a
  new hundred. `audit_cleanup` and the whole suite green.

## 7. Documentation

`docs/COMPUTED_PLOTS.md`: a section on switching a recording off (what it
does, what is kept, the cell, the context menu, the import preference), the status bar and
logbook sections where they say every recording is computed, and the known
limitation "there is no switch that pauses background work" restated: there
is one per recording and none for all. `docs/CALCULATIONS.md` section 16:
the condition, the reading of the attribute, the stop of the running job
(and 15.3 where it says no product code cancels a job), the fill, the
pending mark's third text. `docs/DATA_SCHEMA.md` section 9 (the attribute
line) and section 11 (the index entry). `docs/SENSOR_FUSION.md` section 2
(using it, beside the Orientation). `tests/README.md`: the test rows, the
manual step, the specification's appendix and matrix.

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, staged by explicit path, with the
session's attribution line; `PLANS/` is never staged.
