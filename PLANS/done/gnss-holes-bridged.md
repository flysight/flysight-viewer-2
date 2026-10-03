# GNSS holes bridged by the IMU

Date: 2026-10-02; amended 2026-10-03 after the first implementation's
escalation (the cap, the growth claim, the slow-tail rule) and again after
the second's review (the never-below clause struck, the cap per interval,
the measurement's wording).
Status: specification for planning. Not an implementation plan. Small enough
to be implemented directly by one agent in one phase. It follows
`PLANS/done/sample-continuity.md`, whose continuity rule the kernel reads
(`src/samplecontinuity.h`, implemented in 1cafa7e..af34628).
Baseline: branch `store-requested-calculations` at `61f590a`; the committed
code is authoritative.
Related: `src/fusion/fusion.cpp` (`planFit`, `kGnssOutageSeconds`,
`kGnssOutageMedians`), `src/fusion/fusionsamples.h/.cpp`
(`requireNoGnssOutage`, `fittedWindow`), `src/fusion/initializer.cpp`
(`segmentBounds`), `src/fusion/fusionoutput.cpp` (the input audit, the
algorithm string), `src/fusion/fitcovariance.h` (the widening window),
`tests/fusion/fusionfixtures.cpp` (`reject_gnss_gap`), `tests/tst_fusion_golden.cpp`,
`tests/tst_fusion_kernel.cpp`, `docs/SENSOR_FUSION.md` sections 2, 6, 7 and
8, `docs/COMPUTED_PLOTS.md`, `docs/DATA_SCHEMA.md` section 12,
`tests/acceptance_map.txt`, `tests/README.md` (the fixture table, section 12).

## 1. Motivation

The kernel rejects a recording whose GNSS fixes are more than max(2 s, 5
median intervals) apart, calling it "disconnected". Two recordings of the
reference corpus fail this way, and in both the hole is in the aircraft
during the climb with the IMU running through it: `24-09-07/08-35-48` (unit
00769, 35 minutes) has one hole of 15.6 s ten minutes before exit, and
`24-09-04/13-35-10` (unit 01086, 56 minutes) has one of 13.2 s and six of
2.4 to 5.4 s in the five minutes before exit. The jumps themselves are
continuous in both. Each recording gets no fusion at all.

The trajectory is connected by the IMU, not by the receiver. The graph
already bridges every interval between fixes with one preintegrated IMU
factor whose covariance grows with the interval; a 15 s interval is the same
factor with more samples. A GNSS hole with a continuous IMU is the case the
fusion exists for, and the model carries its own account of what the bridged
stretch is worth.

## 2. Principles

- **Connectedness is the IMU's.** The one disconnection rule is the IMU gap
  rule of the continuity authority. There is no rule on the spacing of
  fixes short of the cap below: the preintegrated covariance prices the
  hole, and the published accuracies report it.
- **The cap is measured, not guessed.** Within a pass the bias and the scale
  enter an IMU factor to first order, and the settle test rebuilds the graph
  at the fitted values; over one factor spanning a long hole that correction
  is poor enough that the passes stop contracting. On the synthetic
  long-hole recording every hole up to 40 s settles in two or
  three passes and 50 s on the fifth; 52, 54 and 58 s cycle and never
  settle; 60 s settles only on the twelfth of thirty passes. So an interval
  between successive fixes longer than 30 s, twice the longest hole in the
  corpus, is not bridged, whether or not the continuity rule calls it a hole
  (the limit is the factor's span, so a uniformly sparse window is bound by
  it too): the recording is rejected with a reason that names the interval
  and the limit. The limit is a property of the single factor, and a later
  specification that places states inside a long interval removes it.
- **Everything is published.** Every IMU sample inside a hole gets its
  reconstructed state and its accuracies. The growth through a hole is in
  the position and velocity of the sample covariance, which the fit carries
  and the reconstruction composes; the four published accuracies (heading,
  tilt, two accelerations) are bounded by global terms, the heading's
  observability and the bias priors, and grow through a hole only where
  those terms allow, as they do on the reference recording and not on the
  synthetic fixture, where inside the hole they move from one neighbouring
  fix's value towards the other's and the accelerations' follow the
  manoeuvre. Nothing is suppressed; the accuracy tells the user what the
  stretch is worth.
- **The fixes around a hole are ordinary fixes.** They carry their stated
  sigmas, which on the corpus are 100 to 180 m after a hole, and the model
  weights them as it weights every fix.
- **A change that alters what a fit returns changes the algorithm string.**
  Two recordings that were rejections become fits, so the string changes and
  every stored result is dropped once.
- **A slow tail is accepted only when every factor kind fits.** The first
  implementation showed a recording whose GNSS factors were satisfied while
  its IMU factors were ignored (IMU normalized RMS 1707, an accelerometer
  scale of -0.058) accepted as a slow tail, because the rule bounded the
  position and velocity misfit alone. The rule bounds all three.

## 3. What changes

- **The outage rule goes; the cap replaces it.** `requireNoGnssOutage` and
  its two constants are deleted. In their place one kernel constant, the
  longest interval between fixes the fit bridges, 30 s, and one check in
  `planFit` after the window's validation: an interval between successive
  fixes of the fitted window longer than it rejects the recording with a
  reason that names the interval's length, to two decimals, and the limit.
  The holes the continuity authority finds serve the diagnostics; the cap
  walks every interval. Below the cap there is no rule on fix spacing. The
  IMU gap rule, read from the continuity authority, is the only
  disconnection.
- **The segment cutter merges any sparse piece.** Today only the final piece
  of the initializer's cut is merged into its predecessor when it is shorter
  than the minimum or holds fewer than three fixes. With holes, a middle
  piece can hold one or two fixes. Any piece with fewer than three fixes is
  merged into the piece before it, or into the piece after it when it is the
  first; a piece that is the whole window is left alone, and the fitted
  window's own rule of three fixes still rejects a recording with fewer. A
  stretch of whole segment lengths without a fix yields no piece, as it
  already does.
- **The fit and the reconstruction are unchanged in kind.** The IMU factor
  spans the hole as it spans any interval; the reconstruction's per-interval
  pass publishes every IMU sample of the interval; the covariance step's
  adjacent-pair marginals and the widening, whose window already extends to
  the two fixes around a sample when none fall inside it, need nothing.
- **The input audit names the holes.** `input` gains `gnss_holes`, an array
  with one object per hole of the fitted window, each `start_s` (seconds
  since the epoch, like `start_s` of the fit) and `length_s`, in time order,
  empty when there is none. A hole is the continuity authority's hole for
  the GNSS axis of the window.
- **The algorithm string becomes `batch-temperature-bias-v8`.** Every stored
  result, fits and rejections alike, is dropped at its recording's next load
  and the recording is fitted again once when something needs it, as the
  documentation of stored results describes.
- **The slow-tail rule bounds the IMU misfit too.** A final pass at its
  iteration limit is accepted as a slow tail only when its mean relative
  decrease over the window is below the bound and the position, velocity
  and IMU normalized RMS are all below `slowTailMaxNrms`; otherwise the
  rule is `iteration limit`, a solver failure, as today. The tuning field
  keeps its name and value.

## 4. Tests

- **The bridged fixture.** `reject_gnss_gap` (`coarse_maneuver` with fixes
  12 to 23 removed, a 2.6 s hole) becomes the success fixture `bridged_hole`:
  it converges, its golden is captured, and the golden suite holds four fits
  and ten rejections. In the kernel tests, the published attitude and
  accelerations at every IMU sample inside the hole lie within three of their
  own published accuracies of the fixture's generating trajectory; the four
  published accuracies inside the hole are logged against their values at
  the published samples nearest the two fixes around it (bounded by global
  terms, they need not grow, and on this fixture do not); and the position
  and velocity parts of the sample covariance, read through the
  reconstruction's per-interval seam, grow through the hole and collapse at
  the fix after it.
- **The long hole, at the cap and above it.** A second synthetic recording
  with `coarse_maneuver`'s kind of motion and a manoeuvre on both sides of a
  30 s hole, the IMU continuous, converges under the production tuning; the
  test logs its iterations per pass, the largest accuracy inside the hole
  and the residuals at the fix after it. The same recording with a hole
  above the cap is rejected with the reason naming the length and the
  limit, and so is a window whose fixes are uniformly more than 30 s apart,
  which the continuity rule calls no hole. The measurement behind the cap
  (every length to 40 s settles, 50 s on the fifth pass, 52 to 58 s cycle,
  60 s only on the twelfth of thirty passes) is recorded in the
  documentation, not re-run by a test.
- **The sparse piece.** A window whose cut yields a middle piece of two fixes
  is fitted, with the piece merged into its predecessor; a window whose first
  piece has two fixes merges it into its successor.
- **The audit.** `gnss_holes` is in the diagnostics of every fit, empty for
  the three unbroken fixtures and one entry of 2.6 s for `bridged_hole`;
  `git grep` finds no outage constant and no `requireNoGnssOutage` in `src`,
  `tests` or `docs`; the cap is one constant, named by the rejection's
  reason and the documentation.
- **The slow tail.** The slow-tail test gains the case where the IMU
  normalized RMS alone is above the bound and the tail is refused; the
  fusion-improvements item that states the rule is restated "(as
  amended)".
- **The reference recordings.** A manual step, in the form of M11 to M14,
  runs the two recordings of section 1 through the runner and records the
  outcome, the stopping rule, the iterations per pass, the objective, the
  quality, `gnss_holes`, and the largest heading and tilt accuracy inside
  the longest hole against the median accuracy of the recording. The numbers
  go in the report as they are. Under the amended slow-tail rule
  `24-09-04/13-35-10` is expected to end `iteration limit`, a solver
  failure: its second segment's fit reaches its limit and the full fit
  starts from that attitude, which is the solver follow-up the lab notes
  name, not this specification's.
- The acceptance map opens a new hundred for this specification's items.
  `audit_cleanup` and the whole suite green, the exact tests included.

## 5. Documentation

`docs/SENSOR_FUSION.md`: section 6 drops the GNSS gap rejection and states
that a hole in the fixes up to the cap is bridged by the IMU, with the
measurement behind the cap and why a single factor imposes it, and that a
longer interval between fixes is rejected naming the limit; the
stopping-rule paragraph
states the slow tail's three bounds; section 2 says what the user sees over
a hole and that the GNSS plots break there while the fusion plots draw
through; section 7 adds
`gnss_holes` to the input audit and `v8` to the algorithm string's history,
naming both the bridged holes and the continuity rule's 1.5 threshold that
the kernel adopted under the compatibility marker's bump to 3;
section 8 gains the two recordings' numbers from the manual step. The
golden count ("three fits, eleven rejections") becomes four and ten
wherever it is stated. `docs/COMPUTED_PLOTS.md`'s section on holes gains the
sentence about the fusion plots. `docs/DATA_SCHEMA.md` section 12 names
`v8`. `tests/README.md`: the fixture table, section 12's new step, the
specification's appendix and matrix.

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, staged by explicit path, with the
session's attribution line; `PLANS/` is never staged.
