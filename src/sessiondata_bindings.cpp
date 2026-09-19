#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <cstring>
#include <memory>
#include <string>
#include <QString>
#include <QVariant>
#include <QVector>
#include "pluginsessionview.h"

namespace py = pybind11;
using namespace FlySight;

namespace {

// A new 1-D float64 array that OWNS A COPY of the samples. The QVector buffer
// is implicitly shared with the source layer and the engine's cache, so it is
// never exposed to Python, writable or not.
py::array_t<double> copyToArray(const QVector<double> &values)
{
    py::array_t<double> out(static_cast<py::ssize_t>(values.size()));
    if (!values.isEmpty())
        std::memcpy(out.mutable_data(), values.constData(), size_t(values.size()) * sizeof(double));
    return out;
}

QString qs(const std::string &s) { return QString::fromStdString(s); }

} // namespace

// The object a plugin's compute() receives. Its Python name stays
// "SessionData"; on the C++ side it is a view over the inputs the plugin
// declared (see pluginsessionview.h). Plugins return results; nothing here can
// write to a session, the engine, or the registry.
//
// Everything in this file runs inside the bridge module, which has its own
// copy of every flysight_model static: it only ever touches the
// EvaluationContext behind the view it was handed.
void register_sessiondata(py::module_ &m) {
    // Undeclared reads surface in Python at the offending line. A stale view
    // (StaleSessionViewError) needs no registration: pybind11 maps any
    // std::runtime_error to RuntimeError.
    py::register_exception<UndeclaredInputError>(m, "UndeclaredInputError", PyExc_RuntimeError);

    py::class_<PluginSessionView, std::shared_ptr<PluginSessionView>>(m, "SessionData")
        // ---- effective layer: the key must be a declared attr() / meas() input

        // session.getMeasurement(sensor, measurement) -> numpy.ndarray (float64, private copy)
        .def("getMeasurement",
             [](PluginSessionView &self, const std::string &sensor, const std::string &measurement) {
                 return copyToArray(self.getMeasurement(qs(sensor), qs(measurement)));
             },
             py::arg("sensorKey"),
             py::arg("measurementKey"))

        // session.getAttribute(key) -> float, str, or None
        .def("getAttribute",
             [](PluginSessionView &self, const std::string &key) -> py::object {
                 const QVariant v = self.getAttribute(qs(key));
                 if (!v.isValid()) {
                     return py::none();
                 }
                 // numeric? (includes numeric-looking text, which is how
                 // attributes loaded from a file arrive)
                 bool ok = false;
                 const double d = v.toDouble(&ok);
                 if (ok) {
                     return py::cast(d);
                 }
                 // plain string?
                 if (v.typeId() == QMetaType::QString) {
                     return py::cast(v.toString().toStdString());
                 }
                 return py::none();
             },
             py::arg("key"))

        // session.effectiveUnit(sensor, measurement) -> str
        .def("effectiveUnit",
             [](PluginSessionView &self, const std::string &sensor, const std::string &measurement) {
                 return self.effectiveUnit(qs(sensor), qs(measurement)).toStdString();
             },
             py::arg("sensorKey"),
             py::arg("measurementKey"))

        .def("hasMeasurement",
             [](PluginSessionView &self, const std::string &sensor, const std::string &measurement) {
                 return self.hasMeasurement(qs(sensor), qs(measurement));
             },
             py::arg("sensorKey"),
             py::arg("measurementKey"))

        .def("hasAttribute",
             [](PluginSessionView &self, const std::string &key) {
                 return self.hasAttribute(qs(key));
             },
             py::arg("key"))

        // ---- source layer: the key must be a declared source() input

        // session.sourceMeasurement(sensor, measurement) -> numpy.ndarray (float64, private copy)
        .def("sourceMeasurement",
             [](PluginSessionView &self, const std::string &sensor, const std::string &measurement) {
                 return copyToArray(self.sourceMeasurement(qs(sensor), qs(measurement)));
             },
             py::arg("sensorKey"),
             py::arg("measurementKey"))

        // session.sourceUnit(sensor, measurement) -> str (the recorded unit text; may be "")
        .def("sourceUnit",
             [](PluginSessionView &self, const std::string &sensor, const std::string &measurement) {
                 return self.sourceUnit(qs(sensor), qs(measurement)).toStdString();
             },
             py::arg("sensorKey"),
             py::arg("measurementKey"))

        .def("hasSourceMeasurement",
             [](PluginSessionView &self, const std::string &sensor, const std::string &measurement) {
                 return self.hasSourceMeasurement(qs(sensor), qs(measurement));
             },
             py::arg("sensorKey"),
             py::arg("measurementKey"))
        ;
}
