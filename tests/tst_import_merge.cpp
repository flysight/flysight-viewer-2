// Import and merge rules (spec 6.2 - 6.5) through the application's import
// path: SessionImport::importFiles -> SessionModel::mergeSessions, against a
// temporary logbook.
//
//  - acceptance 3: a file with an unsupported SCHEMA_VER is rejected and the
//    existing session - loaded or not - is left exactly as it was;
//  - acceptance 7: TRACK-then-SENSOR and SENSOR-then-TRACK give the same
//    session, loaded or unloaded or in one batch; a conflicting header
//    attribute fails the file and changes nothing; edits and measurements the
//    file does not mention survive;
//  - acceptance 8: re-importing a file with `$VAR,SCHEMA_VER,2` added updates
//    the same session and the effective gyro values follow;
//  - acceptance 10 (merge part): a merge that makes a better candidate viable
//    switches the calculated value;
//  - acceptance 18 (merge part): a merge refreshes only the logbook columns it
//    can affect.
//
// Files live in a device-style folder <tmp>/24-01-01/12-00-00/ unless stated,
// so the description default is the same whichever file creates the session.
// Every expected value and message is a literal; computed comparisons are
// limited to A-versus-B end states, before/after snapshots, and
// verifyAgainstFresh.

#include <memory>

#include <QtTest>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>

#include "builtinfixture.h"
#include "csvformat.h"
#include "engine/calculationengine.h"
#include "fixturebuilder.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionimport.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;

using Outcome = MergeResult::Outcome;

Q_DECLARE_METATYPE(FlySight::DependencyKey)

namespace {

constexpr double T0 = 1704110400.0;     // 2024-01-01T12:00:00Z

constexpr int kD = 0;   // column indices
constexpr int kG = 1;

const char kId[] = "test-session";

bool isNear(double a, double b)
{
    return qAbs(a - b) <= 1e-9;
}

LogbookColumn descriptionColumn()
{
    LogbookColumn col;
    col.type = ColumnType::SessionAttribute;
    col.attributeKey = QStringLiteral("_DESCRIPTION");
    return col;
}

// IMU/wx at marker _M: needs IMU data, so a SENSOR merge affects it.
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

// Fixtures::sensorFile() with parts that can be varied. With default options
// the bytes are those of the canned fixture.
struct SensorOptions {
    QByteArray sessionId = kId;
    QByteArray firmware = "v2023.09.22";
    QByteArray device = "test-device";
    QByteArray schema;          // empty: no SCHEMA_VER line
    QByteArray wx = "62.5";
    bool fooSensor = false;     // adds FOO/{time,bar} = {3},{7} in s, furlongs
};

Fs2FileBuilder sensorVariant(const SensorOptions &options)
{
    Fs2FileBuilder b;
    b.var("FIRMWARE_VER", options.firmware)
     .var("SESSION_ID", options.sessionId)
     .var("DEVICE_ID", options.device);
    if (!options.schema.isEmpty())
        b.var("SCHEMA_VER", options.schema);
    b.sensor("IMU",
             {"time", "wy", "ax", "wz", "wx", "temperature"},
             {"s", "deg/s", "g", "deg/s", "deg/s", "deg C"})
     .sensor("MAG",
             {"time", "x", "y", "z", "temperature"},
             {"s", "gauss", "gauss", "gauss", "deg C"});
    if (options.fooSensor)
        b.sensor("FOO", {"time", "bar"}, {"s", "furlongs"});
    b.row("IMU", "3,-125,1,0," + options.wx + ",40")
     .row("MAG", "3,1,0,-0.5,40");
    if (options.fooSensor)
        b.row("FOO", "3,7");
    return b;
}

// A fresh device-style folder: <tmp>/24-01-01/12-00-00
QString deviceFolder()
{
    const QString folder = TestEnvironment::instance().newTempDir(QStringLiteral("card"))
                           + QStringLiteral("/24-01-01/12-00-00");
    if (!QDir().mkpath(folder))
        qFatal("could not create %s", qPrintable(folder));
    return folder;
}

QString writeTo(const QString &folder, const QString &fileName, const Fs2FileBuilder &file)
{
    const QString path = folder + QLatin1Char('/') + fileName;
    if (!file.write(path))
        qFatal("could not write %s", qPrintable(path));
    return path;
}

QJsonObject readIndex()
{
    return QJsonDocument::fromJson(readFileBytes(TestEnvironment::instance().indexPath())).object();
}

QString sessionFilePath(const QString &sessionId)
{
    const QString uuid = readIndex()[QStringLiteral("sessions")].toObject()[sessionId].toObject()
                             [QStringLiteral("uuid")].toString();
    if (uuid.isEmpty())
        return QString();
    return TestEnvironment::instance().sessionsDir() + QLatin1Char('/') + uuid + QStringLiteral(".csv");
}

QStringList sessionCsvFiles()
{
    return QDir(TestEnvironment::instance().sessionsDir())
        .entryList({QStringLiteral("*.csv")}, QDir::Files, QDir::Name);
}

// The value index.json holds for (session, column): Undefined when absent.
QJsonValue indexValue(const QString &sessionId, const LogbookColumn &col)
{
    const QJsonObject root = readIndex();
    const QJsonObject columns = root[QStringLiteral("columns")].toObject();
    for (auto it = columns.constBegin(); it != columns.constEnd(); ++it) {
        const QJsonObject def = it.value().toObject();
        const bool match = col.type == ColumnType::SessionAttribute
            ? def[QStringLiteral("type")].toString() == QLatin1String("SessionAttribute")
                  && def[QStringLiteral("attributeKey")].toString() == col.attributeKey
            : def[QStringLiteral("type")].toString() == QLatin1String("MeasurementAtMarker")
                  && def[QStringLiteral("sensorID")].toString() == col.sensorID
                  && def[QStringLiteral("measurementID")].toString() == col.measurementID;
        if (match) {
            return root[QStringLiteral("sessions")].toObject()[sessionId].toObject()
                       [QStringLiteral("values")].toObject().value(it.key());
        }
    }
    return QJsonValue(QJsonValue::Undefined);
}

// What is on disk for one session. index.json gets fresh column ids on every
// flush, so equal bytes also mean "the index was not rewritten".
struct Snapshot {
    QByteArray csv;
    QByteArray index;
    bool operator==(const Snapshot &other) const { return csv == other.csv && index == other.index; }
};

Snapshot snapshot(const QString &sessionId)
{
    return { readFileBytes(sessionFilePath(sessionId)),
             readFileBytes(TestEnvironment::instance().indexPath()) };
}

QString attributeText(const SessionData &session, const QString &key)
{
    return CsvFormat::formatAttributeValue(session.storedAttribute(key)).value_or(QStringLiteral("<unrepresentable>"));
}

QByteArray withoutImportTimeLine(const QByteArray &csv)
{
    QByteArray result;
    const QList<QByteArray> lines = csv.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).startsWith("$VAR,_IMPORT_TIME,"))
            continue;
        result += lines.at(i);
        if (i + 1 < lines.size())
            result += '\n';
    }
    return result;
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

bool spyHasAttribute(const QSignalSpy &spy, const QString &sessionId, const QString &key)
{
    for (const QList<QVariant> &args : spy) {
        const DependencyKey name = args.at(1).value<DependencyKey>();
        if (args.at(0).toString() == sessionId && name == DependencyKey::attribute(key))
            return true;
    }
    return false;
}

// The end state of importing the TRACK / SENSOR fixture pair, for the
// order-independence comparison.
struct EndState {
    SessionData session;
    QByteArray csv;
    QList<Outcome> outcomes;
    int rowCount = 0;
};

// The literals every end state of the fixture pair must show. "" when it does.
QString checkPairLiterals(const EndState &state)
{
    const SessionData &s = state.session;
    if (state.rowCount != 1)
        return QStringLiteral("row count %1").arg(state.rowCount);
    if (s.sensorKeys() != QStringList({"GNSS", "IMU", "MAG"}))
        return QStringLiteral("sensors ") + s.sensorKeys().join(QLatin1Char(','));
    if (!isNear(s.getMeasurement("IMU", "wx").value(0), 71.68))
        return QStringLiteral("IMU/wx");
    if (s.getMeasurement("IMU", "ax") != QVector<double>({9.80665}))
        return QStringLiteral("IMU/ax");
    if (s.sourceUnit("IMU", "ax") != QLatin1String("g"))
        return QStringLiteral("source unit of IMU/ax");
    if (s.getMeasurement("GNSS", "hMSL") != QVector<double>({4000.0, 3999.0, 3998.0}))
        return QStringLiteral("GNSS/hMSL");
    // sqrt(62.5^2 + 125^2) x 1.14688
    if (!isNear(s.getMeasurement("IMU", "wTotal").value(0), 160.28135262718493))
        return QStringLiteral("IMU/wTotal");
    if (s.getAttribute("_START_TIME").toDouble() != 1704110400.0)
        return QStringLiteral("_START_TIME");
    if (qAbs(s.getAttribute("_DURATION").toDouble() - 0.4) > 1e-6)
        return QStringLiteral("_DURATION");
    if (attributeText(s, "_DESCRIPTION") != QLatin1String("24-01-01/12-00-00"))
        return QStringLiteral("_DESCRIPTION ") + attributeText(s, "_DESCRIPTION");
    if (!state.csv.contains("$COL,GNSS,") || !state.csv.contains("$COL,IMU,") || !state.csv.contains("$COL,MAG,"))
        return QStringLiteral("saved sensors");
    return QString();
}

// The order-independence comparison rule: everything equal except the VALUE of
// _IMPORT_TIME (the wall clock of the creating import). "" when the same.
QString compareEndStates(const EndState &a, const EndState &b)
{
    if (!(a.session.sourceData() == b.session.sourceData()))
        return QStringLiteral("source data differ");
    if (a.session.attributeKeys() != b.session.attributeKeys()) {
        return QStringLiteral("attribute keys differ: ") + a.session.attributeKeys().join(QLatin1Char(','))
               + QStringLiteral(" / ") + b.session.attributeKeys().join(QLatin1Char(','));
    }
    if (!a.session.hasStoredAttribute("_IMPORT_TIME") || !b.session.hasStoredAttribute("_IMPORT_TIME"))
        return QStringLiteral("_IMPORT_TIME missing");
    const QStringList keys = a.session.attributeKeys();
    for (const QString &key : keys) {
        if (key == QLatin1String("_IMPORT_TIME"))
            continue;
        if (attributeText(a.session, key) != attributeText(b.session, key))
            return QStringLiteral("attribute %1 differs").arg(key);
    }
    if (a.session.getMeasurement("IMU", "wx") != b.session.getMeasurement("IMU", "wx")
        || a.session.getMeasurement("IMU", "ax") != b.session.getMeasurement("IMU", "ax")
        || a.session.getMeasurement("GNSS", "hMSL") != b.session.getMeasurement("GNSS", "hMSL")
        || a.session.getMeasurement("IMU", "wTotal") != b.session.getMeasurement("IMU", "wTotal")
        || a.session.getAttribute("_START_TIME") != b.session.getAttribute("_START_TIME")
        || a.session.getAttribute("_DURATION") != b.session.getAttribute("_DURATION"))
        return QStringLiteral("effective values differ");
    if (a.csv.isEmpty() || withoutImportTimeLine(a.csv) != withoutImportTimeLine(b.csv))
        return QStringLiteral("saved files differ");
    return QString();
}

} // namespace

class ImportMergeTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void newSessionGetsDefaults();

    void mergeOrderLoaded();
    void mergeOrderUnloaded();
    void mergeInOneBatch();

    void conflictChangesNothing_loaded_data();
    void conflictChangesNothing_loaded();
    void conflictChangesNothing_unloaded_data();
    void conflictChangesNothing_unloaded();
    void batchPartialFailure();

    void editsSurviveMerge_data();
    void editsSurviveMerge();
    void unmatchedMeasurementsSurvive_data();
    void unmatchedMeasurementsSurvive();
    void defaultsNotReappliedOnMerge();
    void viewerSavedFileKeepsExistingEdits();

    void rejectedSchemaLeavesSession_loaded_data();
    void rejectedSchemaLeavesSession_loaded();
    void rejectedSchemaLeavesSession_unloaded_data();
    void rejectedSchemaLeavesSession_unloaded();

    void escapeHatch_data();
    void escapeHatch();
    void explicitSchemaMismatch();
    void absenceNeverConflicts();

    void failedLoadIsAnError();
    void failedPlaceholderIsNeverSaved();
    void identityStubIsMatched();

    void mergeEffects();
    void identicalReimportIsNoop_data();
    void identicalReimportIsNoop();
    void fs1ReimportMergesIntoItself();
    void candidateSwitchAfterMerge_data();
    void candidateSwitchAfterMerge();
    void raggedMergeRejected();
    void sessionDataOverloadAdoptsAsIs();

private:
    // A fresh logbook and a fresh model (init() does this; the two-logbook
    // tests call it again for their second scenario).
    void freshStart();
    // Application restart: everything saved, then every row an unloaded stub
    // with its column values cached.
    bool makeStub();
    MergeResult importOne(const QString &path);
    SessionData &session(const QString &id = QString::fromLatin1(kId))
    {
        return m_model->sessionRef(m_model->getSessionRow(id));
    }

    enum class Mode { Loaded, StubBetween, OneBatch };
    // Imports the fixture pair in the given order ("TRACK", "SENSOR") into a
    // fresh logbook and reports the end state.
    bool runPair(const QStringList &order, Mode mode, EndState *out, int *sessionLoadedEmissions = nullptr);

    void conflictRows();
    void runConflict(bool unloaded);
    void schemaRows();
    void runRejectedSchema(bool unloaded);

    std::unique_ptr<SessionModel> m_model;
    const LogbookColumn m_d = descriptionColumn();
    const LogbookColumn m_g = gyroColumn();
};

void ImportMergeTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumnStore::instance().setColumns({m_d, m_g});
}

void ImportMergeTest::init()
{
    freshStart();
}

void ImportMergeTest::cleanup()
{
    m_model.reset();
}

void ImportMergeTest::freshStart()
{
    m_model.reset();
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();
    m_model = std::make_unique<SessionModel>();
}

bool ImportMergeTest::makeStub()
{
    if (!waitForIdle(*m_model))
        return false;
    m_model.reset();

    LogbookManager &logbook = LogbookManager::instance();
    TestEnvironment::instance().reopenLogbook();
    logbook.initialize();
    if (!logbook.hasIndexData() || logbook.cachedValuesDiscardedOnLoad())
        return false;

    m_model = std::make_unique<SessionModel>();
    m_model->populateFromIndex(logbook.cachedColumnValues(LogbookColumnStore::instance().enabledColumns()),
                               logbook.lastAccessedMap());
    for (int row = 0; row < m_model->rowCount(); ++row) {
        if (m_model->rowAt(row).isLoaded() || m_model->rowAt(row).cachedValues.size() != 2)
            return false;
    }
    return m_model->rowCount() > 0;
}

MergeResult ImportMergeTest::importOne(const QString &path)
{
    const SessionImport::BatchResult batch = SessionImport::importFiles(*m_model, {path});
    return batch.files.size() == 1 ? batch.files.first() : MergeResult();
}

bool ImportMergeTest::runPair(const QStringList &order, Mode mode, EndState *out, int *sessionLoadedEmissions)
{
    freshStart();

    const QString folder = deviceFolder();
    QMap<QString, QString> paths;
    paths[QStringLiteral("TRACK")] = writeTo(folder, QStringLiteral("TRACK.CSV"), Fixtures::trackFile());
    paths[QStringLiteral("SENSOR")] = writeTo(folder, QStringLiteral("SENSOR.CSV"), Fixtures::sensorFile());

    out->outcomes.clear();
    if (mode == Mode::OneBatch) {
        const SessionImport::BatchResult batch =
            SessionImport::importFiles(*m_model, {paths.value(order.at(0)), paths.value(order.at(1))});
        for (const MergeResult &result : batch.files)
            out->outcomes.append(result.outcome);
    } else {
        out->outcomes.append(importOne(paths.value(order.at(0))).outcome);
        if (mode == Mode::StubBetween && !makeStub())
            return false;

        QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
        out->outcomes.append(importOne(paths.value(order.at(1))).outcome);
        if (sessionLoadedEmissions)
            *sessionLoadedEmissions = int(loadedSpy.count());
        if (mode == Mode::StubBetween && !m_model->rowAt(0).isLoaded())
            return false;       // an unloaded merge leaves the session loaded
    }

    if (!waitForIdle(*m_model))
        return false;

    out->rowCount = m_model->rowCount();
    const int row = m_model->getSessionRow(QString::fromLatin1(kId));
    if (row < 0 || m_model->rowAt(row).dirty)
        return false;
    out->session = m_model->sessionRef(row);        // a copy: stored state only
    out->csv = readFileBytes(sessionFilePath(QString::fromLatin1(kId)));
    return true;
}

// ─────────────────────────────── creation (spec 6.2)

void ImportMergeTest::newSessionGetsDefaults()
{
    const QString track = writeTo(deviceFolder(), QStringLiteral("TRACK.CSV"), Fixtures::trackFile());

    const MergeResult result = importOne(track);
    QCOMPARE(result.outcome, Outcome::Created);
    QVERIFY(result.ok());
    QVERIFY(result.error.isEmpty());
    QCOMPARE(result.sessionId, QStringLiteral("test-session"));
    QCOMPARE(result.filePath, track);

    QCOMPARE(m_model->rowCount(), 1);
    const SessionData &s = session();
    QCOMPARE(s.storedAttribute("_DESCRIPTION").toString(), QStringLiteral("24-01-01/12-00-00"));
    QCOMPARE(s.storedAttribute("_JUMPER_MASS").toDouble(), 1.0);
    QCOMPARE(s.storedAttribute("_PLANFORM_AREA").toDouble(), 1.0);
    QVERIFY(s.hasStoredAttribute("_WIND_N"));
    QCOMPARE(s.storedAttribute("_WIND_N").toDouble(), 0.0);
    QVERIFY(s.storedAttribute("_IMPORT_TIME").toDouble() > 0.0);
    QCOMPARE(s.storedAttribute("DEVICE_ID").toString(), QStringLiteral("test-device"));
    QVERIFY(!s.hasStoredAttribute("_GROUND_ELEV"));
    QVERIFY(!s.hasStoredAttribute("SCHEMA_VER"));

    // Dirty, then saved by the idle scheduler
    QVERIFY(m_model->rowAt(0).dirty);
    QVERIFY(sessionCsvFiles().isEmpty());
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(!m_model->rowAt(0).dirty);
    QCOMPARE(sessionCsvFiles().size(), 1);
    QVERIFY(readFileBytes(sessionFilePath(kId)).contains("$VAR,_DESCRIPTION,24-01-01/12-00-00\n"));
}

// ─────────────────────────────── order independence (acceptance 7)

// Acceptance 7
void ImportMergeTest::mergeOrderLoaded()
{
    EndState a, b;
    QVERIFY(runPair({"TRACK", "SENSOR"}, Mode::Loaded, &a));
    QVERIFY(runPair({"SENSOR", "TRACK"}, Mode::Loaded, &b));

    QCOMPARE(a.outcomes, QList<Outcome>({Outcome::Created, Outcome::Merged}));
    QCOMPARE(b.outcomes, QList<Outcome>({Outcome::Created, Outcome::Merged}));
    QCOMPARE(checkPairLiterals(a), QString());
    QCOMPARE(checkPairLiterals(b), QString());
    QCOMPARE(compareEndStates(a, b), QString());
}

// Acceptance 7: the existing session is not loaded when the second file arrives
void ImportMergeTest::mergeOrderUnloaded()
{
    EndState a, b, aLoaded;
    int loadedA = 0;
    int loadedB = 0;
    QVERIFY(runPair({"TRACK", "SENSOR"}, Mode::StubBetween, &a, &loadedA));
    QVERIFY(runPair({"SENSOR", "TRACK"}, Mode::StubBetween, &b, &loadedB));
    QVERIFY(runPair({"TRACK", "SENSOR"}, Mode::Loaded, &aLoaded));

    QCOMPARE(a.outcomes, QList<Outcome>({Outcome::Created, Outcome::Merged}));
    QCOMPARE(b.outcomes, QList<Outcome>({Outcome::Created, Outcome::Merged}));
    QCOMPARE(loadedA, 1);       // sessionLoaded, once
    QCOMPARE(loadedB, 1);
    QCOMPARE(checkPairLiterals(a), QString());
    QCOMPARE(checkPairLiterals(b), QString());
    QCOMPARE(compareEndStates(a, b), QString());
    QCOMPARE(compareEndStates(a, aLoaded), QString());
}

// Acceptance 7: both files in one import
void ImportMergeTest::mergeInOneBatch()
{
    EndState a, b, separate;
    QVERIFY(runPair({"TRACK", "SENSOR"}, Mode::OneBatch, &a));
    QVERIFY(runPair({"SENSOR", "TRACK"}, Mode::OneBatch, &b));
    QVERIFY(runPair({"TRACK", "SENSOR"}, Mode::Loaded, &separate));

    QCOMPARE(a.outcomes, QList<Outcome>({Outcome::Created, Outcome::Merged}));
    QCOMPARE(b.outcomes, QList<Outcome>({Outcome::Created, Outcome::Merged}));
    QCOMPARE(checkPairLiterals(a), QString());
    QCOMPARE(checkPairLiterals(b), QString());
    QCOMPARE(compareEndStates(a, b), QString());
    QCOMPARE(compareEndStates(a, separate), QString());
}

// ─────────────────────────────── conflicts (acceptance 7, spec 6.3)

void ImportMergeTest::conflictRows()
{
    QTest::addColumn<QByteArray>("firmware");
    QTest::addColumn<QByteArray>("device");
    QTest::addColumn<QString>("error");

    QTest::newRow("FIRMWARE_VER")
        << QByteArray("v2024.01.01") << QByteArray("test-device")
        << "Attribute 'FIRMWARE_VER' conflicts with the existing session "
           "(session: 'v2023.09.22', file: 'v2024.01.01'). "
           "To replace the session, delete it and re-import its files.";
    QTest::newRow("DEVICE_ID")
        << QByteArray("v2023.09.22") << QByteArray("other-device")
        << "Attribute 'DEVICE_ID' conflicts with the existing session "
           "(session: 'test-device', file: 'other-device'). "
           "To replace the session, delete it and re-import its files.";
}

void ImportMergeTest::runConflict(bool unloaded)
{
    QFETCH(QByteArray, firmware);
    QFETCH(QByteArray, device);
    QFETCH(QString, error);

    const QString folder = deviceFolder();
    QCOMPARE(importOne(writeTo(folder, QStringLiteral("TRACK.CSV"), Fixtures::trackFile())).outcome, Outcome::Created);
    if (unloaded)
        QVERIFY(makeStub());
    else
        QVERIFY(waitForIdle(*m_model));

    LogbookManager &logbook = LogbookManager::instance();
    const Snapshot before = snapshot(kId);
    QVERIFY(!before.csv.isEmpty());
    const QMap<int, QVariant> cachedBefore = m_model->rowAt(0).cachedValues;

    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QSignalSpy resetSpy(m_model.get(), SIGNAL(modelReset()));
    QSignalSpy changedSpy(m_model.get(), &SessionModel::modelChanged);
    QSignalSpy dataSpy(m_model.get(), &SessionModel::dataChanged);
    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);

    SensorOptions options;
    options.firmware = firmware;
    options.device = device;
    const MergeResult result = importOne(writeTo(folder, QStringLiteral("SENSOR.CSV"), sensorVariant(options)));

    QCOMPARE(result.outcome, Outcome::Failed);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, error);
    QCOMPARE(result.sessionId, QStringLiteral("test-session"));

    // Nothing happened, now or later
    QVERIFY(!m_model->rowAt(0).dirty);
    QVERIFY(!logbook.hasUnsavedColumns(kId));
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(snapshot(kId) == before);
    QCOMPARE(m_model->rowCount(), 1);
    QVERIFY(!m_model->rowAt(0).dirty);
    QCOMPARE(m_model->rowAt(0).cachedValues, cachedBefore);
    QCOMPARE(dependencySpy.count(), 0);
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(changedSpy.count(), 0);
    QCOMPARE(dataSpy.count(), 0);
    QCOMPARE(loadedSpy.count(), 0);

    // The session did not acquire the file's sensors
    QCOMPARE(m_model->rowAt(0).isLoaded(), !unloaded);
    if (unloaded) {
        const std::optional<SessionData> onDisk = logbook.loadSessionRaw(kId);
        QVERIFY(onDisk.has_value());
        QCOMPARE(onDisk->sensorKeys(), QStringList({"GNSS"}));
    } else {
        QCOMPARE(session().sensorKeys(), QStringList({"GNSS"}));
    }
}

void ImportMergeTest::conflictChangesNothing_loaded_data() { conflictRows(); }
// Acceptance 7
void ImportMergeTest::conflictChangesNothing_loaded() { runConflict(false); }
void ImportMergeTest::conflictChangesNothing_unloaded_data() { conflictRows(); }
// Acceptance 7
void ImportMergeTest::conflictChangesNothing_unloaded() { runConflict(true); }

void ImportMergeTest::batchPartialFailure()
{
    const QString folder = deviceFolder();
    SensorOptions options;
    options.firmware = "v2024.01.01";
    const QString track = writeTo(folder, QStringLiteral("TRACK.CSV"), Fixtures::trackFile());
    const QString sensor = writeTo(folder, QStringLiteral("SENSOR.CSV"), sensorVariant(options));

    const SessionImport::BatchResult batch = SessionImport::importFiles(*m_model, {track, sensor});
    QCOMPARE(batch.files.size(), 2);
    QCOMPARE(batch.files.at(0).outcome, Outcome::Created);
    QCOMPARE(batch.files.at(1).outcome, Outcome::Failed);
    QVERIFY(batch.files.at(1).error.contains(QStringLiteral("'FIRMWARE_VER'")));
    QCOMPARE(batch.importedSessionIds(), QStringList({"test-session"}));
    QCOMPARE(batch.failures().size(), 1);

    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(session().sensorKeys(), QStringList({"GNSS"}));
    QCOMPARE(session().storedAttribute("FIRMWARE_VER").toString(), QStringLiteral("v2023.09.22"));

    QVERIFY(waitForIdle(*m_model));
    QVERIFY(!m_model->rowAt(0).dirty);
    const QByteArray csv = readFileBytes(sessionFilePath(kId));
    QVERIFY(csv.contains("$COL,GNSS,"));
    QVERIFY(!csv.contains("$COL,IMU,"));
}

// ─────────────────────────────── what survives a merge (acceptance 7, spec 6.4 / 6.5)

void ImportMergeTest::editsSurviveMerge_data()
{
    QTest::addColumn<bool>("unloaded");
    QTest::newRow("loaded") << false;
    QTest::newRow("unloaded") << true;
}

// Acceptance 7
void ImportMergeTest::editsSurviveMerge()
{
    QFETCH(bool, unloaded);

    const QString folder = deviceFolder();
    QCOMPARE(importOne(writeTo(folder, QStringLiteral("TRACK.CSV"), Fixtures::trackFile())).outcome, Outcome::Created);

    QVERIFY(m_model->updateAttribute(kId, "_DESCRIPTION", QStringLiteral("my jump")));
    QVERIFY(m_model->updateAttribute(kId, "_EXIT_TIME", 1704110400.2));     // a marker
    QVERIFY(m_model->updateAttribute(kId, "_GROUND_ELEV", 12.5));
    QVERIFY(m_model->updateAttribute(kId, "_WSP_TOP_ALT", 2400.0));         // an analysis parameter
    const QString importTimeBefore = attributeText(session(), "_IMPORT_TIME");
    QVERIFY(importTimeBefore.toDouble() > 0.0);

    if (unloaded)
        QVERIFY(makeStub());

    QCOMPARE(importOne(writeTo(folder, QStringLiteral("SENSOR.CSV"), Fixtures::sensorFile())).outcome, Outcome::Merged);

    const SessionData &s = session();
    QCOMPARE(s.sensorKeys(), QStringList({"GNSS", "IMU", "MAG"}));
    QCOMPARE(attributeText(s, "_DESCRIPTION"), QStringLiteral("my jump"));
    QCOMPARE(attributeText(s, "_EXIT_TIME"), QStringLiteral("1704110400.2"));
    QCOMPARE(attributeText(s, "_GROUND_ELEV"), QStringLiteral("12.5"));
    QCOMPARE(attributeText(s, "_WSP_TOP_ALT"), QStringLiteral("2400"));
    QCOMPARE(attributeText(s, "_IMPORT_TIME"), importTimeBefore);

    QVERIFY(waitForIdle(*m_model));
    const QByteArray csv = readFileBytes(sessionFilePath(kId));
    QVERIFY(csv.contains("$VAR,_DESCRIPTION,my jump\n"));
    QVERIFY(csv.contains("$VAR,_EXIT_TIME,1704110400.2\n"));
    QVERIFY(csv.contains("$VAR,_GROUND_ELEV,12.5\n"));
    QVERIFY(csv.contains("$VAR,_WSP_TOP_ALT,2400\n"));
    QVERIFY(csv.contains("$VAR,_IMPORT_TIME," + importTimeBefore.toLatin1() + "\n"));
    QVERIFY(csv.contains("$COL,IMU,"));
}

void ImportMergeTest::unmatchedMeasurementsSurvive_data()
{
    QTest::addColumn<bool>("unloaded");
    QTest::newRow("loaded") << false;
    QTest::newRow("stub") << true;      // Phase 8: the session is an unloaded stub when the file arrives
}

// Acceptance 7
void ImportMergeTest::unmatchedMeasurementsSurvive()
{
    QFETCH(bool, unloaded);

    const QString folder = deviceFolder();
    SensorOptions withFoo;
    withFoo.fooSensor = true;
    QCOMPARE(importOne(writeTo(folder, QStringLiteral("SENSOR.CSV"), sensorVariant(withFoo))).outcome, Outcome::Created);
    QCOMPARE(session().sensorKeys(), QStringList({"FOO", "IMU", "MAG"}));
    QVERIFY(waitForIdle(*m_model));

    if (unloaded)
        QVERIFY(makeStub());

    QSignalSpy spy(m_model.get(), &SessionModel::dependencyChanged);

    // The plain file, with one gyro sample changed and without FOO
    SensorOptions changed;
    changed.wx = "10";
    const MergeResult result = importOne(writeTo(deviceFolder(), QStringLiteral("SENSOR.CSV"), sensorVariant(changed)));
    QCOMPARE(result.outcome, Outcome::Merged);

    const SessionData &s = session();
    QCOMPARE(s.sensorKeys(), QStringList({"FOO", "IMU", "MAG"}));
    QCOMPARE(s.sourceMeasurement("FOO", "bar"), QVector<double>({7.0}));
    QCOMPARE(s.sourceUnit("FOO", "bar"), QStringLiteral("furlongs"));
    QCOMPARE(s.sourceMeasurement("IMU", "wx"), QVector<double>({10.0}));
    QCOMPARE(s.sourceMeasurement("IMU", "wy"), QVector<double>({-125.0}));
    QCOMPARE(s.sourceMeasurement("MAG", "x"), QVector<double>({1.0}));
    QCOMPARE(s.sourceUnit("MAG", "x"), QStringLiteral("gauss"));

    QVERIFY(spyHasMeasurement(spy, kId, "IMU", "wx"));
    QVERIFY(!spyHasMeasurement(spy, kId, "MAG", "x"));
    QVERIFY(!spyHasMeasurement(spy, kId, "FOO", "bar"));

    QVERIFY(waitForIdle(*m_model));
    const QByteArray csv = readFileBytes(sessionFilePath(kId));
    QVERIFY(csv.contains("$COL,FOO,"));
    QVERIFY(csv.contains("$COL,FOO,bar,time\n"));       // unknown sensor: columns by name
    QVERIFY(csv.contains("$UNIT,FOO,furlongs,s\n"));
    QVERIFY(csv.contains("$FOO,7,3\n"));
}

// Spec 6.3 as decided for Viewer's own attributes (Q2), through the model: a
// file Viewer itself saved carries `_` attributes. Re-importing it never
// overwrites what the session holds now (existing wins); a `_` attribute the
// session lacks is added; none of it is ever a conflict.
void ImportMergeTest::viewerSavedFileKeepsExistingEdits()
{
    QCOMPARE(importOne(writeTo(deviceFolder(), QStringLiteral("TRACK.CSV"), Fixtures::trackFile())).outcome,
             Outcome::Created);
    QVERIFY(m_model->updateAttribute(kId, "_DESCRIPTION", QStringLiteral("mine")));
    QVERIFY(m_model->updateAttribute(kId, "_GROUND_ELEV", 12.5));
    QVERIFY(waitForIdle(*m_model));

    // The Viewer-exported file, kept aside as a user would keep a backup
    const QByteArray exported = readFileBytes(sessionFilePath(kId));
    QVERIFY(exported.contains("$VAR,_DESCRIPTION,mine\n"));
    QVERIFY(exported.contains("$VAR,_GROUND_ELEV,12.5\n"));
    const QString backup = TestEnvironment::instance().newTempDir(QStringLiteral("backup")) + QStringLiteral("/backup.csv");
    QVERIFY(writeFile(backup, exported));

    // The session moves on
    QVERIFY(m_model->updateAttribute(kId, "_DESCRIPTION", QStringLiteral("newer")));
    QVERIFY(waitForIdle(*m_model));
    const QByteArray savedNewer = readFileBytes(sessionFilePath(kId));

    // Every `_` key of the backup exists in the session: nothing to do, nothing saved
    MergeResult result = importOne(backup);
    QCOMPARE(result.outcome, Outcome::Unchanged);
    QVERIFY(result.error.isEmpty());
    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(attributeText(session(), "_DESCRIPTION"), QStringLiteral("newer"));
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(readFileBytes(sessionFilePath(kId)), savedNewer);

    // A `_` key the session lacks is added; the one it has still wins
    QVERIFY(m_model->removeAttribute(kId, "_GROUND_ELEV"));
    QVERIFY(!session().hasAttribute("_GROUND_ELEV"));
    result = importOne(backup);
    QCOMPARE(result.outcome, Outcome::Merged);
    QCOMPARE(attributeText(session(), "_GROUND_ELEV"), QStringLiteral("12.5"));
    QCOMPARE(attributeText(session(), "_DESCRIPTION"), QStringLiteral("newer"));

    QVERIFY(waitForIdle(*m_model));
    const QByteArray csv = readFileBytes(sessionFilePath(kId));
    QVERIFY(csv.contains("$VAR,_DESCRIPTION,newer\n"));
    QVERIFY(csv.contains("$VAR,_GROUND_ELEV,12.5\n"));
}

void ImportMergeTest::defaultsNotReappliedOnMerge()
{
    PreferencesManager &prefs = PreferencesManager::instance();

    QCOMPARE(importOne(writeTo(deviceFolder(), QStringLiteral("TRACK.CSV"), Fixtures::trackFile())).outcome,
             Outcome::Created);
    const QString importTimeBefore = attributeText(session(), "_IMPORT_TIME");

    // Everything an import-time default is derived from changes ...
    prefs.setValue(PreferenceKeys::AeroMass, 90.0);
    prefs.setValue(PreferenceKeys::AeroArea, 3.0);
    prefs.setValue(PreferenceKeys::ImportGroundReferenceMode, QStringLiteral("Fixed"));
    prefs.setValue(PreferenceKeys::ImportFixedElevation, 50.0);

    // ... and the second file comes from a different, non-device folder
    const QString elsewhere = TestEnvironment::instance().newTempDir(QStringLiteral("elsewhere"));
    QCOMPARE(importOne(writeTo(elsewhere, QStringLiteral("SENSOR.CSV"), Fixtures::sensorFile())).outcome,
             Outcome::Merged);

    const SessionData &s = session();
    QCOMPARE(s.sensorKeys(), QStringList({"GNSS", "IMU", "MAG"}));
    QCOMPARE(attributeText(s, "_JUMPER_MASS"), QStringLiteral("1"));
    QCOMPARE(attributeText(s, "_PLANFORM_AREA"), QStringLiteral("1"));
    QVERIFY(!s.hasStoredAttribute("_GROUND_ELEV"));
    QCOMPARE(attributeText(s, "_DESCRIPTION"), QStringLiteral("24-01-01/12-00-00"));
    QCOMPARE(attributeText(s, "_IMPORT_TIME"), importTimeBefore);
    QCOMPARE(attributeText(s, "DEVICE_ID"), QStringLiteral("test-device"));
}

// ─────────────────────────────── rejected files (acceptance 3, spec 6.1)

void ImportMergeTest::schemaRows()
{
    QTest::addColumn<QByteArray>("schema");
    QTest::addColumn<QString>("error");
    QTest::newRow("3") << QByteArray("3") << "Unsupported SCHEMA_VER '3' (supported: 1, 2)";
    QTest::newRow("abc") << QByteArray("abc") << "Unsupported SCHEMA_VER 'abc' (supported: 1, 2)";
}

void ImportMergeTest::runRejectedSchema(bool unloaded)
{
    QFETCH(QByteArray, schema);
    QFETCH(QString, error);

    const QString folder = deviceFolder();
    QCOMPARE(importOne(writeTo(folder, QStringLiteral("SENSOR.CSV"), Fixtures::sensorFile())).outcome, Outcome::Created);
    QVERIFY(m_model->updateAttribute(kId, "_DESCRIPTION", QStringLiteral("edited")));
    if (unloaded)
        QVERIFY(makeStub());
    else
        QVERIFY(waitForIdle(*m_model));

    const Snapshot before = snapshot(kId);
    QVERIFY(before.csv.contains("$VAR,_DESCRIPTION,edited\n"));
    const QMap<int, QVariant> cachedBefore = m_model->rowAt(0).cachedValues;
    SessionData copyBefore;
    if (!unloaded)
        copyBefore = session();

    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QSignalSpy resetSpy(m_model.get(), SIGNAL(modelReset()));

    SensorOptions options;
    options.schema = schema;
    const MergeResult result = importOne(writeTo(folder, QStringLiteral("SENSOR-EDITED.CSV"), sensorVariant(options)));
    QCOMPARE(result.outcome, Outcome::Failed);
    QCOMPARE(result.error, error);
    QVERIFY(result.sessionId.isEmpty());        // rejected by the parser: it never reached the model

    QVERIFY(waitForIdle(*m_model));
    QVERIFY(snapshot(kId) == before);
    QCOMPARE(m_model->rowCount(), 1);
    QVERIFY(!m_model->rowAt(0).dirty);
    QCOMPARE(m_model->rowAt(0).cachedValues, cachedBefore);
    QCOMPARE(dependencySpy.count(), 0);
    QCOMPARE(resetSpy.count(), 0);

    QCOMPARE(m_model->rowAt(0).isLoaded(), !unloaded);
    if (!unloaded) {
        const SessionData &s = session();
        QCOMPARE(s.attributeKeys(), copyBefore.attributeKeys());
        for (const QString &key : copyBefore.attributeKeys())
            QCOMPARE(attributeText(s, key), attributeText(copyBefore, key));
        QVERIFY(s.sourceData() == copyBefore.sourceData());
        QVERIFY(!s.hasAttribute("SCHEMA_VER"));
        QCOMPARE(attributeText(s, "_DESCRIPTION"), QStringLiteral("edited"));
        QVERIFY(isNear(s.getMeasurement("IMU", "wx").value(0), 71.68));
    } else {
        const std::optional<SessionData> onDisk = LogbookManager::instance().loadSessionRaw(kId);
        QVERIFY(onDisk.has_value());
        QVERIFY(!onDisk->hasAttribute("SCHEMA_VER"));
    }
}

void ImportMergeTest::rejectedSchemaLeavesSession_loaded_data() { schemaRows(); }
// Acceptance 3
void ImportMergeTest::rejectedSchemaLeavesSession_loaded() { runRejectedSchema(false); }
void ImportMergeTest::rejectedSchemaLeavesSession_unloaded_data() { schemaRows(); }
// Acceptance 3
void ImportMergeTest::rejectedSchemaLeavesSession_unloaded() { runRejectedSchema(true); }

// ─────────────────────────────── the escape hatch (acceptance 8, spec 6.3)

void ImportMergeTest::escapeHatch_data()
{
    QTest::addColumn<bool>("unloaded");
    QTest::newRow("loaded") << false;
    QTest::newRow("unloaded") << true;
}

// Acceptance 8
void ImportMergeTest::escapeHatch()
{
    QFETCH(bool, unloaded);

    const QString folder = deviceFolder();
    QCOMPARE(importOne(writeTo(folder, QStringLiteral("SENSOR.CSV"), Fixtures::sensorFile())).outcome, Outcome::Created);

    // Unmarked file: the legacy gyro correction applies
    QVERIFY(isNear(session().getMeasurement("IMU", "wx").value(0), 71.68));
    QVERIFY(isNear(session().getMeasurement("IMU", "wTotal").value(0), 160.28135262718493));
    QCOMPARE(session().getMeasurement("IMU", "ax"), QVector<double>({9.80665}));

    QVERIFY(m_model->updateAttribute(kId, "_DESCRIPTION", QStringLiteral("keep me")));
    QCOMPARE(m_model->rowCount(), 1);
    if (unloaded)
        QVERIFY(makeStub());

    QSignalSpy spy(m_model.get(), &SessionModel::dependencyChanged);

    // The user adds $VAR,SCHEMA_VER,2 to the file and imports it again
    SensorOptions options;
    options.schema = "2";
    const MergeResult result = importOne(writeTo(folder, QStringLiteral("SENSOR.CSV"), sensorVariant(options)));
    QCOMPARE(result.outcome, Outcome::Merged);
    QCOMPARE(m_model->rowCount(), 1);

    SessionData &s = session();
    QCOMPARE(s.storedAttribute("SCHEMA_VER").toString(), QStringLiteral("2"));
    QCOMPARE(s.getMeasurement("IMU", "wx"), QVector<double>({62.5}));
    QCOMPARE(s.getMeasurement("IMU", "wy"), QVector<double>({-125.0}));
    QVERIFY(isNear(s.getMeasurement("IMU", "wTotal").value(0), 139.75424859373686));
    QCOMPARE(s.getMeasurement("IMU", "ax"), QVector<double>({9.80665}));
    QCOMPARE(attributeText(s, "_DESCRIPTION"), QStringLiteral("keep me"));

    QVERIFY(spyHasAttribute(spy, kId, "SCHEMA_VER"));
    QVERIFY(!spyHasMeasurement(spy, kId, "IMU", "ax"));
    if (!unloaded) {
        // A loaded session's engine knows what depended on the schema. (A
        // session loaded for the merge has a cold engine: nothing was cached,
        // so the written names are all there is to report.)
        QVERIFY(spyHasMeasurement(spy, kId, "IMU", "wx"));
        QVERIFY(spyHasMeasurement(spy, kId, "IMU", "wTotal"));
    }

    QVERIFY(s.calculationEngine().verifyAgainstFresh({DependencyKey::measurement("IMU", "wx"),
                                                      DependencyKey::measurement("IMU", "wy"),
                                                      DependencyKey::measurement("IMU", "wz"),
                                                      DependencyKey::measurement("IMU", "wTotal")}).isEmpty());

    QVERIFY(waitForIdle(*m_model));
    const QByteArray csv = readFileBytes(sessionFilePath(kId));
    QCOMPARE(csv.count("$VAR,SCHEMA_VER,"), 1);
    QVERIFY(csv.contains("$VAR,SCHEMA_VER,2\n"));
    // The recorded values, never the corrected ones (the exporter's column
    // order: time, wx, wy, wz, ax, temperature)
    QVERIFY(csv.contains("$IMU,3,62.5,-125,0,1,40\n"));
    QVERIFY(csv.contains("$VAR,_DESCRIPTION,keep me\n"));
    QCOMPARE(sessionCsvFiles().size(), 1);
}

void ImportMergeTest::explicitSchemaMismatch()
{
    const QString folder = deviceFolder();
    SensorOptions two;
    two.schema = "2";
    QCOMPARE(importOne(writeTo(folder, QStringLiteral("SENSOR.CSV"), sensorVariant(two))).outcome, Outcome::Created);
    QCOMPARE(session().getMeasurement("IMU", "wx"), QVector<double>({62.5}));

    SensorOptions one;
    one.schema = "1";
    const MergeResult result = importOne(writeTo(folder, QStringLiteral("SENSOR-1.CSV"), sensorVariant(one)));
    QCOMPARE(result.outcome, Outcome::Failed);
    QCOMPARE(result.error,
             QStringLiteral("Attribute 'SCHEMA_VER' conflicts with the existing session (session: '2', file: '1'). "
                            "To change a session's schema version, delete the session and re-import its files."));
    QVERIFY(result.error.contains(QStringLiteral("delete the session and re-import")));

    QCOMPARE(session().storedAttribute("SCHEMA_VER").toString(), QStringLiteral("2"));
    QCOMPARE(session().getMeasurement("IMU", "wx"), QVector<double>({62.5}));
}

void ImportMergeTest::absenceNeverConflicts()
{
    SensorOptions two;
    two.schema = "2";

    // The session declares SCHEMA_VER, the second file does not
    {
        const QString folder = deviceFolder();
        QCOMPARE(importOne(writeTo(folder, QStringLiteral("SENSOR.CSV"), sensorVariant(two))).outcome, Outcome::Created);
        QCOMPARE(importOne(writeTo(folder, QStringLiteral("TRACK.CSV"), Fixtures::trackFile())).outcome, Outcome::Merged);
        QCOMPARE(session().storedAttribute("SCHEMA_VER").toString(), QStringLiteral("2"));
        QCOMPARE(session().getMeasurement("IMU", "wx"), QVector<double>({62.5}));
        QCOMPARE(session().sensorKeys(), QStringList({"GNSS", "IMU", "MAG"}));
    }

    // The reverse order, in another logbook
    freshStart();
    {
        const QString folder = deviceFolder();
        QCOMPARE(importOne(writeTo(folder, QStringLiteral("TRACK.CSV"), Fixtures::trackFile())).outcome, Outcome::Created);
        QVERIFY(!session().hasStoredAttribute("SCHEMA_VER"));
        QCOMPARE(importOne(writeTo(folder, QStringLiteral("SENSOR.CSV"), sensorVariant(two))).outcome, Outcome::Merged);
        QCOMPARE(session().storedAttribute("SCHEMA_VER").toString(), QStringLiteral("2"));
        QCOMPARE(session().getMeasurement("IMU", "wx"), QVector<double>({62.5}));
    }
}

// ─────────────────────────────── existing sessions that cannot be loaded (spec 6.2)

void ImportMergeTest::failedLoadIsAnError()
{
    const QString folder = deviceFolder();
    QCOMPARE(importOne(writeTo(folder, QStringLiteral("TRACK.CSV"), Fixtures::trackFile())).outcome, Outcome::Created);
    QVERIFY(makeStub());

    const QString csvPath = sessionFilePath(kId);
    QVERIFY(writeFile(csvPath, "corrupt"));
    const QByteArray indexBefore = readFileBytes(TestEnvironment::instance().indexPath());

    const MergeResult result = importOne(writeTo(folder, QStringLiteral("SENSOR.CSV"), Fixtures::sensorFile()));
    QCOMPARE(result.outcome, Outcome::Failed);
    QCOMPARE(result.error,
             QStringLiteral("Existing session 'test-session' could not be loaded (Unknown file format); "
                            "the file was not imported."));

    // Never a reason to replace the session with the incoming file alone
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(readFileBytes(csvPath), QByteArray("corrupt"));
    QCOMPARE(m_model->rowCount(), 1);
    QVERIFY(!m_model->rowAt(0).isLoaded());
    QVERIFY(!m_model->rowAt(0).dirty);
    QCOMPARE(readFileBytes(TestEnvironment::instance().indexPath()), indexBefore);
    QCOMPARE(sessionCsvFiles().size(), 1);
}

// The empty session sessionRef() hands out after a failed load must never be
// merged into or written over the real file.
void ImportMergeTest::failedPlaceholderIsNeverSaved()
{
    const QString folder = deviceFolder();
    QCOMPARE(importOne(writeTo(folder, QStringLiteral("TRACK.CSV"), Fixtures::trackFile())).outcome, Outcome::Created);
    QVERIFY(makeStub());

    const QString csvPath = sessionFilePath(kId);
    QVERIFY(writeFile(csvPath, "corrupt"));

    // Something looks at the session: the placeholder is installed
    QVERIFY(m_model->sessionRef(0).attributeKeys().isEmpty());
    QVERIFY(m_model->rowAt(0).isLoaded());
    QVERIFY(m_model->rowAt(0).loadFailed);

    const MergeResult result = importOne(writeTo(folder, QStringLiteral("SENSOR.CSV"), Fixtures::sensorFile()));
    QCOMPARE(result.outcome, Outcome::Failed);
    QCOMPARE(result.error,
             QStringLiteral("Existing session 'test-session' could not be loaded (Unknown file format); "
                            "the file was not imported."));
    QVERIFY(m_model->sessionRef(0).sensorKeys().isEmpty());
    QCOMPARE(readFileBytes(csvPath), QByteArray("corrupt"));

    m_model->updateAttribute(kId, "_DESCRIPTION", QStringLiteral("x"));
    QVERIFY(!m_model->rowAt(0).dirty);
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(readFileBytes(csvPath), QByteArray("corrupt"));
    m_model->flushDirtySessions();
    QCOMPARE(readFileBytes(csvPath), QByteArray("corrupt"));
    QCOMPARE(sessionCsvFiles().size(), 1);
}

// A row known only by its file name (index.json lost) must still be matched.
void ImportMergeTest::identityStubIsMatched()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();

    const QString folder = deviceFolder();
    QCOMPARE(importOne(writeTo(folder, QStringLiteral("TRACK.CSV"), Fixtures::trackFile())).outcome, Outcome::Created);
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(sessionCsvFiles().size(), 1);
    const QString stem = QFileInfo(sessionCsvFiles().first()).completeBaseName();
    m_model.reset();

    QVERIFY(QFile::remove(env.indexPath()));
    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(logbook.hasDeferredScan());

    m_model = std::make_unique<SessionModel>();
    m_model->populateFromUuids(logbook.scannedUuids());
    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(m_model->rowAt(0).sessionId, stem);

    // The import arrives before the column worker has parsed the file
    const MergeResult result = importOne(writeTo(folder, QStringLiteral("SENSOR.CSV"), Fixtures::sensorFile()));
    QCOMPARE(result.outcome, Outcome::Merged);
    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(m_model->rowAt(0).sessionId, QStringLiteral("test-session"));
    QCOMPARE(session().sensorKeys(), QStringList({"GNSS", "IMU", "MAG"}));

    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(sessionCsvFiles(), QStringList({stem + QStringLiteral(".csv")}));
    QVERIFY(readFileBytes(env.sessionsDir() + QLatin1Char('/') + stem + QStringLiteral(".csv")).contains("$COL,IMU,"));
}

// ─────────────────────────────── effects of a merge (spec 6.5)

// Acceptance 18 (merge part)
void ImportMergeTest::mergeEffects()
{
    const QString id = QStringLiteral("descent");
    const QString folder = deviceFolder();
    QCOMPARE(importOne(writeTo(folder, QStringLiteral("TRACK.CSV"), DescentFixture::trackFile())).outcome,
             Outcome::Created);
    QVERIFY(m_model->updateAttribute(id, "_M", T0 + 15.0));
    QVERIFY(waitForIdle(*m_model));

    // Without IMU data the gyro column has no value
    QCOMPARE(m_model->rowAt(0).cachedValues.size(), 2);
    QVERIFY(!m_model->rowAt(0).cachedValues.value(kG).isValid());
    QCOMPARE(indexValue(id, m_d).toString(), QStringLiteral("24-01-01/12-00-00"));
    QVERIFY(indexValue(id, m_g).isNull());
    QVERIFY(!readFileBytes(sessionFilePath(id)).contains("$COL,IMU,"));

    m_model->resetColumnWorkStats();
    QSignalSpy spy(m_model.get(), &SessionModel::dependencyChanged);
    QSignalSpy resetSpy(m_model.get(), SIGNAL(modelReset()));
    QSignalSpy changedSpy(m_model.get(), &SessionModel::modelChanged);

    QCOMPARE(importOne(writeTo(folder, QStringLiteral("SENSOR.CSV"), DescentFixture::sensorFile())).outcome,
             Outcome::Merged);

    // Invalidation through the normal notification path, dirty, columns
    QVERIFY(spyHasMeasurement(spy, id, "IMU", "wx"));
    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(changedSpy.count(), 1);
    QVERIFY(m_model->rowAt(0).dirty);
    QVERIFY(LogbookManager::instance().hasUnsavedColumns(id));
    QVERIFY(m_model->rowAt(0).cachedValues.contains(kD));       // cannot depend on the merge
    QVERIFY(!m_model->rowAt(0).cachedValues.contains(kG));

    // Saved, and only the affected column recomputed
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(!m_model->rowAt(0).dirty);
    QVERIFY(!LogbookManager::instance().hasUnsavedColumns(id));
    QVERIFY(readFileBytes(sessionFilePath(id)).contains("$COL,IMU,"));
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 1);
    QCOMPARE(indexValue(id, m_d).toString(), QStringLiteral("24-01-01/12-00-00"));
    // wx is 3 at t = 10 and 6 at t = 20: 4.5 at the marker, x 1.14688
    QVERIFY(isNear(m_model->rowAt(0).cachedValues.value(kG).toDouble(), 5.16096));
    QVERIFY(isNear(indexValue(id, m_g).toDouble(), 5.16096));
}

void ImportMergeTest::identicalReimportIsNoop_data()
{
    QTest::addColumn<bool>("unloaded");
    QTest::newRow("loaded") << false;
    QTest::newRow("unloaded") << true;
}

void ImportMergeTest::identicalReimportIsNoop()
{
    QFETCH(bool, unloaded);

    const QString folder = deviceFolder();
    const QString track = writeTo(folder, QStringLiteral("TRACK.CSV"), Fixtures::trackFile());
    const QString sensor = writeTo(folder, QStringLiteral("SENSOR.CSV"), Fixtures::sensorFile());
    QCOMPARE(SessionImport::importFiles(*m_model, {track, sensor}).failures().size(), 0);
    if (unloaded)
        QVERIFY(makeStub());
    else
        QVERIFY(waitForIdle(*m_model));

    const Snapshot before = snapshot(kId);
    const QDateTime modifiedBefore = QFileInfo(sessionFilePath(kId)).lastModified();
    m_model->resetColumnWorkStats();

    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QSignalSpy resetSpy(m_model.get(), SIGNAL(modelReset()));
    QSignalSpy changedSpy(m_model.get(), &SessionModel::modelChanged);
    QSignalSpy dataSpy(m_model.get(), &SessionModel::dataChanged);
    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);

    const SessionImport::BatchResult again = SessionImport::importFiles(*m_model, {track, sensor});
    QCOMPARE(again.files.size(), 2);
    QCOMPARE(again.files.at(0).outcome, Outcome::Unchanged);
    QCOMPARE(again.files.at(1).outcome, Outcome::Unchanged);
    QVERIFY(again.files.at(0).ok());
    QCOMPARE(again.importedSessionIds(), QStringList({"test-session"}));    // still shown to the user

    QVERIFY(!m_model->rowAt(0).dirty);
    QVERIFY(waitForIdle(*m_model));

    QCOMPARE(dependencySpy.count(), 0);
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(changedSpy.count(), 0);
    QCOMPARE(dataSpy.count(), 0);
    QCOMPARE(loadedSpy.count(), 0);
    QVERIFY(!m_model->rowAt(0).dirty);
    QVERIFY(!LogbookManager::instance().hasUnsavedColumns(kId));
    QVERIFY(snapshot(kId) == before);
    QCOMPARE(QFileInfo(sessionFilePath(kId)).lastModified(), modifiedBefore);

    QCOMPARE(m_model->rowAt(0).isLoaded(), !unloaded);
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 0);
}

// A file without a recorded SESSION_ID is matched by the hash of its bytes.
void ImportMergeTest::fs1ReimportMergesIntoItself()
{
    // MD5 computed once with md5sum on these exact bytes
    const QByteArray bytes =
        "time,lat,lon,hMSL,velN,velE,velD,hAcc,vAcc,sAcc,heading,cAcc,gpsFix,numSV\n"
        ",(deg),(deg),(m),(m/s),(m/s),(m/s),(m),(m),(m/s),(deg),(deg),,\n"
        "2024-01-01T12:00:00.00Z,45.5,-73.25,4000.5,10,-20,5,1.5,2.5,0.25,296.5,1.25,3,12\n";
    const QString id = QStringLiteral("277eace764c3329f1f7fab1dc85c64c6");

    const QString path = TestEnvironment::instance().newTempDir(QStringLiteral("fs1")) + QStringLiteral("/12-00-00.CSV");
    QVERIFY(writeFile(path, bytes));

    const MergeResult first = importOne(path);
    QCOMPARE(first.outcome, Outcome::Created);
    QCOMPARE(first.sessionId, id);
    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(m_model->rowAt(0).sessionId, id);
    QCOMPARE(session(id).storedAttribute("SESSION_ID").toString(), id);
    QCOMPARE(session(id).storedAttribute("DEVICE_ID").toString(), QStringLiteral("n/a"));
    QCOMPARE(session(id).storedAttribute("_DESCRIPTION").toString(), QStringLiteral("12-00-00"));

    QCOMPARE(importOne(path).outcome, Outcome::Unchanged);
    QCOMPARE(m_model->rowCount(), 1);

    // ... and against its saved-and-reloaded form
    QVERIFY(makeStub());
    QCOMPARE(importOne(path).outcome, Outcome::Unchanged);
    QCOMPARE(m_model->rowCount(), 1);
    QVERIFY(!m_model->rowAt(0).isLoaded());
    QCOMPARE(sessionCsvFiles().size(), 1);
}

void ImportMergeTest::candidateSwitchAfterMerge_data()
{
    QTest::addColumn<bool>("unloaded");
    QTest::newRow("loaded") << false;
    QTest::newRow("unloaded") << true;
}

// Acceptance 10 (merge part): the merge makes the GNSS candidate viable
void ImportMergeTest::candidateSwitchAfterMerge()
{
    QFETCH(bool, unloaded);
    const QString id = QStringLiteral("descent");
    const QString folder = deviceFolder();

    QCOMPARE(importOne(writeTo(folder, QStringLiteral("SENSOR.CSV"), DescentFixture::sensorFile())).outcome,
             Outcome::Created);
    QCOMPARE(session(id).getAttribute("_START_TIME").toDouble(), 1704110410.0);
    QCOMPARE(session(id).getAttribute("_DURATION").toDouble(), 20.0);

    if (unloaded)
        QVERIFY(makeStub());

    QSignalSpy spy(m_model.get(), &SessionModel::dependencyChanged);
    QCOMPARE(importOne(writeTo(folder, QStringLiteral("TRACK.CSV"), DescentFixture::trackFile())).outcome,
             Outcome::Merged);

    if (!unloaded) {
        // The loaded session's engine had both values cached
        QVERIFY(spyHasAttribute(spy, id, "_START_TIME"));
        QVERIFY(spyHasAttribute(spy, id, "_DURATION"));
    }
    QVERIFY(spyHasMeasurement(spy, id, "GNSS", "time"));

    SessionData &s = session(id);
    QCOMPARE(s.getAttribute("_START_TIME").toDouble(), 1704110400.0);
    QCOMPARE(s.getAttribute("_DURATION").toDouble(), 295.0);
    QCOMPARE(s.calculationEngine().runCount("builtin.attr.timeExtent.GNSS"), 1);
    QVERIFY(s.calculationEngine().verifyAgainstFresh({DependencyKey::attribute("_START_TIME"),
                                                      DependencyKey::attribute("_DURATION"),
                                                      DependencyKey::attribute("_EXIT_TIME")}).isEmpty());
    QCOMPARE(s.getAttribute("_EXIT_TIME").toDouble(), T0 + 9.0);
}

void ImportMergeTest::raggedMergeRejected()
{
    SessionData existing;
    existing.setAttribute("SESSION_ID", QStringLiteral("ragged"));
    existing.setAttribute("_DESCRIPTION", QStringLiteral("three rows"));
    existing.setSourceMeasurement("X", "time", {1.0, 2.0, 3.0}, "s");
    existing.setSourceMeasurement("X", "v", {4.0, 5.0, 6.0}, "m");
    existing.setSourceMeasurement("X", "keep", {7.0, 8.0, 9.0}, "m");
    QCOMPARE(m_model->mergeSessions({existing}).first().outcome, Outcome::Created);
    QVERIFY(waitForIdle(*m_model));
    const Snapshot before = snapshot(QStringLiteral("ragged"));
    QVERIFY(before.csv.contains("$COL,X,"));

    Fs2FileBuilder file;
    file.var("SESSION_ID", "ragged")
        .sensor("X", {"time", "v"}, {"s", "m"})
        .row("X", "1,4")
        .row("X", "2,5");
    const MergeResult result = importOne(writeTo(deviceFolder(), QStringLiteral("X.CSV"), file));
    QCOMPARE(result.outcome, Outcome::Failed);
    QCOMPARE(result.error,
             QStringLiteral("Sensor 'X': the file has 2 rows but the session's column 'keep' has 3. "
                            "Delete the session and re-import its files."));

    QVERIFY(waitForIdle(*m_model));
    QVERIFY(snapshot(QStringLiteral("ragged")) == before);
    QVERIFY(!m_model->rowAt(0).dirty);
    QCOMPARE(session(QStringLiteral("ragged")).sourceMeasurement("X", "v"), QVector<double>({4.0, 5.0, 6.0}));
}

void ImportMergeTest::sessionDataOverloadAdoptsAsIs()
{
    SessionData programmatic;
    programmatic.setAttribute("SESSION_ID", QStringLiteral("built"));
    programmatic.setSourceMeasurement("X", "time", {1.0}, "s");

    SessionData anonymous;
    anonymous.setSourceMeasurement("X", "time", {1.0}, "s");

    const QList<MergeResult> results = m_model->mergeSessions({programmatic, anonymous});
    QCOMPARE(results.size(), 2);
    QCOMPARE(results.at(0).outcome, Outcome::Created);
    QCOMPARE(results.at(0).sessionId, QStringLiteral("built"));
    QVERIFY(results.at(0).filePath.isEmpty());
    QCOMPARE(results.at(1).outcome, Outcome::Failed);
    QCOMPARE(results.at(1).error, QStringLiteral("File has no SESSION_ID"));

    // Adopted as is: no import-time default of any kind
    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(session(QStringLiteral("built")).attributeKeys(), QStringList({"SESSION_ID"}));

    QVERIFY(m_model->mergeSessions(QList<SessionData>()).isEmpty());
}

FLYSIGHT_TEST_MAIN(ImportMergeTest)
#include "tst_import_merge.moc"
