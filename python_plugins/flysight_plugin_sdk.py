"""
FlySight Plugin SDK
====================

This module provides the Python SDK for creating FlySight Viewer plugins.
Plugins can define custom attributes, measurements, and plots that integrate
seamlessly with the FlySight Viewer application.

Overview
--------
The SDK provides four main extension points:

1. **AttributePlugin**: Compute single-value attributes from session data
   (e.g., start time, duration, exit time)

2. **MeasurementPlugin**: Compute array-based measurements that align with
   sensor timestamps (e.g., derived calculations, filtered data)

3. **SimplePlot**: Register plots that display measurements in the plot view
   with automatic unit conversion support

4. **SimpleMarker**: Register marker definitions that appear on the plot as
   reference or analysis markers (e.g., exit, start, max vertical speed)

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

For more complex calculations, subclass AttributePlugin or MeasurementPlugin
and implement the `inputs()` and `compute()` methods.
"""
from __future__ import annotations
import numpy as np
from datetime import datetime, timezone
from dataclasses import dataclass
from typing import List, Union, Optional
# Pull in the C++ bridge for DependencyKey
from flysight_cpp_bridge import DependencyKey


# ─── internal registries ────────────────────────────────────────────────
_attributes:   List[AttributePlugin]   = []
_measurements: List[MeasurementPlugin] = []
_simple_plots: List[SimplePlot]        = []
_markers:      List[SimpleMarker]      = []


# ─── helpers to construct DependencyKey ─────────────────────────────────
def meas(sensor: str, name: str) -> DependencyKey:
    k = DependencyKey()
    k.kind            = DependencyKey.Type.Measurement
    k.sensorKey       = sensor
    k.measurementKey  = name
    return k

def attr(name: str) -> DependencyKey:
    k = DependencyKey()
    k.kind           = DependencyKey.Type.Attribute
    k.attributeKey   = name
    return k


# ─── base classes for plug-ins ─────────────────────────────────────────
class AttributePlugin:
    """Return a single QVariant-compatible value or small NumPy array.

    `compute()` may read only the keys returned by `inputs()`; any other read
    makes the result unavailable.
    """
    name:  str
    units: Optional[str] = None

    def inputs(self) -> List[DependencyKey]:
        return []

    def compute(self, session) -> Union[float, str]:
        raise NotImplementedError

def register_attribute(plugin: AttributePlugin) -> None:
    _attributes.append(plugin)

class MeasurementPlugin:
    """Return a full-length NumPy array (one value per sample).

    `compute()` may read only the keys returned by `inputs()`; any other read
    makes the result unavailable.
    """
    name:   str
    units:  Optional[str] = None
    sensor: str

    def inputs(self) -> List[DependencyKey]:
        return []

    def compute(self, session) -> np.ndarray:
        raise NotImplementedError

def register_measurement(plugin: MeasurementPlugin) -> None:
    _measurements.append(plugin)

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
        measurements: List of (sensor, measurement) tuples this marker relates to (default: empty)
        editable: Whether the user can reposition this marker by dragging (default: False)

    Example:
        register_marker(SimpleMarker(
            category="Analysis",
            display_name="Maximum horizontal speed",
            short_label="Max HS",
            color="#FF5722",
            attribute_key="_MAX_VELH_TIME",
            measurements=[("GNSS", "velH")]
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


# ─── default plug-ins ──────────────────────────────────────────────────
class DefaultStartTime(AttributePlugin):
    def __init__(self, sensor: str):
        self.name   = "_START_TIME"
        self.sensor = sensor
    def inputs(self):
        return [ meas(self.sensor, "_time") ]
    def compute(self, session):
        times = np.array(session.getMeasurement(self.sensor, "_time"), float)
        if times.size == 0: return None
        t0 = float(times.min())
        dt = datetime.fromtimestamp(t0, tz=timezone.utc)
        return dt.isoformat().replace("+00:00","Z")

class DefaultDuration(AttributePlugin):
    units = "s"
    def __init__(self, sensor: str):
        self.name   = "_DURATION"
        self.sensor = sensor
    def inputs(self):
        return [ meas(self.sensor, "_time") ]
    def compute(self, session):
        times = np.array(session.getMeasurement(self.sensor, "_time"), float)
        if times.size == 0: return None
        dur = float(times.max() - times.min())
        return dur if dur>=0 else None

class DefaultExitTime(AttributePlugin):
    def __init__(self, sensor: str):
        self.name   = "_EXIT_TIME"
        self.sensor = sensor
    def inputs(self):
        return [ meas(self.sensor, "_time") ]
    def compute(self, session):
        times = np.array(session.getMeasurement(self.sensor, "_time"), float)
        if times.size == 0: return None
        t0 = float(times[-1])
        dt = datetime.fromtimestamp(t0, tz=timezone.utc)
        return dt.isoformat().replace("+00:00","Z")

class DefaultTime(MeasurementPlugin):
    units = "s"
    def __init__(self, sensor: str, time: str):
        self.name   = "_time"
        self.sensor = sensor
        self.time   = time
    def inputs(self):
        return [
            meas(self.sensor, self.time),
        ]
    def compute(self, session):
        raw = np.array(session.getMeasurement(self.sensor, self.time), float)
        if raw.size==0: return None
        return raw 

