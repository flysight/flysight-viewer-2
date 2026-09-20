"""The session view: undeclared reads, what it exposes, and the stale view."""
import numpy as np
from flysight_plugin_sdk import (AttributePlugin, UndeclaredInputError, meas,
                                 register_attribute)

stash = None


class PyUndeclared(AttributePlugin):
    name = "_PY_UNDECL"
    def inputs(self): return [meas("IMU", "wx")]
    def compute(self, session):
        return float(session.getMeasurement("IMU", "wy")[0])       # not declared


class PyUndeclaredSwallowed(AttributePlugin):
    name = "_PY_UNDECL_SW"
    def inputs(self): return [meas("IMU", "wx")]
    def compute(self, session):
        try:
            session.getAttribute("FIRMWARE_VER")                   # not declared
        except Exception:
            pass
        return 1.0


class PyViewSurface(AttributePlugin):
    """1.0 iff the view offers no way to reach the source layer."""
    name = "_PY_VIEW_NO_SOURCE"
    def inputs(self): return [meas("IMU", "wx")]
    def compute(self, session):
        gone = ("sourceMeasurement", "sourceUnit", "hasSourceMeasurement")
        return 0.0 if any(hasattr(session, n) for n in gone) else 1.0


class PyDerivedWTotal(AttributePlugin):
    name = "_PY_EFF_WTOTAL0"
    def inputs(self): return [meas("IMU", "wTotal")]
    def compute(self, session):
        return float(session.getMeasurement("IMU", "wTotal")[0])


class PyStash(AttributePlugin):
    name = "_PY_STASH"
    def inputs(self): return [meas("IMU", "wx")]
    def compute(self, session):
        global stash
        stash = session
        return 1.0


def poke_stale():
    """True iff the stashed session object refuses to be used after compute()."""
    try:
        stash.getMeasurement("IMU", "wx")
    except UndeclaredInputError:
        return False
    except RuntimeError:
        return True
    return False


for cls in (PyUndeclared, PyUndeclaredSwallowed, PyViewSurface,
            PyDerivedWTotal, PyStash):
    register_attribute(cls())
