# Phase 1: The fit publishes the covariance blocks

## Purpose

The specification's section 3, the kernel and registration-and-record
bullets of section 8, the parts of section 7 that describe the fit's outputs
and the record, and the first clause of the audit bullet. After this phase
the fit publishes, beside the four accuracies, the upper triangles of the
position and velocity covariance blocks of the published state at every
sample, in the navigation frame, widened once by the squared widening
factor: twelve new measurement outputs, thirty-three with the seventeen of
the state and the four accuracies, thirty-four with the diagnostics
attribute. `Fusion::Algorithm` becomes `batch-temperature-bias-v10`, so that
every record written under `v9` is stale at load.

It is one phase because the kernel's arrays, the registration's output
table, the goldens, the runner, the record tests and the documents that
count the fit's channels cannot be green apart (overview, "Decisions and
constraints"). The pattern is `PLANS/done/noise-model-part-1-plan/04-accuracy.md`,
"Output, registration, record" and "Goldens", which added the four
accuracies through the same files.

The fit's arithmetic does not change: the solution, the seventeen state
channels, the four accuracies and the diagnostics are bit for bit what they
were, and the re-capture note records it. Sections 4, 5 and 6 of the
specification are phases 2 and 3; this phase adds no calculation and no
plot row.

## Dependencies

Depends on nothing: it starts from `9a3ccc0`, where `IntervalComposition`
(`src/fusion/trajectoryreconstruction.cpp`) forms the attitude rows of `J_j`
only (`attitudeBiasScale(j)`, a `3 x 33` Jacobian from the top three rows of
`byStart`, `byFitted` through `K`, `byBias`, `byScale` and the retraction),
the publication loop of `reconstructAtImuRate()` rotates the attitude block
with `R*joint.block<3, 3>(0, 0)*R.transpose()`, `fillOutputChannels()`
widens the four by `w` under `if (trajectory.headingAcc.empty()) return;`,
`kFitOutputs` holds twenty-one names, `fusionChannelNames()` twenty-one
columns, `Fusion::Algorithm` is `batch-temperature-bias-v9` (pinned by five
audit rules, carried by the goldens) and the acceptance map's last range is
1601-1612.

Blocks phase 2 (its calculations declare the twelve as inputs) and, through
it, phase 3.

## What changes

### The composition: `src/fusion/trajectoryreconstruction.h` / `.cpp`

Section 3, first bullet. The composer forms the position and velocity rows
of `J_j` as it forms the attitude rows: the same formula with rows 3-5 and
6-8 of the nine-row Jacobians in place of rows 0-2 (`byStart`, `byBias`,
`byScale`, the retraction `M_j` in `K_j` and in the conditional `C_j`,
`byFitted` through `K_j`), the same `N` on the fix states and the same
`dT_k` slope column. Per edge the result is the position block and the
velocity block of `Sigma_j = J_j Sigma_z J_j^T + C_j`, each `3 x 3`,
symmetrized as the attitude block is.

The frames, which the overview fixes: `Sigma_j` is in the forward state's
tangent, a `NavState` tangent, where the translation and the velocity are
both in the body frame (`N` is what takes the graph's `V(k)`, NED, into that
tangent on the input side). Each block is turned into the navigation frame
exactly as the attitude block is, in the publication loop:
`R*block*R.transpose()` with `R` the published attitude of the sample. A
sample on a fix has `P_0 = 0`, `Psi_0 = I`: its blocks are the fix's own
marginal (the Pose3 tangent's translation block, turned; `V(k)`'s block,
which `N` took into the body and `R` takes back to NED).

`ImuRateTrajectory` gains two members beside `attitudeCovariance`, one
`std::vector<gtsam::Matrix3>` each for the position and the velocity block,
navigation frame, unwidened, "with a computed covariance only", aligned with
`time`, documented in the struct's comment; their names are the
implementer's. The comment of `reconstructAtImuRate()` replaces "Only the
attitude rows are formed" with what is formed and in which frame the blocks
are published.

**Bits must not change.** The attitude block and the four accuracies stay
bit for bit. Leave the attitude's expressions as written and compute the new
rows in expressions of their own: a nine-row product whose top rows are then
taken is not promised to give the attitude block the same bits. The
pre-capture check is the proof.

### The output: `fusion.h`, `fusionoutput.h` / `.cpp`, `fusion.cpp`

- `Result` gains twelve `QVector<double>` members after `accDAcc`, in this
  order and with these names: `posCovNN`, `posCovNE`, `posCovND`,
  `posCovEE`, `posCovED`, `posCovDD`, `velCovNN`, `velCovNE`, `velCovND`,
  `velCovEE`, `velCovED`, `velCovDD`. Their comment states the contract: the
  upper triangles of the position (m^2) and velocity (m^2/s^2) covariance
  blocks of the published state in the navigation frame (north, east, down),
  widened by the squared widening factor, aligned with `time`; filled
  exactly when the four accuracies are, empty otherwise; the diagnostics'
  `accuracy` account unchanged.
- `Fusion::Algorithm` becomes `batch-temperature-bias-v10`.
- `fillOutputChannels()`: after the four, inside the same guard (the blocks
  are filled under the same `composition` as `headingAcc`), each of the six
  entries of each block times `w*w`, once, uncapped: the one place the
  widening is applied (section 3, second bullet; phase 2 applies none). The
  function's and the file's comments count thirty-three. `accuracyObject()`,
  `successDiagnostics()` and `assembleSuccess()` do not change.

### The registration and the record: `fusionregistration.cpp` / `.h`

- `kFitOutputs` gains the twelve rows after `accDAcc` (`{ "posCovNN",
  &Fusion::Result::posCovNN }` ...), in the order above; its comment counts
  thirty-three; `publish()`'s comment says sixteen stay unset without the
  covariance. `fitOutputs()`, `fitOutputChannels()` and `registerFit()`
  follow from the table.
- The header: `fitOutputChannels()`'s comment names the thirty-three in
  order; the contract of `registerFusionCalculations()` says thirty-four
  outputs published together, names the twelve with their units, and says a
  success whose covariance could not be computed publishes the seventeen and
  leaves the sixteen accuracy channels unavailable.
- The record needs no code change: `calculationrecord.cpp` writes the
  outputs that are set, and a record under `v9` is stale by its result
  version as any stale stamp is. This phase adds the test.

### The goldens and the tooling

- `tests/fusion/fusiongolden.h` / `.cpp`: `fusionChannelNames()` and
  `fusionChannel()` gain the twelve after `accDAcc`; the comments count
  thirty-three; `portableFloor()` gives the twelve `kPortableAbsolute`
  explicitly, and the header's tolerance comment says why (Decision 2).
  `isExactKey()` is unchanged: no diagnostics key is added.
- The re-capture, by `tests/README.md` section 11's procedure in
  `build-agent/`, preceded by the pre-capture check of the noise model's
  phase 4: against HEAD's goldens, the twenty-one existing columns of the
  four channels files byte-identical, every diagnostics key but `algorithm`
  identical, the ten rejections changed in `algorithm` alone, the `_time`
  axes identical (step 3). What changes: the column line and twelve columns
  of the four channels files, `algorithm` in all fourteen `.json`,
  `capture.json`. The phase report records the pre-capture result; section
  11 gains a dated history paragraph ("The capture of 2026-10-0x (the fused
  accuracies, phase 1)") in the form of those before it.
  `fusion_golden_capture.cpp` needs no change.
- `tests/fusion_runner.cpp`: the file comment, `kUsage` and `outputCsv()`'s
  comment say "the thirty-three, or the seventeen without the accuracy".
- `tests/fusion/fusionsessions.h` / `.cpp`: `fusionMeasurementNames()` lists
  the thirty-three in output order; the unit table of
  `syntheticFitSession()` gains the twelve with `m^2` and `m^2/s^2`, so its
  `Q_ASSERT` of equal sizes holds; the comments that count twenty-one count
  thirty-three. `fusionNames()` and `goldenDifference()` iterate the names.
  `fusionPlots()` is not touched.

### What must not change

- `factorgraphfit.*`, `imuintegration.*`, `fitcovariance.*`,
  `initializer.*`, `scaledimufactor.*`, `sensornoise.*`, `inputadapter.*`:
  a diff in any rejects the phase (overview). `fitcovariance.h` is the
  vocabulary, read only.
- The reconstruction's states and acceleration, the attitude block, the
  four accuracies, the widening, every diagnostics key, the progress texts,
  the cancellation boundaries, `fitInputs()`, the record format, the derived
  calculations, `src/mainwindow.cpp`, `fusionPlots()` and every count of
  fifteen plots (phase 3's).
- GTSAM and Eigen stay out of `fusion.h` and `fusionregistration.*`.

### Documents (the same change)

Section 7 as it concerns the fit's outputs and the record. The audit counts
sentences of `docs/SENSOR_FUSION.md` by line: keep each on one line.

- `docs/SENSOR_FUSION.md` section 4: "Accuracy" opens by naming the four
  accuracies and the two blocks (the audited one-line sentence stays);
  "The composition at every sample" replaces "Only the attitude rows of
  `J_j` are formed ..." with the attitude, position and velocity rows and
  the frame (turned with the published attitude as the attitude block is; a
  sample on a fix the fix's own marginal); "Widening" says the blocks are
  widened by the factor's square, once; the "Outputs" table gains twelve
  rows with meaning and unit; the paragraph after it says the sixteen
  accuracy channels are absent when the covariance step failed; "Accuracy"
  says the composed position and velocity covariance, before the widening,
  grows inside a hole in the fixes and returns at the next fix, since the
  step chain's share grows there, where the four accuracies do not, and
  that the published accuracy carries the per-window widening as well,
  which can move it either way, so the published figures are recorded
  beside the statement rather than asserted (phase 2 may restate it for the
  derived names). The diagnostics identity line names `v10`.
- Section 7: "installs all thirty-four outputs"; "Stored results" holds the
  sixteen accuracy channels with the state; the stamp is
  `batch-temperature-bias-v10` since the fit publishes the position and
  velocity covariance blocks, which changes the record's shape and no
  number of the fit, `v9` having been the scale factors released from the
  held solution, then the history as it stands.
- Section 8, "Validating the accuracy": the edge-graph and fix-marginal
  agreements gain the blocks' figures; the `bridged_hole` passage becomes a
  statement about the composed blocks the fit now publishes: the unwidened
  position and velocity standard deviations (the root of each block's trace)
  beside the fix before the hole, their largest inside it and their values
  beside the fix after it, the published (widened) figures at the same three
  samples recorded beside them, and the error against the generating
  trajectory in units of the published standard deviation, the measured
  maxima beside the four's ratios; "the growth through a hole is in the
  sample covariance's position and velocity" says the fit now publishes it,
  widened. The staged-scale paragraph's "under the current algorithm string,
  `v9`" says `v9` (the numbers are those of `v10`, which changed only what
  is published). The numbers come from the tests' log lines, as the four's.
- `docs/CALCULATIONS.md` section 17: "the 34 below"; "Outputs of the fit"
  lists the twelve with units, "thirty-four outputs, one table"; "Outcome
  mapping" `Succeeded`: the seventeen, the sixteen accuracy channels when
  the covariance was computed, the diagnostics; the "Plots" paragraph's list
  of outputs without a plot gains the twelve; `v10` since ..., `v9` in the
  history. `docs/DATA_SCHEMA.md` section 11: the `"records"` example says
  `v10`; section 12: thirty-three measurements since the fused accuracies,
  the twelve after the four (the seventeen alone without the covariance),
  same format; "Validity", "Code": `v10` and why, `v9` before it.
- Every comment and `tests/README.md` line found by
  `grep -n "twenty-one\|twenty-two"` over `src`, `tests` and `docs` is
  updated (thirty-three measurements, thirty-four outputs): the test-table
  rows of the six fusion executables named under Tests; section 11's
  channels-file bullet and the algorithm-string bullet of the re-capture
  procedure (`v10` since ..., `v9` was ...); "Fusion sessions"; M10 and M50
  (a header of thirty-three names, the twelve last); section 10's
  `fusion-model` description; the intros and rows of 9.3, 9.4, 9.10, 9.11
  and appendices C, D, J, K as "Acceptance map" says. Section 11's history
  paragraphs keep their counts.

## Interfaces

Provided to phase 2 (names are contracts):

- `Fusion::Result` members `posCovNN` ... `posCovDD`, `velCovNN` ...
  `velCovDD` after `accDAcc`, aligned with `time`, filled exactly when
  `headingAcc` is. `Fusion::Algorithm` is `batch-temperature-bias-v10`.
- `kFitOutputs` and `fitOutputChannels()` list them in that order after
  `accDAcc`, published as `Fusion/posCovNN` ... `Fusion/velCovDD`:
  thirty-three measurement outputs, thirty-four with `_FUSION_DIAGNOSTICS`.
  No unit text is published, as for every output of the fit; the units
  (`m^2`, `m^2/s^2`) are the documents' and the tests' unit table's.
- `fusionChannelNames()` and `fusionMeasurementNames()` list the
  thirty-three in that order; the four success goldens carry the twelve.
- The acceptance range 1701-1717 declared in the audit, items 1701-1706
  complete, section 9.18 and appendix R opened, the map's header counting
  eighteen specifications.

A filled block's six entries describe a symmetric matrix with a finite,
non-negative diagonal, widened by `w*w` with the sample's `w`: a derived
standard deviation needs no widening of its own.

Consumed: nothing beyond the code at `9a3ccc0`.

## Acceptance criteria

1. **Members and names** (section 3, table; items 1701, 1704). `Result` has
   the twelve after `accDAcc`; `kFitOutputs`, `fitOutputChannels()`,
   `fusionChannelNames()` and `fusionMeasurementNames()` list the
   thirty-three in the one order; `registrationShape` holds the thirty-four
   outputs as a literal list and `resultVersion` to `v10`.
2. **The blocks are the composed sample covariance** (section 3, first
   bullet; section 8, first bullet). On `coarse_maneuver` at every tenth
   sample the kernel's unwidened position block equals
   `R_sol Sigma_X(j)[3:6, 3:6] R_sol^T` and the velocity block equals
   `Sigma_V(j)` of the edge graph's joint marginals (the Pose3 tangent's
   translation is body-frame, `V(j)` is NED) within 1e-5 relative
   (Frobenius). On `sacc_anchor` every sample on a fix has
   `R Sigma_k[3:6, 3:6] R^T` and `Sigma_k[6:9, 6:9]` of the covariance step
   within 1e-9.
3. **Widened once by the square** (section 3, second bullet). On the
   understated stretch of `scale_recording` every published entry equals
   `w*w` times the kernel's unwidened entry, bit for bit, with the sample's
   `w`.
4. **Filled exactly when the four are; symmetric, finite, non-negative
   diagonal** (section 3, third bullet). On `kAccuracyFixtures` the twelve
   have the length of `time`, every entry finite, the diagonal entries
   non-negative, each kernel block equal to its transpose; the forced
   covariance failure leaves the twelve empty with the four and everything
   else bit-identical; every failure path's result has the twelve empty.
5. **The pipeline publishes the composition** (section 3; item 1701).
   `imuRateIsWhatTheFitPublishes` holds all thirty-three channels of
   `runPipeline()` to the test's own composition and widening, bit for bit.
6. **`bridged_hole`** (section 8, first bullet; the overview's bound). At the
   260 samples inside the hole, each axis's position error against the
   generating trajectory (the polynomials in `coarseManeuver()`'s comment)
   over the published standard deviation of that axis (`sqrt(posCovNN)`,
   ...), and likewise for the velocity, is below 3; the largest ratio of
   each is logged and recorded in section 8. The trace of the kernel's
   unwidened position block and of the unwidened velocity block (the two
   `ImuRateTrajectory` members, through `fixtureTrajectory()`) is larger at
   its largest sample inside the hole than at the samples beside the two
   fixes around it (`samplesBeside`), and at the sample beside the fix after
   the hole it is below that largest: asserted. The traces of the published
   blocks at the same three samples are logged and recorded, not asserted
   (Decision 4).
7. **The fit is unchanged** (section 3, fourth bullet; item 1702). The
   pre-capture check holds; after the capture the goldens hold thirty-three
   columns and the golden tests pass in both modes; no expected number for
   the seventeen, the four or the diagnostics changes in any test; the
   kernel files under "What must not change" have no diff.
8. **The record and the tooling** (section 8, second bullet; item 1704). A
   fit stored and restored after a restart has the thirty-three bit for bit;
   a record rewritten to `batch-temperature-bias-v9` is deleted as stale at
   the next load and the fit runs once more, its new record stamped `v10`;
   the stored success rewritten without the sixteen restores the seventeen
   and leaves the sixteen unavailable; a job publishes thirty-three aligned
   measurements with the diagnostics; `fitOutputChannels()` and the runner's
   CSV header are the thirty-three in the goldens' order.
9. **The audit** (section 8, audit bullet, first clause; item 1706). The
    `accuracy-channels` group has an `expect_only` of the twelve quoted names
    allowed in `src/fusion/fusionregistration.cpp` alone, planted once, and
    the stale-count rule below; the five rules pinning
    `batch-temperature-bias-v9` pin `v10` with unchanged counts; the range
    check accepts 1701-1717, the completeness loop covers 1701-1706;
    `audit_cleanup` is green.
10. **The documents** (section 7; item 1705) carry what "Documents" lists;
    the grep of the last bullet there finds no current count of twenty-one
    or twenty-two; the suite is green in `build-agent/` (Release,
    sequentially).

## Tests

### `tst_fusion_kernel`

- `sampleCovarianceMatchesTheEdgeGraph` (criterion 2): two more quantities
  beside the five, the position block against
  `R_sol * joint.block<3, 3>(3, 3) * R_sol^T` and the velocity block against
  `joint.block<3, 3>(6, 6)` of `jointOf(marginals, {X(j), V(j), B(0), S(0)},
  solution)`, within 1e-5; the log line gains their worst.
- `sampleOnAFixHasTheFixMarginal` (criterion 2): the two blocks against
  `c.node[k]`'s translation block (turned with `R`) and velocity block (not
  turned), within 1e-9.
- `wideningGrowsWithAnUnderstatedSigma` (criterion 3): the twelve published
  entries equal `w*w` times the unwidened entries, bit for bit, as the four
  are held to `w` times theirs.
- `accuraciesFiniteAndPositive` (criterion 4): the twelve channels' length,
  finiteness, non-negative diagonal, and the kernel blocks' symmetry
  (`fixtureTrajectory(name)`), minima and maxima logged.
- `covarianceFailureLeavesTheFitAsItIs` and `allChannelsEmpty` (criterion
  4): the twelve empty with the four; `names.mid(0, 17)` stays.
- `imuRateIsWhatTheFitPublishes` and `releaseFailureFallsBackToTheHeldFit`
  (criterion 5): `fusionChannelNames().size()` is 33; both loop over every
  name.
- `bridgedHoleFollowsTheTruth` (criterion 6): the position and velocity
  truth from the fixture's polynomials at each sample inside the hole, the
  per-axis ratios against the published standard deviations, their maxima
  logged and asserted below 3; the traces of the kernel's unwidened blocks
  at `before`, at their largest inside and at `after`, logged and asserted
  as criterion 6 says, and the published blocks' traces at the same three
  samples logged beside them. The seam measurements through
  `reconstructInterval()` stay; the head comment says the growth is now
  published, widened, and why the assertion is on the composition.
- The three `batch-temperature-bias-v9` literals in diagnostics assertions
  become `v10`; the head comment names the blocks among the accuracy's
  subjects.

### Other executables

- `tst_fusion_golden`: `comparatorHoldsItsBounds` pins the twelve's floor
  (one position and one velocity column pass at 9e-8 and fail at 2e-7, as
  `accHAcc` does); `successFixturesMatchGolden`,
  `rejectionFixturesMatchGolden` and `channelsWriterIsTheInverseOfTheLoader`
  run over the thirty-three through `fusionChannelNames()`.
- `tst_fusion_runner`: `outputTableMatchesGolden` (33),
  `successMatchesDirectRun` (header 33), the comments.
- `tst_fusion_session`: `registrationShape` (the literal list of thirty-four,
  `resultVersion` `v10`, the comment); `restoredFitIsIndistinguishable`
  (`v10`); `requestRunsOnceAndPublishesTogether` and `readsNeverRunTheFit`
  cover the twelve through the name lists.
- `tst_fusion_store`: `kAccuracies` becomes the sixteen (the channels a
  success without the covariance leaves unset); `kSolverFailureDiagnostics`
  says `v10`; the head comment counts thirty-three;
  `restoredAfterRestartIsBitIdentical` and
  `restoredFitWithoutAccuracyDrawsTheRest` (compared count still 17) need
  no other change. **Added** `recordUnderPreviousAlgorithmIsComputedAgainOnce`
  (criterion 8, Decision 3): `coarse_linear` fitted through the Roll row,
  unloaded by the cache capacity as `codeStampChangeDropsRecordOnLoad` does,
  its record rewritten with `resultVersion` `batch-temperature-bias-v9`,
  shown again: the record deleted as stale (`staleRecordsDeleted` 1), one
  fit started by the still checked row and run to its end
  (`waitDemandIdle`), `runCount(kFit)` 1, a new record whose
  `resultVersion` is `Fusion::Algorithm`, the channels the golden's, further
  spins running nothing. `codeStampChangeDropsRecordOnLoad` keeps its `v5`
  row.
- `tst_fusion_jobs`: `jobPublishesAllOutputsTogether` (33, the comment); the
  two index stamp literals say `v10`.
- `tst_fusion_derived` and `tst_fusion_rows` need no change.

### Audit (`tests/audit/cleanup_audit.cmake`)

- Group `accuracy-channels`: an `expect_only` of
  `"(posCov(NN|NE|ND|EE|ED|DD)|velCov(NN|NE|ND|EE|ED|DD))"` in `src`,
  allowed in `^src/fusion/fusionregistration\.cpp$` alone, with an "Allow:"
  comment (the kernel holds them as `Result` members, unquoted; phase 2's
  inputs are the quoted form in the registration, which the rule allows; no
  plot row names them). Planted once. The group's head comment names the
  twelve.
- Group `accuracy-channels`: an `expect_none` refusing a stale count of the
  fit's channels, `twenty-one (measurement|channel)|twenty-two outputs`, over
  `docs` and `README.md` (`tests/README.md` keeps its history), as `naming`
  refuses the old plot counts. Planted once.
- The five rules pinning the algorithm string change their literal to
  `batch-temperature-bias-v10`, counts unchanged: "one authority: the fusion
  algorithm string" (count 1 in `src`, `fusion.h` only), "the fusion
  document names the algorithm string" (2), "the schema document ..." (2),
  "the calculations document ..." (1); the comment "The goldens say
  batch-temperature-bias-v9" says `v10`. The documents' history mentions use
  the short form `v9`, which no rule counts.
- Group `naming`: the rule "the removed fusion plots stay out of the
  registry" lists the fit's channels without a row; the twelve join its
  alternation and its comment says so (Decision 6). The count of fifteen
  plots is untouched.
- Acceptance traceability: a header bullet for "the fused position and
  speed accuracy: the fit publishes the position and velocity covariance
  blocks, the derived accuracies, the three rows (items 1701-1717)"; the
  range comment names 1701-1717 (the twelve outputs, `v10`, the kernel
  tests, the registration and the record, the documents, the audit rule,
  then phases 2 and 3's items); the range check accepts 1701-1717 and its
  violation text lists it; a completeness loop `foreach(item RANGE 1701
  1706)` (phase 2 extends it to 1709, phase 3 to 1717).
- `tests/README.md` section 10: the `accuracy-channels` bullet names the two
  new rules.

### Acceptance map and the test guide

`tests/acceptance_map.txt`: the header says "Eighteen specifications,
eighteen item ranges" and gains the entry for 1701-1717 ("fused position and
speed accuracy: the covariance blocks, the derived accuracies, the plots;
item = 1700 + item number of appendix R"); the entries of 201-247, 301-350
and 901-940 say they are amended by the specification of 1701-1717 (items
230 and 235; 337; 912). The comment lines of 230, 235, 337 and 912 are
restated "(as amended)" with the thirty-three (the runner writes the
thirty-three or the seventeen; the result contract gains the twelve,
thirty-four outputs; a restored fit has thirty-three channels; the fit
publishes the thirty-three channels of section 4), and their rows in 9.3,
9.4, 9.10 and appendices C, D, J likewise; 1033, 1042 and 1044, whose
evidence alone counts, have the evidence text updated in 9.11. Michael may
confine the restatement.

The block of 1701-1706 at the end, in the form of 1601-1612:

- **1701** (3) the twelve outputs: `Fusion/posCovNN`, `posCovNE`, `posCovND`,
  `posCovEE`, `posCovED`, `posCovDD` (m^2) and `velCovNN`, `velCovNE`,
  `velCovND`, `velCovEE`, `velCovED`, `velCovDD` (m^2/s^2), measurement
  outputs of the fit after the four accuracies, aligned with `Fusion/_time`:
  the upper triangles of the position and velocity blocks of the composed
  sample covariance, turned from the reconstruction's tangent into the
  navigation frame with the published attitude as the attitude block is, a
  sample on a fix carrying the fix's own marginal; widened once, by the
  square of the sample's widening factor; filled for a success whose
  covariance was computed and empty for every other outcome, exactly when
  the four accuracies are, the diagnostics' `accuracy` account unchanged;
  diagonal finite and non-negative wherever filled.
  Lines: `tst_fusion_kernel sampleCovarianceMatchesTheEdgeGraph`,
  `sampleOnAFixHasTheFixMarginal`, `wideningGrowsWithAnUnderstatedSigma`,
  `accuraciesFiniteAndPositive`, `covarianceFailureLeavesTheFitAsItIs`,
  `imuRateIsWhatTheFitPublishes`; `tst_fusion_session registrationShape`.
- **1702** (3) `v10` and what is unchanged: `Fusion::Algorithm` is
  `batch-temperature-bias-v10`; a record written under `v9` is stale at load
  and computed again once, as a result is needed, shown like any
  computation; nothing else of the record's format changes; the fit's
  inputs, its solution, the seventeen state channels, the four accuracies
  and the diagnostics are unchanged and the goldens' existing channels
  remain bit for bit; neither `Cov(x_j, g)` nor the position-velocity cross
  block is published.
  Lines: `tst_fusion_session registrationShape`,
  `restoredFitIsIndistinguishable`; `tst_fusion_store
  recordUnderPreviousAlgorithmIsComputedAgainOnce`; `tst_fusion_golden
  successFixturesMatchGolden`, `rejectionFixturesMatchGolden`;
  `tst_fusion_kernel fitTraceMatchesGolden`; `audit accuracy-channels`.
- **1703** (8, first bullet) test: the kernel: on the fixtures with a
  computed covariance the published blocks equal the position and velocity
  marginals of a graph with a state at every edge, on a fix the fix's own
  marginal; symmetric with a finite, non-negative diagonal wherever filled,
  empty exactly when the four accuracies are; on `bridged_hole` the
  composed position and velocity covariance before the widening is larger
  inside the hole than beside the fixes around it and returns at the next
  fix, the published figures recorded beside it and not asserted,
  and the error against the generating trajectory in units of the published
  standard deviation is measured, recorded in section 8 of the fusion
  document and held below 3; the seventeen and the four of every golden
  unchanged bit for bit, the goldens gaining the twelve.
  Lines: the six kernel tests of 1701, `bridgedHoleFollowsTheTruth`;
  `tst_fusion_golden successFixturesMatchGolden`,
  `channelsWriterIsTheInverseOfTheLoader`, `comparatorHoldsItsBounds`.
- **1704** (8, second bullet) test: the registration and the record:
  thirty-three measurement outputs in order, thirty-four with the
  diagnostics; the twelve published and stored with the rest and restored
  bit for bit after a restart; a record written under `v9` stale at load and
  the fit run again once; the stored success without the covariance restores
  the seventeen and leaves the sixteen accuracy channels unavailable.
  Lines: `tst_fusion_session registrationShape`,
  `requestRunsOnceAndPublishesTogether`; `tst_fusion_runner
  outputTableMatchesGolden`, `successMatchesDirectRun`; `tst_fusion_store
  restoredAfterRestartIsBitIdentical`,
  `recordUnderPreviousAlgorithmIsComputedAgainOnce`,
  `restoredFitWithoutAccuracyDrawsTheRest`; `tst_fusion_jobs
  jobPublishesAllOutputsTogether`.
- **1705** (7) the documents of the fit's outputs and the record:
  `SENSOR_FUSION.md` section 4 (the rows of `J_j`, the twelve in the outputs
  table, the squared widening, the growth through a hole), section 7
  (thirty-four outputs, `v10`) and section 8 (the validation with its
  measured numbers); `CALCULATIONS.md`'s thirty-four outputs;
  `DATA_SCHEMA.md`'s thirty-three measurements and `v10`; the contract of
  `registerFusionCalculations`; every count of twenty-one or twenty-two
  channels in the documents, the comments and `tests/README.md` updated.
  Lines: `audit accuracy-channels` (a floor; the reviewer reads the rest).
- **1706** (8, audit bullet, first clause) the audit's `accuracy-channels`
  group: the twelve covariance names, quoted, are spelled in `src` in the
  registration's output table alone, the kernel holding them as `Result`
  members; the rule planted once; the range 1701-1717 declared and items
  1701-1706 complete; the audit and the map check green.
  Lines: `audit accuracy-channels`.

`tests/README.md`: section 9.18 "Fused position and speed accuracy (items
1701-1717)" in the form of 9.17 (1701-1702 the specification's section 3,
1703-1704 the first two bullets of its section 8, 1705 its section 7, 1706
the audit bullet's first clause; phases 2 and 3's items named as following),
with the six rows and the restated items; appendix R "The acceptance items of
the fused position and speed accuracy (1701-1717)" in the form of Q, items
1-6 written and a sentence that 7-17 follow with the derived accuracies and
the plots. Section 12 is not touched (M59 is phase 3's).

## Decisions

1. **The kernel's split stays** (overview). The reconstruction composes the
   unwidened blocks in the navigation frame beside `attitudeCovariance`;
   `fillOutputChannels()` widens by `w*w` and splits each into six channels,
   the one place the widening is applied. Phase 2 relies on it.
2. **The twelve take the default golden floor.** `kPortableAbsolute` is 1e-7
   in the solver's unit, from the 1.06e-8 CI has shown on a channel of order
   one. A position variance on the fixtures is of order 0.1 m^2 (sigma
   0.3-0.4 m under `hAcc` 1.5) and a velocity variance of order 5e-3; a
   sigma that moves by the floor moves the variance by `2 s delta`, below
   1e-7 at those magnitudes, so the default plus the relative term fits
   them and a constant of their own would be generality nothing uses.
   `comparatorHoldsItsBounds` pins the choice, the header says why, and the
   first CI run is the measurement; a flip is a reported sample, as the
   four's policy says.
3. **The stale-`v9` record is a sibling test, not a data row.**
   `codeStampChangeDropsRecordOnLoad` ends by showing the fit offered and
   dropped on uncheck; the specification wants the fit run again once and
   its new record seen, a longer body on the fastest fixture. The `v5` row,
   the string users' logbooks hold, stays.
4. **The rise through the hole is asserted on the unwidened composition and
   logged as published.** The four accuracies' test logs where the
   specification's claim was measured false. The composed blocks are
   different: they are the posterior at the sample, and a posterior's
   variance has a local minimum where a fix observes it, so inside a hole it
   exceeds the values beside the fixes around it and returns at the next
   fix whatever the fit's own share; the hole's one IMU factor ties the two
   fix marginals to millimetres, so the step chain's share, (4.2 mm)^2 at
   the last sample inside, is the difference that decides it. The published
   blocks carry `w*w` besides, and `w` is a per-window factor whose window
   changes sample to sample across a 2.6 s hole as fixes enter and leave
   it; a change of a percent in `w` is ten times the composition's rise of
   1.4e-4 relative, so an assertion on the published figures would decide
   on the widening, not on the growth. The trace of each kernel block is the
   quantity asserted; the published traces are logged and recorded. If the
   measurement contradicts the claim on the unwidened blocks, the
   implementer stops and reports the figures rather than weakening the
   assertion: section 7's sentence about the growth depends on it.
5. **One bound for every hole ratio.** The overview allows tightening below
   3 once measured; this document keeps 3, the four's bound, so the hole
   test has one rule. The measured maxima go into section 8 as the four's.
6. **The `naming` rule's channel list gains the twelve.** The rule keeps the
   fit's unplotted channels out of the plot registry; a channel this phase
   adds belongs in it, or the rule's claim is stale. Maintenance of an
   existing rule; the pinned count of fifteen plots is phase 3's.
7. **The audit refuses stale channel counts in the documents.** The overview
   says a stale count is the defect the naming rule exists to catch for the
   plots; the same rule for the channels costs one line, is plantable, and
   gives item 1705 an automated line. `tests/README.md` is excluded because
   section 11's history legitimately counts twenty-one.
8. **No new diagnostics key.** The `accuracy` account is unchanged by the
   specification's own words; `isExactKey()` and `accuracyObject()` are not
   touched, and the goldens' diagnostics change in `algorithm` alone.

Ready with caveats: Decision 4 (the rise asserted on the unwidened blocks)
rests on the posterior argument until the implementation measures it; Decision 2 (the default floor
for the twelve) is unmeasured across compilers until CI runs, as the four's
floors were.
