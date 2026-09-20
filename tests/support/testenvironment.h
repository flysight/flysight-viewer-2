#ifndef FLYSIGHTTEST_TESTENVIRONMENT_H
#define FLYSIGHTTEST_TESTENVIRONMENT_H

#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace FlySight {
class SessionModel;
}

namespace FlySightTest {

/// Process-wide isolation for a test executable.
///
/// Exactly one instance exists per process. It must be constructed in main(),
/// after the QCoreApplication and before any application singleton is touched
/// (PreferencesManager binds its QSettings backend on first use). Use
/// FLYSIGHT_TEST_MAIN from testmain.h rather than constructing it by hand.
///
/// The constructor redirects QSettings to an INI file inside a temporary
/// directory, points the logbook folder preference into that directory, and
/// aborts the process (qFatal) if either redirection did not take effect. A
/// test therefore can never read or write the user's preferences or logbook.
class TestEnvironment {
public:
    explicit TestEnvironment(const QString &testName);
    ~TestEnvironment();

    TestEnvironment(const TestEnvironment &) = delete;
    TestEnvironment &operator=(const TestEnvironment &) = delete;

    /// The one instance; aborts if none has been constructed.
    static TestEnvironment &instance();

    QString rootPath() const;        ///< the process-wide temporary directory
    QString settingsPath() const;    ///< <root>/settings
    QString logbookFolder() const;   ///< current value of general/logbookFolder
    QString logbookDir() const;      ///< logbookFolder() + "/FlySight Viewer/logbook"
    QString sessionsDir() const;     ///< logbookDir() + "/sessions"
    QString indexPath() const;       ///< logbookDir() + "/index.json"

    /// Creates and returns a fresh subdirectory of rootPath().
    QString newTempDir(const QString &prefix = {});

    /// Points the logbook folder preference at a new, empty folder under
    /// rootPath() and resets LogbookManager. Does not call initialize().
    void useFreshLogbook();

    /// Resets LogbookManager but keeps the folder: simulates an application restart.
    void reopenLogbook();

    /// Registers the preferences the core library reads. Idempotent; called by
    /// the constructor.
    void registerCorePreferences();

    /// Registers built-in attributes and built-in calculations, once per
    /// process (the registries are global and cannot be cleared). Deliberately
    /// does not register the UI-owned built-in plots and markers.
    void registerBuiltIns();

    /// Restores every core preference to its default, except the logbook
    /// folder, which keeps its current test value.
    void resetPreferencesToDefaults();

private:
    QTemporaryDir m_root;
    int m_logbookCounter = 0;
    int m_tempDirCounter = 0;
    QStringList m_corePreferenceKeys;
};

/// Spins the event loop until the model's idle scheduler has no work left.
/// Returns false if that does not happen within timeoutMs.
bool waitForIdle(FlySight::SessionModel &model, int timeoutMs = 5000);

} // namespace FlySightTest

#endif // FLYSIGHTTEST_TESTENVIRONMENT_H
