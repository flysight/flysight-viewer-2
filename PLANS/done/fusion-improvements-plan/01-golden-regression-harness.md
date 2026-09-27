# Phase 1: Golden regression harness

## Overview

This phase brings the golden capture into the repository. A non-test executable, `fusion_golden_capture`, built in the `FLYSIGHT_BUILD_FUSION_TESTS` block, runs the product kernel on the twelve synthetic fixtures and writes `<fixture>.json`, `<fixture>.channels.txt` and `capture.json` into `tests/data/fusion/` in exactly the formats `tests/fusion/fusiongolden.cpp` reads. Its first run against the unchanged kernel must reproduce the committed goldens bit for bit in every number and every string the tests compare literally (the only lines that change are the eighteen progress texts that the branch worded differently and that the test translated until now; see Decisions Made). That run is the proof that the tool captures what the tests compare, and the only time the tool's output is compared with the branch's.

With the tool in place the branch-parity provenance is retired: `capture.json` records this repository's revision, the tool's build configuration and the file hashes instead of the branch revision, the cross-check and the `batchfusion.cpp` command line; `tst_fusion_parity` becomes `tst_fusion_golden`, a golden regression test against the product kernel; `tests/README.md` section 11 describes the in-tree procedure instead of the out-of-tree harness. From here on every numerical phase (3, 4, 5, 6) ends by running the re-capture recipe of Task 1.6. No kernel source changes: `src/fusion/` is untouched.

## Dependencies

- **Depends on:** None — can begin immediately
- **Blocks:** Phases 3 and 4 (and through them 5, 6, 7)
- **Assumptions:**
  - The working tree is `master` (or the plan branch created from it) with the kernel at `1f3139a` or later, the goldens under `tests/data/fusion/` as captured from the branch on 2026-09-21, and `capture.json` naming `cl_version` `19.44.35220`.
  - The configured test build on Michael's machine is `build-phase1/` (`FLYSIGHT_BUILD_TESTS=ON`, `FLYSIGHT_BUILD_FUSION_TESTS=ON`, `FLYSIGHT_BUILD_THIRD_PARTY=OFF`, GTSAM from `build-solver-deps/GTSAM-install`, oneTBB from `build-solver-deps/oneTBB-install`, `FLYSIGHT_FUSION_EXACT_TESTS=AUTO`). Build with `cmake --build build-phase1 --config Release`; the test executables are in `build-phase1/FlySightViewer-build/Release/`. Never rebuild `build/`.
  - The compiler is 64-bit MSVC 19.44 (the capture configuration), so the exact tests are registered and `ctest -C Release -L exact` is meaningful.

## Tasks

### Task 1.1: Golden writers next to the golden readers

**Purpose:** The capture tool must write exactly what the tests read; putting the hex-bits channel writer beside `fromHexBits()` and the trace serializer in one shared header means the two formats cannot drift.

**Files to modify:**
- `tests/fusion/fusiongolden.h` — declare the channel writer; reword the branch provenance in the comments (see below)
- `tests/fusion/fusiongolden.cpp` — add `toHexBits()` next to `fromHexBits()` and the channel writer next to `loadChannels()`, sharing the header and column-line constants
- `tests/tst_fusion_kernel.cpp` — remove the file-local `traceJson()` (lines 130-143) and include the new header instead (its only user is `traceJson()`, so `toJsonArray()` at lines 125-128 moves with it)

**Files to create:**
- `tests/fusion/fusiontrace.h` — header-only `inline QJsonObject traceJson(const FlySight::Fusion::Detail::PipelineTrace &)`, moved verbatim from `tests/tst_fusion_kernel.cpp` lines 130-143 (with its `toJsonArray(const Vector3 &)` helper, lines 125-128, whose only user it is)

**Technical Approach:**

`tests/fusion/fusiongolden.cpp`:
- Add `QByteArray toHexBits(double value)`: the sixteen upper-case hexadecimal digits of the IEEE-754 bit pattern (`%016llX` of the `quint64` obtained with `std::memcpy`), exactly the inverse of `fromHexBits()` (lines 38-47). Keep both in the same anonymous namespace, one after the other.
- Add `QByteArray fusionChannelsText(const FlySight::Fusion::Result &result)` (declared in the header, next to `fusionChannelNames()` / `fusionChannel()`): line 1 `kChannelsHeader`, line 2 `"# columns: " + fusionChannelNames().join(' ')`, then one line per output sample with the seventeen columns in `fusionChannelNames()` order, taken through `fusionChannel(result, name)`, space-separated, each `toHexBits()`, LF line endings, a final LF after the last row, nothing else. Build the column line from the same expression `loadChannels()` uses for its check (line 61), so the two cannot differ. The branch harness's `channelsText()` in `tests/README.md` lines 1300-1329 (`hexBits()` and `channelsText()`) is the model for the loop; the output must be byte-identical to what it wrote.
- Do not change `loadChannels()`, `compareSamples()`, `compareJson()`, the tolerance constants or `exactParityRequested()`.

`tests/fusion/fusiontrace.h`:
- Includes `<QJsonArray>`, `<QJsonObject>`, `"fusion/fusionpipeline.h"`; namespace `FlySightTest`; one `inline` function `traceJson(const FlySight::Fusion::Detail::PipelineTrace &trace)` returning the object with keys `method`, `interval_s`, `anchor_time_s`, `gyro_bias_rad_s`, `start_quaternion_xyzw`, `converged`, `history` (rows `[outer, iteration, before, after]`) exactly as `tst_fusion_kernel.cpp` lines 130-143 build it today. Do not `#include <gtsam/...>` in this header: it reaches `gtsam::Rot3` through `fusion/fusionpipeline.h`, so the cleanup audit's `solver-confinement` regex needs no entry for it.
- A one-paragraph comment: this is the one authority for the golden `trace` object; `fusion_golden_capture` writes it and `tst_fusion_kernel::fitTraceMatchesGolden` reads it back; a later phase that changes `PipelineTrace` (Phase 5) changes this function and re-captures.
- It is header-only because `flysight_fusion_test_support` must stay free of GTSAM headers (it is linked by `tst_fusion_golden`, which is a "reacher", not a "namer", in `flysight_assert_solver_confinement()`); the two targets that include it (`tst_fusion_kernel`, `fusion_golden_capture`) both link `gtsam`. List it in both targets' `SOURCES` in `tests/CMakeLists.txt` for IDE visibility.

`tests/fusion/fusiongolden.h` comment rewording (prose only; no identifier changes):
- `FusionGolden` doc: "What the product kernel produced for one fixture when the goldens were last captured by `fusion_golden_capture` (tests/data/fusion/, see tests/README.md section 11)"; the field comments "the reference's diagnostics object" / "the reference's progress texts" become "the kernel's ...".
- `exactParityRequested()` doc: "This is the mode that proves a rebuild on the capture configuration is bit-identical to the capture" instead of "decides parity". Keep the identifier names `exactParityRequested`, `ParityStatistics` and `ParityModeOverride`: they describe bit-exact comparison with the golden and are used by five tests; renaming them is churn without value (Decisions Made).

**Acceptance Criteria:**
- [ ] `fusiongolden.h` declares `fusionChannelsText()`; `fusiongolden.cpp` defines it and `toHexBits()` immediately after `fromHexBits()`
- [ ] `tests/fusion/fusiontrace.h` exists, is header-only, contains no `#include <gtsam/` line, and `tst_fusion_kernel.cpp` no longer defines `traceJson()` itself
- [ ] `tst_fusion_golden` (Task 1.4) has a test `channelsWriterIsTheInverseOfTheLoader`: for each of the three success fixtures, a `Fusion::Result` filled from the loaded golden's channels, passed through `fusionChannelsText()`, equals the bytes of `<fixture>.channels.txt` with `\r\n` normalized to `\n`; and `toHexBits()` round-trips `fromHexBits()` for `0.0`, `-0.0`, a NaN, `std::numeric_limits<double>::denorm_min()` and `1700000000.037` (compare bit patterns, not values)
- [ ] No comment in `tests/fusion/fusiongolden.h`, `fusiongolden.cpp`, `fusionfixtures.h` or `fusiontrace.h` presents `sensor-fusion-clean-port` as the source of the goldens

**Complexity:** M

---

### Task 1.2: The capture tool `fusion_golden_capture`

**Purpose:** One command captures the goldens from the product kernel, deterministically, in the committed formats, with provenance.

**Files to modify:**
- `tests/CMakeLists.txt` — add the executable inside the `FLYSIGHT_BUILD_FUSION_TESTS` block, after `flysight_fusion_test_support` (lines 308-316) and next to `solver_deploy_probe` (lines 436-443)
- `cmake/SolverDependencies.cmake` — add `fusion_golden_capture` to `_FLYSIGHT_GTSAM_NAMERS` (line 523-524)
- `tests/audit/cleanup_audit.cmake` — add `fusion_golden_capture\.cpp` to the allowed-file regex of `expect_only("GTSAM headers: kernel and its tests only" ...)` (line 315-317)

**Files to create:**
- `tests/fusion_golden_capture.cpp` — the tool

**Technical Approach:**

*CMake.* Follow the `solver_deploy_probe` pattern (a plain `add_executable` inside the block, no `add_test`, no install rule) plus what a fit needs:

```cmake
add_executable(fusion_golden_capture fusion_golden_capture.cpp fusion/fusiontrace.h)
target_link_libraries(fusion_golden_capture PRIVATE flysight_fusion_test_support gtsam)
flysight_solver_stack(fusion_golden_capture)          # fits run on the main thread
if(MSVC)
  target_compile_options(fusion_golden_capture PRIVATE /bigobj)
endif()
target_compile_definitions(fusion_golden_capture PRIVATE
  FLYSIGHT_CAPTURE_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
  FLYSIGHT_CAPTURE_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
  FLYSIGHT_CAPTURE_GENERATOR="${CMAKE_GENERATOR}"
  FLYSIGHT_CAPTURE_PLATFORM="${CMAKE_GENERATOR_PLATFORM}"
  FLYSIGHT_CAPTURE_CONFIGURATION="$<CONFIG>"
  FLYSIGHT_CAPTURE_CMAKE_VERSION="${CMAKE_VERSION}"
  FLYSIGHT_CAPTURE_GTSAM_DIR="${GTSAM_DIR}"
  FLYSIGHT_FUSION_FIXTURE_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}/fusion")
```

`FLYSIGHT_FUSION_GOLDEN_DIR` (the source `tests/data/fusion`) reaches the tool through the support library's PUBLIC definition (lines 313-314). No `-ffp-contract=off`: the tool's own translation unit does no floating-point arithmetic (the fixtures are generated inside the support library and the fit inside the kernel). Update the block comment at lines 436-438 so it introduces both non-test executables. The tool needs no `flysight_solver_test_environment()` (that is for CTest tests); it is run by hand with the DLL directories on `PATH` (Task 1.6).

Add `fusion_golden_capture` to `_FLYSIGHT_GTSAM_NAMERS` in `cmake/SolverDependencies.cmake`; without it `flysight_assert_solver_confinement()` fails the configure. Add the source to the audit regex; the tool includes `<gtsam/config.h>` directly for `GTSAM_VERSION_STRING`, `GTSAM_USE_TBB`, `GTSAM_ENABLE_BOOST_SERIALIZATION` and `GTSAM_USE_BOOST_FEATURES`.

*Command line.* `fusion_golden_capture [--revision <text>] [<output-directory>]`. The output directory defaults to `FLYSIGHT_FUSION_GOLDEN_DIR`; it is created if missing (`QDir::mkpath`). `--revision` is the repository revision recorded in `capture.json` (the recipe passes `$(git rev-parse HEAD)`); when absent the tool writes `null` and prints a warning on stderr (`no --revision given: capture.json records null; pass --revision "$(git rev-parse HEAD)" before committing`). Anything else on the command line is a usage error (exit 1, usage on stderr). Use a `QCoreApplication` for `arguments()` as the branch harness did (`tests/README.md` lines 1380-1405); no `TestEnvironment`, no settings, no logbook: the kernel is pure and the tool touches nothing but the output directory.

*Per fixture* (in `fusionFixtures()` order; the model is the branch harness's `capture()` in `tests/README.md` lines 1331-1375, read it before that section is rewritten):
1. `Channels channels = toChannels(fixture)`; `QStringList progress`; `PipelineTrace trace`; `Result result = Detail::runPipeline(channels, Detail::Tuning{}, Detail::Checkpoint([&](const QString &t) { progress.append(t); }, {}), &trace)`. This is exactly what `Fusion::run()` does (`src/fusion/fusion.cpp` lines 131-135: `run()` builds a `Checkpoint` and calls `runPipeline(channels, Tuning{}, checkpoint)`) with the trace seam added, so one run yields the result, the progress texts and the trace. `runPipeline` never throws for these inputs (it catches every `std::exception`); `std::bad_alloc` may propagate and end the tool.
2. Build the golden object: `fixture` (name); `outcome` = `"succeeded"` / `"rejected"` / `"solver_failed"` (the last never expected; see step 5); `diagnostics` = `QJsonDocument::fromJson(result.diagnosticsJson.toUtf8()).object()` (the compact string re-serialized indented; the exact-mode tests already prove that this parse-and-reserialize round trip is byte-exact for the compact form); `progress` = the collected texts as a `QJsonArray` (empty for a rejection: nothing is reported before "Starting fit", `src/fusion/fusion.cpp` line 63). For a success also `trace` = `traceJson(trace)`, `rows` = `int(result.time.size())`, `channels_file` = `<name>.channels.txt`.
3. Serialize the golden with `QJsonDocument(object).toJson(QJsonDocument::Indented)` (Qt sorts keys and indents four spaces: the committed layout) and the channels with `fusionChannelsText(result)`.
4. Determinism: repeat steps 1-3 a second time and compare the JSON bytes and the channel bytes of the two captures. A difference is fatal: print `<fixture>: two captures differ (<json|channels>)` on stderr, write nothing more, exit 3. (Two in-process runs are what `twoRunsAreBitIdentical` checks; TBB is on.)
5. Write `<name>.json` and, for a success, `<name>.channels.txt`, overwriting (`QIODevice::WriteOnly | QIODevice::Truncate`, **without** `QIODevice::Text`, which would turn LF into CRLF on Windows). Print one line on stdout: `<name>: succeeded, <rows> rows, objective <objective>` or `<name>: rejected (<failure>)` or `<name>: solver_failed (<failure>)`, followed by `  ** UNEXPECTED **` when `(outcome == succeeded) != fixture.expectSuccess`. Remember the flag.
6. Record the SHA-256 (lower-case hex, `QCryptographicHash::Sha256`) of every byte array written, keyed by file name, for `capture.json`.

*After all fixtures* write `capture.json` (Task 1.3), print `wrote <n> files to <directory>`, and exit 0, or 2 when any fixture was unexpected (the files are still written so that the phase can look at them; a `capture.json` produced by an exit-2 run is never committed).

Two runs of the tool into two directories must produce byte-identical `<fixture>.json` and `.channels.txt` (the in-process check makes a cross-process difference implausible; the recipe does not require a second process run).

**Acceptance Criteria:**
- [ ] `cmake --build build-phase1 --config Release` builds `build-phase1/FlySightViewer-build/Release/fusion_golden_capture.exe`; the configure log still prints `GTSAM link confinement: OK (...)` and `Fusion exact tests registered for Release (...)`
- [ ] Run with no arguments and the DLL directories on `PATH` (Task 1.6 step 2), the tool prints twelve lines in fixture order, none marked `** UNEXPECTED **`, then `wrote 16 files to <...>/tests/data/fusion` (twelve `.json`, three `.channels.txt`, `capture.json`), and exits 0
- [ ] Run into an empty scratch directory twice, the two directories are byte-identical for all fifteen fixture files (`diff -rq` prints nothing; `capture.json` differs only if the date changed)
- [ ] A build with `FLYSIGHT_BUILD_FUSION_TESTS=OFF` (configure only, in a scratch build directory, or by inspection) defines no target named `fusion_golden_capture`
- [ ] `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L audit` passes (the confinement regex admits the new source; nothing else under `tests/` includes a GTSAM header)
- [ ] `src/fusion/` is untouched (`git status --porcelain -- src/fusion/` prints nothing)

**Complexity:** L

---

### Task 1.3: `capture.json` from the tool, and the exact-test gate

**Purpose:** Provenance is written by the tool, from this repository, in a shape the configure-time gate (`tests/CMakeLists.txt` lines 361-368) keeps reading unchanged.

**Files to modify:**
- `tests/fusion_golden_capture.cpp` — the `capture.json` writer (part of the tool; listed separately so the field layout is reviewable)
- `tests/data/fusion/capture.json` — replaced by the tool's output in Task 1.5
- `tests/CMakeLists.txt` — comment lines 336-345 only, if they still say "not a defect of the port"; the gate logic is unchanged

**Technical Approach:**

Write `capture.json` with `QJsonDocument::Indented` (keys sort alphabetically; that is fine). Fields, all written by the tool:

| Key | Value | Source |
|---|---|---|
| `description` | `"Provenance of the fusion goldens in this directory, written by fusion_golden_capture from the product kernel. Procedure: tests/README.md, section 11."` | constant |
| `captured_by` | `"fusion_golden_capture"` | constant |
| `capture_date` | ISO date | `QDate::currentDate().toString(Qt::ISODate)` |
| `repository_revision` | the `--revision` text, or `null` | command line |
| `machine` | `{"os": ..., "cpu_architecture": ...}` | `QSysInfo::prettyProductName()`, `QSysInfo::currentCpuArchitecture()` |
| `compiler` | `{"id", "version", "cl_version" (MSVC only, same string as `version`), "generator", "platform", "configuration", "cmake"}` | the `FLYSIGHT_CAPTURE_*` definitions; `cl_version` written only when `FLYSIGHT_CAPTURE_COMPILER_ID` is `"MSVC"` |
| `qt` | `qVersion()` | runtime |
| `solver` | `{"gtsam_version": GTSAM_VERSION_STRING, "gtsam_use_tbb": bool, "gtsam_enable_boost_serialization": 0/1, "gtsam_use_boost_features": 0/1, "gtsam_dir": FLYSIGHT_CAPTURE_GTSAM_DIR, "pinned_in": "cmake/SolverSuperbuild.cmake"}` | `<gtsam/config.h>` macros, CMake |
| `determinism` | `{"runs": 2, "byte_identical": true, "method": "every fixture is captured twice in one process and the bytes of each file compared before it is written"}` | constant (a difference ends the tool before this file is written) |
| `sha256_lf` | file name -> SHA-256 of the bytes written, for the fifteen fixture files | Task 1.2 step 6 |
| `sha256_note` | `"Hashes of the files as written (LF line endings). A checkout with core.autocrlf=true has CRLF in the working tree; the loader accepts both."` | constant |
| `fixture_generator_sha256_lf` | `tests/fusion/fusionfixtures.cpp` and `.h` -> SHA-256 of the file bytes with every `\r\n` replaced by `\n` | read from `FLYSIGHT_FUSION_FIXTURE_SOURCE_DIR` |
| `fixture_generator_note` | `"The fixture generator the goldens were captured with. The goldens are valid for these inputs only: if either file changes, re-capture (tests/README.md, section 11)."` | constant |

Dropped, deliberately: `reference_branch`, `reference_revision`, `cl_command_line_batchfusion_cpp`, `cross_check`, the solver's `repository`/`revision`/`options`/`config_h`/`gtsam_link_interface` blocks (the solver revision is fixed by the pin in `cmake/SolverSuperbuild.cmake` at the recorded repository revision), `msvc_toolset`, `windows_sdk`, the CPU model string.

The gate: `tests/CMakeLists.txt` line 365 matches `"cl_version" *: *"([0-9]+\.[0-9]+)[0-9.]*"` anywhere in the file. `CMAKE_CXX_COMPILER_VERSION` under MSVC 19.44 is `19.44.<build>`, so the gate keeps working with no edit. Confirm after Task 1.5's reconfigure that the log says `Fusion exact tests registered for Release (FLYSIGHT_FUSION_EXACT_TESTS=AUTO, MSVC 19.44...)`. If `CMAKE_CXX_COMPILER_VERSION` turns out not to start with `19.44` on the capture machine, stop: that is a different compiler from the one the goldens were captured with, and Michael decides.

Convention for `repository_revision`, to be stated in `tests/README.md` (Task 1.7): the capture happens before the phase's commit exists, so the recorded revision is the parent of the commit that contains the goldens, and the phase's working-tree changes are what the kernel was at capture time. The commit that holds the goldens is `git log -1 -- tests/data/fusion/capture.json`.

**Acceptance Criteria:**
- [ ] After Task 1.5's run, `tests/data/fusion/capture.json` contains every key of the table above and none of the dropped ones (`grep -c "reference_branch\|cross_check\|batchfusion" tests/data/fusion/capture.json` prints 0)
- [ ] `compiler.cl_version` is `"19.44.<build>"` and `cmake build-phase1/FlySightViewer-build` prints `Fusion exact tests registered for Release`
- [ ] `fixture_generator_sha256_lf["tests/fusion/fusionfixtures.cpp"]` equals `568b1b6ce8f6244beafe92e3b3e8c08797e57531d6929946ad57ed6952055906`, the value of the previous `capture.json` (`git show HEAD:tests/data/fusion/capture.json`): the LF normalization and the hashing are right (the `.h` hash differs because Task 1.1 reworded its comment)
- [ ] `sha256_lf` for the twelve files Task 1.5 leaves unchanged (nine rejections, three `.channels.txt`) equals the values in the previous `capture.json`
- [ ] `repository_revision` is the forty-character SHA the recipe passed, not `null`

**Complexity:** S (the writer is about forty lines inside the tool)

---

### Task 1.4: Rename `tst_fusion_parity` to `tst_fusion_golden` and retire the progress-wording translation

**Purpose:** The test is now a golden regression against the product kernel, and the goldens hold the kernel's own progress texts; the branch's wording and the test's translation of it go.

**Files to modify:**
- `tests/tst_fusion_parity.cpp` -> delete after creating the new file (plain file operations; the orchestrator stages the removal and the addition by path)
- `tests/CMakeLists.txt` — lines 318 and 426: `tst_fusion_parity` -> `tst_fusion_golden`; the comment at line 307 ("golden parity against sensor-fusion-clean-port") -> "golden regression against the product kernel (tests/README.md section 11)"; comments at lines 335-345 and 425 reworded (no "port", no "branch")
- `cmake/SolverDependencies.cmake` line 530 — `tst_fusion_parity` -> `tst_fusion_golden` in `_FLYSIGHT_GTSAM_REACHERS`
- `tests/acceptance_map.txt` lines 235-237 and 275-276 — target name; comment 235 -> `# 104 - the kernel reproduces its goldens (captured from it by fusion_golden_capture)`
- `tests/tst_fusion_kernel.cpp` — header comment lines 1-8 and line 175 (`tst_fusion_parity` -> `tst_fusion_golden`; "a parity failure" -> "a golden failure"; "the reference's own self-test" may stay: it is where the literal expectations come from)
- `tests/tst_fusion_jobs.cpp` line 428 — comment: `tst_fusion_parity` -> `tst_fusion_golden`
- `tests/fusion/fusionfixtures.h` lines 12-17 — "compiled twice: by the ported tests, and by the golden capture harness against sensor-fusion-clean-port" -> compiled once into `flysight_fusion_test_support`, used by the tests and by `fusion_golden_capture`; the bit-reproducibility rules stay (the fixtures must be the same bits on every CI compiler for the portable comparison to mean anything)

**Files to create:**
- `tests/tst_fusion_golden.cpp` — the content of `tst_fusion_parity.cpp` with the changes below

**Technical Approach:**

- Class `FusionParityTest` -> `FusionGoldenTest`; `FLYSIGHT_TEST_MAIN(FusionGoldenTest)`; `#include "tst_fusion_golden.moc"`. All test function names stay (the acceptance map names `successFixturesMatchGolden`, `rejectionFixturesMatchGolden`, `cancelAtEachKindOfBoundary`, `cancelDuringPreparation`; the audit checks `tests/tst_fusion_golden.cpp` contains `::<function>()`).
- Header comment (lines 1-15): the kernel through its public API only; for each committed fixture the fit reproduces the goldens captured from the kernel by `fusion_golden_capture` (`tests/data/fusion/`, procedure in `tests/README.md` section 11); goldens change only by re-capture at the end of a phase that changes numerical results, never to make a test pass. Keep the paragraph on expectations and the `FLYSIGHT_FUSION_EXACT` note. Drop "ported", "reference", "sensor-fusion-clean-port".
- Delete `referenceProgressText()` (lines 95-104) and its use at line 351. `progressMatchesReferenceBoundaries` becomes `progressMatchesGoldenBoundaries` and appends the received text unchanged; it compares `received` with `golden.progress` directly. This is the change that makes the goldens' `progress` arrays the kernel's own texts (`Starting fit`, `Pass N, iteration M`, `Integrating IMU factors`), which Task 1.5's first capture writes.
- Add `channelsWriterIsTheInverseOfTheLoader` (Task 1.1's acceptance criterion): for each success fixture, load the golden, copy `golden.channels` into a `Fusion::Result` (the seventeen arrays through the same field order `fusionChannel()` uses), call `fusionChannelsText()`, read `<FLYSIGHT_FUSION_GOLDEN_DIR>/<fixture>.channels.txt`, replace `\r\n` by `\n`, `QCOMPARE` the bytes. Then the five `toHexBits`/`fromHexBits` round trips by bit pattern (`sameBits` from `testutil.h`). No fit runs in this test.
- Everything else (`comparatorHoldsItsBounds`, `fixturesAreDeterministic`, both golden tests, the cancellation tests, `cancelNeverRequestedChangesNothing`, `twoRunsAreBitIdentical`, `workerThreadMatchesMainThread`, `resultIsIndependentOfCallerState`) is unchanged.
- Sequence: complete this task *before* running the tool into `tests/data/fusion/` (Task 1.5). Until the capture, `progressMatchesGoldenBoundaries` fails against the committed goldens (they hold the branch wording); that is expected and is the reason the two steps are ordered.

**Acceptance Criteria:**
- [ ] `tests/tst_fusion_parity.cpp` no longer exists; `tests/tst_fusion_golden.cpp` exists; `grep -rn "tst_fusion_parity" --include=*.cpp --include=*.h --include=*.txt --include=*.cmake --include=*.md --include=*.yml . ` (excluding `build*/`, `third-party/`, `PLANS/`, `experiments/`, `TEMP/`) prints nothing
- [ ] `ctest --test-dir build-phase1/FlySightViewer-build -C Release -N` lists `tst_fusion_golden` and `tst_fusion_golden_exact` and no `tst_fusion_parity*`
- [ ] `tst_fusion_golden.cpp` contains no function `referenceProgressText` and no string `"Heading 0 deg"`
- [ ] After Task 1.5, `ctest -C Release -R "tst_fusion_golden(_exact)?$" --output-on-failure` passes, including `progressMatchesGoldenBoundaries` and `channelsWriterIsTheInverseOfTheLoader`
- [ ] `ctest -C Release -L audit` passes (the acceptance map resolves every `tst_fusion_golden` line)

**Complexity:** M

---

### Task 1.5: First capture — the proof

**Purpose:** Re-capturing from the unchanged kernel and getting the committed goldens back is the proof that the tool writes what the tests compare; it is the only time the tool's output is compared with the branch's.

**Files to modify:**
- `tests/data/fusion/capture.json` — rewritten by the tool
- `tests/data/fusion/coarse_linear.json`, `coarse_maneuver.json`, `stationary_spin.json` — rewritten by the tool; only their `progress` strings change (3, 8 and 7 lines)
- The nine `reject_*.json` and the three `.channels.txt` — rewritten by the tool, byte-identical, so `git status` does not list them

**Technical Approach:**

Run the re-capture recipe of Task 1.6 (all five steps) from the repository root, with Tasks 1.1-1.4 built. Then check the result mechanically (Git Bash):

```bash
git status --porcelain -- tests/data/fusion/
# expected, exactly these four:
#  M tests/data/fusion/capture.json
#  M tests/data/fusion/coarse_linear.json
#  M tests/data/fusion/coarse_maneuver.json
#  M tests/data/fusion/stationary_spin.json
git diff --numstat -- tests/data/fusion/coarse_linear.json tests/data/fusion/coarse_maneuver.json tests/data/fusion/stationary_spin.json
# expected: 3 3, 8 8, 7 7
git diff -U0 -- tests/data/fusion/coarse_linear.json tests/data/fusion/coarse_maneuver.json tests/data/fusion/stationary_spin.json \
  | grep -E '^[-+][^-+]' \
  | grep -vE '"(Starting heading 0 deg|Heading 0 deg, pass [0-9]+, iteration [0-9]+|Starting fit|Pass [0-9]+, iteration [0-9]+)",?[[:space:]]*$'
# expected: no output (every changed line is a progress text: the branch's wording out, the kernel's in)
```

`core.autocrlf=true` on the capture machine: the tool writes LF, the index holds LF, `git diff` normalizes, so line endings never show up as a difference. Do not open the golden files in an editor between the capture and the check.

Then the tests, in both modes: `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L fusion --output-on-failure` and `... -L exact --output-on-failure`, all green, and the whole suite once without `-L`.

**If the check fails** (any other file listed, any other changed line, a `** UNEXPECTED **` line, exit 2 or 3, or a red exact test): the tool is wrong, not the kernel. Stop; do not touch `src/fusion/`, the fixtures or the goldens by hand; find the difference with `git diff` on the offending file (`fitTraceMatchesGolden` and `successFixturesMatchGolden` also name the first differing value) and fix the tool or the writers of Task 1.1; then discard the tool's output (`git checkout -- tests/data/fusion/` is the orchestrator's call; the implementer reports and asks) and run again. A difference in a `.channels.txt` file means `fusionChannelsText()` or `toHexBits()`; in a `trace` object, `fusiontrace.h`; in `diagnostics`, the serialization of `result.diagnosticsJson` (it must be parsed and written with `QJsonDocument::Indented`, nothing edited); a different `rows` or `progress` count, the wrong entry point (it must be `runPipeline` with `Tuning{}`, as `Fusion::run()` calls it).

**Acceptance Criteria:**
- [ ] `git status --porcelain -- tests/data/fusion/` lists exactly `capture.json`, `coarse_linear.json`, `coarse_maneuver.json`, `stationary_spin.json`
- [ ] `git diff --numstat` on the three success goldens is `3 3`, `8 8`, `7 7`, and the filtered `git diff -U0` above prints nothing
- [ ] The tool printed twelve lines with no `** UNEXPECTED **` and exited 0; `coarse_linear`'s objective is about `2.9e-12`, `coarse_maneuver`'s `gnss_states` is 28 and `stationary_spin`'s `initialization` is the stationary method (visible in the JSON)
- [ ] `ctest -C Release -L fusion` and `ctest -C Release -L exact` pass after `cmake build-phase1/FlySightViewer-build` re-read the new `capture.json`; the whole suite passes without `-L`
- [ ] The phase's report lists the four changed golden files as part of the phase's files (Commit Policy: re-captured goldens are committed with the phase)
- [ ] The phase's report lists the eighteen changed progress lines side by side, per fixture (`coarse_linear` 3, `coarse_maneuver` 8, `stationary_spin` 7): golden wording before (`Starting heading 0 deg`, `Heading 0 deg, pass N, iteration M`) and kernel wording after (`Starting fit`, `Pass N, iteration M`), one row per line, taken from `git diff -U0` on the three files, so the record shows that the difference is wording and nothing else
- [ ] The phase's report states that after this capture the reference branch `sensor-fusion-clean-port` is the source of nothing: not of the goldens, not of the provenance, not of any test expectation

**Complexity:** S (running and checking; the work is in Tasks 1.1-1.4)

---

### Task 1.6: The re-capture procedure (recipe for Phases 3-6)

**Purpose:** Every numerical phase ends by re-capturing the goldens; the recipe must be short, exact and repeatable so a later implementation agent follows it verbatim. It is verified once in Task 1.5 and written into `tests/README.md` in Task 1.7.

**Files to modify:**
- `tests/README.md` section 11, subsection "Re-capture procedure" (Task 1.7 places it)

**Technical Approach:**

The recipe, Git Bash, from the repository root, on the capture machine (64-bit MSVC 19.44, Release, `build-phase1/`):

```bash
# 1. Build the kernel, the tests and the tool (Release).
cmake --build build-phase1 --config Release

# 2. Capture into tests/data/fusion/ (the default output directory), recording HEAD.
#    PATH: Qt's bin, then the GTSAM and oneTBB install bin directories (gtsam.dll,
#    metis-gtsam.dll, cephes-gtsam.dll, tbb12.dll, tbbmalloc.dll). No other solver on PATH.
PATH="/c/Qt/6.9.3/msvc2022_64/bin:$PWD/build-solver-deps/GTSAM-install/bin:$PWD/build-solver-deps/oneTBB-install/bin:$PATH" \
  build-phase1/FlySightViewer-build/Release/fusion_golden_capture.exe --revision "$(git rev-parse HEAD)"
#    Check: twelve lines, one per fixture, in fixture order; the three success fixtures
#    "succeeded", the nine reject_* "rejected (<reason>)"; no "** UNEXPECTED **";
#    then "wrote 16 files to .../tests/data/fusion"; exit status 0.

# 3. Reconfigure the application build: the exact-test gate reads capture.json at
#    configure time (file(READ) is not a dependency, so this step is explicit).
cmake build-phase1/FlySightViewer-build
#    Check: the log says "Fusion exact tests registered for Release (... MSVC 19.44...)".

# 4. Run the fusion tests in both modes.
ctest --test-dir build-phase1/FlySightViewer-build -C Release -L fusion --output-on-failure
ctest --test-dir build-phase1/FlySightViewer-build -C Release -L exact --output-on-failure
#    Check: all green. -L fusion covers the portable comparison and (on this machine) the
#    exact runs; -L exact alone is the explicit bit-identity proof of the rebuilt kernel.

# 5. Report the changed golden files; they are part of the phase's files.
git status --porcelain -- tests/data/fusion/
git diff --stat -- tests/data/fusion/
```

PowerShell equivalent of step 2's `PATH`: `$env:PATH = "C:\Qt\6.9.3\msvc2022_64\bin;$PWD\build-solver-deps\GTSAM-install\bin;$PWD\build-solver-deps\oneTBB-install\bin;$env:PATH"` then `build-phase1\FlySightViewer-build\Release\fusion_golden_capture.exe --revision (git rev-parse HEAD)`.

What changes, and what it means:
- `capture.json` changes on every capture (date, revision, hashes).
- A `<fixture>.json` / `.channels.txt` changes when the phase changed that fixture's numbers or texts. Phase 3 changes the `algorithm` string, so all twelve `.json` files change then; the nine `reject_*.json` otherwise change only when a rejection reason changes.
- Two captures of the same build are byte-identical; the tool verifies this in-process and refuses to write otherwise (exit 3).
- A `** UNEXPECTED **` line (exit 2) means a success fixture no longer converges or a rejection fixture is no longer rejected: a kernel regression or a fixture the phase invalidated. Do not commit that capture; fix the kernel, or (only when the phase's specification says a fixture's expectation changes) the fixture and its `expectSuccess`, then capture again.
- The phase's report lists every file `git status` shows under `tests/data/fusion/`.

**Acceptance Criteria:**
- [ ] The recipe above, run verbatim in Task 1.5, produced the expected result on the capture machine
- [ ] The same five steps appear in `tests/README.md` section 11 under a heading "Re-capture procedure" (Task 1.7) with no step that references the branch, a scratch harness, `git archive`, a Boost build or a cross-check
- [ ] The recipe names the exact paths (`build-phase1/`, `build-phase1/FlySightViewer-build/Release/fusion_golden_capture.exe`, the three `bin` directories) and the exact checks after each step

**Complexity:** S

---

### Task 1.7: Rewrite `tests/README.md` section 11 and the cross-references

**Purpose:** The documentation states the in-tree procedure and the golden-regression meaning of the tests; nothing describes goldens as captured from the branch any more.

**Files to modify:**
- `tests/README.md` — section 11 (lines 803-1467), the table of contents (line 13), section 1 (lines 35-40, 122, 126, 129-131), section 3 (lines 235-236, 248-252, 262), section 9.2 (line 675), section 10 (lines 739-751), the exact-mode command at lines 1010-1012
- `README.md` — line 207 (option table: "golden parity tests" -> "golden regression tests", `tst_fusion_*_exact`), line 225 ("re-validating the fusion goldens ... 'Fusion golden parity'" -> re-capturing them with `fusion_golden_capture`, section "Fusion golden regression"), line 420
- `CMakeLists.txt` — lines 87 and 101 (option help text: "golden parity tests" -> "golden regression tests")
- `docs/SENSOR_FUSION.md` — section 8 table row `tst_fusion_parity` -> `tst_fusion_golden`, "the goldens captured from the reference" -> "the goldens captured from the kernel by `fusion_golden_capture`"; the sentence "The fixtures, the two modes and the capture procedure are described in tests/README.md, section 11" stays. The frozen-algorithm paragraph is left for Phase 7 (Open Questions)
- `cmake/SolverSuperbuild.cmake` — comment lines 52-60: replace the references to "Solver configuration of the goldens", the harness's `find_package(GTSAM 4.3 ...)` and `capture.json ("solver")`'s revision with: `tests/data/fusion/capture.json` (`solver.gtsam_version`) and `tests/README.md` section 11; and "Moving the GTSAM pin changes the solver the fusion goldens were captured against. They must be re-validated before the new pin is accepted, by the procedure in tests/README.md: never edited to match" -> moving the pin means re-capturing with `fusion_golden_capture` and examining the golden diff; the exact tests then prove the rebuild is bit-identical to the new capture
- `tests/audit/cleanup_audit.cmake` — comments at lines 291-295 and 332-333; remove `":!tests/data/fusion/capture.json"` from `NAMING_PATHS` (the new file names nothing of the branch); keep `":!tests/README.md"` there with a new reason (section 10 of the README spells the `EKF` / `posN` patterns when it describes this rule); remove `":!tests/README.md"` from the Boost rule (the harness listing that mentioned `find_dependency(Boost` is gone). Run the audit to confirm

**Technical Approach:**

Section 11 becomes "## 11. Fusion golden regression" (update the TOC anchor to `#11-fusion-golden-regression`). Structure and content:

1. *Intro.* `tests/data/fusion/` holds what the product kernel (`src/fusion/`, `flysight_fusion`) produced for twelve synthetic fixtures when the goldens were last captured by `fusion_golden_capture`; `tst_fusion_golden` and `tst_fusion_kernel` (and, through the engine and the queue, `tst_fusion_session`, `tst_fusion_jobs`, `tst_fusion_rows`) compare the kernel with them (sensor-fusion-jobs acceptance 4). Policy in bold: **goldens change only by re-capture with the tool, at the end of a phase that deliberately changes numerical results, never by hand and never to make a test pass**; a fixture is adjusted only when its specification changes. One sentence of history: the first goldens were captured from the branch `sensor-fusion-clean-port` by an out-of-tree harness; on 2026-09-2x the in-tree tool reproduced them bit for bit from the ported kernel, apart from the wording of the progress texts, which is now the kernel's own; since that capture the branch is the source of nothing. (This is the only remaining mention of the branch in the section.)
2. *Fixtures.* Both tables unchanged, except "with the reason the reference gives" -> "with the reason the kernel gives" and "the reference self-test's" -> "the constant-velocity" (the literal fixture descriptions stay).
3. *Files in `tests/data/fusion/`.* As today for `<fixture>.json` and `.channels.txt` ("the reference's" -> "the kernel's"; the progress texts are the kernel's: `Starting fit`, `Integrating IMU factors`, `Pass N, iteration M`; drop "the branch's posN/posE/posD under their new names"). `capture.json`: the field table of Task 1.3 in prose (revision convention included), and that the tool writes it.
4. *Tolerance policy.* Unchanged in substance and numbers. Reword: "This is the mode that decides whether the port is faithful" -> "the mode that proves a rebuild on the capture configuration is bit-identical to the capture, which is what catches an unintended numerical change"; "not a defect of the port" -> "not a defect of the kernel"; `tst_fusion_parity` -> `tst_fusion_golden` everywhere including the `-R "tst_fusion_(golden|kernel|session|jobs|rows)$"` command; "the branch's docs/PORT_VALIDATION.md records" may stay as the source of the sensitivity numbers.
5. *The capture tool.* What it is (non-test executable in the fusion block, links `flysight_fusion_test_support` and `gtsam`, 64 MiB stack, not installed), the command line, the per-fixture output line, the in-process determinism check, the exit statuses (0, 1 usage/IO, 2 unexpected outcome, 3 non-deterministic), where the writers live (`fusionChannelsText()` next to the loader, `fusiontrace.h` shared with `fitTraceMatchesGolden`), and that it uses `runPipeline` with the production `Tuning` as `Fusion::run()` does, so the golden is what the public API returns plus the trace.
6. *Re-capture procedure.* The recipe of Task 1.6 verbatim, with its checks and "what changes" list.
7. *Fusion sessions* and *Real recordings*: unchanged (the real-recordings paragraph may keep its reference to the branch's validation document as the source of the recording's numbers).

Delete "Solver configuration of the goldens" and "Capture procedure" entirely, including the harness `CMakeLists.txt` and `main.cpp` listings (lines 1032-1407).

Section 1: line 39 "`solver_deploy_probe` is also built, but is not a test" -> "`solver_deploy_probe` and `fusion_golden_capture` are also built, but are not tests"; table row 122 for `tst_fusion_golden` (reword: goldens captured from the kernel by `fusion_golden_capture`, progress texts at the kernel's boundaries, the channel writer as the inverse of the loader; drop "captured from sensor-fusion-clean-port" and "the reference's"); row 123 (`tst_fusion_kernel`: "a parity failure" -> "a golden failure"; "the reference's self-test" may stay); row 126 (`tst_fusion_golden_exact`; "The first two decide parity" -> "The first two decide bit-identity"); paragraph 129-131 introduces both non-test executables.

Section 3: option table row 235 names `tst_fusion_golden` and `fusion_golden_capture` among what `FLYSIGHT_BUILD_FUSION_TESTS` builds; row 236 wording; line 262 (`ctest -L exact` comment "bit-exact golden parity" -> "bit-exact golden regression").

Section 9.2 row 4: "the port reproduces the goldens" -> "the kernel reproduces its goldens, captured from it by `fusion_golden_capture`"; test names updated. Appendix B is the old specification verbatim and stays as it is.

Section 10: the `naming` bullet's exclusion sentence -> "This file is excluded: this section spells the patterns"; the `solver-confinement` bullet "the four GTSAM test sources" -> "the five GTSAM test and tool sources".

**Acceptance Criteria:**
- [ ] `grep -n "sensor-fusion-clean-port\|batchfusion\|git archive\|cross-check\|cross_check\|Boost-enabled\|fusion_golden_harness" tests/README.md` prints only the one historical sentence of the section 11 intro (and, if kept, the real-recordings reference to the branch's validation document)
- [ ] `grep -rn "tst_fusion_parity\|golden parity\|Fusion golden parity" README.md tests/README.md docs/ CMakeLists.txt cmake/ tests/ src/` prints nothing
- [ ] `tests/README.md` section 11 has the subsections Fixtures, Files, Tolerance policy, The capture tool, Re-capture procedure, Fusion sessions, Real recordings, in that order, and no listing of a scratch harness
- [ ] The TOC link for section 11 resolves (heading and anchor agree)
- [ ] `ctest -C Release -L audit` passes with the two exclusions removed and the comments updated
- [ ] `cmake/SolverSuperbuild.cmake`'s pin comment names `fusion_golden_capture` and no longer names a README section that does not exist

**Complexity:** L

---

### Task 1.8: CI check (no workflow edit expected)

**Purpose:** Confirm that the workflow references nothing this phase retires; a CI edit, if one turns out to be needed, is a separate commit per the Commit Policy.

**Files to modify:**
- `.github/workflows/build.yml` — none expected

**Technical Approach:**

`.github/workflows/build.yml` configures with `-DFLYSIGHT_BUILD_TESTS=ON` on non-tag builds (lines 496-512) and runs `ctest --test-dir build --output-on-failure -C Release` (line 525). It does not name `tst_fusion_parity`, `capture.json`, the branch or the exact label; the deployment checks name `solver_deploy_probe` and the runtime DLLs (`tests/acceptance_map.txt` `ci` lines), which are unchanged. The tool is built on every CI platform because it is in the fusion block: its source must compile with GCC and AppleClang (no MSVC-only constructs; `_MSC_FULL_VER` is not used since the compiler version comes from CMake; `<gtsam/config.h>` and Qt only). CI never runs the tool. The exact tests register on the Windows runner only if its MSVC is 19.44, exactly as today.

Verify by reading the workflow (`grep -n "fusion\|parity\|capture\|exact" .github/workflows/build.yml` prints nothing relevant) and report "no CI change" in the phase's report. If the implementer finds a reference that must change, list the edit separately as `Phase 1: CI (unverified)` and do not fold it into the phase's files.

**Acceptance Criteria:**
- [ ] The phase's report states whether `.github/workflows/build.yml` was edited; expected: not edited, with the grep evidence
- [ ] `tests/acceptance_map.txt`'s `ci` lines (`solver_deploy_probe`, `libgtsam`, `gtsam.dll`) still resolve (`ctest -L audit` passes)

**Complexity:** S

---

## Testing Requirements

### Unit Tests
- New in `tst_fusion_golden`: `channelsWriterIsTheInverseOfTheLoader` (the writer against the committed channel files, LF-normalized; `toHexBits`/`fromHexBits` round trips by bit pattern). Everything else in the file is the existing `tst_fusion_parity` content, with `progressMatchesReferenceBoundaries` renamed to `progressMatchesGoldenBoundaries` and comparing the kernel's texts directly.
- `tst_fusion_kernel::fitTraceMatchesGolden` is unchanged in behaviour; it now obtains `traceJson()` from `tests/fusion/fusiontrace.h`.
- No QProcess test of the tool: its proof is Task 1.5's bit-for-bit reproduction, and Task 1.6 re-proves it at every numerical phase (Decisions Made).
- No test in `tst_fusion_session`, `tst_fusion_jobs` or `tst_fusion_rows` changes; they compare through `goldenDifference()` and `compareJson()` and depend on nothing in `capture.json`.

### Integration Tests
- `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L fusion --output-on-failure`: all fusion tests green against the re-captured goldens (portable mode).
- `ctest ... -C Release -L exact --output-on-failure`: `tst_fusion_golden_exact`, `tst_fusion_kernel_exact`, `tst_fusion_session_exact`, `tst_fusion_jobs_exact`, `tst_fusion_rows_exact` green: the rebuilt kernel is bit-identical to the capture.
- `ctest ... -C Release -L audit`: the acceptance map, the confinement regex and the naming rules after the edits.
- The whole suite once without `-L`.
- Configure-time: `flysight_assert_solver_confinement()` prints `GTSAM link confinement: OK`; the exact-test gate prints "registered for Release".

### Manual Verification
- Task 1.5's three `git` checks (status, numstat, filtered `diff -U0`) and the hash comparisons of Task 1.3 against `git show HEAD:tests/data/fusion/capture.json`.
- Run the tool twice into two scratch directories and `diff -rq` them (Task 1.2).
- Run the tool with a bogus argument: usage on stderr, exit 1, nothing written.

## Notes for Implementer

### The re-capture recipe (quote this in Phases 3-6)

From the repository root, Git Bash, on the capture machine (`build-phase1/`, 64-bit MSVC 19.44, Release):

1. `cmake --build build-phase1 --config Release`
2. `PATH="/c/Qt/6.9.3/msvc2022_64/bin:$PWD/build-solver-deps/GTSAM-install/bin:$PWD/build-solver-deps/oneTBB-install/bin:$PATH" build-phase1/FlySightViewer-build/Release/fusion_golden_capture.exe --revision "$(git rev-parse HEAD)"` — twelve lines, no `** UNEXPECTED **`, `wrote 16 files to .../tests/data/fusion`, exit 0.
3. `cmake build-phase1/FlySightViewer-build` — log says `Fusion exact tests registered for Release`.
4. `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L fusion --output-on-failure` and `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L exact --output-on-failure` — all green.
5. `git status --porcelain -- tests/data/fusion/` and `git diff --stat -- tests/data/fusion/` — list every changed golden file among the phase's files.

An unexpected outcome (exit 2) or a non-deterministic capture (exit 3) is never committed; a red test after the capture means the kernel and the goldens disagree with the tests' literal expectations (a count, a text, a key set), which the phase must resolve in the tests or the kernel, never by editing a golden.

### Required elements of the implementer's summary

Besides the file list, the phase's report must contain:

1. The output of the three `git` checks of Task 1.5 (`status --porcelain`, `--numstat`, the filtered `diff -U0`, which must be empty).
2. A table of the eighteen changed progress lines, side by side: fixture, golden wording before, kernel wording after; one row per line (3 + 8 + 7), taken from `git diff -U0` on the three success goldens. This is the record that the first capture changed wording and nothing else.
3. The sentence: after this capture the reference branch `sensor-fusion-clean-port` is the source of nothing (goldens, provenance and test expectations all come from the product kernel and this repository).
4. The tool's twelve output lines and its exit status; the configure log line `Fusion exact tests registered for Release (...)`; the `ctest -L fusion`, `-L exact` and `-L audit` results.
5. Whether `.github/workflows/build.yml` was edited (Task 1.8; expected: not).

### Gotchas
- **Order of Tasks 1.4 and 1.5.** Remove `referenceProgressText()` before the first capture; run the capture before expecting `tst_fusion_golden` to pass. In between, `progressMatchesGoldenBoundaries` is red by design.
- **Line endings.** Open output files without `QIODevice::Text`; write `\n` only. `QJsonDocument::toJson()` and `fusionChannelsText()` produce LF. The working tree has CRLF (`core.autocrlf=true`), the index LF; `git diff` and `git status` normalize, `cmp`/`diff` on working-tree files do not. Hash the fixture generator after replacing `\r\n` by `\n`.
- **The confinement lists.** A target that links `gtsam` and is not in `_FLYSIGHT_GTSAM_NAMERS` fails the configure; a test source that includes `<gtsam/...>` and is not in the audit regex fails `audit_cleanup`. `fusiontrace.h` includes no GTSAM header directly and needs no regex entry; `fusion_golden_capture.cpp` does (`<gtsam/config.h>`) and needs one.
- **`flysight_fusion_test_support` stays GTSAM-header-free.** Do not compile `fusiontrace.h`'s function into it; do not add `gtsam` to its link line.
- **Linking.** `flysight_fusion` is a static library; `fusionregistration.obj` references engine symbols but is pulled in only if referenced, so the tool should link without `flysight_core`. If the linker nevertheless reports unresolved engine symbols, add `flysight_core` to the tool's link line (then GeographicLib's `bin` also goes on `PATH` in the recipe) and record it.
- **The gate regex** matches `"cl_version"` anywhere in `capture.json`; keep that key name and the `major.minor.build` string. A `capture.json` without it disables the exact tests with a WARNING at configure time.
- **`QIODevice::NewOnly`** (the branch harness's choice) refuses to overwrite; the tool must overwrite the committed files.
- **Determinism check** doubles the run time (about two seconds per success fixture on the capture machine); that is intended. Do not add a flag to skip it.
- **`$<CONFIG>` in a compile definition** is evaluated per configuration by the Visual Studio generator; the Release build records "Release".
- **`git` commands.** The implementer runs only read-only git commands (`status`, `diff`, `show`, `rev-parse`, `log`); the orchestrator stages, commits and, if the proof fails, restores `tests/data/fusion/`. Report file paths, including the deleted `tests/tst_fusion_parity.cpp`, for staging by path.
- **Do not touch `src/fusion/`.** The trace seam (`Detail::runPipeline`, `PipelineTrace`) already exists; nothing needs adding.

### Decisions Made
- **Test name:** `tst_fusion_golden` (class `FusionGoldenTest`, CTest `tst_fusion_golden` / `tst_fusion_golden_exact`). It says what the test is: a golden regression of the kernel. Function names unchanged so the acceptance map's evidence stays valid.
- **The progress-wording exception to "bit for bit".** The committed goldens hold the branch's progress texts (`Starting heading 0 deg`, `Heading 0 deg, pass N, iteration M`), and `tst_fusion_parity` translated the kernel's texts into that wording before comparing. The overview says goldens hold the kernel's own texts from here on (Phase 5 defines new segment texts against that assumption), and the policy is that goldens change only by re-capture, never by hand. So the tool writes the kernel's texts, the translation is deleted, and the first capture changes exactly the eighteen progress lines (3 + 8 + 7) of the three success goldens and nothing else. Every number, every diagnostics key and value, every trace row, every channel bit and every rejection file reproduces bit for bit, which is the proof the overview asks for; the enumerated wording diff is checked mechanically in Task 1.5. This is a deliberate deviation from the overview's letter ("no change except `capture.json`") in favour of its intent, recorded here for the orchestrator.
- **Trace seam:** the tool includes `fusion/fusionpipeline.h` and links `gtsam`, like `tst_fusion_kernel`, and calls `runPipeline` once per fixture with a progress-collecting `Checkpoint` and a `PipelineTrace` (equivalent to `Fusion::run()` plus the trace). The overview's "the capture tool uses the public `fusion.h` only" cannot hold: the golden `trace` object exists only through the internal seam, and `fitTraceMatchesGolden` needs it. No new seam is added to `src/fusion/`.
- **Where the writers live:** `fusionChannelsText()` and `toHexBits()` in `fusiongolden.cpp` next to the loader; `traceJson()` in the new header-only `tests/fusion/fusiontrace.h` shared by the tool and `tst_fusion_kernel` (the support library must stay free of GTSAM headers).
- **Determinism is checked by the tool** (two in-process captures per fixture, compared before writing), not by the procedure; the recipe therefore has no second run and no `diff -rq` step.
- **`capture.json` layout:** the table of Task 1.3. The solver revision is no longer copied into the file; it is fixed by `cmake/SolverSuperbuild.cmake` at the recorded repository revision. The CPU model string is dropped (not obtainable portably); OS and CPU architecture stay.
- **`repository_revision` is passed on the command line** (`--revision`), recorded as `null` with a warning when absent; the tool does not run `git` itself. The convention (parent of the goldens' commit) is documented in the README.
- **Exit statuses:** 0 ok; 1 usage or I/O error; 2 at least one fixture's outcome was not the expected one (files still written); 3 the two in-process captures of a fixture differed (nothing further written).
- **Identifiers kept:** `exactParityRequested()`, `ParityStatistics`, `ParityModeOverride`; only prose and the test target are renamed.
- **No QProcess test for the tool;** Task 1.5 is its test, repeated by every numerical phase.
- **Audit exclusions:** `:!tests/data/fusion/capture.json` removed from `NAMING_PATHS`; `:!tests/README.md` kept there (section 10 spells the patterns) with its comment corrected; the Boost rule's README exclusion removed.
- **`docs/SENSOR_FUSION.md`:** only the test name and the provenance phrase in the section 8 table change in this phase; the "algorithm is frozen" paragraph is Phase 7's (it becomes false in Phase 3 and Phase 7 rewrites the section).
- **Fixture generator hashes stay in `capture.json`** and are computed by the tool from the source files, so a changed generator is visible in the provenance without a manual step.

### Open Questions
- None that block implementation. Two notes for the orchestrator: (1) the progress-wording deviation above; (2) if `CMAKE_CXX_COMPILER_VERSION` on the capture machine is not `19.44.*`, the exact tests will not register after the capture and the proof of Task 1.5 is incomplete; stop and ask Michael (the compiler changed since the branch capture).

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria, in particular Task 1.5's three mechanical `git` checks and the exact tests green against the tool's own capture
2. All tests pass: `ctest -C Release` on `build-phase1/FlySightViewer-build` without `-L`, including `-L fusion`, `-L exact` and `-L audit`
3. Code follows the patterns of `solver_deploy_probe` (non-test executable in the fusion block), `tst_fusion_kernel` (a GTSAM-naming test target), and `fusiongolden.cpp` (writer beside reader)
4. `src/fusion/` is untouched; `.github/workflows/build.yml` is untouched (or its edit is reported separately)
5. No TODOs or placeholder code remains; `tests/README.md` section 11 describes only the in-tree procedure; the phase's report lists every file created, modified, deleted and re-captured
