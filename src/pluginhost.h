#ifndef PLUGINHOST_H
#define PLUGINHOST_H

#include <QString>
#include <QStringList>
#include <memory>

namespace pybind11 {
class scoped_interpreter;
}

/// What initialise() did with the plugins it found.
struct PluginLoadReport {
    QStringList registeredIds;     ///< calculation ids, in registration order
    QStringList rejected;          ///< "<label>: <reason>", label = "<module>.<class>"
    QStringList failedImports;     ///< plugin file names whose import raised
};

class PluginHost {
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
    void initialise(const QString& pluginDir);

    // True iff the interpreter booted, the bridge module imported, and the SDK
    // imported, i.e. plugin loading actually ran.
    bool isInitialised() const { return m_ready; }

    const PluginLoadReport& report() const { return m_report; }

private:
    PluginHost() = default;
    std::unique_ptr<pybind11::scoped_interpreter> m_interp;
    bool m_ready = false;
    PluginLoadReport m_report;
};

#endif // PLUGINHOST_H
