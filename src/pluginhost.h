#ifndef PLUGINHOST_H
#define PLUGINHOST_H

#include <QString>
#include <QStringList>
#include <memory>

namespace pybind11 {
class scoped_interpreter;
}

// pybind11 declares its types with hidden symbol visibility. A type that holds
// one as a member must be hidden too, or GCC warns that it is "declared with
// greater visibility than the type of its field". These types are only used
// inside the binary that defines them.
#ifndef FLYSIGHT_PYBIND_HIDDEN
#  if defined(__GNUC__)
#    define FLYSIGHT_PYBIND_HIDDEN __attribute__((visibility("hidden")))
#  else
#    define FLYSIGHT_PYBIND_HIDDEN
#  endif
#endif

/// What initialise() did with the plugins it found.
struct PluginLoadReport {
    QStringList registeredIds;     ///< calculation ids, in registration order
    QStringList rejected;          ///< "<label>: <reason>", label = "<module>.<class>"
    QStringList failedImports;     ///< plugin file names whose import raised
};

class FLYSIGHT_PYBIND_HIDDEN PluginHost {
public:
    // Singleton accessor
    static PluginHost& instance();

    // Call once, after the QCoreApplication exists and BEFORE
    // registerBuiltInCalculations(): candidates are tried in registration
    // order, so registering plugins first is what lets a plugin that declares
    // a built-in output take precedence over the built-in. Later calls are
    // no-ops (there is one interpreter per process and no teardown).
    //
    // `pluginDir` is where the SDK and the plugin .py files live (e.g.
    // "<exe>/python_plugins"). Every top-level *.py in it is imported, in name
    // order; then the SDK's lists are registered in a fixed order: attributes,
    // measurements, calculations, plots, markers. A plugin whose declaration is
    // invalid is rejected alone (see report()); the others still load.
    //
    // Before any plugin is imported, the plug-in code identity
    // (plugincodeidentity.h) is computed over every *.py file under `pluginDir`
    // (subfolders included, although only the top-level files are imported),
    // the SDK file actually imported, and the Python and numpy versions; it is
    // the result version of every attribute, measurement and calculation it
    // registers. Empty when plugin loading did not run.
    void initialise(const QString& pluginDir);

    // True iff the interpreter booted, the bridge module imported, and the SDK
    // imported, i.e. plugin loading actually ran.
    bool isInitialised() const { return m_ready; }

    const PluginLoadReport& report() const { return m_report; }

    // The plug-in code identity (see initialise()); empty until computed.
    const QString& codeIdentity() const { return m_codeIdentity; }

private:
    PluginHost() = default;
    std::unique_ptr<pybind11::scoped_interpreter> m_interp;
    bool m_ready = false;
    PluginLoadReport m_report;
    QString m_codeIdentity;
};

#endif // PLUGINHOST_H
