"""Plugins whose declarations are invalid: each is rejected alone."""
from flysight_plugin_sdk import AttributePlugin, Key, meas, register_attribute


class PyBogusKind(AttributePlugin):
    name = "_PY_BOGUS"
    def inputs(self): return [Key("bogus", "IMU", "wx")]
    def compute(self, session): return 1.0


class PyPrefKind(AttributePlugin):
    name = "_PY_PREF"
    def inputs(self): return [Key("preference", "", "general/units")]
    def compute(self, session): return 1.0


class PyIntKey(AttributePlugin):
    name = "_PY_INTKEY"
    def inputs(self): return [42]
    def compute(self, session): return 1.0


class PyNonStrKind(AttributePlugin):
    name = "_PY_NONSTR"
    def inputs(self): return [Key(1, "IMU", "wx")]
    def compute(self, session): return 1.0


class PyEmptyField(AttributePlugin):
    name = "_PY_EMPTY"
    def inputs(self): return [meas("IMU", "")]
    def compute(self, session): return 1.0


class PyInputsRaises(AttributePlugin):
    name = "_PY_INRAISE"
    def inputs(self): raise RuntimeError("nope")
    def compute(self, session): return 1.0


class PyNoName(AttributePlugin):
    def compute(self, session): return 1.0


class PyGoodNeighbour(AttributePlugin):
    name = "_PY_GOOD"
    def compute(self, session): return 1.0


for cls in (PyBogusKind, PyPrefKind, PyIntKey, PyNonStrKind, PyEmptyField,
            PyInputsRaises, PyNoName, PyGoodNeighbour):
    register_attribute(cls())
