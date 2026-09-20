/*****************************************************************************
 *  FlySight Viewer - Python plugin adapters
 *
 *  Key decoding, result marshalling, and the calculation descriptors that wrap
 *  Python plugin objects. See pluginadapters.h for the rules of the road; the
 *  failure model in one line: a plugin calculation either publishes a fully
 *  validated bundle or nothing, and no Python exception ever reaches the engine.
 *****************************************************************************/

#pragma push_macro("slots")
#undef  slots
#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#pragma pop_macro("slots")

#include <cmath>
#include <cstring>
#include <string>

#include <QDebug>
#include <QHash>

#include "pluginadapters.h"
#include "pluginsessionview.h"

namespace py = pybind11;

namespace FlySight::PluginBridge {

namespace {

// The strings of flysight_plugin_sdk.KIND_*. "preference" is named only so
// that its rejection is specific.
constexpr const char *kKindAttribute   = "attribute";
constexpr const char *kKindMeasurement = "measurement";
constexpr const char *kKindPreference  = "preference";

PluginError pluginError(const QString &message)
{
    return PluginError(message.toStdString());
}

// repr(obj) for messages; never throws.
QString safeRepr(py::handle obj)
{
    try {
        return QString::fromStdString(py::repr(obj).cast<std::string>());
    } catch (...) {
        PyErr_Clear();
        return QStringLiteral("<unprintable>");
    }
}

QString typeName(py::handle obj)
{
    try {
        return QString::fromStdString(py::type::of(obj).attr("__name__").cast<std::string>());
    } catch (...) {
        PyErr_Clear();
        return QStringLiteral("<unknown type>");
    }
}

bool isExactStr(py::handle obj)
{
    return obj && PyUnicode_CheckExact(obj.ptr());
}

QString toQString(py::handle str)
{
    return QString::fromStdString(str.cast<std::string>());
}

// An exact-str attribute of a plugin object (name, sensor), non-empty.
QString requiredStrAttribute(py::handle plugin, const char *attribute)
{
    if (!py::hasattr(plugin, attribute))
        throw pluginError(QStringLiteral("the plugin has no '%1' attribute").arg(QLatin1String(attribute)));
    const py::object value = plugin.attr(attribute);
    if (!isExactStr(value))
        throw pluginError(QStringLiteral("the plugin's '%1' must be a str, got %2")
                              .arg(QLatin1String(attribute), safeRepr(value)));
    const QString text = toQString(value);
    if (text.isEmpty())
        throw pluginError(QStringLiteral("the plugin's '%1' is empty").arg(QLatin1String(attribute)));
    return text;
}

// A list or tuple returned by inputs() / outputs().
py::sequence requireSequence(py::handle value, const char *method)
{
    if (!value || !(PyList_Check(value.ptr()) || PyTuple_Check(value.ptr())))
        throw pluginError(QStringLiteral("%1() must return a list or tuple of keys, got %2")
                              .arg(QLatin1String(method), safeRepr(value)));
    return py::reinterpret_borrow<py::sequence>(value);
}

DependencyKey toOutputKey(const DecodedKey &key)
{
    switch (key.kind) {     // no default: every KeyKind is handled explicitly
    case KeyKind::Attribute:
        return DependencyKey::attribute(key.name);
    case KeyKind::Measurement:
        break;
    }
    return DependencyKey::measurement(key.sensor, key.name);
}

// "<calculation id> (<module>.<qualname>)": what log lines and Python errors name.
QString viewLabel(const QString &id, py::handle plugin)
{
    return id + QStringLiteral(" (") + pluginLabel(plugin) + QLatin1Char(')');
}

// Invalidates the view on every exit path, so a plugin that keeps the object
// never holds a pointer to a dead evaluation context.
struct ViewGuard {
    explicit ViewGuard(std::shared_ptr<PluginSessionView> v) : view(std::move(v)) {}
    ~ViewGuard() { view->invalidate(); }
    ViewGuard(const ViewGuard &) = delete;
    ViewGuard &operator=(const ViewGuard &) = delete;
    std::shared_ptr<PluginSessionView> view;
};

// The skeleton shared by the three adapter forms. `convert` turns the Python
// return value into the complete CalculationResult, throwing PluginError on a
// malformed value; nothing is returned from a partially validated result.
template <typename Convert>
CalculationResult runPlugin(const std::shared_ptr<PyPluginHolder> &holder, const QString &label,
                            const EvaluationContext &ctx, Convert convert)
{
    py::gil_scoped_acquire gil;
    ViewGuard guard(std::make_shared<PluginSessionView>(&ctx, label));
    try {
        const py::object out = holder->plugin.attr("compute")(guard.view);
        return convert(out);
    } catch (py::error_already_set &e) {
        // Logged, converted, and destroyed inside the GIL scope: what crosses
        // into the engine holds no Python reference.
        const bool undeclared = e.matches(holder->undeclaredInputError);
        const QString what = QString::fromUtf8(e.what());     // type, message, traceback
        qWarning().noquote() << "[PluginHost]" << label << "raised:" << what;
        if (undeclared)
            return CalculationResult::unavailable();        // the engine publishes UndeclaredRead
        throw pluginError(what.section(QLatin1Char('\n'), 0, 0));   // the engine publishes Failed
    } catch (const PluginError &e) {
        qWarning().noquote() << "[PluginHost]" << label << "returned malformed output:" << e.what();
        throw;                                              // the engine publishes Failed
    } catch (const std::exception &e) {
        // pybind11's own C++ errors (cast_error, ...): same outcome as a raise.
        qWarning().noquote() << "[PluginHost]" << label << "failed:" << e.what();
        throw pluginError(QString::fromUtf8(e.what()));
    }
}

} // namespace

// ------------------------------------------------------------------ decoding

QString DecodedKey::display() const
{
    switch (kind) {
    case KeyKind::Attribute:   return QStringLiteral("attribute ") + name;
    case KeyKind::Measurement: return QStringLiteral("measurement ") + sensor + QLatin1Char('/') + name;
    }
    return QString();
}

DecodedKey decodeKey(py::handle key)
{
    if (!key || !py::hasattr(key, "kind") || !py::hasattr(key, "sensor") || !py::hasattr(key, "name"))
        throw pluginError(QStringLiteral("dependency key %1 is not a flysight_plugin_sdk.Key").arg(safeRepr(key)));

    const py::object kindObj = key.attr("kind");
    const py::object sensorObj = key.attr("sensor");
    const py::object nameObj = key.attr("name");

    if (!isExactStr(kindObj))
        throw pluginError(QStringLiteral("unknown dependency kind %1").arg(safeRepr(kindObj)));
    if (!isExactStr(sensorObj) || !isExactStr(nameObj))
        throw pluginError(QStringLiteral("dependency key %1: 'sensor' and 'name' must be str").arg(safeRepr(key)));

    const QString kind = toQString(kindObj);
    DecodedKey decoded;
    decoded.sensor = toQString(sensorObj);
    decoded.name = toQString(nameObj);

    // The decoding table. There is deliberately no branch that produces a key
    // for a kind it does not recognise.
    if (kind == QLatin1String(kKindAttribute)) {
        decoded.kind = KeyKind::Attribute;
        if (decoded.name.isEmpty())
            throw pluginError(QStringLiteral("dependency key %1 has an empty name").arg(safeRepr(key)));
        if (!decoded.sensor.isEmpty())
            throw pluginError(QStringLiteral("attribute key %1 must have an empty sensor").arg(safeRepr(key)));
    } else if (kind == QLatin1String(kKindMeasurement)) {
        decoded.kind = KeyKind::Measurement;
        if (decoded.sensor.isEmpty() || decoded.name.isEmpty())
            throw pluginError(QStringLiteral("dependency key %1 has an empty sensor or name").arg(safeRepr(key)));
    } else if (kind == QLatin1String(kKindPreference)) {
        throw pluginError(QStringLiteral("preference inputs are not available to plugins"));
    } else {
        throw pluginError(QStringLiteral("unknown dependency kind %1").arg(safeRepr(kindObj)));
    }
    return decoded;
}

QList<CalcInput> decodeInputs(py::handle inputsResult)
{
    QList<CalcInput> inputs;
    for (py::handle item : requireSequence(inputsResult, "inputs")) {
        const DecodedKey key = decodeKey(item);
        switch (key.kind) {     // no default: every KeyKind is handled explicitly
        case KeyKind::Attribute:
            inputs.append(CalcInput::attribute(key.name));
            break;
        case KeyKind::Measurement:
            inputs.append(CalcInput::measurement(key.sensor, key.name));
            break;
        }
    }
    return inputs;
}

QList<DependencyKey> decodeOutputs(py::handle outputsResult)
{
    QList<DependencyKey> outputs;
    for (py::handle item : requireSequence(outputsResult, "outputs"))
        outputs.append(toOutputKey(decodeKey(item)));
    if (outputs.isEmpty())
        throw pluginError(QStringLiteral("outputs() must declare at least one attr() or meas() key"));
    return outputs;
}

QString pluginLabel(py::handle plugin)
{
    try {
        const py::object type = py::type::of(plugin);
        return toQString(py::str(type.attr("__module__"))) + QLatin1Char('.')
             + toQString(py::str(type.attr("__qualname__")));
    } catch (...) {
        PyErr_Clear();
        return QStringLiteral("<unknown>");
    }
}

// --------------------------------------------------------------- marshalling

bool toAttributeValue(py::handle value, py::handle numbersReal, QVariant *out)
{
    if (!value || value.is_none())
        return false;

    // bool is an int in Python: test it first.
    if (PyBool_Check(value.ptr()))
        throw pluginError(QStringLiteral("bool is not a valid attribute value (return a float, int, or str)"));

    // A string is a string; it is never parsed as a number or a date.
    if (PyUnicode_Check(value.ptr())) {
        *out = QVariant(toQString(value));
        return true;
    }

    const bool real = PyFloat_Check(value.ptr()) || PyLong_Check(value.ptr())
                      || (numbersReal && py::isinstance(value, numbersReal));   // np.float64, np.int64, ...
    if (!real)
        throw pluginError(QStringLiteral("%1 is not a valid attribute value (return a float, int, str, or None)")
                              .arg(typeName(value)));

    const double d = PyFloat_AsDouble(value.ptr());
    if (d == -1.0 && PyErr_Occurred()) {
        PyErr_Clear();
        throw pluginError(QStringLiteral("attribute value %1 cannot be converted to a float").arg(safeRepr(value)));
    }
    if (!std::isfinite(d))
        return false;       // NaN / inf: no value
    *out = QVariant(d);
    return true;
}

bool toMeasurementValues(py::handle value, QVector<double> *out)
{
    if (!value || value.is_none())
        return false;

    // Before NumPy would try to parse them.
    if (PyUnicode_Check(value.ptr()) || PyBytes_Check(value.ptr()))
        throw pluginError(QStringLiteral("%1 is not a valid measurement (return a 1-D numeric array)")
                              .arg(typeName(value)));

    const py::array a = py::array::ensure(value);
    if (!a) {
        PyErr_Clear();
        throw pluginError(QStringLiteral("%1 cannot be converted to a NumPy array").arg(typeName(value)));
    }

    // Checked before forcecast, which would silently turn bool into 0/1.
    const char kind = a.dtype().kind();
    if (kind != 'f' && kind != 'i' && kind != 'u')
        throw pluginError(QStringLiteral("dtype %1 is not numeric")
                              .arg(toQString(py::str(a.dtype()))));

    if (a.ndim() != 1)
        throw pluginError(QStringLiteral("expected a 1-D array, got %1-D").arg(a.ndim()));

    const auto d = py::array_t<double, py::array::c_style | py::array::forcecast>::ensure(a);
    if (!d) {
        PyErr_Clear();
        throw pluginError(QStringLiteral("the array cannot be converted to float64"));
    }

    // The copy is complete before the adapter returns.
    const qsizetype n = static_cast<qsizetype>(d.shape(0));
    out->resize(n);
    if (n > 0)
        std::memcpy(out->data(), d.data(), size_t(n) * sizeof(double));
    return true;
}

// ------------------------------------------------------------------ adapters

std::shared_ptr<PyPluginHolder> makeHolder(py::object plugin)
{
    auto *holder = new PyPluginHolder;
    std::shared_ptr<PyPluginHolder> shared(holder, [](PyPluginHolder *p) {
        if (Py_IsInitialized()) {
            py::gil_scoped_acquire gil;
            delete p;
        } else {
            // The interpreter is gone: dropping a reference now would crash.
            p->plugin.release();
            p->undeclaredInputError.release();
            p->numbersReal.release();
            delete p;
        }
    });
    holder->plugin = std::move(plugin);
    holder->undeclaredInputError = py::module_::import("flysight_cpp_bridge").attr("UndeclaredInputError");
    holder->numbersReal = py::module_::import("numbers").attr("Real");
    return shared;
}

CalculationDescriptor makeAttributeAdapter(int index, py::object plugin)
{
    const QString key = requiredStrAttribute(plugin, "name");

    CalculationDescriptor d;
    d.id = QStringLiteral("plugin.attr.%1.%2").arg(index).arg(key);
    d.inputs = decodeInputs(plugin.attr("inputs")());
    d.outputs = { DependencyKey::attribute(key) };

    const QString label = viewLabel(d.id, plugin);
    const std::shared_ptr<PyPluginHolder> holder = makeHolder(std::move(plugin));
    d.compute = [holder, label, key](const EvaluationContext &ctx) -> CalculationResult {
        return runPlugin(holder, label, ctx, [&](const py::object &out) {
            CalculationResult result;
            QVariant value;
            if (toAttributeValue(out, holder->numbersReal, &value))
                result.setAttribute(key, value);
            return result;
        });
    };
    return d;
}

CalculationDescriptor makeMeasurementAdapter(int index, py::object plugin)
{
    const QString sensor = requiredStrAttribute(plugin, "sensor");
    const QString name = requiredStrAttribute(plugin, "name");

    // `units` is read once, here; it becomes the effective unit of the output.
    QString unit;
    const py::object unitsObj = py::getattr(plugin, "units", py::none());
    if (!unitsObj.is_none()) {
        if (!isExactStr(unitsObj))
            throw pluginError(QStringLiteral("the plugin's 'units' must be a str or None, got %1")
                                  .arg(safeRepr(unitsObj)));
        unit = toQString(unitsObj);
    }

    CalculationDescriptor d;
    d.id = QStringLiteral("plugin.meas.%1.%2/%3").arg(index).arg(sensor, name);
    d.inputs = decodeInputs(plugin.attr("inputs")());
    d.outputs = { DependencyKey::measurement(sensor, name) };

    const QString label = viewLabel(d.id, plugin);
    const std::shared_ptr<PyPluginHolder> holder = makeHolder(std::move(plugin));
    d.compute = [holder, label, sensor, name, unit](const EvaluationContext &ctx) -> CalculationResult {
        return runPlugin(holder, label, ctx, [&](const py::object &out) {
            CalculationResult result;
            QVector<double> values;
            if (toMeasurementValues(out, &values))
                result.setMeasurement(sensor, name, values, unit);
            return result;
        });
    };
    return d;
}

CalculationDescriptor makeCalculationAdapter(int index, py::object plugin)
{
    // `name` is optional; it defaults to the class name.
    QString name;
    if (py::hasattr(plugin, "name"))
        name = requiredStrAttribute(plugin, "name");
    else
        name = toQString(py::str(py::type::of(plugin).attr("__name__")));

    CalculationDescriptor d;
    d.id = QStringLiteral("plugin.calc.%1.%2").arg(index).arg(name);
    d.inputs = decodeInputs(plugin.attr("inputs")());
    d.outputs = decodeOutputs(plugin.attr("outputs")());

    // `units`: a unit label per *measurement* output, read once, here.
    QHash<DependencyKey, QString> units;
    const py::object unitsObj = py::getattr(plugin, "units", py::dict());
    if (!unitsObj || !PyDict_Check(unitsObj.ptr()))
        throw pluginError(QStringLiteral("the plugin's 'units' must be a dict of meas() key -> str, got %1")
                              .arg(safeRepr(unitsObj)));
    for (const auto &item : py::reinterpret_borrow<py::dict>(unitsObj)) {
        const DecodedKey key = decodeKey(item.first);
        const DependencyKey output = toOutputKey(key);
        if (key.kind != KeyKind::Measurement || !d.outputs.contains(output))
            throw pluginError(QStringLiteral("'units' names %1, which is not a declared measurement output")
                                  .arg(key.display()));
        if (!isExactStr(item.second))
            throw pluginError(QStringLiteral("the unit of %1 must be a str, got %2")
                                  .arg(key.display(), safeRepr(item.second)));
        units.insert(output, toQString(item.second));
    }

    const QString label = viewLabel(d.id, plugin);
    const QList<DependencyKey> outputs = d.outputs;
    const std::shared_ptr<PyPluginHolder> holder = makeHolder(std::move(plugin));
    d.compute = [holder, label, outputs, units](const EvaluationContext &ctx) -> CalculationResult {
        return runPlugin(holder, label, ctx, [&](const py::object &out) {
            // Built completely in a local: nothing is published from a bundle
            // that did not validate as a whole.
            CalculationResult result;
            if (!out.is_none()) {
                if (!PyDict_Check(out.ptr()))
                    throw pluginError(QStringLiteral("expected a dict keyed by attr() / meas() keys (or None), got %1")
                                          .arg(typeName(out)));

                for (const auto &item : py::reinterpret_borrow<py::dict>(out)) {
                    const DecodedKey key = decodeKey(item.first);
                    const DependencyKey output = toOutputKey(key);
                    if (!outputs.contains(output))
                        throw pluginError(QStringLiteral("the result names %1, which is not in outputs()")
                                              .arg(key.display()));
                    try {
                        if (key.kind == KeyKind::Attribute) {
                            QVariant value;
                            if (toAttributeValue(item.second, holder->numbersReal, &value))
                                result.setAttribute(key.name, value);
                        } else {
                            QVector<double> values;
                            if (toMeasurementValues(item.second, &values))
                                result.setMeasurement(key.sensor, key.name, values, units.value(output));
                        }
                    } catch (const PluginError &e) {
                        throw pluginError(key.display() + QStringLiteral(": ") + QString::fromUtf8(e.what()));
                    }
                }
            }
            return result;      // None, or a missing / None entry: that output is unavailable
        });
    };
    return d;
}

} // namespace FlySight::PluginBridge
