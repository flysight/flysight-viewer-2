// SessionData on the calculation engine, with the real built-ins in the
// process-wide registry and the real PreferencesManager behind the declared
// preference input. Every expectation is a literal; the only computed
// comparison is the engine's fresh-evaluation oracle.

#include <utility>

#include <QtTest>

#include "builtinfixture.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

constexpr double T0 = DescentFixture::T0;

DependencyKey attr(const char *key)
{
    return DependencyKey::attribute(QString::fromLatin1(key));
}

DependencyKey meas(const char *sensor, const char *name)
{
    return DependencyKey::measurement(QString::fromLatin1(sensor), QString::fromLatin1(name));
}

// TIME/time, tow, week whose fit is exactly a = 1, b = T0.
void addTimeData(SessionData &session)
{
    session.setMeasurement("TIME", "time", {10, 20, 30});
    session.setMeasurement("TIME", "tow", {129610, 129620, 129630});
    session.setMeasurement("TIME", "week", {2295, 2295, 2295});
}

// Eight samples: a descent of 200 m, a 30-second climb of 30 m, a descent of 230 m.
void addPausedDescent(SessionData &session)
{
    session.setMeasurement("GNSS", "time", {0, 10, 20, 30, 40, 50, 60, 70});
    session.setMeasurement("GNSS", "hMSL", {1000, 900, 800, 810, 820, 830, 700, 600});
}

QList<DependencyKey> goldenNames()
{
    QList<DependencyKey> names;
    for (const GoldenValue &golden : goldenValues())
        names.append(golden.name);
    return names;
}

void readAll(const SessionData &session, const QList<DependencyKey> &names, int first = 0, int step = 1)
{
    for (int i = first; i < names.size(); i += step) {
        const DependencyKey &name = names.at(i);
        if (name.type == DependencyKey::Type::Attribute)
            session.getAttribute(name.attributeKey);
        else
            session.getMeasurement(name.measurementKey.first, name.measurementKey.second);
    }
}

const QStringList wspOutputs = {
    "_WSP_ENTRY_TIME", "_WSP_EXIT_TIME", "_WSP_ENTRY_LAT", "_WSP_ENTRY_LON", "_WSP_EXIT_LAT",
    "_WSP_EXIT_LON", "_WSP_TIME_RESULT", "_WSP_DIST_RESULT", "_WSP_SPEED_RESULT", "_WSP_SEP_RESULT"};

} // namespace

class SessionEngineTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void multiOutputRunsOnce();
    void declaredInputChangeRunsOnceMore();
    void unrelatedChangeRunsNothing();
    void preferredSensorReplacesFallback();
    void overrideOneOutput();
    void overrideFlareStart();
    void declaredPreferenceInvalidates();
    void snapshotPreferencesDoNot();
    void derivedWTotalFollowsSource();
    void interpolatedAttributeFollows();
    void noUndeclaredReadsAcrossBuiltIns();
    void oracleOnFixture();
    void copyAndMoveSemantics();

private:
    // Reads every multi-output group of multiOutputRunsOnce.
    void readMultiOutputGroups(const SessionData &session);
};

void SessionEngineTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();
}

void SessionEngineTest::init()
{
    TestEnvironment::instance().resetPreferencesToDefaults();
}

void SessionEngineTest::readMultiOutputGroups(const SessionData &session)
{
    QCOMPARE(session.getAttribute("_TIME_FIT_B").toString(), QStringLiteral("1704110400"));
    QCOMPARE(session.getAttribute("_TIME_FIT_A").toString(), QStringLiteral("1"));
    QCOMPARE(session.getAttribute("_TIME_FIT_B").toString(), QStringLiteral("1704110400"));

    for (int pass = 0; pass < 2; ++pass) {
        QCOMPARE(session.getMeasurement("Simplified", "_time"), QVector<double>({T0, T0 + 295.0}));
        QCOMPARE(session.getMeasurement("Simplified", "lat").size(), 2);
        QCOMPARE(session.getMeasurement("Simplified", "hMSL"), QVector<double>({4000.0, 100.0}));
        QCOMPARE(session.getMeasurement("Simplified", "lon"), QVector<double>({-75.0, -75.0}));
    }

    for (const QString &key : wspOutputs)
        QVERIFY2(session.getAttribute(key).isValid(), qPrintable(key));
}

// Acceptance 9: a multi-output calculation runs once, whichever output is read
// first and however often.
void SessionEngineTest::multiOutputRunsOnce()
{
    const SessionData session = DescentFixture::load();
    readMultiOutputGroups(session);
    if (QTest::currentTestFailed())
        return;

    CalculationEngine &engine = session.calculationEngine();
    QCOMPARE(engine.runCount("builtin.time.fit"), 1);
    QCOMPARE(engine.runCount("builtin.simplified.track"), 1);
    QCOMPARE(engine.runCount("builtin.wsp.results"), 1);
}

// Acceptance 9: a change to a declared input causes exactly one new run.
void SessionEngineTest::declaredInputChangeRunsOnceMore()
{
    SessionData session = DescentFixture::load();
    readMultiOutputGroups(session);
    if (QTest::currentTestFailed())
        return;
    QCOMPARE(session.getMeasurement("IMU", "_time"), QVector<double>({T0 + 10.0, T0 + 20.0, T0 + 30.0}));

    const QSet<DependencyKey> invalidated =
        session.setMeasurement("TIME", "tow", {129611, 129621, 129631});
    QVERIFY(invalidated.contains(meas("TIME", "tow")));
    QVERIFY(invalidated.contains(attr("_TIME_FIT_A")));
    QVERIFY(invalidated.contains(attr("_TIME_FIT_B")));
    QVERIFY(invalidated.contains(meas("IMU", "_time")));
    QVERIFY(!invalidated.contains(meas("Simplified", "lat")));

    QCOMPARE(session.getAttribute("_TIME_FIT_B").toString(), QStringLiteral("1704110401"));
    QCOMPARE(session.getAttribute("_TIME_FIT_A").toString(), QStringLiteral("1"));
    QCOMPARE(session.getMeasurement("IMU", "_time"), QVector<double>({T0 + 11.0, T0 + 21.0, T0 + 31.0}));

    CalculationEngine &engine = session.calculationEngine();
    QCOMPARE(engine.runCount("builtin.time.fit"), 2);
    QCOMPARE(engine.runCount("builtin.simplified.track"), 1);
    QCOMPARE(engine.runCount("builtin.wsp.results"), 1);
}

// Acceptance 9: an unrelated change causes no run at all.
void SessionEngineTest::unrelatedChangeRunsNothing()
{
    SessionData session = DescentFixture::load();
    const QList<DependencyKey> names = goldenNames();
    readAll(session, names);

    CalculationEngine &engine = session.calculationEngine();
    const int runs = engine.totalRunCount();
    QVERIFY(runs > 0);

    const QSet<DependencyKey> invalidated = session.setAttribute("_DESCRIPTION", "x");
    QCOMPARE(invalidated, QSet<DependencyKey>({attr("_DESCRIPTION")}));

    readAll(session, names);
    QCOMPARE(engine.totalRunCount(), runs);
}

// Acceptance 10: a candidate that could not run because its input was absent
// replaces the cached fallback once the input arrives.
void SessionEngineTest::preferredSensorReplacesFallback()
{
    SessionData session;
    addTimeData(session);
    session.setMeasurement("BARO", "time", {10, 20, 30});

    QCOMPARE(session.getAttribute("_START_TIME").toDouble(), T0 + 10.0);
    QCOMPARE(session.getAttribute("_DURATION").toDouble(), 20.0);

    const QSet<DependencyKey> invalidated =
        session.setMeasurement("GNSS", "time", {1704110405.0, 1704110406.0});
    QVERIFY(invalidated.contains(attr("_START_TIME")));
    QVERIFY(invalidated.contains(attr("_DURATION")));

    QCOMPARE(session.getAttribute("_START_TIME").toDouble(), 1704110405.0);
    QCOMPARE(session.getAttribute("_DURATION").toDouble(), 1.0);

    CalculationEngine &engine = session.calculationEngine();
    QCOMPARE(engine.runCount("builtin.attr.timeExtent.BARO"), 1);
    QCOMPARE(engine.runCount("builtin.attr.timeExtent.GNSS"), 1);
    QVERIFY(engine.verifyAgainstFresh({attr("_START_TIME"), attr("_DURATION")}).isEmpty());
}

// Acceptance 11: a user override of one output of a multi-output calculation
// coexists with the calculation's other outputs and causes no cycle.
void SessionEngineTest::overrideOneOutput()
{
    SessionData session;
    addTimeData(session);
    session.setMeasurement("BARO", "time", {10, 20, 30});

    session.setAttribute("_TIME_FIT_A", QStringLiteral("2"));
    QCOMPARE(session.getAttribute("_TIME_FIT_A").toString(), QStringLiteral("2"));
    QCOMPARE(session.getAttribute("_TIME_FIT_B").toString(), QStringLiteral("1704110400"));
    QCOMPARE(session.getMeasurement("BARO", "_time").value(0), 1704110420.0);

    CalculationEngine &engine = session.calculationEngine();
    QCOMPARE(engine.cycleCount(), 0);
    QCOMPARE(engine.runCount("builtin.time.fit"), 1);

    // Removing the override brings the calculated value back without a new run.
    const QSet<DependencyKey> invalidated = session.removeAttribute("_TIME_FIT_A");
    QVERIFY(invalidated.contains(meas("BARO", "_time")));
    QCOMPARE(session.getAttribute("_TIME_FIT_A").toString(), QStringLiteral("1"));
    QCOMPARE(engine.runCount("builtin.time.fit"), 1);
    QCOMPARE(session.getMeasurement("BARO", "_time").value(0), 1704110410.0);
    QCOMPARE(engine.cycleCount(), 0);
}

// Acceptance 11, on a marker pair.
void SessionEngineTest::overrideFlareStart()
{
    SessionData session = DescentFixture::load();
    session.setAttribute(SessionKeys::FlareStartTime, T0 + 150.0);

    QCOMPARE(session.getAttribute(SessionKeys::FlareStartTime).toDouble(), T0 + 150.0);
    QCOMPARE(session.getAttribute(SessionKeys::FlareEndTime).toDouble(), T0 + 204.0);
    QCOMPARE(session.calculationEngine().runCount("builtin.attr.flare"), 1);
    QCOMPARE(session.calculationEngine().cycleCount(), 0);
}

// Acceptance 15: changing a declared preference input invalidates dependents.
void SessionEngineTest::declaredPreferenceInvalidates()
{
    SessionData session;
    addPausedDescent(session);

    QCOMPARE(session.getAttribute("_ANALYSIS_START_TIME").toDouble(), 0.0);
    QCOMPARE(session.getAttribute("_ANALYSIS_END_TIME").toDouble(), 70.0);
    QCOMPARE(session.getAttribute("_GROUND_ELEV").toDouble(), 600.0);

    QList<QSet<DependencyKey>> delivered;
    session.calculationEngine().setInvalidationListener(
        [&delivered](const QSet<DependencyKey> &keys) { delivered.append(keys); });

    PreferencesManager::instance().setValue(PreferenceKeys::ImportDescentPauseSeconds, 5.0);

    QCOMPARE(delivered.size(), 1);
    QVERIFY(delivered.first().contains(attr("_ANALYSIS_START_TIME")));
    QVERIFY(delivered.first().contains(attr("_GROUND_ELEV")));

    QCOMPARE(session.getAttribute("_ANALYSIS_START_TIME").toDouble(), 45.0);
    QCOMPARE(session.getAttribute("_ANALYSIS_END_TIME").toDouble(), 70.0);
    QCOMPARE(session.getAttribute("_GROUND_ELEV").toDouble(), 600.0);
    QCOMPARE(session.getMeasurement("GNSS", "z"),
             QVector<double>({400, 300, 200, 210, 220, 230, 100, 0}));
    QCOMPARE(session.calculationEngine().runCount("builtin.attr.analysisRange"), 2);
}

// Acceptance 15: preferences that are snapshotted into a session at import are
// session attributes; changing them later does not touch existing sessions.
void SessionEngineTest::snapshotPreferencesDoNot()
{
    SessionData session;
    addPausedDescent(session);
    session.setAttribute("_JUMPER_MASS", 80.0);
    session.setAttribute("_PLANFORM_AREA", 2.0);
    session.setAttribute("_GROUND_ELEV", 50.0);

    QCOMPARE(session.getMeasurement("GNSS", "z").value(0), 950.0);
    QCOMPARE(session.getAttribute("_ANALYSIS_START_TIME").toDouble(), 0.0);

    CalculationEngine &engine = session.calculationEngine();
    const int runs = engine.totalRunCount();
    int deliveries = 0;
    engine.setInvalidationListener([&deliveries](const QSet<DependencyKey> &) { ++deliveries; });

    PreferencesManager &prefs = PreferencesManager::instance();
    prefs.setValue(PreferenceKeys::AeroMass, 90.0);
    prefs.setValue(PreferenceKeys::AeroArea, 3.0);
    prefs.setValue(PreferenceKeys::ImportFixedElevation, 10.0);

    QCOMPARE(deliveries, 0);
    QCOMPARE(session.getAttribute("_JUMPER_MASS").toDouble(), 80.0);
    QCOMPARE(session.getAttribute("_PLANFORM_AREA").toDouble(), 2.0);
    QCOMPARE(session.getAttribute("_GROUND_ELEV").toDouble(), 50.0);
    QCOMPARE(session.getMeasurement("GNSS", "z").value(0), 950.0);
    QCOMPARE(engine.totalRunCount(), runs);
}

// Acceptance 16: a derived measurement follows its sources, and a supplied
// measurement of the same name keeps precedence over the derived one.
void SessionEngineTest::derivedWTotalFollowsSource()
{
    SessionData session;
    session.setMeasurement("IMU", "wx", {3.0});
    session.setMeasurement("IMU", "wy", {4.0});
    session.setMeasurement("IMU", "wz", {0.0});

    // BASELINE: no gyro correction - Phase 4 (5.0 becomes 5.0 * 1.14688)
    QCOMPARE(session.getMeasurement("IMU", "wTotal"), QVector<double>({5.0}));
    QVERIFY(!session.hasMeasurement("IMU", "wTotal"));

    const QSet<DependencyKey> invalidated = session.setMeasurement("IMU", "wx", {6.0});
    QVERIFY(invalidated.contains(meas("IMU", "wTotal")));
    session.setMeasurement("IMU", "wy", {8.0});

    // BASELINE: no gyro correction - Phase 4 (10.0 becomes 10.0 * 1.14688)
    QCOMPARE(session.getMeasurement("IMU", "wTotal"), QVector<double>({10.0}));
    QVERIFY(!session.hasMeasurement("IMU", "wTotal"));

    CalculationEngine &engine = session.calculationEngine();
    const int runs = engine.runCount("builtin.imu.wTotal");
    QCOMPARE(runs, 2);

    session.setMeasurement("IMU", "wTotal", {42.0});
    QCOMPARE(session.getMeasurement("IMU", "wTotal"), QVector<double>({42.0}));
    QCOMPARE(engine.runCount("builtin.imu.wTotal"), runs);
    QVERIFY(session.hasMeasurement("IMU", "wTotal"));
}

// Acceptance 16: an interpolated attribute follows the measurement, the time
// axis, and the time attribute it is built from.
void SessionEngineTest::interpolatedAttributeFollows()
{
    SessionData session;
    addTimeData(session);
    session.setMeasurement("IMU", "time", {10, 20, 30});
    session.setMeasurement("IMU", "wx", {1, 2, 3});
    session.setAttribute("_M", 1704110415.0);

    const QString key = SessionData::interpolationKey("_M", "IMU", "_time", "wx");
    QVERIFY(qAbs(session.getAttribute(key).toDouble() - 1.5) <= 1e-9);
    QVERIFY(!session.hasAttribute(key));

    QVERIFY(session.setMeasurement("IMU", "wx", {10, 20, 30}).contains(DependencyKey::attribute(key)));
    QVERIFY(qAbs(session.getAttribute(key).toDouble() - 15.0) <= 1e-9);

    QVERIFY(session.setAttribute("_M", 1704110425.0).contains(DependencyKey::attribute(key)));
    QVERIFY(qAbs(session.getAttribute(key).toDouble() - 25.0) <= 1e-9);

    session.setAttribute("_M", 1704110500.0);   // beyond the last sample
    QVERIFY(!session.getAttribute(key).isValid());
    QCOMPARE(session.calculationEngine().cachedState(DependencyKey::attribute(key)),
             CalculationEngine::CachedState::Unavailable);
}

// Every built-in reads only what it declares, and none is on a cycle.
void SessionEngineTest::noUndeclaredReadsAcrossBuiltIns()
{
    const SessionData session = DescentFixture::load();
    CalculationEngine &engine = session.calculationEngine();
    const CalculationRegistry &registry = CalculationRegistry::instance();

    int plain = 0;
    for (const QString &id : registry.registeredIds()) {
        if (registry.isFamily(id))
            continue;
        ++plain;
        QVERIFY2(engine.request(id).found, qPrintable(id));
    }
    QCOMPARE(plain, 67);

    QCOMPARE(session.getAttribute("_EXIT_TIME:GNSS/_time/hMSL").toDouble(), 4000.0);
    QCOMPARE(session.getAttribute("_WSP_ENTRY_TIME:GNSS/_time/hMSL").toDouble(), 2600.0);

    QCOMPARE(engine.undeclaredReadCount(), 0);
    QCOMPARE(engine.cycleCount(), 0);
    QCOMPARE(engine.scopeDepth(), 0);

    // On the full fixture nothing is missing an input.
    for (const QString &id : registry.registeredIds()) {
        if (!registry.isFamily(id))
            QVERIFY2(engine.resultStatus(id) == ResultStatus::Ok, qPrintable(id));
    }
}

// Spec 7.4: after any sequence of reads and edits, every value equals the value
// of a fresh evaluation with caches cleared.
void SessionEngineTest::oracleOnFixture()
{
    SessionData session = DescentFixture::load();
    CalculationEngine &engine = session.calculationEngine();
    const QList<DependencyKey> names = goldenNames();

    auto mismatches = [&engine, &names]() {
        QStringList tags;
        for (const DependencyKey &name : engine.verifyAgainstFresh(names))
            tags.append(goldenTag(name));
        return tags.join(QStringLiteral(", "));
    };

    readAll(session, names);
    QCOMPARE(mismatches(), QString());

    session.setAttribute("_GROUND_ELEV", 0.0);
    readAll(session, names, 0, 3);
    QCOMPARE(mismatches(), QString());

    session.removeAttribute("_GROUND_ELEV");
    readAll(session, names, 1, 3);
    QCOMPARE(mismatches(), QString());

    session.setAttribute("_EXIT_TIME", T0 + 30.0);
    readAll(session, names, 2, 3);
    QCOMPARE(mismatches(), QString());

    QVector<double> velD = session.getMeasurement("GNSS", "velD");
    for (double &v : velD)
        v *= 0.5;
    session.setMeasurement("GNSS", "velD", velD);
    readAll(session, names, 0, 2);
    QCOMPARE(mismatches(), QString());

    PreferencesManager::instance().setValue(PreferenceKeys::ImportDescentPauseSeconds, 5.0);
    readAll(session, names, 1, 2);
    QCOMPARE(mismatches(), QString());

    QCOMPARE(engine.undeclaredReadCount(), 0);
    QCOMPARE(engine.cycleCount(), 0);
}

// A copy is the stored state only (cold engine of its own); a move carries the
// engine and its cache to the new object.
void SessionEngineTest::copyAndMoveSemantics()
{
    SessionData a = DescentFixture::load();
    QCOMPARE(a.getAttribute("_EXIT_TIME").toDouble(), T0 + 9.0);
    QCOMPARE(a.getMeasurement("GNSS", "z").value(0), 3900.0);

    const int runsOnOriginal = a.calculationEngine().totalRunCount();
    QVERIFY(runsOnOriginal > 0);

    // Copy: equal values, computed by the copy's own engine.
    SessionData b = a;
    QCOMPARE(b.calculationEngine().totalRunCount(), 0);
    QCOMPARE(b.calculationEngine().cachedState(attr("_EXIT_TIME")),
             CalculationEngine::CachedState::NotCached);
    QCOMPARE(b.getAttribute("_EXIT_TIME").toDouble(), T0 + 9.0);
    QCOMPARE(b.getMeasurement("GNSS", "z").value(0), 3900.0);
    QVERIFY(b.calculationEngine().totalRunCount() > 0);
    QCOMPARE(a.calculationEngine().totalRunCount(), runsOnOriginal);
    QCOMPARE(b.attributeKeys(), a.attributeKeys());
    QCOMPARE(b.sensorKeys(), a.sensorKeys());

    // An edit of the copy does not reach the original.
    b.setAttribute("_GROUND_ELEV", 0.0);
    QCOMPARE(b.getMeasurement("GNSS", "z").value(0), 4000.0);
    QCOMPARE(a.getMeasurement("GNSS", "z").value(0), 3900.0);
    QCOMPARE(a.calculationEngine().totalRunCount(), runsOnOriginal);

    // Move: the cache comes along and serves reads without new runs.
    int deliveries = 0;
    a.calculationEngine().setInvalidationListener([&deliveries](const QSet<DependencyKey> &) { ++deliveries; });

    SessionData c = std::move(a);
    CalculationEngine &moved = c.calculationEngine();
    QCOMPARE(moved.cachedState(attr("_EXIT_TIME")), CalculationEngine::CachedState::Available);
    QCOMPARE(moved.totalRunCount(), runsOnOriginal);
    QCOMPARE(c.getAttribute("_EXIT_TIME").toDouble(), T0 + 9.0);
    QCOMPARE(c.getMeasurement("GNSS", "z").value(0), 3900.0);
    QCOMPARE(moved.totalRunCount(), runsOnOriginal);

    // The engine reads the state at its new address, and kept its listener.
    const QSet<DependencyKey> invalidated = c.setAttribute("_GROUND_ELEV", 50.0);
    QVERIFY(invalidated.contains(meas("GNSS", "z")));
    QCOMPARE(c.getMeasurement("GNSS", "z").value(0), 3950.0);
    PreferencesManager::instance().setValue(PreferenceKeys::ImportDescentPauseSeconds, 5.0);
    QCOMPARE(deliveries, 1);

    // Move assignment behaves the same way.
    SessionData d;
    d = std::move(c);
    QCOMPARE(d.calculationEngine().cachedState(meas("GNSS", "z")), CalculationEngine::CachedState::Available);
    QCOMPARE(d.getMeasurement("GNSS", "z").value(0), 3950.0);

    // A moved-from session is empty but usable.
    QVERIFY(!a.getAttribute("_EXIT_TIME").isValid());
}

FLYSIGHT_TEST_MAIN(SessionEngineTest)
#include "tst_session_engine.moc"
