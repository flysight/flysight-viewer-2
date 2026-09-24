// SessionModel on the calculation engine: one registration serving several
// sessions, registry and preference broadcasts surfacing as dependencyChanged,
// merges notifying dependents, and listeners surviving row moves.

#include <memory>

#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QtTest>

#include "altitudemarkerfeature.h"
#include "builtinfixture.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "logbookcolumn.h"
#include "logbookprobe.h"
#include "markerregistry.h"
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
    void deviceIdIsReadOnly();
    void altitudeMarkerOnlyForRegisteredCalculation();
    void rowStabilityGuardNests();
    void forEachLoadedSessionVisitsInIdOrder();
    void forEachLoadedSessionIsAPlainRead();
    void loadedSessionIsAGuardedPlainLookup();
    void pinnedSessionIsNotEvicted();
    void calculationInvalidationIsPublished();

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

    m_altitudes = std::make_unique<AltitudeMarkerManager>();
    writeAltitudes({1000});
    PreferencesManager::instance().setValue(PreferenceKeys::AltitudeMarkersUnits, QStringLiteral("Metric"));
    m_altitudes->refresh();
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
// without marking anything dirty. (Cached columns are cleared by the model's
// calculation-environment handler, which the same flush runs, for the columns
// whose closure reads the preference - loaded and unloaded rows alike, see
// tst_column_cache. The one column here, the description, does not.)
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

    // Not cleared: SessionModel::checkCalculationEnvironment clears only the
    // columns whose environment the preference is part of.
    QCOMPARE(m_model->rowAt(row1).cachedValues.value(0), QVariant(QStringLiteral("stale")));
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
// DEVICE_ID is a header attribute recorded by the device. Rewriting it would
// make the session's other file fail to merge (attribute conflict), so no
// model path may edit it: not flags(), not setData(), not the bulk edit.
void SessionModelEngineTest::deviceIdIsReadOnly()
{
    LogbookColumn description;
    description.type = ColumnType::SessionAttribute;
    description.attributeKey = QString::fromLatin1(SessionKeys::Description);
    LogbookColumn device;
    device.type = ColumnType::SessionAttribute;
    device.attributeKey = QString::fromLatin1(SessionKeys::DeviceId);
    const auto restoreColumns = qScopeGuard([description] {
        LogbookColumnStore::instance().setColumns({description});
    });
    LogbookColumnStore::instance().setColumns({description, device});
    QCOMPARE(m_model->columnCount(), 2);

    const int row = m_model->getSessionRow("s1");
    const QModelIndex deviceIndex = m_model->index(row, 1);
    const QModelIndex descriptionIndex = m_model->index(row, 0);
    QCOMPARE(session("s1").storedAttribute("DEVICE_ID").toString(), QStringLiteral("test-device"));
    QCOMPARE(m_model->data(deviceIndex, Qt::DisplayRole).toString(), QStringLiteral("test-device"));

    QVERIFY(!(m_model->flags(deviceIndex) & Qt::ItemIsEditable));
    QVERIFY(m_model->flags(descriptionIndex) & Qt::ItemIsEditable);      // control: an editable column

    QSignalSpy spy(m_model.get(), &SessionModel::dependencyChanged);
    QVERIFY(!m_model->setData(deviceIndex, QStringLiteral("renamed"), Qt::EditRole));
    m_model->startBulkEdit({row}, 1, QStringLiteral("renamed"));
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(session("s1").storedAttribute("DEVICE_ID").toString(), QStringLiteral("test-device"));
    QCOMPARE(m_model->data(deviceIndex, Qt::DisplayRole).toString(), QStringLiteral("test-device"));
    QVERIFY(!spyHasAttribute(spy, "s1", "DEVICE_ID"));

    // Control: the same call on the description column is accepted.
    QVERIFY(m_model->setData(descriptionIndex, QStringLiteral("renamed"), Qt::EditRole));
    QCOMPARE(session("s1").storedAttribute("_DESCRIPTION").toString(), QStringLiteral("renamed"));
    QVERIFY(waitForIdle(*m_model));
}

// A marker exists only for an altitude whose calculation was registered. The
// id of the 2000 m calculation is taken by another registration here, so the
// manager's registration of it is refused: no marker and no colour preference
// for it, while the 1000 m marker is untouched.
void SessionModelEngineTest::altitudeMarkerOnlyForRegisteredCalculation()
{
    const auto altitudeMarkerKeys = [] {
        QStringList keys;
        for (const MarkerDefinition &def : MarkerRegistry::instance()->allMarkers()) {
            if (def.groupId == QLatin1String("altitude"))
                keys.append(def.attributeKey);
        }
        return keys;
    };
    QCOMPARE(altitudeMarkerKeys(), QStringList({"_ALTITUDE_1000_M"}));

    CalculationDescriptor squatter;
    squatter.id = QStringLiteral("builtin.altitude._ALTITUDE_2000_M");
    squatter.outputs = {DependencyKey::attribute("_SQUATTER")};
    squatter.compute = [](const EvaluationContext &) { return CalculationResult::unavailable(); };
    CalculationRegistry &registry = CalculationRegistry::instance();
    QVERIFY(registry.registerCalculation(squatter));
    bool squatting = true;      // so that a failing assertion below still frees the id
    const auto removeSquatter = qScopeGuard([&registry, &squatting] {
        if (squatting)
            registry.unregister(QStringLiteral("builtin.altitude._ALTITUDE_2000_M"),
                                CalculationRegistry::Removal::Change);
    });

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("id already registered")));
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("could not register the calculation for")));
    writeAltitudes({1000, 2000});       // the manager refreshes

    QCOMPARE(altitudeMarkerKeys(), QStringList({"_ALTITUDE_1000_M"}));
    QVERIFY(!registry.hasCandidateFor(DependencyKey::attribute("_ALTITUDE_2000_M")));
    QSettings settings;
    QVERIFY(settings.contains(QStringLiteral("markers/_ALTITUDE_1000_M/color")));
    QVERIFY(!settings.contains(QStringLiteral("markers/_ALTITUDE_2000_M/color")));

    // Control: with the id free again, the next refresh adds the marker.
    QVERIFY(registry.unregister(QStringLiteral("builtin.altitude._ALTITUDE_2000_M"),
                                CalculationRegistry::Removal::Change));
    squatting = false;
    writeAltitudes({1000, 2000});
    QCOMPARE(altitudeMarkerKeys(), QStringList({"_ALTITUDE_1000_M", "_ALTITUDE_2000_M"}));
    QVERIFY(QSettings().contains(QStringLiteral("markers/_ALTITUDE_2000_M/color")));
    m_model->flushPendingInvalidations();
}

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

// The guard is a counter: it nests, and it is released on every way out of a
// scope. (What it guards - the debug-build assertion in every operation that
// moves rows or loads, replaces or evicts a session - aborts the process, so it
// is not exercised here.)
void SessionModelEngineTest::rowStabilityGuardNests()
{
    QCOMPARE(m_model->rowStabilityDepth(), 0);
    {
        const auto outer = m_model->stableRows();
        QCOMPARE(m_model->rowStabilityDepth(), 1);
        {
            const SessionModel::RowStabilityGuard inner(*m_model);
            QCOMPARE(m_model->rowStabilityDepth(), 2);
        }
        QCOMPARE(m_model->rowStabilityDepth(), 1);
    }
    QCOMPARE(m_model->rowStabilityDepth(), 0);

    // Early return from a guarded scope
    const auto guardedRead = [this](bool leaveEarly) {
        const auto guard = m_model->stableRows();
        if (leaveEarly)
            return m_model->rowStabilityDepth();
        return -1;
    };
    QCOMPARE(guardedRead(true), 1);
    QCOMPARE(m_model->rowStabilityDepth(), 0);

    // forEachLoadedSession holds a guard while it reads, nests inside a
    // caller's guard, and releases it when the callback throws.
    {
        const auto outer = m_model->stableRows();
        int depthInside = -1;
        m_model->forEachLoadedSession({QStringLiteral("s1")}, [&](const SessionData &) {
            depthInside = m_model->rowStabilityDepth();
        });
        QCOMPARE(depthInside, 2);
        QCOMPARE(m_model->rowStabilityDepth(), 1);
    }
    bool thrown = false;
    try {
        m_model->forEachLoadedSession({QStringLiteral("s1"), QStringLiteral("s2")},
                                      [](const SessionData &) { throw 1; });
    } catch (int) {
        thrown = true;
    }
    QVERIFY(thrown);
    QCOMPARE(m_model->rowStabilityDepth(), 0);

    // With no guard alive the model mutates as usual.
    m_model->sort(0, Qt::DescendingOrder);
    QVERIFY(m_model->removeSessions({QStringLiteral("s2")}));
    QCOMPARE(m_model->rowCount(), 1);
}

// Loaded rows only, in the order of the ids, each the model's own session.
void SessionModelEngineTest::forEachLoadedSessionVisitsInIdOrder()
{
    QStringList visited;
    QList<const SessionData *> addresses;
    const auto record = [&](const SessionData &s) {
        visited.append(s.storedAttribute(SessionKeys::SessionId).toString());
        addresses.append(&s);       // compared below, never dereferenced
    };

    m_model->forEachLoadedSession({"s2", "no-such-session", "s1", QString()}, record);
    QCOMPARE(visited, QStringList({"s2", "s1"}));
    QCOMPARE(addresses.at(0), &m_model->rowAt(m_model->getSessionRow("s2")).session.value());
    QCOMPARE(addresses.at(1), &m_model->rowAt(m_model->getSessionRow("s1")).session.value());

    // The live session: a value computed through the callback is cached in the
    // model's engine, not in a copy.
    const int runsBefore = session("s1").calculationEngine().totalRunCount();
    m_model->forEachLoadedSession({"s1"}, [](const SessionData &s) { s.getAttribute("_EXIT_TIME"); });
    const int runsAfter = session("s1").calculationEngine().totalRunCount();
    QVERIFY(runsAfter > runsBefore);
    QCOMPARE(session("s1").getAttribute("_EXIT_TIME").toDouble(), T0 + 9.0);
    QCOMPARE(session("s1").calculationEngine().totalRunCount(), runsAfter);

    visited.clear();
    m_model->forEachLoadedSession({}, record);
    m_model->forEachLoadedSession({"no-such-session"}, record);
    QVERIFY(visited.isEmpty());
}

// It does not count as a use for the LRU, and it never loads a stub.
void SessionModelEngineTest::forEachLoadedSessionIsAPlainRead()
{
    QVERIFY(waitForIdle(*m_model));
    session("s1");
    session("s2");                  // s1 is now the least recently used

    int visits = 0;
    m_model->forEachLoadedSession({"s1"}, [&visits](const SessionData &) { ++visits; });
    QCOMPARE(visits, 1);

    // Room for one session: had the read touched the LRU, s2 would go.
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 1);
    const int row1 = m_model->getSessionRow("s1");
    const int row2 = m_model->getSessionRow("s2");
    QVERIFY(!m_model->rowAt(row1).isLoaded());
    QVERIFY(m_model->rowAt(row2).isLoaded());
    QVERIFY(waitForIdle(*m_model));

    // s1 is a stub now: skipped, and still a stub afterwards.
    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    m_model->resetColumnWorkStats();
    QStringList visited;
    m_model->forEachLoadedSession({"s1", "s2"}, [&visited](const SessionData &s) {
        visited.append(s.storedAttribute(SessionKeys::SessionId).toString());
    });
    QCOMPARE(visited, QStringList({"s2"}));
    QVERIFY(!m_model->rowAt(row1).isLoaded());
    QVERIFY(m_model->rowAt(row2).isLoaded());
    QCOMPARE(loadedSpy.count(), 0);
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 0);
    QCOMPARE(m_model->columnWorkStats().calculationRuns, 0);
}

// The model's own session for a loaded row, nullptr otherwise; no load, no LRU use.
void SessionModelEngineTest::loadedSessionIsAGuardedPlainLookup()
{
    QVERIFY(waitForIdle(*m_model));
    session("s1");
    session("s2");                  // s1 is now the least recently used

    {
        const auto guard = m_model->stableRows();
        QCOMPARE(m_model->loadedSession("s1"),
                 &m_model->rowAt(m_model->getSessionRow("s1")).session.value());
        QCOMPARE(m_model->loadedSession("s2"),
                 &m_model->rowAt(m_model->getSessionRow("s2")).session.value());
        QVERIFY(m_model->loadedSession("no-such-session") == nullptr);
        QVERIFY(m_model->loadedSession(QString()) == nullptr);

        // Reads through the pointer, calculated values included, are allowed
        // under the guard.
        QCOMPARE(m_model->loadedSession("s1")->getAttribute("_EXIT_TIME").toDouble(), T0 + 9.0);
    }
    QCOMPARE(m_model->rowStabilityDepth(), 0);

    // Room for one session: had the lookups of s1 touched the LRU, s2 would go.
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 1);
    const int row1 = m_model->getSessionRow("s1");
    const int row2 = m_model->getSessionRow("s2");
    QVERIFY(!m_model->rowAt(row1).isLoaded());
    QVERIFY(m_model->rowAt(row2).isLoaded());
    QVERIFY(waitForIdle(*m_model));

    // s1 is a stub now: nullptr, and still a stub afterwards.
    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    m_model->resetColumnWorkStats();
    {
        const auto guard = m_model->stableRows();
        QVERIFY(m_model->loadedSession("s1") == nullptr);
        QCOMPARE(m_model->loadedSession("s2"), &m_model->rowAt(row2).session.value());
    }
    QVERIFY(!m_model->rowAt(row1).isLoaded());
    QVERIFY(m_model->rowAt(row2).isLoaded());
    QCOMPARE(loadedSpy.count(), 0);
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);
}

// A pinned row is passed over by eviction (the job queue pins the session of
// every active job); pins are counted, and they prevent nothing but eviction.
void SessionModelEngineTest::pinnedSessionIsNotEvicted()
{
    QVERIFY(waitForIdle(*m_model));     // both rows clean
    session("s1");
    session("s2");                      // s1 is now the least recently used

    QVERIFY(!m_model->isSessionPinned("s1"));
    m_model->pinSession("s1");
    m_model->pinSession("s1");
    m_model->pinSession(QString());     // ignored
    QVERIFY(m_model->isSessionPinned("s1"));
    QVERIFY(!m_model->isSessionPinned("s2"));
    QVERIFY(!m_model->isSessionPinned(QString()));

    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 1);
    const int row1 = m_model->getSessionRow("s1");
    const int row2 = m_model->getSessionRow("s2");

    // Room for one: the least recently used row is pinned and passed over, so
    // the pass goes on to s2.
    QVERIFY(m_model->rowAt(row1).isLoaded());
    QVERIFY(!m_model->rowAt(row2).isLoaded());

    // Loading s2 again leaves the cache over its capacity by the pinned row,
    // and never evicts the session that is being returned.
    QCOMPARE(session("s2").storedAttribute(SessionKeys::SessionId).toString(), QStringLiteral("s2"));
    QVERIFY(m_model->rowAt(row1).isLoaded());
    QVERIFY(m_model->rowAt(row2).isLoaded());

    // Counted: the first unpin changes nothing, not even after an event-loop pass
    m_model->unpinSession("s1");
    QVERIFY(m_model->isSessionPinned("s1"));
    QCoreApplication::processEvents();
    QVERIFY(m_model->rowAt(row1).isLoaded());

    // The last unpin evicts nothing by itself; the pass it schedules does
    m_model->unpinSession("s1");
    QVERIFY(!m_model->isSessionPinned("s1"));
    QVERIFY(m_model->rowAt(row1).isLoaded());
    QTRY_VERIFY(!m_model->rowAt(row1).isLoaded());
    QVERIFY(m_model->rowAt(row2).isLoaded());

    // Unpinning something that is not pinned warns and changes nothing
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("session is not pinned")));
    m_model->unpinSession("s1");
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("session is not pinned")));
    m_model->unpinSession("no-such-session");
    QVERIFY(!m_model->isSessionPinned("no-such-session"));

    // A pin prevents eviction and nothing else: removal goes ahead. The pin
    // outlives the row and is balanced by its owner.
    m_model->pinSession("s2");
    QVERIFY(m_model->removeSessions({QStringLiteral("s2")}));
    QCOMPARE(m_model->getSessionRow("s2"), -1);
    QVERIFY(m_model->isSessionPinned("s2"));
    m_model->unpinSession("s2");
    QVERIFY(!m_model->isSessionPinned("s2"));
    QCoreApplication::processEvents();  // the scheduled pass finds no such row
    QCOMPARE(m_model->rowCount(), 1);
}

// What the job queue passes on after a publication: consumers are told to
// re-read, at once, and nothing about the row becomes persistent work.
void SessionModelEngineTest::calculationInvalidationIsPublished()
{
    QVERIFY(waitForIdle(*m_model));
    const int row1 = m_model->getSessionRow("s1");
    QVERIFY(!m_model->rowAt(row1).dirty);
    const qsizetype cachedBefore = m_model->rowAt(row1).cachedValues.size();

    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QSignalSpy dataSpy(m_model.get(), &SessionModel::dataChanged);
    QSignalSpy modelSpy(m_model.get(), &SessionModel::modelChanged);
    QSignalSpy workSpy(&m_model->scheduler(), &IdleScheduler::progressChanged);
    const DependencyKey x = DependencyKey::attribute(QStringLiteral("X"));

    m_model->publishCalculationInvalidation("s1", {x});
    QCOMPARE(dependencySpy.count(), 1);
    QCOMPARE(spyCountFor(dependencySpy, "s1", "X"), 1);
    QCOMPARE(dataSpy.count(), 1);
    QCOMPARE(publicationCount(dataSpy, row1), 1);
    QCOMPARE(modelSpy.count(), 1);

    // Not a persistent change: nothing dirty, no cached column dropped, and no
    // work for the idle saver or the column worker
    QVERIFY(!m_model->rowAt(row1).dirty);
    QCOMPARE(m_model->rowAt(row1).cachedValues.size(), cachedBefore);
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(workSpy.count(), 0);

    // Nothing at all for an empty set, an unknown id, or a row that is not loaded
    m_model->publishCalculationInvalidation("s1", {});
    m_model->publishCalculationInvalidation("no-such-session", {x});
    session("s1");
    session("s2");                      // s1 is now the least recently used
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 1);
    QVERIFY(!m_model->rowAt(row1).isLoaded());
    m_model->publishCalculationInvalidation("s1", {x});
    QCOMPARE(dependencySpy.count(), 1);
    QCOMPARE(dataSpy.count(), 1);
    QCOMPARE(modelSpy.count(), 1);
}

FLYSIGHT_TEST_MAIN(SessionModelEngineTest)
#include "tst_session_model_engine.moc"
