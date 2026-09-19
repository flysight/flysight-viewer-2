"""
Example plugin: IMU tilt (one computation, several outputs)
===========================================================

Demonstrates the multi-output form, `CalculationPlugin`:

* declared inputs - `compute()` reads only the keys returned by `inputs()`;
* effective reads - `session.getMeasurement("IMU", "ax")` is in m/s^2 whatever
  unit the file recorded (FlySight 2 files record `g`);
* a bundle return - one dict, keyed by the same keys `outputs()` declares; the
  total acceleration is computed once and the plugin runs once per session,
  however many of its three outputs are read;
* returning `None` when there is nothing sensible to publish.

This file is NOT loaded from the `examples` folder. To enable it, copy it one
level up, next to `flysight_plugin_sdk.py` (or into the folder named by the
FLYSIGHT_PLUGINS environment variable, together with the SDK), and restart
FlySight Viewer: "Tilt pitch" and "Tilt roll" appear in the plot list.

See README.md in the plugin folder for the full guide.
"""
import numpy as np
from flysight_plugin_sdk import (CalculationPlugin, SimplePlot, attr, meas,
                                 register_calculation, register_plot)


class ImuTilt(CalculationPlugin):
    name = "ImuTilt"
    units = {meas("IMU", "tiltPitch"): "deg", meas("IMU", "tiltRoll"): "deg"}

    def inputs(self):
        return [meas("IMU", "ax"), meas("IMU", "ay"), meas("IMU", "az")]

    def outputs(self):
        return [meas("IMU", "tiltPitch"), meas("IMU", "tiltRoll"), attr("_IMU_PEAK_ACCEL")]

    def compute(self, session):
        ax = session.getMeasurement("IMU", "ax")
        ay = session.getMeasurement("IMU", "ay")
        az = session.getMeasurement("IMU", "az")
        if ax.size == 0 or not (ax.size == ay.size == az.size):
            return None

        g = np.sqrt(ax**2 + ay**2 + az**2)      # total acceleration, m/s^2
        return {
            meas("IMU", "tiltPitch"): np.degrees(np.arctan2(-ax, np.sqrt(ay**2 + az**2))),
            meas("IMU", "tiltRoll"):  np.degrees(np.arctan2(ay, az)),
            attr("_IMU_PEAK_ACCEL"):  float(g.max()),   # m/s^2
        }


register_calculation(ImuTilt())
register_plot(SimplePlot("IMU (examples)", "Tilt pitch", "deg", "#8E24AA", "IMU", "tiltPitch", "angle"))
register_plot(SimplePlot("IMU (examples)", "Tilt roll",  "deg", "#3949AB", "IMU", "tiltRoll",  "angle"))
