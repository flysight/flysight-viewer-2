# Phase 2: Record files in the logbook

## Overview

This phase defines the on-disk record of one stored requested-calculation
result: a versioned binary encoding of the engine's snapshot (Phase 1) plus
the two code stamps, and a file name that the logbook's session scan cannot
mistake for a session. It then gives `LogbookManager` the only code that
touches record files: atomic write, read, list, delete one or all of a
session, deletion together with the session, and removal of stray records
when the logbook is scanned at start-up. Nothing here is wired into
`SessionModel`. Publishing and restoring are Phase 3.

## Dependencies

- **Depends on:** Phase 1 (Engine snapshot and restore). It defines the
  snapshot value type the record encodes. This document calls it by its
  working name `StoredCalculationResult` (header in `src/engine/`). The
  Phase 1 document fixes the exact type name, header name and accessors; use
  those and keep the wire layout below unchanged.
  Coordinator integration note: Phase 1 fixed these. The type is
  `StoredCalculationResult` in `src/engine/storedcalculationresult.h`, with
  public members `calculationId`, `resultVersion`, `detail` (kept separately,
  equal to `bundle.reason()`), `bundle`, `leaves` (`QList<GraphNode>`) and
  `inputFingerprint` (raw `QByteArray` of `InputFingerprintSize` = 32 bytes, not
  hex). Leaf kinds go through `storedLeafKindCode` / `storedLeafFromCode`.
- **Later phases extend this phase's manager code** (coordinator integration
  note; do not implement these here, but do not write comments that
  contradict them): Phase 3 adds stem reservations for sessions not saved yet
  (`reserveSessionFile`, a private `recordStem()` lookup used by every record
  method, and changes to `saveSession`, `removeSession`, `remapSessionId` and
  `reset`). Phase 4 references records from `index.json` (a per-session
  `"records"` stamp), makes `writeCalculationRecord` flush the index first in
  one case, adds the `calculationRecordsChanged` signal, and gives `reset`,
  `remapSessionId` and `removeSession` record-stamp state. Phase 2's
  statements "records are never referenced from index.json" and "no record
  method flushes the index" hold at this phase's commit only.
- **Blocks:** Phase 3 (store on publish / restore on load), and through it
  Phases 4 and 5.
- **Assumptions:**
  - The snapshot holds exactly the content that the overview's "Snapshot
    contract" lists, and nothing that belongs to an engine:
    1. the calculation id (`QString`; for a family instance this is the
       instance id and may contain `#`);
    2. the result version (`QString`, empty when the descriptor declares
       none);
    3. the reason / detail text (`QString`; for an `Ok` result it equals the
       bundle's `reason()`);
    4. the installed `CalculationResult` bundle;
    5. the dependency leaves: an ordered list of `(GraphNode::Kind, name)`
       entries, where the kind is one of `StoredAttribute`,
       `SourceMeasurement`, `SourceUnit` or `Preference`. The name is one
       string for an attribute or preference and two strings
       (sensor, name) for a source node, exactly like `GraphNode::a` and
       `GraphNode::b` (`src/engine/calctypes.h` 109-183);
    6. the input fingerprint: a SHA-256 value, held either as a
       `QByteArray` of the raw digest or as a hex `QString`.
  - The type is a plain copyable value: it can be default-constructed and
    filled field by field, so a decoder can build one without an engine.
  - `CalculationResult` (`src/engine/calculationresult.h`) keeps the public
    API it has at 8dc4e38: `setOutputs()`, `isAvailable()`,
    `attributeValue()`, `measurementValues()`, `measurementUnit()` and
    `reason()` to read a bundle, and `setAttribute()`, `setMeasurement()`,
    `setUnavailable()` and `setReason()` to build one. The codec needs
    nothing else, and no `friend` access.

## Tasks

### Task 2.1: Record value type, code stamps and file names

**Purpose:** Give the record and its file name one widget-free definition in
the core library that the logbook manager, Phase 3 and the tests all share.

**Files to create:**
- `src/calculationrecord.h`: the record type, its status enum, its
  constants, the stamp helpers and the file-name functions. Task 2.2 adds
  the codec to the same header.
- `src/calculationrecord.cpp`: their implementation.

**Files to modify:**
- `src/CMakeLists.txt`: add `calculationrecord.cpp calculationrecord.h` to
  `flysight_core`, next to `logbookmanager.cpp` (around line 272), with a
  one-line comment: "Stored requested-calculation results: record format and
  file names (the logbook manager does the I/O)".

**Technical Approach:**

Put everything in `namespace FlySight`. The file goes in `src/` (the
`flysight_core` library), not in `src/engine/`, for two reasons. The engine
layer must not depend on `src/calculations/`, which the stamp helpers need.
And the audit rule "no schema inference (conversion, engine)"
(`tests/audit/cleanup_audit.cmake` 204-205) bans `QDate`, `fileName` and
`filePath` under `src/engine/`, and this file must name `QMetaType` date
constants and file names.

Types and constants:

```cpp
/// One stored requested-calculation result, as a record file holds it.
struct CalculationRecord {
    int calculationCompatibility = 0;   // CalculationCompatibilityVersion at write time
    QString calculationEnvironment;     // calculationEnvironmentFingerprint() at write time
    StoredCalculationResult result;     // Phase 1's snapshot

    /// The snapshot with the CURRENT stamps (computed fresh from `registry`).
    static CalculationRecord stamped(const StoredCalculationResult &result,
                                     const CalculationRegistry &registry = CalculationRegistry::instance());
    /// Both stamps equal the current ones, computed fresh. Never LogbookManager::cacheEnvironment().
    bool stampsAreCurrent(const CalculationRegistry &registry = CalculationRegistry::instance()) const;
};

enum class CalculationRecordStatus {
    Ok,
    Missing,             // no record file (or the session is not in the logbook)
    Unreadable,          // the file exists but could not be opened or read
    NotARecord,          // does not start with the magic
    UnsupportedVersion,  // format version other than CalculationRecordFormatVersion
    Corrupt              // checksum, structure, trailing bytes, wrong calculation id
};

inline constexpr quint32 CalculationRecordFormatVersion = 1;
```

Declare the file extension and the magic in the `.cpp` file only, as
file-local constants. The literal `fvresult` then appears in exactly one
source file under `src/`, which Phase 5 can turn into an audit rule. Expose
the extension through `QString calculationRecordExtension()` (returns
`"fvresult"`, without the dot) for the logbook manager.

`stamped()` sets `calculationCompatibility = CalculationCompatibilityVersion`
and `calculationEnvironment = calculationEnvironmentFingerprint(registry)`
(`src/calculations/builtincalculations.h` 38, 62). `stampsAreCurrent()`
compares both fields with the same two expressions. Phase 3 calls these;
Phase 2 only tests them.

Coordinator integration note (audit): the rule "one authority: compatibility
marker" counts lines matching `CalculationCompatibilityVersion *=` in `src`
and must stay at 1. Write the comparison with the field on the left
(`calculationCompatibility == CalculationCompatibilityVersion`), never
`CalculationCompatibilityVersion == ...`, which the rule would count.

File names. A record of session file stem `S` (the `<uuid>` of
`sessions/<uuid>.csv`, i.e. `m_sessionIdToUuid[sessionId]`) and calculation
id `I` is named:

```
S + "." + encodeRecordFileId(I) + ".fvresult"
```

For example, `3f2c...-9a1e.builtin%2Efusion%2Efit.fvresult`.

`QString encodeRecordFileId(const QString &id)`: the UTF-8 bytes of `id`.
Each byte in `[a-z0-9_-]` is written as itself. Every other byte, including
`.`, `#`, `%`, upper-case letters, path and shell characters and non-ASCII
bytes, is written as `%` followed by two upper-case hex digits. So
`builtin.fusion.fit` becomes `builtin%2Efusion%2Efit`, `A` becomes `%41` and
`é` becomes `%C3%A9`.

This mapping has three properties:

1. **The encoded id contains no `.`.** The file name therefore splits
   unambiguously at its last two dots, even when the stem contains dots.
   (Identity stems from a user-placed `a.b.csv` can.)
2. **Only characters that are valid in a file name everywhere.** Windows,
   macOS and Linux all accept every character the encoding produces.
3. **Injective under case folding.** Two different ids never produce names
   that differ only in letter case. A literal letter is always lower case,
   an escape is always `%` plus upper-case hex, and the escapes sit at fixed
   positions. Two ids therefore cannot collide on a case-insensitive file
   system (Windows, default macOS).

`std::optional<QString> decodeRecordFileId(QStringView text)` is the
inverse. It returns `nullopt` unless `text` is non-empty, contains only
`[a-z0-9_-]` and `%XX` escapes (upper-case hex), decodes to valid UTF-8, and
re-encodes to exactly `text`. The last check makes the canonical form the
only accepted form.

`QString recordFileName(const QString &stem, const QString &calculationId)`
builds the name above.

`std::optional<std::pair<QString, QString>> parseRecordFileName(QStringView fileName)`
returns `(stem, calculationId)`. The name must end in `.fvresult`, compared
case-insensitively because `QDir` name filters match case-insensitively.
Strip that ending, then split the rest at its last `.`. The stem is
everything before that dot and must be non-empty. The id is
`decodeRecordFileId` of everything after it and must succeed. Anything else
gives `nullopt`.

Why the session scan can never take a record for a session: records never
end in `.csv`. `sessionFileStems()` (`src/logbookmanager.cpp` 656-667) lists
`*.csv` with `QDir::Files` only. Both the fallback filename scan
(`scanSessionFilenames`, 669) and orphan adoption (`initialize`, 210-224)
use that list. The QSaveFile temporary of a record, `<name>.fvresult.XXXXXX`,
ends neither in `.csv` nor in `.fvresult`. `mainwindow.cpp` 546-552 imports
only `*.csv` / `*.CSV`.

**Acceptance Criteria:**
- [ ] `encodeRecordFileId("builtin.fusion.fit") == "builtin%2Efusion%2Efit"`;
      `encodeRecordFileId("x#Key/é") == "x%23%4Bey%2F%C3%A9"`.
- [ ] `decodeRecordFileId` inverts `encodeRecordFileId` for every id in the
      test table, and returns `nullopt` for `""`, `"a.b"`, `"%2e"`
      (lower-case hex), `"A"` (literal upper case), `"%G1"`, `"%2"` and
      `"%FF"` (not valid UTF-8).
- [ ] `parseRecordFileName("a.b.x%2Ey.fvresult") == ("a.b", "x.y")`;
      `parseRecordFileName("s.x.FVRESULT") == ("s", "x")`; `nullopt` for
      `"x.fvresult"`, `".x.fvresult"`, `"s.x.csv"` and `"s.x.fvresult.Ab12Cd"`.
- [ ] For the ids `"A"` and `"a"`, the two record names compared with
      `Qt::CaseInsensitive` are different.
- [ ] `CalculationRecord::stamped(r).stampsAreCurrent()` is true.
      `stampsAreCurrent()` is false after `calculationCompatibility` is
      changed, after `calculationEnvironment` is changed, and after an extra
      calculation is registered on the global registry.
- [ ] `src/calculationrecord.*` includes no Qt Widgets header and nothing
      from `src/ui/`. The file compiles into `flysight_core`.

**Complexity:** M

---

### Task 2.2: Binary encoding and decoding

**Purpose:** Round-trip a record bit for bit, and refuse unknown versions and
damaged files, so a restored result cannot differ from the published one.

**Files to modify:**
- `src/calculationrecord.h` / `.cpp`: add the codec.

**Technical Approach:**

```cpp
/// Pure functions: no file I/O, no engine, no global state except the two
/// stamp helpers above (which the codec does not call). Safe on any thread.
std::optional<QByteArray> encodeCalculationRecord(const CalculationRecord &record, QString *error = nullptr);
CalculationRecordStatus decodeCalculationRecord(const QByteArray &bytes, CalculationRecord *out,
                                                QString *error = nullptr);
```

**Stream settings.** The bytes after the magic are written with one
`QDataStream` using:

- `setVersion(QDataStream::Qt_6_0)`, pinned (value 20). This is the oldest
  Qt 6 format. It is identical for Qt 6.0-6.5 and is what every later Qt 6
  reads and writes when asked.
- `setByteOrder(QDataStream::LittleEndian)`.
- `setFloatingPointPrecision(QDataStream::DoublePrecision)`.

The reader sets the same three values. A `double` is then its 8 IEEE-754
bytes, so `-0`, every NaN payload and sign, `±inf` and subnormals survive
unchanged. A `QString` is a `quint32` byte length (`0xFFFFFFFF` for a null
string) followed by UTF-16LE code units, so strings round-trip
byte-identically and null stays distinct from empty. Never rely on the
stream's default version or precision.

**Layout, format version 1** (offsets only for the fixed prefix):

| # | Field | Wire form |
|---|-------|-----------|
| 1 | magic | 8 raw bytes `46 56 52 45 53 55 4C 54` (`"FVRESULT"`), written with `writeRawData` |
| 2 | format version | `quint32` = 1 (bytes 8-11: `01 00 00 00`) |
| 3 | calculation compatibility | `qint32` |
| 4 | calculation environment | `QString` |
| 5 | calculation id | `QString` (non-empty) |
| 6 | result version | `QString` |
| 7 | reason / detail | `QString` |
| 8 | input fingerprint | `QByteArray`: the raw 32-byte SHA-256 digest (Phase 1's `StoredCalculationResult::inputFingerprint`, `InputFingerprintSize`); the decoder refuses any other length as `Corrupt` |
| 9 | leaf count | `quint32` |
| 9a | per leaf | `quint8` kind code from Phase 1's pinned helpers `storedLeafKindCode` / `storedLeafFromCode` (0 StoredAttribute, 1 SourceMeasurement, 2 SourceUnit, 3 Preference; an unknown code is `Corrupt`), `QString` first (`GraphNode::a`: key or sensor), `QString` second (`GraphNode::b`: measurement name; whatever the leaf holds, normally null, for attribute and preference) |
| 10 | output count | `quint32` |
| 10a | per output, in `setOutputs()` order | `quint8` key code (1 attribute, 2 measurement), `QString` first (attribute key or sensor), `QString` second (measurement name; `QString()` for an attribute), `bool` available (1 byte), then, only if available: attribute: `QVariant` value; measurement: `quint32` sample count, that many `double`s, `QString` unit |
| 11 | checksum | 32 raw bytes: SHA-256 (`QCryptographicHash::Sha256`) of every preceding byte, magic included |

- The leaf kind codes are Phase 1's pinned codes (coordinator integration
  note: one set of codes serves the fingerprint and the record); the output
  key codes are the record's own stable numbers. Neither is a
  `static_cast` of `GraphNode::Kind` or `DependencyKey::Type`, so
  reordering those enums cannot change the format.
- Leaves and outputs are written in the snapshot's order and read back in
  the same order. The decoder never sorts.
- The reason is written once. On decode it goes into the bundle
  (`setReason`) and into the snapshot's `detail` field (Phase 1 keeps one
  separately).
- Coordinator integration note (audit): never spell
  `GraphNode::Kind::SourceMeasurement` / `Kind::SourceUnit` in
  `calculationrecord.cpp` or `tests/tst_result_records.cpp`. The rule "source
  inputs: conversion layer only" allows that spelling only under
  `src/conversion`, `src/engine` and three named tests. Use
  `storedLeafKindCode` / `storedLeafFromCode` in the codec and the
  `GraphNode::sourceMeasurement(...)` factory functions in tests.
- The status is not stored. Only `Ok` results are recorded (overview, "What
  is stored"), so every record means `Ok`.
- Samples use the same bytes as `operator<<(QList<double>)` at `Qt_6_0`, a
  `quint32` count then the elements, but are written and read element by
  element. The decoder then checks the count against the remaining bytes
  before it allocates (see Gotchas).
- The attribute `QVariant` is written with `QDataStream`'s own
  `operator<<(QVariant)` at the pinned version: a `quint32` type id, a
  `qint8` null flag, then the value. The encoder accepts only these type
  ids, and the decoder refuses any other: `QString`, `Double`, `Float`,
  `Int`, `LongLong`, `Short`, `Long`, `Char`, `SChar`, `UInt`, `ULongLong`,
  `UShort`, `ULong`, `UChar`, `Bool` and `QByteArray`. That is the set
  `CsvFormat::formatAttributeValue` handles explicitly
  (`src/csvformat.cpp` 58-92), minus the date and time types, whose
  round-trip depends on the time zone database. No `Explicit` calculation
  produces those today. Widening the set is a format change: bump
  `CalculationRecordFormatVersion`.

**Encoding** (`encodeCalculationRecord`) fails with `nullopt`, writing
nothing and setting `*error`, when:
- the calculation id is empty (`"The record has no calculation id"`);
- an available attribute has a type outside the set above. The error names
  the output, for example
  `"Attribute '_X' has a type a record cannot hold (QVariantList)"`, using
  `QMetaType::name()`;
- a sample count is `>= 0xFFFFFFFE`;
- the stream reports anything but `QDataStream::Ok` at the end.

Build the bytes in a `QByteArray` through `QDataStream(&bytes, QIODevice::WriteOnly)`,
then append the SHA-256 of the whole array.

**Decoding** (`decodeCalculationRecord`) runs these checks in this order and
stops at the first failure:
1. Fewer than 8 bytes, or the first 8 are not the magic: `NotARecord`.
2. Fewer than 12 bytes: `Corrupt`. The `quint32` LE at offset 8 is not
   `CalculationRecordFormatVersion`: `UnsupportedVersion`. This includes 0,
   older and newer versions. There is no migration; the caller treats the
   record as stale. The version is checked before the checksum, so a future
   format may change everything after the version field.
3. Fewer than 12 + 32 bytes, or the SHA-256 of `bytes[0, n-32)` differs from
   the last 32 bytes: `Corrupt`.
4. Parse `bytes[12, n-32)` with the pinned stream settings, in the order of
   the table. Any of the following is `Corrupt`:
   - stream status not `Ok`;
   - a leaf or output count larger than the remaining bytes divided by the
     smallest possible entry (a leaf is at least 9 bytes: code plus two
     4-byte string lengths; an output at least 10);
   - a sample count larger than the remaining bytes / 8;
   - an unknown kind or key code;
   - an available attribute whose `QVariant` is invalid or of a type outside
     the set;
   - an available measurement with zero samples;
   - the same output key twice;
   - an empty calculation id;
   - bytes left over after the last output.
5. Rebuild the bundle through the public setters, in file order: an
   unavailable output uses `setUnavailable(key)` (it then reads
   `contains() && !isAvailable()`, as when published), an available
   attribute `setAttribute`, an available measurement
   `setMeasurement(sensor, name, samples, unit)`, and then `setReason`.
   Only when every check has passed does `*out` receive the record.
   Otherwise `*out` is untouched.

`*error` gets short fixed texts such as "not a calculation record", "format
version 3 is not supported", "checksum mismatch", "unknown leaf kind 9" and
"unexpected bytes after the last output". Tests match them.

**Size.** A record is `17 × 8 × N` bytes of samples for the fit's seventeen
channels over N fused samples (a subset of the IMU samples), plus the
diagnostics JSON (tens of kB in the goldens), plus under 2 kB of header,
names, leaves and checksum. A session file spends roughly 70-90 bytes of
text per IMU line, plus its GNSS lines. The record (136 bytes per fused
sample) is therefore of the same order as the session file, as spec §6
requires. No compression.

**Acceptance Criteria:**
- [ ] A record round-trips with every double bit-identical
      (`FlySightTest::sameBitsEverywhere`), including `0.0`, `-0.0`, a quiet
      NaN, a NaN with the sign bit and a non-default payload, `+inf`,
      `-inf`, `1e-320`, `DBL_MAX` and `0.1`. Units round-trip non-empty,
      empty `""` and null, with `isNull()` preserved. Attribute strings
      compare equal with `==` and in `.toUtf8()` bytes, including
      non-ASCII, a surrogate pair, `"\r\n"`, an embedded `QChar(0)`, `""`
      and a null `QString`. Attribute types `double` (including `-0.0` and
      NaN bits), `qlonglong`, `int` and `bool` keep their `typeId()`.
- [ ] Unavailable outputs (attribute and measurement, via `setUnavailable`)
      come back `contains() && !isAvailable()`. `setOutputs()` order,
      `reason()`, leaves (all four kinds, in order), fingerprint, result
      version (empty and non-empty), calculation id and both stamps are
      equal after the round trip.
- [ ] A rejection-shaped record (a diagnostics string attribute plus a
      reason, no measurements) and an empty bundle (no outputs, empty
      reason) both round-trip.
- [ ] Layout pinned: for the small record the test specifies,
      `encoded.first(n - 32)` equals a literal byte array written out by
      hand (or with an external tool) from the table above, and
      `encoded.last(32)` equals `QCryptographicHash::hash(encoded.first(n - 32), Sha256)`.
- [ ] Version refusal: bytes 8-11 patched to 0, 2 and `0xFFFFFFFF` give
      `UnsupportedVersion`, whatever the checksum.
- [ ] Corrupt input gives the stated status and leaves `*out` untouched:
      empty or 7 bytes (`NotARecord`); wrong magic (`NotARecord`); a valid
      record truncated at 11 bytes, at 40 bytes, in the middle of the
      samples and one byte short (`Corrupt`); one flipped bit in a sample
      (`Corrupt`); one extra trailing byte (`Corrupt`). Also `Corrupt` are
      hand-crafted payloads with a correct checksum that contain an unknown
      leaf kind, an unknown key code, a sample count of `0x7FFFFFFF` with
      8 bytes left (this must not allocate), an available measurement with
      0 samples, a `QVariantList` attribute, a duplicate output and extra
      bytes after the last output.
- [ ] `encodeCalculationRecord` refuses an empty calculation id and an
      available attribute of type `QVariantList` or `QPointF`. It returns
      `nullopt` and sets an error that names the output key.
- [ ] Size: a record of 17 measurements × 10 000 samples plus a 20 kB
      string attribute encodes to at most `17 × 8 × 10 000 + 20 000 × 2 + 4096` bytes.

**Complexity:** L

---

### Task 2.3: Record storage in the logbook manager

**Purpose:** Make `LogbookManager` the only code that writes, reads, lists
and deletes record files, with the same atomicity as a session save.

**Files to modify:**
- `src/logbookmanager.h`: a new public section of methods, a class-comment
  paragraph, and the private helpers.
- `src/logbookmanager.cpp`: their implementation.

**Technical Approach:**

Public API, in a new section `// --- Calculation records ---` after
`removeSession`:

```cpp
struct CalculationRecordRead {
    CalculationRecordStatus status = CalculationRecordStatus::Missing;
    std::optional<CalculationRecord> record;   // set only for Ok
    QString error;                             // empty for Ok and for a plain Missing
};

// Writes sessions/<stem>.<encoded id>.fvresult atomically (QSaveFile),
// replacing any previous record for (session, record.result's calculation id).
// On failure *error is set, nothing on disk has changed, and the previous
// record (if any) is intact.
bool writeCalculationRecord(const QString &sessionId, const CalculationRecord &record,
                            QString *error = nullptr);

// Reads and decodes one record. Missing when the session is unknown or has
// no such file; Unreadable when the file cannot be opened / read; the codec's
// status otherwise; Corrupt when the record names a different calculation id.
CalculationRecordRead readCalculationRecord(const QString &sessionId, const QString &calculationId) const;

// Calculation ids of the session's record files, sorted. Names only: no
// record is opened. Empty for an unknown session.
QStringList calculationRecordIds(const QString &sessionId) const;

// Deletes one record / every record of the session. True when no such file
// remains (absent counts as success); false for an unknown session or when a
// file could not be removed (warned).
bool removeCalculationRecord(const QString &sessionId, const QString &calculationId);
bool removeCalculationRecords(const QString &sessionId);
```

Private helpers, keyed by file stem, because the stray pass (Task 2.4) and
`removeSession` have a stem, not an id:

```cpp
QString calculationRecordPath(const QString &stem, const QString &calculationId) const;
QStringList calculationRecordFileNames() const;          // *.fvresult in sessions/, QDir::Files, sorted
QStringList calculationRecordIdsForStem(const QString &stem) const;
bool removeCalculationRecordsForStem(const QString &stem);
```

Implementation rules:

- **Session to stem.** Look up `m_sessionIdToUuid[sessionId]`, exactly as
  `loadSessionRaw` does (492-506). For an unknown id, `write` fails with
  `"not in the logbook index"` (the phrase `loadSessionRaw` uses), `read`
  returns `Missing` with that error, `list` returns empty and `remove*`
  returns false. Identity entries (stem == id) need no special case.
  Coordinator integration note: put this lookup in one private helper;
  Phase 3 turns it into `recordStem()`, which also consults stems reserved
  for sessions not saved yet.
- **Write.** Encode first. If encoding fails, return false with the codec's
  error before any file is opened; the previous record is untouched. Then
  follow `DataExporter::exportSession` (`src/dataexporter.cpp` 253-286)
  step by step: `QSaveFile` `open(WriteOnly)`, `write`, check `error()`,
  `cancelWriting()` on error, then `commit()`. Use the same error text shape
  as `writeError` (dataexporter.cpp 229): `"Couldn't write file '<path>': <errorString>"`.
  Leave `setDirectWriteFallback` at its default (false), so the write is
  always a temporary file plus rename. Warn once per failure with
  `qWarning("LogbookManager: calculation record %s of %s not written: %s", ...)`,
  like `saveSession`. Nothing is retried. The in-memory index is unaffected:
  records are not referenced from `index.json`, and no flush is needed.
  (Coordinator integration note: true at this phase's commit; Phase 4 adds
  the `"records"` stamp and a flush-before-write step to this function.)
- **Read.** If the file does not exist, return `Missing` with an empty
  error. If `QFile::open(ReadOnly)` fails, return `Unreadable` with the
  `errorString()`. Otherwise read all bytes and call
  `decodeCalculationRecord`. If the decoded
  `result.calculationId != calculationId`, return `Corrupt` with the error
  `"the record belongs to calculation '<id>'"`. A read never deletes
  anything; deciding what is stale is Phase 3's job.
- **List.** `calculationRecordFileNames()`, then `parseRecordFileName`, then
  keep the entries whose stem equals the session's stem exactly. Never
  build a `QDir` wildcard from a stem: an identity stem may contain `[`,
  `*` or `?`. With stems `a` and `a.b` both present, listing `a` does not
  return `a.b`'s records, because the split is at the last dot.
- **Remove.** `QFile::remove` on each path. A path that does not exist
  counts as removed. On failure, warn with the path and return false.
- **Class comment.** Extend the comment at the top of `logbookmanager.h`
  (19-49) with one paragraph: sessions/ also holds
  `<stem>.<encoded calculation id>.fvresult` record files, one per (session,
  requested calculation); they are never referenced from index.json, never
  listed as sessions, written only through `writeCalculationRecord`
  (QSaveFile), and removed with their session, by the stray pass of
  `initialize()`, and by explicit removal. Also change "one CSV per session
  under sessions/" accordingly. Do not describe Phase 3's validity rules
  here beyond "the caller decides validity". (Coordinator integration note:
  Phase 3 adds a sentence on reserved stems to this paragraph, and Phase 4
  changes "never referenced from index.json" to "referenced only by the
  per-session record stamp".)
- **Threading.** The main thread, like every other `LogbookManager` method.
  The codec is pure: a later phase may encode elsewhere and add a
  bytes-taking overload if its measurement shows a need. Phase 2 does not
  add one.

**Acceptance Criteria:**
- [ ] After `saveSession(s1)` and `writeCalculationRecord("s1", r)` for id
      `builtin.fusion.fit`, `sessions/` holds exactly `<stem>.csv` and
      `<stem>.builtin%2Efusion%2Efit.fvresult`, where `<stem>` is the csv's
      base name. There is no temporary file.
- [ ] `readCalculationRecord` returns `Ok` and a record equal (bit-exact,
      Task 2.2 criteria) to `r`. A second write with different samples
      replaces it: the read returns the second. `calculationRecordIds("s1")`
      is `{"builtin.fusion.fit"}`.
- [ ] Read statuses: unknown session gives `Missing` with
      `"not in the logbook index"`; a known session without a record gives
      `Missing` with an empty error; `"garbage"` bytes give `NotARecord`; a
      patched version gives `UnsupportedVersion`; a valid record of id `x`
      copied to the file name of id `y` gives `Corrupt`.
- [ ] Encode failure: after a successful write, a write whose record holds
      a `QVariantList` attribute returns false, sets a non-empty error that
      names the attribute, and leaves the file's bytes identical.
- [ ] I/O failure (portable, no permission bits): with an empty directory
      created at the record's exact path, the write returns false, sets an
      error that contains the path, and leaves the sessions directory
      listing unchanged: the directory is still there and no temporary
      file was added. After the directory is removed, the same write
      succeeds.
- [ ] Unknown session: the write returns false with
      `"not in the logbook index"` and creates no file.
- [ ] `removeCalculationRecord` removes one of two records and leaves the
      other and the csv bytes unchanged. Calling it again returns true.
      `removeCalculationRecords` removes the rest. Both return false for an
      unknown session.
- [ ] Records written under an identity entry are read and listed under the
      real id after `remapSessionId(stem, realId)`, and are not stray at the
      next `initialize()`.
- [ ] `saveSession` of a session that has a record rewrites the same csv
      bytes as before the record existed, and leaves the record's bytes
      unchanged.

**Complexity:** M

---

### Task 2.4: Removal with the session, and stray removal at the scan

**Purpose:** Delete records when their session is deleted, and delete
records whose session file is gone when the logbook starts. Never touch a
session file while doing either.

**Files to modify:**
- `src/logbookmanager.cpp`: `removeSession`, `initialize`, the new private
  `removeStrayCalculationRecords()`, and short comments at `remapSessionId`
  and `saveSession`.
- `src/logbookmanager.h`: declare `removeStrayCalculationRecords()`; update
  the `removeSession` and `initialize` comments.

**Technical Approach:**

- **`removeSession`** (719-742). Keep the csv removal first and unchanged:
  if it fails, return false with every record still in place, and the
  session and its records stay consistent. Once the csv is removed, and
  before the map entry is erased (the stem is still needed), call
  `removeCalculationRecordsForStem(uuid)`. If that fails, warn and still
  return true: the session is gone, and the next start's stray pass removes
  the leftovers. Update the header comment: "Deletes the .csv file and the
  calculation records of the given SESSION_ID".
  `SessionModel::removeSessions` (sessionmodel.cpp 916) and the delete path
  in `MainWindow` (mainwindow.cpp 820-827) need no change.
- **`removeStrayCalculationRecords()`** (private; returns the number of
  files removed). Take `sessionFileStems()` (the `*.csv` files on disk)
  into a `QSet`. For every name from `calculationRecordFileNames()`: if
  `parseRecordFileName` fails, or its stem is not in the set, remove the
  file. If removal fails, warn and go on. Only names are looked at: no
  record and no session file is ever opened, and only `*.fvresult` files
  are deleted.
- **`initialize()`** (142-250). Call `removeStrayCalculationRecords()` as
  its first statement, before `index.json` is read. The rule depends only
  on the files on disk, so it runs identically in all three branches
  (extended index, legacy flat index, fallback filename scan), and the
  early `return`s need no restructuring. Its meaning is "a record whose
  session file does not exist" (overview, "When a record is deleted"). A
  record whose csv exists but that the index does not know stays; orphan
  adoption will make that csv a session.
- **`remapSessionId`** (681-713). Add no code, only a one-line comment:
  "File stems never change (the uuid moves to the new id), so calculation
  records need no move." This settles the overview's question: no code in
  `src/` renames a session file. `saveSession` reuses
  `m_sessionIdToUuid[sessionId]` (618-624), `remapSessionId` keeps the
  uuid, and no other code writes into `sessions/`.
- **`reset()`** (256). Nothing to add: records have no in-memory state.
  (Coordinator integration note: in this phase. Phase 3 adds reservations
  to `reset`, `removeSession` and `remapSessionId`, and Phase 4 adds its
  record-stamp maps to them.)
- **`saveSession`**. Add nothing. Records are never read or written during a
  save, so the session file's bytes cannot depend on them (spec §7).

**Acceptance Criteria:**
- [ ] With records for sessions `s1` and `s2`, `removeSession("s1")` removes
      `s1`'s csv and all its records. `s2`'s csv and records keep their
      bytes.
- [ ] With identity sessions from `a.csv` and `a.b.csv` (fallback scan, no
      index), each with one record, `removeSession("a")` removes only `a`'s
      record.
- [ ] If the csv cannot be removed (`removeSession` of an entry whose csv
      was already deleted by the test), the result is false and the
      session's records are still present.
- [ ] Stray pass, for each of the three `initialize()` branches (data rows:
      extended index, legacy flat index, no `index.json`), with these files
      in `sessions/`: two sessions with one record each; a record whose
      stem has no csv; `junk.fvresult`; `notes.txt`; `x.csv.AbC123` (a
      leftover session-save temporary); and
      `<stem>.builtin%2Efusion%2Efit.fvresult.Q1w2E3` (a leftover record
      temporary). After `reopenLogbook()` and `initialize()`, exactly the
      stray record and `junk.fvresult` are gone. Every other file keeps its
      bytes, and every csv's bytes are unchanged.
- [ ] In the same test, no record ever shows up as a session:
      `cachedColumnValues(...)` keys (extended and legacy branches) and
      `scannedUuids()` (fallback branch) are exactly the session ids /
      stems of the csv files. `sessionCsvFiles()` lists only the csv files.
- [ ] Orphan adoption with a record present (csv committed, index not
      flushed, one record for it): after restart the orphan is adopted, its
      record is kept, and no identity entry is created for the record file.

**Complexity:** M

---

### Task 2.5: Test target, probes and codec tests

**Purpose:** A core test executable that pins the format and the name
mapping, and probes of the record files that Phase 3 can reuse.

**Files to create:**
- `tests/tst_result_records.cpp`: one `QObject` test class
  (`ResultRecordsTest`), `FLYSIGHT_TEST_MAIN(ResultRecordsTest)` and
  `#include "tst_result_records.moc"`.

**Files to modify:**
- `tests/CMakeLists.txt`: in the persistence block (175-178), add
  `flysight_add_test(tst_result_records SOURCES tst_result_records.cpp)`,
  and extend that block's comment with "the stored calculation records".
- `tests/support/logbookprobe.h` / `.cpp`, under "session files":
  - `QStringList calculationRecordFiles();`: file names (not paths) of the
    `*.fvresult` files in the sessions directory, sorted. Mirrors
    `sessionCsvFiles()`.
  - `QString sessionFileStem(const QString &sessionId);`: the base name of
    `sessionFilePath(sessionId)`, empty when there is none.
- `tests/README.md`: one catalogue row for `tst_result_records` next to
  `tst_logbook_index` (the persistence table). Leave sections 8-10 and the
  acceptance matrix to Phase 5.

**Technical Approach:**

- Follow `tests/tst_logbook_index.cpp`: `initTestCase` calls
  `registerBuiltIns()` (the stamp helpers need the real registry).
  `init()` calls `useFreshLogbook()`, `resetPreferencesToDefaults()` and
  `LogbookManager::instance().initialize()`. `cleanup()` unregisters any
  extra id a test registered (tst_logbook_index.cpp 118-121 and its
  `kExtraId`).
- Build snapshots directly as Phase 1 values (no engine). Write a file-local
  builder `StoredCalculationResult sampleSnapshot(...)` and a comparison
  helper `QString recordDifference(const CalculationRecord &a, const CalculationRecord &b)`,
  which returns empty when equal and otherwise says what differs. Per
  tests/README §8, helpers return a `QString` instead of calling `QVERIFY`.
  It compares every field: doubles with `sameBits`, strings with `==` and
  `isNull()`, `QVariant`s by `typeId()` and then by value (by bits for
  `double`/`float`). Include `testutil.h`.
- Expected values are literals, never produced by the code under test. The
  pinned-layout test spells out its prefix bytes as a literal
  `QByteArray::fromHex(...)`, built from the Task 2.2 table. Suggested
  record: stamps 7 / `"e"`; id `"x.y"`; result version `"v1"`; empty
  reason; fingerprint `QByteArray(32, '\xAB')` (32 bytes: the decoder
  refuses any other length, so a shorter literal could not round-trip);
  one `GraphNode::sourceMeasurement("S","t")` leaf; one available measurement `("S","m")` with samples
  `{1.0, -0.0}` and unit `"u"`; one unavailable attribute `"a"`. Compute
  the trailer with `QCryptographicHash` in the test. Crafted corrupt
  payloads are built by a small writer in the test with its own
  `QDataStream` (same pinned settings), followed by a SHA-256 appended in
  the test.
- Codec test functions (Task 2.1 and 2.2 criteria):
  `fileIdEncoding_data/fileIdEncoding`, `fileNameParsing_data/fileNameParsing`,
  `fileNamesDifferIgnoringCase`, `stampsAreCurrent`, `roundTripIsBitExact`,
  `unavailableAndEmptyOutputs`, `rejectionShapedRecord`, `layoutIsPinned`,
  `futureVersionIsRefused_data/futureVersionIsRefused`,
  `corruptInputIsRefused_data/corruptInputIsRefused`,
  `encoderRefusesUnsupportedAttribute`, `sizeIsOrderOfSamples`.

**Acceptance Criteria:**
- [ ] `tst_result_records` builds in `build-phase1` and is registered with
      label `core`.
- [ ] Every Task 2.1 and 2.2 criterion is covered by a named test function
      above, and all of them pass.
- [ ] `calculationRecordFiles()` and `sessionFileStem()` exist in
      `logbookprobe.h`, with a one-line doc comment each.

**Complexity:** L

---

### Task 2.6: Logbook storage tests

**Purpose:** Prove the Task 2.3 and 2.4 behaviour on a real temporary
logbook, on every platform CI builds.

**Files to modify:**
- `tests/tst_result_records.cpp`: add the storage test functions.

**Technical Approach:**

- Sessions come from a local `makeSession(id)` like tst_logbook_index.cpp
  41-54, saved with `LogbookManager::saveSession`. Paths come from
  `TestEnvironment::sessionsDir()`, `sessionFilePath` and `sessionFileStem`.
  Expected record names are literals such as
  `stem + ".builtin%2Efusion%2Efit.fvresult"`.
- Restarts use `reopenLogbook()` + `initialize()`. The legacy flat index is
  written with `writeIndex` as in `legacyFlatIndexStartsAsStubs` (633-679),
  and the no-index branch by deleting `index.json`.
- The I/O-failure test uses `QDir().mkdir(<record path>)`. It must not use
  `QFile::setPermissions` or a read-only directory: Windows ignores the
  read-only attribute on directories. The encode-failure test uses a
  `QVariantList` attribute. Together they cover "a failed write leaves the
  previous file intact". The first shows that nothing is created or
  replaced when `QSaveFile` cannot open. The second shows that a refused
  write never opens the file. Between a successful open and `commit()`,
  atomicity is `QSaveFile`'s contract (temporary file plus rename).
- The expected warnings (a failed write, a failed removal) are consumed with
  `QTest::ignoreMessage` or `WarningCapture`, as tst_logbook_index.cpp 407
  does.
- Test functions: `writeReadReplace`, `readStatuses`,
  `writeFailureRefusedEncoding`, `writeFailureDirectoryAtPath`,
  `writeForUnknownSession`, `removeOneAndAll`, `removeSessionDeletesRecords`,
  `removeSessionDottedStems`, `failedSessionRemovalKeepsRecords`,
  `strayRecordsRemovedAtScan_data/strayRecordsRemovedAtScan`,
  `orphanAdoptionKeepsRecord`, `recordsFollowRemap`,
  `saveSessionIgnoresRecords`.

**Acceptance Criteria:**
- [ ] Every Task 2.3 and 2.4 criterion is covered by a named test function
      above, and all of them pass.
- [ ] No test depends on file permission bits, on the platform's case
      sensitivity, or on directory iteration order (listings are sorted).
- [ ] The existing `tst_logbook_index`, `tst_persistence_roundtrip` and
      `tst_column_cache` pass unchanged.

**Complexity:** L

## Testing Requirements

### Unit Tests
- New `tst_result_records` (core label): the codec (name mapping, bit-exact
  round trip with `-0`, NaN payloads, `±inf`, subnormals, empty and
  unavailable outputs, byte-identical strings, pinned layout, version and
  corruption refusal, encoder refusal, size) and logbook storage (write,
  read, replace, list, remove, write failures, `removeSession`, stray pass
  in all three `initialize()` branches, orphan adoption, remap, save
  independence).
- No existing test changes. `tst_logbook_index`'s `orphanSessionFileIsAdopted`,
  `identityEntries` and `legacyFlatIndexStartsAsStubs` must keep passing
  as they are, with no records present. That shows the stray pass is inert
  on a logbook without records.

### Integration Tests
- None across `SessionModel` in this phase (Phase 3). The storage tests use
  the real `LogbookManager` singleton on the per-test temporary logbook,
  which is the level Phase 3 builds on.
- The audit: `ctest ... -L audit` must stay green. The new source is not in
  `src/engine/`, so it is outside the "no schema inference (conversion,
  engine)" ban on `QDate` / `fileName` / `filePath`. It does not use
  `FloatingPointShortest` (`csvformat.cpp` only) or `<charconv>`, does not
  call `exportSession(`, and creates no thread, lock or atomic.

### Manual Verification
Build and test in `build-phase1/` only. **Never build `build/`**: it would
overwrite the Boost-enabled solver install.

1. `cmake --build build-phase1 --config Release`
2. `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure -R "tst_result_records|tst_logbook_index|tst_persistence_roundtrip|tst_column_cache"`
3. `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure -L audit`
4. The full suite: `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`.
   Fusion tests may take up to 600 s each.
5. Optional hex check: run `tst_result_records layoutIsPinned` by hand (PATH
   as in tests/README §2-4) and confirm that the literal in the test matches
   the table in this document.

## Notes for Implementer

### Gotchas
- **Do not use `operator>>(QList<double>)` to read samples.** Qt reserves
  the declared count before reading, so a crafted count of `0x7FFFFFFF`
  would try to allocate 16 GB. Read the `quint32` count, check
  `count <= remaining / 8` (`stream.device()->bytesAvailable()`), then
  resize and read element by element. Leaf and output counts get the same
  check. Strings and byte arrays are safe: Qt reads them in 1 MB steps and
  fails with `ReadPastEnd`.
- **Set all three stream settings on both sides.** A `double` written at
  `SinglePrecision` loses bits. A reader at a different version reads
  `QVariant` differently.
- **`QVariant` float:** with `DoublePrecision` a `Float` variant is written
  as 8 bytes and read back as the same `float`. Its type id is preserved.
  This is expected.
- **Null vs empty strings** are distinct on the wire and must stay distinct.
  `CalculationResult::setMeasurement` defaults `unit` to a null `QString`.
- **Unavailable outputs carry no payload.** Rebuild them with
  `setUnavailable`, not `setAttribute(key, QVariant())`. Both give
  "contains, not available", but `setUnavailable` states the intent and
  does not depend on the normalization rule.
- **Case-insensitive name filters.** `QDir`'s default filter is
  case-insensitive, so `*.fvresult` also matches `X.FVRESULT`.
  `parseRecordFileName` handles that ending. The stray pass removes such a
  file unless it parses and its stem has a csv.
- **Identity stems are arbitrary file names.** Never interpolate a stem into
  a `QDir` wildcard. Always list `*.fvresult` and compare parsed stems.
- **The stray pass runs before anything else in `initialize()`**, including
  before the logbook folder is known to exist. `sessionsDirectory()`
  already creates it, so an empty logbook is fine.
- **Warnings in tests.** `writeCalculationRecord` and the removal helpers
  warn on failure. Tests that provoke a failure must expect the warning,
  or QtTest's default handler prints noise.
- **Do not flush the index** from any record method. Records are not in
  `index.json`, and a flush has save-ordering meaning (h 34-49).
  (Coordinator integration note: superseded in Phase 4, whose
  `writeCalculationRecord` flushes the index first when the index on disk
  holds a value depending on that record. Phase 2 adds no flush.)

### Decisions Made
- **Codec location:** `src/calculationrecord.{h,cpp}` in `flysight_core`,
  not `src/engine/`. The stamps come from `src/calculations/`, which the
  engine must not depend on, and the engine directory is audited against
  `QDate` / `fileName` / `filePath`. It is pure (no I/O, no engine state),
  so Phase 3 may encode on any thread.
- **File name:** `<stem>.<percent-encoded id>.fvresult`. Every byte outside
  `[a-z0-9_-]` is encoded, the dot included, so the name parses at its last
  dot even for stems with dots, and ids that differ only in case cannot
  collide on case-insensitive file systems. The fusion record is
  `<uuid>.builtin%2Efusion%2Efit.fvresult`.
- **Records keyed by file stem, not SESSION_ID.** Stems never change, so a
  remap (identity stub to real id) carries the records for free. No code
  renames a session file.
- **Binary layout:** magic `"FVRESULT"`, then a `quint32` format version 1,
  then everything else in `QDataStream` `Qt_6_0` / little-endian / double
  precision, then a SHA-256 trailer. The trailer is an addition to the
  overview's list. It turns silent bit rot or a partial copy into `Corrupt`
  (stale, deleted, recomputed on request) instead of a wrong restored
  channel. It costs about 10 ms per few MB.
- **Attribute values** use Qt's own `QVariant` stream form, restricted to
  the non-date types `CsvFormat` handles. Anything else is refused at
  encode, so no record is written and the in-memory result is untouched.
- **Version policy:** any version other than 1 is `UnsupportedVersion`
  (stale). There is no migration, as spec §6 allows.
- **Read never deletes.** Deciding what is stale, and deleting it, belongs
  to Phase 3. `readCalculationRecord` only reports a status.
- **Stray removal** runs at the start of `initialize()`, using only the csv
  files on disk, so it is the same in every index branch. It looks at names
  only.
- **`removeSession` order:** the csv first, then the records. A failed
  session delete leaves everything as it was. A failed record delete after
  it leaves a stray that the next start removes.
- **Write-failure tests without a product test seam:** a directory at the
  record path (I/O refusal, portable) plus an encoder refusal (the previous
  record stays byte-identical). No permission bits. Phase 3's spec §8 test
  ("a record whose write fails ... the previous record intact") can use the
  same two techniques. For the previous-record case it needs an Explicit
  test calculation whose second publish carries an attribute the record
  cannot hold.
- **tests/README.md:** add only the catalogue row now, so the catalogue
  stays true at this phase's commit. Everything else there is Phase 5's.

### Open Questions
- None blocking. Phase 1 fixes the snapshot's type name, accessors and the
  fingerprint's C++ type. The wire form above is fixed either way: row 8
  covers both fingerprint representations.

### Deviations / questions for the coordinator
- **Refinement of the name form.** The overview writes
  `<session file stem>.<calculation id>.<extension>`. This document encodes
  the id (`.` becomes `%2E`), so the fusion record reads
  `<uuid>.builtin%2Efusion%2Efit.fvresult`, not
  `<uuid>.builtin.fusion.fit.fvresult`. This is the "name mapping for ids
  containing characters unsafe in file names" the overview asks Phase 2 to
  document. The dot is encoded as well because a raw dot would make names
  ambiguous for stems that contain dots. It is flagged here only because
  the literal differs from the overview's example.
- **Addition: SHA-256 trailer** (see Decisions Made). It does not conflict
  with any constraint.
- **Phase 3 hand-off.** Phase 3 calls `CalculationRecord::stamped()` before
  `writeCalculationRecord`, and `stampsAreCurrent()` after
  `readCalculationRecord`. It treats every status other than `Ok` and
  `Missing` as stale, and deletes with `removeCalculationRecord`. It can
  read a known id directly (for example each registered Explicit id)
  instead of listing. `calculationRecordIds` exists for records of ids that
  are no longer registered.
- **Phase 4 hand-off.** Every record mutation goes through four places:
  `writeCalculationRecord`, `removeCalculationRecord(s)`, `removeSession`
  and `removeStrayCalculationRecords`. A record-set token or signal for the
  column-cache stamp can hook those without touching callers. Phase 2 adds
  none.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria
2. All tests pass (full `ctest` in `build-phase1`, audit included)
3. Code follows patterns established in reference files (`QSaveFile` write
   as in `dataexporter.cpp`, logbook tests as in `tst_logbook_index.cpp`)
4. No TODOs or placeholder code remains
