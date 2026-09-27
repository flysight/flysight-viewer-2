# Sensor fusion: next steps

Updated: 2026-09-17

## Objective and constraints

Improve the reliability and physical accuracy of the IMU/GNSS solution while keeping normal computation close to one solve per recording.

- Support arbitrary FlySight uses, including skydiving, BASE jumping, parachute tests, amusement rides, rockets, and ground measurements. Do not require a recognizable jump profile, aircraft climb, or known mounting heading.
- Assess measurement consistency and available motion information. Ground/jump labels are useful for reviewing this dataset, not a proposed production classifier.
- Keep numerical convergence, measurement fit quality, and confidence in heading distinct. None of these alone establishes physical accuracy.
- Consider a recommended preparation procedure, such as keeping the powered unit stationary for 60 seconds. Its duration and effectiveness remain to be tested; recordings without that preparation still need an honest fallback.
- Preserve original recordings and completed experiments. Use small controlled experiments before repeating the bulk study.

This document records the agreed investigation priorities. The implementation and new experiments below are not yet complete.

## Evidence to carry forward

The five-start study tested yaw offsets of 0°, 45°, 90°, 180°, and 270° relative to the existing initializer on 97 eligible recordings from seven device groups. There were 435 reported-converged fits and 50 nonconverged fits; eight additional recordings were excluded for input/coverage reasons.

- **No universal starting-yaw winner.** Among 81 recordings where both 0° and 45° reported convergence, 45° won 20 cost comparisons, 0° won 17, and 44 tied. Validation groupings selected different defaults.
- **Single-fit diagnostics are promising.** Whole-fit normalized IMU RMS, rotation residual RMS, and the 95th-percentile attitude correction rate ranked poorer results reasonably well. They are not calibrated measures of attitude accuracy.
- **Conditional retries can help.** One exploratory policy reduced material objective losses/unavailable results from 10 to 5 on 32 validation recordings, retrying 11 and using about 1.6 times single-start computation. Its angles and thresholds are not validated production settings.
- **Yaw retries do not resolve all disagreement.** Even the best tested reported-converged result exceeded worst-minute normalized IMU RMS 3 on 51 recordings. That threshold itself is unvalidated.
- **Motion influences scores.** Much of the pooled RMS clustering comes from quiet ground recordings versus dynamic recordings. Low residuals during stillness do not establish accurate yaw.
- **The status flag needs repair.** A controlled trial returned convergence with an accelerometer bias near 19 m/s² and an objective about 71 times the best original attempt. Replaying its saved state proved that an internal rejected-step stop could be mistaken for convergence.
- **There is a concrete gyro conversion discrepancy.** All 105 recordings identify firmware `v2023.09.22`. That source uses `2000 / 32768` degrees/s/count at ±2000°/s; the LSM6DSO datasheet specifies `0.070`. The nominal correction ratio is **1.14688**. The investigated recording's gyro values match the firmware quantization exactly. Physical calibration and corrected-fit performance have not yet been tested.

The dataset has no independent attitude ground truth, includes correlated recordings and repeated starts, and has some prior development exposure. Treat the original convergence labels as reported solver status, not proof of settled optima. Revisit its quality thresholds after correcting measurement or solver assumptions.

## 1. Validate measurement conversion before tuning the model

### Work

- [ ] Trace the complete LSM6DSO-to-CSV-to-fusion conversion for the affected firmware: configured ranges, sensitivities, axis mapping, signs, units, integer arithmetic, rounding, and timestamps.
- [ ] Confirm the gyro sensitivity with controlled rotations of known angle/rate on each axis, with stationary intervals to estimate offsets. Include both rotation directions and more than one rate to separate scale error from additive bias.
- [ ] Check accelerometer scaling and orientation using known static orientations. Audit saturation and arithmetic behavior at larger inputs; a scale correction cannot recover corrupted or clipped measurements.
- [ ] Establish which firmware versions and configurations are affected. Define separate handling for future firmware output and historical recordings, with explicit provenance and protection against double correction. Do not infer correction solely from filename/date or silently rewrite input files.
- [ ] Create separate corrected fixtures for a small diagnostic set. Initially change only the conversion, holding solver settings, noise weights, and initialization policy fixed. Retain detailed termination diagnostics for interpretation.

### Diagnostic set

Include the original yaw-flip example, the all-start-failure recording `Data comp 2 / 24-09-07/10-15-24` (`23f35513fda9`), an already well-behaved dynamic recording, a persistently high-residual recording, and a quiet recording. Identify them by full paths and input hashes. For the original example, preserve the study's exact TRACK.CSV choice; do not substitute `TRACK - Copy.CSV`.

### Decision point

Demonstrate the conversion on known motion and determine whether corrected inputs improve physical consistency and optimization behavior. Do not assume the discrepancy explains every failure. Capture a new baseline before drawing further conclusions about preferred yaw or fit-quality cutoffs.

## 2. Fix termination reporting and preserve useful diagnostics

This is one of the three explicitly retained follow-up items and can be investigated independently of the conversion work.

### Work

- [ ] Record whether an optimizer update was accepted, trial/rejection counts, damping parameter, cost change, and the reason the inner search stopped. Record outer-pass bias changes and budget exhaustion separately.
- [ ] Reconcile GTSAM's internal trial-search tolerance with the application's stopping rule. The current internal relative tolerance is `1e-5`, while the wrapper checks `1e-8`; calling `iterate()` does not bypass the internal rule.
- [ ] Prevent an unchanged state following rejected trials from automatically becoming successful convergence. Distinguish convergence, stalled search, exhausted budget, cancellation, and invalid input. Allow legitimate zero-update convergence only with an explicit appropriate optimality check.
- [ ] Keep diagnostics for unsuccessful solves, even when Viewer does not expose their attitude output as a successful result.
- [ ] Add focused regression coverage for the demonstrated rejected-step stop and for legitimate convergence. Verify that fixing status reporting does not merely turn good results into failures or accept poor results by relaxing tolerances.

### Decision point

The saved bad trial must no longer receive an unqualified convergence status merely because its state did not change. Known good solves must still terminate correctly, and exhausted original attempts must retain an accurate reason.

## 3. Improve bias-aware initialization and test preparation guidance

This is the second explicitly retained follow-up item.

### Work

- [ ] Retain candidate-window rejection reasons and distinguish a lack of quiet data from rejection of otherwise useful data by the absolute gyro-rate bound.
- [ ] Revisit the current 1°/s bound on the mean gyro-vector magnitude using measured LSM6DSO offsets. Do not equate a typical datasheet value with a guaranteed bound or a Gaussian prior standard deviation.
- [ ] Preserve protection against real rotation, including slow rotation about gravity that may leave accelerometer readings nearly unchanged. Use gyro variability, force stability, GNSS consistency, coverage, and other available evidence together.
- [ ] Evaluate the initial attitude over the full recording, not just at the selected gravity anchor. A locally good bias/tilt estimate can still lead to a poor trajectory after long propagation.
- [ ] Compare the current initializer with narrowly scoped alternatives if necessary, such as a short preliminary fit followed by propagation. Measure their added computation and avoid routinely running several full fits.
- [ ] Test stationary preparation durations around the current 30-second window and proposed 60-second procedure, across several devices and thermal conditions. Separate the benefit of averaging quiet samples from the benefit of temperature stabilization.
- [ ] Define useful feedback when initialization is weak or unsupported, without assuming a jump phase or equating GNSS course with sensor heading.

### Decision point

A candidate must improve complete-recording results on corrected inputs, not merely increase accepted-window counts. Document what a preparation procedure provides—principally gyro-offset and gravity/tilt information—and that stillness alone does not establish absolute yaw.

The existing diagnostic relaxation is **not a ready fix**: accepting the otherwise quiet 85–115 s window improved initial local gravity consistency but led to a much worse whole-recording fit. Re-evaluate after conversion and termination work.

## 4. Characterize the effective IMU model

### Work

- [ ] Document the sensor's actual operating mode, output rate, filters, logging cadence, quantization, and measurement timing for each supported configuration. Distinguish hardware output rate from observed CSV sample intervals.
- [ ] Measure noise and bias stability on actual FlySight units under static and controlled dynamic conditions. Use sufficiently long stationary recordings to examine how error changes with averaging time, where useful.
- [ ] Investigate temperature-related bias changes. Temperature is logged but is absent from the current fusion input; the investigated recording spans about 25.8–43.1°C.
- [ ] Assess whether one shared constant bias per sensor is adequate. Compare a simple temperature-dependent or slowly varying bias model only if measurements justify it; constrain added freedom so it cannot hide incorrect attitudes or other data errors.
- [ ] Investigate timing, filter delay, and finite-rate integration where residuals follow motion. Avoid treating every systematic discrepancy as white noise.
- [ ] Tune noise densities and bias priors from measured behavior and validation. Current densities are modeling weights, not established LSM6DSO characteristics.

### Decision point

Accept additional model complexity only when it improves independently checked motion/attitude behavior or measurement prediction at acceptable computational cost. Smaller normalized RMS obtained merely by increasing assumed noise is not evidence of improved accuracy. Raw objectives are directly comparable only under the same input and weighting definitions.

## 5. Build an independent quality assessment

This is the third explicitly retained follow-up item.

### Work

- [ ] Retain whole-fit IMU normalized RMS, rotational residual RMS, attitude correction rates, GNSS residuals, and bias plausibility as initial candidates.
- [ ] Include local diagnostics so a long quiet portion cannot hide a bad dynamic interval. Handle short recordings and unsupported rolling windows explicitly.
- [ ] Evaluate whether scores predict correctable poor fits across different amounts and types of motion. Use measurement-derived information rather than requiring an activity label.
- [ ] Explore a separate indication that heading is weakly constrained. Small residuals, similar costs across starts, and numerical convergence are insufficient evidence of heading certainty.
- [ ] Calibrate any thresholds after measurement conversion and solver behavior stabilize. Assess missed bad results and unnecessary warnings/retries, including already well-fitted challenging motion.
- [ ] Reject the known implausible-bias/high-residual trial through single-result checks regardless of its solver status.

### Decision point

Quality reporting must distinguish a numerically settled but inconsistent fit from a consistent fit with weak heading information. Do not present an unvalidated score as a probability of attitude correctness.

## 6. Evaluate a bounded retry policy

### Work

- [ ] Keep one full solve as the normal path. Use a result's termination reason and quality diagnostics to decide whether additional computation is justified.
- [ ] Evaluate at most one targeted retry initially. Choose between an alternate initialization, a fixed alternate yaw, or additional iterations based on evidence about the failure mode.
- [ ] Compare accepted candidates using the same measurements and model, while applying quality checks to the selected result. Lower objective alone does not guarantee an acceptable solution.
- [ ] Select policy parameters on training data and evaluate them on separate recordings/events and device groups. Include unavailable results, missed improvements, unnecessary retries, total work, and worst-case latency.
- [ ] Revisit the preferred yaw only if new evidence supports it. Do not adopt 45° or the old RMS thresholds of 1.5/2 as established defaults.

### Decision point

Show a useful reliability improvement over one start with a bounded, measured computation increase. Benchmark native solve time and Viewer latency separately from diagnostic export overhead and concurrent-process contention.

## 7. Broaden validation and document the resulting contract

- [ ] Add controlled cases with independent orientation or known motion: stillness, constant translation, known rotations, mixed-axis motion, high acceleration, vibration, short captures, thermal changes, and missing coverage. Include cases that provide little heading information.
- [ ] Obtain representative recordings from uses beyond wingsuit skydiving. Keep shared events and repeated yaw starts together during validation; preserve genuinely unexposed cases.
- [ ] Validate proposed preparation guidance and clearly describe its benefits and limits.
- [ ] Once the small experiments justify an approach, run a new versioned bulk study. Retain inputs, configuration/source hashes, initial states, final states, residuals, solver histories, and failure reasons for analysis without repeated solves.
- [ ] Compare physical accuracy, availability, quality-detection performance, and computation. Do not use best-of-five agreement as ground truth.
- [ ] Document supported firmware/configuration handling, initialization fallback, termination states, quality limitations, and behavior when heading is weakly constrained.

## Recommended sequence

1. Verify conversion and preserve corrected fixtures; repair termination interpretation.
2. Establish a small corrected baseline, then improve initialization and characterize residual causes.
3. Evaluate quality assessment and a bounded retry policy against that baseline.
4. Broaden validation, establish preparation guidance, and perform the next versioned bulk study.

Avoid rerunning the entire yaw grid after each exploratory change. The current artifacts support most diagnosis without new solves; focused experiments should determine which changes deserve broader testing.

## References and reusable artifacts

- [Current implementation and limitations](../docs/SENSOR_FUSION.md)
- [Main five-start study](../TEMP/five-start-study-20260915/README.md)
- [Study review and validation limits](../TEMP/five-start-study-20260915/REVIEW.md)
- [All-start-failure investigation and saved-state replay](../TEMP/five-start-study-20260915/failure-investigation/README.md)
- [Raw-motion review](../TEMP/five-start-study-20260915/motion-review/README.md)
- [RMS cluster interpretation](../TEMP/five-start-study-20260915/motion-review/RMS_CLUSTERS.md)
- [LSM6DSO conversion evidence and downloaded firmware sources](../TEMP/five-start-study-20260915/imu-hardware-review/README.md)
- [Discussion constraints and retained findings](../TEMP/five-start-study-20260915/DISCUSSION_NOTES.md)

The TEMP reports and experiment outputs are local artifacts; preserve or archive them with their provenance before cleanup. The key findings and constraints are summarized here so this plan remains understandable without those files.
