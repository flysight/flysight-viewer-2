// SessionImport: the widget-free import driver behind MainWindow::importFiles.
// One result per attempted file in input order (parse failures included),
// cancellation through the progress callback, and the text of the existing
// import-failure dialog, which now carries each file's error. Every expected
// value and message is a literal.

#include <memory>

#include <QtTest>

#include <QDir>

#include "fixturebuilder.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessionimport.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;

using Outcome = MergeResult::Outcome;

namespace {

QString writeBytes(const QString &folder, const QString &fileName, const QByteArray &bytes)
{
    const QString path = folder + QLatin1Char('/') + fileName;
    if (!writeFile(path, bytes))
        qFatal("could not write %s", qPrintable(path));
    return path;
}

MergeResult failure(const QString &path, const QString &error, const QString &hint = QString())
{
    MergeResult result;
    result.filePath = path;
    result.outcome = Outcome::Failed;
    result.error = error;
    result.hint = hint;
    return result;
}

const QString kReplaceHint = QStringLiteral("To replace the session, delete it and re-import its files.");
const QString kSchemaHint =
    QStringLiteral("To change a session's schema version, delete the session and re-import its files.");

MergeResult firmwareConflict(const QString &path, const QString &fileVersion)
{
    return failure(path,
                   QStringLiteral("Attribute 'FIRMWARE_VER' conflicts with the existing session "
                                  "(session: 'v2023.09.22', file: '%1').").arg(fileVersion),
                   kReplaceHint);
}

} // namespace

class ImportBatchTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void resultsKeepInputOrder();
    void progressCancels();
    void failureMessageFew();
    void failureMessageMany();
    void failureMessageGroupsSharedReasons();
    void failureMessageHintsOnce();
    void failureMessageEmpty();
    void failureMessageWithoutBaseDir();
    void mergeFailureReachesMessage();

private:
    std::unique_ptr<SessionModel> m_model;
};

void ImportBatchTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumn description;
    description.type = ColumnType::SessionAttribute;
    description.attributeKey = QStringLiteral("_DESCRIPTION");
    LogbookColumnStore::instance().setColumns({description});
}

void ImportBatchTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();
    m_model = std::make_unique<SessionModel>();
}

void ImportBatchTest::cleanup()
{
    m_model.reset();
}

void ImportBatchTest::resultsKeepInputOrder()
{
    const QString folder = TestEnvironment::instance().newTempDir(QStringLiteral("batch"));
    const QString good = writeBytes(folder, QStringLiteral("good.csv"), Fixtures::trackFile("one").toBytes());
    const QString hello = writeBytes(folder, QStringLiteral("hello.csv"), "hello\nworld\n");
    const QString missing = folder + QStringLiteral("/missing.csv");
    const QString good2 = writeBytes(folder, QStringLiteral("good2.csv"), Fixtures::sensorFile("two").toBytes());

    QSignalSpy resetSpy(m_model.get(), SIGNAL(modelReset()));
    const SessionImport::BatchResult batch = SessionImport::importFiles(*m_model, {good, hello, missing, good2});

    QCOMPARE(batch.files.size(), 4);
    QCOMPARE(batch.files.at(0).outcome, Outcome::Created);
    QCOMPARE(batch.files.at(0).filePath, good);
    QCOMPARE(batch.files.at(0).sessionId, QStringLiteral("one"));
    QVERIFY(batch.files.at(0).error.isEmpty());

    QCOMPARE(batch.files.at(1).outcome, Outcome::Failed);
    QCOMPARE(batch.files.at(1).filePath, hello);
    QCOMPARE(batch.files.at(1).error, QStringLiteral("Unknown file format"));
    QVERIFY(batch.files.at(1).sessionId.isEmpty());

    QCOMPARE(batch.files.at(2).outcome, Outcome::Failed);
    QCOMPARE(batch.files.at(2).filePath, missing);
    QCOMPARE(batch.files.at(2).error, QStringLiteral("Couldn't read file"));

    QCOMPARE(batch.files.at(3).outcome, Outcome::Created);
    QCOMPARE(batch.files.at(3).filePath, good2);
    QCOMPARE(batch.files.at(3).sessionId, QStringLiteral("two"));

    QCOMPARE(batch.importedSessionIds(), QStringList({"one", "two"}));
    QCOMPARE(batch.failures().size(), 2);
    QCOMPARE(batch.failures().at(0).filePath, hello);
    QCOMPARE(batch.failures().at(1).filePath, missing);

    // One merge call, one model reset per batch
    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(m_model->rowCount(), 2);
    QVERIFY(waitForIdle(*m_model));
}

void ImportBatchTest::progressCancels()
{
    const QString folder = TestEnvironment::instance().newTempDir(QStringLiteral("batch"));
    const QStringList paths = {
        writeBytes(folder, QStringLiteral("a.csv"), Fixtures::trackFile("a").toBytes()),
        writeBytes(folder, QStringLiteral("b.csv"), Fixtures::trackFile("b").toBytes()),
        writeBytes(folder, QStringLiteral("c.csv"), Fixtures::trackFile("c").toBytes()),
    };

    QList<QPair<int, int>> calls;
    const SessionImport::BatchResult batch =
        SessionImport::importFiles(*m_model, paths, [&calls](int current, int total) {
            calls.append(qMakePair(current, total));
            return current < 1;     // cancel before the second file
        });

    QCOMPARE(calls, (QList<QPair<int, int>>({{0, 3}, {1, 3}})));
    QCOMPARE(batch.files.size(), 1);
    QCOMPARE(batch.files.first().outcome, Outcome::Created);
    QCOMPARE(batch.importedSessionIds(), QStringList({"a"}));
    QCOMPARE(m_model->rowCount(), 1);
    QVERIFY(waitForIdle(*m_model));
}

void ImportBatchTest::failureMessageFew()
{
    const QString base = QStringLiteral("C:/cards/flysight");

    // One file: its reason on its line, the hint below the list
    QCOMPARE(SessionImport::failureMessage({firmwareConflict(base + QStringLiteral("/sub/SENSOR.CSV"), "v2024.01.01")},
                                           base),
             QStringLiteral("Import has been completed.\n"
                            "However, some files failed to import:\n"
                            "sub/SENSOR.CSV: Attribute 'FIRMWARE_VER' conflicts with the existing session "
                            "(session: 'v2023.09.22', file: 'v2024.01.01').\n"
                            "\n"
                            "To replace the session, delete it and re-import its files."));

    // Three files, three reasons: one line each, in input order. The hint
    // follows the list although the file it belongs to comes first.
    const QList<MergeResult> failures = {
        firmwareConflict(base + QStringLiteral("/sub/SENSOR.CSV"), "v2024.01.01"),
        failure(base + QStringLiteral("/bad.csv"), QStringLiteral("Unknown file format")),
        failure(base + QStringLiteral("/empty.csv"), QStringLiteral("Empty file")),
    };
    QCOMPARE(SessionImport::failureMessage(failures, base),
             QStringLiteral("Import has been completed.\n"
                            "However, some files failed to import:\n"
                            "sub/SENSOR.CSV: Attribute 'FIRMWARE_VER' conflicts with the existing session "
                            "(session: 'v2023.09.22', file: 'v2024.01.01').\n"
                            "bad.csv: Unknown file format\n"
                            "empty.csv: Empty file\n"
                            "\n"
                            "To replace the session, delete it and re-import its files."));

    // Five failures still use the full listing; one shared reason is given once
    QList<MergeResult> five;
    for (int i = 1; i <= 5; ++i)
        five.append(failure(base + QStringLiteral("/f%1.csv").arg(i), QStringLiteral("Empty file")));
    QCOMPARE(SessionImport::failureMessage(five, base),
             QStringLiteral("Import has been completed.\n"
                            "However, some files failed to import:\n"
                            "Empty file\n"
                            "    f1.csv\n    f2.csv\n    f3.csv\n    f4.csv\n    f5.csv"));
}

void ImportBatchTest::failureMessageMany()
{
    const QString base = QStringLiteral("C:/cards/flysight");

    // Input order is kept: f12 ... f1, not alphabetical
    QList<MergeResult> twelve;
    for (int i = 12; i >= 1; --i)
        twelve.append(failure(base + QStringLiteral("/f%1.csv").arg(i), QStringLiteral("Couldn't read file")));

    QCOMPARE(SessionImport::failureMessage(twelve, base),
             QStringLiteral("Import has been completed.\n"
                            "However, 12 files failed to import.\n"
                            "Failed Files:\n"
                            "Couldn't read file\n"
                            "    f12.csv\n    f11.csv\n    f10.csv\n    f9.csv\n    f8.csv\n"
                            "    f7.csv\n    f6.csv\n    f5.csv\n    f4.csv\n    f3.csv\n"
                            "...and 2 more."));

    // Six to ten failures: the count line, every file, no "more" line. Six
    // conflicts that differ in the file's value are six reasons and ONE hint.
    QList<MergeResult> six;
    for (int i = 1; i <= 6; ++i)
        six.append(firmwareConflict(base + QStringLiteral("/f%1.csv").arg(i), QStringLiteral("v2024.01.0%1").arg(i)));
    const QString sixMessage = SessionImport::failureMessage(six, base);
    QCOMPARE(sixMessage,
             QStringLiteral(
                 "Import has been completed.\n"
                 "However, 6 files failed to import.\n"
                 "Failed Files:\n"
                 "f1.csv: Attribute 'FIRMWARE_VER' conflicts with the existing session (session: 'v2023.09.22', file: 'v2024.01.01').\n"
                 "f2.csv: Attribute 'FIRMWARE_VER' conflicts with the existing session (session: 'v2023.09.22', file: 'v2024.01.02').\n"
                 "f3.csv: Attribute 'FIRMWARE_VER' conflicts with the existing session (session: 'v2023.09.22', file: 'v2024.01.03').\n"
                 "f4.csv: Attribute 'FIRMWARE_VER' conflicts with the existing session (session: 'v2023.09.22', file: 'v2024.01.04').\n"
                 "f5.csv: Attribute 'FIRMWARE_VER' conflicts with the existing session (session: 'v2023.09.22', file: 'v2024.01.05').\n"
                 "f6.csv: Attribute 'FIRMWARE_VER' conflicts with the existing session (session: 'v2023.09.22', file: 'v2024.01.06').\n"
                 "\n"
                 "To replace the session, delete it and re-import its files."));
    QCOMPARE(sixMessage.count(kReplaceHint), 1);
}

// Files with the identical reason are listed under it, where the reason first
// occurred; the cap of ten counts files, not lines.
void ImportBatchTest::failureMessageGroupsSharedReasons()
{
    const QString base = QStringLiteral("C:/cards/flysight");

    // 12 files: a1 (its own reason), then s1..s8 sharing one conflict with
    // b1, b2 in between, then c1
    QList<MergeResult> twelve;
    twelve.append(failure(base + QStringLiteral("/a1.csv"), QStringLiteral("Unknown file format")));
    for (int i = 1; i <= 8; ++i) {
        twelve.append(firmwareConflict(base + QStringLiteral("/s%1.csv").arg(i), "v2024.01.01"));
        if (i == 2)
            twelve.append(failure(base + QStringLiteral("/b1.csv"), QStringLiteral("Empty file")));
        if (i == 5)
            twelve.append(failure(base + QStringLiteral("/b2.csv"), QStringLiteral("Couldn't read file")));
    }
    twelve.append(failure(base + QStringLiteral("/c1.csv"), QStringLiteral("File has no SESSION_ID")));
    QCOMPARE(twelve.size(), 12);

    const QString message = SessionImport::failureMessage(twelve, base);
    QCOMPARE(message,
             QStringLiteral("Import has been completed.\n"
                            "However, 12 files failed to import.\n"
                            "Failed Files:\n"
                            "a1.csv: Unknown file format\n"
                            "Attribute 'FIRMWARE_VER' conflicts with the existing session "
                            "(session: 'v2023.09.22', file: 'v2024.01.01').\n"
                            "    s1.csv\n    s2.csv\n    s3.csv\n    s4.csv\n"
                            "    s5.csv\n    s6.csv\n    s7.csv\n    s8.csv\n"
                            "b1.csv: Empty file\n"
                            "...and 2 more.\n"
                            "\n"
                            "To replace the session, delete it and re-import its files."));
    QCOMPARE(message.count(QStringLiteral("conflicts with")), 1);

    // The cap can fall inside a group
    QList<MergeResult> eleven;
    for (int i = 1; i <= 11; ++i)
        eleven.append(failure(base + QStringLiteral("/f%1.csv").arg(i),
                              i <= 2 ? QStringLiteral("Empty file %1").arg(i) : QStringLiteral("Couldn't read file")));
    QCOMPARE(SessionImport::failureMessage(eleven, base),
             QStringLiteral("Import has been completed.\n"
                            "However, 11 files failed to import.\n"
                            "Failed Files:\n"
                            "f1.csv: Empty file 1\n"
                            "f2.csv: Empty file 2\n"
                            "Couldn't read file\n"
                            "    f3.csv\n    f4.csv\n    f5.csv\n    f6.csv\n"
                            "    f7.csv\n    f8.csv\n    f9.csv\n    f10.csv\n"
                            "...and 1 more."));
}

// Each distinct hint once, in the order first seen, also for files beyond the
// ten that are listed.
void ImportBatchTest::failureMessageHintsOnce()
{
    const QString base = QStringLiteral("C:/cards/flysight");
    const QString schemaConflict =
        QStringLiteral("Attribute 'SCHEMA_VER' conflicts with the existing session (session: '2', file: '1').");

    const QList<MergeResult> mixed = {
        failure(base + QStringLiteral("/s1.csv"), schemaConflict, kSchemaHint),
        firmwareConflict(base + QStringLiteral("/f1.csv"), "v2024.01.01"),
        failure(base + QStringLiteral("/bad.csv"), QStringLiteral("Unknown file format")),
        failure(base + QStringLiteral("/s2.csv"), schemaConflict, kSchemaHint),
        firmwareConflict(base + QStringLiteral("/f2.csv"), "v2024.01.02"),
    };
    const QString message = SessionImport::failureMessage(mixed, base);
    QCOMPARE(message,
             QStringLiteral("Import has been completed.\n"
                            "However, some files failed to import:\n"
                            "Attribute 'SCHEMA_VER' conflicts with the existing session (session: '2', file: '1').\n"
                            "    s1.csv\n"
                            "    s2.csv\n"
                            "f1.csv: Attribute 'FIRMWARE_VER' conflicts with the existing session "
                            "(session: 'v2023.09.22', file: 'v2024.01.01').\n"
                            "bad.csv: Unknown file format\n"
                            "f2.csv: Attribute 'FIRMWARE_VER' conflicts with the existing session "
                            "(session: 'v2023.09.22', file: 'v2024.01.02').\n"
                            "\n"
                            "To change a session's schema version, delete the session and re-import its files.\n"
                            "To replace the session, delete it and re-import its files."));
    QCOMPARE(message.count(kSchemaHint), 1);
    QCOMPARE(message.count(kReplaceHint), 1);

    // The only file with a hint is the twelfth: not listed, its hint is
    QList<MergeResult> twelve;
    for (int i = 1; i <= 11; ++i)
        twelve.append(failure(base + QStringLiteral("/f%1.csv").arg(i), QStringLiteral("Empty file")));
    twelve.append(firmwareConflict(base + QStringLiteral("/f12.csv"), "v2024.01.01"));
    const QString capped = SessionImport::failureMessage(twelve, base);
    QVERIFY(!capped.contains(QStringLiteral("f12.csv")));
    QVERIFY(capped.endsWith(QStringLiteral("    f10.csv\n...and 2 more.\n\n") + kReplaceHint));
}

void ImportBatchTest::failureMessageEmpty()
{
    QCOMPARE(SessionImport::failureMessage({}, QStringLiteral("C:/cards")), QString());
    QCOMPARE(SessionImport::failureMessage({}, QString()), QString());
}

void ImportBatchTest::failureMessageWithoutBaseDir()
{
    // File > Import passes no base directory: the path as given
    QCOMPARE(SessionImport::failureMessage({failure(QStringLiteral("C:/cards/bad.csv"), QStringLiteral("Empty file"))},
                                           QString()),
             QStringLiteral("Import has been completed.\n"
                            "However, some files failed to import:\n"
                            "C:/cards/bad.csv: Empty file"));
}

// End to end: a merge failure (not only a parse failure) reaches the dialog text.
void ImportBatchTest::mergeFailureReachesMessage()
{
    const QString base = TestEnvironment::instance().newTempDir(QStringLiteral("card"));
    QVERIFY(QDir(base).mkpath(QStringLiteral("24-01-01/12-00-00")));
    const QString track = writeBytes(base, QStringLiteral("24-01-01/12-00-00/TRACK.CSV"),
                                     Fixtures::trackFile().toBytes());

    Fs2FileBuilder conflicting;
    conflicting.var("FIRMWARE_VER", "v2024.01.01")
               .var("SESSION_ID", "test-session")
               .var("DEVICE_ID", "test-device")
               .sensor("IMU", {"time", "wx"}, {"s", "deg/s"})
               .row("IMU", "3,62.5");
    const QString sensor = writeBytes(base, QStringLiteral("24-01-01/12-00-00/SENSOR.CSV"), conflicting.toBytes());

    const SessionImport::BatchResult first = SessionImport::importFiles(*m_model, {track});
    QCOMPARE(first.failures().size(), 0);
    QCOMPARE(SessionImport::failureMessage(first.failures(), base), QString());

    const SessionImport::BatchResult second = SessionImport::importFiles(*m_model, {sensor});
    QCOMPARE(second.files.size(), 1);
    QCOMPARE(second.files.first().outcome, Outcome::Failed);
    QVERIFY(second.importedSessionIds().isEmpty());

    QCOMPARE(SessionImport::failureMessage(second.failures(), base),
             QStringLiteral("Import has been completed.\n"
                            "However, some files failed to import:\n"
                            "24-01-01/12-00-00/SENSOR.CSV: Attribute 'FIRMWARE_VER' conflicts with the existing "
                            "session (session: 'v2023.09.22', file: 'v2024.01.01').\n"
                            "\n"
                            "To replace the session, delete it and re-import its files."));
    QCOMPARE(second.files.first().hint, kReplaceHint);
    QCOMPARE(second.files.first().errorWithHint(), second.files.first().error + QLatin1Char(' ') + kReplaceHint);
    QVERIFY(waitForIdle(*m_model));
}

FLYSIGHT_TEST_MAIN(ImportBatchTest)
#include "tst_import_batch.moc"
