// SessionMerge: the merge rules of spec 6.3 (attribute conflicts) and 6.4
// (measurement merge) as a pure plan-then-apply function on two SessionData,
// without a model or a logbook. Sessions are built programmatically; every
// expected value and message is a literal.

#include <QtTest>

#include <cmath>
#include <limits>

#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "sessiondata.h"
#include "sessionmerge.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

SessionData baseSession()
{
    SessionData s;
    s.setAttribute("SESSION_ID", QStringLiteral("m1"));
    s.setAttribute("FIRMWARE_VER", QStringLiteral("v2023.09.22"));
    s.setAttribute("DEVICE_ID", QStringLiteral("abc"));
    return s;
}

QStringList plannedAttributeKeys(const MergePlan &plan)
{
    QStringList keys;
    for (const auto &attribute : plan.attributesToSet)
        keys.append(attribute.first);
    return keys;
}

// "SENSOR/column" names of the plan's columns, sorted.
QStringList plannedColumns(const MergePlan &plan)
{
    QStringList names;
    for (auto sensorIt = plan.columnsToSet.constBegin(); sensorIt != plan.columnsToSet.constEnd(); ++sensorIt) {
        for (auto colIt = sensorIt.value().constBegin(); colIt != sensorIt.value().constEnd(); ++colIt)
            names.append(sensorIt.key() + QLatin1Char('/') + colIt.key());
    }
    return names;
}

} // namespace

class SessionMergeTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    // attributes (spec 6.3)
    void absentAttributeIsAdded();
    void equalAttributeIsNoop();
    void differentAttributeConflicts();
    void absenceNeverConflicts();
    void explicitSchemaMismatch();
    void multipleConflictsSorted();
    void typedVersusTextEquality();
    void viewerAttributesExistingWins();
    void deviceIdPlaceholder();

    // measurements (spec 6.4)
    void columnsReplaceAddKeep();
    void unitOnlyChangeIsAChange();
    void identicalIsEmpty();
    void raggedIsRejected();
    void disjointSensorsUnaffected();

    // structure
    void planIsPure();
    void applyReturnsInvalidation();
    void rejectedPlanIsNotApplied();
};

void SessionMergeTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();
}

// ─────────────────────────────── attributes

void SessionMergeTest::absentAttributeIsAdded()
{
    const SessionData existing = baseSession();
    SessionData incoming = baseSession();
    incoming.setAttribute("CUSTOM_KEY", QStringLiteral("a,b"));

    const MergePlan plan = SessionMerge::plan(existing, incoming);
    QVERIFY(plan.ok());
    QVERIFY(!plan.isEmpty());
    QCOMPARE(plannedAttributeKeys(plan), QStringList({"CUSTOM_KEY"}));
    QCOMPARE(plan.attributesToSet.first().second.toString(), QStringLiteral("a,b"));
    QVERIFY(plan.columnsToSet.isEmpty());
    QCOMPARE(plan.changedKeys(), QSet<DependencyKey>({DependencyKey::attribute("CUSTOM_KEY")}));
}

void SessionMergeTest::equalAttributeIsNoop()
{
    const MergePlan plan = SessionMerge::plan(baseSession(), baseSession());
    QVERIFY(plan.ok());
    QVERIFY(plan.isEmpty());
    QVERIFY(plan.changedKeys().isEmpty());
}

void SessionMergeTest::differentAttributeConflicts()
{
    SessionData existing = baseSession();
    SessionData incoming = baseSession();
    incoming.setAttribute("FIRMWARE_VER", QStringLiteral("v2024.01.01"));
    incoming.setAttribute("CUSTOM_KEY", QStringLiteral("new"));
    incoming.setSourceMeasurement("IMU", "wx", {1.0}, "deg/s");

    const MergePlan plan = SessionMerge::plan(existing, incoming);
    QVERIFY(!plan.ok());
    QCOMPARE(plan.error,
             QStringLiteral("Attribute 'FIRMWARE_VER' conflicts with the existing session "
                            "(session: 'v2023.09.22', file: 'v2024.01.01'). "
                            "To replace the session, delete it and re-import its files."));

    // A rejected plan holds nothing to apply
    QVERIFY(plan.attributesToSet.isEmpty());
    QVERIFY(plan.columnsToSet.isEmpty());
}

void SessionMergeTest::absenceNeverConflicts()
{
    // The session declares it, the file does not
    {
        SessionData existing = baseSession();
        existing.setAttribute("SCHEMA_VER", QStringLiteral("2"));
        const MergePlan plan = SessionMerge::plan(existing, baseSession());
        QVERIFY(plan.ok());
        QVERIFY(plan.isEmpty());
        SessionMerge::apply(existing, plan);
        QCOMPARE(existing.storedAttribute("SCHEMA_VER").toString(), QStringLiteral("2"));
    }
    // The file declares it, the session does not
    {
        SessionData existing = baseSession();
        SessionData incoming = baseSession();
        incoming.setAttribute("SCHEMA_VER", QStringLiteral("2"));
        const MergePlan plan = SessionMerge::plan(existing, incoming);
        QVERIFY(plan.ok());
        QCOMPARE(plannedAttributeKeys(plan), QStringList({"SCHEMA_VER"}));
        SessionMerge::apply(existing, plan);
        QCOMPARE(existing.storedAttribute("SCHEMA_VER").toString(), QStringLiteral("2"));
    }
}

void SessionMergeTest::explicitSchemaMismatch()
{
    SessionData existing = baseSession();
    existing.setAttribute("SCHEMA_VER", QStringLiteral("2"));
    SessionData incoming = baseSession();
    incoming.setAttribute("SCHEMA_VER", QStringLiteral("1"));

    const MergePlan plan = SessionMerge::plan(existing, incoming);
    QVERIFY(!plan.ok());
    QCOMPARE(plan.error,
             QStringLiteral("Attribute 'SCHEMA_VER' conflicts with the existing session (session: '2', file: '1'). "
                            "To change a session's schema version, delete the session and re-import its files."));
    QVERIFY(plan.error.contains(QStringLiteral("delete the session and re-import")));
}

void SessionMergeTest::multipleConflictsSorted()
{
    SessionData incoming = baseSession();
    incoming.setAttribute("FIRMWARE_VER", QStringLiteral("v2024.01.01"));
    incoming.setAttribute("DEVICE_ID", QStringLiteral("def"));

    const MergePlan plan = SessionMerge::plan(baseSession(), incoming);
    QVERIFY(!plan.ok());
    QCOMPARE(plan.error,
             QStringLiteral("Attribute 'DEVICE_ID' conflicts with the existing session (session: 'abc', file: 'def'). "
                            "Attribute 'FIRMWARE_VER' conflicts with the existing session "
                            "(session: 'v2023.09.22', file: 'v2024.01.01'). "
                            "To replace the session, delete it and re-import its files."));
}

void SessionMergeTest::typedVersusTextEquality()
{
    QVERIFY(SessionMerge::sameAttributeValue(QVariant(2), QVariant(QStringLiteral("2"))));
    QVERIFY(SessionMerge::sameAttributeValue(QVariant(1718900000.123), QVariant(QStringLiteral("1718900000.123"))));
    QVERIFY(!SessionMerge::sameAttributeValue(QVariant(QStringLiteral("2")), QVariant(QStringLiteral("2.0"))));
    QVERIFY(!SessionMerge::sameAttributeValue(QVariant(QStringLiteral("2")), QVariant(QStringLiteral(" 2"))));
    QVERIFY(!SessionMerge::sameAttributeValue(QVariant(QStringLiteral("abc")), QVariant(QStringLiteral("ABC"))));
    QVERIFY(SessionMerge::sameAttributeValue(QVariant(), QVariant()));

    // Through plan(): a typed in-memory header value against the file's text
    SessionData existing = baseSession();
    existing.setAttribute("SCHEMA_VER", 2);
    existing.setAttribute("RECORDED_AT", 1718900000.123);

    SessionData same = baseSession();
    same.setAttribute("SCHEMA_VER", QStringLiteral("2"));
    same.setAttribute("RECORDED_AT", QStringLiteral("1718900000.123"));
    const MergePlan samePlan = SessionMerge::plan(existing, same);
    QVERIFY(samePlan.ok());
    QVERIFY(samePlan.isEmpty());

    SessionData different = baseSession();
    different.setAttribute("SCHEMA_VER", QStringLiteral("2.0"));
    const MergePlan differentPlan = SessionMerge::plan(existing, different);
    QVERIFY(!differentPlan.ok());
    QVERIFY(differentPlan.error.contains(QStringLiteral("(session: '2', file: '2.0')")));
}

void SessionMergeTest::viewerAttributesExistingWins()
{
    SessionData existing = baseSession();
    existing.setAttribute("_DESCRIPTION", QStringLiteral("mine"));
    SessionData incoming = baseSession();
    incoming.setAttribute("_DESCRIPTION", QStringLiteral("theirs"));
    incoming.setAttribute("_EXIT_TIME", QStringLiteral("5"));

    const MergePlan plan = SessionMerge::plan(existing, incoming);
    QVERIFY(plan.ok());
    QCOMPARE(plannedAttributeKeys(plan), QStringList({"_EXIT_TIME"}));

    SessionMerge::apply(existing, plan);
    QCOMPARE(existing.storedAttribute("_DESCRIPTION").toString(), QStringLiteral("mine"));
    QCOMPARE(existing.storedAttribute("_EXIT_TIME").toString(), QStringLiteral("5"));
}

void SessionMergeTest::deviceIdPlaceholder()
{
    // The session's placeholder counts as absent
    {
        SessionData existing = baseSession();
        existing.setAttribute("DEVICE_ID", QStringLiteral("n/a"));
        const MergePlan plan = SessionMerge::plan(existing, baseSession());
        QVERIFY(plan.ok());
        QCOMPARE(plannedAttributeKeys(plan), QStringList({"DEVICE_ID"}));
        QCOMPARE(plan.attributesToSet.first().second.toString(), QStringLiteral("abc"));
    }
    // An incoming placeholder is not a recorded fact
    {
        SessionData incoming = baseSession();
        incoming.setAttribute("DEVICE_ID", QStringLiteral("n/a"));
        const MergePlan plan = SessionMerge::plan(baseSession(), incoming);
        QVERIFY(plan.ok());
        QVERIFY(plan.isEmpty());

        // ... not even for a session that has no DEVICE_ID at all
        SessionData without;
        without.setAttribute("SESSION_ID", QStringLiteral("m1"));
        const MergePlan second = SessionMerge::plan(without, incoming);
        QVERIFY(second.ok());
        QCOMPARE(plannedAttributeKeys(second), QStringList({"FIRMWARE_VER"}));
    }
    // Two recorded ids that differ
    {
        SessionData incoming = baseSession();
        incoming.setAttribute("DEVICE_ID", QStringLiteral("def"));
        const MergePlan plan = SessionMerge::plan(baseSession(), incoming);
        QVERIFY(!plan.ok());
        QVERIFY(plan.error.contains(QStringLiteral("'DEVICE_ID'")));
    }
}

// ─────────────────────────────── measurements

void SessionMergeTest::columnsReplaceAddKeep()
{
    SessionData existing = baseSession();
    existing.setSourceMeasurement("IMU", "time", {1.0, 2.0}, "s");
    existing.setSourceMeasurement("IMU", "wx", {10.0, 20.0}, "rad/s");
    existing.setSourceMeasurement("IMU", "extra", {7.0, 8.0}, "furlongs");
    existing.setSourceMeasurement("GNSS", "hMSL", {100.0, 90.0, 80.0}, "m");

    SessionData incoming = baseSession();
    incoming.setSourceMeasurement("IMU", "time", {1.0, 2.0}, "s");
    incoming.setSourceMeasurement("IMU", "wx", {11.0, 21.0}, "deg/s");
    incoming.setSourceMeasurement("IMU", "wy", {-1.0, -2.0}, "deg/s");

    const MergePlan plan = SessionMerge::plan(existing, incoming);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    QCOMPARE(plannedColumns(plan), QStringList({"IMU/wx", "IMU/wy"}));
    QCOMPARE(plan.changedKeys(), QSet<DependencyKey>({DependencyKey::measurement("IMU", "wx"),
                                                      DependencyKey::measurement("IMU", "wy")}));

    SessionMerge::apply(existing, plan);
    QCOMPARE(existing.measurementKeys("IMU"), QStringList({"extra", "time", "wx", "wy"}));
    QCOMPARE(existing.sourceMeasurement("IMU", "extra"), QVector<double>({7.0, 8.0}));
    QCOMPARE(existing.sourceUnit("IMU", "extra"), QStringLiteral("furlongs"));
    QCOMPARE(existing.sourceMeasurement("IMU", "wx"), QVector<double>({11.0, 21.0}));
    QCOMPARE(existing.sourceUnit("IMU", "wx"), QStringLiteral("deg/s"));     // samples and unit move together
    QCOMPARE(existing.sourceMeasurement("IMU", "wy"), QVector<double>({-1.0, -2.0}));
    QCOMPARE(existing.sourceMeasurement("GNSS", "hMSL"), QVector<double>({100.0, 90.0, 80.0}));
}

void SessionMergeTest::unitOnlyChangeIsAChange()
{
    SessionData existing = baseSession();
    existing.setSourceMeasurement("IMU", "ax", {1.0}, "g");
    SessionData incoming = baseSession();
    incoming.setSourceMeasurement("IMU", "ax", {1.0}, "m/s^2");

    const MergePlan plan = SessionMerge::plan(existing, incoming);
    QVERIFY(plan.ok());
    QCOMPARE(plannedColumns(plan), QStringList({"IMU/ax"}));

    SessionMerge::apply(existing, plan);
    QCOMPARE(existing.sourceUnit("IMU", "ax"), QStringLiteral("m/s^2"));
    QCOMPARE(existing.sourceMeasurement("IMU", "ax"), QVector<double>({1.0}));
}

void SessionMergeTest::identicalIsEmpty()
{
    SessionData existing = baseSession();
    existing.setAttribute("_DESCRIPTION", QStringLiteral("mine"));
    existing.setSourceMeasurement("X", "time", {1.0, 2.0, 3.0}, "s");
    existing.setSourceMeasurement("X", "v", {std::numeric_limits<double>::quiet_NaN(), -0.0, 5.0}, "m");

    // A copy shares the sample buffers ...
    const SessionData copy = existing;
    const MergePlan shared = SessionMerge::plan(existing, copy);
    QVERIFY(shared.ok());
    QVERIFY(shared.isEmpty());

    // ... and separately built equal columns compare bitwise (NaN equals NaN)
    SessionData rebuilt = baseSession();
    rebuilt.setSourceMeasurement("X", "time", {1.0, 2.0, 3.0}, "s");
    rebuilt.setSourceMeasurement("X", "v", {std::numeric_limits<double>::quiet_NaN(), -0.0, 5.0}, "m");
    const MergePlan bitwise = SessionMerge::plan(existing, rebuilt);
    QVERIFY(bitwise.ok());
    QVERIFY(bitwise.isEmpty());

    // 0.0 is not -0.0
    SessionData positiveZero = baseSession();
    positiveZero.setSourceMeasurement("X", "v", {std::numeric_limits<double>::quiet_NaN(), 0.0, 5.0}, "m");
    QCOMPARE(plannedColumns(SessionMerge::plan(existing, positiveZero)), QStringList({"X/v"}));

    QVERIFY(SessionMerge::sameColumn(SourceColumn{{}, QStringLiteral("m")}, SourceColumn{{}, QStringLiteral("m")}));
    QVERIFY(!SessionMerge::sameColumn(SourceColumn{{1.0}, QStringLiteral("m")}, SourceColumn{{1.0, 2.0}, QStringLiteral("m")}));
}

void SessionMergeTest::raggedIsRejected()
{
    SessionData existing = baseSession();
    existing.setSourceMeasurement("X", "time", {1.0, 2.0, 3.0}, "s");
    existing.setSourceMeasurement("X", "v", {4.0, 5.0, 6.0}, "m");
    existing.setSourceMeasurement("X", "keep", {7.0, 8.0, 9.0}, "m");

    SessionData shorter = baseSession();
    shorter.setSourceMeasurement("X", "time", {1.0, 2.0}, "s");
    shorter.setSourceMeasurement("X", "v", {4.0, 5.0}, "m");

    const MergePlan rejected = SessionMerge::plan(existing, shorter);
    QVERIFY(!rejected.ok());
    QCOMPARE(rejected.error,
             QStringLiteral("Sensor 'X': the file has 2 rows but the session's column 'keep' has 3. "
                            "Delete the session and re-import its files."));
    QVERIFY(rejected.columnsToSet.isEmpty());

    // Replacing every column of the sensor together is fine
    shorter.setSourceMeasurement("X", "keep", {7.0, 8.0}, "m");
    const MergePlan accepted = SessionMerge::plan(existing, shorter);
    QVERIFY2(accepted.ok(), qPrintable(accepted.error));
    QCOMPARE(plannedColumns(accepted), QStringList({"X/keep", "X/time", "X/v"}));
}

void SessionMergeTest::disjointSensorsUnaffected()
{
    SessionData existing = baseSession();
    existing.setSourceMeasurement("GNSS", "time", {1.0, 2.0, 3.0}, "s");
    SessionData incoming = baseSession();
    incoming.setSourceMeasurement("IMU", "time", {1.0, 2.0, 3.0, 4.0, 5.0}, "s");

    const MergePlan plan = SessionMerge::plan(existing, incoming);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    QCOMPARE(plannedColumns(plan), QStringList({"IMU/time"}));
}

// ─────────────────────────────── structure

void SessionMergeTest::planIsPure()
{
    SessionData incoming = baseSession();
    incoming.setAttribute("SCHEMA_VER", QStringLiteral("2"));
    incoming.setSourceMeasurement("IMU", "wx", {99.0}, "deg/s");

    // A session without an engine: none is created
    {
        SessionData existing = baseSession();
        existing.setSourceMeasurement("IMU", "wx", {62.5}, "deg/s");
        const int enginesBefore = CalculationRegistry::instance().enrolledEngineCount();
        const MergePlan plan = SessionMerge::plan(existing, incoming);
        QVERIFY(plan.ok());
        QVERIFY(!plan.isEmpty());
        QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), enginesBefore);
    }

    // A warm engine: nothing runs, nothing is dropped, nothing is stored
    {
        SessionData existing = baseSession();
        existing.setSourceMeasurement("IMU", "wx", {62.5}, "deg/s");
        QVERIFY(qAbs(existing.getMeasurement("IMU", "wx").value(0) - 71.68) <= 1e-9);

        const SourceData sourceBefore = existing.sourceData();
        const QStringList keysBefore = existing.attributeKeys();
        const int runsBefore = existing.calculationEngine().totalRunCount();
        const int nodesBefore = existing.calculationEngine().cachedNodeCount();
        QVERIFY(nodesBefore > 0);

        const MergePlan plan = SessionMerge::plan(existing, incoming);
        QVERIFY(plan.ok());
        QVERIFY(!plan.isEmpty());

        QVERIFY(existing.sourceData() == sourceBefore);
        QCOMPARE(existing.attributeKeys(), keysBefore);
        QCOMPARE(existing.attributeKeys(), QStringList({"DEVICE_ID", "FIRMWARE_VER", "SESSION_ID"}));
        QCOMPARE(existing.calculationEngine().totalRunCount(), runsBefore);
        QCOMPARE(existing.calculationEngine().cachedNodeCount(), nodesBefore);

        // The incoming session is untouched as well
        QCOMPARE(incoming.attributeKeys(), QStringList({"DEVICE_ID", "FIRMWARE_VER", "SCHEMA_VER", "SESSION_ID"}));
        QCOMPARE(incoming.sourceMeasurement("IMU", "wx"), QVector<double>({99.0}));
    }
}

void SessionMergeTest::applyReturnsInvalidation()
{
    SessionData existing = baseSession();
    existing.setSourceMeasurement("IMU", "time", {3.0}, "s");
    existing.setSourceMeasurement("IMU", "wx", {62.5}, "deg/s");
    existing.setSourceMeasurement("IMU", "wy", {-125.0}, "deg/s");
    existing.setSourceMeasurement("IMU", "wz", {0.0}, "deg/s");
    existing.setSourceMeasurement("IMU", "ax", {1.0}, "g");

    // Warm: no SCHEMA_VER, so the gyro is read with the legacy correction
    QVERIFY(qAbs(existing.getMeasurement("IMU", "wx").value(0) - 71.68) <= 1e-9);
    // sqrt(62.5^2 + 125^2) x 1.14688 = 139.75424859373686 x 1.14688
    QVERIFY(qAbs(existing.getMeasurement("IMU", "wTotal").value(0) - 160.28135262718493) <= 1e-9);
    QCOMPARE(existing.getMeasurement("IMU", "ax"), QVector<double>({9.80665}));

    SessionData incoming = baseSession();
    incoming.setAttribute("SCHEMA_VER", QStringLiteral("2"));

    const MergePlan plan = SessionMerge::plan(existing, incoming);
    QVERIFY(plan.ok());
    const QSet<DependencyKey> invalidated = SessionMerge::apply(existing, plan);

    QVERIFY(invalidated.contains(DependencyKey::attribute("SCHEMA_VER")));
    QVERIFY(invalidated.contains(DependencyKey::measurement("IMU", "wx")));
    QVERIFY(invalidated.contains(DependencyKey::measurement("IMU", "wTotal")));
    QVERIFY(!invalidated.contains(DependencyKey::measurement("IMU", "ax")));

    // Effective values follow: recorded values, uncorrected
    QCOMPARE(existing.getMeasurement("IMU", "wx"), QVector<double>({62.5}));
    QVERIFY(qAbs(existing.getMeasurement("IMU", "wTotal").value(0) - 139.75424859373686) <= 1e-9);
}

void SessionMergeTest::rejectedPlanIsNotApplied()
{
    SessionData existing = baseSession();
    SessionData incoming = baseSession();
    incoming.setAttribute("FIRMWARE_VER", QStringLiteral("v2024.01.01"));

    MergePlan plan = SessionMerge::plan(existing, incoming);
    QVERIFY(!plan.ok());

    // Even a hand-made rejected plan with contents changes nothing
    plan.attributesToSet.append(qMakePair(QStringLiteral("CUSTOM_KEY"), QVariant(QStringLiteral("x"))));
    QVERIFY(SessionMerge::apply(existing, plan).isEmpty());
    QVERIFY(!existing.hasStoredAttribute("CUSTOM_KEY"));
    QCOMPARE(existing.storedAttribute("FIRMWARE_VER").toString(), QStringLiteral("v2023.09.22"));
}

FLYSIGHT_TEST_MAIN(SessionMergeTest)
#include "tst_session_merge.moc"
