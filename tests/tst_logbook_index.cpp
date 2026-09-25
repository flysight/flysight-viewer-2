// LogbookManager's index.json column cache at the storage level:
//
//  - the calculation-compatibility marker gates every cached value, and each
//    column's environment gates the values of that column: a missing or
//    different marker drops every cached value, a missing or different column
//    environment the values of that column only (an index without column
//    environments is discarded once); uuid / lastAccessed are kept, session
//    files are not touched (acceptance 18);
//  - unsaved-column tracking and the save ordering make it impossible for
//    index.json on disk to hold a column value that disagrees with the session
//    file on disk after an interrupted save;
//  - a session file the index does not know is adopted as an identity stub;
//  - the raw load (file contents only, with the failure reason) that merges
//    use, the legacy backfill as a separate step, identity-entry queries, and
//    a legacy flat index coming up as stubs without rewriting a session file.
//
// index.json is inspected with QJsonDocument; QJsonObject orders keys
// alphabetically, so nothing here depends on field order.

#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>

#include "calculationrecord.h"
#include "calculations/builtincalculations.h"
#include "engine/calculationregistry.h"
#include "fixturebuilder.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "logbookprobe.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

// A small session that already carries the four attributes loadSession()
// would otherwise backfill.
SessionData makeSession(const QString &id, const QString &description = QStringLiteral("first"))
{
    SessionData s;
    s.setAttribute("SESSION_ID", id);
    s.setAttribute("DEVICE_ID", QStringLiteral("test-device"));
    s.setAttribute("_DESCRIPTION", description);
    s.setAttribute("_JUMPER_MASS", 80.0);
    s.setAttribute("_PLANFORM_AREA", 2.0);
    s.setAttribute("_WIND_N", 0.0);
    s.setAttribute("_WIND_E", 0.0);
    s.setSourceMeasurement("IMU", "time", {10.0, 20.0, 30.0}, "s");
    s.setSourceMeasurement("IMU", "wx", {1.0, 2.0, 3.0}, "deg/s");
    return s;
}

const char kExtraId[] = "test.logbookindex.extra";

} // namespace

class LogbookIndexTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void markerWrittenOnFlush();
    void missingMarkerDiscardsValues();
    void differentMarkerDiscards_data();
    void differentMarkerDiscards();
    void differentColumnEnvironmentDiscardsThatColumn();
    void missingColumnEnvironmentsDiscardOnce();
    void matchingMarkerKeepsValues();
    void environmentIsTheCachedOne();

    void unsavedColumnsAreNotFlushed();
    void saveFlushesIndexFirst();
    void interruptedSaveNeverDisagrees();
    void failedSaveKeepsMarks();
    void markSessionUnsavedOmitsAllValues();

    void orphanSessionFileIsAdopted();
    void remapAndRemoveCarryMarks();

    void rawLoadSkipsBackfill();
    void rawLoadReportsReason();
    void identityEntries();
    void legacyFlatIndexStartsAsStubs();

    void recordReasonsRoundTrip();

private:
    // One saved session "s1" with D = "x" and G = 1.5 cached and flushed,
    // lastAccessed 1234.
    void prepareCachedSession();
    // prepareCachedSession() + D marked unsaved, the session edited in memory
    // (description "edited") and D's new value cached. Returns the edited session.
    SessionData prepareUnsavedEdit();

    LogbookColumn m_d = descriptionColumn();
    LogbookColumn m_g = gyroColumn();
};

void LogbookIndexTest::initTestCase()
{
    // As the application does: registrations are complete before initialize(),
    // so the fingerprint captured there is the one of the next start.
    TestEnvironment::instance().registerBuiltIns();
    LogbookColumnStore::instance().setColumns({m_d, m_g});
}

void LogbookIndexTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();
}

void LogbookIndexTest::cleanup()
{
    CalculationRegistry::instance().unregister(QString::fromLatin1(kExtraId), CalculationRegistry::Removal::Change);
}

void LogbookIndexTest::prepareCachedSession()
{
    LogbookManager &logbook = LogbookManager::instance();
    QVERIFY(logbook.saveSession(makeSession(QStringLiteral("s1"))));
    logbook.setCachedValues(QStringLiteral("s1"), {{m_d, QStringLiteral("x")}, {m_g, 1.5}});
    logbook.setLastAccessed(QStringLiteral("s1"), 1234.0);
    QVERIFY(logbook.flushIndex());
    QVERIFY(!logbook.indexNeedsFlush());
}

SessionData LogbookIndexTest::prepareUnsavedEdit()
{
    prepareCachedSession();
    LogbookManager &logbook = LogbookManager::instance();

    SessionData edited = makeSession(QStringLiteral("s1"), QStringLiteral("edited"));
    logbook.markColumnsUnsaved(QStringLiteral("s1"), {m_d});
    logbook.updateCachedValues(QStringLiteral("s1"), {{m_d, QStringLiteral("edited")}});
    return edited;
}

void LogbookIndexTest::markerWrittenOnFlush()
{
    LogbookManager &logbook = LogbookManager::instance();
    QVERIFY(!logbook.cachedValuesDiscardedOnLoad());
    QVERIFY(logbook.saveSession(makeSession(QStringLiteral("s1"))));
    QVERIFY(logbook.indexNeedsFlush());
    QVERIFY(logbook.flushIndex());

    // No value cached yet: no column has an environment to record
    QVERIFY(indexColumnEnvironment(readIndex(), m_d).isEmpty());
    QVERIFY(logbook.columnEnvironment(m_d).isEmpty());

    // Storing a value records the column's current environment
    logbook.setCachedValues(QStringLiteral("s1"), {{m_d, QStringLiteral("x")}, {m_g, 1.5}});
    QVERIFY(logbook.flushIndex());

    const QJsonObject root = readIndex();
    QVERIFY(root[QStringLiteral("calculationCompatibility")].isDouble());
    QCOMPARE(root[QStringLiteral("calculationCompatibility")].toInt(), 2);
    QCOMPARE(CalculationCompatibilityVersion, 2);

    for (const LogbookColumn &col : {m_d, m_g}) {
        const QString environment = indexColumnEnvironment(root, col);
        QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{40}$")).match(environment).hasMatch());
        QCOMPARE(environment, logbookColumnEnvironment(col, CalculationRegistry::instance()));
        QCOMPARE(environment, logbook.columnEnvironment(col));
    }
    // The closures differ, and so do the environments
    QVERIFY(indexColumnEnvironment(root, m_d) != indexColumnEnvironment(root, m_g));

    // Exactly these three root fields: no schema stamp of any kind, and no
    // environment of the whole registry
    QCOMPARE(root.keys(), QStringList({"calculationCompatibility", "columns", "sessions"}));
}

// Acceptance 18: an index.json without the compatibility marker (as every
// released version wrote it) has its cached column values discarded; nothing
// else is lost and no session file is touched.
void LogbookIndexTest::missingMarkerDiscardsValues()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();
    prepareCachedSession();

    const QString csvPath = sessionFilePath(QStringLiteral("s1"));
    const QByteArray csvBytes = readFileBytes(csvPath);
    QVERIFY(!csvBytes.isEmpty());

    QJsonObject root = readIndex();
    QCOMPARE(indexValue(root, QStringLiteral("s1"), m_g).toDouble(), 1.5);
    root.remove(QStringLiteral("calculationCompatibility"));
    QVERIFY(writeIndex(root));      // the column environments still match: the marker alone discards
    const QByteArray indexBytes = readFileBytes(env.indexPath());

    env.reopenLogbook();
    logbook.initialize();

    QVERIFY(logbook.cachedValuesDiscardedOnLoad());
    QVERIFY(logbook.cachedValuesForSession(QStringLiteral("s1")).isEmpty());
    QVERIFY(logbook.indexNeedsFlush());
    QCOMPARE(logbook.lastAccessedMap().value(QStringLiteral("s1")), 1234.0);
    QVERIFY(logbook.cachedColumnValues({m_d, m_g}).contains(QStringLiteral("s1")));
    QVERIFY(logbook.cachedColumnValues({m_d, m_g}).value(QStringLiteral("s1")).isEmpty());

    // initialize() wrote nothing and the session file is as it was
    QCOMPARE(readFileBytes(env.indexPath()), indexBytes);
    QCOMPARE(readFileBytes(csvPath), csvBytes);

    // The uuid was kept: the session still loads
    const std::optional<SessionData> loaded = logbook.loadSession(QStringLiteral("s1"));
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->storedAttribute("_DESCRIPTION").toString(), QStringLiteral("first"));
    QCOMPARE(readFileBytes(csvPath), csvBytes);

    // The rewritten index carries the current marker
    QVERIFY(logbook.flushIndex());
    QCOMPARE(readIndex()[QStringLiteral("calculationCompatibility")].toInt(), 2);
    QVERIFY(indexValue(readIndex(), QStringLiteral("s1"), m_g).isUndefined());
}

void LogbookIndexTest::differentMarkerDiscards_data()
{
    QTest::addColumn<QJsonValue>("marker");

    QTest::newRow("0") << QJsonValue(0);
    QTest::newRow("1") << QJsonValue(1);
    QTest::newRow("3") << QJsonValue(3);
    QTest::newRow("-1") << QJsonValue(-1);
    QTest::newRow("string 2") << QJsonValue(QStringLiteral("2"));
}

void LogbookIndexTest::differentMarkerDiscards()
{
    QFETCH(QJsonValue, marker);

    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();
    prepareCachedSession();

    QJsonObject root = readIndex();
    root[QStringLiteral("calculationCompatibility")] = marker;
    QVERIFY(writeIndex(root));

    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(logbook.cachedValuesDiscardedOnLoad());
    QVERIFY(logbook.cachedValuesForSession(QStringLiteral("s1")).isEmpty());
    QCOMPARE(logbook.lastAccessedMap().value(QStringLiteral("s1")), 1234.0);
}

// A column whose recorded environment differs from its current one loses its
// values; every other column keeps its own.
void LogbookIndexTest::differentColumnEnvironmentDiscardsThatColumn()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();
    prepareCachedSession();
    const QString gyroEnvironment = logbook.columnEnvironment(m_g);

    QJsonObject root = readIndex();
    QVERIFY(setIndexColumnEnvironment(root, m_d, QString(40, QLatin1Char('0'))));
    QVERIFY(writeIndex(root));

    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(logbook.cachedValuesDiscardedOnLoad());
    QVERIFY(logbook.indexNeedsFlush());
    const QMap<QString, QJsonValue> &values = logbook.cachedValuesForSession(QStringLiteral("s1"));
    QCOMPARE(values.keys(), QStringList({LogbookManager::columnDefKey(m_g)}));
    QCOMPARE(values.value(LogbookManager::columnDefKey(m_g)).toDouble(), 1.5);
    QCOMPARE(logbook.columnEnvironment(m_d), logbookColumnEnvironment(m_d, CalculationRegistry::instance()));
    QCOMPARE(logbook.columnEnvironment(m_g), gyroEnvironment);

    // The rewritten index records the current environment of both
    QVERIFY(logbook.flushIndex());
    QCOMPARE(indexColumnEnvironment(readIndex(), m_d), logbook.columnEnvironment(m_d));
    QVERIFY(indexValue(readIndex(), QStringLiteral("s1"), m_d).isUndefined());
    QCOMPARE(indexValue(readIndex(), QStringLiteral("s1"), m_g).toDouble(), 1.5);
}

// An index written before column environments existed (no "environment" on
// any column, a root "calculationEnvironment" instead) is all-mismatched: its
// values are discarded once, and the index rewritten with the environments
// is valid at the next start.
void LogbookIndexTest::missingColumnEnvironmentsDiscardOnce()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();
    prepareCachedSession();

    QJsonObject root = readIndex();
    removeColumnEnvironments(root);
    root[QStringLiteral("calculationEnvironment")] = QString(40, QLatin1Char('a'));
    QVERIFY(writeIndex(root));

    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(logbook.cachedValuesDiscardedOnLoad());
    QVERIFY(logbook.cachedValuesForSession(QStringLiteral("s1")).isEmpty());
    QCOMPARE(logbook.lastAccessedMap().value(QStringLiteral("s1")), 1234.0);

    // Recomputed values are written with the environments ...
    logbook.setCachedValues(QStringLiteral("s1"), {{m_d, QStringLiteral("x")}, {m_g, 2.5}});
    QVERIFY(logbook.flushIndex());
    QVERIFY(!readIndex().contains(QStringLiteral("calculationEnvironment")));

    // ... and kept from then on
    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(!logbook.cachedValuesDiscardedOnLoad());
    QCOMPARE(logbook.cachedValuesForSession(QStringLiteral("s1"))
                 .value(LogbookManager::columnDefKey(m_g)).toDouble(), 2.5);
}

// The control: gating, not unconditional recomputation.
void LogbookIndexTest::matchingMarkerKeepsValues()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();
    prepareCachedSession();

    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(!logbook.cachedValuesDiscardedOnLoad());
    QVERIFY(!logbook.indexNeedsFlush());

    const QMap<QString, QJsonValue> &values = logbook.cachedValuesForSession(QStringLiteral("s1"));
    QCOMPARE(values.value(LogbookManager::columnDefKey(m_g)).toDouble(), 1.5);
    QCOMPARE(values.value(LogbookManager::columnDefKey(m_d)).toString(), QStringLiteral("x"));

    const QMap<int, QVariant> byIndex = logbook.cachedColumnValues({m_d, m_g}).value(QStringLiteral("s1"));
    QCOMPARE(byIndex.value(0), QVariant(QStringLiteral("x")));
    QCOMPARE(byIndex.value(1), QVariant(1.5));
}

// flushIndex() records, per column, the environment the in-memory values
// were computed under, never a fresh digest: an environment change nobody
// reported is detected at the next start - for the columns it reaches only.
// Here a second candidate for _DESCRIPTION reaches the description column
// and not the gyro column.
void LogbookIndexTest::environmentIsTheCachedOne()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();
    const CalculationRegistry &registry = CalculationRegistry::instance();
    prepareCachedSession();
    const QString oldDescription = logbook.columnEnvironment(m_d);
    const QString gyro = logbook.columnEnvironment(m_g);

    CalculationDescriptor extra;
    extra.id = QString::fromLatin1(kExtraId);
    extra.outputs = {DependencyKey::attribute(QStringLiteral("_DESCRIPTION"))};
    extra.compute = [](const EvaluationContext &) { return CalculationResult(); };
    QVERIFY(CalculationRegistry::instance().registerCalculation(extra));
    const QString newDescription = logbookColumnEnvironment(m_d, registry);
    QVERIFY(newDescription != oldDescription);
    QCOMPARE(logbookColumnEnvironment(m_g, registry), gyro);

    // Not told: the old environment is written, and the next start discards
    // the description; the gyro value is kept.
    QVERIFY(logbook.flushIndex());
    QCOMPARE(indexColumnEnvironment(readIndex(), m_d), oldDescription);
    QCOMPARE(indexColumnEnvironment(readIndex(), m_g), gyro);

    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(logbook.cachedValuesDiscardedOnLoad());
    QCOMPARE(logbook.cachedValuesForSession(QStringLiteral("s1")).keys(),
             QStringList({LogbookManager::columnDefKey(m_g)}));
    QCOMPARE(logbook.columnEnvironment(m_d), newDescription);

    // Told: values recomputed under the new environment are kept.
    QCOMPARE(logbook.checkColumnEnvironments({m_d, m_g}), QStringList());     // already current
    logbook.updateCachedValues(QStringLiteral("s1"), {{m_d, QStringLiteral("y")}});
    QVERIFY(logbook.flushIndex());
    QCOMPARE(indexColumnEnvironment(readIndex(), m_d), newDescription);

    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(!logbook.cachedValuesDiscardedOnLoad());
    QCOMPARE(logbook.cachedValuesForSession(QStringLiteral("s1"))
                 .value(LogbookManager::columnDefKey(m_d)).toString(), QStringLiteral("y"));
    QCOMPARE(logbook.cachedValuesForSession(QStringLiteral("s1"))
                 .value(LogbookManager::columnDefKey(m_g)).toDouble(), 1.5);

    // checkColumnEnvironments() with values in memory: the removal changes the
    // description's environment again, and drops exactly its values
    QVERIFY(CalculationRegistry::instance().unregister(extra.id, CalculationRegistry::Removal::Change));
    QVERIFY(!logbook.indexNeedsFlush());
    QCOMPARE(logbook.checkColumnEnvironments({m_d, m_g}), QStringList({LogbookManager::columnDefKey(m_d)}));
    QCOMPARE(logbook.cachedValuesForSession(QStringLiteral("s1")).keys(),
             QStringList({LogbookManager::columnDefKey(m_g)}));
    QCOMPARE(logbook.columnEnvironment(m_d), oldDescription);
    QVERIFY(logbook.indexNeedsFlush());
    QCOMPARE(logbook.checkColumnEnvironments({m_d, m_g}), QStringList());
}

void LogbookIndexTest::unsavedColumnsAreNotFlushed()
{
    LogbookManager &logbook = LogbookManager::instance();
    prepareUnsavedEdit();
    QVERIFY(logbook.hasUnsavedColumns(QStringLiteral("s1")));
    QVERIFY(logbook.indexNeedsFlush());

    // What a ColumnTask completion would do while the row is still dirty
    QVERIFY(logbook.flushIndex());

    const QJsonObject root = readIndex();
    QCOMPARE(indexValue(root, QStringLiteral("s1"), m_g).toDouble(), 1.5);
    QVERIFY(indexValue(root, QStringLiteral("s1"), m_d).isUndefined());

    // Memory always reflects the in-memory session
    QCOMPARE(logbook.cachedValuesForSession(QStringLiteral("s1"))
                 .value(LogbookManager::columnDefKey(m_d)).toString(), QStringLiteral("edited"));
    QVERIFY(logbook.hasUnsavedColumns(QStringLiteral("s1")));
}

// Interrupted save: saveSession() removes the affected values from
// the on-disk index BEFORE it writes the session file.
void LogbookIndexTest::saveFlushesIndexFirst()
{
    LogbookManager &logbook = LogbookManager::instance();
    const SessionData edited = prepareUnsavedEdit();

    // Before the save the on-disk index still holds the old D
    QCOMPARE(indexValue(readIndex(), QStringLiteral("s1"), m_d).toString(), QStringLiteral("x"));

    QVERIFY(logbook.saveSession(edited));
    // ... and no further flush: a crash right after the session file was written.

    const QJsonObject root = readIndex();
    QVERIFY(indexValue(root, QStringLiteral("s1"), m_d).isUndefined());
    QCOMPARE(indexValue(root, QStringLiteral("s1"), m_g).toDouble(), 1.5);
    QVERIFY(readFileBytes(sessionFilePath(QStringLiteral("s1"))).contains("$VAR,_DESCRIPTION,edited\n"));

    QVERIFY(!logbook.hasUnsavedColumns(QStringLiteral("s1")));
    QVERIFY(logbook.indexNeedsFlush());

    // The normal completion flush publishes the new value
    QVERIFY(logbook.flushIndex());
    QCOMPARE(indexValue(readIndex(), QStringLiteral("s1"), m_d).toString(), QStringLiteral("edited"));
}

void LogbookIndexTest::interruptedSaveNeverDisagrees()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();
    const SessionData edited = prepareUnsavedEdit();
    QVERIFY(logbook.saveSession(edited));
    // crash: no flush

    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(!logbook.cachedValuesDiscardedOnLoad());

    // G survived; for D there is no value anywhere that could differ from the CSV
    const QMap<QString, QJsonValue> &values = logbook.cachedValuesForSession(QStringLiteral("s1"));
    QCOMPARE(values.keys(), QStringList({LogbookManager::columnDefKey(m_g)}));
    QCOMPARE(values.value(LogbookManager::columnDefKey(m_g)).toDouble(), 1.5);

    const std::optional<SessionData> loaded = logbook.loadSession(QStringLiteral("s1"));
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->storedAttribute("_DESCRIPTION").toString(), QStringLiteral("edited"));
}

void LogbookIndexTest::failedSaveKeepsMarks()
{
    LogbookManager &logbook = LogbookManager::instance();
    prepareCachedSession();
    const QString csvPath = sessionFilePath(QStringLiteral("s1"));
    const QByteArray csvBytes = readFileBytes(csvPath);

    SessionData ragged = makeSession(QStringLiteral("s1"), QStringLiteral("never saved"));
    ragged.setSourceMeasurement("IMU", "wx", {1.0, 2.0}, "deg/s");
    logbook.markSessionUnsaved(QStringLiteral("s1"));
    logbook.updateCachedValues(QStringLiteral("s1"), {{m_d, QStringLiteral("never saved")}, {m_g, 9.0}});

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("not saved: Sensor 'IMU'")));
    QVERIFY(!logbook.saveSession(ragged));
    QCOMPARE(logbook.lastSaveError(),
             QStringLiteral("Sensor 'IMU' has columns of unequal length (time: 3, wx: 2)"));
    QVERIFY(logbook.hasUnsavedColumns(QStringLiteral("s1")));
    QCOMPARE(readFileBytes(csvPath), csvBytes);

    // The pre-save flush already removed the values; a later flush keeps them out
    QVERIFY(logbook.flushIndex());
    const QJsonObject entry = readIndex()[QStringLiteral("sessions")].toObject()[QStringLiteral("s1")].toObject();
    QVERIFY(entry.contains(QStringLiteral("values")));
    QVERIFY(entry[QStringLiteral("values")].toObject().isEmpty());
    QCOMPARE(entry[QStringLiteral("lastAccessed")].toDouble(), 1234.0);

    // A successful save clears the error and the marks
    QVERIFY(logbook.saveSession(makeSession(QStringLiteral("s1"), QStringLiteral("saved"))));
    QVERIFY(logbook.lastSaveError().isEmpty());
    QVERIFY(!logbook.hasUnsavedColumns(QStringLiteral("s1")));
}

void LogbookIndexTest::markSessionUnsavedOmitsAllValues()
{
    LogbookManager &logbook = LogbookManager::instance();
    prepareCachedSession();
    QVERIFY(logbook.saveSession(makeSession(QStringLiteral("s2"))));
    logbook.setCachedValues(QStringLiteral("s2"), {{m_d, QStringLiteral("y")}, {m_g, 2.5}});
    QVERIFY(logbook.flushIndex());

    logbook.markSessionUnsaved(QStringLiteral("s1"));
    QVERIFY(logbook.cachedValuesForSession(QStringLiteral("s1")).isEmpty());
    logbook.updateCachedValues(QStringLiteral("s1"), {{m_g, 7.0}});
    QVERIFY(logbook.flushIndex());

    // Only the marked session is affected
    const QJsonObject root = readIndex();
    QVERIFY(indexValue(root, QStringLiteral("s1"), m_g).isUndefined());
    QVERIFY(indexValue(root, QStringLiteral("s1"), m_d).isUndefined());
    QCOMPARE(indexValue(root, QStringLiteral("s2"), m_g).toDouble(), 2.5);
    QCOMPARE(indexValue(root, QStringLiteral("s2"), m_d).toString(), QStringLiteral("y"));
}

// A new session whose CSV was committed but whose index entry was not (the
// process died before the flush) is found again at the next start.
void LogbookIndexTest::orphanSessionFileIsAdopted()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();

    QVERIFY(logbook.saveSession(makeSession(QStringLiteral("a"))));
    QVERIFY(logbook.saveSession(makeSession(QStringLiteral("b"))));
    QVERIFY(logbook.flushIndex());
    const QStringList indexedFiles = sessionCsvFiles();
    QCOMPARE(indexedFiles.size(), 2);

    QVERIFY(logbook.saveSession(makeSession(QStringLiteral("c"))));   // never indexed
    QStringList orphanFiles = sessionCsvFiles();
    QCOMPARE(orphanFiles.size(), 3);
    for (const QString &file : indexedFiles)
        orphanFiles.removeOne(file);
    QCOMPARE(orphanFiles.size(), 1);
    const QString stem = QFileInfo(orphanFiles.first()).completeBaseName();

    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(logbook.hasIndexData());
    QVERIFY(!logbook.cachedValuesDiscardedOnLoad());
    QVERIFY(logbook.indexNeedsFlush());

    const QMap<QString, QMap<int, QVariant>> rows = logbook.cachedColumnValues({m_d, m_g});
    QCOMPARE(rows.keys().size(), 3);
    QVERIFY(rows.contains(QStringLiteral("a")));
    QVERIFY(rows.contains(QStringLiteral("b")));
    QVERIFY(rows.contains(stem));

    const std::optional<SessionData> loaded = logbook.loadSession(stem);
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->storedAttribute("SESSION_ID").toString(), QStringLiteral("c"));

    // What the model does when it first loads an identity stub
    QVERIFY(logbook.remapSessionId(stem, QStringLiteral("c")));
    QVERIFY(logbook.flushIndex());

    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(!logbook.indexNeedsFlush());
    QCOMPARE(logbook.cachedColumnValues({m_d, m_g}).keys(), QStringList({"a", "b", "c"}));
    QCOMPARE(sessionCsvFiles().size(), 3);
}

void LogbookIndexTest::remapAndRemoveCarryMarks()
{
    LogbookManager &logbook = LogbookManager::instance();
    prepareCachedSession();

    logbook.markColumnsUnsaved(QStringLiteral("s1"), {m_d});
    QVERIFY(logbook.hasUnsavedColumns(QStringLiteral("s1")));
    QVERIFY(!logbook.hasUnsavedColumns(QStringLiteral("renamed")));

    QVERIFY(logbook.remapSessionId(QStringLiteral("s1"), QStringLiteral("renamed")));
    QVERIFY(!logbook.hasUnsavedColumns(QStringLiteral("s1")));
    QVERIFY(logbook.hasUnsavedColumns(QStringLiteral("renamed")));

    // The mark still does its job under the new id
    logbook.updateCachedValues(QStringLiteral("renamed"), {{m_d, QStringLiteral("new")}});
    QVERIFY(logbook.flushIndex());
    QVERIFY(indexValue(readIndex(), QStringLiteral("renamed"), m_d).isUndefined());
    QCOMPARE(indexValue(readIndex(), QStringLiteral("renamed"), m_g).toDouble(), 1.5);

    logbook.markSessionUnsaved(QStringLiteral("renamed"));
    QVERIFY(logbook.remapSessionId(QStringLiteral("renamed"), QStringLiteral("again")));
    QVERIFY(logbook.hasUnsavedColumns(QStringLiteral("again")));

    QVERIFY(logbook.removeSession(QStringLiteral("again")));
    QVERIFY(!logbook.hasUnsavedColumns(QStringLiteral("again")));

    // reset() clears everything, marks included
    logbook.markSessionUnsaved(QStringLiteral("ghost"));
    QVERIFY(logbook.hasUnsavedColumns(QStringLiteral("ghost")));
    TestEnvironment::instance().reopenLogbook();
    QVERIFY(!logbook.hasUnsavedColumns(QStringLiteral("ghost")));
    QVERIFY(!logbook.indexNeedsFlush());
    QVERIFY(!logbook.cachedValuesDiscardedOnLoad());
    QVERIFY(logbook.columnEnvironment(m_d).isEmpty());
    QVERIFY(logbook.lastSaveError().isEmpty());
}

// A session file written before the mass / area / wind attributes existed.
void LogbookIndexTest::rawLoadSkipsBackfill()
{
    LogbookManager &logbook = LogbookManager::instance();

    SessionData old;
    old.setAttribute("SESSION_ID", QStringLiteral("old"));
    old.setAttribute("DEVICE_ID", QStringLiteral("test-device"));
    old.setAttribute("_DESCRIPTION", QStringLiteral("released"));
    old.setSourceMeasurement("IMU", "time", {10.0}, "s");
    QVERIFY(logbook.saveSession(old));

    QString error = QStringLiteral("stale");
    const std::optional<SessionData> raw = logbook.loadSessionRaw(QStringLiteral("old"), &error);
    QVERIFY(raw.has_value());
    QVERIFY(error.isEmpty());
    QCOMPARE(raw->attributeKeys(), QStringList({"DEVICE_ID", "SESSION_ID", "_DESCRIPTION"}));

    const std::optional<SessionData> ordinary = logbook.loadSession(QStringLiteral("old"));
    QVERIFY(ordinary.has_value());
    QCOMPARE(ordinary->attributeKeys(),
             QStringList({"DEVICE_ID", "SESSION_ID", "_DESCRIPTION",
                          "_JUMPER_MASS", "_PLANFORM_AREA", "_WIND_E", "_WIND_N"}));
    QCOMPARE(ordinary->storedAttribute("_JUMPER_MASS").toDouble(), 1.0);
    QCOMPARE(ordinary->storedAttribute("_WIND_N").toDouble(), 0.0);

    // The shim only fills what is absent
    SessionData partial;
    partial.setAttribute("_JUMPER_MASS", QStringLiteral("80"));
    LogbookManager::applyLegacyBackfill(partial);
    QCOMPARE(partial.storedAttribute("_JUMPER_MASS").toString(), QStringLiteral("80"));
    QCOMPARE(partial.attributeKeys(),
             QStringList({"_JUMPER_MASS", "_PLANFORM_AREA", "_WIND_E", "_WIND_N"}));
}

void LogbookIndexTest::rawLoadReportsReason()
{
    LogbookManager &logbook = LogbookManager::instance();
    prepareCachedSession();

    QString error;
    QVERIFY(!logbook.loadSessionRaw(QStringLiteral("nobody"), &error).has_value());
    QCOMPARE(error, QStringLiteral("not in the logbook index"));

    QVERIFY(writeFile(sessionFilePath(QStringLiteral("s1")), "garbage"));
    QVERIFY(!logbook.loadSessionRaw(QStringLiteral("s1"), &error).has_value());
    QCOMPARE(error, QStringLiteral("Unknown file format"));

    QVERIFY(QFile::remove(sessionFilePath(QStringLiteral("s1"))));
    QVERIFY(!logbook.loadSessionRaw(QStringLiteral("s1"), &error).has_value());
    QCOMPARE(error, QStringLiteral("Couldn't read file"));

    // The error pointer is optional
    QVERIFY(!logbook.loadSessionRaw(QStringLiteral("s1")).has_value());
    QVERIFY(!logbook.loadSession(QStringLiteral("s1")).has_value());
}

void LogbookIndexTest::identityEntries()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();

    // The setup of orphanSessionFileIsAdopted: "a" is indexed, "c" is not
    QVERIFY(logbook.saveSession(makeSession(QStringLiteral("a"))));
    QVERIFY(logbook.flushIndex());
    const QStringList indexedFiles = sessionCsvFiles();
    QVERIFY(logbook.saveSession(makeSession(QStringLiteral("c"))));
    QStringList orphanFiles = sessionCsvFiles();
    for (const QString &file : indexedFiles)
        orphanFiles.removeOne(file);
    QCOMPARE(orphanFiles.size(), 1);
    const QString stem = QFileInfo(orphanFiles.first()).completeBaseName();

    env.reopenLogbook();
    logbook.initialize();

    QVERIFY(logbook.isIdentityEntry(stem));
    QVERIFY(!logbook.isIdentityEntry(QStringLiteral("a")));
    QVERIFY(!logbook.isIdentityEntry(QStringLiteral("nobody")));

    QCOMPARE(logbook.peekSessionId(stem).value_or(QStringLiteral("<none>")), QStringLiteral("c"));
    QCOMPARE(logbook.peekSessionId(QStringLiteral("a")).value_or(QStringLiteral("<none>")), QStringLiteral("a"));
    QVERIFY(!logbook.peekSessionId(QStringLiteral("nobody")).has_value());

    // The model resolves the stub without loading the file
    SessionModel model;
    model.populateFromIndex(logbook.cachedColumnValues({m_d, m_g}), logbook.lastAccessedMap());
    QCOMPARE(model.rowCount(), 2);
    QVERIFY(model.getSessionRow(stem) >= 0);
    model.resolveIdentityStubs();
    QCOMPARE(model.getSessionRow(stem), -1);
    const int row = model.getSessionRow(QStringLiteral("c"));
    QVERIFY(row >= 0);
    QVERIFY(!model.rowAt(row).isLoaded());
    QVERIFY(!logbook.isIdentityEntry(stem));
    QVERIFY(logbook.loadSession(QStringLiteral("c")).has_value());
}

// A logbook written by a release that kept a flat index ({"<id>": {"uuid": ...}}):
// it comes up as stubs like any other; no session file is rewritten.
void LogbookIndexTest::legacyFlatIndexStartsAsStubs()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();

    QVERIFY(logbook.saveSession(makeSession(QStringLiteral("s1"))));
    QVERIFY(logbook.flushIndex());
    const QString csvPath = sessionFilePath(QStringLiteral("s1"));
    const QString uuid = QFileInfo(csvPath).completeBaseName();
    const QByteArray csvBefore = readFileBytes(csvPath);
    QVERIFY(!csvBefore.isEmpty());

    QJsonObject entry;
    entry[QStringLiteral("uuid")] = uuid;
    QJsonObject flat;
    flat[QStringLiteral("s1")] = entry;
    QVERIFY(writeIndex(flat));

    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(!logbook.hasIndexData());
    QVERIFY(!logbook.hasDeferredScan());

    // What MainWindow does at startup
    SessionModel model;
    model.populateFromIndex(logbook.cachedColumnValues({m_d, m_g}), logbook.lastAccessedMap());
    model.startColumnWorker();
    QCOMPARE(model.rowCount(), 1);
    QVERIFY(!model.rowAt(0).isLoaded());
    QVERIFY(model.rowAt(0).cachedValues.isEmpty());

    QVERIFY(waitForIdle(model));

    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.rowAt(0).sessionId, QStringLiteral("s1"));
    QVERIFY(!model.rowAt(0).dirty);
    QVERIFY(!model.rowAt(0).isLoaded());
    QCOMPARE(model.rowAt(0).cachedValues.value(0).toString(), QStringLiteral("first"));
    QCOMPARE(readFileBytes(csvPath), csvBefore);
    QCOMPARE(sessionCsvFiles().size(), 1);

    // The index was rewritten in the extended format
    const QJsonObject root = readIndex();
    QVERIFY(root.contains(QStringLiteral("columns")));
    QCOMPARE(root[QStringLiteral("calculationCompatibility")].toInt(), CalculationCompatibilityVersion);
    QCOMPARE(indexValue(root, QStringLiteral("s1"), m_d).toString(), QStringLiteral("first"));
}

// ---- Record outcomes ("recordReasons") ------------------------------------------------

// The reason a stored result did not produce its outputs lives in the index
// beside the record stamp: learned, flushed, read back while its record file
// exists, forgotten with the record, moved with the id.
void LogbookIndexTest::recordReasonsRoundTrip()
{
    LogbookManager &logbook = LogbookManager::instance();
    const QString g1 = QStringLiteral("g1");
    const QString g2 = QStringLiteral("g2");
    const QString x = QStringLiteral("x");
    QVERIFY(logbook.saveSession(makeSession(g1)));
    QVERIFY(logbook.saveSession(makeSession(g2)));
    QVERIFY(logbook.flushIndex());
    QVERIFY(!logbook.indexNeedsFlush());
    QCOMPARE(logbook.calculationRecordReason(g1, x), QString());

    // Learned: marks the index for a flush; the same value again does not
    logbook.setCalculationRecordReason(g1, x, QStringLiteral("no"));
    QVERIFY(logbook.indexNeedsFlush());
    QCOMPARE(logbook.calculationRecordReason(g1, x), QStringLiteral("no"));
    QVERIFY(logbook.flushIndex());
    logbook.setCalculationRecordReason(g1, x, QStringLiteral("no"));
    QVERIFY(!logbook.indexNeedsFlush());

    // Flushed beside "records", for the entry that has one only
    QJsonObject root = readIndex();
    const QJsonObject sessions = root[QStringLiteral("sessions")].toObject();
    QCOMPARE(sessions[g1].toObject()[QStringLiteral("recordReasons")].toObject(),
             QJsonObject({{x, QStringLiteral("no")}}));
    QVERIFY(!sessions[g2].toObject().contains(QStringLiteral("recordReasons")));
    QVERIFY(sessions[g1].toObject()[QStringLiteral("records")].isObject());

    // Read back only while cache/ holds the record: without it, forgotten
    logbook.reset();
    logbook.initialize();
    QCOMPARE(logbook.calculationRecordReason(g1, x), QString());

    // A record written with a reason: the write teaches it, the restart reads it back
    StoredCalculationResult result;
    result.calculationId = x;
    result.inputFingerprint = QCryptographicHash::hash("inputs", QCryptographicHash::Sha256);
    result.bundle.setReason(QStringLiteral("no"));
    result.detail = QStringLiteral("no");
    QString error;
    QVERIFY2(logbook.writeCalculationRecord(g1, CalculationRecord::stamped(result), &error), qPrintable(error));
    QCOMPARE(logbook.calculationRecordReason(g1, x), QStringLiteral("no"));
    QVERIFY(logbook.flushIndex());
    logbook.reset();
    logbook.initialize();
    QVERIFY(logbook.knownCalculationRecords(g1).contains(x));
    QCOMPARE(logbook.calculationRecordReason(g1, x), QStringLiteral("no"));

    // A replacement without a reason replaces it
    result.bundle.setReason(QString());
    result.detail.clear();
    QVERIFY(logbook.writeCalculationRecord(g1, CalculationRecord::stamped(result)));
    QCOMPARE(logbook.calculationRecordReason(g1, x), QString());
    result.bundle.setReason(QStringLiteral("no"));
    result.detail = QStringLiteral("no");
    QVERIFY(logbook.writeCalculationRecord(g1, CalculationRecord::stamped(result)));
    QCOMPARE(logbook.calculationRecordReason(g1, x), QStringLiteral("no"));

    // Removed with the record
    QVERIFY(logbook.removeCalculationRecord(g1, x));
    QCOMPARE(logbook.calculationRecordReason(g1, x), QString());
    QVERIFY(logbook.flushIndex());
    QVERIFY(!readIndex()[QStringLiteral("sessions")].toObject()[g1].toObject().contains(QStringLiteral("recordReasons")));

    // Moved with the id
    logbook.setCalculationRecordReason(g2, x, QStringLiteral("moved"));
    QVERIFY(logbook.remapSessionId(g2, QStringLiteral("g3")));
    QCOMPARE(logbook.calculationRecordReason(g2, x), QString());
    QCOMPARE(logbook.calculationRecordReason(QStringLiteral("g3"), x), QStringLiteral("moved"));

    // An empty reason clears it
    QVERIFY(logbook.flushIndex());
    logbook.setCalculationRecordReason(QStringLiteral("g3"), x, QString());
    QVERIFY(logbook.indexNeedsFlush());
    QCOMPARE(logbook.calculationRecordReason(QStringLiteral("g3"), x), QString());
}

FLYSIGHT_TEST_MAIN(LogbookIndexTest)
#include "tst_logbook_index.moc"
