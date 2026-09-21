"""Plugins over the outputs of an EXPLICIT calculation (the synthetic expA of
tests/support/fakesessionstate.h, registered by the test function only).
A plugin is an ordinary on-demand reader: it never starts explicit work.
(Named to sort after t_zdocs, so it does not shift the registration indices of the others.)"""
from flysight_plugin_sdk import AttributePlugin, attr, register_attribute


class PyExplicitDirect(AttributePlugin):
    """Declares an output of the explicit calculation itself."""
    name = "_PY_EXP_DIRECT"

    def inputs(self):
        return [attr("EA1")]

    def compute(self, session):
        return float(session.getAttribute("EA1")) + 0.5


class PyExplicitDerived(AttributePlugin):
    """Declares an on-demand value derived from the explicit calculation (DA = EA1 + 100)."""
    name = "_PY_EXP_DERIVED"

    def inputs(self):
        return [attr("DA")]

    def compute(self, session):
        return float(session.getAttribute("DA")) + 0.5


PROBE_CALLS = 0


class PyExplicitProbe(AttributePlugin):
    """Runs whenever EA_IN exists and then reaches for the explicit output WITHOUT
    declaring it, swallowing the error: Python code really executes the read."""
    name = "_PY_EXP_PROBE"

    def inputs(self):
        return [attr("EA_IN")]

    def compute(self, session):
        global PROBE_CALLS
        PROBE_CALLS += 1
        for key in ("EA1", "DA"):
            try:
                session.getAttribute(key)
            except Exception:
                pass
        return 1.0


register_attribute(PyExplicitDirect())
register_attribute(PyExplicitDerived())
register_attribute(PyExplicitProbe())
