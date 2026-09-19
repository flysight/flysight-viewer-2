/*****************************************************************************
 *  FlySight Viewer – Plugin host
 *
 *  Boots CPython (via pybind11) and registers Python-side plug-ins.
 *  • If <exe>/python exists  → use the bundled “embeddable” runtime.
 *  • Otherwise               → fall back to the developer’s system Python.
 *****************************************************************************/

/* Qt headers – compile with _DEBUG still defined */
#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <QVariant>
#include <QDebug>

/* STL */
#include <memory>
#include <optional>
#include <string>

#pragma push_macro("slots")
#undef  slots                    // avoid Qt's `slots` macro clash

/* Python headers */
#if defined(_MSC_VER)            // mask _DEBUG only while including Python.h
#  pragma push_macro("_DEBUG")
#  undef  _DEBUG
#endif
#include <Python.h>              // PEP-587 API (needs release mode)
#if defined(_MSC_VER)
#  pragma pop_macro("_DEBUG")
#endif

#include <pybind11/embed.h>      // pybind11 does its own _DEBUG masking
#include <pybind11/numpy.h>
#pragma pop_macro("slots")

/* FlySight headers */
#include "pluginhost.h"
#include "pluginsessionview.h"
#include "dependencykey.h"
#include "engine/calculationdescriptor.h"
#include "engine/calculationregistry.h"
#include "plotregistry.h"
#include "markerregistry.h"
#include "python_output_redirector.h"

namespace py = pybind11;
using   namespace FlySight;

/* ────────────────────────────────────────────────────────────────────────────
 *  Utility
 * ────────────────────────────────────────────────────────────────────────── */
static QString pyStatusToString(const PyStatus& s)
{
    return PyStatus_Exception(s) ?
               QString::fromUtf8(s.err_msg ? s.err_msg : "unknown") :
               QStringLiteral("success");
}

/* ------------------------------------------------------------------------
 *  Calculation adapters
 * ---------------------------------------------------------------------- */

// The declared inputs of a plug-in: the DependencyKey objects returned by its
// inputs() method, decoded by their .kind.
static QList<CalcInput> decodeInputs(const py::object& plugin)
{
    QList<CalcInput> inputs;
    for (py::handle hi : plugin.attr("inputs")().cast<py::list>()) {
        py::object dk = hi.cast<py::object>();
        int kind      = dk.attr("kind").cast<int>();

        if (kind == static_cast<int>(DependencyKey::Type::Attribute)) {
            inputs.append(CalcInput::attribute(
                QString::fromStdString(dk.attr("attributeKey").cast<std::string>())));
        } else {
            inputs.append(CalcInput::measurement(
                QString::fromStdString(dk.attr("sensorKey").cast<std::string>()),
                QString::fromStdString(dk.attr("measurementKey").cast<std::string>())));
        }
    }
    return inputs;
}

// The "session" handed to one compute() call. The view is made inert when the
// call ends, whether it returns or throws, so a plug-in that keeps the object
// never holds a pointer to a dead evaluation context.
struct ViewScope {
    explicit ViewScope(const EvaluationContext* ctx)
        : view(std::make_shared<PluginSessionView>(ctx)) {}
    ~ViewScope() { view->invalidate(); }
    ViewScope(const ViewScope&) = delete;
    ViewScope& operator=(const ViewScope&) = delete;

    std::shared_ptr<PluginSessionView> view;
};

/* ────────────────────────────────────────────────────────────────────────────
 *  Singleton
 * ────────────────────────────────────────────────────────────────────────── */
PluginHost& PluginHost::instance()
{
    static PluginHost inst;
    return inst;
}

/* ────────────────────────────────────────────────────────────────────────────
 *  Initialise interpreter and load plug-ins
 * ────────────────────────────────────────────────────────────────────────── */
void PluginHost::initialise(const QString& pluginDir)
{
    if (m_interp)                       // already initialised
        return;

    const QString appDir = QCoreApplication::applicationDirPath();

    /* Determine embedded Python location based on platform:
     * - Windows:        <exe>/python
     * - Linux AppImage: <exe>/../share/python (appDir is AppDir/usr/bin)
     * - Linux dev:      <exe>/python
     * - macOS bundle:   Contents/Resources/python (appDir is Contents/MacOS)
     * - macOS dev:      <exe>/python
     */
    QString embedDir;
#ifdef Q_OS_MACOS
    // On macOS, check for bundle structure first
    const QString bundlePythonDir = QDir(appDir).filePath(QStringLiteral("../Resources/python"));
    if (QDir(bundlePythonDir).exists()) {
        embedDir = QDir(bundlePythonDir).canonicalPath();
    } else {
        // Fall back to same-directory layout for development builds
        embedDir = QDir(appDir).filePath(QStringLiteral("python"));
    }
#elif defined(Q_OS_LINUX)
    // On Linux AppImage, Python is in usr/share/python relative to usr/bin
    const QString appImagePythonDir = QDir(appDir).filePath(QStringLiteral("../share/python"));
    if (QDir(appImagePythonDir).exists()) {
        embedDir = QDir(appImagePythonDir).canonicalPath();
    } else {
        // Fall back to same-directory layout for development builds
        embedDir = QDir(appDir).filePath(QStringLiteral("python"));
    }
#else
    // Windows: Python is in <exe>/python
    embedDir = QDir(appDir).filePath(QStringLiteral("python"));
#endif

    const bool useEmbed = QDir(embedDir).exists();

    /* ------------------------------------------------------------------ */
    /* 1. Boot CPython                                                    */
    /* ------------------------------------------------------------------ */
    try {
        if (useEmbed) {
            /* Build a PyConfig pointing at the bundled runtime */
            PyConfig cfg;
            PyConfig_InitPythonConfig(&cfg);          // "normal" configuration

            // PyConfig_SetString requires wide strings (wchar_t*) on all platforms
            const std::wstring wHome = embedDir.toStdWString();
            PyStatus st = PyConfig_SetString(&cfg, &cfg.home, wHome.c_str());
            if (PyStatus_Exception(st)) {
                qCritical().noquote() << "PyConfig_SetString failed:"
                                      << pyStatusToString(st);
                PyConfig_Clear(&cfg);
                return;
            }

            /* Add runtime-local import paths
             * Note: PyWideStringList_Append requires wide strings on all platforms.
             * PyStringList_Append does not exist in Python C API.
             *
             * Directory structures:
             * - Windows: <exe>/python/pythonXY.zip, <exe>/python/Lib/site-packages
             * - macOS/Linux: <python>/lib/python3.XX/, <python>/lib/python3.XX/site-packages
             */
#ifdef _WIN32
            // Build the python zip filename dynamically from the detected version
            // The embeddable package uses pythonXY.zip naming (e.g., python313.zip)
            const std::wstring pyVersion = std::to_wstring(PY_MAJOR_VERSION) + std::to_wstring(PY_MINOR_VERSION);
            const std::wstring zip  = wHome + L"\\python" + pyVersion + L".zip";
            // Site-packages is in Lib/site-packages for the embeddable package
            const std::wstring site = wHome + L"\\Lib\\site-packages";
            PyWideStringList_Append(&cfg.module_search_paths, wHome.c_str());
            PyWideStringList_Append(&cfg.module_search_paths, zip.c_str());
            PyWideStringList_Append(&cfg.module_search_paths, site.c_str());
#else
            // macOS bundle / Linux AppImage: Python stdlib in lib/python3.XX/
            // Build the path dynamically from the detected Python version
            const std::wstring pyVersionStr = L"python" + std::to_wstring(PY_MAJOR_VERSION) + L"." + std::to_wstring(PY_MINOR_VERSION);
            const std::wstring lib = wHome + L"/lib/" + pyVersionStr;
            const std::wstring zip = lib + L".zip";
            const std::wstring site = lib + L"/site-packages";
            const std::wstring libDynload = lib + L"/lib-dynload";  // C extension modules
            PyWideStringList_Append(&cfg.module_search_paths, wHome.c_str());
            PyWideStringList_Append(&cfg.module_search_paths, lib.c_str());
            PyWideStringList_Append(&cfg.module_search_paths, zip.c_str());
            PyWideStringList_Append(&cfg.module_search_paths, site.c_str());
            PyWideStringList_Append(&cfg.module_search_paths, libDynload.c_str());
#endif
            cfg.module_search_paths_set = 1;

#ifdef _WIN32
            /* Ensure Windows can locate .pyd extension modules */
            const QString newPath =
                qEnvironmentVariable("PATH") + u';' + embedDir;
            _wputenv_s(L"PATH", newPath.toStdWString().c_str());
#endif
            m_interp = std::make_unique<py::scoped_interpreter>(&cfg, 0, nullptr, false);
            PyConfig_Clear(&cfg);

            qInfo().noquote() << "[PluginHost] Embedded Python booted from"
                              << QDir::toNativeSeparators(embedDir);
        } else {
            m_interp = std::make_unique<py::scoped_interpreter>();
            qInfo() << "[PluginHost] Using system Python runtime.";
        }
    } catch (const py::error_already_set& e) {
        qCritical().noquote()
        << "[PluginHost] Python error while starting interpreter:" << e.what();
        return;
    } catch (const std::exception& e) {
        qCritical().noquote()
        << "[PluginHost] Failed to start interpreter:" << e.what();
        return;
    }

    // Declare sys module variable here to be used by both redirection and path modification
    py::module_ sys;

    // Import C++ bridge module FIRST
    try {
        qDebug() << "[PluginHost] Importing flysight_cpp_bridge from C++...";
        py::module_::import("flysight_cpp_bridge"); // This initializes the module and registers types
        qDebug() << "[PluginHost] flysight_cpp_bridge imported successfully by C++.";
    } catch (const py::error_already_set& e) {
        qCritical().noquote() << "[PluginHost] CRITICAL: Failed to import flysight_cpp_bridge from C++:" << e.what();
        // ... (error handling) ...
        return;
    }

    // Setup Python output redirection
    try {
        qDebug() << "[PluginHost] Attempting to import 'sys' module for redirection.";
        sys = py::module_::import("sys");
        qDebug() << "[PluginHost] 'sys' module imported. Creating redirector.";
        // PythonOutputRedirector should now be a "known registered type" by pybind11
        py::object redirector_instance = py::cast(new PythonOutputRedirector(), py::return_value_policy::take_ownership);
        qDebug() << "[PluginHost] Redirector instance created. Assigning to sys.stdout/stderr.";
        sys.attr("stdout") = redirector_instance;
        sys.attr("stderr") = redirector_instance;
        qInfo() << "[PluginHost] Python stdout/stderr redirected to qDebug().";
    } catch (const py::error_already_set& e) {
        qCritical().noquote() << "[PluginHost] Failed to redirect Python stdout/stderr (after bridge import):" << e.what();
        if (Py_IsInitialized()) PyErr_Print();
    }

    /* ------------------------------------------------------------------ */
    /* 2.  Put <plugins>/ on sys.path & import SDK                        */
    /* ------------------------------------------------------------------ */
    qDebug() << "[PluginHost] Preparing to modify sys.path. Plugin dir:" << pluginDir;
    if (pluginDir.isEmpty()) {
        qWarning() << "[PluginHost] pluginDir is empty. Aborting further plugin loading.";
        return;
    }

    if (!sys) { // Check if sys was successfully imported earlier
        qCritical() << "[PluginHost] 'sys' module was not imported successfully earlier. Cannot modify sys.path.";
        return;
    }

    try {
        std::string pluginDirStd = pluginDir.toStdString();
        qDebug() << "[PluginHost] Adding to sys.path:" << pluginDir;
        sys.attr("path").attr("insert")(0, pluginDirStd.c_str()); // Use the 'sys' object obtained earlier
        qDebug() << "[PluginHost] Successfully added" << pluginDir << "to sys.path.";
    } catch (const py::error_already_set& e) {
        qCritical().noquote() << "[PluginHost] Python error modifying sys.path with '" << pluginDir << "':" << e.what();
        if (Py_IsInitialized()) PyErr_Print();
        return;
    } catch (const std::exception& e) {
        qCritical().noquote() << "[PluginHost] C++ error modifying sys.path with '" << pluginDir << "':" << e.what();
        return;
    }

    py::module sdk;
    try {
        qDebug() << "[PluginHost] Attempting to import flysight_plugin_sdk...";
        sdk = py::module::import("flysight_plugin_sdk");
        qDebug() << "[PluginHost] flysight_plugin_sdk imported successfully.";
    } catch (const py::error_already_set& e) {
        qCritical().noquote() << "[PluginHost] Python error importing flysight_plugin_sdk. Ensure it's in Python path ('" << pluginDir << "') and has no internal import errors (like flysight_cpp_bridge).";
        if (Py_IsInitialized()) PyErr_Print();
        return;
    } catch (const std::exception& e) {
        qCritical().noquote() << "[PluginHost] C++ error importing flysight_plugin_sdk:" << e.what();
        return;
    }

    /* ------------------------------------------------------------------ */
    /* 3.  Import every .py file in the plug-in directory                 */
    /* ------------------------------------------------------------------ */
    QDir dir(pluginDir);
    for (const QFileInfo& fi : dir.entryInfoList({ "*.py" }, QDir::Files)) {
        try {
            py::module::import(fi.baseName().toStdString().c_str());
        } catch (const py::error_already_set& e) {
            qWarning().noquote() << "[PluginHost] Plug-in" << fi.fileName()
            << "failed to import:" << e.what();
        }
    }

    /* ------------------------------------------------------------------ */
    /* 4.  Register calculated attributes                                 */
    /* ------------------------------------------------------------------ */
    // Each plug-in becomes one single-output calculation in the process-wide
    // registry. This runs before the built-ins are registered, so a plug-in
    // that declares a built-in output is tried first.
    CalculationRegistry &registry = CalculationRegistry::instance();

    int attributeIndex = 0;
    for (py::handle h : sdk.attr("_attributes")) {
        py::object plugin = h.cast<py::object>();
        const QString key =
            QString::fromStdString(plugin.attr("name").cast<std::string>());

        CalculationDescriptor d;
        d.id = QStringLiteral("plugin.attr.%1.%2").arg(attributeIndex++).arg(key);
        d.inputs = decodeInputs(plugin);
        d.outputs = { DependencyKey::attribute(key) };
        d.compute = [plugin, key](const EvaluationContext &ctx) -> CalculationResult {
            py::gil_scoped_acquire gil;
            ViewScope scope(&ctx);
            py::object out = plugin.attr("compute")(py::cast(scope.view));
            if (out.is_none()) return CalculationResult::unavailable();

            if (py::isinstance<py::float_>(out) ||
                py::isinstance<py::int_>(out))
                return CalculationResult().setAttribute(
                    key, QVariant::fromValue(out.cast<double>()));

            if (py::isinstance<py::str>(out)) {
                const QString txt =
                    QString::fromStdString(out.cast<std::string>());
                const QDateTime dt =
                    QDateTime::fromString(txt, Qt::ISODateWithMs);
                if (dt.isValid()) {
                    // Store as UTC-seconds double so the value participates
                    // in the interpolation system's canConvert<double>() check.
                    return CalculationResult().setAttribute(
                        key, QVariant::fromValue(dt.toMSecsSinceEpoch() / 1000.0));
                }
                return CalculationResult().setAttribute(key, QVariant::fromValue(txt));
            }
            return CalculationResult::unavailable();
        };

        if (!registry.registerCalculation(d)) {
            qWarning().noquote() << "[PluginHost] Attribute plug-in" << key
                                 << "could not be registered as" << d.id;
        }
    }

    /* ------------------------------------------------------------------ */
    /* 5.  Register calculated measurements                               */
    /* ------------------------------------------------------------------ */
    int measurementIndex = 0;
    for (py::handle h : sdk.attr("_measurements")) {
        py::object plugin = h.cast<py::object>();
        const QString sensor =
            QString::fromStdString(plugin.attr("sensor").cast<std::string>());
        const QString name =
            QString::fromStdString(plugin.attr("name").cast<std::string>());

        CalculationDescriptor d;
        d.id = QStringLiteral("plugin.meas.%1.%2/%3").arg(measurementIndex++).arg(sensor, name);
        d.inputs = decodeInputs(plugin);
        d.outputs = { DependencyKey::measurement(sensor, name) };
        d.compute = [plugin, sensor, name](const EvaluationContext &ctx) -> CalculationResult {
            py::gil_scoped_acquire gil;
            ViewScope scope(&ctx);
            py::object out = plugin.attr("compute")(py::cast(scope.view));
            if (out.is_none()) return CalculationResult::unavailable();

            auto buf = out.cast<
                py::array_t<double,
                            py::array::c_style | py::array::forcecast>>();
            // The copy into the QVector detaches the result from the NumPy buffer.
            return CalculationResult().setMeasurement(
                sensor, name, QVector<double>(buf.data(), buf.data() + buf.shape(0)));
        };

        if (!registry.registerCalculation(d)) {
            qWarning().noquote() << "[PluginHost] Measurement plug-in" << (sensor + "/" + name)
                                 << "could not be registered as" << d.id;
        }
    }

    /* ------------------------------------------------------------------ */
    /* 6.  Register simple plot definitions                               */
    /* ------------------------------------------------------------------ */
    for (py::handle h : sdk.attr("_simple_plots")) {
        py::object plt = h.cast<py::object>();
        PlotRegistry::instance().registerPlot({
            QString::fromStdString(plt.attr("category").cast<std::string>()),
            QString::fromStdString(plt.attr("name").cast<std::string>()),
            plt.attr("units").is_none() ? QString{} :
                QString::fromStdString(plt.attr("units").cast<std::string>()),
            QColor(QString::fromStdString(
                plt.attr("color").cast<std::string>())),
            QString::fromStdString(plt.attr("sensor").cast<std::string>()),
            QString::fromStdString(
                plt.attr("measurement").cast<std::string>()),
            // measurementType: optional, defaults to empty string (no conversion)
            py::hasattr(plt, "measurement_type") && !plt.attr("measurement_type").is_none()
                ? QString::fromStdString(plt.attr("measurement_type").cast<std::string>())
                : QString{}
        });
    }

    /* ------------------------------------------------------------------ */
    /* 6b. Register simple marker definitions                             */
    /* ------------------------------------------------------------------ */
    for (py::handle h : sdk.attr("_markers")) {
        py::object mk = h.cast<py::object>();

        MarkerDefinition def;
        def.category     = QString::fromStdString(mk.attr("category").cast<std::string>());
        def.displayName  = QString::fromStdString(mk.attr("display_name").cast<std::string>());
        def.shortLabel   = QString::fromStdString(mk.attr("short_label").cast<std::string>());
        def.color        = QColor(QString::fromStdString(mk.attr("color").cast<std::string>()));
        def.attributeKey = QString::fromStdString(mk.attr("attribute_key").cast<std::string>());
        def.editable     = mk.attr("editable").cast<bool>();

        // Convert measurements list: each element is a (sensor, timeVector, dataVector) tuple
        for (py::handle mh : mk.attr("measurements")) {
            py::tuple t = mh.cast<py::tuple>();
            def.measurements.append({
                QString::fromStdString(t[0].cast<std::string>()),
                QString::fromStdString(t[1].cast<std::string>()),
                QString::fromStdString(t[2].cast<std::string>())
            });
        }

        MarkerRegistry::instance()->registerMarker(def);
    }

    /* ------------------------------------------------------------------ */
    /* 7.  Summary                                                        */
    /* ------------------------------------------------------------------ */
    qInfo() << "[PluginHost] Registered"
            << py::len(sdk.attr("_attributes"))   << "attributes,"
            << py::len(sdk.attr("_measurements")) << "measurements,"
            << py::len(sdk.attr("_simple_plots")) << "plots and"
            << py::len(sdk.attr("_markers"))      << "markers.";
}
