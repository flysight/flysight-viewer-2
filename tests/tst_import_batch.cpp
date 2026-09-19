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

MergeResult failure(const QString &path, const QString &error)
{
    MergeResult result;
    result.filePath = path;
    result.outcome = Outcome::Failed;
    result.error = error;
    return result;
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
    const QList<MergeResult> failures = {
        failure(base + QStringLiteral("/sub/SENSOR.CSV"),
                QStringLiteral("Attribute 'FIRMWARE_VER' conflicts with the existing session "
                               "(session: 'v2023.09.22', file: 'v2024.01.01'). "
                               "To replace the session, delete it and re-import its files.")),
        failure(base + QStringLiteral("/bad.csv"), QStringLiteral("Unknown file format")),
    };

    QCOMPARE(SessionImport::failureMessage(failures, base),
             QStringLiteral("Import has been completed.\n"
                            "However, some files failed to import:\n"
                            "sub/SENSOR.CSV: Attribute 'FIRMWARE_VER' conflicts with the existing session "
                            "(session: 'v2023.09.22', file: 'v2024.01.01'). "
                            "To replace the session, delete it and re-import its files.\n"
                            "bad.csv: Unknown file format"));

    // Five failures still use the full listing
    QList<MergeResult> five;
    for (int i = 1; i <= 5; ++i)
        five.append(failure(base + QStringLiteral("/f%1.csv").arg(i), QStringLiteral("Empty file")));
    QCOMPARE(SessionImport::failureMessage(five, base),
             QStringLiteral("Import has been completed.\n"
                            "However, some files failed to import:\n"
                            "f1.csv: Empty file\nf2.csv: Empty file\nf3.csv: Empty file\n"
                            "f4.csv: Empty file\nf5.csv: Empty file"));
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
                            "f12.csv: Couldn't read file\nf11.csv: Couldn't read file\n"
                            "f10.csv: Couldn't read file\nf9.csv: Couldn't read file\n"
                            "f8.csv: Couldn't read file\nf7.csv: Couldn't read file\n"
                            "f6.csv: Couldn't read file\nf5.csv: Couldn't read file\n"
                            "f4.csv: Couldn't read file\nf3.csv: Couldn't read file\n"
                            "...and 2 more."));

    // Six to ten failures: the count line, every file, no "more" line
    QList<MergeResult> six;
    for (int i = 1; i <= 6; ++i)
        six.append(failure(base + QStringLiteral("/f%1.csv").arg(i), QStringLiteral("Empty file")));
    QCOMPARE(SessionImport::failureMessage(six, base),
             QStringLiteral("Import has been completed.\n"
                            "However, 6 files failed to import.\n"
                            "Failed Files:\n"
                            "f1.csv: Empty file\nf2.csv: Empty file\nf3.csv: Empty file\n"
                            "f4.csv: Empty file\nf5.csv: Empty file\nf6.csv: Empty file"));
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
                            "session (session: 'v2023.09.22', file: 'v2024.01.01'). "
                            "To replace the session, delete it and re-import its files."));
    QVERIFY(waitForIdle(*m_model));
}

FLYSIGHT_TEST_MAIN(ImportBatchTest)
#include "tst_import_batch.moc"
