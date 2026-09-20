"""The multi-output form: one computation, a bundle of declared outputs."""
import numpy as np
from flysight_plugin_sdk import CalculationPlugin, Key, attr, meas, register_calculation


class PyGyroStats(CalculationPlugin):
    units = {meas("IMU", "pyWNorm"): "deg/s"}
    def inputs(self): return [meas("IMU", "wx"), meas("IMU", "wy"), meas("IMU", "wz")]
    def outputs(self): return [attr("_PY_W_MAX"), attr("_PY_W_MIN"), meas("IMU", "pyWNorm")]
    def compute(self, session):
        w = np.array([session.getMeasurement("IMU", n) for n in ("wx", "wy", "wz")])
        return {
            attr("_PY_W_MAX"): float(w.max()),
            attr("_PY_W_MIN"): float(w.min()),
            meas("IMU", "pyWNorm"): np.sqrt((w ** 2).sum(axis=0)),
        }


class PyPartial(CalculationPlugin):
    def inputs(self): return [meas("IMU", "wx")]
    def outputs(self): return [attr("_PY_PART_A"), attr("_PY_PART_B"), attr("_PY_PART_C")]
    def compute(self, session):
        return {attr("_PY_PART_A"): 1.0, attr("_PY_PART_B"): None}


class PyEffectiveProbe(CalculationPlugin):
    def inputs(self): return [meas("IMU", "ax")]
    def outputs(self): return [attr("_PY_EFF_UNIT"), attr("_PY_EFF_AX0")]
    def compute(self, session):
        return {
            attr("_PY_EFF_UNIT"): session.effectiveUnit("IMU", "ax"),
            attr("_PY_EFF_AX0"): float(session.getMeasurement("IMU", "ax")[0]),
        }


class PyBundleRaises(CalculationPlugin):
    def inputs(self): return [meas("IMU", "wz")]
    def outputs(self): return [attr("_PY_BR_A"), attr("_PY_BR_B")]
    def compute(self, session):
        bundle = {attr("_PY_BR_A"): 1.0}
        if session.getMeasurement("IMU", "wz")[0] == 0:
            raise ValueError("late boom")
        bundle[attr("_PY_BR_B")] = 2.0
        return bundle


class PyWrongKey(CalculationPlugin):
    def inputs(self): return [meas("IMU", "wx")]
    def outputs(self): return [attr("_PY_WK_A")]
    def compute(self, session):
        return {attr("_PY_WK_A"): 1.0, attr("_PY_WK_OTHER"): 2.0}


class PyNotDict(CalculationPlugin):
    def inputs(self): return [meas("IMU", "wx")]
    def outputs(self): return [attr("_PY_ND_A")]
    def compute(self, session): return 5.0


class PyBadMember(CalculationPlugin):
    def inputs(self): return [meas("IMU", "wx")]
    def outputs(self): return [attr("_PY_BM_A"), meas("IMU", "pyBmM")]
    def compute(self, session):
        return {attr("_PY_BM_A"): 1.0, meas("IMU", "pyBmM"): np.zeros((2, 2))}


class PySourceOutput(CalculationPlugin):
    """The source kind is not a plugin key kind, as an output or otherwise."""
    def outputs(self): return [Key("source", "IMU", "wx")]
    def compute(self, session): return None


class PyNoOutputs(CalculationPlugin):
    def outputs(self): return []
    def compute(self, session): return None


for cls in (PyGyroStats, PyPartial, PyEffectiveProbe, PyBundleRaises, PyWrongKey,
            PyNotDict, PyBadMember, PySourceOutput, PyNoOutputs):
    register_calculation(cls())
