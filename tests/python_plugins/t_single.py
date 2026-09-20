"""Existing-style single-output plugins, written exactly as a plugin that
predates the engine would be: inputs() + compute(session) + session.get*()."""
import numpy as np
from flysight_plugin_sdk import (AttributePlugin, MeasurementPlugin, SimplePlot, SimpleMarker,
                                 attr, meas, register_attribute, register_measurement,
                                 register_plot, register_marker)


class PyWx0(AttributePlugin):
    name = "_PY_WX0"

    def inputs(self):
        return [meas("IMU", "wx")]

    def compute(self, session):
        return float(np.array(session.getMeasurement("IMU", "wx"), float)[0])


class PyWx2(MeasurementPlugin):
    sensor = "IMU"
    name = "pyWx2"
    units = "deg/s"

    def inputs(self):
        return [meas("IMU", "wx")]

    def compute(self, session):
        return np.array(session.getMeasurement("IMU", "wx"), float) * 2


class PyFirmware(AttributePlugin):
    name = "_PY_FW"

    def inputs(self):
        return [attr("FIRMWARE_VER")]

    def compute(self, session):
        return session.getAttribute("FIRMWARE_VER")


register_attribute(PyWx0())
register_measurement(PyWx2())
register_attribute(PyFirmware())

register_plot(SimplePlot("PyTest", "Py Wx2", "deg/s", "#112233", "IMU", "pyWx2", "rotation"))
register_marker(SimpleMarker("PyTest", "Py marker", "PyM", "#445566", "_PY_WX0",
                             [("IMU", "_time", "wx")]))
