// SessionData on the calculation engine, with the real built-ins in the
// process-wide registry and the real PreferencesManager behind the declared
// preference input. Every expectation is a literal; the only computed
// comparison is the engine's fresh-evaluation oracle.

#include <memory>
#include <stdexcept>
#include <utility>

#include <QtTest>

#include "asyncdriver.h"
#include "builtinfixture.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "fakesessionstate.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;
using Synthetic::attr;
using Synthetic::measKey;

namespace {

constexpr double T0 = DescentFixture::T0;

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
    void cleanup();

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

    // Acceptance clauses on a real session with temporary global registrations
    void explicitPolicyOnSession();
    void asyncRequestOnSession();
    void safetyOnRealSession();

private:
    // Registers on the global registry; cleanup() removes it again.
    bool registerTemporary(const CalculationDescriptor &d);
    QStringList m_registryBefore;
    QStringList m_temporaryIds;

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
    m_registryBefore = CalculationRegistry::instance().registeredIds();
    m_temporaryIds.clear();
}

// The global registry is left as it was found.
void SessionEngineTest::cleanup()
{
    for (const QString &id : std::as_const(m_temporaryIds))
        CalculationRegistry::instance().unregister(id);
    m_temporaryIds.clear();
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
}

bool SessionEngineTest::registerTemporary(const CalculationDescriptor &d)
{
    if (!CalculationRegistry::instance().registerCalculation(d))
        return false;
    m_temporaryIds.append(d.id);
    return true;
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
    QVERIFY(invalidated.contains(measKey("TIME", "tow")));
    QVERIFY(invalidated.contains(attr("_TIME_FIT_A")));
    QVERIFY(invalidated.contains(attr("_TIME_FIT_B")));
    QVERIFY(invalidated.contains(measKey("IMU", "_time")));
    QVERIFY(!invalidated.contains(measKey("Simplified", "lat")));

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
    QVERIFY(invalidated.contains(measKey("BARO", "_time")));
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

    // No SCHEMA_VER: the gyro is legacy-scaled, so the derived magnitude is
    // 5 x 1.14688 (compared with a tolerance; the product is not exact).
    QVector<double> wTotal = session.getMeasurement("IMU", "wTotal");
    QCOMPARE(wTotal.size(), 1);
    QVERIFY(qAbs(wTotal.at(0) - 5.7344) <= 1e-9);
    QVERIFY(!session.hasMeasurement("IMU", "wTotal"));

    const QSet<DependencyKey> invalidated = session.setMeasurement("IMU", "wx", {6.0});
    QVERIFY(invalidated.contains(measKey("IMU", "wTotal")));
    session.setMeasurement("IMU", "wy", {8.0});

    wTotal = session.getMeasurement("IMU", "wTotal");
    QCOMPARE(wTotal.size(), 1);
    QVERIFY(qAbs(wTotal.at(0) - 11.4688) <= 1e-9);
    QVERIFY(!session.hasMeasurement("IMU", "wTotal"));

    CalculationEngine &engine = session.calculationEngine();
    const int runs = engine.runCount("builtin.imu.wTotal");
    QCOMPARE(runs, 2);

    // A supplied wTotal wins and is never corrected: only wx / wy / wz are in
    // the schema table.
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
    // The interpolation reads the effective (legacy-corrected) gyro: 1.5 x 1.14688.
    QVERIFY(qAbs(session.getAttribute(key).toDouble() - 1.72032) <= 1e-9);
    QVERIFY(!session.hasAttribute(key));

    QVERIFY(session.setMeasurement("IMU", "wx", {10, 20, 30}).contains(DependencyKey::attribute(key)));
    QVERIFY(qAbs(session.getAttribute(key).toDouble() - 17.2032) <= 1e-9);

    QVERIFY(session.setAttribute("_M", 1704110425.0).contains(DependencyKey::attribute(key)));
    QVERIFY(qAbs(session.getAttribute(key).toDouble() - 28.672) <= 1e-9);

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
    QCOMPARE(plain, 70);

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

// Idempotency: after any sequence of reads and edits, every value equals the value
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
    QVERIFY(invalidated.contains(measKey("GNSS", "z")));
    QCOMPARE(c.getMeasurement("GNSS", "z").value(0), 3950.0);
    PreferencesManager::instance().setValue(PreferenceKeys::ImportDescentPauseSeconds, 5.0);
    QCOMPARE(deliveries, 1);

    // Move assignment behaves the same way.
    SessionData d;
    d = std::move(c);
    QCOMPARE(d.calculationEngine().cachedState(measKey("GNSS", "z")), CalculationEngine::CachedState::Available);
    QCOMPARE(d.getMeasurement("GNSS", "z").value(0), 3950.0);

    // A moved-from session is empty but usable.
    QVERIFY(!a.getAttribute("_EXIT_TIME").isValid());
}

// Acceptance 14, on a real SessionData and the global registry: an
// explicit-policy calculation reports unavailable until requested, and
// requesting it publishes all of its outputs at once.
void SessionEngineTest::explicitPolicyOnSession()
{
    const QString id = QStringLiteral("test.explicit.pair");
    CalculationDescriptor d;
    d.id = id;
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute("_EXIT_TIME")};
    d.outputs = {attr("_T_EXPL_A"), attr("_T_EXPL_B")};
    d.compute = [](const EvaluationContext &ctx) {
        const double exit = ctx.attribute("_EXIT_TIME").toDouble();
        CalculationResult r;
        r.setAttribute("_T_EXPL_A", exit + 1.0);
        r.setAttribute("_T_EXPL_B", exit + 2.0);
        return r;
    };
    QVERIFY(registerTemporary(d));

    SessionData session = DescentFixture::load();
    CalculationEngine &engine = session.calculationEngine();
    const QList<DependencyKey> both = {attr("_T_EXPL_A"), attr("_T_EXPL_B")};

    // Before the request: unavailable, and reading starts no work
    QVERIFY(!session.getAttribute("_T_EXPL_A").isValid());
    QVERIFY(!session.getAttribute("_T_EXPL_B").isValid());
    QCOMPARE(engine.runCount(id), 0);
    QVERIFY(engine.resultStatus(id).has_value());
    QVERIFY(*engine.resultStatus(id) == ResultStatus::NotRequested);
    QVERIFY(engine.verifyAgainstFresh(both).isEmpty());

    // The request publishes one result for both outputs
    const CalculationEngine::RequestOutcome first = engine.request(id);
    QVERIFY(first.found);
    QVERIFY(first.status == ResultStatus::Ok);
    QVERIFY(first.invalidated.contains(attr("_T_EXPL_A")));
    QVERIFY(first.invalidated.contains(attr("_T_EXPL_B")));
    QCOMPARE(session.getAttribute("_T_EXPL_A").toDouble(), 1704110410.0);
    QCOMPARE(session.getAttribute("_T_EXPL_B").toDouble(), 1704110411.0);
    QCOMPARE(engine.runCount(id), 1);
    QVERIFY(engine.verifyAgainstFresh(both).isEmpty());

    // A valid result is not recomputed
    const CalculationEngine::RequestOutcome second = engine.request(id);
    QVERIFY(second.found);
    QVERIFY(second.status == ResultStatus::Ok);
    QCOMPARE(engine.runCount(id), 1);

    // An input change reverts the result to "not requested"; nothing runs
    session.setAttribute("_EXIT_TIME", T0 + 20.0);
    QVERIFY(!session.getAttribute("_T_EXPL_A").isValid());
    QVERIFY(!session.getAttribute("_T_EXPL_B").isValid());
    QCOMPARE(engine.runCount(id), 1);
    QVERIFY(engine.verifyAgainstFresh(both).isEmpty());

    QVERIFY(engine.request(id).found);
    QCOMPARE(session.getAttribute("_T_EXPL_A").toDouble(), 1704110421.0);
    QCOMPARE(session.getAttribute("_T_EXPL_B").toDouble(), 1704110422.0);
    QCOMPARE(engine.runCount(id), 2);
    QVERIFY(engine.verifyAgainstFresh(both).isEmpty());
}

// The asynchronous request against real SessionData ownership: the engine is
// held by pointer, so moving a session keeps the ticket valid; destroying the
// session (or move-assigning over it) destroys the engine, and copy-assigning
// over it clears the engine. In every case the engine, not the caller, decides
// whether the computed result may be installed.
void SessionEngineTest::asyncRequestOnSession()
{
    using Prepare = CalculationEngine::PrepareOutcome;
    const QString id = QStringLiteral("test.async.pair");
    CalculationDescriptor d;
    d.id = id;
    d.title = QStringLiteral("Async pair");
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute("_EXIT_TIME")};
    d.outputs = {attr("_T_ASYNC_A"), attr("_T_ASYNC_B")};
    d.compute = [](const EvaluationContext &ctx) {
        const double exit = ctx.attribute("_EXIT_TIME").toDouble();
        CalculationResult r;
        r.setAttribute("_T_ASYNC_A", exit + 1.0);
        r.setAttribute("_T_ASYNC_B", exit + 2.0);
        return r;
    };
    QVERIFY(registerTemporary(d));
    const QList<DependencyKey> both = {attr("_T_ASYNC_A"), attr("_T_ASYNC_B")};

    {
        // Published, on a worker thread; blocker inspection before and after.
        SessionData session = DescentFixture::load();
        CalculationEngine &engine = session.calculationEngine();
        for (const DependencyKey &name : both) {
            const BlockerReport report = engine.blockers(name);
            QVERIFY(report.state == BlockerReport::State::Blocked);
            QCOMPARE(report.blockers.size(), 1);
            QCOMPARE(report.blockers.first().registrationId, id);
            QCOMPARE(report.blockers.first().title, QStringLiteral("Async pair"));
        }
        QVERIFY(!session.getAttribute("_T_ASYNC_A").isValid());
        QCOMPARE(engine.runCount(id), 0);

        Prepare prepared = engine.prepare(id);
        QVERIFY(prepared.kind == Prepare::Kind::Ready);
        ComputedCalculation computed = computeOn(ComputeMode::StdThread, *prepared.ticket);
        QVERIFY(!session.getAttribute("_T_ASYNC_B").isValid());     // computed, not published
        const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
        QVERIFY(outcome.kind == PublishOutcome::Kind::Published);
        QVERIFY(outcome.status == ResultStatus::Ok);
        QVERIFY(outcome.invalidated.contains(attr("_T_ASYNC_A")));
        QVERIFY(outcome.invalidated.contains(attr("_T_ASYNC_B")));
        QCOMPARE(session.getAttribute("_T_ASYNC_A").toDouble(), 1704110410.0);
        QCOMPARE(session.getAttribute("_T_ASYNC_B").toDouble(), 1704110411.0);
        QCOMPARE(engine.runCount(id), 1);
        for (const DependencyKey &name : both)
            QVERIFY(engine.blockers(name).state == BlockerReport::State::Available);
        QVERIFY(engine.verifyAgainstFresh(both).isEmpty());
    }
    {
        // The session is moved between prepare and publish: same engine, still valid.
        SessionData session = DescentFixture::load();
        Prepare prepared = session.calculationEngine().prepare(id);
        QVERIFY(prepared.kind == Prepare::Kind::Ready);
        ComputedCalculation computed = computeOn(ComputeMode::QtThread, *prepared.ticket);

        SessionData moved = std::move(session);
        const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
        QVERIFY(outcome.kind == PublishOutcome::Kind::Published);
        QCOMPARE(moved.getAttribute("_T_ASYNC_A").toDouble(), 1704110410.0);
        QCOMPARE(moved.calculationEngine().runCount(id), 1);
        QVERIFY(moved.calculationEngine().verifyAgainstFresh(both).isEmpty());
    }
    {
        // The session is destroyed: the ticket outlives the engine.
        auto session = std::make_unique<SessionData>(DescentFixture::load());
        Prepare prepared = session->calculationEngine().prepare(id);
        QVERIFY(prepared.kind == Prepare::Kind::Ready);
        ComputedCalculation computed = computeOn(ComputeMode::StdThread, *prepared.ticket);
        session.reset();
        const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
        QVERIFY(outcome.kind == PublishOutcome::Kind::RefusedGone);
        QVERIFY(outcome.reason == PublishOutcome::Reason::SessionGone);
    }
    {
        // Move-assigned over: the old engine is destroyed with the old state.
        SessionData session = DescentFixture::load();
        Prepare prepared = session.calculationEngine().prepare(id);
        QVERIFY(prepared.kind == Prepare::Kind::Ready);
        ComputedCalculation computed = computeOn(ComputeMode::StdThread, *prepared.ticket);
        session = DescentFixture::load();
        const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
        QVERIFY(outcome.kind == PublishOutcome::Kind::RefusedGone);
        QVERIFY(outcome.reason == PublishOutcome::Reason::SessionGone);
        QVERIFY(!session.getAttribute("_T_ASYNC_A").isValid());
    }
    {
        // Copy-assigned over: the engine stays and is cleared, so the inputs are stale.
        SessionData session = DescentFixture::load();
        const SessionData other = DescentFixture::load();
        Prepare prepared = session.calculationEngine().prepare(id);
        QVERIFY(prepared.kind == Prepare::Kind::Ready);
        ComputedCalculation computed = computeOn(ComputeMode::StdThread, *prepared.ticket);
        session = other;
        const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
        QVERIFY(outcome.kind == PublishOutcome::Kind::RefusedStale);
        QVERIFY(outcome.reason == PublishOutcome::Reason::InputsChanged);
        QVERIFY(!session.getAttribute("_T_ASYNC_A").isValid());
        QCOMPARE(session.calculationEngine().runCount(id), 0);
    }
    {
        // An edit of the declared input while the calculation runs.
        SessionData session = DescentFixture::load();
        CalculationEngine &engine = session.calculationEngine();
        Prepare prepared = engine.prepare(id);
        QVERIFY(prepared.kind == Prepare::Kind::Ready);
        ComputedCalculation computed = computeOn(ComputeMode::StdThread, *prepared.ticket);
        session.setAttribute("_EXIT_TIME", T0 + 20.0);
        const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
        QVERIFY(outcome.kind == PublishOutcome::Kind::RefusedStale);
        QVERIFY(outcome.reason == PublishOutcome::Reason::InputsChanged);
        QVERIFY(!session.getAttribute("_T_ASYNC_A").isValid());
        QCOMPARE(engine.runCount(id), 0);

        // Still requestable, and an unrelated edit does not refuse.
        Prepare again = engine.prepare(id);
        QVERIFY(again.kind == Prepare::Kind::Ready);
        ComputedCalculation recomputed = computeOn(ComputeMode::StdThread, *again.ticket);
        session.setAttribute("_T_UNRELATED", 1);
        QVERIFY(again.ticket->publish(std::move(recomputed)).kind == PublishOutcome::Kind::Published);
        QCOMPARE(session.getAttribute("_T_ASYNC_A").toDouble(), 1704110421.0);
        QCOMPARE(session.getAttribute("_T_ASYNC_B").toDouble(), 1704110422.0);
        QVERIFY(engine.verifyAgainstFresh(both).isEmpty());
    }
}

// Acceptance 12, on a real session next to the real built-ins: a thrown
// exception, a calculation nested on the failed one, and a two-calculation
// cycle leave no partial result and no corrupted evaluation state.
void SessionEngineTest::safetyOnRealSession()
{
    CalculationDescriptor thrower;
    thrower.id = QStringLiteral("test.throw");
    thrower.inputs = {CalcInput::attribute("_EXIT_TIME")};
    thrower.outputs = {attr("_T_THROW_A"), attr("_T_THROW_B")};
    thrower.compute = [](const EvaluationContext &ctx) -> CalculationResult {
        CalculationResult r;
        r.setAttribute("_T_THROW_A", ctx.attribute("_EXIT_TIME").toDouble());   // must never be published
        if (r.contains(DependencyKey::attribute(QStringLiteral("_T_THROW_A"))))
            throw std::runtime_error("x");
        return r;
    };
    QVERIFY(registerTemporary(thrower));

    CalculationDescriptor nested;
    nested.id = QStringLiteral("test.nested");
    nested.inputs = {CalcInput::attribute("_T_THROW_A")};
    nested.outputs = {attr("_T_NESTED")};
    nested.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute("_T_NESTED", ctx.attribute("_T_THROW_A").toDouble() + 1.0);
    };
    QVERIFY(registerTemporary(nested));

    CalculationDescriptor cycX;
    cycX.id = QStringLiteral("test.cycX");
    cycX.inputs = {CalcInput::attribute("_T_CYC_Y")};
    cycX.outputs = {attr("_T_CYC_X")};
    cycX.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute("_T_CYC_X", ctx.attribute("_T_CYC_Y").toDouble() + 1.0);
    };
    QVERIFY(registerTemporary(cycX));

    CalculationDescriptor cycY;
    cycY.id = QStringLiteral("test.cycY");
    cycY.inputs = {CalcInput::attribute("_T_CYC_X")};
    cycY.outputs = {attr("_T_CYC_Y")};
    cycY.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute("_T_CYC_Y", ctx.attribute("_T_CYC_X").toDouble() + 1.0);
    };
    QVERIFY(registerTemporary(cycY));

    const SessionData session = DescentFixture::load();
    CalculationEngine &engine = session.calculationEngine();

    const QStringList testNames = {"_T_NESTED", "_T_THROW_A", "_T_THROW_B", "_T_CYC_X", "_T_CYC_Y"};
    for (int pass = 0; pass < 3; ++pass) {
        for (const QString &name : testNames)
            QVERIFY2(!session.getAttribute(name).isValid(), qPrintable(name));
        QCOMPARE(engine.scopeDepth(), 0);
    }

    QVERIFY(engine.resultStatus(QStringLiteral("test.throw")).has_value());
    QVERIFY(*engine.resultStatus(QStringLiteral("test.throw")) == ResultStatus::Failed);
    QVERIFY(engine.resultStatus(QStringLiteral("test.nested")).has_value());
    QVERIFY(*engine.resultStatus(QStringLiteral("test.nested")) == ResultStatus::MissingInput);
    QVERIFY(engine.cycleCount() >= 1);
    QCOMPARE(engine.runCount(QStringLiteral("test.throw")), 1);     // the failure is cached
    QCOMPARE(engine.runCount(QStringLiteral("test.nested")), 0);

    // The real values around them are intact
    QCOMPARE(session.getAttribute("_EXIT_TIME").toDouble(), T0 + 9.0);
    QVERIFY(qAbs(session.getMeasurement("IMU", "wTotal").value(0) - 5.7344) <= 1e-9);
    QCOMPARE(engine.scopeDepth(), 0);

    QList<DependencyKey> names = goldenNames();
    for (const QString &name : testNames)
        names.append(DependencyKey::attribute(name));
    QVERIFY(engine.verifyAgainstFresh(names).isEmpty());
}

FLYSIGHT_TEST_MAIN(SessionEngineTest)
#include "tst_session_engine.moc"
