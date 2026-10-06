# Phase 2: The derived accuracies

## Purpose

The specification's section 4, the third bullet of its section 8, the second
clause of the audit bullet, and the parts of section 7 that describe the
calculations. After this phase three on-demand calculations under the
`Fusion` sensor read the covariance blocks phase 1 publishes and produce
`Fusion/hAcc` (m), `Fusion/vAcc` (m) and `Fusion/sAcc` (m/s): the fused
counterparts of the receiver's Horizontal, Vertical and Speed accuracy, each
a presentation of the published facts (section 2, first principle), none
widening anything (phase 1 widened the blocks once).

It is one phase because the registrations, their tests, the audit rule for
their names and the documents that list the calculations cannot be split
without a stale table in between. The pattern is
`PLANS/done/fused-speed-plots-plan/01-fused-speed-plots.md`, "The
calculations", which added `velH` and `vel` through the same files. No plot
row, no colour and no count of fusion plots changes (sections 5 and 6 are
phase 3's), and nothing under `src/fusion/` other than
`fusionregistration.cpp` and `.h` changes.

## Dependencies

Depends on phase 1, whose document fixes what this phase may assume: the fit
publishes `Fusion/posCovNN`, `posCovNE`, `posCovND`, `posCovEE`, `posCovED`,
`posCovDD`, `velCovNN`, `velCovNE`, `velCovND`, `velCovEE`, `velCovED`,
`velCovDD` (the upper triangles of two symmetric 3x3 blocks in the
navigation frame, widened by `w*w`, every filled entry finite, the diagonal
non-negative, filled exactly when `headingAcc` is);
`fusionMeasurementNames()` and `syntheticFitSession()`'s unit table carry
the twelve, so a test can store a block as data; the range 1701-1717 is
declared in the audit with a completeness loop over 1701-1706; section 9.18
and appendix R of `tests/README.md` hold items 1-6; `readsNeverRunTheFit`
counts 39 names.

Blocks phase 3: its rows draw the three measurements, and `tst_fusion_rows`
requires each to be explicit-backed by the fit through a registered
producer.

## What changes

### The calculations (`src/fusion/fusionregistration.cpp`, `.h`)

Three registrations in the manner of `registerHorizontalAcceleration`: a
descriptor with the id, the inputs as `CalcInput::measurement(kSensor, ...)`,
one output, OnDemand, no title, no result version, a compute lambda that
reads its inputs through the context, returns
`CalculationResult::unavailable()` when any input is empty or the inputs
differ in length (the `z` registration's `isEmpty()` check and `accH`'s
length check together), and otherwise one value per sample; and a comment
that says what the value is and why it never starts the fit.
`registerTrackAcceleration` shows a calculation of more than two inputs.

| Id | Inputs, in this order | Output |
| --- | --- | --- |
| `builtin.fusion.hAcc` | `Fusion/posCovNN`, `posCovNE`, `posCovEE` | `Fusion/hAcc` |
| `builtin.fusion.vAcc` | `Fusion/posCovDD` | `Fusion/vAcc` |
| `builtin.fusion.sAcc` | `Fusion/velCovNN`, `velCovNE`, `velCovND`, `velCovEE`, `velCovED`, `velCovDD`, `velN`, `velE`, `velD` | `Fusion/sAcc` |

The definitions (section 4), per sample:

- `hAcc = sqrt(lambda)`, `lambda` the larger eigenvalue of
  `[[NN, NE], [NE, EE]]`: `(NN + EE)/2 + sqrt((NN - EE)^2/4 + NE^2)`, the
  expression `accelerationAccuracy()` in `fitcovariance.cpp` spells for the
  horizontal acceleration (`(hn+he)/2+std::sqrt((hn-he)*(hn-he)/4+hne*hne)`).
- `vAcc = sqrt(DD)`.
- `sAcc`: with `v = (velN, velE, velD)` and `m = |v|`, when `m > 0` the
  along-track variance is
  `(vN^2 NN + vE^2 EE + vD^2 DD + 2 vN vE NE + 2 vN vD ND + 2 vE vD ED) / m^2`
  and `along` its square root; `sAcc = along` where `m >= along`, else the
  square root of the largest eigenvalue of the 3x3 block (`m == 0`
  included): the two tests of `accelerationAccuracy()` (`magnitude > 0`,
  `magnitude >= along`), applied to the speed `GNSS/sAcc` qualifies.

The file includes no Eigen or GTSAM header (audit `solver-confinement`), so
both eigenvalues are closed forms in plain arithmetic (Decision 1). A square
root's argument that round-off has made negative reads as zero and the
cosine's argument is clamped to [-1, 1]; the output is then finite and
non-negative for every finite input, which phase 1's contract makes every
filled entry. Nothing is filtered or widened. Helper functions and their
names are the implementer's.

`registerFusionCalculations` registers them after
`registerHorizontalAcceleration` and before `registerSystemTime`, in the
order `hAcc`, `vAcc`, `sAcc` (overview decision), which leaves both pins of
`derivedRegistrationShape` true.

The header's contract gains three bullets after the `builtin.fusion.accH`
bullet, in the registered order, each naming the output, its unit, its
inputs and its definition in the words above (the `sAcc` bullet says the
rule is the horizontal acceleration accuracy's). The sentence "The derived
values' inputs exist only once the fit has published ..." already covers
them. Nothing else in the header changes.

### The tests' helpers (`tests/fusion/fusionsessions.h`, `.cpp`)

`fusionNames()` gains `Fusion/hAcc`, `Fusion/vAcc`, `Fusion/sAcc` beside
`Fusion/accH`, and its header comment names them. `syntheticFitSession()`,
`fusionMeasurementNames()` and `fusionPlots()` are not touched.

### The audit (`tests/audit/cleanup_audit.cmake`)

In group `accuracy-channels`, after the rule "the accuracy channels are
named in the registration and the plot rows", one `expect_only` for the
derived names: regex `(kSensor|"Fusion"), *"(hAcc|vAcc|sAcc)"`, allowed in
`^src/(fusion/fusionregistration\.cpp|mainwindow\.cpp)$`, over `src`, with an
"Allow:" comment. The bare strings are also the GNSS names
(`gnsscalculations.cpp`, the "GNSS (Basic)" rows and Plots-menu items of
`mainwindow.cpp`, the `kFitInputs` table of the registration itself), so the
rule matches the sensor-qualified form alone: `kSensor, "hAcc"` in the
registration's outputs, `"Fusion", "hAcc"` in a plot row. `mainwindow.cpp`
is allowed now so that phase 3's rows satisfy the rule as written (Decision
3). The group's head comment names the three beside the twelve and the four;
its item list gains 1709. The rule is planted once (a `"Fusion", "hAcc"` in a
file outside the allowed pair, caught, then removed), as every rule was.

Acceptance traceability: the completeness loop `foreach(item RANGE 1701
1706)` becomes `1701 1709`; the range comment's clause for 1707-1709 (the
three calculations, their tests, their documents and audit rule) replaces
what phase 1 wrote for this phase's items. The range check and the header
bullet already name 1701-1717.

### The acceptance map and the test guide

`tests/acceptance_map.txt`: items 1707-1709 appended to the 1701 block in
the form of its first six, with the lines under Tests. No earlier item is
restated: none counts the derived calculations.

`tests/README.md`: section 9.18's intro says 1707 is the specification's
section 4, 1708 section 8's third bullet and 1709 section 7 with the audit
bullet's second clause, and its table gains the three rows; appendix R gains
items 7-9 and its closing sentence says 10-17 follow with the plots. The
test table: `tst_fusion_derived`'s row names the fused accuracies'
registration shape, known answers and waiting on the fit;
`tst_fusion_session`'s row counts seventeen registrations and names the
accuracies among what a read never runs the fit for and what a request
brings. Section 10's `accuracy-channels` paragraph gains the derived-names
rule in the words of the four's. Sections 11 and 12 are not touched.

### The documents (the same change)

- `docs/SENSOR_FUSION.md` section 4: the table "These values are derived from
  the outputs on demand" gains `hAcc`, `vAcc` and `sAcc` rows after `accH`,
  with unit and definition. Where phase 1's "Accuracy" paragraph says the
  composed position and velocity covariance grows inside a hole, one
  sentence may add that `hAcc`, `vAcc` and `sAcc` are the published
  (widened) standard deviations read on demand; the audit counts this document's sentences by
  line, so it stays on one line. Sections 2 and 8 are not this phase's.
- `docs/CALCULATIONS.md` section 17: "Fourteen calculations are registered"
  becomes seventeen, with three rows after `builtin.fusion.accH` in the
  table's form; the "Derived values" paragraph gains the three definitions in
  the registration's words, says each is unavailable when an input is empty
  or the inputs differ in length and that none widens (the blocks are
  widened); "The eight derived calculations" becomes eleven and names the
  three. The "Plots" paragraph is phase 3's.
- `src/fusion/fusionregistration.h`: as under The calculations.

### What must not change

Every file under `src/fusion/` but the registration pair (a diff there
rejects the phase); `kFitOutputs`, `kFitInputs`, `Fusion::Algorithm`, the
goldens, `gnsscalculations.cpp`, `mainwindow.cpp`, `fusionPlots()`,
`tst_fusion_rows`, `tst_fusion_store`, every count of fifteen fusion plots
and of four accuracy rows, the `naming` group. GTSAM and Eigen stay out of
the registration pair; `<cmath>` and `<algorithm>` are what the arithmetic
needs.

## Interfaces

Provided to phase 3 (names are contracts):

- `builtin.fusion.hAcc`, `builtin.fusion.vAcc`, `builtin.fusion.sAcc`:
  OnDemand under `Fusion`, no title, no result version, registered in that
  order between `builtin.fusion.accH` and `builtin.fusion.systemTime`, with
  the inputs, outputs and definitions of the table and list above. Each
  output has one candidate and `explicitDependencies(output) == {"builtin.fusion.fit"}`,
  so a row over it is explicit-backed by the fit alone. No unit text is
  published, as for every fusion value; the units (m, m, m/s) are the rows'.
- Unavailable when any input is empty (no fit, a fit whose covariance
  failed) or the inputs differ in length; otherwise one finite, non-negative
  value per sample of `Fusion/_time`.
- `fusionNames()` lists the three; every test iterating it covers them.
- The `accuracy-channels` rule for the derived names with `mainwindow.cpp`
  allowed: phase 3 adds rows and changes nothing in the rule.
- Items 1707-1709 complete, the completeness loop at 1701-1709, section 9.18
  and appendix R carrying items 1-9.

Consumed from phase 1: what Dependencies lists.

## Acceptance criteria

1. **Registration shape** (section 4; item 1707). `registry.instance()` of
   each id exists, OnDemand, empty title and result version, inputs exactly
   the table's lists in order, one output each, one candidate per output,
   `explicitDependencies(output) == {"builtin.fusion.fit"}`;
   `registeredIds()` has `velH, vel, accH, hAcc, vAcc, sAcc` after the fit
   and `z, accAlongTrack, accCrossTrack` after the system time;
   `registrationShape`'s literal tail is the seventeen ids.
2. **Known answers and the `sAcc` rule** (section 4; section 8, third
   bullet; item 1708). On `syntheticFitSession` with the blocks of Decision
   2 stored as data, every sample of the three equals the stated answer
   (exactly where Decision 2 says exact, within 1e-12 where it says so): a
   velocity at or above its along-track sigma gives that sigma and not the
   largest eigenvalue, one below it and a zero velocity give the largest, a
   full block exercises the 3x3 form on both branches, the zero block gives
   0; `runCount(fit) == 0`, `undeclaredReadCount() == 0`.
3. **Unavailable** (section 4; item 1708). On a synthetic session with the
   velocity and no `velCov`, `Fusion/sAcc` is empty and the fit does not run;
   with `posCovDD` alone, `vAcc` is served and `hAcc` is empty; a `velCov`
   or `posCov` entry one sample short makes the calculation that reads it
   unavailable and leaves the others as they were; on a fixture session
   without a fit the three are `Blocked` by the fit alone, merely
   uncomputed, empty, `runCount(fit) == 0`.
4. **With the fit** (section 4; item 1708). On the four success fixtures of
   `requestRunsOnceAndPublishesTogether` the three have the length of
   `Fusion/_time` after one request, each ran once, `vAcc[i]` is
   `std::sqrt(posCovDD[i])` bit for bit, `hAcc[i]` and `sAcc[i]` are finite
   and non-negative; `readsNeverRunTheFit` holds over 42 names with strides
   coprime with 42.
5. **The audit** (section 8, audit bullet, second clause; item 1709). The
   derived-names rule exists as specified, planted once, green with
   `mainwindow.cpp` allowed; the completeness loop covers 1701-1709;
   `audit_cleanup` is green.
6. **The documents** (section 7; item 1709). The derived table of
   `SENSOR_FUSION.md` section 4 has the three rows; `CALCULATIONS.md`
   section 17 says seventeen calculations with the three rows, the three
   definitions and eleven derived calculations; the header's contract has
   the three bullets; `tests/README.md` carries the rows, 9.18 and R as
   stated; the suite is green in `build-agent/` (Release, sequentially).
7. **Nothing else moved** (section 2, second principle). The diff touches no
   file under `src/fusion/` but the registration pair, nothing of
   `mainwindow.cpp`, `fusionPlots()` or the goldens; the `naming` count of
   fifteen and the four accuracy rows hold.

## Tests

Function names are proposals; the map and the guide cite whatever exists.

### `tst_fusion_derived`

- `derivedRegistrationShape` (criterion 1): the `expected[]` table gains
  the three with their input lists; the pin after the fit becomes
  `ids.mid(fit + 1, 6) == {velH, vel, accH, hAcc, vAcc, sAcc}`; the pin
  after the system time stays. Constants `kHAcc`, `kVAcc`, `kSAcc` beside
  `kAccH`.
- `derivedValuesWaitOnTheFit` (criterion 3): `"hAcc"`, `"vAcc"`, `"sAcc"`
  join its names.
- **New** `fusedAccuraciesKnownAnswers` (criteria 2, 3): the blocks of
  Decision 2 as `syntheticFitSession` channels (a helper that spreads six
  entries over the six names, as `velocityChannels()` does for three), the
  stated answers, the unavailable cases, `runCount(fit) == 0`,
  `undeclaredReadCount() == 0`. One function with blocks or a `_data` table
  is the implementer's choice. The head comment names the fused accuracies.

### `tst_fusion_session`

- `registrationShape` (criterion 1): the literal tail becomes seventeen
  (`.mid(size - 17)`), `kHAcc`, `kVAcc`, `kSAcc` between `kAccH` and
  `kSystemTime`; the head comment counts them.
- `readsNeverRunTheFit` (criterion 4): the count literal becomes 42 and the
  strides numbers coprime with 42 (4, 7 and 8 are not; 5, 11 and 13 are);
  `runCount` of one of the three asserted 0 beside `kAccH`'s.
- `requestRunsOnceAndPublishesTogether` (criterion 4): the three read after
  the request, their lengths, `runCount` 1 each, `vAcc` against
  `std::sqrt(posCovDD)` with `sameBits`, `hAcc` and `sAcc` finite and
  non-negative.

### Audit, map and guide

The rule and the loop as under What changes. The map's new items, their
statements the contracts under Interfaces in the words of the specification:

- **1707** (4) the three calculations: ids, outputs with units, inputs in
  order, definitions (the `sAcc` rule named as the horizontal acceleration
  accuracy's), on demand under `Fusion`, no title, no result version,
  registered after `accH` and before the system time in that order, one
  candidate each with the fit alone behind it; unavailable when an input is
  absent or the inputs differ in length, finite and non-negative otherwise;
  no widening of their own.
  Lines: `tst_fusion_derived derivedRegistrationShape`,
  `fusedAccuraciesKnownAnswers`; `tst_fusion_session registrationShape`.
- **1708** (8, third bullet) test: known answers on chosen blocks (a
  diagonal block, a rotated one whose larger eigenvalue is exact, a velocity
  along one axis and one below its standard deviation, a full 3x3 block);
  unavailable without the blocks and on unequal lengths; waiting on the fit
  and never starting it; appearing with the fit.
  Lines: `tst_fusion_derived fusedAccuraciesKnownAnswers`,
  `derivedValuesWaitOnTheFit`; `tst_fusion_session
  requestRunsOnceAndPublishesTogether`, `readsNeverRunTheFit`.
- **1709** (7; 8, audit bullet, second clause) the documents and the audit:
  the derived table of `SENSOR_FUSION.md` section 4, the registration table
  and derived values of `CALCULATIONS.md` section 17 (seventeen
  calculations, eleven derived), the contract of
  `registerFusionCalculations`; the `accuracy-channels` rule that the three
  derived names, sensor-qualified, are spelled in `src` in the registration
  and the plot rows alone, planted once; the loop at 1701-1709; the audit
  and the map check green.
  Lines: `audit accuracy-channels` (a floor for the documents; the reviewer
  reads them against What changes).

## Decisions

1. **The closed forms and the clamps.** The 2x2 larger eigenvalue is the
   expression `accelerationAccuracy()` already spells, so the two rules are
   visibly one; its discriminant is a sum of squares and the result is at
   least the larger diagonal entry, so for a non-negative diagonal nothing
   under its square roots is negative. The 3x3 largest eigenvalue is the
   trigonometric form: `q = tr/3`, `p1 = NE^2 + ND^2 + ED^2` (zero means a
   diagonal block, whose largest entry is the answer),
   `p2 = (NN-q)^2 + (EE-q)^2 + (DD-q)^2 + 2 p1`, `p = sqrt(p2/6)`,
   `B = (A - qI)/p`, `r = det(B)/2` clamped to [-1, 1],
   `lambda = q + 2 p cos(acos(r)/3)`. Round-off can leave `r` a hair past 1
   and a quadratic form a hair below 0, so `r` is clamped and every square
   root's argument is `std::max(0.0, x)`; that is the whole treatment, and
   the comment says so. An iterative method or a library would be
   generality nothing uses.
2. **The known-answer blocks** (entries `NN, NE, ND, EE, ED, DD` for a 3x3,
   `NN, NE, EE` for the horizontal block), chosen so the eigenvalues are
   exact and, where the arithmetic is dyadic, the computed value is exact:
   - `hAcc`: diagonal `(9, 0, 4)` and `(4, 0, 9)`, eigenvalues 9 and 4,
     answer 3 whichever axis holds it; rotated `(8, 2, 5)`, eigenvalues 9
     and 4, answer 3 (`6.5 + sqrt(2.25 + 4) = 9`); `(5, 4, 5)` and
     `(5, -4, 5)`, eigenvalues 9 and 1, answer 3; the zero block, 0. Exact.
   - `vAcc`: `DD = 6.25` gives 2.5, `DD = 0` gives 0. Exact.
   - `sAcc`, block `diag(4, 9, 16)`: velocity `(10, 0, 0)` gives 2 (along,
     not the largest 4); `(0, 3, 0)` gives 3 (speed equal to its sigma takes
     the along branch); `(0, 0, -10)` gives 4; `(1, 0, 0)` gives 4 (below its
     sigma 2); `(0, 0, 0)` gives 4. Exact.
   - `sAcc`, full block `(5, 2, 2, 5, 2, 5)`, eigenvalues 9, 3, 3: velocity
     `(3, 3, 3)` gives 3 (quadratic form `243/27 = 9`, speed
     `3*sqrt(3) >= 3`); velocity `(1, 0, 0)` gives 3 by the largest
     eigenvalue, exact on the 3x3 form (`q = 5`, `p = 2`, `det(B) = 2`,
     `r = 1`, `acos(1) = 0`).
   - `sAcc`, distinct eigenvalues `(5, 4, 0, 5, 0, 2)`, eigenvalues 9, 2, 1:
     zero velocity gives 3 within 1e-12 (`q = 4`, `p = sqrt(19/3)`, not
     dyadic).
3. **The audit rule matches the sensor-qualified form and allows
   `mainwindow.cpp` now.** The bare names are the receiver's and are spelled
   legitimately in three places; the qualified form is spelled nowhere in
   `src` today, so the rule is true at the phase's end with the registration
   alone hitting it, and phase 3's rows need no edit to it. `expect_only`
   passes on zero hits, which is why the planting step is required.
4. **No "the fused value is the GNSS definition" test.** `GNSS/hAcc`,
   `vAcc` and `sAcc` are source measurements, not calculations, so there is
   no GNSS definition to compare against, and the rule `sAcc` repeats lives
   in `fitcovariance.cpp`, which `tst_fusion_derived` may not include (audit
   `solver-confinement`, `fusion-tooling`). The known answers hold the rule;
   `requestRunsOnceAndPublishesTogether` holds `vAcc` to one correctly
   rounded square root bit for bit on real fits.
5. **The inputs are exactly what each definition reads.** `hAcc` declares
   the three horizontal entries, not six, and `vAcc` the one, so the
   dependency edges are honest and `undeclaredReadCount()` stays 0; `sAcc`
   declares the six and then the three components, the block before the
   vector.
6. **The strides of `readsNeverRunTheFit` change with the count.** Two of
   the current strides are even; a stride sharing a factor with 42 would
   skip names silently.
7. **One sentence in "Accuracy", on one line.** Section 7 wants the hole
   statement to be about the published accuracies; naming the three derived
   values beside the blocks' standard deviations is one line, which the
   audit's line counts of that document allow.

Ready.
