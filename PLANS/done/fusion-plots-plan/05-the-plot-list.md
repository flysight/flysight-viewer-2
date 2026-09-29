# Phase 5: The plot list

No fixed interface is unworkable. This phase renames two test functions that
`tests/acceptance_map.txt` and `tests/README.md` section 9 cite; it updates
both citations (Decisions, D6).

## Purpose

Spec §5, the plot half of §11, and the plot-list and profile bullets of §13,
with overview decisions 12, 13, 14 and 15. The "Sensor fusion" category
becomes the eight plots that phases 3 and 4 made possible, the "GNSS (Local
frame)" category goes, the silent profile rule becomes a stated contract with
a test, and every test, audit rule and document that knows the old lists
follows. It is one phase because the application's list, the tests' mirror of
it and the audit counts that pin them to each other can only change together.

## Dependencies

- Depends on phases 3 and 4, implemented (with 1 and 2) before this one. At
  its start `Fusion/z`, `Fusion/accAlongTrack`, `Fusion/accCrossTrack`,
  `Fusion/bodyHeading`, `Fusion/bodyPitch` and `Fusion/bodyRoll` are
  registered, each on demand with `explicitDependencies()` exactly
  `{builtin.fusion.fit}`; `_ORIENTATION` and the wind attributes have
  constant defaults; `docs/SENSOR_FUSION.md` §2 has phase 4's attitude
  paragraph after the plot-list bullet; `docs/CALCULATIONS.md` §17 describes
  eight registrations.
- `Fusion/z` needs a numeric `_GROUND_ELEV`. The fusion fixture sessions have
  no `GNSS/hMSL`, so without a stored ground elevation `Fusion/z` is
  `NotApplicable` on them, not "merely uncomputed" (phase 3's decision on
  `fusionNames()`). Every real-fit test below that expects Elevation to wait
  on the fit stores `_GROUND_ELEV` on its session.
- Blocks phase 6, which cites the test functions and audit groups listed at
  the end.

## What changes

### The rows (`src/mainwindow.cpp`, `registerBuiltInPlots()`)

The "Sensor fusion" block becomes exactly these rows, in this order
(overview, "Phase 5 provides"):

| Plot name | Units | Sensor / measurement | Type | GNSS counterpart |
| --- | --- | --- | --- | --- |
| Elevation | m | Fusion / `z` | altitude | GNSS (Basic) Elevation |
| Horizontal acceleration | m/s^2 | Fusion / `accH` | acceleration | GNSS (Advanced) Horizontal acceleration |
| Vertical acceleration | m/s^2 | Fusion / `accD` | acceleration | GNSS (Advanced) Vertical acceleration |
| Along-track acceleration | m/s^2 | Fusion / `accAlongTrack` | acceleration | GNSS (Advanced) Along-track acceleration |
| Cross-track acceleration | m/s^2 | Fusion / `accCrossTrack` | acceleration | GNSS (Advanced) Cross-track acceleration |
| Heading | deg | Fusion / `bodyHeading` | angle | GNSS (Basic) Course |
| Pitch | deg | Fusion / `bodyPitch` | angle | none |
| Roll | deg | Fusion / `bodyRoll` | angle | none |

- The six "GNSS (Local frame)" rows and their comment go. The category no
  longer exists.
- **Colours (decision 12).** Pick them from the function's palette variables
  (`S`, `S_dk`, `L_w`, `L_c`, `L_b`, `L_d*`, `group_a`) or Qt named colours,
  as the other rows are built. The rule: a row with a chromatic counterpart
  differs from it in hue by at least `group_a` (40°, measured round the
  circle); Elevation, whose counterpart is gray, is chromatic; Heading, Pitch
  and Roll differ pairwise by at least 40°. Today's fused `accH` (hue 20
  against the GNSS hue 30) fails the rule and changes.
- **The block comment** says why the rows are what they are: named, united
  and typed as their GNSS counterparts so the two overlay on one axis, in
  colours that tell them apart; every row waits on the one fit. It no longer
  mentions "GNSS (Local frame)".
- Nothing else in the function changes: the other categories, their colours,
  the independent variables, the position of the block.
- Per-plot settings of removed rows (`state/plots/Fusion/qx`, their colour
  preferences) stay in the user's settings unread. Nothing migrates or deletes
  them, as nothing rewrites a profile.

No other reader of the plot registry (`logbookcolumn.cpp`,
`addcolumndialog.cpp`, `plotssettingspage.cpp`, `PlotWidget.cpp`,
`LegendPresenter.cpp`, `measuretool.cpp`, `pluginhost.cpp`) or shipped
profile names a local-frame or fusion row; none changes.

### The profile rule (`src/plotmodel.h`, `.cpp`; `src/profilestatebridge.cpp`)

`profilestatebridge.cpp` is compiled into the application executable only
(`PROJECT_SOURCES` in `src/CMakeLists.txt`) and includes `mainwindow.h`, so
no test can link it. `plotmodel.cpp` is in `flysight_core`, which every test
links. The enabled-plots step therefore moves into `PlotModel` (decision 14):

```cpp
void PlotModel::setEnabledPlotIds(const QStringList &plotIds);
```

Its contract comment says, in prose:

- every plot of the model is enabled exactly when its id
  (`sensorID + "/" + measurementID`, the `PlotValueIdRole` form a profile
  stores) is in `plotIds`, and disabled otherwise;
- each change goes through `setPlotEnabled()`, so the settings key and
  `dataChanged` follow as for a click, and whoever watches the model sees a
  profile exactly as a click;
- an id for which the model has no plot is ignored: no message, no failure,
  no row added. A profile may name plots the application no longer has, or
  those of a plug-in that is not loaded, and applies without them;
- the list is only read; nothing rewrites the profile it came from.

It walks the model's own rows. In the application these are
`PlotRegistry::dependentPlots()` (`m_plotModel->setPlots(...)` in the main
window), which today's loop walks, so the behaviour is unchanged; the order of
the calls does not matter, because the demand layer orders plots by the
model. `applyProfile()` step 1 becomes one call,
`mainWindow->plotModel()->setEnabledPlotIds(*profile.enabledPlots)`, with a
comment that the rule is the plot model's. `captureCurrentState()` is
unchanged.

The audit's `demand` and `widget-free-core` rules cover `src/plotmodel.*`:
the new code includes no demand-layer header, names no `JobQueue` or
`JobModel`, and includes no widget.

### The removed plots' measurements (decision 13)

No code changes. Each measurement behind a removed plot is still registered
and computed as before. What reads such a measurement without a plot row:

- **A logbook column.** A column kept in a saved column list or a profile
  computes as any column does, labelled with the fallback of
  `logbookColumnDisplayName()`, `Fusion/roll @ <marker name>`. The Add Column
  dialog, which lists plot rows, no longer offers it.
- **"Export".** The application has no export of computed values.
  `DataExporter` writes stored data only and never did write `Fusion/*` or a
  computed `Local/*`; that stays true. The fit's own export is its stored
  result: `CalculationResultStore` writes `exportResult()`'s snapshot as the
  record beside the session, and it carries every fit channel, those without a
  plot included. The test asserts this.
- **Plug-ins** read a measurement by declaring it an input (`meas('Fusion',
  'roll')`), resolved by the calculation registry; the plot registry is not
  involved. No test: a plug-in test cannot link the fusion library.

### The tests' mirror (`tests/fusion/fusionsessions.h`, `.cpp`)

`fusionPlots()` returns the eight rows of the table, in order, with the same
name, units, measurement and type, category "Sensor fusion", sensor "Fusion",
colours default. Its comment says it mirrors the application's eight rows and
that `audit_cleanup` pins the application's list at eight. The file's head
comment keeps listing its users. `fusionMeasurementNames()` and `fusionNames()`
do not change: they are the fit's outputs, not the plot list.

## Interfaces

**Provides (to phase 6):**

- The eight rows above, in order.
- `PlotModel::setEnabledPlotIds(const QStringList &)` and its contract, called
  by `applyProfile()`.
- The test functions and the audit group listed under "Names for phase 6".

**Consumes:** phase 3's `Fusion/z`, `accAlongTrack`, `accCrossTrack`
(`Fusion/accD` and `accH` as the fit leaves them); phase 4's
`Fusion/bodyHeading`, `bodyPitch`, `bodyRoll` and the `_ORIENTATION` default;
phase 2's wind defaults, through the track accelerations.

## Acceptance criteria

1. The "Sensor fusion" category of `registerBuiltInPlots()` has exactly the
   eight rows of the table, in order, with those names, units, measurements
   and types; no row has category "GNSS (Local frame)". (§5, §11)
2. Each fused colour differs from its counterpart's by the colour rule above;
   the colours come from the palette variables or Qt named colours.
   (decision 12)
3. `fusionPlots()` equals the application's eight rows field for field except
   colour. (§13)
4. For each of the eight measurements, `explicitDependencies()` is exactly
   `{builtin.fusion.fit}`; checking all eight with one visible fusable session
   (with a stored ground elevation) creates one fit job; before it runs every
   row is merely uncomputed; after it every row's value is non-empty and has
   the length of `Fusion/_time`, and the fit ran once. (§13 "every fusion plot
   is explicit-backed")
5. Checking Heading, Pitch and Roll for one session creates one job and one
   counted session. (§11, §13)
6. `setEnabledPlotIds()` enables exactly the listed plots the model has,
   disables its others, adds no row, and emits no message of any type for ids
   it does not have; `applyProfile()` step 1 is that call and nothing else.
   (§5 "Profiles", §13; decision 14)
7. With the eight rows as the plot list, a column over `Fusion/roll` at the
   exit marker and one over `Local/north` at the exit marker compute (the roll
   column with the value of `getAttribute(fusionRollAtExit())`), labelled
   `Fusion/roll @ …` and `Local/north @ …`, while a column over
   `Fusion/bodyRoll` is labelled `Roll @ …`; the fit's stored record carries
   every fit channel behind a removed plot with the samples the session reads.
   (§5, §13; decision 13)
8. `audit_cleanup` passes with the rules below, each proven once by a planted
   hit; no other rule is loosened. `tests/acceptance_map.txt` resolves.
9. The documents below describe the eight plots, no local-frame category and
   the profile rule. The whole suite passes sequentially in `build-agent/`,
   the `_exact` runs included; the fit, its goldens and stored-result tests
   are unchanged (decision 17).

## Tests

No executable is added; `tests/CMakeLists.txt` does not change.

### `tst_calculation_demand` (every build, no GTSAM)

- **New `profileNamingRemovedPlotsAppliesWithoutThem`**: criterion 6. On the
  fixture's `PlotModel` (`PlotFixture::plots()`), enable a plot the profile
  will not list, then call `setEnabledPlotIds()` with two of the model's ids
  and the removed ids `Fusion/qx`, `Fusion/roll`, `Local/north` (and one
  unknown plug-in id). The listed plots are enabled, every other model plot
  is disabled, the category and row counts are unchanged, `isPlotEnabled` is
  false for the removed ids, and no message of any type (debug, info,
  warning, critical) is emitted during the call. Observe that with a message
  handler installed around the call; `WarningCapture` (`testutil.h`) shows
  the pattern but counts warnings and criticals only. No session is shown, so
  no demand is involved.
- **Amended `profileStyleApplyCreatesDemand`**: its imitation loop becomes one
  `setEnabledPlotIds()` call with the same five ids, so the test runs
  `applyProfile()`'s real step. Name unchanged (items 116, 514, 556).

### `tst_fusion_rows`

- `init()` sets `fusionPlots()` alone. `localFramePlots()` and every use of it
  go.
- `initTestCase()` registers the eight rows of `fusionPlots()` in
  `PlotRegistry::instance()`, once, so column labels resolve as in the
  application (nothing else in this executable reads that registry).
- **`allSeventeenFusionPlotsAreExplicitBacked` → `allEightFusionPlotsAreExplicitBacked`**:
  criteria 3 and 4. It pins `fusionPlots()` to the table literally, asserts
  the registry half, stores `_GROUND_ELEV` on `s2` before adding it, and keeps
  the one-job, progress and during-the-fit assertions. The local-frame half
  becomes one read: `Local/north` still reads normally beside the unrun fit.
- **`rollPitchYawShareOneJob` → `headingPitchRollShareOneJob`**: criterion 5,
  checking `bodyHeading`, `bodyPitch` and `bodyRoll`; after the fit the three
  values are non-empty.
- **New `removedPlotMeasurementsStayAvailable`**: criterion 7. One
  `coarse_linear` fixture session with its exit marker (`fixtureSession()`),
  no plot checked, not shown. Set the logbook columns to the description, a
  column over `Fusion/roll` at `_EXIT_TIME` (type "angle") and one over
  `Local/north` at `_EXIT_TIME`, restoring `{descriptionColumn()}` with a
  scope guard as `tst_fusion_store` does. Column demand fits the session;
  after `waitDemandIdle()` both cells hold numbers, the roll cell's equal to
  `getAttribute(fusionRollAtExit())` (a fixture stores `Local/*` as source
  data; `tst_local_coordinates` proves the calculation behind it). The labels
  are as criterion 7. Then
  `LogbookManager::readCalculationRecord("s2", kFit)` is `Ok` and its bundle
  has `north`, `east`, `down`, `velN`, `velE`, `velD`, `accN`, `accE`,
  `roll`, `pitch`, `yaw`, `qx`, `qy`, `qz`, `qw` available, each
  `sameBitsEverywhere` with the session's read. The fit ran once.
- Every other function that checks `roll` to create demand (`realRowScript`,
  `rejectedTrackShowsBadge`, `editsAndVisibilityDuringFit`) checks the Roll
  row, `bodyRoll`, instead: `setPlotEnabled` ignores a plot the model lacks,
  so a check of `roll` would now create no demand. Reads of the fit's own
  channels (`fusion("s3", "roll")`, the golden comparisons) stay.
  `accHRowIsBlockedByFusion` and `noImuSessionIsNeverCounted` keep their
  bodies over the eight rows. The file's head comment and the comments that
  say "seventeen" or "local-frame plots" follow.

### `tst_fusion_session`

`plotNames()` becomes the measurement behind each of the eight rows, built
from `fusionPlots()`, with its comment. `missingInputsAreNotApplicable`
keeps asserting `NotApplicable` for each; the derived rows are
`NotApplicable` there because the fit has a missing input. `registrationShape`
is untouched here.

### `tst_fusion_jobs`

`noImuSessionCannotHaveAJob`: the seventeen names become `fusionPlots()`'s
eight (`QCOMPARE(plots.size(), 8)`), each `NotApplicable`. `rollColumn()` and
the column tests over `Fusion/roll` stay: they are the kept-column case of
decision 13.

### `tst_fusion_store`

- `rollAtExitColumn()` takes its type as the literal "angle", with a comment
  that it is a column kept from before, over a fit channel without a plot; it
  no longer looks the type up in `fusionPlots()`.
- Every `check("roll")` becomes `check("bodyRoll")`, the Roll row; blocker
  and environment checks over `Fusion/roll` stay (the fit's output). The head
  comment's "the roll plot" says the Roll plot.
- Everything else passes unchanged; only `runCount(kFit)` is asserted, so the
  attitude's on-demand evaluation after a fit changes no expectation.

### Audit (`tests/audit/cleanup_audit.cmake`, group `naming`)

Replace the two counts and add two rules, all under `audit_group(naming)`,
each with an "Allow:" comment:

- `expect_count("eight fusion plots" "^ *\\{\"Sensor fusion\", " 8 src/mainwindow.cpp)`.
  The Allow comment keeps today's: the count pins the application's list to
  `fusionPlots()`, and a plot is added to both and to the number
  (`PLANS/fusion-accuracy.md` will).
- `expect_none("no local-frame plots" "GNSS \\(Local frame\\)|localFramePlots" src tests ":!tests/README.md")`.
- `expect_none("the removed fusion plots stay out of the registry"
  "\"Fusion\", *\"(north|east|down|velN|velE|velD|accN|accE|roll|pitch|yaw|q[xyzw])\", *\"" src)`.
  The trailing `, "` matches a plot row's measurement followed by its type,
  never a calculation's `("Fusion", "down")`.
- `expect_none("the documents describe the eight fusion plots"
  "[Ss]eventeen( real)? plots|GNSS \\(Local frame\\)|Quaternion [WXYZ]|quaternion plots" docs README.md)`.
  Allow: describe the fit's outputs as outputs or measurements; name no
  removed category or plot.

The header's list of facts gains a clause: the sensor fusion category is the
eight plots of the tests' mirror, and no removed fusion plot, no local-frame
plot and no document describing either remains. `tests/README.md` section 10
is phase 6's.

### Acceptance map (decision 15)

Rename in place, the lines keep their items: line
`115 tst_fusion_rows rollPitchYawShareOneJob` → `headingPitchRollShareOneJob`;
`115 …` and `509 tst_fusion_rows allSeventeenFusionPlotsAreExplicitBacked` →
`allEightFusionPlotsAreExplicitBacked`. No line is added.

### `tests/README.md` section 1

- `tst_fusion_rows`: the eight plots of `fusionPlots()` (the audit pins
  eight); all eight explicit-backed and every one drawn after the fit;
  heading, pitch and roll share one job; the removed plots' measurements
  still serve columns, with the fallback label, and the fit's record; no
  local-frame sentence.
- `tst_calculation_demand`: a profile naming plots the model does not have
  enables its others and ignores those, silently
  (`profileNamingRemovedPlotsAppliesWithoutThem`).
- `tst_fusion_store`: "while roll is checked" names the Roll plot.
- `audit_cleanup`: the plot list's eight fusion plots, no local-frame or
  removed fusion plot in code or documents.

Counts are unchanged.

### `tests/README.md` section 9 (the renamed functions only)

In the section 9.2 row of item 115 ("row script with the real fusion plots;
...") and the section 9.6 row of item 509, replace the two old function names
with the new ones, and nothing else. Restating the rows' wording and the items
"(as amended)" is phase 6's, as are sections 10, 12 and the appendices.

### Documentation

- **`docs/SENSOR_FUSION.md` §2.** The plot-list bullet: the eight plots,
  named, united and typed as their GNSS counterparts so that they overlay, in
  their own colours; which is derived how in one clause each (§4 has the
  detail). The fit's other outputs (position, velocity, north and east
  acceleration, the device-frame roll, pitch and yaw, the quaternion) have no
  plot and remain measurements: a logbook column kept from earlier still
  shows them, plug-ins read them, and the stored fit keeps them. A profile
  saved with a plot the list no longer has applies without it. "All
  seventeen plots … come from the same fit" says all eight. Phase 4's
  attitude paragraph stays.
- **`docs/CALCULATIONS.md` §17.** "**Plots.**": the eight rows and their
  measurements; `accD` is a fit output, the others on demand and blocked by
  the fit; all requested (16.3); the rest of the fit's outputs have no plot.
  The test paragraph's "the seventeen real plots" says eight.
- **`docs/CALCULATIONS.md` §16.4 "Profiles."**: the rule and its owner,
  `PlotModel::setEnabledPlotIds()`.
- **`docs/COMPUTED_PLOTS.md`.** §1 names the category's eight plots; §2's
  "Roll, pitch and yaw … come from one and the same computation" says
  heading, pitch and roll; check every other mention of fusion plots and
  columns ("roll at the exit marker" stays true: it is the Roll plot's value).
- **`docs/LOCAL_COORDINATES.md`.** §1: the frame has no plots of its own; it
  is the input of sensor fusion and of the simplified map track, and a
  column's source. §4: "the plots that show it are ordinary plots" says reads
  of it are ordinary. §10 adds that the fused elevation reads
  `_LOCAL_ORIGIN_HMSL`.
- **`README.md`** (root), the documents list: `LOCAL_COORDINATES.md` is "the
  recording-wide north/east/down frame that sensor fusion and the simplified
  map track build on".

## Decisions

- **D1. The rule lives in `PlotModel`**, the owner of check state in
  `flysight_core`, which tests link. A member, because it uses the model's
  private id form (`makePlotId`), the one authority for it.
- **D2. The profile test is not a fusion test.** The rule does not depend on
  which plots exist, so it runs in every build, with the removed plots' real
  ids.
- **D3. Tests that checked `roll` check `bodyRoll`**, so they keep modelling
  the application's list rather than a test-only row.
- **D4. A colour rule, not colours**: decision 12 leaves the choice here, and
  a measurable rule keeps criterion 2 free of taste.
- **D5. One audit group.** The new rules join `naming`, which already pins the
  plot list and searches the documents; `fusion-model`'s document rule reads
  one file and is about the fit.
- **D6. Renamed functions.** The old names would lie. The map lines and the
  function names in the `tests/README.md` section 9 rows of items 115 and 509
  follow here (decision 15), so no document cites a function that does not
  exist; phase 6 restates the rows' wording.
- **D7. Export is the stored record**: the one place a fit channel leaves the
  process, since no user-facing export of computed values exists.

## Names for phase 6

- `tst_calculation_demand::profileNamingRemovedPlotsAppliesWithoutThem`,
  `profileStyleApplyCreatesDemand` (amended).
- `tst_fusion_rows::allEightFusionPlotsAreExplicitBacked`,
  `headingPitchRollShareOneJob`, `removedPlotMeasurementsStayAvailable`
  (with `accHRowIsBlockedByFusion`, `noImuSessionIsNeverCounted`,
  `realRowScript` over the eight rows).
- `tst_fusion_session::missingInputsAreNotApplicable` and
  `tst_fusion_jobs::noImuSessionCannotHaveAJob` over the eight rows.
- Audit group `naming`: "eight fusion plots", "no local-frame plots", "the
  removed fusion plots stay out of the registry", "the documents describe the
  eight fusion plots".
- Left for phase 6: the wording of the section 9 rows of items 115 and 509,
  section 10's description of the `naming` counts, and appendix B wording on
  "seventeen" and local-frame plots where phase 6 restates items 115 and 509
  "(as amended)".

Ready.
