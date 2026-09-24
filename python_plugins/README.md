# FlySight Viewer Python plugins

A plugin is a Python file that registers calculations, plots, or markers with
FlySight Viewer through `flysight_plugin_sdk.py` (next to this file).

## 1. Where plugins live

| Platform | Default plugin folder |
|---|---|
| Windows, Linux | `<application folder>/python_plugins` |
| macOS | `FlySightViewer.app/Contents/Resources/python_plugins` |

Set the environment variable `FLYSIGHT_PLUGINS` to use another folder (it must
also contain `flysight_plugin_sdk.py`).

At startup every `*.py` file directly inside the plugin folder is imported, in
file-name order. Subfolders are not imported, so nothing in `examples/` is
loaded: to try an example, copy it one level up and restart. (Every `*.py`
file in the folder and its subfolders still counts for the plug-in code
identity, section 7.)

## 2. The simple forms

One class, one output. `inputs()` says what the plugin reads; `compute()`
returns the value.

```python
import numpy as np
from flysight_plugin_sdk import (AttributePlugin, MeasurementPlugin, meas,
                                 register_attribute, register_measurement)

class PyDuration(AttributePlugin):
    """Length of the IMU recording, in seconds."""
    name  = "_PY_DURATION"
    units = "s"

    def inputs(self):
        return [meas("IMU", "time")]

    def compute(self, session):
        t = session.getMeasurement("IMU", "time")
        return float(t.max() - t.min())

class PyGyroDouble(MeasurementPlugin):
    """A measurement: one value per sample of the sensor's time vector."""
    sensor = "IMU"
    name   = "pyWx2"
    units  = "deg/s"            # reported as the measurement's unit

    def inputs(self):
        return [meas("IMU", "wx")]

    def compute(self, session):
        return session.getMeasurement("IMU", "wx") * 2

register_attribute(PyDuration())
register_measurement(PyGyroDouble())
```
<!-- exercised by tests/python_plugins/t_zdocs.py (PyDuration) and tests/python_plugins/t_single.py (PyWx2) -->

A `MeasurementPlugin` should return one value per sample of `<sensor>/_time`.
The host does not check the length (it could not without reading something the
plugin did not declare); a plot whose x and y lengths differ is simply skipped.

## 3. The declared-inputs rule

* `inputs()` is read **once**, at startup.
* **All declared inputs are required.** The plugin runs only when every one of
  them is available, so do not declare inputs "just in case". To support two
  alternative inputs, register two plugins for the same output; they are tried
  in registration order.
* Reading a key that was not declared raises `UndeclaredInputError`, naming
  the plugin, the key, and the line to add to `inputs()`. The result is
  unavailable **even if the exception is caught**.
* `compute()` must be a pure function of its inputs: no clock, randomness,
  files, or globals. Results are cached and recomputed only when a declared
  input changes.
* The `session` object is valid only during `compute()`. Using it afterwards
  raises `RuntimeError`.

The same rules bind the built-in C++ calculations; they are described for
contributors in [the calculations note](https://github.com/flysight/flysight-viewer-2/blob/master/docs/CALCULATIONS.md)
(in the repository: `docs/CALCULATIONS.md`).

## 4. Effective values

`session.getMeasurement(sensor, name)` returns the **effective** values - what
FlySight Viewer itself uses: corrected for the file's data schema and
normalized to SI units, under the recorded names. For example, gyro rates from
files recorded before the data schema existed are multiplied by 1.14688,
accelerations recorded in `g` arrive in m/s^2, and magnetic fields recorded in
`gauss` arrive in T. `session.effectiveUnit(sensor, name)` gives the unit of
what you were handed.

```python
from flysight_plugin_sdk import CalculationPlugin, attr, meas, register_calculation

class PyEffectiveProbe(CalculationPlugin):
    def inputs(self):
        return [meas("IMU", "ax")]

    def outputs(self):
        return [attr("_PY_EFF_UNIT"), attr("_PY_EFF_AX0")]

    def compute(self, session):
        return {
            attr("_PY_EFF_UNIT"): session.effectiveUnit("IMU", "ax"),               # "m/s^2", whatever the file used
            attr("_PY_EFF_AX0"):  float(session.getMeasurement("IMU", "ax")[0]),    # 9.80665 for a recorded 1 g
        }

register_calculation(PyEffectiveProbe())
```
<!-- exercised by tests/python_plugins/t_multi.py (PyEffectiveProbe) and tst_python_bridge::effectiveReadAndUnitMatchCpp -->

Effective values are the only ones a plugin can read. What the file literally
recorded (the source layer) is read by FlySight Viewer's conversion layer and
by nothing else.

The schemas, the unit table, and the conversion rules are described in
[the data schema document](https://github.com/flysight/flysight-viewer-2/blob/master/docs/DATA_SCHEMA.md#6-the-conversion-layer)
(in the repository: `docs/DATA_SCHEMA.md`).

## 5. Multi-output calculations

`CalculationPlugin` is one computation with several declared outputs.
`outputs()` lists `attr()` / `meas()` keys; `compute()` returns a dict keyed by
those same keys.

```python
import numpy as np
from flysight_plugin_sdk import CalculationPlugin, attr, meas, register_calculation

class ImuTilt(CalculationPlugin):
    name  = "ImuTilt"
    units = {meas("IMU", "tiltPitch"): "deg", meas("IMU", "tiltRoll"): "deg"}

    def inputs(self):
        return [meas("IMU", "ax"), meas("IMU", "ay"), meas("IMU", "az")]

    def outputs(self):
        return [meas("IMU", "tiltPitch"), meas("IMU", "tiltRoll"), attr("_IMU_PEAK_ACCEL")]

    def compute(self, session):
        ax = session.getMeasurement("IMU", "ax")
        ay = session.getMeasurement("IMU", "ay")
        az = session.getMeasurement("IMU", "az")
        if ax.size == 0:
            return None
        g = np.sqrt(ax**2 + ay**2 + az**2)
        return {
            meas("IMU", "tiltPitch"): np.degrees(np.arctan2(-ax, np.sqrt(ay**2 + az**2))),
            meas("IMU", "tiltRoll"):  np.degrees(np.arctan2(ay, az)),
            attr("_IMU_PEAK_ACCEL"):  float(g.max()),
        }

register_calculation(ImuTilt())
```
<!-- this is examples/imu_tilt.py, trimmed; exercised by tst_python_bridge::bundledExampleRuns -->

* A value of `None`, or a declared output with no entry, makes **that output**
  unavailable; the others are published. Returning `None` makes all of them
  unavailable.
* A key that is not in `outputs()`, or a malformed value, means **nothing** is
  published.
* `units` optionally labels measurement outputs.
* The computation runs once per session however many of its outputs are read.

## 6. Return types

Attributes: `float` or `int` (NumPy scalars included), `str`, or `None` for "no
value". NaN and infinity also mean "no value". `bool`, lists, tuples, dicts and
arrays are rejected. Times are UTC seconds as a `float`
(`datetime.timestamp()`), like every built-in time attribute. **Strings are
never parsed as dates** (earlier versions converted ISO date strings to
seconds; they no longer do): a string stays a string.

Measurements: any 1-D real numeric array-like - a NumPy array of a float or
integer dtype, a list, a tuple; non-contiguous slices are fine - or `None`.
NaN samples are gaps; an empty array means "no value". Scalars, 2-D arrays,
bool / complex / string / object arrays, `str` and `bytes` are rejected.

Returned arrays are **copied** before `compute()` returns to the host, so
reusing or mutating a returned buffer afterwards is safe. Reads
(`getMeasurement`) return NumPy `float64` arrays that are private copies: modifying one changes nothing in the session.

`getAttribute` returns a `float` for numbers and numeric-looking text, a `str`
for other text.

## 7. Errors

An exception in `compute()`, or a malformed return value, gives an unavailable
result. The traceback is written **once** to the application log
(`[PluginHost] ... raised:` or `... returned malformed output:`), other plugins
are unaffected, and the plugin is **not called again until one of its declared
inputs changes**.

Registration problems - an unknown key kind, a bad `outputs()` list, a missing
`name`, a duplicate id - reject that one plugin at startup with a log line
(`[PluginHost] Plugin ... rejected:`); the others still load.

### Plug-in code identity: stored results and cached columns

At startup, before any plugin is imported, FlySight Viewer computes one digest,
the *plug-in code identity*, over:

* every `*.py` file in the plugin folder and its subfolders (its path relative
  to the folder and its bytes), hidden files included, except files in
  `__pycache__` and in hidden folders (names that start with `.`, or folders
  the file system marks hidden); a linked folder (a symbolic link, or a
  junction on Windows) is read through under its own name, and a link back to
  a folder above it is not followed again; `examples/` counts although
  nothing in it is imported;
* the SDK file (counted a second time as one of the files, since it lives in
  the plugin folder);
* the Python version and the NumPy version (`none` if one cannot be read).

The log shows it:
`[PluginHost] Plug-in code identity: plugins-sha256:... (3 files)`. An
ingredient that cannot be read (the NumPy version, the SDK file, a locked
`.py` file) is named in one warning,
`[PluginHost] Plug-in code identity: could not read ...`.

That is all it covers. Data files a plugin reads (a `.json` table, a model
file), compiled modules (`.pyd`, `.so`) and packages installed elsewhere that a
plugin imports are not part of it: if a plugin's results depend on one of
them, changing it does not change the identity. Rename the plugin's
calculation, or edit one of its `.py` files, after changing such a file. The
walk reads every `.py` file under the folder at every start, so keep large
trees out of it: a virtual environment in a folder whose name does not start
with `.` (`venv/` rather than `.venv/`) is read and hashed each time.

Every attribute, measurement and calculation a plugin registers declares it as
its result version. So editing, adding, removing or renaming any file in the
folder (not only the plugin you changed), or upgrading Python or NumPy, changes
it for all plugins at once. The digest is over the files' raw bytes: the same
plugin checked out with different line endings (by git's `autocrlf`, for
example) has a different identity on another machine. A logbook is meant for
one machine: a logbook folder synced between machines whose plugins or NumPy
versions differ has a different identity on each, so each machine discards the
other's stored results that went through a plugin and the cached values of the
columns over plugin calculations in `index.json` at its next start, and
recomputes them.

* The logbook column values cached for sessions that are not loaded are then
  discarded at the next start and recomputed in the background, for every
  column that can be computed through a plugin calculation (a column over a
  plugin value, or over a name a plugin provides): such a column never keeps
  showing what old code returned. No renaming is needed. Columns that no
  plugin calculation can reach keep their cached values.
* Plugin results are never stored: they are recomputed when read. But a stored
  result of a requested calculation (sensor fusion today) whose inputs were
  looked up through any plugin calculation (a plugin that provides a name the
  calculation reads, section 8) is stale at its session's next load after such
  a change, and must be requested again. A stored result that looked up no
  plugin output is not affected.
* Plugins are loaded once, at startup. An edit takes effect at the next start;
  nothing watches the files.

## 8. Precedence

A name is resolved in this order: recorded data or a stored attribute first,
then plugins in registration order (attributes, then measurements, then
calculations; files in name order), then built-ins. A plugin that declares a
built-in output (for example `IMU/aTotal`) therefore replaces the built-in
whenever the plugin's inputs are available. It can never override recorded
data or a value the user has set.

A plugin that declares a name a requested calculation looks up also changes
what that name resolves to: adding or removing such a plugin makes the stored
results that looked the name up stale at their next load (they are requested
again from the plot list).

## 9. What was removed

* `session.setCalculatedMeasurement(...)`, the direct cache setter. Plugins
  *return* results; they do not publish them. Use `CalculationPlugin` for
  several outputs.
* `flysight_cpp_bridge.DependencyKey`. Use `attr()` and `meas()`.
* Source access (`source()`, `session.sourceMeasurement`, `session.sourceUnit`,
  `session.hasSourceMeasurement`) is not offered: plugins read effective values
  only. A plugin file that still imports `source` from the SDK fails to import
  (none of its plugins load; the log names the file), and a plugin that still
  declares a `source` key is rejected at startup.
  A plugin that needs to know how the data was recorded can declare
  `attr("SCHEMA_VER")` as an ordinary input.
* The `Default*` example classes in the SDK.
* ISO date strings are no longer converted to seconds (section 6).

Preference inputs are not available to plugins.

## 10. Header attributes and the conflict rule

Attributes read from a file's `$VAR` lines (`FIRMWARE_VER`, `SCHEMA_VER`,
`DEVICE_ID`, ...) reach plugins through `getAttribute` exactly as recorded,
with numeric-looking text arriving as `float`. An absent attribute is "no
value": a plugin that declares it simply does not run. Never assume a default,
in particular for `SCHEMA_VER`, whose interpretation belongs to the conversion
layer. When a second file is merged into a session, a header attribute with a
*different* value makes that import fail rather than overwrite, so a plugin can
rely on header attributes being single-valued per session.

Two refinements: FlySight Viewer's own attributes (keys starting with `_`) in
an incoming file never overwrite existing values - the existing value wins and
absent ones are added, which is never a conflict; and the `DEVICE_ID`
placeholder `n/a` (stored when no device id is known) counts as absent.
Details: [the data schema document](https://github.com/flysight/flysight-viewer-2/blob/master/docs/DATA_SCHEMA.md#8-importing-and-merging)
(in the repository: `docs/DATA_SCHEMA.md`).

## 11. Plots and markers

See the `SimplePlot` and `SimpleMarker` docstrings in `flysight_plugin_sdk.py`.

## 12. Testing a plugin

The application's own bridge test (`tests/tst_python_bridge.cpp`, plugins under
`tests/python_plugins/`) runs plugins through the real embedded interpreter;
`tests/README.md` explains how to run it and how to add a case.
