#ifndef PLUGINADAPTERS_H
#define PLUGINADAPTERS_H

/*  Python plugin adapters: key decoding, result marshalling, and the
 *  CalculationDescriptors that wrap Python plugin objects.
 *
 *  Compiled into the application (and the embedded-Python test), NEVER into the
 *  flysight_cpp_bridge module: the descriptors built here are registered with
 *  the process's CalculationRegistry, and the bridge has its own copy of every
 *  flysight_model static.
 */

#include <memory>
#include <stdexcept>

// pybind11 must not see Qt's `slots` macro, whatever the includer's order was.
#pragma push_macro("slots")
#undef  slots
#include <pybind11/pybind11.h>
#pragma pop_macro("slots")

#include <QList>
#include <QString>
#include <QVariant>
#include <QVector>

#include "dependencykey.h"
#include "engine/calculationdescriptor.h"

namespace FlySight::PluginBridge {

/// A problem with one plugin: a bad declaration at registration, or a
/// malformed return value at compute time. The message is user-facing. This is
/// the only exception the adapters let escape into the engine; it holds no
/// Python reference.
struct PluginError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class KeyKind { Attribute, Measurement, Source };

/// A flysight_plugin_sdk.Key, decoded.
struct DecodedKey {
    KeyKind kind;
    QString sensor;     ///< empty for attributes
    QString name;       ///< attribute key when kind == Attribute

    QString display() const;    ///< "attribute _X" / "measurement IMU/wx" / "source IMU/wx"
};

/// The only place a Python dependency kind is interpreted. Throws PluginError
/// for anything that is not a well-formed attribute / measurement / source key.
DecodedKey decodeKey(pybind11::handle key);

/// `inputsResult` is what plugin.inputs() returned: a list or tuple of Key.
/// A source key becomes two inputs (samples, then unit text) and sets *usesSource.
QList<CalcInput> decodeInputs(pybind11::handle inputsResult, bool *usesSource);

/// `outputsResult` is what plugin.outputs() returned: a non-empty list or tuple
/// of attribute / measurement keys.
QList<DependencyKey> decodeOutputs(pybind11::handle outputsResult);

/// "<module>.<qualname>" of the plugin's class; "<unknown>" if even that fails.
QString pluginLabel(pybind11::handle plugin);

// ---- return-value marshalling. Both throw PluginError on a malformed value
// and return false for "no value" (None; a non-finite attribute).

/// `numbersReal` is the numbers.Real ABC (see PyPluginHolder).
bool toAttributeValue(pybind11::handle value, pybind11::handle numbersReal, QVariant *out);
/// Copies the samples; nothing in *out references NumPy memory.
bool toMeasurementValues(pybind11::handle value, QVector<double> *out);

// ---- adapters

/// Everything Python that a registered compute function keeps alive. The
/// functions live in CalculationRegistry::instance(), whose destruction order
/// relative to the interpreter is not guaranteed, so they capture a shared_ptr
/// to this holder, whose deleter only touches Python while it is still
/// initialised. No py::object is ever captured by value.
struct PyPluginHolder {
    pybind11::object plugin;
    pybind11::object undeclaredInputError;  ///< flysight_cpp_bridge.UndeclaredInputError
    pybind11::object numbersReal;           ///< numbers.Real
};
std::shared_ptr<PyPluginHolder> makeHolder(pybind11::object plugin);

/// AttributePlugin -> "plugin.attr.<index>.<name>", one attribute output.
CalculationDescriptor makeAttributeAdapter(int index, pybind11::object plugin);
/// MeasurementPlugin -> "plugin.meas.<index>.<sensor>/<name>", one measurement output.
CalculationDescriptor makeMeasurementAdapter(int index, pybind11::object plugin);
/// CalculationPlugin -> "plugin.calc.<index>.<name>", a bundle of declared outputs.
CalculationDescriptor makeCalculationAdapter(int index, pybind11::object plugin);

} // namespace FlySight::PluginBridge

#endif // PLUGINADAPTERS_H
