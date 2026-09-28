# Phase 1: Progress and failures in the demand layer

## Contract notes (read first)

The interface in the overview under "Phase 1 provides" works as written. Four
details the overview leaves implicit are fixed here. None renames anything,
and none blocks the phase:

1. **"The executor's running job"** means the running job that has not been
   asked to stop. This matches the Running track (`classifyLoaded()`:
   `running.id != 0 && !running.cancelRequested`). A job that is winding down
   publishes nothing, so `sessionName` and `progressText` are empty while it
   finishes, and its session counts as Waiting.
2. **`sessionName` is read by the walk** as it passes the running job's row.
   A pass with no source walks no row (`m_sources.isEmpty() ? Walk()`), so in
   that pass `sessionName` and `progressText` are empty. `count` is 0 then too,
   so no view has anything to show. Reading the name outside the walk would
   need `getSessionRow()`, which is a second linear scan of the rows.
3. **`retriedAtNextStart` differs from `DemandTrack::jobFailure`** for an
   unstored result (a computation that threw). `jobFailure` is false for that
   case today, because `failedTrack()` sets `jobFailure = !onlyResults`. The
   interface makes `retriedAtNextStart` true, and that is correct: nothing is
   stored, so the next start runs it again. `jobFailure` stays as it is and
   goes with the per-source state in phase 4.
4. **`sessionFailures(id)` for a session without a failure** returns
   `sessionId == id`, an empty `sessionName` and no calculations.

## Purpose

This phase implements spec §10 and the demand side of §5 and §7. After a pass,
the demand layer holds two new presentation values, each with its own
announcement: **progress** (sessions waiting or running, the high-water mark,
the running recording and its step) and **failures** (per recording, in row
order, each failed pair with its title, reason and retry flag). The pending
cells get an announcement separate from the per-source state, and the one
text form of a recording's failures, and of the capped list, is written
beside the values.

The phase is additive. `plotState`, `columnState`, `workingPlotIds`,
`workingColumnIds`, `plotStateChanged`, `columnStateChanged` and
`statesChanged` stay exactly as they are until phase 4, and no view changes.
It is one phase because every later phase reads what it adds: the status bar
in phase 2, the logbook row in phase 3, and in phase 4 the reduced surface
that these values become.

## Dependencies

- **Depends on:** nothing. The phase starts from the committed head of
  `store-requested-calculations`.
- **Blocks:**
  - phase 2, whose status bar reads `progress()`, `failures()`,
    `SessionFailures::listText()` and the two signals;
  - phase 3, whose logbook row reads `sessionFailures()` and `text()`, and
    whose cell delegate moves to `pendingCellsChanged`;
  - phase 4, which removes the per-source surface and leaves this one as the
    demand layer's presentation.
- **May assume:** the demand layer as described by the contract comment in
  `src/calculationdemand.h`: one walk (`walkRows()`) under one
  `RowStabilityGuard`, the pair memory (`m_memory`, `PairMemory`), the
  record-set memo (`recordSet()`, `m_recordReasons`), and the pass
  (`recompute()`, then `applyStates()`).

## What changes

### Presentation values (`src/demandstate.h`, `src/demandstate.cpp`)

Add `DemandProgress`, `FailedCalculation` and `SessionFailures`, exactly as
listed under Interfaces. Follow the pattern of `DemandState`: plain values
with `operator==` and `operator!=`, `tr()` text through
`Q_DECLARE_TR_FUNCTIONS` placed last in the struct (the macro ends in
`private:`), QtCore only, and no knowledge of the model, the executor or the
widgets. Extend the file's head comment to name the new values.

Two things in this file must not change: `DemandState`, `DemandTrack`,
`buildToolTip()` and `kToolTipListLimit`; and the text of any existing
tooltip. `SessionFailures::kListLimit` is a separate constant.
`DemandState::kToolTipListLimit` limits a different list and goes away in
phase 4.

**The text form is part of the contract.** Phases 2 and 3 show it as is, and
phase 5 documents it.

- **`text()`**: one line per failed calculation, in list order, joined by
  `"\n"`:
  - the line is `"<title>: <reason>"`;
  - when `retriedAtNextStart` is true, the line ends with
    `" (tried again at the next start)"`;
  - with no calculations, `text()` is empty.
- **`listText(failures)`**: for each of the first `kListLimit` recordings, its
  `sessionName` on one line, then each line of its `text()` indented by two
  spaces. When there are more recordings, a last line at column 0 says
  `"and %n more"` (`tr()` with `%n`), where n counts the recordings left out.
  With an empty list, `listText()` is empty. There is no header line; the
  status bar's own label says what the list is.

Example with twelve recordings:

```text
Jump 1
  Explicit A: negative input
Jump 3
  Gated: The worker thread could not be started (tried again at the next start)
... (up to ten recordings)
and 2 more
```

### The demand layer (`src/calculationdemand.h`, `src/calculationdemand.cpp`)

#### Where the values come from

The values come from the one walk, from the tracks it already classifies, and
from the pair memory.

- **`count`** is the number of rows at which at least one track of any source
  is `Waiting` or `Running`, so each session counts once. The walk already
  visits every row once. A per-row flag beside the existing `waiting` flag is
  enough; `walk.pendingSessions` covers column cells only and is not the
  count. Because the fill's sessions are a subset, spec §5's "contained in
  the computations' count" holds by construction.
- **`highWater`** is kept by the pass: 0 when `count` is 0, otherwise the
  greater of the previous `highWater` and `count`.
- **`sessionName` and `progressText`** come from the running record that
  `recompute()` already reads before the guard:
  - the walk reads the running job's row name as it passes that row. It
    reuses the `name` it reads for the row's listed tracks, and a Running
    track is always listed.
  - `progressText` is `running.progressText`.
  - The pass records which job the value describes.
  - `onJobProgress()` then updates `progressText` without a pass, for that job
    only, next to its existing update of the running tracks.
- **`failures`** are built per row as the walk goes, which gives row order.
  Each Failed track contributes one `FailedCalculation` per failed pair. A
  pair already met at that row, in an earlier source, is skipped; the first
  occurrence wins. `sessionName` is the `name` the walk reads for the row's
  listed tracks; a Failed track is listed.
- **Inert component.** When the component is inert, progress is the default
  value and there are no failures.

The entries come from the same code, in the same call, that makes the track
Failed: `failedTrack()`, the `NotProduced` branch of `classifyLoaded()`, and
rules 2, 4 and 5 of `classifyUnloaded()`. That way a track and its entries
cannot disagree. How the entries travel is the implementer's choice: an
out-parameter, a pair returned, or a list in `Walk`. They must not become a
field of `DemandTrack`, because its equality decides the per-source
announcements, and those must not change.

`failedTrack()` today receives titles and facts but no calculation ids. It
must receive the ids as well.

| Failed track built at | One entry per | `calculationId`, `title` | `reason` | `retriedAtNextStart` |
|---|---|---|---|---|
| loaded, `Available`, a storable calculation remembered failed | remembered failed storable calculation | `source.storable[i]`, `storableTitles[i]` | the memory's reason (below) | true |
| loaded, `NotProduced` | note | `note.calculation.instanceId`, `titleOf(note.calculation)` | `note.detail`, else the default `noteReason()` uses | false only when `note.status == Ok`, the calculation is in `source.storable`, and the memory holds no failure for the pair; otherwise true |
| loaded, `Blocked`, a blocker remembered failed | remembered failed blocker | `blocker.instanceId`, `titleOf(blocker)` | the memory's reason | true |
| not loaded, rule 2 | remembered failed storable calculation | `storable[i]`, `storableTitles[i]` | the memory's reason | true |
| not loaded, rule 4 (every record known, some with a reason) | storable calculation whose record has a non-empty reason | `storable[i]`, `storableTitles[i]` | the reason from `m_recordReasons` | false |
| not loaded, rule 5 (failed-load placeholder) | storable calculation | `storable[i]`, `storableTitles[i]` | `loadFailureReason()` | true |

Three details of this table:

- **Rule 4.** The track keeps only the first reason, as today. The entries
  list every pair that has a reason, read from the existing memo, so there is
  no second `calculationRecordReason(` call.
- **Rule 5.** The facts this rule learns give the same entries through rule 2
  at the next pass, so that pass announces nothing. The same holds for a
  `NotProduced` note whose status is not Ok: it gives the same entry through
  the memory once the session is evicted. `titleOf()` and `storableTitles`
  agree, because the engine fills `CalculationBlocker::title` from the
  descriptor's title (or the instance id when the title is empty).
- **Loaded `NotProduced` notes.** The memory check is what marks a rejection
  whose record could not be written (a Write failure) as retried.

#### The per-pair reason: the title appears once

`PairMemory::reason` today holds `"<title>: <why>"` for the Job, Write and
Result origins, and the load text alone for the Load origin. Store the *why*
alone (the reason without the title):

- `onJobFinished` stores `record.reason`;
- `onCalculationRecordWriteFailed` stores its `reason` and no longer looks up
  the title;
- the `NotProduced` branch stores the note's detail, or the default detail
  when the note has none;
- the Load origin stores `loadFailureReason()`.

`failedTrack()` then becomes the one place that builds `DemandTrack::reason`
from a fact. For every origin except Load it writes `"<title>: <why>"`, and
for Load the why alone, because a failed load is the session's failure, not a
calculation's. It joins distinct texts with `"; "` as today.

Every existing `DemandTrack::reason` string stays byte for byte the same. The
existing tests check "Explicit A: The worker thread could not be started",
"Exhausted: Out of memory", "Gated: Couldn't write file…", "Thrower:
synthetic failure", "The session file could not be loaded" and "Explicit A:
negative input".

Two things are not allowed: recovering the why by stripping a `"<title>: "`
prefix, and keying the memory differently (the pair stays the only key). The
why is never empty; where a source text could be empty, use the defaults
`noteReason()` already uses. Update the comment on `PairMemory::reason` and
the MEMORY paragraph of the class comment to match.

#### Compare, store, announce

After the walk, `recompute()` / `applyStates()` build the new progress, the
failures list, and the per-column pending sets, which exist today as
`m_pendingCells`.

**Store everything before emitting anything.** A slot of any signal reads
`progress()`, `failures()`, `sessionFailures()` and `isCellPending()`.

Then emit:

- **`progressChanged()`**: once, when the new progress differs from the stored
  one;
- **`failuresChanged()`**: once, when the list differs. Order counts, so a
  sort that reorders failed rows announces.
- **`pendingCellsChanged(columnId)`**: for each requested column whose pending
  set differs, and for each dropped column whose old set was not empty.

`columnStateChanged` keeps its present meaning (the state or the pending
cells changed) and keeps firing as today. The order of the new signals
relative to the old ones is not a contract.

`onJobProgress()` emits `progressChanged()` when it changed `progressText`,
and nothing when the text is the same, the job is not the one described, or
the id is unknown.

`sessionFailures()` is called for every painted logbook row (phase 3). Keep a
per-session index beside the list so that it does not scan the list. A lookup
takes no guard.

Update the PRESENTATION paragraph of the class comment and the signal
comments. They must name the three values and the three signals and say
"stored before announced". The contract comment states the contract; `docs/`
is phase 5's.

#### What must not change

- the walk's single guard;
- the candidates and the offers;
- the fill's inputs (`pendingSessions`, `loadCandidates`);
- the pair memory's keys and clearing;
- the per-source values and signals;
- `isMerelyUncomputed()`;
- the test seams.

`src/demandfill.*`, `src/demandsettleclock.*`, and everything under `src/ui`
and `src/mainwindow.*` are untouched.

## Interfaces

**Provided** (the contract; names as the overview fixes them).

In `src/demandstate.h`, namespace `FlySight`, plain values with `==` and `!=`:

- `struct DemandProgress { int count = 0; int highWater = 0; QString sessionName; QString progressText; };`
- `struct FailedCalculation { QString calculationId; QString title; QString reason; bool retriedAtNextStart = false; };`
  - `title` is the registry title, or the id when the title is empty;
  - `reason` never contains the title and is never empty.
- `struct SessionFailures { QString sessionId; QString sessionName; QList<FailedCalculation> calculations; static constexpr int kListLimit = 10; QString text() const; static QString listText(const QList<SessionFailures> &failures); };`
  - with the text form given above.

On `CalculationDemand`:

- `DemandProgress progress() const;`
- `QList<SessionFailures> failures() const;`
- `SessionFailures sessionFailures(const QString &sessionId) const;`
- signals `void progressChanged();`, `void failuresChanged();` and
  `void pendingCellsChanged(const QString &columnId);`

Semantics are as stated in the overview, with the contract notes at the top
of this document.

**Consumed:** nothing new. The phase reads `JobQueue::job()`,
`JobQueue::runningJob()`, `jobProgress`, `SessionModel::sessionDisplayName()`
(inside the walk, as today) and the existing record-set memo.

## Acceptance criteria

Each criterion is checked by a test named under Tests, unless it says
otherwise.

1. **Count** (§10, §5). `progress().count` is the number of sessions with a
   Waiting or Running track in any plot or column. A session with tracks in
   several sources counts once, and a session that is not loaded with a
   waiting column cell counts.
2. **High-water** (§5). `progress().highWater` is the largest `count` since
   `count` was last 0, and 0 while `count` is 0. A new burst of work after
   `count` reached 0 starts at its own count.
3. **Running recording** (§10, §5). While a job not asked to stop runs:
   - `sessionName` is that session's display name and `progressText` its
     latest text;
   - a `jobProgress` text changes `progressText` with no pass (`passCount()`
     unchanged, no pending update);
   - with no job, or with the running job asked to stop, both are empty.
4. **Failure list** (§10, §7, §4). `failures()` lists every session that has
   a Failed track in some source, in session-model row order, with its
   display name.
   - A pair that failed for a plot and for a column is one entry.
   - A recording with two failed calculations is one element with two
     entries.
   - After a sort, the order follows the new row order.
5. **Entry fields** (§10, §7). Each entry's `title` is the calculation's
   title, and its `reason` does not contain the title. `text()` names each
   calculation once. `retriedAtNextStart` is:
   - false for a stored rejection, whether the session is loaded or not;
   - true for a failed job, a failed load, a failed write and an unstored
     result.
6. **Stored rejection without a load** (§7). A stored rejection of a session
   that is not loaded is in `failures()` at the first pass of a new demand
   layer (after `restartDemand()` and after `restartApplication()`), without
   loading the session and without a job.
   - From an index without `"recordReasons"`, it is absent until the column
     worker's pass reports the reason, and then present.
7. **Unstored failure after a restart** (§7). A failure that is not stored is
   absent after a new demand layer. It is present again only when the retry
   fails again, and absent once a retry succeeds.
8. **Clearing** (§7). A failure leaves `failures()` on:
   - a record change of the pair;
   - an input change of the session;
   - a registry change;
   - the session's row being removed;
   - the last source over the calculation being switched off.

   It stays through eviction, a sort and the column worker's pass. Switched on
   again, it is back with no new job, because the pair memory still holds it.
9. **Signals fire only on change** (§10, §13). `progressChanged` and
   `failuresChanged` fire when the value differs and at no other time: a pass
   that changes nothing, or a repeated or foreign progress text, emits
   neither.
10. **Pending cells** (§10; overview decision 4).
    `pendingCellsChanged(columnId)` fires for exactly the columns whose
    pending set changed in a pass, including a disabled column that had
    pending cells, and never for a progress text alone. Values are stored
    before any signal.
11. **Text form** (§7, §8). `text()` and `listText()` produce the text form
    above. The list stops at ten recordings with "and N more"; ten exactly
    has no such line; empty input gives empty text.
12. **Existing behaviour** (§13, last bullet). Every existing test function
    of the suite, and of every other suite, passes unchanged. The audit
    passes, with the "one walk" count still 1 and the
    `calculationRecordReason(` count in the demand layer still 1.

## Tests

In `tests/tst_calculation_demand.cpp`, add a new section "Progress and
failures" with new functions. Do not edit an existing function. The names
below are suggestions; phase 5 cites whatever the tree holds.

Use the existing helpers: `giveInput`, `show`, `check`, `enableColumns`,
`makeStubs`, `settle`, `spin`, `restartDemand`, `restartApplication`,
`restartWithoutRecordReasons`, `recordPath`, `running()`, `jobOf`, `Quiet`,
`WarningCapture` and `gate()`. A small local helper that renders
`failures()` as strings for `QCOMPARE` is welcome.

1. **`progressCountsEachSessionOnce`** (criteria 1, 2, 3).
   - Setup: s1 and s2 visible with `g` checked; s3 and s4 made stubs; column
     `G_OUT` enabled; the gate held.
   - `count` is 4 with s1 and s2 counted once, and `highWater` is 4.
   - `sessionName` is "Jump 1", and `progressText` reaches "step 1".
   - Open the gate one job at a time: `count` falls and `highWater` stays 4;
     at the end both are 0.
   - An input change of s2 with a long settle delay gives `count` 1,
     `highWater` 1, and an empty `sessionName`, because no job runs.
   - An input change of the running session, which makes the job stale, empties
     `sessionName` while the job winds down.
2. **`progressTextWithoutAPass`** (criteria 3, 9).
   - Emit `m_queue->jobProgress(runningJob, "iteration 7")` by hand, as
     `progressUpdatesWithoutInspection` does. `progressText` updates,
     `progressChanged` fires once, and `passCount()` and `totalRuns()` are
     unchanged.
   - The same text again, or an unknown job id, emits nothing.
   - `check("plain")` toggled with a flush emits no `progressChanged`.
3. **`failuresNameEachCalculationOnce`** (criteria 5, 11). One origin per
   step, each asserting the whole entry and the `text()` line:
   - rejection: {expA, "Explicit A", "negative input", false};
   - thrower: {thrower, "Thrower", "synthetic failure", true}. After
     `makeStubs()` the entry is equal, with no `failuresChanged`.
   - job failure via `failNextWorkerStarts`: "The worker thread could not be
     started", true;
   - load failure (a session file removed): "The session file could not be
     loaded", true;
   - write failure (the `recordPath` directory trick): the reason starts with
     "Couldn't write file", true.
4. **`failuresAreOnePerSessionInRowOrder`** (criterion 4).
   - s2 with `T_IN` 1 and `EA_IN` -1 under columns `T_OUT` and `EA1` and the
     plot `ea` with s2 visible: one element for s2, with thrower then expA,
     and expA once.
   - A failure of s1 that happens later is listed before s2.
   - `m_model->sort(0, Qt::DescendingOrder)` reverses the order, and
     `failuresChanged` fires once.
   - `sessionFailures("s2")` equals that element; `sessionFailures("s4")` has
     no calculations.
5. **`storedRejectionIsAFailureWithoutLoad`** (criterion 6). Follow the three
   legs of `storedRejectionIsBadgedAfterRestartWithoutLoad`:
   - after `restartDemand()` and after `restartApplication()`, spy on
     `failuresChanged` before the first flush. s2's entry is present at that
     flush, with `failuresChanged` once, `loadsOf(s2) == 0`, and no job.
   - with `restartWithoutRecordReasons()`, the list is empty until
     `startColumnWorker()` teaches the reason, and then the entry is present.
6. **`unstoredFailureReturnsOnlyWhenItFailsAgain`** (criterion 7).
   - A job failure is listed.
   - Call `failNextWorkerStarts(1)`, then `restartDemand()`. The first flush
     lists nothing; after `waitDemandIdle()` the failure is listed again,
     from the new job.
   - A second `restartDemand()` succeeds: the list is empty and the result is
     stored.
7. **`failuresClearAsThePairMemoryClears`** (criterion 8, first half). Follow
   `pairMemoryIsClearedByRecordInputAndRegistryChanges`, asserting
   `failures()` right after each clearing event, before the retry runs:
   - record change, input change, registry change, `removeSessions`;
   - then a column job failure over stubs: unchanged through `makeStubs()`,
     sort and `startColumnWorker()`, with no `failuresChanged`.
8. **`failuresFollowWhatIsSwitchedOn`** (criterion 8, second half).
   - A gated failure wanted by `g`, `g2` and `G_OUT`. Unchecking `g`, then
     `g2`, keeps it; disabling `G_OUT` removes it.
   - Enabling `G_OUT` again brings it back with `Quiet` holding (no job).
   - For a plot-only failure, hiding the session removes it and showing the
     session brings it back, still without a job.
9. **`pendingCellsChangedPerColumn`** (criterion 10).
   - Setup: columns `G_OUT` and `EA1`, with only s1 having `G_IN`.
   - `pendingCellsChanged(G_OUT)` fires on enabling, and never for `EA1`.
   - A progress text fires `columnStateChanged` but no `pendingCellsChanged`.
   - The job's end fires `pendingCellsChanged(G_OUT)`.
   - Disabling `G_OUT` while a cell is pending fires `pendingCellsChanged`,
     and a slot that calls `isCellPending()` then sees false.
   - A pass that changes nothing fires nothing.
10. **`failureTextAndItsLimit`** (criterion 11). Pure tests of `text()` and
    `listText()`: exact strings for one and two calculations, with and
    without the retry suffix; twelve recordings give ten, then "and 2 more";
    ten give no such line; empty input gives an empty string. Also: an inert
    demand layer, built like `nullCollaborators` does, reports the default
    progress and no failures.

Other test work:

- **Executables and docs.** There is no new test executable, so no new
  `tests/README.md` row and no `tests/CMakeLists.txt` change. New
  acceptance-map items (701 and on), the README appendix and all `docs/`
  changes are phase 5's (overview decision 11). No existing map line cites a
  renamed or removed function.
- **Audit.** No rule names anything this phase removes or renames, so none
  has to change. Extend one rule: add `DemandProgress|SessionFailures|FailedCalculation`
  to "the fill and the settle clock know nothing of the walk", which keeps
  the new values out of the fill as its present pattern does for
  `DemandState`. Keep the counts the phase could trip:
  - "the one walk reads the rows under one guard" (1): no guard in the new
    queries;
  - "the demand layer reads a record's reason in one place" (1): rule 4
    entries read the memo;
  - "the demand layer observes no display change of the model" (1);
  - "one display name of a session": no `_DESCRIPTION` in the demand layer;
  - `widget-free-core`: QtCore only in `demandstate.*`.
  - "the demand layer's replaced machinery stays gone": no member or helper
    named `buildState`, `finishState`, `.waiting` or `.settling`.

## Decisions

- **Entries come from where a track fails.** Deriving failures from the
  Failed tracks, in the call that builds them, is what "current failure"
  means (overview: "the Failed tracks the walk already classifies"). It also
  makes a failure disappear when its session leaves demand, for example when
  it is hidden from a plot-only source, and come back when the session
  returns. The memory keeps it; nothing is cleared. This matches spec §7:
  "about what the user has switched on".
- **The memory stores the why**, the one way to get a per-pair reason without
  the title and without parsing text (see "The per-pair reason").
- **The text form is fixed here** (overview decision 5), with no header, so
  that phase 2's warning label and hover do not repeat each other. The retry
  wording is "(tried again at the next start)".
- **The pending-cells signal fires for dropped columns only if they had
  pending cells.** A column that goes away changes nothing a painted cell
  shows unless it had one.

Ready with caveats: the four contract notes at the top (not asked to stop;
empty name in a pass with no source; retry flag versus `jobFailure`; the
empty `sessionFailures()` value).
