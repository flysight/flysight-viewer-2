# GNSS acceleration accuracy

Date: 2026-10-01
Status: specification for planning. Not an implementation plan. Small enough
to be implemented directly by one agent in one phase.
Baseline: branch `store-requested-calculations` at the head that implements
`PLANS/noise-model-part-1.md` and its fixups; the committed code is
authoritative.
Related: `docs/COMPUTED_PLOTS.md`, `docs/CALCULATIONS.md` (the GNSS
calculations), `src/calculations/gnsscalculations.cpp`
(`registerGnssDerivative`, the central difference of `computeDerivative`),
`src/mainwindow.cpp` (`registerBuiltInPlots`, the "GNSS (Advanced)" rows and
the deep palette of the GNSS accuracy rows), `tests/acceptance_map.txt`.

## 1. Motivation

The GNSS accelerations are central differences of the receiver's velocity
over two fix intervals. Users who measure loads from them have no stated
accuracy for the result and estimate one by hand, conservatively, from the
receiver's speed accuracy. The viewer can state that figure itself, from the
receiver's own numbers and the one assumption the derivative makes.

## 2. What is published

- One on-demand GNSS calculation, `GNSS/accAcc`, m/s^2, aligned with
  `GNSS/_time`, from `GNSS/_time` and `GNSS/sAcc` only:
  `accAcc[i] = sqrt(sAcc[i+1]^2 + sAcc[i-1]^2) / (t[i+1] - t[i-1])` for the
  interior samples, and at the two ends the forward and backward forms that
  `computeDerivative` uses there (`sqrt(sAcc[1]^2 + sAcc[0]^2) / (t[1] -
  t[0])` and its mirror). It is the standard deviation of every GNSS
  acceleration component and, to the usual approximation for a magnitude
  well above its sigma, of the horizontal, along-track and cross-track
  accelerations too, because `sAcc` is one figure for all three velocity
  components.
- Strictly the receiver's `sAcc` and the sample times: no correction factor,
  nothing from the fusion, nothing measured on a corpus.
- One plot, "Acceleration accuracy" in "GNSS (Advanced)", type
  `acceleration`, in the deep palette of the GNSS accuracy rows, a hue at
  least 40 degrees from every other acceleration row of its category.
  Unavailable when `sAcc` or `_time` is missing or shorter than two samples,
  as the derivative is.

## 3. What the documentation says

In one sentence each: that the figure is the receiver's stated speed
accuracy propagated through the central difference under the assumption
that the two fixes' errors are independent; that the assumption is
conservative, since a receiver's velocity errors are correlated between
fixes and its stated accuracy is itself cautious; and the measurement that
shows how much, on the reference recording `24-09-05/11-17-12` with the
fused velocity as reference: the actual error of the central-difference
acceleration was 0.09 g RMS overall and 0.058 g in steady flight against the
formula's 0.19 g, the receiver's velocity error 0.18 m/s against a stated
0.52, and its autocorrelation over two fixes 0.36. These numbers describe
the gap; they are not applied.

## 4. Tests

- Known answers: on a synthetic session with chosen `sAcc` and times, every
  sample equals the formula bit for bit, the ends included.
- Unavailable without `sAcc`, with one sample, with lengths that differ.
- The plot row is in the registry with its type and palette, and a legend
  value over it formats as an acceleration (`tst_plot_format` pattern).
- One acceptance item in the next free hundred of `tests/acceptance_map.txt`,
  with its appendix and section in `tests/README.md`.

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, subject `GNSS acceleration accuracy`, with
the session's attribution line, staged by explicit path; `PLANS/` is never
staged.
