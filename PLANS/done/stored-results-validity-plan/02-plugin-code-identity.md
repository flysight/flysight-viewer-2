# Phase 2: Plug-in code identity

## Overview

A Python plug-in can change between runs under the same calculation id, so
every attribute, measurement and calculation the plug-in host registers
declares a result version: the plug-in code identity, one SHA-256 digest over
every `*.py` file under the plug-in folder (recursively, so helper modules
and helper packages in subfolders are included), the SDK file, and the Python
and numpy versions, computed once at plug-in load (spec section 6). The calculation
environment fingerprint that stamps the logbook column cache in `index.json`
also covers every registration's result version, so a plug-in edit (or a
built-in result-version change) discards cached column values at the next
start exactly as a registry change does today.

## Dependencies

- **Depends on:** None — can begin immediately.
- **Blocks:** Phase 3 (its plug-in staleness tests build stand-in plug-in
  registrations whose result version is `pluginCodeIdentity(...)` over chosen
  ingredients, and rely on the resolutions of Phase 1 carrying that version),
  Phase 4 (documents the identity and catalogues the new test).
- **Assumptions:**
  - Branch `store-requested-calculations` at or after ce2fb2b. Phase 1 may be
    implemented in parallel; it edits `src/engine/` (engine, snapshot,
    registry). This phase edits exactly one comment sentence in
    `src/engine/calculationdescriptor.h` and nothing else under `src/engine/`
    (see Task 2.3 and Gotchas).
  - `CalculationDescriptor::resultVersion` exists (it does, since the
    stored-results work) and the registry keeps what it is given.
  - The plug-in host registers plug-ins before the built-ins, once per process
    (`src/mainwindow.cpp` ~154-161, `src/pluginhost.h` 36-47).

## Tasks

### Task 2.1: The plug-in code identity function

**Purpose:** One pure, Python-free function that turns the ingredients into
the identity string, so it is deterministic, testable in a core test, and
usable by Phase 3's tests to build stand-in plug-in registrations.

**Files to create:**
- `src/plugincodeidentity.h` — declarations below, with the byte encoding
  documented in the header comment (it is the contract).
- `src/plugincodeidentity.cpp` — implementation (Qt Core only:
  `QCryptographicHash`, `QDir`, `QFile`, `QFileInfo`).
- `tests/tst_plugin_identity.cpp` — core test (Testing Requirements).

**Files to modify:**
- `src/CMakeLists.txt` — add `plugincodeidentity.cpp plugincodeidentity.h` to
  `flysight_core` (after `altitudemarkerfeature.cpp`, ~line 305, with a
  one-line comment: "Plug-in code identity: the result version of every
  plug-in registration (no Python)"). Not in `flysight_model` (the Python
  bridge module must not gain it) and not in the executable list (the core
  test must link it).
- `tests/CMakeLists.txt` — `flysight_add_test(tst_plugin_identity SOURCES tst_plugin_identity.cpp)`
  in the "Persistence" block or a new one-line-commented block right after
  `tst_builtins_engine` ("Plug-in code identity: the digest over the plug-in
  folder, the SDK and the versions (no Python)"). Labels: the default `core`
  only — it must run where Python tests are disabled.

**Technical Approach:**

Namespace `FlySight`. Public surface (names are binding for Phases 3-4):

```cpp
/// One *.py file anywhere under the plug-in folder.
struct PluginSourceFile {
    QString name;                       ///< path relative to the plug-in folder, '/' separators ("pkg/helper.py")
    std::optional<QByteArray> bytes;    ///< nullopt: exists but could not be read in full
};

struct PluginCodeIngredients {
    QList<PluginSourceFile> files;      ///< any order; the digest sorts them
    std::optional<QByteArray> sdk;      ///< the imported SDK file's bytes; nullopt: not readable
    QString pythonVersion;              ///< e.g. "3.13.3"; empty: unknown
    QString numpyVersion;               ///< e.g. "2.2.4"; empty: numpy absent / unreadable
};

inline constexpr char PluginCodeIdentityPrefix[] = "plugins-sha256:";
/// Written in place of a version that could not be read.
inline constexpr char PluginCodeIdentityUnknownVersion[] = "none";

/// The whole file, or nullopt when it cannot be opened, reading fails, or
/// fewer bytes than QFile::size() were read.
std::optional<QByteArray> readWholeFile(const QString &path);

/// Every *.py file under `pluginDir`, recursively, read with readWholeFile(),
/// sorted by name (see below). Not the host's import list: that stays the
/// top-level *.py files only.
QList<PluginSourceFile> readPluginCodeFiles(const QString &pluginDir);

/// PluginCodeIdentityPrefix + 64 lower-case hex digits. Pure function of the
/// ingredients.
QString pluginCodeIdentity(const PluginCodeIngredients &ingredients);
```

Byte encoding (SHA-256, `QCryptographicHash::Sha256`, over the concatenation
of the following, all text UTF-8; `<n>` is a decimal byte count with no
padding; `\n` is a single LF):

1. `flysight-plugin-code-identity 1\n`
2. `python <n>\n<v>\n` — `v` = `pythonVersion`, or `none` when it is empty.
3. `numpy <n>\n<v>\n` — `v` = `numpyVersion`, or `none` when it is empty.
4. The SDK: `sdk <n>\n<bytes>\n`, or `sdk unreadable\n` when `sdk` is nullopt.
5. `files <count>\n` — the number of entries in `files`.
6. For each file, in ascending order of `name` by
   `QString::compare(a, b, Qt::CaseSensitive)` (ordinal; independent of the
   order the caller passed and of the file system's listing order):
   `name <n>\n<name>\n`, then `bytes <n>\n<bytes>\n`, or `unreadable\n` when
   `bytes` is nullopt.

Every variable-length field is length-prefixed, so the encoding is injective
(moving bytes between two files, or between a name and its content, changes
the digest). The result is `QString::fromLatin1(PluginCodeIdentityPrefix) +
QString::fromLatin1(hash.result().toHex())`.

Only the path relative to the folder enters, never an absolute path: moving
the application or the plug-in folder does not change the identity.

`readPluginCodeFiles()` walks the folder recursively (a small recursive
function over `QDir`, not `QDirIterator`, so that directories can be pruned):

- In each directory, files: `entryInfoList({QStringLiteral("*.py")}, QDir::Files)`
  — the same filter flags the host's import listing uses (no `QDir::Hidden`,
  so hidden files are skipped at every level, exactly as the host skips them
  at the top level).
- Subdirectories: `entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)` (no
  `QDir::Hidden`), additionally skipping any directory named `__pycache__`,
  any whose name starts with `.` (hidden on every platform, not only where the
  file system says so), and any that is a symbolic link or a junction
  (`QFileInfo::isSymLink()` / `isJunction()`), so the walk cannot cycle.
  Symbolic links to files are read (their target's bytes).
- `name` = `QDir(pluginDir).relativeFilePath(fi.absoluteFilePath())`, which
  Qt already writes with `/` separators on every platform (e.g. `a.py`,
  `pkg/helper.py`, `pkg/sub/deep.py`). Top-level files keep their plain file
  name.
- The result is sorted by `QString::compare(a.name, b.name, Qt::CaseSensitive)`;
  the digest sorts again, so ordering never depends on the walk.
- An empty or missing folder gives an empty list.

The digested list is therefore NOT the host's import list: the host still
imports only the top-level `*.py` files (Task 2.2 leaves its loop as it is),
while the digest also covers subfolders — helper packages a plug-in imports,
and anything else such as the installed `examples/imu_tilt.py`. Every file the
host imports is in the digested list (same flags at the top level); the
converse does not hold.

**Acceptance Criteria:**
- [ ] `pluginCodeIdentity()` returns a string matching
      `^plugins-sha256:[0-9a-f]{64}$` for any ingredients, including no files,
      an unreadable SDK and empty versions.
- [ ] The same ingredients give the same identity; the same files passed in a
      different list order give the same identity.
- [ ] The identity equals SHA-256 over the byte string of the encoding above,
      built by hand in the test for a fixed example with the files `a.py` and
      `pkg/helper.py` passed in the order `pkg/helper.py`, `a.py` (pins the
      encoding, the `/` name form and the sort).
- [ ] Each of these changes the identity (one data row each): one file's bytes
      edited; a file renamed with the same bytes; a file added (a helper
      module); a file removed; the SDK bytes edited; the SDK unreadable instead
      of empty bytes; a file unreadable instead of empty bytes; the Python
      version changed; the numpy version changed; numpy absent (empty) instead
      of a real version; the bytes `"x"` moved from file `a.py` to file `b.py`.
- [ ] An empty `numpyVersion` gives the same identity as `numpyVersion ==
      "none"` (the fixed token), and likewise for `pythonVersion`.
- [ ] On a temporary folder holding `a.py`, `b.py`, `notes.txt`, `sub/c.py`,
      `sub/deeper/d.py`, `__pycache__/x.py`, `__pycache__/a.cpython-313.pyc`,
      `sub/__pycache__/y.py` and `.hidden/e.py`, `readPluginCodeFiles()` returns
      exactly the names `a.py`, `b.py`, `sub/c.py`, `sub/deeper/d.py`, in that
      order, with their bytes.
- [ ] On that folder, the identity (fixed SDK and versions) changes when
      `a.py`, `sub/c.py` or `sub/deeper/d.py` is rewritten on disk, when a new
      `sub/e.py` is added, and when `sub/c.py` is removed; it does not change
      when `notes.txt`, `__pycache__/x.py`, `__pycache__/a.cpython-313.pyc`,
      `sub/__pycache__/y.py` or `.hidden/e.py` is rewritten, or when a new file
      is added under `__pycache__/`.
- [ ] `readWholeFile()` of a path that does not exist gives `std::nullopt`;
      of an empty file, an empty `QByteArray` (not nullopt).
- [ ] `plugincodeidentity.*` include no Python or pybind11 header and are
      compiled into `flysight_core` only.

**Complexity:** M

---

### Task 2.2: The host computes the identity and declares it on every registration

**Purpose:** Every attribute, measurement and calculation the plug-in host
registers carries the identity as its `resultVersion`, computed once, before
any plug-in is imported.

**Files to modify:**
- `src/pluginhost.h` — accessor and member; class comment.
- `src/pluginhost.cpp` — compute the identity; pass it to `registerEach`.
  The import loop (step 3) is unchanged.
- `tests/tst_python_bridge.cpp` — new test function (Testing Requirements).

**Technical Approach:**

1. `PluginHost` gains `const QString &codeIdentity() const` (empty until
   computed) and `QString m_codeIdentity`. Extend the `initialise()` comment:
   "Before any plugin is imported, the plug-in code identity
   (plugincodeidentity.h) is computed over every *.py file under `pluginDir`
   (subfolders included, although only the top-level files are imported),
   the SDK file actually imported, and the Python and numpy versions; it is
   the result version of every attribute, measurement and calculation it
   registers. Empty when plugin loading did not run."

2. In `initialise()`, immediately after the SDK import succeeded and
   `m_ready = true` (~line 318) and before step 3:
   - Files: `ingredients.files = readPluginCodeFiles(pluginDir);`
   - SDK: the path is `sdk.attr("__file__")` cast to `std::string` →
     `QString::fromStdString`, read with `readWholeFile(path)`.
     If `__file__` is missing, not a `str`, or raises, `sdk` stays nullopt.
     The SDK normally lives in `pluginDir` (README section 1; `src/CMakeLists.txt`
     ~596 installs it there), so it is then both a folder file and the SDK
     entry — that is intended and harmless; using `__file__` covers an SDK
     found elsewhere on `sys.path`.
   - Python version: `Py_GetVersion()` up to (not including) the first space,
     e.g. `"3.13.3"` (what `platform.python_version()` reports). No Python
     import needed.
   - numpy version: `py::module_::import("numpy").attr("__version__")` cast to
     `std::string`, inside `try { … } catch (const py::error_already_set &) {}
     catch (const std::exception &) {}`; on any failure the version is empty
     (the digest writes `none`). The SDK imports numpy, so in practice this
     never fails once the SDK imported.
   - `m_codeIdentity = pluginCodeIdentity(ingredients);` and
     `qInfo().noquote() << "[PluginHost] Plug-in code identity:" << m_codeIdentity
     << QStringLiteral("(%1 files)").arg(ingredients.files.size());`
   - Step 3 is left exactly as it is: it keeps its own
     `dir.entryInfoList({ "*.py" }, QDir::Files, QDir::Name)` and imports the
     top-level files only. The digested list and the imported list are two
     different lists: the digest's is a superset (subfolders included) built
     by `readPluginCodeFiles()`; the import list is derived as today.
     Computing the identity first means the bytes digested are read before any
     plug-in code runs.

3. `registerEach(py::handle sdkList, const QString &resultVersion,
   PluginLoadReport &report, Fn makeAdapter)`: after
   `CalculationDescriptor d = makeAdapter(index, plugin);` set
   `d.resultVersion = resultVersion;` before `registry.registerCalculation(d)`.
   The three calls in step 4 pass `m_codeIdentity`. The adapters in
   `src/pluginadapters.cpp` stay unchanged (they build descriptors;
   the host decides the version — one place for all three lists).

4. Nothing else changes: plots and markers are not calculations; plug-in ids,
   registration order, rejection and the report are unchanged; the host is
   still initialised once per process and never reloads or watches files
   (spec 9, third bullet).

What happens in the non-loading cases (all leave `m_codeIdentity` empty and
register no plug-in calculation, so nothing carries an identity and the
environment fingerprint is what the registrations alone give):
- `pluginDir` empty (e.g. `FLYSIGHT_PLUGINS` set to an empty string): early
  return before the SDK (unchanged).
- The interpreter, the bridge module or the SDK fails to load: early return
  (unchanged).
- A folder with the SDK and no plug-in: the identity is computed (over the SDK
  file etc.) but no registration carries it.
- A plug-in file that fails to import still enters the digest (it is in the
  folder); it registers nothing.
- A `*.py` file in a subfolder is never imported by the host but enters the
  digest, so editing a helper package, or the installed `examples/imu_tilt.py`,
  changes the identity.

**Acceptance Criteria:**
- [ ] After `initialise()` in `tst_python_bridge`, `codeIdentity()` matches
      `^plugins-sha256:[0-9a-f]{64}$`.
- [ ] For every id in `report().registeredIds`,
      `CalculationRegistry::instance().instance(id)->descriptor->resultVersion ==
      codeIdentity()` (attributes, measurements and calculations alike).
- [ ] `codeIdentity()` equals `pluginCodeIdentity()` recomputed in the test
      from `readPluginCodeFiles(m_pluginDir)`, the SDK bytes of `flysight_plugin_sdk.__file__`, `platform.python_version()`
      and `numpy.__version__` (read through pybind11 in the test — an
      independent path to the same ingredients).
- [ ] A built-in registered after the plug-ins (`builtin.attr.exitTime`) still
      has an empty `resultVersion`.
- [ ] A second `initialise()` leaves `codeIdentity()` unchanged
      (`secondInitialiseIsNoOp` gains this check).
- [ ] `registrationOrderIsDeterministic`, `pluginWorkflowThroughModel` and every
      other existing `tst_python_bridge` function pass unchanged.
- [ ] The host's import loop (step 3) is unchanged: `report().failedImports`
      and the registered ids are the same as before this phase (the existing
      `registrationOrderIsDeterministic` expectations hold verbatim).

**Complexity:** M

---

### Task 2.3: The environment fingerprint covers every registration's result version

**Purpose:** A changed result version of any registration (a plug-in edit, a
Python/numpy upgrade, a new fit algorithm string) discards the cached logbook
column values at the next start, as a registry change does.

**Files to modify:**
- `src/calculations/builtincalculations.cpp` — `calculationEnvironmentFingerprint()`.
- `src/calculations/builtincalculations.h` — the fingerprint's comment; one
  clause in the `CalculationCompatibilityVersion` comment.
- `src/engine/calculationdescriptor.h` — one sentence of the `resultVersion`
  comment (see Gotchas on Phase 1).
- `tests/tst_builtins_engine.cpp` — new test function.

**Technical Approach:**

1. In `calculationEnvironmentFingerprint()` build, once per call, a
   `QHash<CalculationId, QString>` from `registry.registeredIds()`: for each id,
   `registry.instance(id)` (a plain calculation yields its descriptor; a family
   or source-conversion family yields nullopt without a name, see
   `src/engine/calculationregistry.cpp` 334-347) → `descriptor->resultVersion`
   when non-empty. No registry API is added.

2. The `addList` lambda writes, for each id, `"#<id>\n"` when the id has no
   (or an empty) result version — byte-identical to today — and otherwise
   `"#<id>#<escaped version>\n"`, where escaping replaces every `\` with `\\`
   and every LF with the two characters `\n` (in that order). An id cannot
   contain `#`, so the second `#` delimits. This applies to every list the
   fingerprint writes (by output, families, conversions); families and
   conversions never have a version, so those lists are unchanged. Candidate
   order is unchanged: a version is written wherever its id appears, i.e. in
   candidate order, once per output name it declares.

3. Everything else in the fingerprint (labels, sort, preferences, SHA-1, 40 hex
   characters) is unchanged, as is its role: `LogbookManager::initialize()`
   compares it at start (`src/logbookmanager.cpp` ~199), `flushIndex()` writes
   the one captured (`m_cacheEnvironment`), and
   `SessionModel::checkCalculationEnvironment()` compares it after a registry
   change or a preference change. None of those changes: a result version can
   change only by unregistering and re-registering (which already notifies the
   observers) or between runs (caught at start).

4. Comments in `builtincalculations.h`:
   - Fingerprint comment: the list items become `+ candidate ids, each with its
     result version` and a paragraph states: "Each id line is `#<id>\n`, or
     `#<id>#<version>\n` when the registration declares a result version
     (backslash and line feed escaped as `\\` and `\n`); family and
     conversion ids never carry one. A result version is declared by every
     Python plug-in registration (the plug-in code identity, plugincodeidentity.h)
     and by the sensor fusion fit (its algorithm string); built-ins declare
     none." Replace the "Not covered: …" paragraph with: "Not covered: a
     registration whose code changed while its id and result version did not;
     for the built-ins, that is what CalculationCompatibilityVersion is for."
   - `CalculationCompatibilityVersion` comment, the sentence "Do not bump for
     pure additions / removals / renames of registrations - the environment
     fingerprint below already covers those." becomes "… of registrations, or
     for a changed result version - the environment fingerprint below already
     covers those." The value stays 2; no History line (History records marker
     values, and this change does not bump it).
   - Do NOT write `CalculationDescriptor::resultVersion`, `fusion/`, `Fusion::`
     or the fit's algorithm literal in this header (audit rules, see Gotchas).

5. `src/engine/calculationdescriptor.h`, `resultVersion` comment: the sentence
   "Opaque text to the engine: it never affects evaluation, identity,
   candidate order, or the environment fingerprint." becomes "Opaque text to
   the engine: it never affects evaluation, identity or candidate order. The
   calculation environment fingerprint (the logbook column cache) covers it,
   and every Python plug-in registration declares the plug-in code identity."
   Change that sentence only.

Effect on existing logbooks: the application registers the fusion fit, which
declares a result version, so the application's fingerprint changes once with
this build; the first start discards every cached column value in `index.json`
(`cachedValuesDiscardedOnLoad()`), recomputes lazily and rewrites the stamps.
Session files are untouched. Records written by earlier builds (format 1,
which carry the environment fingerprint) read stale at their next load and
are deleted — the same outcome Phase 3's format bump gives. A registry of
built-ins only (tests' private `World` registries) has the same fingerprint as
before.

**Acceptance Criteria:**
- [ ] For a private `CalculationRegistry` with one plain calculation
      `test.pinned` (output attribute `_TEST_PINNED`) and no preference
      provider, the fingerprint equals SHA-1 hex of
      `"attribute:_TEST_PINNED\n#test.pinned\nfamilies\nconversions\n"` with an
      empty result version, and of
      `"attribute:_TEST_PINNED\n#test.pinned#a\\\\b\\nc\nfamilies\nconversions\n"`
      (C++ literal) with result version `"a\\b\nc"` (C++ literal: backslash,
      line feed).
- [ ] On a built-ins `World` registry: registering an extra calculation with
      result version `"v1"` gives a fingerprint different from the same
      registration with `""`; re-registering it with `"v2"` differs from
      `"v1"`; re-registering with `"v1"` again restores the `"v1"` value.
- [ ] With two candidates for one output, changing the result version of the
      second (not first-tried) candidate changes the fingerprint.
- [ ] `fingerprintChanges`, `fingerprintSurvivesRuntimeAltitudeMarker`,
      `tst_logbook_index::differentEnvironmentDiscards`,
      `environmentIsTheCachedOne`, `tst_column_cache::upgradeDiscardsAndRecomputes`
      and every other existing test pass unchanged.
- [ ] `CalculationCompatibilityVersion` is still `2`; the audit passes
      (`CalculationDescriptor::resultVersion` still appears on exactly one line
      of `builtincalculations.h`; no `Fusion::`/`fusion/` in `src/calculations`).

**Complexity:** S

---

### Task 2.4: A plug-in edit discards cached column values at the next start

**Purpose:** The end-to-end form of spec section 6's last paragraph and of the
section 10 test "A plug-in edit discards cached logbook column values over
plug-in calculations at the next start", in a core test (the interpreter
cannot be booted twice in one process).

**Files to modify:**
- `tests/tst_column_cache.cpp` — new test function
  `pluginEditDiscardsCachedValues`.

**Technical Approach:**

A stand-in plug-in registration on the application registry, whose result
version is a real `pluginCodeIdentity()`; an application restart is
`restartAsStubs()` (`reopenLogbook()` + `initialize()` + new model), and "the
next start with an edited plug-in" is unregister + re-register with the
identity over the edited bytes before `restartAsStubs()`.

1. Constants in the test file: `kPluginId = "test.columncache.plugin"`,
   output attribute `_TEST_COLUMNCACHE_PLUGIN`; the descriptor declares input
   `DependencyKey::attribute("_DESCRIPTION")` and returns the constant `7.0`.
   Ingredients `v1`: files `{"a_plugin.py": "x = 1\n", "helper.py": "y = 2\n"}`,
   SDK `"sdk"`, Python `"3.13.3"`, numpy `"2.2.4"`; `v2` = `v1` with
   `a_plugin.py` = `"x = 2\n"`.
2. Add a `SessionAttribute` column over `_TEST_COLUMNCACHE_PLUGIN` to
   `{m_d, m_g, m_e}` (restored by `cleanup()`), register the stand-in with
   `resultVersion = pluginCodeIdentity(v1)`, then
   `env.reopenLogbook(); logbook.initialize();` so the start environment
   includes it. `startWithLoadedSessions({gyroSession()})`; the index's
   `calculationEnvironment` equals `calculationEnvironmentFingerprint()` and
   holds the plug-in column value 7.
3. Control — restart with the unedited plug-in: `m_model.reset()`, unregister,
   re-register with `pluginCodeIdentity(v1)`, `restartAsStubs()`:
   `!cachedValuesDiscardedOnLoad()`, the row's plug-in column value is 7
   served from the index, and after `startColumnWorker()` + `waitForIdle`
   `columnWorkStats().sessionsLoaded == 0`.
4. Edit — restart with `pluginCodeIdentity(v2)`: `cachedValuesDiscardedOnLoad()`;
   the stub's `cachedValues` is empty; after the worker the plug-in column is
   7 again and `sessionsLoaded == 1`; the index's `calculationEnvironment`
   equals the new `calculationEnvironmentFingerprint()`, which differs from
   the one of step 2; the session file's bytes are unchanged.
5. Unregister at the end; `cleanup()` also calls
   `CalculationRegistry::instance().unregister(kPluginId)` before its registry
   comparison (the `tst_logbook_index` `kExtraId` pattern, lines 118-121), so
   a failing run does not cascade.

**Acceptance Criteria:**
- [ ] `tst_column_cache::pluginEditDiscardsCachedValues` passes with every
      assertion listed in steps 2-4.
- [ ] `tst_column_cache` passes as a whole, and its `cleanup()` registry check
      holds after the new function.

**Complexity:** S

## Testing Requirements

### Unit Tests
- New `tests/tst_plugin_identity.cpp` (core label, no Python):
  `identityIsDeterministic`, `encodingIsPinned`,
  `eachIngredientChangesIdentity` (data-driven, rows as in Task 2.1),
  `absentVersionUsesFixedToken`, `readsTheFolderRecursively` (the listing
  criterion), `subfolderFileChangesIdentity` (a rewritten, added or removed
  file in `sub/` or `sub/deeper/` changes the identity),
  `pycacheAndHiddenDirectoriesAreIgnored` (rewriting or adding files under
  `__pycache__/`, `sub/__pycache__/` or `.hidden/` leaves it unchanged),
  `unreadableFileIsNotEmpty`. Temporary folders through
  `TestEnvironment::instance().newTempDir()`; nothing written to the source tree.
- `tests/tst_builtins_engine.cpp`: new `fingerprintCoversResultVersions`
  (Task 2.3 criteria 1-3). `fingerprintChanges` unchanged.
- `tests/tst_python_bridge.cpp`: new `pluginRegistrationsCarryCodeIdentity`
  (Task 2.2 criteria 1-4; the test's plug-in folder holds top-level files
  only, so the recursive listing and the imported files coincide there);
  `secondInitialiseIsNoOp` gains the identity check.
  Follow the file's rule: no second `initialise()` beyond the existing one in
  `secondInitialiseIsNoOp`; read `platform`/`numpy` under
  `py::gil_scoped_acquire` as `bootsRealBridge` does.
- `tests/tst_column_cache.cpp`: new `pluginEditDiscardsCachedValues` (Task 2.4).

### Integration Tests
- Full suite on `build-phase1` only:
  `cmake --build build-phase1 --config Release`, then
  `cmake build-phase1/FlySightViewer-build` (a new test source exists), build
  again, then
  `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`.
  All labels green (core, fusion, exact, python, audit). **Never build `build/`.**
- `tst_plugin_identity` does not link fusion, so `_FLYSIGHT_GTSAM_REACHERS` is
  not touched.

### Manual Verification
- Start the application with a plug-in folder containing the SDK, a copy of
  `examples/imu_tilt.py` at the top level, and the `examples/` subfolder: the
  log shows `[PluginHost] Plug-in code identity: plugins-sha256:… (3 files)`.
  Quit, add a comment line to the top-level copy, restart: the identity in the
  log differs, and `index.json`'s `calculationEnvironment` is rewritten after
  the column worker settles. Repeat with an edit to `examples/imu_tilt.py`
  (not imported): the identity changes too. Restart again without edits: the
  identity and `calculationEnvironment` are unchanged and no cached value is
  recomputed.

## Notes for Implementer

### Gotchas
- **Audit, `src/calculations`:** `builtincalculations.h` must keep exactly one
  line containing `CalculationDescriptor::resultVersion` (rule "the bump rule
  names the result version"); write "result version" in new comments. No
  `fusion/` or `Fusion::` anywhere under `src/calculations` (rule "nobody but
  the application references the fusion library"), and the fit's algorithm
  literal appears only in `src/fusion/fusion.h` — say "the sensor fusion fit
  (its algorithm string)".
- **Audit, compatibility marker:** `CalculationCompatibilityVersion *=` must
  stay on one line in `src`; do not bump it.
- **Phase 1 overlap:** Phase 1 edits `src/engine/` in parallel. This phase
  changes only the one `resultVersion` sentence in
  `src/engine/calculationdescriptor.h`. If Phase 1 has already reworded that
  comment, keep its wording and only remove the claim that the result version
  never affects the environment fingerprint, adding the coverage sentence.
  Do not touch `calculationregistry.*`.
- `registry.instance(id)` for a family id without a name returns nullopt by
  design; do not call it with a made-up name.
- The host holds the GIL on the main thread during `initialise()`; the numpy
  and `__file__` reads need no extra GIL handling there. In the test, use
  `py::gil_scoped_acquire` as the existing functions do.
- Do not change the host's import listing (step 3): the digest's recursive
  walk is separate. In the walk, use the same file flags as the host
  (`QDir::Files`, no `QDir::Hidden`, no `QDir::CaseSensitive`) so every
  imported top-level file is digested.
- The walk must prune `__pycache__`, dot-named and symlinked/junction
  directories before descending; do not use `QDirIterator::Subdirectories`
  (it cannot prune and follows no exclusion rule). `__pycache__` normally holds
  only `.pyc` files, but a `.py` placed there must still be ignored (tested).
- Names must be relative with `/` separators on Windows too; use
  `QDir::relativeFilePath`, never `QDir::toNativeSeparators`.
- The identity is read from the files before any plug-in is imported, so a
  plug-in that rewrites files at import (it must not) cannot make the digest
  disagree with what was imported.
- A large tree under the plug-in folder (e.g. a virtual environment) is read
  in full at every start; accepted — the folder is meant to hold plug-ins.

### Decisions Made
- **Location:** `src/plugincodeidentity.{h,cpp}` in `flysight_core` (Qt Core
  only) so core tests and Phase 3 can use it without Python; the host reads
  the ingredients and calls it.
- **Encoding:** a versioned header line plus length-prefixed fields (injective);
  files sorted by ordinal case-sensitive name inside the function; names are
  paths relative to the plug-in folder with `/` separators; absolute paths
  never enter.
- **Recursive coverage (coordinator decision, superseding the overview's "the
  same set the host imports"):** every `*.py` under the folder, excluding
  `__pycache__` and hidden (dot-named or file-system-hidden) directories and
  not descending into symlinked directories, so helper packages in subfolders
  are covered (spec 6: "helper modules included"). The host still imports the
  top-level files only; the digested list is a separate, larger list. The
  installed `examples/imu_tilt.py` is therefore part of the identity: an
  application update that changes the example stales plug-in results once,
  which is accepted.
- **SDK location:** the SDK module's `__file__`, not `pluginDir +
  "/flysight_plugin_sdk.py"`, so an SDK found elsewhere on `sys.path` is still
  covered. When it lives in the folder it is digested twice (as a folder file
  and as the SDK field); harmless and deterministic.
- **Versions:** Python from `Py_GetVersion()` up to the first space (no
  import; equals `platform.python_version()`); numpy from
  `numpy.__version__`; an unreadable or empty version is written as the fixed
  token `none` by the digest function itself (one place).
- **Unreadable files** are encoded distinctly from empty ones (`unreadable`),
  so an identity is never vouched for content that was not read.
- **Where the version is set:** in `registerEach`, not in the adapters, so all
  three SDK lists get it in one place and the adapters stay builders.
- **Fingerprint encoding:** an empty result version leaves the id line
  byte-identical to today, so built-ins-only registries keep their
  fingerprint; a non-empty one appends `#<escaped version>`. Versions are
  looked up per call through `registeredIds()` + `instance(id)`; no registry
  API change (Phase 1 owns `src/engine/`).
- **No `CalculationCompatibilityVersion` bump and no History line:** the
  fingerprint change itself discards existing caches once in the application
  (the fit declares a version); History records marker values only.
- **Plug-in edit test** lives in `tst_column_cache` with a stand-in
  registration carrying a real `pluginCodeIdentity()`, because one process
  cannot boot the interpreter twice; `tst_python_bridge` proves the real host
  declares that same function's output.
- `tst_logbook_index` needs no new function: its two environment tests cover
  the index mechanics and pass unchanged; the result-version input is covered
  by `tst_builtins_engine` and `tst_column_cache`.

### Open Questions
- None blocking. Hand-off to Phase 4: document the identity (what it covers,
  every `*.py` under the folder including subfolders but not `__pycache__` or
  hidden folders, although only top-level files are imported; SDK,
  Python/numpy, `none` token; editing a plug-in
  stales stored results that used it and discards cached column values once)
  in `python_plugins/README.md` and `docs/DATA_SCHEMA.md` §11; catalogue
  `tst_plugin_identity`, `fingerprintCoversResultVersions`,
  `pluginRegistrationsCarryCodeIdentity` and `pluginEditDiscardsCachedValues`
  in `tests/README.md` and the acceptance map.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria
2. All tests pass (full suite on `build-phase1`, including the audit)
3. Code follows patterns established in reference files
4. No TODOs or placeholder code remains
