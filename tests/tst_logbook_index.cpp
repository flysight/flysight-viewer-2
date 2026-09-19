// LogbookManager's index.json column cache (spec 9.4) at the storage level:
//
//  - the calculation-compatibility marker and the environment fingerprint gate
//    the cached values: missing or different -> every cached value is dropped,
//    uuid / lastAccessed are kept, session files are not touched (acceptance 18);
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

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>

#include "calculations/builtincalculations.h"
#include "engine/calculationregistry.h"
#include "fixturebuilder.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

LogbookColumn descriptionColumn()
{
    LogbookColumn col;
    col.type = ColumnType::SessionAttribute;
    col.attributeKey = QStringLiteral("_DESCRIPTION");
    return col;
}

LogbookColumn gyroColumn()
{
    LogbookColumn col;
    col.type = ColumnType::MeasurementAtMarker;
    col.sensorID = QStringLiteral("IMU");
    col.measurementID = QStringLiteral("wx");
    col.measurementType = QStringLiteral("rotation");
    col.markerAttributeKey = QStringLiteral("_M");
    return col;
}

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

QJsonObject readIndex()
{
    const QByteArray bytes = readFileBytes(TestEnvironment::instance().indexPath());
    return QJsonDocument::fromJson(bytes).object();
}

bool writeIndex(const QJsonObject &root)
{
    return writeFile(TestEnvironment::instance().indexPath(), QJsonDocument(root).toJson());
}

// The value index.json holds for (session, column): Undefined when absent.
QJsonValue indexValue(const QJsonObject &root, const QString &sessionId, const LogbookColumn &col)
{
    const QJsonObject columns = root[QStringLiteral("columns")].toObject();
    QString columnUuid;
    for (auto it = columns.constBegin(); it != columns.constEnd(); ++it) {
        const QJsonObject def = it.value().toObject();
        const bool match = col.type == ColumnType::SessionAttribute
            ? def[QStringLiteral("type")].toString() == QLatin1String("SessionAttribute")
                  && def[QStringLiteral("attributeKey")].toString() == col.attributeKey
            : def[QStringLiteral("type")].toString() == QLatin1String("MeasurementAtMarker")
                  && def[QStringLiteral("sensorID")].toString() == col.sensorID
                  && def[QStringLiteral("measurementID")].toString() == col.measurementID
                  && def[QStringLiteral("markerAttributeKey")].toString() == col.markerAttributeKey;
        if (match)
            columnUuid = it.key();
    }
    if (columnUuid.isEmpty())
        return QJsonValue(QJsonValue::Undefined);

    const QJsonObject values = root[QStringLiteral("sessions")].toObject()[sessionId].toObject()
                                   [QStringLiteral("values")].toObject();
    return values.value(columnUuid);    // Undefined when absent
}

QStringList sessionCsvFiles()
{
    return QDir(TestEnvironment::instance().sessionsDir())
        .entryList({QStringLiteral("*.csv")}, QDir::Files, QDir::Name);
}

QString sessionFilePath(const QString &sessionId)
{
    const QString uuid = readIndex()[QStringLiteral("sessions")].toObject()[sessionId].toObject()
                             [QStringLiteral("uuid")].toString();
    return TestEnvironment::instance().sessionsDir() + QLatin1Char('/') + uuid + QStringLiteral(".csv");
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
    void differentEnvironmentDiscards();
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
    CalculationRegistry::instance().unregister(QString::fromLatin1(kExtraId));
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

    const QJsonObject root = readIndex();
    QVERIFY(root[QStringLiteral("calculationCompatibility")].isDouble());
    QCOMPARE(root[QStringLiteral("calculationCompatibility")].toInt(), 1);
    QCOMPARE(CalculationCompatibilityVersion, 1);

    const QString environment = root[QStringLiteral("calculationEnvironment")].toString();
    QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{40}$")).match(environment).hasMatch());
    QCOMPARE(environment, calculationEnvironmentFingerprint());
    QCOMPARE(environment, logbook.cacheEnvironment());

    // Exactly these four root fields: no schema stamp of any kind
    QCOMPARE(root.keys(), QStringList({"calculationCompatibility", "calculationEnvironment",
                                       "columns", "sessions"}));
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
    root.remove(QStringLiteral("calculationEnvironment"));
    QVERIFY(writeIndex(root));
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
    QCOMPARE(readIndex()[QStringLiteral("calculationCompatibility")].toInt(), 1);
    QVERIFY(indexValue(readIndex(), QStringLiteral("s1"), m_g).isUndefined());
}

void LogbookIndexTest::differentMarkerDiscards_data()
{
    QTest::addColumn<QJsonValue>("marker");

    QTest::newRow("0") << QJsonValue(0);
    QTest::newRow("2") << QJsonValue(2);
    QTest::newRow("-1") << QJsonValue(-1);
    QTest::newRow("string 1") << QJsonValue(QStringLiteral("1"));
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

void LogbookIndexTest::differentEnvironmentDiscards()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();
    prepareCachedSession();

    QJsonObject root = readIndex();
    root[QStringLiteral("calculationEnvironment")] = QString(40, QLatin1Char('0'));
    QVERIFY(writeIndex(root));

    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(logbook.cachedValuesDiscardedOnLoad());
    QVERIFY(logbook.cachedValuesForSession(QStringLiteral("s1")).isEmpty());
    QCOMPARE(logbook.cacheEnvironment(), calculationEnvironmentFingerprint());
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

// flushIndex() records the environment the in-memory values were computed
// under, never a fresh fingerprint: an environment change nobody reported is
// detected at the next start.
void LogbookIndexTest::environmentIsTheCachedOne()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();
    prepareCachedSession();
    const QString oldEnvironment = logbook.cacheEnvironment();

    CalculationDescriptor extra;
    extra.id = QString::fromLatin1(kExtraId);
    extra.outputs = {DependencyKey::attribute(QStringLiteral("_TEST_LOGBOOKINDEX_EXTRA"))};
    extra.compute = [](const EvaluationContext &) { return CalculationResult(); };
    QVERIFY(CalculationRegistry::instance().registerCalculation(extra));
    const QString newEnvironment = calculationEnvironmentFingerprint();
    QVERIFY(newEnvironment != oldEnvironment);

    // Not told: the old fingerprint is written, and the next start discards.
    QVERIFY(logbook.flushIndex());
    QCOMPARE(readIndex()[QStringLiteral("calculationEnvironment")].toString(), oldEnvironment);

    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(logbook.cachedValuesDiscardedOnLoad());
    QVERIFY(logbook.cachedValuesForSession(QStringLiteral("s1")).isEmpty());
    QCOMPARE(logbook.cacheEnvironment(), newEnvironment);

    // Told: values recomputed under the new environment are kept.
    logbook.discardCachedValues();
    logbook.setCachedValues(QStringLiteral("s1"), {{m_d, QStringLiteral("x")}, {m_g, 2.5}});
    QVERIFY(logbook.flushIndex());
    QCOMPARE(readIndex()[QStringLiteral("calculationEnvironment")].toString(), newEnvironment);

    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(!logbook.cachedValuesDiscardedOnLoad());
    QCOMPARE(logbook.cachedValuesForSession(QStringLiteral("s1"))
                 .value(LogbookManager::columnDefKey(m_g)).toDouble(), 2.5);

    // discardCachedValues() with values in memory
    logbook.discardCachedValues();
    QVERIFY(logbook.cachedValuesForSession(QStringLiteral("s1")).isEmpty());
    QVERIFY(logbook.indexNeedsFlush());
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

// Spec 9.4, interrupted save: saveSession() removes the affected values from
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
    QVERIFY(logbook.cacheEnvironment().isEmpty());
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
    QVERIFY(logbook.initialize().isEmpty());
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

FLYSIGHT_TEST_MAIN(LogbookIndexTest)
#include "tst_logbook_index.moc"
