// The command-line runner (tests/fusion_runner.cpp) as a child process on
// fixtures written out as TRACK.CSV / SENSOR.CSV: same diagnostics as a direct
// Fusion::run() on the fixture, same diagnostics as the application's own
// import-and-fit path, CSV round trip, --dump-inputs, the legacy gyro
// correction, exit codes and usage (spec section 8).
//
// Expected values are literals or a direct kernel run on the fixture's arrays
// (on this test's main thread, which has the 64 MiB stack), never a second
// call of the tool. Comparisons go through the golden comparator
// (compareJson / compareSamples): portable by default, bit-exact under
// FLYSIGHT_FUSION_EXACT=1, where this test also runs as tst_fusion_runner_exact.
//
// The child inherits this process's environment: CTest put Qt, GeographicLib
// and the solver runtime on the library search path, which is how the tool
// finds its DLLs. The test never replaces the child's environment.

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QPair>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QtTest>

#include "csvformat.h"
#include "dataexporter.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "fixturebuilder.h"
#include "fusion/fusion.h"
#include "fusion/fusionregistration.h"
#include "fusionfixtures.h"
#include "fusiongolden.h"
#include "fusionsessions.h"
#include "logbookmanager.h"
#include "sessiondata.h"
#include "sessionimport.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

const QString kFusion = QStringLiteral("Fusion");

// The exit codes of the tool (tests/fusion_runner.cpp, enum ExitCode)
const int kExitSucceeded = 0, kExitRejected = 1, kExitSolverFailed = 2, kExitImportFailed = 3,
          kExitInternal = 5, kExitUsage = 64;

/// One run of the tool.
struct ToolRun {
    int exitCode = -1;
    QByteArray stdoutBytes;
    QStringList stderrLines;    ///< every line, CR stripped; "# " lines kept
    bool normalExit = false;
    QString startError;         ///< non-empty when the process did not start or finish

    /// For a failure message: the status and the whole stderr.
    QString describe() const
    {
        return QStringLiteral("exit %1 (%2)%3\nstderr:\n%4")
            .arg(exitCode)
            .arg(normalExit ? QStringLiteral("normal") : QStringLiteral("crashed or not run"))
            .arg(startError.isEmpty() ? QString() : QStringLiteral(" ") + startError)
            .arg(stderrLines.join(QLatin1Char('\n')));
    }
};

QStringList splitLines(const QByteArray &bytes)
{
    QStringList lines;
    for (QByteArray line : bytes.split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        lines.append(QString::fromUtf8(line));
    }
    // The text ends with a newline: the piece after it is not a line
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines;
}

ToolRun runTool(const QStringList &arguments)
{
    ToolRun run;
    QProcess process;
    process.setProgram(QStringLiteral(FLYSIGHT_FUSION_RUNNER));
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(30000)) {
        run.startError = QStringLiteral("did not start: ") + process.errorString();
        return run;
    }
    if (!process.waitForFinished(300000)) {
        run.startError = QStringLiteral("did not finish within 300 s");
        process.kill();
        process.waitForFinished(5000);
        return run;
    }
    run.normalExit = process.exitStatus() == QProcess::NormalExit;
    run.exitCode = process.exitCode();
    run.stdoutBytes = process.readAllStandardOutput();
    run.stderrLines = splitLines(process.readAllStandardError());
    return run;
}

/// A copy of `session`'s stored state restricted to `sensors` (every stored
/// attribute, the source columns of those sensors), as tst_fusion_session's
/// withoutSensor() builds a part.
SessionData partWith(const SessionData &session, const QStringList &sensors)
{
    SessionData part;
    for (const QString &key : session.attributeKeys())
        part.setAttribute(key, session.storedAttribute(key));
    const SourceData source = session.sourceData();
    SourceData kept;
    for (const QString &sensor : sensors) {
        if (source.contains(sensor))
            kept.insert(sensor, source.value(sensor));
    }
    part.mergeSourceData(kept);
    return part;
}

/// The session as a recording: <dir>/TRACK.CSV holds the stored attributes
/// plus the GNSS and Local sensors, <dir>/SENSOR.CSV the same attributes plus
/// IMU and TIME. Both carry SESSION_ID, DEVICE_ID and (unless removed)
/// SCHEMA_VER, so the match ids agree and the merge has no conflict. Empty on
/// success, else what went wrong.
QString writeRecording(const SessionData &session, const QString &dir)
{
    QString error;
    if (!DataExporter::exportSession(dir + QStringLiteral("/TRACK.CSV"),
                                     partWith(session, {QStringLiteral("GNSS"), QStringLiteral("Local")}), &error))
        return QStringLiteral("TRACK.CSV: ") + error;
    if (!DataExporter::exportSession(dir + QStringLiteral("/SENSOR.CSV"),
                                     partWith(session, {QStringLiteral("IMU"), QStringLiteral("TIME")}), &error))
        return QStringLiteral("SENSOR.CSV: ") + error;
    return QString();
}

/// The tool's stdout as a JSON object. Empty on success; else the parse error
/// with the raw bytes.
QString jsonOf(const QByteArray &stdoutBytes, QJsonObject &object)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(stdoutBytes, &parseError);
    if (document.isNull() || !document.isObject()) {
        return QStringLiteral("stdout is not a JSON object (%1): '%2'")
            .arg(parseError.errorString(), QString::fromUtf8(stdoutBytes));
    }
    object = document.object();
    return QString();
}

QJsonObject jsonOf(const QString &compactJson)
{
    return QJsonDocument::fromJson(compactJson.toUtf8()).object();
}

/// A --csv file: the header names and the columns, each field through
/// CsvFormat::parseDouble.
bool readCsv(const QString &path, QStringList &header, QHash<QString, QVector<double>> &columns)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QStringList lines = splitLines(file.readAll());
    if (lines.isEmpty())
        return false;
    header = lines.first().split(QLatin1Char(','));
    columns.clear();
    for (qsizetype row = 1; row < lines.size(); ++row) {
        const QStringList fields = lines.at(row).split(QLatin1Char(','));
        if (fields.size() != header.size())
            return false;
        for (qsizetype column = 0; column < header.size(); ++column) {
            double value = 0;
            if (!CsvFormat::parseDouble(fields.at(column), &value))
                return false;
            columns[header.at(column)].append(value);
        }
    }
    return true;
}

/// A --dump-inputs file: per line the label before the first comma and the
/// rest split on commas (empty when the label stands alone).
bool readDump(const QString &path, QList<QPair<QString, QStringList>> &lines)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    lines.clear();
    for (const QString &line : splitLines(file.readAll())) {
        const qsizetype comma = line.indexOf(QLatin1Char(','));
        if (comma < 0)
            lines.append({line, QStringList()});
        else
            lines.append({line.left(comma), line.mid(comma + 1).split(QLatin1Char(','))});
    }
    return true;
}

/// The values of the dump line labelled `label`, parsed; false when there is
/// no such line or a field does not parse.
bool dumpSamples(const QList<QPair<QString, QStringList>> &lines, const QString &label, QVector<double> &samples)
{
    for (const auto &line : lines) {
        if (line.first != label)
            continue;
        samples.clear();
        for (const QString &field : line.second) {
            double value = 0;
            if (!CsvFormat::parseDouble(field, &value))
                return false;
            samples.append(value);
        }
        return true;
    }
    return false;
}

/// fitInputs() rendered as the dump labels them.
QStringList inputLabels()
{
    QStringList labels;
    for (const CalcInput &input : Fusion::fitInputs()) {
        labels.append(input.kind == CalcInput::Kind::Measurement
                          ? input.sensor + QLatin1Char('/') + input.name
                          : input.key);
    }
    return labels;
}

QStringList dumpLabels(const QList<QPair<QString, QStringList>> &lines)
{
    QStringList labels;
    for (const auto &line : lines)
        labels.append(line.first);
    return labels;
}

/// The first channel of `columns` that differs from `result`'s; empty when
/// all seventeen match bit for bit.
QString firstColumnNotBitIdentical(const QHash<QString, QVector<double>> &columns, const Fusion::Result &result)
{
    for (const QString &name : fusionChannelNames()) {
        if (!sameBitsEverywhere(columns.value(name), fusionChannel(result, name)))
            return name;
    }
    return QString();
}

int exitCodeOf(Fusion::Outcome outcome)
{
    switch (outcome) {
    case Fusion::Outcome::Succeeded: return kExitSucceeded;
    case Fusion::Outcome::Rejected: return kExitRejected;
    case Fusion::Outcome::SolverFailed: return kExitSolverFailed;
    case Fusion::Outcome::Cancelled: break;
    }
    return kExitInternal;
}

} // namespace

class FusionRunnerTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void noPreferenceOnTheFitPath();
    void outputTableMatchesGolden();
    void successMatchesDirectRun();
    void matchesTheApplicationImportPath();
    void rejectionExitsOne();
    void legacySchemaScalesTheGyro();
    void usageAndImportFailures();

private:
    QStringList m_registryBefore;
};

void FusionRunnerTest::initTestCase()
{
    // As the application does: the built-ins, then sensor fusion
    TestEnvironment::instance().registerBuiltIns();
    registerFusionOnce();
    QVERIFY2(QFileInfo::exists(QStringLiteral(FLYSIGHT_FUSION_RUNNER)), FLYSIGHT_FUSION_RUNNER);
}

void FusionRunnerTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();
    m_registryBefore = CalculationRegistry::instance().registeredIds();
}

// Every SessionModel is a local of its test function, destroyed before this
// runs; a session that were still alive would show as an enrolled engine.
void FusionRunnerTest::cleanup()
{
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

// The premise of the tool's model-free path: it installs no preference
// provider, so nothing on the fit's input path may declare a preference. If a
// future calculation on the path does, this fails first: revisit the runner's
// registration (tests/fusion_runner.cpp) before anything else.
void FusionRunnerTest::noPreferenceOnTheFitPath()
{
    const CalculationRegistry &registry = CalculationRegistry::instance();
    const QSet<QString> timePreferences =
        registry.staticDependencies(DependencyKey::measurement(kFusion, QStringLiteral("_time"))).preferences;
    QVERIFY2(timePreferences.isEmpty(), qPrintable(QStringList(timePreferences.values()).join(QLatin1Char(','))));
    const QSet<QString> rollPreferences =
        registry.staticDependencies(DependencyKey::measurement(kFusion, QStringLiteral("roll"))).preferences;
    QVERIFY2(rollPreferences.isEmpty(), qPrintable(QStringList(rollPreferences.values()).join(QLatin1Char(','))));
}

// The output table the tool derives its CSV from is the goldens' column list,
// each entry sharing the buffer of the corresponding Result array.
void FusionRunnerTest::outputTableMatchesGolden()
{
    const Fusion::Result result = Fusion::run(toChannels(fusionFixture(QStringLiteral("coarse_maneuver"))));
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));

    const QList<Fusion::FitOutputChannel> channels = Fusion::fitOutputChannels(result);
    QCOMPARE(channels.size(), 17);
    QStringList names;
    for (const Fusion::FitOutputChannel &channel : channels)
        names.append(channel.name);
    QCOMPARE(names, fusionChannelNames());
    for (const Fusion::FitOutputChannel &channel : channels)
        QVERIFY2(sameBitsEverywhere(channel.samples, fusionChannel(result, channel.name)), qPrintable(channel.name));
}

// A fixture session written as a recording stores Local/*, IMU/_time, the
// origin attributes and SCHEMA_VER=2, so the imported effective inputs are
// the fixture bit for bit (tst_fusion_session::inputsAreBitIdenticalToFixture):
// the tool's every output must then equal a direct run on the fixture.
void FusionRunnerTest::successMatchesDirectRun()
{
    const FusionFixture fixture = fusionFixture(QStringLiteral("coarse_maneuver"));
    const QString dir = TestEnvironment::instance().newTempDir(QStringLiteral("recording"));
    QCOMPARE(writeRecording(sessionFromFixture(fixture, QStringLiteral("f1")), dir), QString());

    const QString csvPath = dir + QStringLiteral("/out.csv");
    const QString dumpPath = dir + QStringLiteral("/in.txt");
    const ToolRun run = runTool({dir, QStringLiteral("--csv"), csvPath, QStringLiteral("--dump-inputs"), dumpPath});
    QVERIFY2(run.normalExit, qPrintable(run.describe()));
    QVERIFY2(run.exitCode == kExitSucceeded, qPrintable(run.describe()));

    QStringList progress;
    const Fusion::Result direct = Fusion::run(toChannels(fixture),
                                              [&progress](const QString &text) { progress.append(text); });
    QVERIFY2(direct.outcome == Fusion::Outcome::Succeeded, qPrintable(direct.reason));

    // stdout: the diagnostics, and nothing else
    QJsonObject diagnostics;
    QVERIFY2(jsonOf(run.stdoutBytes, diagnostics).isEmpty(), qPrintable(jsonOf(run.stdoutBytes, diagnostics)));
    const QString difference = compareJson(QStringLiteral("diagnostics"), diagnostics, jsonOf(direct.diagnosticsJson));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    // stderr: exactly the kernel's progress texts, then the outcome (no "# "
    // line: a Qt warning on the headless import path is a defect of the path)
    QVERIFY2(!run.stderrLines.isEmpty(), qPrintable(run.describe()));
    QVERIFY2(run.stderrLines.last() == QStringLiteral("Succeeded"), qPrintable(run.describe()));
    QVERIFY2(run.stderrLines.mid(0, run.stderrLines.size() - 1) == progress, qPrintable(run.describe()));

    // --csv: derived header, one row per output sample, bit-identical columns
    QStringList header;
    QHash<QString, QVector<double>> columns;
    QVERIFY2(readCsv(csvPath, header, columns), qPrintable(csvPath));
    QCOMPARE(header.join(QLatin1Char(',')), fusionChannelNames().join(QLatin1Char(',')));
    QCOMPARE(columns.value(QStringLiteral("_time")).size(), direct.time.size());
    QVERIFY2(firstColumnNotBitIdentical(columns, direct).isEmpty(),
             qPrintable(QStringLiteral("column %1 differs from the direct run").arg(firstColumnNotBitIdentical(columns, direct))));

    // --dump-inputs: the table of fitInputs(), holding the fixture bit for bit
    QList<QPair<QString, QStringList>> dump;
    QVERIFY2(readDump(dumpPath, dump), qPrintable(dumpPath));
    QCOMPARE(dumpLabels(dump), inputLabels());
    QCOMPARE(dump.size(), 21);
    QVector<double> samples;
    QVERIFY(dumpSamples(dump, QStringLiteral("GNSS/_time"), samples));
    QVERIFY(sameBitsEverywhere(samples, fixture.gnssTime));
    QVERIFY(dumpSamples(dump, QStringLiteral("Local/north"), samples));
    QVERIFY(sameBitsEverywhere(samples, fixture.north));
    QVERIFY(dumpSamples(dump, QStringLiteral("IMU/wx"), samples));
    QVERIFY(sameBitsEverywhere(samples, fixture.wx));
    QVERIFY(dumpSamples(dump, QStringLiteral("_LOCAL_ORIGIN_INDEX"), samples));
    QCOMPARE(samples, QVector<double>{double(fixture.originIndex)});
    QVERIFY(dumpSamples(dump, QStringLiteral("_LOCAL_ORIGIN_LAT"), samples));
    QCOMPARE(samples.size(), 1);
    QVERIFY(sameBits(samples.first(), fixture.originLat));
}

// "Imports exactly as the application does": for a recording as the importer
// leaves it (GNSS lat/lon/hMSL, IMU device time, TIME pulses; the local frame
// and the time fit are calculated), the tool's diagnostics and channels equal
// what SessionImport + SessionModel (creation defaults, logbook, column worker
// and all) + the engine's explicit request produce for the same two files.
void FusionRunnerTest::matchesTheApplicationImportPath()
{
    const QString dir = TestEnvironment::instance().newTempDir(QStringLiteral("recording"));
    QCOMPARE(writeRecording(naturalSession(QStringLiteral("n1")), dir), QString());
    const QString track = dir + QStringLiteral("/TRACK.CSV");
    const QString sensor = dir + QStringLiteral("/SENSOR.CSV");
    const QString csvPath = dir + QStringLiteral("/out.csv");

    // The child, in the two-path form
    const ToolRun run = runTool({track, sensor, QStringLiteral("--csv"), csvPath});
    QVERIFY2(run.normalExit, qPrintable(run.describe()));
    QVERIFY2(run.exitCode == kExitSucceeded, qPrintable(run.describe()));
    QJsonObject diagnostics;
    QVERIFY2(jsonOf(run.stdoutBytes, diagnostics).isEmpty(), qPrintable(jsonOf(run.stdoutBytes, diagnostics)));
    QStringList header;
    QHash<QString, QVector<double>> columns;
    QVERIFY2(readCsv(csvPath, header, columns), qPrintable(csvPath));

    // The application's path, in this process
    {
        SessionModel model;
        const SessionImport::BatchResult imported = SessionImport::importFiles(model, {track, sensor});
        QVERIFY2(imported.failures().isEmpty(), qPrintable(SessionImport::failureMessage(imported.failures(), dir)));
        QCOMPARE(imported.importedSessionIds().size(), 1);
        const SessionData *session = model.loadedSession(imported.importedSessionIds().first());
        QVERIFY(session != nullptr);
        const CalculationEngine::RequestOutcome request =
            session->calculationEngine().request(QString::fromLatin1(Fusion::FitCalculationId));
        QVERIFY(request.found);
        QVERIFY2(request.status == ResultStatus::Ok, qPrintable(QString::number(int(request.status))));
        QVERIFY(waitForIdle(model));

        const QString difference = compareJson(
            QStringLiteral("diagnostics"), diagnostics,
            jsonOf(session->getAttribute(SessionKeys::FusionDiagnostics).toString()));
        QVERIFY2(difference.isEmpty(), qPrintable(difference));
        for (const QString &name : fusionChannelNames()) {
            const QString channelDifference =
                compareSamples(name, columns.value(name), session->getMeasurement(kFusion, name));
            QVERIFY2(channelDifference.isEmpty(), qPrintable(channelDifference));
        }
    }
}

// A rejection is a result: the failure JSON on stdout, the reason on stderr,
// exit 1, and no CSV file.
void FusionRunnerTest::rejectionExitsOne()
{
    const FusionFixture fixture = fusionFixture(QStringLiteral("reject_too_few_fixes"));
    QVERIFY(!fixture.expectSuccess);
    const QString dir = TestEnvironment::instance().newTempDir(QStringLiteral("recording"));
    QCOMPARE(writeRecording(sessionFromFixture(fixture, QStringLiteral("f1")), dir), QString());
    const QString csvPath = dir + QStringLiteral("/out.csv");

    const ToolRun run = runTool({dir, QStringLiteral("--csv"), csvPath});
    QVERIFY2(run.normalExit, qPrintable(run.describe()));
    QVERIFY2(run.exitCode == kExitRejected, qPrintable(run.describe()));

    const Fusion::Result direct = Fusion::run(toChannels(fixture));
    QVERIFY(direct.outcome == Fusion::Outcome::Rejected);
    QJsonObject diagnostics;
    QVERIFY2(jsonOf(run.stdoutBytes, diagnostics).isEmpty(), qPrintable(jsonOf(run.stdoutBytes, diagnostics)));
    QCOMPARE(diagnostics.keys(), QStringList({QStringLiteral("algorithm"), QStringLiteral("failure")}));
    const QString difference = compareJson(QStringLiteral("diagnostics"), diagnostics, jsonOf(direct.diagnosticsJson));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QVERIFY2(!run.stderrLines.isEmpty(), qPrintable(run.describe()));
    QCOMPARE(run.stderrLines.last(), QStringLiteral("Rejected: ") + direct.reason);
    QVERIFY(!QFileInfo::exists(csvPath));
}

// A recording without SCHEMA_VER is legacy data: the conversion layer
// multiplies the gyro channels by the legacy scale at read time, and the tool
// reads effective values, so its dump shows the scaled gyro (exactly one
// multiply per sample) while everything else stays the fixture; its fit is the
// fit of the scaled channels. Both files must lack the attribute: the merge
// would add it from the other.
void FusionRunnerTest::legacySchemaScalesTheGyro()
{
    const FusionFixture fixture = fusionFixture(QStringLiteral("coarse_maneuver"));
    SessionData session = sessionFromFixture(fixture, QStringLiteral("f1"));
    session.removeAttribute(QStringLiteral("SCHEMA_VER"));
    const QString dir = TestEnvironment::instance().newTempDir(QStringLiteral("recording"));
    QCOMPARE(writeRecording(session, dir), QString());
    const QString dumpPath = dir + QStringLiteral("/in.txt");

    const ToolRun run = runTool({dir, QStringLiteral("--dump-inputs"), dumpPath});
    QVERIFY2(run.normalExit, qPrintable(run.describe()));

    Fusion::Channels scaled = toChannels(fixture);
    for (QVector<double> *gyro : {&scaled.wx, &scaled.wy, &scaled.wz}) {
        for (double &sample : *gyro)
            sample = sample * 1.14688;
    }

    QList<QPair<QString, QStringList>> dump;
    QVERIFY2(readDump(dumpPath, dump), qPrintable(dumpPath));
    QVector<double> samples;
    QVERIFY(dumpSamples(dump, QStringLiteral("IMU/wx"), samples));
    QVERIFY(sameBitsEverywhere(samples, scaled.wx));
    QVERIFY(dumpSamples(dump, QStringLiteral("IMU/wy"), samples));
    QVERIFY(sameBitsEverywhere(samples, scaled.wy));
    QVERIFY(dumpSamples(dump, QStringLiteral("IMU/wz"), samples));
    QVERIFY(sameBitsEverywhere(samples, scaled.wz));
    QVERIFY(!sameBitsEverywhere(samples, fixture.wz));   // the scale is not the identity here
    QVERIFY(dumpSamples(dump, QStringLiteral("IMU/ax"), samples));
    QVERIFY(sameBitsEverywhere(samples, fixture.ax));
    QVERIFY(dumpSamples(dump, QStringLiteral("GNSS/_time"), samples));
    QVERIFY(sameBitsEverywhere(samples, fixture.gnssTime));

    // The fit of the scaled channels, whatever its outcome
    const Fusion::Result direct = Fusion::run(scaled);
    QVERIFY2(run.exitCode == exitCodeOf(direct.outcome), qPrintable(run.describe()));
    QJsonObject diagnostics;
    QVERIFY2(jsonOf(run.stdoutBytes, diagnostics).isEmpty(), qPrintable(jsonOf(run.stdoutBytes, diagnostics)));
    const QString difference = compareJson(QStringLiteral("diagnostics"), diagnostics, jsonOf(direct.diagnosticsJson));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

// Usage errors and --help exit 64; anything the import refuses exits 3 with
// the file and the importer's or the merge's own words.
void FusionRunnerTest::usageAndImportFailures()
{
    const QString dir = TestEnvironment::instance().newTempDir(QStringLiteral("usage"));

    {
        const ToolRun help = runTool({QStringLiteral("--help")});
        QVERIFY2(help.normalExit && help.exitCode == kExitUsage, qPrintable(help.describe()));
        QVERIFY2(help.stdoutBytes.contains("Usage:"), help.stdoutBytes.constData());
    }
    {
        const ToolRun none = runTool({});
        QVERIFY2(none.normalExit && none.exitCode == kExitUsage, qPrintable(none.describe()));
        const QString stderrText = none.stderrLines.join(QLatin1Char('\n'));
        QVERIFY2(stderrText.contains(QStringLiteral("error:")), qPrintable(none.describe()));
        QVERIFY2(stderrText.contains(QStringLiteral("Usage:")), qPrintable(none.describe()));
        QVERIFY(none.stdoutBytes.isEmpty());
    }
    {
        const ToolRun bogus = runTool({QStringLiteral("--bogus"), dir});
        QVERIFY2(bogus.normalExit && bogus.exitCode == kExitUsage, qPrintable(bogus.describe()));
        QVERIFY2(bogus.stderrLines.join(QLatin1Char('\n')).contains(QStringLiteral("error:")), qPrintable(bogus.describe()));
    }
    {
        const ToolRun three = runTool({dir, dir, dir});
        QVERIFY2(three.normalExit && three.exitCode == kExitUsage, qPrintable(three.describe()));
    }
    {
        const QString missing = dir + QStringLiteral("/no-such-recording");
        const ToolRun run = runTool({missing});
        QVERIFY2(run.normalExit && run.exitCode == kExitImportFailed, qPrintable(run.describe()));
        QVERIFY2(run.stderrLines.join(QLatin1Char('\n')).contains(QStringLiteral("TRACK.CSV")), qPrintable(run.describe()));
    }
    {
        // A valid TRACK.CSV next to a SENSOR.CSV without a $DATA section
        const QString recording = dir + QStringLiteral("/no-data-section");
        QVERIFY(QDir().mkpath(recording));
        QVERIFY(Fixtures::trackFile("x").write(recording + QStringLiteral("/TRACK.CSV")));
        QVERIFY(writeFile(recording + QStringLiteral("/SENSOR.CSV"), "$FLYS,1\n$VAR,SESSION_ID,x\n$COL,IMU,time,wx\n"));
        const ToolRun run = runTool({recording});
        QVERIFY2(run.normalExit && run.exitCode == kExitImportFailed, qPrintable(run.describe()));
        const QString stderrText = run.stderrLines.join(QLatin1Char('\n'));
        QVERIFY2(stderrText.contains(QStringLiteral("Missing $DATA section")), qPrintable(run.describe()));
        QVERIFY2(stderrText.contains(QStringLiteral("SENSOR.CSV")), qPrintable(run.describe()));
    }
    {
        // Two files of two sessions
        const QString track = dir + QStringLiteral("/a-TRACK.CSV");
        const QString sensor = dir + QStringLiteral("/b-SENSOR.CSV");
        QVERIFY(Fixtures::trackFile("a").write(track));
        QVERIFY(Fixtures::sensorFile("b").write(sensor));
        const ToolRun run = runTool({track, sensor});
        QVERIFY2(run.normalExit && run.exitCode == kExitImportFailed, qPrintable(run.describe()));
        const QString stderrText = run.stderrLines.join(QLatin1Char('\n'));
        QVERIFY2(stderrText.contains(QStringLiteral("'a'")), qPrintable(run.describe()));
        QVERIFY2(stderrText.contains(QStringLiteral("'b'")), qPrintable(run.describe()));
    }
}

FLYSIGHT_TEST_MAIN(FusionRunnerTest)
#include "tst_fusion_runner.moc"
