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
#include "pluginadapters.h"
#include "plugincodeidentity.h"
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
 *  Per-plugin registration
 *
 *  Decoding, marshalling and the adapters themselves live in
 *  pluginadapters.cpp. Here: one loop used for all three SDK lists, so that a
 *  problem with one plugin rejects that plugin only.
 * ---------------------------------------------------------------------- */
namespace {

struct RegistrationCount {
    int registered = 0;
    int rejected = 0;
};

// `makeAdapter(index, plugin)` builds the descriptor. The index is the position
// in the SDK list, so a rejected plugin still consumes its index and the ids of
// the others do not depend on it. Every registration declares `resultVersion`
// (the plug-in code identity): the adapters build descriptors, the host
// decides their version.
template <typename Fn>
RegistrationCount registerEach(py::handle sdkList, const QString &resultVersion,
                               PluginLoadReport &report, Fn makeAdapter)
{
    RegistrationCount count;
    CalculationRegistry &registry = CalculationRegistry::instance();

    int index = 0;
    for (py::handle h : sdkList) {
        const py::object plugin = py::reinterpret_borrow<py::object>(h);
        const QString label = PluginBridge::pluginLabel(plugin);    // never throws

        QString reason;
        try {
            CalculationDescriptor d = makeAdapter(index, plugin);
            d.resultVersion = resultVersion;
            if (!registry.registerCalculation(d))
                throw PluginBridge::PluginError(
                    "registration refused by the calculation registry (see previous warning)");
            report.registeredIds << d.id;
            ++count.registered;
        } catch (const PluginBridge::PluginError &e) {
            reason = QString::fromUtf8(e.what());
        } catch (const py::error_already_set &e) {     // e.g. inputs() raised
            reason = QString::fromUtf8(e.what());
        } catch (const std::exception &e) {
            reason = QString::fromUtf8(e.what());
        }

        if (!reason.isEmpty()) {
            qWarning().noquote() << "[PluginHost] Plugin" << label << "rejected:" << reason;
            report.rejected << label + QStringLiteral(": ") + reason;
            ++count.rejected;
        }
        ++index;
    }
    return count;
}

// The ingredients of the plug-in code identity (plugincodeidentity.h), read
// before any plugin is imported. `sdk` is the imported SDK module: its
// __file__ is what was actually imported, wherever sys.path found it. Any
// ingredient that cannot be read is left empty / nullopt, which the digest
// encodes distinctly.
PluginCodeIngredients readCodeIngredients(const QString &pluginDir, const py::module_ &sdk)
{
    PluginCodeIngredients ingredients;
    ingredients.files = readPluginCodeFiles(pluginDir);

    try {
        if (py::hasattr(sdk, "__file__")) {
            const py::object file = sdk.attr("__file__");
            if (py::isinstance<py::str>(file))
                ingredients.sdk = readWholeFile(QString::fromStdString(file.cast<std::string>()));
        }
    } catch (const py::error_already_set &) {
    } catch (const std::exception &) {
    }

    // "3.13.3 (tags/v3.13.3:..., ...) [MSC ...]": the part before the first
    // space, which is what platform.python_version() reports.
    const QString version = QString::fromUtf8(Py_GetVersion());
    ingredients.pythonVersion = version.section(QLatin1Char(' '), 0, 0);

    try {
        ingredients.numpyVersion = QString::fromStdString(
            py::module_::import("numpy").attr("__version__").cast<std::string>());
    } catch (const py::error_already_set &) {
    } catch (const std::exception &) {
    }
    return ingredients;
}

} // namespace

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

    // The interpreter, the bridge and the SDK are up: plugin loading runs.
    m_ready = true;

    // The plug-in code identity, from the bytes on disk before any plugin code
    // runs. The digest covers every *.py under the folder (subfolders too);
    // step 3 still imports the top-level files only.
    {
        const PluginCodeIngredients ingredients = readCodeIngredients(pluginDir, sdk);
        m_codeIdentity = pluginCodeIdentity(ingredients);
        qInfo().noquote() << "[PluginHost] Plug-in code identity:" << m_codeIdentity
                          << QStringLiteral("(%1 files)").arg(ingredients.files.size());
    }

    /* ------------------------------------------------------------------ */
    /* 3.  Import every .py file in the plug-in directory (name order)    */
    /* ------------------------------------------------------------------ */
    QDir dir(pluginDir);
    for (const QFileInfo& fi : dir.entryInfoList({ "*.py" }, QDir::Files, QDir::Name)) {
        try {
            py::module::import(fi.baseName().toStdString().c_str());
        } catch (const py::error_already_set& e) {
            qWarning().noquote() << "[PluginHost] Plug-in" << fi.fileName()
            << "failed to import:" << e.what();
            m_report.failedImports << fi.fileName();
        }
    }

    /* ------------------------------------------------------------------ */
    /* 4.  Register plugin calculations                                   */
    /* ------------------------------------------------------------------ */
    // Fixed order: all attributes, then all measurements, then all multi-output
    // calculations; inside a list, registration-call order. This runs before
    // the built-ins are registered, so a plug-in that declares a built-in
    // output is tried first (recorded data still beats both).
    RegistrationCount attributes, measurements, calculations;
    try {
        attributes = registerEach(sdk.attr("_attributes"), m_codeIdentity, m_report,
                                  &PluginBridge::makeAttributeAdapter);
        measurements = registerEach(sdk.attr("_measurements"), m_codeIdentity, m_report,
                                    &PluginBridge::makeMeasurementAdapter);
        calculations = registerEach(sdk.attr("_calculations"), m_codeIdentity, m_report,
                                    &PluginBridge::makeCalculationAdapter);
    } catch (const py::error_already_set& e) {
        qCritical().noquote() << "[PluginHost] The SDK's plugin lists could not be read:" << e.what();
    }

    /* ------------------------------------------------------------------ */
    /* 5.  Register simple plot definitions                               */
    /* ------------------------------------------------------------------ */
    for (py::handle h : sdk.attr("_simple_plots")) try {
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
    } catch (const std::exception& e) {     // a malformed definition skips that plot only
        qWarning().noquote() << "[PluginHost] Plot definition skipped:" << e.what();
    }

    /* ------------------------------------------------------------------ */
    /* 5b. Register simple marker definitions                             */
    /* ------------------------------------------------------------------ */
    for (py::handle h : sdk.attr("_markers")) try {
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
    } catch (const std::exception& e) {     // a malformed definition skips that marker only
        qWarning().noquote() << "[PluginHost] Marker definition skipped:" << e.what();
    }

    /* ------------------------------------------------------------------ */
    /* 6.  Summary                                                        */
    /* ------------------------------------------------------------------ */
    qInfo().noquote() << QStringLiteral(
        "[PluginHost] Registered %1 attributes (%2 rejected), %3 measurements (%4 rejected), "
        "%5 calculations (%6 rejected), %7 plots and %8 markers.")
        .arg(attributes.registered).arg(attributes.rejected)
        .arg(measurements.registered).arg(measurements.rejected)
        .arg(calculations.registered).arg(calculations.rejected)
        .arg(py::len(sdk.attr("_simple_plots")))
        .arg(py::len(sdk.attr("_markers")));
}
