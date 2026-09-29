# Phase 1: The choice attribute type

## Purpose

This phase implements spec §9 and the generic half of §8's "How it is
edited". The attribute registry gets a fifth format type, **Choice**. A
Choice definition carries its list of allowed values, each one a stored token
and a display label. Everything in the logbook follows from the type alone:

- the model displays and sorts by label;
- the model stores only a token from the list;
- "Default" removes the stored attribute (overview decision 9);
- in-place editing gives a list editor instead of the line edit;
- the context menu's "Set ..." action shows a list instead of the text prompt;
- the bulk edit carries a token, or a removal, and refuses a token outside
  the list.

The phase is one unit because the model's contract and the two editors that
present it are only testable together, and none of it is specific to
orientation. No product attribute uses the type yet. The tests register a
choice attribute of their own; phase 4 registers the first product one.

## Dependencies

- **Depends on:** nothing. The phase starts from the committed head of
  `store-requested-calculations`.
- **Blocks:** phase 4, which registers `_ORIENTATION` as a Choice attribute and
  extends this phase's fixture to it (see Interfaces).
- **Implemented before phases 2 and 3.** Stay out of their areas: the
  attribute calculations and the importer, the fusion registration, and every
  document under `docs/`.
- **May assume:**
  - `AttributeDefinition` is an aggregate, brace-initialized with five or six
    initializers in `src/calculations/attributeregistration.cpp`,
    `spcalculations.cpp` and `wspcalculations.cpp`;
  - `SessionData::setAttribute()` and `removeAttribute()` both return the
    changed names, and `hasAttribute()` is true for stored attributes only;
  - `getAttribute()` returns the effective value: stored first, then
    calculated. A stored value wins even when it is invalid (spec §10's
    note).

## What changes

### The registry (`src/attributeregistry.h`, `.cpp`)

- Add `AttributeFormatType::Choice`.
- Add `struct AttributeChoice { QString token; QString label; };`, declared
  before `AttributeDefinition`.
- Add `QVector<AttributeChoice> choices;` as the **last** member of
  `AttributeDefinition`, after `measurementType`. Every existing brace
  initializer must compile unchanged, so do not edit
  `attributeregistration.cpp`, `spcalculations.cpp` or `wspcalculations.cpp`.
- The struct's comment states the contract of a Choice definition:
  - the list is non-empty, in presentation order;
  - tokens are non-empty and unique;
  - labels are unique and none is the editors' "Default";
  - a definition of any other type has an empty list.

  The registry does not check any of this.
- Add one lookup from a token to its choice, which is absent for a token
  outside the list. It may be a member function of `AttributeDefinition`,
  which stays an aggregate, or a free function beside the struct. The model's
  display, sort, `setData()` and `startBulkEdit()` all use it (one authority).
  No second loop over `choices` appears in the model.

### The model (`src/sessionmodel.cpp`, `src/sessionmodel.h`)

Each place below has a per-type switch today. Add the Choice case there. The
other four types behave exactly as before.

- **Display** (`formatAttributeValue()` for loaded rows, `formatRawValue()`
  for stubs): the label of the effective token, or the raw text when the token
  has no label (overview decision 7). An invalid value stays invalid (empty
  cell). `data()` for `Qt::EditRole` is unchanged and returns the same text.
- **Sort** (`sort()`): a Choice column compares the text it displays (label,
  else raw text), case-insensitively, as a Text column compares its value.
  Missing values go to the bottom as now. Loaded rows read the effective
  token and stub rows read the cached token; both map through the one lookup.
- **`setData(index, value, Qt::EditRole)`** on a Choice column:
  - A `QString` token from the list is stored unless the **stored** value is
    already that token. Compare with the stored value, not the effective one:
    see Decisions.
  - An invalid `QVariant` removes the stored attribute through
    `removeAttribute()`. It returns false, and changes nothing, when nothing
    is stored.
  - Any other value returns false: a token outside the list, a label, an
    empty string. Validate **before** the force-load (`sessionRef()`), so a
    refused edit leaves a stub a stub.
  - A change follows the Text path: `invalidateColumns()` for the attribute,
    `dataChanged`, `modelChanged`, `publishInvalidation()` with the names that
    `setAttribute()` / `removeAttribute()` returned, and `scheduleSave()`.
- **`startBulkEdit(rows, columnIndex, value)`** on a Choice column: a token
  from the list, or an invalid `QVariant`. Any other value returns before
  anything is queued, invalidated or woken: no `BulkEditItem`, no
  `invalidateColumns()`, no `m_scheduler.wake()`. For other types an invalid
  `QVariant` keeps today's meaning.
- **`processNextBulkEdit()`**:
  - `editableType` admits Choice;
  - the value switch gets a Choice case;
  - an invalid value means removal on both paths: `removeAttribute()` where
    the loaded path calls `session.setAttribute(...)`, and where the stub path
    calls `loaded->setAttribute(...)`.

  Everything else about the two paths stays as it is: the inline save, the
  stub's temporary load and promotion on a failed save, the published names,
  and the progress counts. `BulkEditItem` is unchanged, since its `QVariant`
  already carries an invalid value. Update the comments that mention the
  value ("Compute the final value (with unit reverse-conversion for
  Double)").
- `flags()` is unchanged: the definition's `editable` decides.
  `updateAttribute()` is unchanged (see Decisions).

### The in-place editor (`src/ui/docks/logbook/LogbookCellDelegate.h`, `.cpp`)

The delegate gets an editor for a Choice cell, and only for one. Every other
cell keeps the base class's editor and sizes.

- **The editor.** A non-editable `QComboBox`, so free text cannot be typed.
  Its entries are "Default" (`tr("Default")`, value an invalid `QVariant`)
  first, then each choice's label in definition order, with its token as the
  value.
- **The opening entry.** The editor opens on the entry whose label is the
  cell's display text. When the text is no label (a raw token, or no value),
  it opens on no entry.
- **The commit.** The delegate writes through
  `SessionModel::setData(index, value, Qt::EditRole)` with the chosen entry's
  value. It writes nothing when the editor closes on the entry it opened on.
  Committing on the base class's Enter and focus-out is enough. Committing
  and closing when an entry is activated is also acceptable.
- **One list for both editors.** The list of entries is built by one function
  that the view's dialog also uses, for example a static of the delegate
  (the view already includes its header).
- **What must not change.** The delegate implements no event handler. The
  audit group `demand` forbids `editorEvent`, `mouse*Event` and
  `keyPressEvent` in the delegate's files, even in a comment. Painting,
  `helpEvent()`, `sizeHint()` and the demand layer's repaints stay the same.
  With a null or destroyed demand layer the delegate is the base class, plus
  the choice editor.
- **The class comment.** Amend it: "Sizes and editing are the base class's"
  becomes a statement that sizes are the base class's, and so is editing,
  except for a Choice cell, whose list editor is described above. Say why the
  editor writes nothing when left on its opening entry.

### The context menu (`src/ui/docks/logbook/LogbookView.cpp`, `.h`)

- `onContextMenuRequested()` keeps building the menu as today, including one
  "Set %1..." action per editable attribute column. It captures the session
  ids and the attribute key before `menu.exec()`.
- The handling of a chosen "Set ..." action moves into a **public member
  function of `LogbookView`**. It takes what the menu captured: the attribute
  key, the column label and the session ids. The implementer names it; its
  comment says the context menu calls it and tests call it directly. Inside
  it:
  1. For a Choice attribute, ask with `QInputDialog::getItem()`, not
     editable. The items are the delegate's list, "Default" first, opening on
     "Default". Map the chosen text back to its entry's value. For any other
     attribute, keep `QInputDialog::getText()` exactly as it is.
  2. After the dialog closes, resolve the column and the sessions again, as
     today.
  3. Call `startBulkEdit(rows, column, value)` with the token, or with an
     invalid `QVariant` for "Default".
- The nested-event-loop comment stays true. Keep it with the code that runs
  the menu and the dialog, and keep its rule: no row or column index is held
  across either loop.
- Add a sentence to the class comment: editable attribute columns are set
  for the selected sessions from the context menu, and a Choice attribute is
  offered as a list.

### Not changed

- `src/logbookcolumn.cpp` and `src/preferences/addcolumndialog.cpp`. The
  label and the Add Column tree come from the registry, so a Choice attribute
  appears like any other.
- The importer, the exporter and the session-file format. A token is a
  `QString` attribute, written verbatim after `$VAR,<key>,`. A comma in a
  token needs nothing, because the importer takes the verbatim remainder of
  the line (`src/csvformat.h`).
- `docs/`. The choice type is described in `docs/DATA_SCHEMA.md` by phase 4,
  with the first attribute that uses it.
- `tests/acceptance_map.txt` (overview decision 15). No function the map
  cites is renamed.

## Interfaces

### Provided (to phase 4)

- `AttributeFormatType::Choice`,
  `struct AttributeChoice { QString token; QString label; };` and
  `QVector<AttributeChoice> choices;` as the last member of
  `AttributeDefinition`, all in `src/attributeregistry.h`. These are exactly
  as the overview states.
- **The model's contract for a Choice column.**
  - `Qt::DisplayRole` is the label of the effective token, or the raw text
    when the token has none.
  - Sort is by that text.
  - `setData(index, QString token, Qt::EditRole)` stores a token from the
    list. It returns false for any other value, or when the token is already
    stored.
  - `setData(index, QVariant(), Qt::EditRole)` removes the stored attribute.
    It returns false when nothing is stored.
  - `startBulkEdit(rows, column, value)` takes a token or `QVariant()` with
    the same meanings. It refuses any other value before queuing anything.
- **The editors.** The in-place editor and the context menu's list both
  show "Default" first, then the labels in definition order. Neither accepts
  free text.
- **The fixture.** `FlySightTest::ChoiceFixture` in
  `tests/support/choicefixture.h` and `.cpp`, compiled into
  `flysight_test_support`. It is widget-free and fusion-free (section 8 of
  `tests/README.md` keeps the support library free of both). It is
  parametrized by attribute key and reads the definition from the registry.
  It knows nothing about the test attribute. It provides at least:
  - **Start.** Adopt given sessions into a fresh `SessionModel` over the test
    logbook, with the logbook columns set to the description column plus
    `attributeColumn(key)`. The rows are either loaded, or stubs after a
    restart (as `tst_column_cache`'s `restartAsStubs()` does).
  - **Accessors.** The model, and the Choice column's index.
  - **Reads.** The display text of a session's cell. The token stored in the
    session's file on disk, which is absent when the file has no
    `$VAR,<key>` line (`DataImporter::peekHeaderAttribute()` reads it without
    parsing the data). The value `index.json` caches.
  - **Edits.** A `setData()` on a session's cell. A bulk edit of sessions
    followed by `waitForIdle()`.

  Following section 8, a helper that can fail returns `[[nodiscard]] bool`
  or a `QString` that is empty on success.
- **The executable.** `tst_choice_attribute` holds the generic choice tests,
  with a test-registered choice attribute. It is a Widgets test, so it cannot
  link `flysight_fusion`.
- **How phase 4 extends them.** Phase 4 tests `_ORIENTATION` in its fusion
  executable (`tst_fusion_derived`, phase 3's) through `ChoiceFixture`, after
  `registerFusionCalculations()`. That covers the model's display of the
  constant default, `setData()` and its refusal, "Default" as removal, and
  the bulk edit. Phase 4 also asserts that the definition's `choices` are the
  orientation type's enumeration of 24 with the default first. The editors
  need no fusion test: this phase proves that both present any Choice
  definition's `choices`, "Default" first and not editable.
- **A fact phase 4 must plan for.** `AttributeRegistry` lives in
  `flysight_core`, and `flysight_fusion` links only `flysight_model` today
  (`src/CMakeLists.txt`). Registering a definition from
  `registerFusionCalculations()` (overview decision 8) needs a link or
  placement decision in phase 4. This phase moves nothing.

### Consumed

None.

## Acceptance criteria

1. `AttributeFormatType` has five values, the last being `Choice`.
   `AttributeChoice` has exactly `token` and `label`. `choices` is the last
   member of `AttributeDefinition`. `attributeregistration.cpp`,
   `spcalculations.cpp` and `wspcalculations.cpp` are unchanged. (§9;
   overview "Phase 1 provides")
2. A Choice cell displays the label of the effective token, from a stored
   value or a calculated one, on loaded and stub rows alike. A token with no
   label displays as its raw text. (§9 "display ... by label"; decision 7)
3. Sorting a Choice column orders rows by that text, ascending and
   descending, on loaded and stub rows. Missing values are last. With the
   test attribute this order differs from both token order and definition
   order. (§9 "sorting by label")
4. `setData()` with a token from the list stores it. The session file then
   has `$VAR,<key>,<token>` (a token containing a comma included), and the
   cell shows its label. The change is published as a direct edit is
   (`dependencyChanged` for the key). (§9; §13 "the choice type")
5. `setData()` with a token outside the list, a label, or an empty string
   returns false. It changes nothing, emits no `dataChanged`, and leaves a
   stub row unloaded. (§8 "A value outside the list is refused"; §13)
6. `setData()` with an invalid `QVariant` removes the stored attribute. The
   file has no `$VAR,<key>` line afterwards. The cell shows the calculated
   default's label again, and dependents are notified. With nothing stored
   it returns false and nothing is saved. (§8 "Default ... removes the stored
   value"; §10 note; decision 9)
7. `startBulkEdit()` with a token sets it for every selected session, loaded
   and stub. The files, `index.json` and the cells agree afterwards. (§9 "the
   same bulk edit with the chosen token"; §13)
8. `startBulkEdit()` with an invalid `QVariant` removes the stored attribute
   on loaded and stub rows. No file keeps a `$VAR,<key>` line, and the cells
   show the default's label. (§9; decision 9)
9. `startBulkEdit()` with a token outside the list queues nothing: the
   scheduler never activates `SessionModel::BulkEditTask`, no file changes,
   and a stub's cached value is still there. (§8; §13 "refused by ... the
   bulk edit")
10. The in-place editor of a Choice cell is a non-editable `QComboBox`. It
    shows "Default" then the labels in definition order, and opens on the
    cell's label, or on no entry for raw text. Choosing a label stores its
    token, choosing "Default" removes the stored value, and closing on the
    opening entry writes nothing (the file bytes are unchanged). A Text cell
    still gets the base class's `QLineEdit`. (§9 "a list editor in place of
    the line edit"; §8; decision 9)
11. The "Set ..." dialog for a Choice attribute is a `QInputDialog` in combo
    box mode, not editable, with "Default" then the labels. Accepting a label
    bulk-edits its token onto the captured sessions, accepting "Default"
    removes the stored value, and rejecting changes nothing. A Text attribute
    still gets the text prompt. (§9 "a list in place of the text prompt";
    §8; decision 9)
12. The delegate's files still contain none of `editorEvent`,
    `mouse*Event` or `keyPressEvent`. `audit_cleanup` passes. The whole suite
    passes sequentially in `build-agent/`, and every existing text and
    number edit test passes unchanged.

## Tests

### New executable `tst_choice_attribute`

Register it in `tests/CMakeLists.txt` inside the `FLYSIGHT_BUILD_WIDGET_TESTS`
block, as `tst_logbook_indicators` is:

- compile `LogbookView.cpp` / `.h` and `LogbookCellDelegate.cpp` / `.h` from
  the application sources;
- `LIBS Qt${QT_VERSION_MAJOR}::Widgets`;
- labels `core;widgets`.

It writes its own `main()` in the widget form of `tests/README.md` section
8: deterministic hash seed, `QApplication`, Fusion style, `TestEnvironment`,
test object. The view is built with a **null** demand layer; nothing here
needs the executor.

The test choice attribute is registered once per process in
`initTestCase()` with a key of its own (for example `_TEST_CHOICE`), editable,
with three choices:

- tokens and labels are chosen so that label order, token order and
  definition order are three different orders;
- one token contains a comma, as phase 4's will.

A no-input calculation, registered on the global registry in the same place,
gives the attribute a default token, so the "effective token" and "Default
returns to it" cases are real. Phase 2's `addConstantDefault` does not exist
yet when this phase is implemented. Register the descriptor directly, as
`tst_calcengine::constantCalculationIsADefault()` does. `init()` snapshots
the registry and `cleanup()` compares it, as the other model suites do.

A token outside the list is planted in a session with
`SessionModel::updateAttribute()`, which stands in for a hand-edited file.

The functions below are proposed names. The implementer may rename them;
the coverage is fixed.

- **Model**
  - `choiceShowsTheLabelOfTheEffectiveToken` (rows: loaded, stubs): criterion 2.
  - `choiceSortsByLabel` (rows: loaded, stubs): criterion 3.
  - `choiceEditStoresAToken`: criterion 4, including a repeat of the same
    token, which returns false.
  - `choiceEditRefusesATokenOutsideTheList`: criterion 5.
  - `choiceDefaultRemovesTheStoredValue`: criterion 6.
- **Bulk edit**
  - `bulkEditSetsAToken` (rows: loaded, stubs): criterion 7.
  - `bulkEditDefaultRemovesTheStoredValue` (rows: loaded, stubs): criterion 8.
  - `bulkEditRefusesATokenOutsideTheList`: criterion 9, with the scheduler
    spy pattern of `BulkEditSignals` in `tst_column_cache`.
- **In-place editor**
  - `cellEditorOffersTheList`: criterion 10. Open the editor with
    `QAbstractItemView::edit()` on the view's tree. Find the combo box among
    the viewport's children. Commit with a key click, or with the delegate's
    `setModelData()`.
- **Context menu**
  - `setDialogOffersTheList`: criterion 11, through the seam below.

### The nested-event-loop seam

`QMenu::exec()` and `QInputDialog` each run a nested event loop, so a test
cannot simply call through them and inspect.

- **The menu's loop is skipped.** The test calls the new public function of
  `LogbookView` with a key, a label and session ids, just as the menu would
  after `exec()` returns.
- **The dialog's loop is driven.** Before calling, the test arms a timer on
  the main thread. The timer waits for `QApplication::activeModalWidget()` to
  be a `QInputDialog`, then records:
  - `inputMode()`;
  - `comboBoxItems()`;
  - `isComboBoxEditable()`.

  It then either picks an entry with `setTextValue()` and `accept()`s, or
  `reject()`s.
- **After the call returns,** the test waits with `waitForIdle()` and checks
  the files and the cells through the fixture.

This needs no human and no platform popup. `QT_QPA_PLATFORM=offscreen`
suffices. Connect the timer with a function-local `QObject` context, as
section 8 requires for slots that capture locals.

### `tests/CMakeLists.txt`

- Add `support/choicefixture.cpp support/choicefixture.h` to
  `flysight_test_support`.
- Register `tst_choice_attribute` as described above.
- Update the two comments that say `tst_logbook_indicators` and
  `tst_status_bar` are the only tests that link Qt Widgets: the file's head
  and the widget block.

### `tests/README.md`

- **Section 1.** Add a row for `tst_choice_attribute`, placed with the other
  widget tests. Raise the executable count and the two `ctest -N` counts by
  one each. Name the third Widgets test in the opening paragraph and in the
  two widget rows' "first/second of the two" wording.
- **Section 3.** Add the new test to the `FLYSIGHT_BUILD_WIDGET_TESTS` row.
- **Section 8.** Name the new test in "(`tst_logbook_indicators` and
  `tst_status_bar` are the only ones)". Add `choicefixture.h` (`ChoiceFixture`)
  to the list of shared helpers.

### Unchanged

- The audit: this phase adds no rule. Nothing is replaced, so no old name
  needs keeping out. A rule on `choices` would name a member that phase 4
  legitimately fills in the fusion registration.
- `tests/acceptance_map.txt`.
- `docs/`.
- Section 12's manual steps are phase 6's (§12.7).

## Decisions

1. **The tests live in a new widget executable, with a shared widget-free
   fixture.** The editors need Qt Widgets, and the model and bulk-edit cases
   share the same world. `tst_logbook_indicators` is about the demand layer's
   indicators and builds an executor and a demand layer for every function,
   which would muddy both files. Phase 4's real attribute needs
   `flysight_fusion`, which only the FUSION block may link, and a Widgets
   executable cannot join that block. The reusable part is therefore the
   fixture in `flysight_test_support`, not the executable.
2. **`setData()` compares with the stored value, not the effective one.**
   Choosing a value is pinning it. A recording that shows the default only
   because nothing is stored should be able to store the same token
   explicitly, from the context menu, so that a later change of the default
   does not move it. The Text case keeps its effective-value comparison.
3. **The in-place editor writes nothing when left on its opening entry.** An
   unset recording shows the default's label. Opening its editor and pressing
   Enter must not pin that value by accident, and a stub's display cannot
   tell stored from calculated without a load. The consequence: pinning a
   value equal to the current default is done from the context menu, not in
   place. A raw token opens on no entry, so choosing "Default" there is a
   change and removes it.
4. **The "Set ..." dialog opens on "Default".** It is the first entry, the
   selected sessions may hold different values, and there is no single
   current value to open on.
5. **Validation precedes the force-load in `setData()`,** so a refused edit
   has no side effect, not even loading a stub.
6. **`updateAttribute()` is not validated.** It is the programmatic writer
   that the plot tools and analysis docks use for known non-choice keys, not
   an editor. Spec §8 and §13 ask for refusal by the model's edit path, the
   editors and the bulk edit. Decision 7 of the overview expects a stored
   token without a label to exist and be shown.
7. **The definition's preconditions are stated, not enforced** (unique
   non-empty tokens, unique labels, none "Default"). The only definitions
   are the test's and phase 4's, whose list comes from one enumeration.
   Checking would be generality nothing uses.
8. **The test attribute's default is a directly registered no-input
   calculation.** Phase 2's helper lands after this phase, and the mechanism
   is the engine's own (`constantCalculationIsADefault`). Phase 2 may switch
   the test to the helper but need not.

Ready, with two caveats: phase 4 must settle how `flysight_fusion` reaches
`AttributeRegistry`, which lives in `flysight_core`; and phase 3's README
executable count must be counted from the tree as phase 1 leaves it.
