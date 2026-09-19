"""
FlySight Plugin SDK
====================

This module provides the Python SDK for creating FlySight Viewer plugins.
Plugins can define custom attributes, measurements, and plots that integrate
seamlessly with the FlySight Viewer application. The full guide is README.md,
next to this file.

Overview
--------
The SDK provides five extension points:

1. **AttributePlugin**: Compute one single-value attribute from session data
   (e.g., a duration, a peak value, a time in UTC seconds)

2. **MeasurementPlugin**: Compute one array-based measurement that aligns with
   a sensor's timestamps (e.g., derived calculations, filtered data)

3. **CalculationPlugin**: One computation that returns several declared
   outputs (attributes and/or measurements) as a bundle

4. **SimplePlot**: Register plots that display measurements in the plot view
   with automatic unit conversion support

5. **SimpleMarker**: Register marker definitions that appear on the plot as
   reference or analysis markers (e.g., exit, start, max vertical speed)

Effective and source values
---------------------------
Ordinary reads (`session.getMeasurement`, declared with `meas()`) return the
*effective* values FlySight Viewer itself uses: corrected for the file's data
schema and normalized to SI units, under the recorded names. Declaring
`source(sensor, name)` additionally gives `session.sourceMeasurement` /
`session.sourceUnit`: exactly what the file recorded, never computed.

Every key read inside `compute()` must be returned by `inputs()`; any other
read raises `UndeclaredInputError` and makes the result unavailable.

Header attributes (`FIRMWARE_VER`, `SCHEMA_VER`, ...) are single-valued per
session: a merged file with a different value is rejected (see README, 'Header
attributes and the conflict rule').

Unit Conversion
---------------
Plots can participate in the automatic unit conversion system by specifying
a `measurement_type` when creating a SimplePlot. This allows the viewer to
convert values between metric and imperial unit systems based on user
preferences. See the SimplePlot class documentation for available measurement
types and usage examples.

Quick Start
-----------
To create a simple plugin that registers a plot:

    from flysight_plugin_sdk import SimplePlot, register_plot

    register_plot(SimplePlot(
        category="My Plugins",
        name="Ground Speed",
        units="m/s",
        color="#1E88E5",
        sensor="GNSS",
        measurement="vel2D",
        measurement_type="speed"  # Enables m/s <-> mph conversion
    ))

For calculations, subclass AttributePlugin, MeasurementPlugin or
CalculationPlugin and implement `inputs()` and `compute()` (see README.md and
examples/imu_tilt.py).
"""
from __future__ import annotations
import numpy as np
from dataclasses import dataclass
from typing import Dict, List, Union, Optional
# Raised by the session object for a read that inputs() did not declare.
from flysight_cpp_bridge import UndeclaredInputError


# ─── internal registries (read by the host, in this order) ──────────────
_attributes:   List[AttributePlugin]   = []
_measurements: List[MeasurementPlugin] = []
_calculations: List[CalculationPlugin] = []
_simple_plots: List[SimplePlot]        = []
_markers:      List[SimpleMarker]      = []


# ─── dependency keys ────────────────────────────────────────────────────
KIND_ATTRIBUTE   = "attribute"
KIND_MEASUREMENT = "measurement"
KIND_SOURCE      = "source"

@dataclass(frozen=True)
class Key:
    """A dependency key: hashable and comparable by value. Build one with attr(), meas() or source()."""
    kind:   str
    sensor: str = ""
    name:   str = ""          # attribute key when kind == "attribute"

def attr(name: str) -> Key:
    """Key of a session attribute (input or output)."""
    return Key(KIND_ATTRIBUTE, "", name)

def meas(sensor: str, name: str) -> Key:
    """Key of a measurement, read as its effective (corrected, SI) value (input or output)."""
    return Key(KIND_MEASUREMENT, sensor, name)

def source(sensor: str, name: str) -> Key:
    """Key of a measurement's recorded samples and unit text (input only)."""
    return Key(KIND_SOURCE, sensor, name)


# ─── base classes for plug-ins ─────────────────────────────────────────
class AttributePlugin:
    """One attribute, named by `name`.

    Inputs: `compute()` may read only the keys returned by `inputs()` (read
    once, at startup). All of them are required: the plugin runs only when
    every one is available. Any other read raises UndeclaredInputError.
    Return: `float` / `int` (NumPy scalars included), `str`, or `None` for "no
    value". NaN / inf also mean "no value"; `bool`, lists and arrays are
    rejected. Times are UTC seconds as `float`; strings are never parsed.
    Errors: an exception or a malformed return gives an unavailable result,
    logged once; the plugin is not called again until a declared input changes.
    See README.md.
    """
    name:  str
    units: Optional[str] = None     # informational only

    def inputs(self) -> List[Key]:
        return []

    def compute(self, session) -> Union[float, int, str, None]:
        """Return the attribute value: float | int | str | None (see the class docstring)."""
        raise NotImplementedError

def register_attribute(plugin: AttributePlugin) -> None:
    _attributes.append(plugin)

class MeasurementPlugin:
    """One measurement, `sensor`/`name`, with the unit label `units`.

    Inputs: `compute()` may read only the keys returned by `inputs()` (read
    once, at startup). All of them are required: the plugin runs only when
    every one is available. Any other read raises UndeclaredInputError.
    Return: a 1-D real numeric array-like (NumPy array of a float / integer
    dtype, list, tuple) with one value per sample of the sensor's time vector,
    or `None` for "no value". NaN samples are gaps. The host copies the values,
    so the returned buffer may be reused. Scalars, 2-D arrays, bool / string /
    object arrays and `str` are rejected.
    Errors: an exception or a malformed return gives an unavailable result,
    logged once; the plugin is not called again until a declared input changes.
    See README.md.
    """
    name:   str
    units:  Optional[str] = None    # reported as the measurement's effective unit
    sensor: str

    def inputs(self) -> List[Key]:
        return []

    def compute(self, session) -> Optional[np.ndarray]:
        """Return a 1-D numeric array (one value per sample) or None (see the class docstring)."""
        raise NotImplementedError

def register_measurement(plugin: MeasurementPlugin) -> None:
    _measurements.append(plugin)

class CalculationPlugin:
    """One computation, several declared outputs.

    Inputs: as for the single-output forms - `inputs()` is read once, every
    declared input is required, any other read raises UndeclaredInputError.
    Outputs: `outputs()` returns at least one attr() / meas() key.
    Return: a dict keyed by those same keys, with values typed as for
    AttributePlugin / MeasurementPlugin. `None` as a value, or a missing entry,
    makes that one output unavailable; returning `None` makes all of them
    unavailable. A key that is not in `outputs()` or a malformed value means
    nothing at all is published. The computation runs once per session however
    many of its outputs are read.
    Errors: as for the single-output forms. See README.md and examples/imu_tilt.py.
    """
    name:  str                      # optional; defaults to the class name
    units: Dict[Key, str] = {}      # optional unit label per *measurement* output

    def inputs(self) -> List[Key]:
        return []

    def outputs(self) -> List[Key]:
        raise NotImplementedError

    def compute(self, session) -> Optional[Dict[Key, object]]:
        raise NotImplementedError

def register_calculation(plugin: CalculationPlugin) -> None:
    _calculations.append(plugin)

@dataclass(frozen=True)
class SimplePlot:
    """
    Defines a simple plot that displays a measurement from SessionData.

    Attributes:
        category: Category name for grouping in the plot selection UI (e.g., "GNSS", "IMU")
        name: Display name of the plot (e.g., "Ground Speed", "Elevation")
        units: Display units string for the y-axis (e.g., "m/s"). Set to None if unitless.
               Note: This is the display string only; actual conversion uses measurement_type.
        color: CSS color string for the plot line (e.g., "#1E88E5", "blue", "rgb(30,136,229)")
        sensor: Sensor ID in SessionData (e.g., "GNSS", "IMU", "BARO")
        measurement: Measurement ID within the sensor (e.g., "hMSL", "velN", "temperature")
        measurement_type: Optional unit conversion category. When set, the UnitConverter
            automatically converts values between metric and imperial systems.

            Available measurement types:
            - "distance": meters <-> feet (for horizontal distances, accuracy values)
            - "altitude": meters <-> feet (for elevation, vertical position)
            - "speed": m/s <-> mph (for horizontal speeds)
            - "vertical_speed": m/s <-> mph (for vertical speeds)
            - "acceleration": m/s^2 <-> g's (displayed as g in both systems)
            - "temperature": Celsius <-> Fahrenheit
            - "pressure": Pascals <-> inHg
            - "rotation": deg/s (same in both systems)
            - "angle": degrees (same in both systems)
            - "magnetic_field": Tesla <-> gauss
            - "voltage": Volts (same in both systems)
            - "percentage": % (same in both systems)
            - "time": seconds (same in both systems)
            - "count": unitless integers (same in both systems)

            Leave as None (default) for plots that should not be unit-converted.

    Example:
        # A speed plot that converts between m/s and mph:
        register_plot(SimplePlot(
            category="My Plugin",
            name="Custom Speed",
            units="m/s",  # Base metric units
            color="#FF5722",
            sensor="GNSS",
            measurement="customSpeed",
            measurement_type="speed"
        ))
    """
    category:    str
    name:        str
    units:       Optional[str]
    color:       str
    sensor:      str
    measurement: str
    measurement_type: Optional[str] = None

def register_plot(meta: SimplePlot) -> None:
    _simple_plots.append(meta)


@dataclass(frozen=True)
class SimpleMarker:
    """
    Defines a simple marker that appears on the plot as a reference or analysis point.

    Attributes:
        category: Category name for grouping in the marker dock UI (e.g., "Reference", "Analysis")
        display_name: Descriptive name shown in the marker dock, reference dropdown, and axis labels
        short_label: Compact label shown in marker bubbles on the plot
        color: CSS color string for the marker (e.g., "#007ACC", "green")
        attribute_key: Unique session attribute key that stores the marker's time value
        measurements: List of (sensor, time_vector, data_vector) triples this marker relates to,
            e.g. ("GNSS", "_time", "velH") (default: empty)
        editable: Whether the user can reposition this marker by dragging (default: False)

    Example:
        register_marker(SimpleMarker(
            category="Analysis",
            display_name="Maximum horizontal speed",
            short_label="Max HS",
            color="#FF5722",
            attribute_key="_MAX_VELH_TIME",
            measurements=[("GNSS", "_time", "velH")]
        ))
    """
    category:      str
    display_name:  str
    short_label:   str
    color:         str
    attribute_key: str
    measurements:  List[tuple] = ()
    editable:      bool = False

def register_marker(meta: SimpleMarker) -> None:
    _markers.append(meta)
