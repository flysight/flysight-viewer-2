"""Return-value marshalling for the single-output forms."""
import gc
import numpy as np
from flysight_plugin_sdk import (AttributePlugin, MeasurementPlugin, meas,
                                 register_attribute, register_measurement)


class PyRaises(AttributePlugin):
    name = "_PY_RAISE"
    def inputs(self): return [meas("IMU", "wz")]
    def compute(self, session):
        if session.getMeasurement("IMU", "wz")[0] == 0:
            raise ValueError("boom")
        return 1.0


def _attribute(attr_name, value):
    class _Attr(AttributePlugin):
        name = attr_name
        def inputs(self): return [meas("IMU", "wx")]
        def compute(self, session): return value()
    _Attr.__name__ = _Attr.__qualname__ = "PyAttr" + attr_name
    register_attribute(_Attr())


def _measurement(meas_name, value):
    class _Meas(MeasurementPlugin):
        sensor = "IMU"
        name = meas_name
        def inputs(self): return [meas("IMU", "wx")]
        def compute(self, session): return value()
    _Meas.__name__ = _Meas.__qualname__ = "PyMeas_" + meas_name
    register_measurement(_Meas())


register_attribute(PyRaises())

# malformed -> Failed
_attribute("_PY_BOOL", lambda: True)
_attribute("_PY_LIST", lambda: [1, 2])
# no value -> Ok, unavailable
_attribute("_PY_NAN", lambda: float("nan"))
_attribute("_PY_NONE", lambda: None)
# accepted
_attribute("_PY_INT", lambda: 3)
_attribute("_PY_NPF", lambda: np.float64(2.5))
_attribute("_PY_NPI", lambda: np.int64(7))
_attribute("_PY_STR", lambda: "2024-01-01T00:00:00Z")

# malformed -> Failed
_measurement("pyTwoD", lambda: np.zeros((2, 2)))
_measurement("pyScalar", lambda: 3.0)
_measurement("pyStrArr", lambda: np.array(["a", "b"]))
_measurement("pyBoolArr", lambda: np.array([True, False]))
_measurement("pyStrMeas", lambda: "1.5")
# accepted
_measurement("pyList", lambda: [1, 2])
_measurement("pyF32", lambda: np.array([1.5, 2.5], dtype=np.float32))
_measurement("pyStride", lambda: np.arange(4.0)[::2])

# The host must copy: the plugin keeps, mutates and frees the returned buffer.
buf = np.array([1.0, 2.0, 3.0])
_measurement("pyKeep", lambda: buf)


def poke():
    global buf
    buf[:] = -1.0
    del buf
    gc.collect()


class PyShadowATotal(MeasurementPlugin):
    """Declares a built-in output: registered first, so it wins over the built-in."""
    sensor = "IMU"
    name = "aTotal"
    def inputs(self): return [meas("IMU", "ax")]
    def compute(self, session):
        return np.full(len(session.getMeasurement("IMU", "ax")), 42.0)


register_measurement(PyShadowATotal())
