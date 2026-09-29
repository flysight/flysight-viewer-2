# Phase 6: Traceability and the test guide

## Purpose

Spec §13, and the test-guide half of §14 (overview decision 15). Phases 1-5
changed the product, added the tests, amended the map lines they renamed, and
updated the documents each one owned. This phase does the rest:

- it states the specification's acceptance items, 801-863, in appendix I, in
  section 9.9 and in `tests/acceptance_map.txt`, each traced to a test
  function, audit group or manual step that phases 1-5 created or kept;
- it restates the earlier items this specification changed "(as amended)";
- it makes section 10 describe the audit rules phases 2, 4 and 5 added or
  changed, and adds section 12.7's manual steps;
- it extends the audit's traceability range to the new items;
- it checks the documents under `docs/` as a whole, closes the cross-document
  consistency items below, and reports any statement a phase left missing.

**No code changes.** The files touched are `tests/README.md`,
`tests/acceptance_map.txt`, `tests/audit/cleanup_audit.cmake` (comments and
the traceability block only), `docs/*.md` and the root `README.md`. No file
under `src/`, no `tests/*.cpp`, no support file and no `CMakeLists.txt`
changes. A stale comment found in `src/` or `tests/` is reported to the
orchestrator as a fixup of the phase that left it, not fixed here.

## Dependencies

- **Depends on phase 5** (tag `plan/fusion-plots/phase-5-done`), and through
  it on phases 1-4. **Blocks nothing.**
- **The tree is the authority for names.** Phase 1's function names were only
  proposals; phases 3, 4 and 5 fixed theirs. Before writing a map line, list
  what the tree holds, for example
  `grep -o -E "::[A-Za-z_0-9]+\(\)" tests/tst_choice_attribute.cpp tests/tst_fusion_derived.cpp tests/tst_fusion_rows.cpp`
  and the `private slots:` of `tests/tst_calculation_demand.cpp` and
  `tests/tst_builtins_engine.cpp`. Cite the function that asserts the fact. If
  no function asserts a fact that a row below names, report it; never drop a
  line to make the audit pass.
- **May assume**, as phases 1-5 state them:
  - `tst_choice_attribute`, a Widgets test, with the choice tests;
  - `tst_fusion_derived`, a fusion test, with the derived-kinematics, attitude
    and orientation-column tests;
  - `tst_builtins_engine::constantDefaults`;
  - `tst_calculation_demand::profileNamingRemovedPlotsAppliesWithoutThem`;
  - in `tst_fusion_rows`: `allEightFusionPlotsAreExplicitBacked`,
    `headingPitchRollShareOneJob` and `removedPlotMeasurementsStayAvailable`;
  - the map lines phase 5 renamed (items 115 and 509);
  - the audit groups `constant-defaults` (phase 2) and `orientation` (phase
    4), `naming` as phase 5 left it, and `solver-confinement`'s GTSAM-free
    list with `src/fusion/orientation.*`.
- **Text the audit searches in `tests/`.** Two of the new rules search
  `tests/` and so read `tests/acceptance_map.txt`:
  - phase 5's "no local-frame plots" excludes only `tests/README.md`;
  - phase 2's "the replaced default helpers stay gone" excludes nothing under
    `tests/`.

  So the map writes "the local-frame category", never the category's quoted
  name, and names neither replaced helper. For the same reason,
  `tests/README.md` (section 10 included) never spells the two helper names.
  The README may name the "GNSS (Local frame)" category.
- **Cite the specification by its title**, "Sensor fusion plots, attitude and
  the orientation attribute", not by path. It moves to `PLANS/done/` when the
  work is finished.

## What changes

### `tests/acceptance_map.txt`

**Head.**

- "Eight specifications, eight item ranges" becomes nine.
- Three entries each gain one sentence, after their existing amendment
  sentences:
  - 1-19: "It is amended by the specification of 801-863: item 6 is stated as
    amended."
  - 101-120: "It is amended by the specification of 801-863: item 115 is
    stated as amended."
  - 501-563: "It is amended by the specification of 801-863: item 509 is
    stated as amended."
- A new entry, in the form of 701-754's: `801-863` sensor fusion plots,
  attitude and the orientation attribute (the eight fusion plots, the derived
  kinematics, heading, pitch and roll on the body frame, the orientation
  attribute, the choice attribute type, constant defaults). It says: item =
  800 + clause number; stated in full in tests/README.md, appendix I; it
  amends the specifications of 1-19, 101-120 and 501-563.
- The sentence on the ranges gains 801-863, and "sections 9.1 to 9.8" becomes
  9.9.

**New block.**

- It goes at the end, after
  `# ---- Sensor fusion plots, attitude and the orientation attribute: item = 800 + clause number ----`.
- One block per item of the table under Tests:
  - the comment line `# <item> - (<section>) <clause>`, or
    `(<section>, as settled)`;
  - then its lines, in the four line forms.

**Amended items.** Rewrite each comment line in place, keep every line that
still resolves, and add the lines given under "Amended items" below.

### `tests/README.md`

**Section 9 intro.** "Eight specifications, eight ranges ... the eight tables"
becomes nine.

**The intros of 9.1, 9.2 and 9.6.** Each names the clause that is restated as
amended by "Sensor fusion plots, attitude and the orientation attribute"
(9.9): item 6, item 115 (again), and clause 9.

**Rows.**

- The amended rows are restated as below.
- The 9.2 row of item 11 is corrected, but not restated: its evidence cell's
  "(all seventeen real rows)" becomes "(all eight real rows)". Clause 11 names
  no plot, so its meaning is unchanged.
- No Evidence cell anywhere in 9.1-9.8 names a function the tree no longer
  has.

**New 9.9 "Sensor fusion plots, attitude and the orientation attribute
(items 801-863)".** It goes after 9.8 and takes 9.8's form. Its intro says:

- the sixty-three clauses, stated in full in appendix I;
- item = 800 + clause number, the same four line forms, and every item has at
  least one test or audit line;
- sections 1-4 (motivation, principles, scope, terms) have no item. Their
  principles are carried by the clauses of the sections that apply them, and
  the two statements of §3 that a test can observe are clauses 4 and 40;
- clauses 54-62 are its §13 tests, one per bullet, and clause 63 is its §14;
- clauses 2, 7, 8, 10, 14, 22, 26, 36, 39 and 50 are stated as settled, each
  with one phrase on how (see Decisions);
- it amends those of 9.1 (item 6), 9.2 (item 115) and 9.6 (item 509).

Then the table `| # | Section | Clause | Evidence |`, with 63 rows. Evidence
takes the full `tst_x::function` form and the map's lines, as 9.8 does.

**Section 10.** Keep the section's style: one bullet per group, one clause per
rule, and the group's items in parentheses.

- **New bullet, group `constant-defaults` (items 835-839, 851).**
  - A compute function that ignores its context is a constant default, and
    only the one helper `addConstantDefault` in `attributecalculations.h`
    writes one.
  - The local helpers the SP and WS-P calculations used before reappear in
    `src` or `tests`. Describe these; do not name them (see Dependencies).
  - The importer or the legacy backfill (`dataimporter.*`,
    `logbookmanager.*`) names wind.
- **New bullet, group `orientation` (item 821).** An orientation token (a
  signed axis, a comma, a signed axis) is spelled in `src` anywhere but
  `src/fusion/orientation.*`: one type owns the mount vocabulary.
- **`naming`.** The bullet gains its items (120, 801, 803, 852, 862, 863).
  "`MainWindow` no longer registers exactly seventeen ... and six ... plots"
  becomes four rules:
  - `MainWindow` does not register exactly eight "Sensor fusion" plots (the
    count pins the application's list to `fusionPlots()`);
  - the "GNSS (Local frame)" category or the tests' old helper for it appears
    in `src` or `tests`;
  - a plot row names a removed fusion measurement (the position, velocity,
    north and east acceleration, the device-frame roll, pitch and yaw, the
    quaternion);
  - `docs` or the root `README.md` describe seventeen plots, the local-frame
    category or a quaternion plot.

  Keep "This file is excluded".
- **`solver-confinement`.**
  - The bullet gains its items (101, 234, 236, 848, 850).
  - "the public or registration files of the fusion library include GTSAM or
    Eigen" names them: `fusion.h`, `fusionregistration.*` and the orientation
    type `orientation.*`.
  - Add that the registration and the orientation type are the kernel's only
    files that may see the engine or the session keys, if phase 4 left the
    rule that way. Describe the rule as it is.
- **`fusion-tooling`.** Its items become 231, 233, 848.
- **`demand`.** Its item list gains 853.
- **The traceability bullet.** It gains 801-863, both in the "outside" list
  and in the "has no test or audit line" list.

**Section 12.**

- "Six scripts." becomes "Seven scripts."
- M3 is restated in place. "Pitch and Yaw, if checked, add nothing to the
  count (one computation per track serves all three)" becomes "Heading and
  Pitch". There is no Yaw plot. Its bold id and item list stay.
- The other steps that check "Roll" or add "roll at the exit marker" stay
  true, because the Roll plot exists. Read each once to confirm it.
- New **12.7 Sensor fusion plots, attitude and the orientation attribute**
  goes after 12.6, with 12.6's preamble form: what the automated tests cannot
  show (the real plot list and its colours, real fits through turns, the
  logbook's editors on a real logbook, an old profile and an old column);
  the 12.1 preamble (a COPY of a logbook); the recordings of 12.4, plus one
  with several turns. The steps continue the ids, each opening
  `**M<k> <title> (<items>).**`:
  - **M39 The eight fusion plots (801, 802, 803, 842).**
    - The plot list has "Sensor fusion" with Elevation, Horizontal
      acceleration, Vertical acceleration, Along-track acceleration,
      Cross-track acceleration, Heading, Pitch and Roll, in that order, and
      no "GNSS (Local frame)" category.
    - With two fusable tracks without stored fits visible, check all eight:
      the status bar shows "Computing results: 0 / 2", and one fit per track
      fills all eight.
    - Check each GNSS counterpart as well (Elevation; the four accelerations;
      Course): each pair shares one axis and unit, and the two lines are told
      apart by colour. Pitch and Roll share the angle axis with Heading.
    - Hide and show a track: its plots draw at once, with no computation.
    - Check this in the light and the dark theme.
  - **M40 Attitude through turns (812, 814, 843).** On a recording with
    several turns, with the default orientation and a unit on the back of a
    helmet, label up:
    - Heading shows no jump of 360 degrees at north. It overlays Course in
      straight flight and turns with it through every turn; the difference
      between them, read with the measure tool at a few points, is the
      sideslip and changes slowly.
    - In straight flight roll is near zero. It is positive in a right turn
      and negative in a left one.
  - **M41 The orientation column (817, 819, 822, 823, 824, 829, 844).**
    - The Add Column dialog, "Session Attribute", lists "Orientation" in
      the "Session" group. A new logbook's default columns do not include it.
    - Add it: every row shows "forward +y, up +z". A text editor shows no
      `$VAR,_ORIENTATION` line in any session file. Import a new recording:
      the same.
    - Double-click a cell: a drop-down list opens, "Default" first and then
      the 24 labels, and it accepts no typing. Choose "forward +x, up +z":
      - the cell shows it;
      - the session file gains `$VAR,_ORIENTATION,+x,+z`;
      - with Heading, Pitch and Roll checked for that track, they redraw at
        once;
      - no "Computing results" appears, and the track's record file in
        `cache/` keeps its modification time.
    - Choose "Default": the line is gone and the plots are back.
  - **M42 Setting it for several recordings (823, 824, 826, 830, 831).**
    - Select rows that are loaded and rows that are not, right-click, and
      choose "Set Orientation...". A list opens, "Default" first, with no
      text box. Choose a label: every selected row shows it, and every file
      has the line. Choose "Default": the lines go. Cancel: nothing changes.
    - Then, with the application closed, edit one session file's line to
      `$VAR,_ORIENTATION,sideways`. Start: its cell shows "sideways" and its
      Heading, Pitch and Roll draw nothing. Choosing a label in the cell
      draws them.
  - **M43 An old profile (805, 845).**
    - With the application closed, add `Fusion/qx`, `Fusion/roll` and
      `Local/north` to the `enabledPlots` list of a copy of a profile file in
      `Documents/FlySight Viewer/profiles/`, beside a GNSS plot it already
      lists. Note the file's modification time.
    - Start and apply the profile: the GNSS plot is checked, and Sensor
      fusion > Roll is not. No message box appears, and the debug output
      has no line about the three ids.
    - The file keeps its bytes and modification time.
  - **M44 A column kept from before (804).**
    - Set up a logbook copy with a build from before this change, with a
      column over Sensor fusion > Roll at the exit marker, filled.
    - Start this build: the column still shows its values, labelled
      "Fusion/roll @ <marker>", with no fit.
    - The Add Column dialog's measurement tree lists eight Sensor fusion
      measurements and no local-frame group.

**Section 1.** Two rows need wording, and cited rows gain their items.

- `audit_cleanup`: add that a default standing in for an unset value is a
  calculation, a constant one registered by one helper, and that one type
  owns the mount vocabulary.
- `tst_persistence_roundtrip`: "the `loadSession` backfill is additive and
  idempotent" gains "(mass and area; wind is not backfilled)".
- Where a row already names items (the `tst_fusion_rows` form "(11)",
  "(status-bar item 753)"), the rows of the executables the new block cites
  name their 8xx items in that form: `tst_choice_attribute`,
  `tst_fusion_derived`, `tst_fusion_rows`, `tst_calculation_demand`,
  `tst_builtins_engine`.
- The counts do not change.

**Appendices A, B, F.** Each amended clause is restated in place, and each
intro names the clause amended by "Sensor fusion plots, attitude and the
orientation attribute" (appendix I).

**New appendix I** goes at the end:
`## Appendix I. The acceptance items of sensor fusion plots, attitude and the orientation attribute (801-863)`.

- The intro follows appendix H's:
  - the testable statements of the specification, which amends appendices A,
    B and F, one sentence each, with the specification's section number in
    front;
  - the numbers are this list's, and item = 800 + the number;
  - which sections have no item, and the two §3 statements in clauses 4
    and 40;
  - clauses 54-62 are the §13 tests and 63 is §14;
  - the settled clauses (9.9 says how).
- Then "1. (5) ..." to "63. (14) ...". Each is the clause of the map's
  comment line, capitalized and ending with a full stop.

### `tests/audit/cleanup_audit.cmake`

- **File head.** Phases 2, 4 and 5 each added a clause to the header's list.
  Gather them into one bullet after the 701-754 one, ending "(items
  801-863)", so that no fact is stated twice. The bullet says:
  - a default that stands in for a value the user has not set is a
    calculation, and every constant one is registered by one helper;
  - neither the importer nor the backfill writes wind;
  - one type owns the orientation vocabulary;
  - the sensor fusion category is the eight plots of the tests' mirror, and
    no removed fusion plot and no local-frame plot remains in code or
    documents.
- **Group comments.** The comment lines of `constant-defaults`, `orientation`
  and `naming` name their items, as `demand`'s does.
- **Traceability.**
  - The comment names 801-863 (item = 800 + clause number).
  - The range test gains `OR (item GREATER_EQUAL 801 AND item LESS_EQUAL 863)`,
    and its message gains the range.
  - Add `foreach(item RANGE 801 863)`, requiring a test or audit line, as for
    701-754.
- **No rule is added or changed.** The rule count the audit reports equals
  the count after phase 5.

### Documents (spec §14): check the whole, close the gaps

Spec §14's list, with the phase that did each part:

| Part | Owner |
| --- | --- |
| `SENSOR_FUSION.md` §2, the plot list | phase 5 |
| `SENSOR_FUSION.md` §2, the attitude and the attribute | phase 4 |
| `SENSOR_FUSION.md` §4 | phases 3, 4 |
| `CALCULATIONS.md` §5 | phase 2 |
| `CALCULATIONS.md` §17 | phases 3-5, with 16.4 for profiles |
| `COMPUTED_PLOTS.md` §1, §2 | phase 5 |
| `DATA_SCHEMA.md` §8, §10 (wind) | phase 2 |
| `DATA_SCHEMA.md` §9, §10 (orientation, choice) | phase 4 |
| `LOCAL_COORDINATES.md` §1, §4, §10 | phase 5 |
| the root `README.md` | phases 4, 5 |
| the audit's document rule | phase 5 |

Read the five documents together and confirm each part, including these
statements that phases 3 and 4 own (report any that is missing as a fixup of
its phase, and do not write it here):

1. `SENSOR_FUSION.md` §8 says which tests have an exact run, every one but
   `tst_fusion_derived` (phase 3).
2. `SENSOR_FUSION.md` §4 says the diagnostics' `orientation` key is the fit's
   own device-frame convention, not the Orientation attribute (phase 4).
3. `SENSOR_FUSION.md` §5 says the Orientation attribute is not an input of
   the fit (phase 4).
4. `COMPUTED_PLOTS.md` §6 says that changing a recording's Orientation keeps
   the stored fit and recomputes heading, pitch and roll without counting
   anything in the status bar (phase 4); and no section of it still names a
   removed plot (phase 5).
5. `CALCULATIONS.md` §17 "Logbook columns" says an orientation edit discards
   a column over the angles and recomputes it from the stored fit (phase 4).
6. `DATA_SCHEMA.md` §11 says what a choice column caches, if it lists caching
   by type (phase 4).

Then close these, which belong to no single phase. Write in each document's
voice; user documents name no classes.

7. **Names across documents.**
   - `builtin.default.<key>`, `addConstantDefault`, `Fusion/bodyHeading`,
     `bodyPitch`, `bodyRoll` and `_ORIENTATION` are spelled the same in
     `CALCULATIONS.md` §5 and §17, `SENSOR_FUSION.md` §4 and `DATA_SCHEMA.md`
     §9.
   - The profile rule is stated once, in `CALCULATIONS.md` 16.4. The other
     documents refer to it or state it for the user without contradicting
     it.
8. **A final sweep.**
   `git grep -n -E -i "seventeen|local frame|quaternion|\byaw\b|wind" -- docs README.md`.
   Every hit describes the fit's outputs or channels, the wind attributes as
   they now are, or the kernel. None describes a plot or an import-time wind
   default.

A gap found beyond this list is closed the same way and reported.

## Interfaces

- **Consumes:**
  - the test functions as the tree holds them after phase 5;
  - the audit groups and rules as phases 2, 4 and 5 left them;
  - the manual step ids up to M38;
  - the documents as phases 2-5 left them.
- **Provides:** nothing to a later phase. The numbering 801-863 and the
  clause texts become the specification's traceability record.

## Acceptance criteria

1. **Audit** (§13). `cmake -DREPO=. -P tests/audit/cleanup_audit.cmake`
   passes with the same rule count as after phase 5. Report both numbers.
2. **Every new item traced** (§13).
   - Items 801-863 each have at least one test or audit line.
   - Every cited function exists as `::<function>()` in its file.
   - `**M39 ` to `**M44 ` each exist once in `tests/README.md`.
   - Two plants each trip the audit, and both are undone:
     - planting `864 audit naming` reports an item outside the ranges;
     - removing every line of one 8xx item reports that item.
3. **Amended items** (§14). Items 6, 115 and 509 read "(as amended)" in the
   map, in their 9.x row and in their appendix, and the head of the map and
   the three intros say which specification amends them. The item-11 cell
   says eight. No text in 9.1-9.9 cites a function the tree lacks.
4. **The new record.** Section 9.9 has 63 rows, appendix I has 63 clauses,
   and the map has 63 comment lines 801-863, all the same in substance. The
   map spells neither the local-frame category's quoted name nor the
   replaced helpers' names.
5. **Section 10.** It describes the `constant-defaults` and `orientation`
   groups, the `naming` group's four plot rules, the `solver-confinement`
   list and the item lists above. It says neither "seventeen" nor "six
   local-frame".
6. **Section 12.** It says "Seven scripts". M3 names Heading and Pitch, not
   Yaw.
7. **Documents** (§14).
   - Statements 1-6 of the documents check are present, or reported as a
     fixup of the owning phase; item 7 is closed.
   - The sweep of item 8 finds nothing describing a removed plot or an
     import-time wind default.
   - `git grep -n -E "[Ss]eventeen (real )?plots|GNSS \(Local frame\)|seven tests a second time" -- docs README.md`
     is empty.
8. **Green.** `cmake --build build-agent --config Release` has nothing to
   rebuild. The whole suite passes sequentially:
   `ctest --test-dir build-agent/FlySightViewer-build -C Release --output-on-failure`.
9. **No code change.** `git status` shows changes only in the files named
   under Purpose.

## Tests

No test is added or changed. The audit's traceability check proves that every
line resolves.

**Abbreviations** in the Lines column:

| Abbreviation | Target |
| --- | --- |
| `ca` | `tst_choice_attribute` |
| `fd` | `tst_fusion_derived` |
| `fr` | `tst_fusion_rows` |
| `fs` | `tst_fusion_session` |
| `fj` | `tst_fusion_jobs` |
| `fst` | `tst_fusion_store` |
| `fg` | `tst_fusion_golden` |
| `fk` | `tst_fusion_kernel` |
| `cd` | `tst_calculation_demand` |
| `be` | `tst_builtins_engine` |
| `bg` | `tst_builtins_golden` |
| `ce` | `tst_calcengine` |
| `im` | `tst_importer` |
| `sm` | `tst_smoke` |
| `imm` | `tst_import_merge` |
| `pr` | `tst_persistence_roundtrip` |
| `lx` | `tst_logbook_index` |
| `a:` | `audit` |
| `M` | `manual` |

The Clause column is the substance; wording may be polished, as long as
appendix I, 9.9 and the map say the same. Items 854-862 state their §13
bullet in full in the map and in appendix I (the table abbreviates them).

| Item | § | Clause | Lines |
|---|---|---|---|
| 801 | 5 | the "Sensor fusion" category holds exactly eight plots, in this order: Elevation, Horizontal acceleration, Vertical acceleration, Along-track acceleration, Cross-track acceleration, Heading, Pitch, Roll | fr allEightFusionPlotsAreExplicitBacked; a: naming; M39 |
| 802 | 5, as settled | each is named, united and typed as its GNSS counterpart (Elevation, the four accelerations, Heading as Course), Pitch and Roll as angles, so that the two overlay; a fused plot's colour differs from its counterpart's | fr allEightFusionPlotsAreExplicitBacked; M39 |
| 803 | 5 | the fused north, east and down position and velocity, north and east acceleration, roll, pitch, yaw and quaternion plots are removed, and the "GNSS (Local frame)" category with them | fr allEightFusionPlotsAreExplicitBacked; a: naming; M39 |
| 804 | 5 | every measurement behind a removed plot still exists and is computed as before: the local frame feeds the fit and a column, and the fit's channels are its record, read by a column kept from before, the stored result and plug-ins | fr removedPlotMeasurementsStayAvailable; fj columnOnFusionOutputIsCachedFromRecord; M44 |
| 805 | 5 | applying a profile enables the listed plots the application has and ignores the rest silently (no message, no log, no failure); nothing rewrites a profile | cd profileNamingRemovedPlotsAppliesWithoutThem; M43 |
| 806 | 6 | each derived quantity is an on-demand calculation over the fit's outputs, registered with the fit: blocked by it until it publishes, it appears with it and never starts one | fd derivedRegistrationShape, derivedValuesWaitOnTheFit; fr accHRowIsBlockedByFusion, allEightFusionPlotsAreExplicitBacked |
| 807 | 6, as settled | elevation is the local origin's height above mean sea level minus the fused down position, above the ground elevation as the GNSS elevation is, so the two overlay; unavailable when either attribute is not a number | fd elevationIsOriginHeightMinusDownAboveGround |
| 808 | 6, as settled | vertical acceleration is the fit's own fused down acceleration, positive down like the GNSS one; no second calculation publishes it | fr allEightFusionPlotsAreExplicitBacked; fs explicitOutputsHaveOneCandidate |
| 809 | 6 | along-track and cross-track acceleration are the GNSS definitions applied to the fused velocity and acceleration, wind-corrected, so their signs and the meaning of "track" agree | fd trackAccelerationsKnownAnswers, trackAccelerationsAreTheGnssDefinitions |
| 810 | 6, as settled | one definition of the track-relative accelerations and of the wind rule serves both categories, and the GNSS values are unchanged | fd trackAccelerationsAreTheGnssDefinitions; bg sessionDataMatchesGolden; be goldenOnEngine |
| 811 | 7 | the orientation's forward and up axes fix the body frame, right being their cross product; the body-to-device rotation is a constant signed axis permutation, a proper rotation for each of the 24 pairs, never a reflection | fd orientationRotationIsProper |
| 812 | 7 | heading, pitch and roll are the aircraft Euler angles of the fit's quaternion composed with that rotation: heading the forward axis's direction clockwise from north, pitch its elevation above the horizontal, roll the rotation about it, positive right side down | fd levelNorthFacingBodyReadsZero, knownAnglesComeBack, sideMountPermutesTheAngles, deviceFrameMountGivesTheFitsOwnAngles; M40 |
| 813 | 7 | they are computed when read, from the quaternion channels and the orientation attribute, never by the fit | fd attitudeRegistrationShape, attitudeWaitsOnTheFit; fs registrationShape |
| 814 | 7, as settled | heading is unwrapped by the application's one unwrap rule, the course's, and offset by the same course-reference angle the GNSS course subtracts, computed in one place for both, so heading and course overlay in straight flight and heading minus course reads as sideslip; heading has course's inputs and availability | fd headingUnwrapsThroughAFullTurn; M40 |
| 815 | 7 | pitch and roll are reported in their natural ranges, pitch from -90 to 90 degrees and roll from -180 to 180 | fd rollAndPitchStayInTheirNaturalRanges |
| 816 | 7 | where the forward axis is vertical the derivation reports what the standard formulas give | fd rollAndPitchStayInTheirNaturalRanges |
| 817 | 7 | a change of the orientation attribute recomputes the angles through ordinary invalidation and never refits | fd storedOrientationRecomputesWithoutAFit; M41 |
| 818 | 8 | the orientation is a token naming the forward and the up axis in a fixed form the fusion module owns (the default `+y,+z`); the 24 valid pairs, and only they, are its choices, labelled "forward +y, up +z" and so on, the default first | fd orientationVocabularyHasTwentyFourPairs, orientationDefinitionIsTheEnumeration |
| 819 | 8 | its default, forward +y and up +z, is a constant calculated attribute: every recording has it, imported before or after, and nothing is written into any session file | fd orientationColumnShowsTheDefaultWithoutAWrite, storedOrientationRecomputesWithoutAFit, attitudeRegistrationShape; M41 |
| 820 | 8 | a stored value wins over the default, and removing it returns to the default | fd storedOrientationRecomputesWithoutAFit, orientationDefaultRemovesTheStoredValue |
| 821 | 8 | one orientation type in the fusion module owns the vocabulary (the axis pair, token, label, body-frame rotation, default and enumeration); the derivation parses with it, the attribute's choices come from it, and nothing else spells a token | fd orientationDefinitionIsTheEnumeration, orientationVocabularyHasTwentyFourPairs; a: orientation |
| 822 | 8, as settled | the fusion registration registers the attribute's definition once per process (key `_ORIENTATION`, "Orientation", category "Session", a choice attribute, editable) with the derivation that reads it; the key is part of the session file's vocabulary | fd orientationDefinitionIsTheEnumeration, attitudeRegistrationShape; M41 |
| 823 | 8 | it is edited as any editable attribute: a logbook column the user can add, not among the default columns, edited in place, and set for the selected sessions from the context menu | fd orientationEditStoresATokenAndRefusesOthers, orientationBulkEdit; ca cellEditorOffersTheList, setDialogOffersTheList; M41; M42 |
| 824 | 8 | both editors offer the list, not free text, and a "Default" entry that removes the stored value rather than storing anything | ca cellEditorOffersTheList, setDialogOffersTheList; fd orientationDefaultRemovesTheStoredValue, orientationBulkEdit; M41; M42 |
| 825 | 8 | a value outside the list is refused by the model, the in-place editor and the bulk edit | fd orientationEditStoresATokenAndRefusesOthers, orientationBulkEdit; ca cellEditorOffersTheList |
| 826 | 8, as settled | a stored token outside the list (a hand-edited file) is kept and shown as written, and heading, pitch and roll are unavailable for it until it is changed or removed | fd invalidStoredOrientationMakesAttitudeUnavailable; ca choiceShowsTheLabelOfTheEffectiveToken; M42 |
| 827 | 9 | the attribute registry's format types gain a fifth, choice, whose definition carries its allowed values, each a stored token and a display label | ca choiceShowsTheLabelOfTheEffectiveToken, choiceEditStoresAToken; fd orientationDefinitionIsTheEnumeration |
| 828 | 9 | a choice is displayed and sorted by its label | ca choiceShowsTheLabelOfTheEffectiveToken, choiceSortsByLabel |
| 829 | 9 | in-place editing of a choice gives a list editor in place of the line edit | ca cellEditorOffersTheList; M41 |
| 830 | 9 | the context menu's "Set ..." action offers a list in place of the text prompt | ca setDialogOffersTheList; M42 |
| 831 | 9 | the bulk edit sets the chosen token for the selected sessions, loaded or not | ca bulkEditSetsAToken; fd orientationBulkEdit; M42 |
| 832 | 9 | "Default" removes the stored attribute on every edit path, and a token outside the list is refused | ca choiceDefaultRemovesTheStoredValue, bulkEditDefaultRemovesTheStoredValue, choiceEditRefusesATokenOutsideTheList, bulkEditRefusesATokenOutsideTheList |
| 833 | 9 | nothing about the type is specific to orientation: any choice definition gets all of it | ca choiceEditStoresAToken, cellEditorOffersTheList |
| 834 | 10 | a constant default is a registered calculation with no inputs whose one output is the attribute: it runs once and is cached, a stored value wins, setting or removing a stored value invalidates what read the attribute, and a dependent follows every change | ce constantCalculationIsADefault; be constantDefaults |
| 835 | 10 | one helper registers a constant default for an attribute key and a value, beside the exit-time defaults, so that every constant default is found by one search; the orientation default uses it from the fusion registration | be constantDefaults; fd attitudeRegistrationShape; a: constant-defaults |
| 836 | 10, as settled | the seven constant defaults of the SP and WS-P calculations are registered through the same helper, with their values | be constantDefaults, inventory; a: constant-defaults |
| 837 | 10 | the importer stores only what is a fact of the import; anything that stands in for a value the user has not set is a calculation, derived where possible, constant otherwise | im creationDefaultsOnlyFillAbsent; sm importAppliesCreationDefaults; a: constant-defaults |
| 838 | 10 | wind north and east are constants of zero: a recording without stored wind reads zero, one with stored wind keeps it, and the importer no longer writes them | be constantDefaults; sm importAppliesCreationDefaults; im creationDefaultsOnlyFillAbsent; imm newSessionGetsDefaults; a: constant-defaults |
| 839 | 10, as settled | the logbook's legacy backfill no longer writes wind either; it adds jumper mass and planform area only | pr releasedLogbookBackfillIsAdditive; lx rawLoadSkipsBackfill; a: constant-defaults |
| 840 | 10 | jumper mass, planform area and the fixed ground elevation stay creation defaults of the importer (section 3) | im creationDefaultsOnlyFillAbsent, fixedGroundElevationOnlyInFixedMode; sm importAppliesCreationDefaults |
| 841 | 10 | a stored attribute wins even when invalid or empty, so returning to a default removes the stored attribute and never stores a blank | ce storedInvalidValueStillWins, removingStoredFallsBackToCalc; ca choiceDefaultRemovesTheStoredValue, bulkEditDefaultRemovesTheStoredValue |
| 842 | 11 | the plot list shows "Sensor fusion" with eight plots named as the GNSS plots and no local-frame category; checking a fusion plot starts the fit as before, the status bar shows it, and a stored fit draws at once | fr allEightFusionPlotsAreExplicitBacked, realRowScript; fst restoredAfterRestartIsBitIdentical; M39 |
| 843 | 11 | the attitude plots are in the aircraft convention for the mount the orientation describes, heading continuous through turns | fd sideMountPermutesTheAngles, headingUnwrapsThroughAFullTurn; M40 |
| 844 | 11 | the orientation column, once added, shows "forward +y, up +z" for every recording not set and the chosen label for one that is; editing offers the list and "Default"; changing it redraws the attitude plots without a fit | fd orientationColumnShowsTheDefaultWithoutAWrite, orientationEditStoresATokenAndRefusesOthers, storedOrientationRecomputesWithoutAFit; ca cellEditorOffersTheList; M41 |
| 845 | 11 | profiles that name removed plots apply without complaint | cd profileNamingRemovedPlotsAppliesWithoutThem; M43 |
| 846 | 11 | wind reads zero where nothing was stored, as before | be constantDefaults; sm importAppliesCreationDefaults |
| 847 | 12 | the fusion kernel and the fit calculation are unchanged (inputs, outputs, diagnostics, algorithm string), and the golden fixtures and stored results stay valid | fs registrationShape; fg successFixturesMatchGolden, rejectionFixturesMatchGolden; fk fitTraceMatchesGolden; fst restoredAfterRestartIsBitIdentical |
| 848 | 12 | GTSAM stays confined to the fusion kernel, the new fusion files include neither GTSAM nor Eigen, and the rules on the fusion tooling hold | a: solver-confinement; a: fusion-tooling |
| 849 | 12 | the fusion registration gains the derived kinematics, the attitude derivation, the orientation type and the orientation attribute with its constant default; each derivation is an ordinary on-demand calculation with declared inputs, the attitude's including the orientation attribute | fs registrationShape; fd derivedRegistrationShape, attitudeRegistrationShape |
| 850 | 12, as settled | the fusion library reaches nothing of the core library: the attribute registry lives in the model library, the constant-default and track helpers are header-only, and the calculations never name the fusion library | a: solver-confinement; fd orientationDefinitionIsTheEnumeration |
| 851 | 12 | the attribute calculations hold the constant-default helper and the wind defaults; the importer holds neither | be constantDefaults; a: constant-defaults |
| 852 | 12 | the plot registry loses the removed plots and the local-frame category; the profile rule is the plot model's, one function the profile bridge calls | a: naming; cd profileNamingRemovedPlotsAppliesWithoutThem, profileStyleApplyCreatesDemand |
| 853 | 12 | the demand layer is untouched: a derived plot's blockers lead to the fit through the chain it already follows, and heading, pitch and roll are filled by one job | fr accHRowIsBlockedByFusion, headingPitchRollShareOneJob, allEightFusionPlotsAreExplicitBacked; a: demand |
| 854 | 13 | test: eight fusion plots, no local-frame plots, each explicit-backed as the seventeen were, an existing column over a removed plot's measurement still computing and export still writing it | fr allEightFusionPlotsAreExplicitBacked, removedPlotMeasurementsStayAvailable, noImuSessionIsNeverCounted; fs missingInputsAreNotApplicable; fj noImuSessionCannotHaveAJob |
| 855 | 13 | test: a profile naming a removed plot | cd profileNamingRemovedPlotsAppliesWithoutThem |
| 856 | 13 | test: derived kinematics on synthetic data with known answers | fd syntheticOutputsAreServedWithoutAFit, elevationIsOriginHeightMinusDownAboveGround, trackAccelerationsKnownAnswers, trackAccelerationsAreTheGnssDefinitions; fr allEightFusionPlotsAreExplicitBacked |
| 857 | 13 | test: attitude on synthetic quaternions with known answers | fd levelNorthFacingBodyReadsZero, knownAnglesComeBack, headingUnwrapsThroughAFullTurn, sideMountPermutesTheAngles, orientationRotationIsProper |
| 858 | 13 | test: the orientation attribute | fd orientationColumnShowsTheDefaultWithoutAWrite, storedOrientationRecomputesWithoutAFit, orientationDefaultRemovesTheStoredValue, orientationEditStoresATokenAndRefusesOthers, orientationBulkEdit, orientationDefinitionIsTheEnumeration; ca cellEditorOffersTheList |
| 859 | 13 | test: the choice type | ca choiceShowsTheLabelOfTheEffectiveToken, choiceSortsByLabel, cellEditorOffersTheList, setDialogOffersTheList, bulkEditSetsAToken |
| 860 | 13 | test: constant defaults | be constantDefaults; ce constantCalculationIsADefault; sm importAppliesCreationDefaults; im creationDefaultsOnlyFillAbsent; imm newSessionGetsDefaults |
| 861 | 13 | test: the fit is unchanged | fg successFixturesMatchGolden, rejectionFixturesMatchGolden; fk fitTraceMatchesGolden; fst restoredAfterRestartIsBitIdentical, codeStampChangeDropsRecordOnLoad; fs registrationShape |
| 862 | 13 | test: the audit keeps the removed plot names out of the registry, and the documents describe the eight plots, the attitude convention, the orientation attribute and the rule for defaults | a: naming |
| 863 | 14 | `docs/` describes the eight fusion plots and the derived quantities, the attitude convention with the orientation attribute, its default and its limits (it describes the mount, not the wearer's posture; a forward axis pointing straight up or down makes heading and roll meaningless), the choice type and the orientation in the session file, the rule for defaults and its helper, and that wind is no longer written at import; no document describes a removed plot or the local-frame category | a: naming |

In the map, a clause that mentions the category writes "the local-frame
category" (items 803, 842, 852, 854, 863). The appendix and 9.9 may name
"GNSS (Local frame)".

**Amended items.** Restate each with the change named. Keep its lines and add
those given.

| Item | Where | The restatement says |
|---|---|---|
| 6 | appendix A clause 6; its 9.1 row "saving does not rescale or relabel"; the map comment | (as amended): loading such a file adds only jumper mass and planform area where they are absent (the legacy backfill), additively and once. Wind is not written: a recording without it reads zero from a constant default. Add the line `6 tst_logbook_index rawLoadSkipsBackfill`; the 9.1 row's evidence gains it |
| 115 | appendix B clause 15; its second 9.2 row; the map comment | (as amended): the real fusion plots are the eight of the "Sensor fusion" category, and heading, pitch and roll share one job. The 9.2 row reads "row script with the real fusion plots (the eight); heading / pitch / roll share one job; `accH` blocked by fusion". Its evidence is as phase 5 left it |
| 509 | appendix F clause 9; its 9.6 row (Section "5, as amended"); the map comment | (as amended): plot demand as before, and the eight plots of the "Sensor fusion" category are such plots. One is an output of the fit; the seven derived from its outputs are blocked by it. The lines are as phase 5 left them |

Checked and **not** amended, because each names the fit's outputs or
channels, which remain:

- 230 and 235 (the runner's and the kernel's seventeen channels);
- 337 (seventeen channels bit-identical);
- 344 (a column over `Fusion/roll`, the kept-column case);
- 404 (an edit of wind the fit does not reach);
- 111, whose clause names no plot; only its evidence cell changes, as above.

If the search in appendices A-H and sections 9.1-9.8 for "seventeen",
"local", "roll", "pitch", "yaw", "quaternion", "wind" and "backfill" finds an
item whose statement is no longer true, restate it the same way and report
it.

## Decisions

- **D1. Items from §5 onward, plus §14.** The specification numbers nothing,
  so appendix H's rule applies:
  - one clause per testable statement of §5-§12;
  - one per §13 bullet, and one for §14.

  §1-§4 have no item. §3's two observable out-of-scope statements (the fit's
  channels remain measurements; mass, area and the fixed ground elevation stay
  creation defaults) are filed as clauses 4 and 40. §12's bullet on the
  attribute registry and the logbook restates §9 and has no item of its own:
  clauses 27-33 carry it.
- **D2. Settled clauses** state what the specification left open and the
  overview decided:
  - 802: the colour rule (decision 12);
  - 807: the ground reference (decision 2);
  - 808: `Fusion/accD` itself (decision 1);
  - 810: one definition (decision 3);
  - 814: no course reference (decision 5);
  - 822: the key, category and once per process (decisions 6, 8);
  - 826: an invalid stored token (decision 7);
  - 836: the SP and WS-P defaults on the helper (decision 11, phase 2 D2);
  - 839: the backfill (decision 10);
  - 850: the library boundaries (decision 18).
- **D3. Item 509 is restated, although its words name no plot.** Its
  real-plot evidence was the list of seventeen. The clause is now true of a
  set in which seven plots are derived and blocked by the fit. Naming that
  set keeps the item honest about what its evidence shows. Item 111 names no
  plot, so only its evidence cell changes.
- **D4. Item 6 is restated for the backfill.** Its evidence
  (`releasedLogbookBackfillIsAdditive`) now pins a smaller backfill, and the
  specification of 1-19 described wind among the import-time values. The
  amendment is stated where the item is.
- **D5. Manual steps M39-M44 cover what no test reaches:**
  - the real plot list and its colours;
  - real fits through turns;
  - the editors on a real logbook, with loaded and unloaded rows;
  - a hand-edited file;
  - an old profile file;
  - a column kept from an earlier build.

  M3 is restated in place, because Yaw is no longer a plot. The steps that
  check Roll stay true, because the Roll plot exists.
- **D6. No new audit rule.** The traceability range grows, and the section-10
  text follows the rules as they are. The rule count is therefore unchanged
  and checkable.
- **D7. The documents are checked here, and written by their phases.** A
  change of behaviour is documented in the phase that makes it (CLAUDE.md),
  so the statements this phase first found missing were moved into phases 3
  and 4; this phase closes only the cross-document items 7 and 8.

Ready, with caveats:

- The map lines use the phase documents' function names, and phase 1's were
  only proposals, so each must be checked against the tree.
- Manual step M44 needs a build from before phase 5 to create the kept
  column.
