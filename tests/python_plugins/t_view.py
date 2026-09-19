"""The session view: undeclared reads, source access, and the stale view."""
import numpy as np
from flysight_plugin_sdk import (AttributePlugin, UndeclaredInputError, meas, source,
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


class PyUndeclaredSource(AttributePlugin):
    name = "_PY_UNDECL_SRC"
    def inputs(self): return [meas("IMU", "wx")]
    def compute(self, session):
        # The effective value is declared; the source layer is not.
        return float(session.sourceMeasurement("IMU", "wx")[0])


class PySrcOfDerived(AttributePlugin):
    """IMU/wTotal is derived, never recorded: this must never run."""
    name = "_PY_SRC_WTOTAL"
    def inputs(self): return [source("IMU", "wTotal")]
    def compute(self, session): return 1.0


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


for cls in (PyUndeclared, PyUndeclaredSwallowed, PyUndeclaredSource,
            PySrcOfDerived, PyDerivedWTotal, PyStash):
    register_attribute(cls())
