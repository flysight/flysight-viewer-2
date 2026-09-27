# FlySight Viewer 2

Qt 6 / C++17 desktop application built with CMake. Development happens on
Windows (Visual Studio 2022, Qt 6.9.3); macOS and Linux build in CI.
Machine-specific notes, when a machine has any, are in `CLAUDE.local.md`,
which is not tracked.

## Build and test

- Build and test in a tree configured as `tests/README.md` section 2
  describes: the superbuild with `FLYSIGHT_BUILD_THIRD_PARTY=OFF` and
  `FLYSIGHT_BUILD_TESTS=ON`, tested with
  `ctest --test-dir <tree>/FlySightViewer-build -C Release --output-on-failure`.
- Do not build a tree configured with `FLYSIGHT_BUILD_THIRD_PARTY=ON` to
  test a change. It rebuilds the installs under `third-party/*-install`,
  which every other tree links against.
- Run the suite sequentially: no `-j`, and not beside another build or ctest.
  The fusion tests that fit through the executor run below normal priority
  and time out under load. Check for stray `ctest` or `tst_*` processes first.
- Only the Release configuration is supported for tests on Windows.
- Running one test or one function: `tests/README.md` section 4.

## What must stay green

- `audit_cleanup` (`tests/audit/cleanup_audit.cmake`): rule groups over the
  source tree, plus a machine check of `tests/acceptance_map.txt`. When a
  change removes or renames something a rule names, the same change updates
  the rule.
- `tests/acceptance_map.txt`: every acceptance item of every specification
  has at least one test, audit or manual line. A new feature continues in a
  new hundred; an amended item is restated "(as amended)".
- `tests/README.md`: a table row per test executable, the manual steps in
  section 12, and an appendix per specification listing its items.
- `docs/`: `CALCULATIONS.md` (engine, executor in section 15, demand layer in
  section 16), `COMPUTED_PLOTS.md` (what the user sees), `DATA_SCHEMA.md`,
  `SENSOR_FUSION.md`, `LOCAL_COORDINATES.md`. A change in behaviour updates
  the document that describes it, in the same change.

## Conventions the audit enforces

- One authority per fact: where two components compute the same thing, one
  stops.
- No generality nothing uses. A replaced mechanism is removed, not kept
  beside its replacement.
- The demand layer is the only starter of requested calculations; nothing
  below it knows about jobs; the core is widget-free; one worker thread and
  no locks; GTSAM is confined to the fusion kernel.
- Comments say why, in prose; a class comment states the class's contract.

## Specifications and plans

`PLANS/<feature>.md` is a specification; `PLANS/implementation-plan/` is the
plan being implemented; `PLANS/done/` holds finished specifications and
their plans. Specifications and archived plans are committed on their own;
an implementation phase's commit never includes anything under `PLANS/`.
The agent workflow is described in `.claude/docs/WORKFLOW-REFERENCE.md`.

`TEMP/` and `experiments/` are local working folders and are ignored; a
document may cite paths in them that exist only on the machine that wrote
it.
