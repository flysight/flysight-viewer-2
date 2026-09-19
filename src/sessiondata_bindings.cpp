#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include <memory>
#include <QString>
#include <QVariant>
#include "pluginsessionview.h"

namespace py = pybind11;
using namespace FlySight;

// The object a plugin's compute() receives. Its Python name stays
// "SessionData"; on the C++ side it is a view over the inputs the plugin
// declared (see pluginsessionview.h). Plugins return results; they cannot
// publish values into the session.
void register_sessiondata(py::module_ &m) {
    py::class_<PluginSessionView, std::shared_ptr<PluginSessionView>>(m, "SessionData")
        // session.getMeasurement(sensor, measurement) → list[float]
        .def("getMeasurement",
             [](PluginSessionView &self,
                std::string sensor,
                std::string measurement)
             {
                 auto qv = self.getMeasurement(
                     QString::fromStdString(sensor),
                     QString::fromStdString(measurement));
                 return std::vector<double>(qv.begin(), qv.end());
             },
             py::arg("sensorKey"),
             py::arg("measurementKey"))

        // session.getAttribute(key) → float, str, or None
        .def("getAttribute",
             [](PluginSessionView &self, std::string key) -> py::object {
                 QVariant v = self.getAttribute(QString::fromStdString(key));
                 if (!v.isValid()) {
                     return py::none();
                 }
                 // numeric?
                 bool ok = false;
                 double d = v.toDouble(&ok);
                 if (ok) {
                     return py::cast(d);
                 }
                 // plain string?
                 if (v.type() == QVariant::String) {
                     return py::cast(v.toString().toStdString());
                 }
                 return py::none();
             },
             py::arg("key"))

        // Optional helpers in Python if you want them
        .def("hasMeasurement",
             [](PluginSessionView &self,
                std::string sensor,
                std::string measurement)
             {
                 return self.hasMeasurement(
                     QString::fromStdString(sensor),
                     QString::fromStdString(measurement));
             },
             py::arg("sensorKey"),
             py::arg("measurementKey"))

        .def("hasAttribute",
             [](PluginSessionView &self, std::string key) {
                 return self.hasAttribute(QString::fromStdString(key));
             },
             py::arg("key"))
        ;
}
