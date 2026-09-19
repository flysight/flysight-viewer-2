// SessionModel on the calculation engine: one registration serving several
// sessions, registry and preference broadcasts surfacing as dependencyChanged,
// merges notifying dependents, and listeners surviving row moves.

#include <memory>

#include <QSettings>
#include <QSignalSpy>
#include <QtTest>

#include "altitudemarkerfeature.h"
#include "builtinfixture.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "logbookcolumn.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

constexpr double T0 = DescentFixture::T0;

const QString kAltitudeKey = QStringLiteral("_ALTITUDE_1000_M");
const QString kAltitudeId = QStringLiteral("builtin.altitude._ALTITUDE_1000_M");

// Writes the altitude list the way the preferences page does: the QSettings
// array first, then a bump of the version preference, which is what makes an
// existing AltitudeMarkerManager refresh.
void writeAltitudes(const QList<int> &altitudes)
{
    {
        QSettings settings;
        settings.beginWriteArray(QStringLiteral("altitudeMarkers"), altitudes.size());
        for (int i = 0; i < altitudes.size(); ++i) {
            settings.setArrayIndex(i);
            settings.setValue(QStringLiteral("value"), altitudes.at(i));
        }
        settings.endArray();
    }

    PreferencesManager &prefs = PreferencesManager::instance();
    if (prefs.hasPreference(PreferenceKeys::AltitudeMarkersVersion)) {
        const int version = prefs.getValue(PreferenceKeys::AltitudeMarkersVersion).toInt();
        prefs.setValue(PreferenceKeys::AltitudeMarkersVersion, version + 1);
    }
}

// (sessionId, attribute key) pairs seen by a dependencyChanged spy
bool spyHasAttribute(const QSignalSpy &spy, const QString &sessionId, const QString &key)
{
    for (const QList<QVariant> &args : spy) {
        const DependencyKey name = args.at(1).value<DependencyKey>();
        if (args.at(0).toString() == sessionId && name == DependencyKey::attribute(key))
            return true;
    }
    return false;
}

bool spyHasMeasurement(const QSignalSpy &spy, const QString &sessionId,
                       const QString &sensor, const QString &measurement)
{
    for (const QList<QVariant> &args : spy) {
        const DependencyKey name = args.at(1).value<DependencyKey>();
        if (args.at(0).toString() == sessionId && name == DependencyKey::measurement(sensor, measurement))
            return true;
    }
    return false;
}

// dataChanged emissions that publish an invalidation of `row`. They carry the
// edit and check-state roles; the column worker's refresh is display-only.
int publicationCount(const QSignalSpy &dataSpy, int row)
{
    int count = 0;
    for (const QList<QVariant> &args : dataSpy) {
        const QModelIndex topLeft = args.at(0).toModelIndex();
        const QList<int> roles = args.at(2).value<QList<int>>();
        if (topLeft.row() == row && roles.contains(Qt::EditRole))
            ++count;
    }
    return count;
}

int spyCountFor(const QSignalSpy &spy, const QString &sessionId, const QString &key)
{
    int count = 0;
    for (const QList<QVariant> &args : spy) {
        const DependencyKey name = args.at(1).value<DependencyKey>();
        if (args.at(0).toString() == sessionId && name == DependencyKey::attribute(key))
            ++count;
    }
    return count;
}

} // namespace

Q_DECLARE_METATYPE(FlySight::DependencyKey)

class SessionModelEngineTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void twoSessionsOneRegistration();
    void unregisterInvalidatesEverySession();
    void reRegisterRestores();
    void preferenceBroadcastReachesModel();
    void snapshotPreferencesEmitNothing();
    void registryChangesAreCoalesced();
    void evictedSessionIsIgnored();
    void mergeEmitsDependencyChanged();
    void rowsSurviveSort();

private:
    SessionData &session(const QString &id) { return m_model->sessionRef(m_model->getSessionRow(id)); }

    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<AltitudeMarkerManager> m_altitudes;
    QStringList m_registryBefore;
};

void SessionModelEngineTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();

    // One logbook column that reads stored data only, so that the model has
    // valid indexes to report without the merge warming any calculation.
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumn description;
    description.type = ColumnType::SessionAttribute;
    description.attributeKey = QString::fromLatin1(SessionKeys::Description);
    LogbookColumnStore::instance().setColumns({description});
}

// Two loaded sessions "s1" and "s2" from the descent fixture; s2 has a stored
// ground elevation of 0 m (s1's is calculated: 100 m). One altitude marker at
// 1000 m.
void SessionModelEngineTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    m_registryBefore = CalculationRegistry::instance().registeredIds();

    m_model = std::make_unique<SessionModel>();
    m_model->mergeSessions({DescentFixture::load("s1"), DescentFixture::load("s2")});
    QCOMPARE(m_model->rowCount(), 2);
    QVERIFY(m_model->updateAttribute("s2", "_GROUND_ELEV", 0.0));

    m_altitudes = std::make_unique<AltitudeMarkerManager>(m_model.get());
    writeAltitudes({1000});
    PreferencesManager::instance().setValue(PreferenceKeys::AltitudeMarkersUnits, QStringLiteral("Metric"));
    m_altitudes->registerAll();
    m_model->flushPendingInvalidations();
}

void SessionModelEngineTest::cleanup()
{
    // Destroying the manager removes its registrations; the global registry is
    // left as it was found.
    m_altitudes.reset();
    writeAltitudes({});
    m_model.reset();
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

// Acceptance 13: two sessions using one registration have independent results.
void SessionModelEngineTest::twoSessionsOneRegistration()
{
    CalculationRegistry &registry = CalculationRegistry::instance();
    QVERIFY(registry.contains(kAltitudeId));
    QCOMPARE(registry.candidatesFor(DependencyKey::attribute(kAltitudeKey)).size(), 1);

    QCOMPARE(session("s1").getAttribute(kAltitudeKey).toDouble(), T0 + 71.5);
    QCOMPARE(session("s2").getAttribute(kAltitudeKey).toDouble(), T0 + 78.0);
    QCOMPARE(session("s1").getAttribute(kAltitudeKey).toDouble(), T0 + 71.5);

    QCOMPARE(session("s1").calculationEngine().runCount(kAltitudeId), 1);
    QCOMPARE(session("s2").calculationEngine().runCount(kAltitudeId), 1);
}

// Acceptance 13: unregistering a calculation invalidates its results in every
// session; a session's cache does not outlive a removed registration.
void SessionModelEngineTest::unregisterInvalidatesEverySession()
{
    QCOMPARE(session("s1").getAttribute(kAltitudeKey).toDouble(), T0 + 71.5);
    QCOMPARE(session("s2").getAttribute(kAltitudeKey).toDouble(), T0 + 78.0);
    const int runs1 = session("s1").calculationEngine().totalRunCount();
    const int runs2 = session("s2").calculationEngine().totalRunCount();

    QSignalSpy spy(m_model.get(), &SessionModel::dependencyChanged);
    writeAltitudes({});     // the manager refreshes and unregisters
    m_model->flushPendingInvalidations();

    QVERIFY(spyHasAttribute(spy, "s1", kAltitudeKey));
    QVERIFY(spyHasAttribute(spy, "s2", kAltitudeKey));

    // Invalidation computed nothing.
    QCOMPARE(session("s1").calculationEngine().totalRunCount(), runs1);
    QCOMPARE(session("s2").calculationEngine().totalRunCount(), runs2);

    QVERIFY(!CalculationRegistry::instance().hasCandidateFor(DependencyKey::attribute(kAltitudeKey)));
    QVERIFY(!session("s1").calculationEngine().resultStatus(kAltitudeId).has_value());
    QVERIFY(!session("s2").calculationEngine().resultStatus(kAltitudeId).has_value());
    QVERIFY(!session("s1").getAttribute(kAltitudeKey).isValid());
    QVERIFY(!session("s2").getAttribute(kAltitudeKey).isValid());
}

void SessionModelEngineTest::reRegisterRestores()
{
    QCOMPARE(session("s1").getAttribute(kAltitudeKey).toDouble(), T0 + 71.5);
    writeAltitudes({});
    QVERIFY(!session("s1").getAttribute(kAltitudeKey).isValid());
    QVERIFY(!session("s2").getAttribute(kAltitudeKey).isValid());
    m_model->flushPendingInvalidations();

    QSignalSpy spy(m_model.get(), &SessionModel::dependencyChanged);
    writeAltitudes({1000});
    m_model->flushPendingInvalidations();

    // The cached "unavailable" answers were dropped in both sessions.
    QVERIFY(spyHasAttribute(spy, "s1", kAltitudeKey));
    QVERIFY(spyHasAttribute(spy, "s2", kAltitudeKey));
    QCOMPARE(session("s1").getAttribute(kAltitudeKey).toDouble(), T0 + 71.5);
    QCOMPARE(session("s2").getAttribute(kAltitudeKey).toDouble(), T0 + 78.0);
}

// Acceptance 15: a declared preference change reaches the model's subscribers
// and refreshes cached columns, without marking anything dirty.
void SessionModelEngineTest::preferenceBroadcastReachesModel()
{
    QVERIFY(waitForIdle(*m_model));     // let the merge's saves finish
    QVERIFY(!m_model->rowAt(0).dirty);
    QVERIFY(!m_model->rowAt(1).dirty);

    // Only s1 has read its analysis range.
    QCOMPARE(session("s1").getAttribute("_ANALYSIS_START_TIME").toDouble(), T0);
    QCOMPARE(session("s1").getAttribute("_EXIT_TIME").toDouble(), T0 + 9.0);

    const int row1 = m_model->getSessionRow("s1");
    m_model->rowAt(row1).cachedValues.insert(0, QStringLiteral("stale"));

    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QSignalSpy dataSpy(m_model.get(), &SessionModel::dataChanged);

    PreferencesManager::instance().setValue(PreferenceKeys::ImportDescentPauseSeconds, 5.0);

    // Invalidation is synchronous; publication waits for the flush.
    QCOMPARE(session("s1").calculationEngine().cachedState(DependencyKey::attribute("_ANALYSIS_START_TIME")),
             CalculationEngine::CachedState::NotCached);
    QCOMPARE(dependencySpy.count(), 0);

    m_model->flushPendingInvalidations();

    QVERIFY(spyHasAttribute(dependencySpy, "s1", "_ANALYSIS_START_TIME"));
    QVERIFY(spyHasAttribute(dependencySpy, "s1", "_EXIT_TIME"));
    QVERIFY(!spyHasAttribute(dependencySpy, "s2", "_ANALYSIS_START_TIME"));

    // s2 never read its analysis start. (Its analysis end was read once, when
    // init() looked up the ground elevation it was about to override.)
    QVERIFY(spyHasAttribute(dependencySpy, "s2", "_ANALYSIS_END_TIME"));
    QVERIFY(!spyHasAttribute(dependencySpy, "s2", "_EXIT_TIME"));

    QCOMPARE(publicationCount(dataSpy, row1), 1);
    QCOMPARE(publicationCount(dataSpy, m_model->getSessionRow("s2")), 1);

    QVERIFY(m_model->rowAt(row1).cachedValues.isEmpty());
    QVERIFY(!m_model->rowAt(row1).dirty);
    QVERIFY(!m_model->rowAt(m_model->getSessionRow("s2")).dirty);

    // A second flush has nothing to say.
    dependencySpy.clear();
    m_model->flushPendingInvalidations();
    QCOMPARE(dependencySpy.count(), 0);
}

// Acceptance 15: snapshotted preferences are not inputs of anything.
void SessionModelEngineTest::snapshotPreferencesEmitNothing()
{
    for (const GoldenValue &golden : goldenValues()) {
        if (golden.name.type == DependencyKey::Type::Attribute)
            session("s1").getAttribute(golden.name.attributeKey);
        else
            session("s1").getMeasurement(golden.name.measurementKey.first,
                                         golden.name.measurementKey.second);
    }

    QSignalSpy spy(m_model.get(), &SessionModel::dependencyChanged);
    PreferencesManager &prefs = PreferencesManager::instance();
    prefs.setValue(PreferenceKeys::AeroMass, 90.0);
    prefs.setValue(PreferenceKeys::ImportFixedElevation, 10.0);
    m_model->flushPendingInvalidations();
    QCoreApplication::processEvents();
    QCOMPARE(spy.count(), 0);
}

// N registry changes inside one event-loop turn are published once per
// session, as the union of the invalidated names.
void SessionModelEngineTest::registryChangesAreCoalesced()
{
    const QStringList keys = {"_ALTITUDE_500_M", "_ALTITUDE_1000_M", "_ALTITUDE_1500_M"};
    writeAltitudes({500, 1000, 1500});
    m_model->flushPendingInvalidations();
    for (const QString &key : keys) {
        QVERIFY2(session("s1").getAttribute(key).isValid(), qPrintable(key));
        QVERIFY2(session("s2").getAttribute(key).isValid(), qPrintable(key));
    }

    QVERIFY(waitForIdle(*m_model));     // no save or column work left to emit dataChanged

    QSignalSpy spy(m_model.get(), &SessionModel::dependencyChanged);
    QSignalSpy dataSpy(m_model.get(), &SessionModel::dataChanged);

    // Three unregistrations, each broadcast separately to both engines.
    writeAltitudes({});
    QCOMPARE(spy.count(), 0);

    QCoreApplication::processEvents();  // the queued flush, not a direct call

    for (const QString &key : keys) {
        QCOMPARE(spyCountFor(spy, "s1", key), 1);
        QCOMPARE(spyCountFor(spy, "s2", key), 1);
    }
    // One publication per session, however many registry changes there were.
    QCOMPARE(publicationCount(dataSpy, m_model->getSessionRow("s1")), 1);
    QCOMPARE(publicationCount(dataSpy, m_model->getSessionRow("s2")), 1);
}

void SessionModelEngineTest::evictedSessionIsIgnored()
{
    QCOMPARE(session("s1").getAttribute(kAltitudeKey).toDouble(), T0 + 71.5);
    QCOMPARE(session("s2").getAttribute(kAltitudeKey).toDouble(), T0 + 78.0);

    QSignalSpy spy(m_model.get(), &SessionModel::dependencyChanged);
    writeAltitudes({});                 // queued for both sessions

    // s1 goes away before the flush.
    QVERIFY(m_model->removeSessions({QStringLiteral("s1")}));
    m_model->flushPendingInvalidations();

    QVERIFY(!spyHasAttribute(spy, "s1", kAltitudeKey));
    QVERIFY(spyHasAttribute(spy, "s2", kAltitudeKey));
}

// A merge into a loaded session tells subscribers what it invalidated.
void SessionModelEngineTest::mergeEmitsDependencyChanged()
{
    // Make the fixture's IMU data differ, and have a dependent cached.
    SessionData sensorOnly = DescentFixture::loadSensorOnly("s1");
    sensorOnly.setMeasurement("IMU", "wx", {30.0, 60.0, 90.0});
    // The fixture declares no SCHEMA_VER: derived from the legacy-corrected gyro, 5 x 1.14688.
    QVERIFY(qAbs(session("s1").getMeasurement("IMU", "wTotal").value(0) - 5.7344) <= 1e-9);

    QSignalSpy spy(m_model.get(), &SessionModel::dependencyChanged);
    m_model->mergeSessions({sensorOnly});

    QCOMPARE(m_model->rowCount(), 2);
    QVERIFY(spyHasMeasurement(spy, "s1", "IMU", "wx"));
    QVERIFY(spyHasMeasurement(spy, "s1", "IMU", "wTotal"));
    QVERIFY(!spyHasMeasurement(spy, "s2", "IMU", "wx"));

    // sqrt(30^2 + 4^2) x 1.14688 = 30.265491900843113 x 1.14688
    QVERIFY(qAbs(session("s1").getMeasurement("IMU", "wTotal").value(0) - 34.710887351239) <= 1e-9);
}

// Rows move when the model sorts; the listener travels with the session's
// engine and identifies the session by id, not by address.
void SessionModelEngineTest::rowsSurviveSort()
{
    QCOMPARE(session("s1").getAttribute("_EXIT_TIME").toDouble(), T0 + 9.0);
    QCOMPARE(session("s2").getAttribute("_EXIT_TIME").toDouble(), T0 + 9.0);
    const int runs1 = session("s1").calculationEngine().totalRunCount();

    m_model->sort(0, Qt::DescendingOrder);
    m_model->sort(0, Qt::AscendingOrder);

    // The caches moved with the rows.
    QCOMPARE(session("s1").calculationEngine().cachedState(DependencyKey::attribute("_EXIT_TIME")),
             CalculationEngine::CachedState::Available);
    QCOMPARE(session("s1").getAttribute("_EXIT_TIME").toDouble(), T0 + 9.0);
    QCOMPARE(session("s1").calculationEngine().totalRunCount(), runs1);

    QSignalSpy spy(m_model.get(), &SessionModel::dependencyChanged);
    PreferencesManager::instance().setValue(PreferenceKeys::ImportDescentPauseSeconds, 60.0);
    m_model->flushPendingInvalidations();

    QVERIFY(spyHasAttribute(spy, "s1", "_EXIT_TIME"));
    QVERIFY(spyHasAttribute(spy, "s2", "_EXIT_TIME"));

    // The descent has no pause longer than 30 s, so a longer timeout changes no
    // golden value.
    QCOMPARE(session("s1").getAttribute("_EXIT_TIME").toDouble(), T0 + 9.0);
    QCOMPARE(session("s2").getAttribute("_EXIT_TIME").toDouble(), T0 + 9.0);
    QCOMPARE(session("s1").getAttribute("_GROUND_ELEV").toDouble(), 100.0);
    QCOMPARE(session("s2").getAttribute("_GROUND_ELEV").toDouble(), 0.0);
}

FLYSIGHT_TEST_MAIN(SessionModelEngineTest)
#include "tst_session_model_engine.moc"
