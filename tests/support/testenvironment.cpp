#include "testenvironment.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTimer>

#include "calculations/attributeregistration.h"
#include "calculations/builtincalculations.h"
#include "idlescheduler.h"
#include "logbookmanager.h"
#include "preferences/enginepreferenceprovider.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessionmodel.h"

using namespace FlySight;

namespace FlySightTest {

namespace {

TestEnvironment *s_instance = nullptr;

// True when path is root itself or lies below it.
bool isUnder(const QString &path, const QString &root)
{
    const QString p = QDir::cleanPath(path);
    const QString r = QDir::cleanPath(root);
    if (r.isEmpty())
        return false;
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    return p.compare(r, cs) == 0 || p.startsWith(r + QLatin1Char('/'), cs);
}

} // namespace

TestEnvironment::TestEnvironment(const QString &testName)
{
    if (s_instance)
        qFatal("TestEnvironment: only one instance may exist per process");
    if (!QCoreApplication::instance())
        qFatal("TestEnvironment: construct the QCoreApplication first");

    // 1. Never the real organization / application names.
    QCoreApplication::setOrganizationName(QStringLiteral("FlySightTests"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("tests.flysight.invalid"));
    QCoreApplication::setApplicationName(testName);

    // 2. Redirect the QStandardPaths locations that support test mode.
    QStandardPaths::setTestModeEnabled(true);

    // 3. One temporary directory holds everything the process writes.
    if (!m_root.isValid())
        qFatal("TestEnvironment: could not create a temporary directory: %s",
               qPrintable(m_root.errorString()));
    if (!QDir().mkpath(settingsPath()))
        qFatal("TestEnvironment: could not create %s", qPrintable(settingsPath()));

    // 4. Default-constructed QSettings (PreferencesManager, LogbookColumnStore,
    //    AltitudeMarkerManager) now resolve to an INI file under the temporary
    //    directory. SystemScope is redirected too so that no machine-wide
    //    settings are read as fallbacks.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsPath());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsPath());

    // 5. Tripwire: refuse to run if the redirect did not take effect.
    {
        QSettings probe;
        if (probe.format() != QSettings::IniFormat || !isUnder(probe.fileName(), rootPath()))
            qFatal("TestEnvironment: QSettings is not isolated (format %d, file '%s', root '%s')",
                   int(probe.format()), qPrintable(probe.fileName()), qPrintable(rootPath()));
    }

    s_instance = this;

    // 6. Preferences first (LogbookManager reads the folder preference), then a
    //    logbook folder inside the temporary directory.
    registerCorePreferences();
    useFreshLogbook();

    // 7. Tripwire: the logbook must live under the temporary directory.
    if (!isUnder(logbookDir(), rootPath()))
        qFatal("TestEnvironment: logbook directory '%s' is outside '%s'",
               qPrintable(logbookDir()), qPrintable(rootPath()));
}

TestEnvironment::~TestEnvironment()
{
    // Flush pending settings writes now. The application singletons outlive
    // this object; a QSettings that still had unsaved changes at process exit
    // would re-create its INI file after the temporary directory (removed
    // when m_root is destroyed, right after this body) is gone and leave it
    // behind. Pending changes are shared per file, so one sync covers them all.
    {
        QSettings settings;
        settings.sync();
    }

    s_instance = nullptr;
}

TestEnvironment &TestEnvironment::instance()
{
    if (!s_instance)
        qFatal("TestEnvironment::instance(): no TestEnvironment exists; use FLYSIGHT_TEST_MAIN");
    return *s_instance;
}

QString TestEnvironment::rootPath() const
{
    return QDir::cleanPath(m_root.path());
}

QString TestEnvironment::settingsPath() const
{
    return rootPath() + QStringLiteral("/settings");
}

QString TestEnvironment::logbookFolder() const
{
    return PreferencesManager::instance()
        .getValue(PreferenceKeys::GeneralLogbookFolder).toString();
}

QString TestEnvironment::logbookDir() const
{
    return logbookFolder() + QStringLiteral("/FlySight Viewer/logbook");
}

QString TestEnvironment::sessionsDir() const
{
    return logbookDir() + QStringLiteral("/sessions");
}

QString TestEnvironment::cacheDir() const
{
    return logbookDir() + QStringLiteral("/cache");
}

QString TestEnvironment::indexPath() const
{
    return logbookDir() + QStringLiteral("/index.json");
}

QString TestEnvironment::newTempDir(const QString &prefix)
{
    const QString stem = prefix.isEmpty() ? QStringLiteral("tmp") : prefix;
    const QString path = rootPath() + QStringLiteral("/%1-%2").arg(stem).arg(++m_tempDirCounter);
    if (!QDir().mkpath(path))
        qFatal("TestEnvironment: could not create %s", qPrintable(path));
    return path;
}

void TestEnvironment::useFreshLogbook()
{
    const QString folder = rootPath() + QStringLiteral("/logbook-%1").arg(++m_logbookCounter);
    if (!QDir().mkpath(folder))
        qFatal("TestEnvironment: could not create %s", qPrintable(folder));
    PreferencesManager::instance().setValue(PreferenceKeys::GeneralLogbookFolder, folder);
    LogbookManager::instance().reset();
}

void TestEnvironment::reopenLogbook()
{
    LogbookManager::instance().reset();
}

void TestEnvironment::registerCorePreferences()
{
    if (!m_corePreferenceKeys.isEmpty())
        return;

    // Literal mirror of the defaults MainWindow registers, restricted to the
    // keys the core library reads. The logbook folder default is a folder
    // under the temporary directory, never the user's Documents folder.
    const QList<QPair<QString, QVariant>> defaults = {
        {PreferenceKeys::GeneralUnits,              QStringLiteral("Metric")},
        {PreferenceKeys::GeneralLogbookFolder,      rootPath() + QStringLiteral("/logbook-0")},
        {PreferenceKeys::ImportGroundReferenceMode, QStringLiteral("Automatic")},
        {PreferenceKeys::ImportFixedElevation,      0.0},
        {PreferenceKeys::ImportDescentPauseSeconds, 30.0},
        {PreferenceKeys::ImportHideOthersOnImport,  false},
        {PreferenceKeys::AeroMass,                  1.0},
        {PreferenceKeys::AeroArea,                  1.0},
    };

    PreferencesManager &prefs = PreferencesManager::instance();
    for (const auto &entry : defaults) {
        prefs.registerPreference(entry.first, entry.second);
        m_corePreferenceKeys.append(entry.first);
    }
}

void TestEnvironment::registerBuiltIns()
{
    // Registrations are global statics and must not be duplicated.
    static bool registered = false;
    if (registered)
        return;
    registered = true;

    FlySight::registerBuiltInAttributes();
    EnginePreferenceProvider::install();
    FlySight::registerBuiltInCalculations();
    FlySight::registerBuiltInCalculationMetadata();
}

void TestEnvironment::resetPreferencesToDefaults()
{
    PreferencesManager &prefs = PreferencesManager::instance();
    for (const QString &key : std::as_const(m_corePreferenceKeys)) {
        if (key == PreferenceKeys::GeneralLogbookFolder)
            continue;   // keeps pointing at the current test logbook
        prefs.setValue(key, prefs.getDefaultValue(key));
    }
}

bool waitForIdle(SessionModel &model, int timeoutMs)
{
    IdleScheduler &scheduler = model.scheduler();

    // A tick that finds work emits progressChanged; the tick that finds none
    // emits schedulerIdle, unless the scheduler was idle already, in which
    // case it emits nothing. IdleScheduler exposes no "is idle" query, so the
    // already-idle case is recognised by a sentinel timer that is queued
    // after the scheduler's own zero-interval timer.
    QSignalSpy idleSpy(&scheduler, &IdleScheduler::schedulerIdle);
    QSignalSpy progressSpy(&scheduler, &IdleScheduler::progressChanged);

    scheduler.wake();

    bool sentinelFired = false;
    QTimer::singleShot(0, &scheduler, [&sentinelFired]() { sentinelFired = true; });

    QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

        if (idleSpy.count() > 0)
            return true;
        if (sentinelFired && progressSpy.count() == 0)
            return true;    // at least one tick ran and found nothing to do
    }
    return false;
}

} // namespace FlySightTest
