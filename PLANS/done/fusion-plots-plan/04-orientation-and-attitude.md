# Phase 4: Orientation and attitude

## Purpose

Spec §7 and §8, with overview decisions 4-8, 11, 17 and 18. The fusion
library gains:

- the orientation type, the one owner of the mount vocabulary;
- the orientation attribute `_ORIENTATION`, a Choice attribute with a constant
  default of `+y,+z`;
- the attitude derivation, which gives heading, pitch and roll of the body
  frame from the fit's quaternion.

The attribute registry moves into `flysight_model` so that the fusion
registration can register the attribute's definition.

It is one phase because the type, the attribute and the derivation are one
vocabulary with three users. The attribute's choices, its default and the
derivation's parser must all come from the same enumeration. Nothing new
appears in the plot list: phase 5 adds the Heading, Pitch and Roll rows. What
the user sees after this phase is the Orientation column, which can be added
from the Add Column dialog's "Session" group.

## Dependencies

- **Depends on phases 1, 2 and 3**, all implemented when this phase starts.
  At that point:
  - `AttributeFormatType::Choice`, `AttributeChoice` and
    `AttributeDefinition::choices` exist, and the model, both editors and the
    bulk edit honour them;
  - `FlySightTest::ChoiceFixture` (`tests/support/choicefixture.h`) is part
    of `flysight_test_support`;
  - `Calculations::addConstantDefault` is inline in
    `src/calculations/attributecalculations.h`, and the audit group
    `constant-defaults` exists;
  - `registerFusionCalculations()` registers, in order, `builtin.fusion.fit`,
    `accH`, `systemTime`, `z`, `accAlongTrack` and `accCrossTrack`;
  - `tests/tst_fusion_derived.cpp` and `FlySightTest::syntheticFitSession()`
    exist, and `docs/SENSOR_FUSION.md` §4, §8 and `docs/CALCULATIONS.md` §17
    describe the derived kinematics.
- **Blocks** phase 5, whose rows name `Fusion/bodyHeading`, `Fusion/bodyPitch`
  and `Fusion/bodyRoll`, and phase 6, which cites the test functions listed
  under Tests.

## What changes

### The orientation type (`src/fusion/orientation.h`, `.cpp`, new)

This is `FlySight::Fusion::Orientation`: a forward axis and an up axis, each
one of +x, −x, +y, −y, +z, −z, neither equal nor opposite. It is the one place
that spells the vocabulary. Its contract is stated in its class comment, and
it provides:

- **Parsing** a token into an orientation. The result is absent for any text
  that is not exactly one of the 24 tokens: `+y,+y`, `+y,-y`, `y,z`,
  `+y, +z`, `+Y,+Z`, an empty string, and so on.
- **The token**, `<forward>,<up>`, and **the label**, `forward <forward>, up
  <up>`. Both use the ASCII hyphen-minus (decision 6), for example `+y,+z` and
  `forward +y, up +z`.
- **The body-to-device rotation**: a constant signed permutation whose
  columns are the forward, right and down axes in device coordinates, with
  right = forward × up and down = −up. It is exact (entries −1, 0, 1), a
  proper rotation for every pair. For the default it is the matrix with
  columns (0,1,0), (1,0,0), (0,0,−1).
- **The default**, forward +y and up +z.
- **The one enumeration** of the 24 pairs, default first, then the other 23
  in enumeration order (Decisions, D1).

Constraints:

- The files include Qt Core and the standard library only: no `<gtsam/`, no
  `<Eigen/`, and none of `sessiondata`, `engine/` or `preferences/` (the audit
  rules "public and registration files are GTSAM-free" and "the kernel is
  pure").
- They do not log.
- Labels are not translated. They name axes, and the application's attribute
  display names are not translated either.
- List both files in `flysight_fusion` in `src/CMakeLists.txt`, with a
  one-line comment.

### The session key (`src/sessiondata.h`)

Add `SessionKeys::Orientation` = `"_ORIENTATION"`, with a comment: which device
axes point forward and up, as a token of `Fusion::Orientation`. Do not spell a
token in that comment (see the audit rule below).

### The attribute registry moves to `flysight_model` (`src/CMakeLists.txt`)

Move `attributeregistry.cpp attributeregistry.h` from the `flysight_core`
list into the `flysight_model` list (decision 18). Add a comment in the style
of the `csvformat` entry: it is here rather than in `flysight_core` so that
the fusion registration can register the orientation attribute, and it needs
Qt Core only. `flysight_core` links `flysight_model` PUBLIC, so every consumer
still resolves it.

Check that each of these builds and passes: `FlySightViewer`,
`flysight_cpp_bridge`, `tst_python_bridge`, the widget tests, `fusion_runner`,
and every test through `flysight_test_support`. No test compiles
`attributeregistry.cpp` directly (`tests/CMakeLists.txt` names it nowhere).

Update the comments and documents that say which library holds what:

- the `flysight_fusion` block comment: the registration also registers the
  orientation attribute's definition, and `orientation.*` is pure;
- the `flysight_model` and `flysight_core` descriptions, if they list the
  attribute registry;
- the root `README.md` tree comment on `src/CMakeLists.txt`, where
  `flysight_model` is "session data + calculation engine": it gains the
  attribute registry.

`_FLYSIGHT_GTSAM_NEVER` still holds, because the move adds no link to either
library. No audit rule names the file's library today. If a search finds
one, update it in this phase.

### The fusion registration (`src/fusion/fusionregistration.cpp`, `.h`)

`registerFusionCalculations(registry)` gains two things, both after
`builtin.fusion.accCrossTrack`:

1. **The orientation attribute.**
   - Its definition goes into `AttributeRegistry::instance()` **once per
     process** (decision 8). The call is skipped when `findByKey` already
     finds `_ORIENTATION`, because tests and `fusion_runner` call the entry
     point for more than one registry.
   - Definition: category "Session", display name "Orientation", key
     `SessionKeys::Orientation`, `AttributeFormatType::Choice`, editable, no
     measurement type. Its `choices` are the enumeration of 24, mapped to
     token and label, in enumeration order.
   - Its default goes into the given registry, every call:
     `Calculations::addConstantDefault(registry, SessionKeys::Orientation,
     <the default's token as a QString>)`. The value is taken from the
     orientation type, never spelled.
2. **`builtin.fusion.attitude`**, following `registerHorizontalAcceleration`:
   - OnDemand, no title, no `resultVersion`;
   - inputs exactly `Fusion/qx`, `Fusion/qy`, `Fusion/qz`, `Fusion/qw`, the
     attribute `_ORIENTATION`, and for the heading reference `GNSS/velN`,
     `GNSS/velE`, `GNSS/_time` and the attribute `_COURSE_REF`, in that
     order (the last four exactly as the GNSS course declares them);
   - outputs exactly `Fusion/bodyHeading`, `Fusion/bodyPitch` and
     `Fusion/bodyRoll`, in that order, published together (decision 4).

   The compute function keeps no state and does not log. It reads
   `_ORIENTATION` as text and parses it with the orientation type. It returns
   unavailable when:
   - the token is not one of the 24 (decision 7);
   - the four arrays are empty or differ in length;
   - any sample's quaternion is not finite or has zero norm (D3).

   Otherwise, per sample:
   - normalize q = (qx, qy, qz, qw);
   - form R, the device-to-NED rotation matrix of the Hamilton quaternion.
     This is the convention of `fillOutputChannels()`
     (`dense.rotation[i].toQuaternion()`, "body to fixed NED; quaternion
     xyzw"): R·v_device = v_NED;
   - form M = R·C, where C is the orientation's body-to-device rotation;
   - heading = atan2(M₁₀, M₀₀), pitch = asin(clamp(−M₂₀, −1, 1)),
     roll = atan2(M₂₁, M₂₂), all in degrees.

   Heading is then unwrapped over the whole array with
   `Calculations::unwrapDegrees` (`calculations/anglehelper.h`, header-only,
   already used in `src/fusion`) and offset by the course reference angle,
   the same value the GNSS course subtracts (decision 5): the unwrapped raw
   GNSS course interpolated at `_COURSE_REF`, zero when the reference is not
   a number or lies outside the GNSS time range. That computation moves out
   of the GNSS course registration into one header-only helper in
   `src/calculations/` (the implementer names it, beside the track helper),
   which the GNSS course and this calculation both call; the GNSS course
   stays bit-identical (`tst_builtins_golden`). Pitch stays in [−90, 90] and
   roll in (−180, 180].

   The per-sample arithmetic may live beside the orientation type or in the
   registration file. It stays in `src/fusion`, never in `src/calculations`.
   Where the forward axis is vertical, the angles are whatever these formulas
   give (spec §7). With the orientation `+x,-z` (C = I) they are the fit's own
   roll, pitch and yaw, up to rounding and unwrapping. A test holds the
   derivation to that (D8).

Constraints on the registration:

- Name the compute lambda's parameter. Phase 2's `constant-defaults` rule
  allows an unnamed `const EvaluationContext &` only in
  `attributecalculations.h`.
- It includes `attributeregistry.h`, `calculations/attributecalculations.h`
  and `fusion/orientation.h`. It calls nothing of `flysight_core`, not even
  `registerAttributeCalculations`, which that header also declares.
- `registerFit`, `kFitOutputs`, `kFitInputs`, `fitInputs()`, the kernel
  files, `fusion.h` (including its `Result` comment) and `tests/data/fusion/`
  stay untouched (decision 17). The fit does not read `_ORIENTATION`, so an
  orientation edit never invalidates it, and stored fits stay valid.

**The header comment** ("Register sensor fusion with the calculation
engine:") gains two bullets in registration order, in the style of phase 3's:

- `builtin.default._ORIENTATION`, the constant default, with a note that the
  definition is registered once per process;
- `builtin.fusion.attitude`, the three angles and their convention.

The "one entry point" paragraph says that it also registers the orientation
attribute's definition. Comments in `src` name the default in words (forward
+y, up +z) or by its label, never as a token: the audit group `orientation`
below allows a token only in `orientation.*`.

### The unwrap helper's comment (`src/calculations/anglehelper.h`)

"the GNSS course and the sensor fusion's roll, pitch and yaw both use it"
also names the heading of the body frame derived from the fused attitude.
Spell neither `fusion/` nor `Fusion::` in the comment (audit, "nobody but the
application references the fusion library").

### What must not change

- The application's registration order in `src/mainwindow.cpp`. The
  definition is registered before the logbook is brought up, as the SP and
  WS-P definitions are.
- `LogbookColumnStore::loadDefaults()`: the orientation is not a default
  column.
- `src/logbookcolumn.cpp`, `src/preferences/addcolumndialog.cpp`,
  `src/sessionmodel.*` and the delegate and view. Phase 1 made them generic.
- `CalculationCompatibilityVersion`. The only change is added registrations,
  and `docs/CALCULATIONS.md` §9 says not to bump it for those.

## Interfaces

**Provided** (as the overview fixes them):

- `SessionKeys::Orientation` = `"_ORIENTATION"` (`src/sessiondata.h`).
- `src/attributeregistry.{h,cpp}` in `flysight_model`.
- `FlySight::Fusion::Orientation` in `src/fusion/orientation.{h,cpp}`: tokens,
  labels, the body-to-device rotation, the default and the enumeration of 24.
  The member names are the implementer's.
- `builtin.fusion.attitude` → `Fusion/bodyHeading`, `Fusion/bodyPitch`,
  `Fusion/bodyRoll`, with inputs `Fusion/qx`, `qy`, `qz`, `qw`,
  `_ORIENTATION`, `GNSS/velN`, `GNSS/velE`, `GNSS/_time` and `_COURSE_REF`.
  Phase 5's Heading, Pitch and Roll rows name these three.
- The course-reference helper (header-only, `src/calculations/`), called by
  the GNSS course registration and by the attitude calculation.
- `builtin.default._ORIENTATION`, value `"+y,+z"` (a `QString`).
- The eight-id tail of `registeredIds()`: `builtin.fusion.fit`, `accH`,
  `systemTime`, `z`, `accAlongTrack`, `accCrossTrack`,
  `builtin.default._ORIENTATION`, `builtin.fusion.attitude`.
- The test functions under Tests, and the audit group `orientation`.

**Consumed:** phase 1's type, model contract and `ChoiceFixture`; phase 2's
`addConstantDefault`; phase 3's registration tail, `syntheticFitSession()`,
`tst_fusion_derived` and its fixture pattern (`registerBuiltIns()` then
`registerFusionOnce()`, and the `registeredIds()` / `enrolledEngineCount()`
check in `cleanup()`).

## Acceptance criteria

1. The enumeration has exactly 24 distinct pairs with distinct tokens and
   labels, and the default `+y,+z` comes first. Every token parses back to its
   pair, and the malformed texts listed above do not parse. (§8 "the 24 valid
   pairs, and only they"; decision 6)
2. For every pair the rotation has integer entries, is orthonormal, has
   determinant +1, and its columns are forward, forward × up and −up. (§7 "a
   proper rotation, never a reflection")
3. `orientation.*` and `fusionregistration.*` include no GTSAM or Eigen
   header. The audit list names the two new files. (decision 17)
4. `AttributeRegistry` holds exactly one `_ORIENTATION` definition with the
   fields above. Its `choices` equal the enumeration, token for token and
   label for label, in order. This holds after a second call of
   `registerFusionCalculations` on another registry, which does register
   `builtin.default._ORIENTATION` there. (§8; decisions 6, 8)
5. `builtin.default._ORIENTATION` has no inputs and one output, the attribute,
   with the value `"+y,+z"`. Outside `src/fusion/orientation.*`, no file in
   `src` spells a token. (§8 "nothing else spells a token", §10; decision 11)
6. `builtin.fusion.attitude` has the policy, inputs and outputs above. Each of
   its outputs has exactly one candidate. `explicitDependencies()` of each of
   the three names is exactly `{builtin.fusion.fit}`, and that of
   `_ORIENTATION` is empty. The `registeredIds()` tail is the eight ids above.
   (§7, §12; decision 4)
7. On a fixture session without a fit, each of the three names is `Blocked`
   by the fit and merely uncomputed, and reads empty. The fit's `runCount`
   stays 0. (§6 and §7 "appears with the fit and never starts one"; §12)
8. Every angle below is within 1e-9 degrees of its expected value, and an
   expected ±180 is compared modulo 360. (§7, §13)
   - For the default orientation, a level north-facing body reads 0, 0, 0.
   - A body built by hand from a known heading, pitch and roll reads them
     back.
   - A full turn of heading reads continuous, equal to `unwrapDegrees` of the
     wrapped headings minus the course reference angle; a stored
     `_COURSE_REF` inside the GNSS time range offsets heading by exactly the
     angle it offsets `GNSS/course`, and one outside it, or none, by zero
     (decision 5).
   - Heading is unavailable exactly when `GNSS/course` is (the same inputs).
   - Roll through a full turn stays in (−180, 180], and pitch in [−90, 90].
   - The side mounts give the angles the permutation predicts (Tests).
9. With the orientation `+x,-z`, the angles derived from a success golden's
   `qx..qw` equal that golden's `yaw`, `pitch` and `roll` modulo 360, within
   1e-6 degrees. (§7; the convention of `fillOutputChannels()`)
10. A stored token outside the 24 makes all three angles unavailable. This
    covers an empty string and a pair with forward equal or opposite to up.
    Removing the stored value restores them. (decision 7)
11. A stored valid token wins over the default, and the angles recompute
    through ordinary invalidation. The fit's `runCount` stays 0 on the
    synthetic session, and the fit's own channels are unchanged. Removing the
    token returns to the default's angles. (§7 last line; §8; §13)
12. A session with nothing stored reads `"+y,+z"` through `getAttribute`, and
    `hasAttribute` is false. In a `SessionModel`, on loaded and stub rows, the
    Orientation cell shows `forward +y, up +z`, the session file has no
    `$VAR,_ORIENTATION` line, and `index.json` caches `+y,+z`. (§8 "every
    recording has it ... with nothing written"; §11; §13)
13. On the Orientation column, with loaded and stub rows:
    - `setData()` stores a token and the file gains `$VAR,_ORIENTATION,<token>`;
    - `setData()` refuses a token outside the list, a label and an empty
      string;
    - `setData(QVariant())` removes the stored value;
    - the bulk edit sets a token, removes with `QVariant()`, and refuses a
      token outside the list before queuing anything.

    (§8 "How it is edited"; §13; decision 9)
14. `attributeregistry.cpp` is in `flysight_model` and not in `flysight_core`,
    and `flysight_fusion` does not link `flysight_core`. The configure-time
    confinement check passes, and so does every target listed above.
    (decision 18)
15. The fit is unchanged. No decision-17 file is modified. `tst_fusion_golden`,
    `tst_fusion_kernel`, `tst_fusion_session` (only `registrationShape`'s tail
    is amended), `tst_fusion_jobs`, `tst_fusion_rows`, `tst_fusion_store` and
    `tst_fusion_runner` pass, with their `_exact` runs. (§12, §13 "the fit is
    unchanged"; decision 17)
16. `audit_cleanup` is green with no rule loosened. The whole suite passes
    sequentially in `build-agent/`. The documents below describe the code.

## Tests

### `tests/tst_fusion_derived.cpp` (phase 3's executable)

It already links `flysight_fusion_session_support`, and through
`flysight_add_test` it links `flysight_test_support`, so `ChoiceFixture` and
`loadFusionGolden()` are both reachable. No new executable is needed, and
`tests/CMakeLists.txt` and `cmake/SolverDependencies.cmake` do not change.

Engine-level tests use `syntheticFitSession()` with `qx..qw` (and `_time`
where useful), plus `setAttribute` for `_ORIENTATION` and `_COURSE_REF`. The
expected values are built by hand in the test (Euler angles to a body-to-NED
quaternion, composed with a hand-written default-mount constant). They never
come from the orientation type's own rotation. Tokens are spelled literally
in the test.

These names are the ones phase 6 cites. Keep them.

- `orientationVocabularyHasTwentyFourPairs`: criterion 1.
- `orientationRotationIsProper`: criterion 2, for all 24 pairs, plus the
  default's matrix exactly.
- `orientationDefinitionIsTheEnumeration`: criterion 4. It also calls
  `Fusion::registerFusionCalculations()` on a private `CalculationRegistry`.
- `attitudeRegistrationShape`: criteria 5 (registry half) and 6.
- `attitudeWaitsOnTheFit`: criterion 7, on `fixtureSession("coarse_linear")`.
  `SessionData` only.
- `levelNorthFacingBodyReadsZero`: criterion 8, first case. Use the device
  quaternion (√½, √½, 0, 0) and its negation (y north, x east, z up).
- `knownAnglesComeBack` (data-driven): criterion 8. Rows cover a heading near
  ±179, a pitch of ±80 and a roll of ±170, one at a time and combined.
- `headingUnwrapsThroughAFullTurn`: criterion 8. Turn a level body through
  720° in 10° steps, over GNSS velocity samples with a known raw course. A
  stored `_COURSE_REF` inside the range offsets heading by the same angle it
  offsets `GNSS/course` (compare the two); outside the range, or absent, by
  zero; without GNSS velocity, heading is unavailable and pitch and roll are
  too (one calculation).
- `rollAndPitchStayInTheirNaturalRanges`: criterion 8, with a barrel roll and
  a loop.
- `sideMountPermutesTheAngles` (data-driven): criterion 8. Take the level
  device attitude of `levelNorthFacingBodyReadsZero`, store each token, and
  expect (heading, pitch, roll):

  | Stored token | Heading | Pitch | Roll |
  | --- | --- | --- | --- |
  | `+x,+z` | 90 | 0 | 0 |
  | `-x,+z` | −90 | 0 | 0 |
  | `+y,+x` | 0 | 0 | 90 |
  | `+y,-x` | 0 | 0 | −90 |
  | `+x,+y` | 90 | 0 | −90 |

  Add one pitched row: a device at heading 0, pitch 30 and roll 0 under the
  default reads (0, 30, 90) under `+y,+x`.
- `deviceFrameMountGivesTheFitsOwnAngles`: criterion 9.
  - Use a success golden whose pitch stays well below 90° in magnitude
    (check this in the test).
  - Store its `_time` and `qx..qw` in a synthetic session, and store `+x,-z`.
  - Compare modulo 360, because the fit unwraps all three angles and the
    derivation unwraps only heading.
- `invalidStoredOrientationMakesAttitudeUnavailable`: criterion 10.
- `storedOrientationRecomputesWithoutAFit`: criterion 11, and the engine half
  of criterion 12. It starts with nothing stored.
- Model tests through `ChoiceFixture`, with key `SessionKeys::Orientation`, on
  sessions that may go into a `SessionModel` (`fixtureSession()` sessions or
  small `FixtureBuilder` ones, never `syntheticFitSession()`), with
  `useFreshLogbook()`:
  - `orientationColumnShowsTheDefaultWithoutAWrite` (rows: loaded, stubs):
    criterion 12.
  - `orientationEditStoresATokenAndRefusesOthers`: criterion 13, `setData()`
    half. It covers a comma token in the file and a restart as stubs showing
    the stored label.
  - `orientationDefaultRemovesTheStoredValue`: criterion 13, removal.
  - `orientationBulkEdit` (rows: loaded, stubs): criterion 13, bulk half.

The editors need no fusion test. Phase 1's `cellEditorOffersTheList` and
`setDialogOffersTheList` prove that both present any Choice definition's
list, "Default" first, with no free text. Phase 6 cites them for "refused by
the editor". The model tests run only in fusion builds, because the attribute
exists only where the fusion library is registered.

### Amended

- `tst_fusion_session::registrationShape`: the tail becomes the eight ids.
  Keep the name, which item 221 cites.
- `tst_fusion_derived::derivedRegistrationShape`: phase 3 pinned a six-id
  tail. Keep its descriptor checks, and assert only that its three ids follow
  `builtin.fusion.systemTime` in order. The tail is pinned in
  `registrationShape` alone.

### Audit (`tests/audit/cleanup_audit.cmake`)

- In `solver-confinement`, "public and registration files are GTSAM-free"
  gains `src/fusion/orientation.h` and `src/fusion/orientation.cpp`. Do not
  add `orientation.h` to the fusion-tooling internal-header list: tests
  include it (decision 17).
- Add a new group, `audit_group(orientation)`, in its own block after
  `constant-defaults`, with an "Allow:" comment:
  `expect_only("one authority: orientation tokens" "[+-][xyz],[+-][xyz]"
  "^src/fusion/orientation\\.(cpp|h)$" src)`. Today it has no hit in `src`.
  Prove it once by planting a hit.
- Add a clause for the single authority of orientation tokens to the file's
  header list.
- Check the text against these rules:
  - "the kernel is pure", "the kernel does not log" and the GTSAM-header rule
    for the new files;
  - "nobody but the application references the fusion library" for
    `anglehelper.h`;
  - `constant-defaults` for the attitude lambda;
  - `fusion-model` for `SENSOR_FUSION.md` (no "frozen", "stationary window",
    "twenty-one", ...).

### `tests/README.md` and the acceptance map

- **Section 1:**
  - the `tst_fusion_derived` row gains the orientation vocabulary, the
    attitude derivation and the orientation column's model and bulk-edit
    cases through `ChoiceFixture`;
  - in the `tst_fusion_session` row, the registration count becomes eight;
  - the executable counts are unchanged.
- **Section 11, "Fusion sessions":** one sentence on the attitude tests, which
  store `qx..qw` through `syntheticFitSession()`, and on the orientation
  column tests, which use `ChoiceFixture` on sessions that may enter a model.
- **`tests/acceptance_map.txt`:** no new line (decision 15). No cited
  function is renamed.

### Documentation

- **`docs/SENSOR_FUSION.md` §2.** Add a paragraph after the plot-list bullet,
  which phase 5 rewrites. Keep this text to the attitude and the attribute.
  It covers:
  - heading, pitch and roll of the body frame: the aircraft convention, and
    heading clockwise from north, unwrapped by the course's rule and
    referenced to the course reference exactly as course is, so that heading
    and course overlay in straight flight and their difference is sideslip;
  - the Orientation attribute: which device axes point forward and up, the
    default forward +y, up +z (a FlySight 2 on the back of a helmet, label
    up), and how to set it (the logbook column and the context menu, where
    "Default" removes the setting);
  - that the attribute describes the mount and not the wearer's posture;
  - that a mount or posture that points the forward axis straight up or down
    makes heading and roll meaningless;
  - that a change recomputes the angles without a new fit.
- **`docs/SENSOR_FUSION.md` §4.**
  - Phase 3's derived table gains `bodyHeading`, `bodyPitch` and `bodyRoll`.
  - The diagnostics' `orientation` key ("the output convention, as text")
    states the fit's own device-frame convention and does not follow the
    Orientation attribute: say so, so the two are not confused.
  - One sentence says that the fit's own `roll`, `pitch` and `yaw` are the
    same angles for the device frame (forward +x, up −z), all three
    unwrapped.
- **`docs/SENSOR_FUSION.md` §5.** Next to the sentence that the initializer
  never assumes "a known mounting heading", say that the Orientation
  attribute is not an input of the fit: it only turns the fit's quaternion
  into heading, pitch and roll (spec §2, "the fit stays the fit").
- **`docs/SENSOR_FUSION.md` §8.** The `tst_fusion_derived` row gains the
  attitude and the orientation.
- **`docs/COMPUTED_PLOTS.md` §6.** A bullet: changing a recording's
  Orientation never discards a kept result; heading, pitch and roll, and any
  column over them, are recomputed from it at once, and nothing is counted in
  the status bar (spec §11). Name the angles, not plot rows (phase 5 adds the
  rows).
- **`docs/CALCULATIONS.md` §17.**
  - "Six calculations" becomes eight, and the table gains
    `builtin.default._ORIENTATION` and `builtin.fusion.attitude`.
  - The opening paragraph says that the entry point also registers the
    orientation attribute's definition in the attribute registry
    (`flysight_model`), once per process.
  - "Derived values" gains the attitude, with the formulas, the unwrap, the
    unavailable cases and the absence of `_COURSE_REF`, and the orientation
    default through `addConstantDefault`.
  - The sentence "Both are on demand ..." now covers the angles.
  - "Logbook columns": a column over `Fusion/bodyRoll` (and the other two
    angles) reads `_ORIENTATION`, so an orientation edit discards its cached
    value, and it is recomputed from the stored fit (loaded rows by the
    engine, stubs by the column worker) without a fit. "A Fusion/roll column
    keeps its cached value ..." stays: that is a column over the fit's own
    channel.
  - The test paragraph names the attitude tests.
  - Phase 2's §5 rule stays as it is.
- **`docs/DATA_SCHEMA.md` §9.**
  - A Choice attribute is saved as its token, verbatim text (for example
    `$VAR,_ORIENTATION,+x,+z`), and only when the user set one. "Default"
    removes the line. An unset recording has no line and reads the constant
    default (`CALCULATIONS.md` §5).
  - A hand-edited token outside the list is kept and shown as written, and the
    attitude is unavailable for it.
- **`docs/DATA_SCHEMA.md` §10.** Existing logbooks gain nothing: every
  recording reads the default orientation, and nothing is written.
- **`docs/DATA_SCHEMA.md` §11.** If it says what an attribute column caches
  by type, a choice column caches its token (the effective one: `+y,+z` for
  an unset recording).

## Decisions

- **D1. Enumeration order.** Forward runs through +x, −x, +y, −y, +z, −z. For
  each forward, up runs through the same order, skipping forward's axis. The
  default is moved to the front. Decision 6 asks for one order, and this is
  the obvious deterministic one.
- **D2. Labels use the token's axis text** (ASCII, untranslated), so the label
  follows from the pair and spells no second vocabulary.
- **D3. Robustness of the derivation.**
  - Each quaternion is normalized, and `asin` is clamped, so rounding near the
    vertical never yields NaN.
  - A non-finite or zero-norm sample makes the result unavailable rather
    than NaN. `unwrapDegrees` would otherwise carry one NaN into every later
    heading. A succeeded fit never produces such a sample.
  - Empty or unequal-length inputs are unavailable, as phase 3's are.
- **D4. The attribute's default comes before the attitude** in registration
  order, so the vocabulary precedes its reader. Each name has one candidate,
  so the order only fixes the `registeredIds()` tail.
- **D5. Once per process by lookup.** `findByKey` before `registerAttribute`
  needs no new registry API. The registry has no removal, and a second
  definition would give the Add Column dialog two entries.
- **D6. All phase 4 tests live in `tst_fusion_derived`.** It is the only
  executable that links `flysight_fusion` and already has the synthetic seam,
  and phase 1 made `ChoiceFixture` widget-free and fusion-free for this use.
- **D7. An audit rule for tokens**, in a group of its own so that phase 6 can
  cite it for spec §8's "nothing else spells a token". It searches `src`
  only, because tests and documents spell tokens legitimately.
- **D8. The convention is held to the fit** (`deviceFrameMountGivesTheFitsOwnAngles`)
  by a golden's own quaternion and angles, without GTSAM in the test. Known
  answers alone cannot tell a transposed rotation from the right one when
  both are built by the same hand.

Ready.
