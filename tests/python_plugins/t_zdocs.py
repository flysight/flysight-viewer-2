"""The attribute snippet of python_plugins/README.md section 2, verbatim.
(Named to sort last, so it does not shift the registration indices of the others.)"""
from flysight_plugin_sdk import AttributePlugin, meas, register_attribute


class PyDuration(AttributePlugin):
    """Length of the IMU recording, in seconds."""
    name  = "_PY_DURATION"
    units = "s"

    def inputs(self):
        return [meas("IMU", "time")]

    def compute(self, session):
        t = session.getMeasurement("IMU", "time")
        return float(t.max() - t.min())


register_attribute(PyDuration())
