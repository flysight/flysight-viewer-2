#include <pybind11/pybind11.h>
#include "python_output_redirector.h"

namespace py = pybind11;
using namespace FlySight;

// flysight_cpp_bridge: the types the embedded interpreter needs from C++.
//
// The module exports exactly:
//   SessionData                 - the read-only view a plugin's compute() receives
//                                 (sessiondata_bindings.cpp / pluginsessionview.h)
//   UndeclaredInputError        - raised by that view for a read outside inputs()
//   PythonOutputRedirector_CPP  - sys.stdout / sys.stderr -> qDebug()
//
// Dependency keys are NOT bound here: they are the pure-Python `Key` dataclass
// of flysight_plugin_sdk, decoded by the host (pluginadapters.cpp).
//
// This module has its own copy of every flysight_model static. Code compiled
// into it must never reach the calculation registry, an engine, or a session;
// it only operates on the objects it is handed.

// Defined in sessiondata_bindings.cpp
void register_sessiondata(py::module_ &m);

// Register PythonOutputRedirector
void register_python_output_redirector(py::module_ &m) {
    py::class_<PythonOutputRedirector>(m, "PythonOutputRedirector_CPP")
        .def(py::init<>())
        .def("write", &PythonOutputRedirector::write, py::arg("message"))
        .def("flush", &PythonOutputRedirector::flush);
}

#pragma push_macro("slots")
#undef slots
PYBIND11_MODULE(flysight_cpp_bridge, m) {
    m.doc() = "C++ bridge module for FlySight Python plugins: the SessionData view, "
              "UndeclaredInputError, and the stdout/stderr redirector";

    register_sessiondata(m);
    register_python_output_redirector(m);
}
#pragma pop_macro("slots")
