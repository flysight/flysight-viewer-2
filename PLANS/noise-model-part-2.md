# The documented noise model and the accuracy, part 2: the measured parameters

Date: 2026-09-30
Status: specification for planning, to be planned only once the two
recordings of section 5 exist and their analyses are written up. Not an
implementation plan.
Baseline: branch `store-requested-calculations` at the head that implements
`PLANS/noise-model-part-1.md`; the committed code is authoritative. Where
this document and the earlier specifications disagree, this one wins;
everything it does not mention stays as they specify.
Related: `docs/SENSOR_FUSION.md` (sections 4 and 8), `docs/DATA_SCHEMA.md`,
`tests/README.md` (section 11), `tests/acceptance_map.txt`.

This document says what the change must do and the boundaries it must
respect. It avoids class layouts and function signatures unless a name is
part of the observable contract. The planning agent decides phasing; the
implementation agent decides code structure.

## 1. Motivation

Part 1 gave every parameter of the noise model a documented source: the
datasheet or a derivation. Two things only a measurement can settle remain.
The datasheet's noise figures are typical values for the part at the bench,
not this unit in this case, and the earlier "at rest" measurement made on a
helmet in a live room was ten times the datasheet's, so a figure taken from
a recording is trusted only when the recording was made for the purpose.
And two terms have no datasheet at all: the bias instability over the length
of a fit, which the model assumes is nil, and the sampling term, which part 1
derives and which a recording at a high rate can measure directly. This
specification turns those measurements into the model, and only those.

## 2. Principles

- **A measured parameter cites its recording**: which unit, which
  configuration, where, when, and the analysis that produced the number, in
  the documentation, so that the number can be reproduced.
- **A measurement confirms or replaces; it never tunes.** Where the bench
  agrees with the datasheet within the analysis's own uncertainty, the
  datasheet figure stays and the confirmation is recorded. Where it does not,
  the measured figure replaces it, with the recording cited.
- **Only what the measurements settle changes.** No other constant of the
  model moves in this specification.
- **A fit output changes, so the result version changes**, once, if anything
  numerical changes; if the measurements confirm everything, this
  specification ends as documentation and no version changes.

## 3. Scope

In scope: the analyses of section 6 and the changes of section 7, as the
results of section 6 select them; the evidence document of section 8.

Out of scope: everything else of the model; the receiver's dynamic model;
the firmware.

## 4. Terms

- **Bench recording**: the unit at rest on a rigid mount for hours, the
  temperature logged, made to measure the sensors and nothing else.
- **Six positions**: the bench recording repeated with each of the
  accelerometer's axes pointing up and then down, from which each axis's
  bias and scale follow from gravity alone.
- **High-rate recording**: the IMU at its highest output data rate through
  real motion, decimated in analysis to the rates users log at.
- **Allan deviation**: the standard measure of a sensor's noise against
  averaging time, whose slope -1/2 region gives the white noise, whose flat
  minimum gives the bias instability, and whose slope +1/2 region gives the
  rate random walk.

## 5. The recordings

Both are made and analysed before this specification is planned; the
analysis and its numbers are the input to planning, not something the plan
produces.

- **The bench recording**: on a concrete floor, overnight, away from cycling
  machinery, at the users' default configuration, for at least eight hours;
  a shorter one at the highest rate; the six positions, ten minutes or more
  each; two or three units if available. Stretches disturbed by traffic or
  footfall are identified from the accelerometer's own variance and left out
  of the averaging.
- **The high-rate recording**: a jump or a drop with the IMU at its highest
  rate and the receiver at its usual rate.
- **Their analysis**, in `experiments/`, written up under `docs/` (section
  8): Allan deviation per axis of each sensor from the bench recording's
  quiet stretches; bias and scale per axis from the six positions; from the
  high-rate recording, the readings integrated at full rate against the same
  readings decimated to each of the lower rates, which measures the sampling
  error the derivation of part 1 predicts.

## 6. What the analyses decide

| Analysis | Result | What section 7 then does |
| --- | --- | --- |
| Allan, short averaging times | White noise within the analysis's uncertainty of the datasheet's figure at that configuration | Nothing; the confirmation is recorded |
| | White noise above it | The table entry for that configuration becomes the measured value, cited |
| Allan, the minimum | Bias instability whose drift over a fit's length is below the fit's own bias uncertainty | Nothing; recorded, with the number |
| | Bias instability that matters over a fit | A bias that moves between fixes (section 7) |
| Six positions | Scale and bias within the datasheet's tolerance | The scale state's prior is confirmed |
| | Scale outside it on a unit | The prior widens to cover what a unit showed, cited |
| High-rate decimation | The sampling error within the derivation's bound at every rate | Nothing; the confirmation is recorded |
| | Outside it | The bound's form is revised to what was measured, and the derivation in the documentation with it |

## 7. The changes

- **A measured table entry** replaces the datasheet's in the one unit that
  holds the table, with a comment naming the recording; the documentation's
  table gains a column that says, for each entry, datasheet or measured, and
  for a measured one the recording.
- **A bias that moves between fixes**, if section 6 asks for it: the biases
  become one per fix instead of one per fit, joined by a random walk whose
  per-interval sigma is the Allan figure times the square root of the
  interval, with the same prior on the first as today's prior on the one
  bias; the temperature model of the gyro bias stays on top of it. The
  reconstruction takes each interval's bias as it does now; the accuracy's
  covariance includes the bias of the interval. This is the one change of
  this specification that changes the graph's shape.
- **A widened scale prior**, if section 6 asks for it: the number changes,
  nothing else.
- **A revised sampling bound**, if section 6 asks for it: the term keeps its
  place and its form as a per-step standard deviation; its expression follows
  the measurement, and the documentation says what the derivation missed.
- Whatever changed, the algorithm string changes once, and the goldens are
  captured again with the existing tool, time axes checked byte for byte.

## 8. The evidence document

A new `docs/SENSOR_NOISE.md`: the datasheet table with its table numbers; each
recording with unit, configuration, place, date and duration; the Allan
curves as numbers (the deviation at a set of averaging times per axis and
sensor); the six-position results; the decimation results per rate; and, for
each parameter of the model, one line that says its value and its source.
`docs/SENSOR_FUSION.md` section 4 refers to it for every parameter instead of
carrying the numbers itself, and section 8 records the validation of section
9.

## 9. Validation

The normalized residuals of the reference recordings and the committed
fixtures are reported again under the revised model, beside part 1's, in the
documentation. They are a result; they change no constant.

## 10. Tests

- Each change of section 7 that section 6 selects has the test of its
  definition: a measured table entry is the one the documentation cites, bit
  for bit; a random-walk bias on a synthetic recording with a drifting bias
  follows the drift within the walk's sigma and stays at its start on one
  without; a widened prior is the documented number; a revised bound
  follows its expression on a synthetic signal.
- The evidence document lists every parameter the model uses, checked by the
  audit against the one unit that holds the table: a parameter in the code
  without a line in the document, or the reverse, fails.
- The goldens, the stale-record test, the audit's confinement rules and the
  documents, as in part 1.

## 11. Documentation

`docs/SENSOR_NOISE.md` (new, section 8); `docs/SENSOR_FUSION.md` (sections 4
and 8); `docs/DATA_SCHEMA.md` (section 11, the algorithm string, if it
changes); `tests/README.md` section 11 (the goldens captured again, and why,
if they are) and `tests/acceptance_map.txt` (a new range in the next free
hundred; amended items restated "(as amended)").

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
The implementation orchestrator makes every commit; implementation, revision,
and review agents never run git commands that change repository state.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. Copy this section into the plan overview as its own
top-level section with this exact heading.

- **Branch.** Confirm the working tree has no modified tracked files (if there
  are any, stop and ask). Switch to the existing branch
  `store-requested-calculations`; never recreate, reset, or rebase it.
- **Never staged:** `PLANS/implementation-plan/`, `TEMP/`, `experiments/`,
  `build*/`, `dist/`, `results/`, and everything under `third-party/` that
  is not tracked. Specifications and archived plans under `PLANS/` are
  committed by Michael, never in a phase commit.
- **One commit per accepted phase**, when the review agent returns ACCEPT,
  containing exactly the files the phase's agents reported, staged by explicit
  path. Subject `Noise model part 2 phase N: <phase name>`; body a short
  summary, then the session's attribution line. Rejected iterations are never
  committed.
- **Tag** each phase commit `plan/noise-model-part-2/phase-N-done`; tags are
  never moved or deleted.
- **Fixes to a closed phase** are committed separately as
  `Noise model part 2 phase N fixup: <what>` once reviewed and accepted.
- **CI workflow edits** are committed separately as
  `Noise model part 2 phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.
