// fusion_runner: the sensor fusion fit on one recording, imported as the
// application imports it.
//
// Reads a recording's TRACK.CSV and SENSOR.CSV with the application's parser
// (DataImporter::parseFile), merges them with the application's merge rules
// (SessionMerge) into one SessionData, reads the fit's inputs through the
// same conversion layer and on-demand calculations the application uses
// (the calculation engine with the built-in and fusion calculations
// registered, so a legacy-schema file gets the gyro scale correction), hands
// the kernel the channels the registered calculation would hand it
// (Fusion::channelsFrom, the one assembly), and prints the fit's diagnostics
// JSON on standard output. Progress texts and the outcome go to standard
// error; --csv writes the seventeen output channels; --dump-inputs writes the
// effective input channels the fit was given. Built in the fusion-tests block
// of tests/CMakeLists.txt, not installed; run by hand with the Qt,
// GeographicLib and solver library directories on the path.
//
// No logbook, no preferences, no GUI. Structurally: nothing here references
// the preferences singleton, the logbook manager, the engine's preference
// provider, the session model or the application's import driver, so no
// settings object is ever constructed and the logbook folder is never
// resolved. Defensively: default settings are redirected into a temporary
// directory removed at exit (main()), so a future change of flysight_core
// that constructs one cannot reach the user's settings.
//
// Streams: stdout carries the diagnostics JSON and nothing else once the
// arguments were accepted; stderr carries, in order, any Qt log message as
// one line prefixed "# ", the kernel's progress texts one per line, and the
// final outcome line ("Succeeded", "Rejected: <reason>", "SolverFailed:
// <reason>"). Every line is flushed as it is written, so a long fit streams.
//
// Numbers become text only through CsvFormat (formatDouble,
// formatAttributeValue): shortest round-trip form, '.' decimal point whatever
// the locale, nan/inf for non-finite values, so a --csv file reloads bit for
// bit through CsvFormat::parseDouble.

#include <cstdio>
#include <exception>
#include <utility>

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVariant>
#include <QVector>
#include <QtGlobal>

#include "calculations/builtincalculations.h"
#include "csvformat.h"
#include "dataimporter.h"
#include "engine/calctypes.h"
#include "fusion/fusion.h"
#include "fusion/fusionregistration.h"
#include "parsedfile.h"
#include "sessiondata.h"
#include "sessionmerge.h"

using namespace FlySight;

namespace {

/// The exit status. The usage text (kUsage) repeats these; keep the two in step.
enum ExitCode {
    kExitSucceeded = 0,         ///< the fit converged
    kExitRejected = 1,          ///< the kernel rejected the inputs before the fit started
    kExitSolverFailed = 2,      ///< the fit started and produced no usable solution
    kExitImportFailed = 3,      ///< a file could not be parsed, the files belong to different
                                ///< sessions, or they cannot be merged
    kExitOutputNotWritten = 4,  ///< --csv or --dump-inputs could not be written
    kExitInternal = 5,          ///< a std::exception out of the kernel, or an outcome that cannot occur
    kExitUsage = 64             ///< a usage error, and --help
};

const char kUsage[] =
    "Usage: fusion_runner [options] <folder>\n"
    "       fusion_runner [options] <TRACK.CSV> <SENSOR.CSV>\n"
    "\n"
    "Runs the sensor fusion fit on one recording, imported as FlySight Viewer\n"
    "imports it, and prints the fit's diagnostics JSON on standard output.\n"
    "Progress goes to standard error, one line per stage.\n"
    "\n"
    "Options:\n"
    "  --csv <path>          write the seventeen output channels as CSV (Succeeded only)\n"
    "  --dump-inputs <path>  write the effective input channels the fit is given\n"
    "  -h, --help            this text\n"
    "\n"
    "Exit status: 0 Succeeded, 1 Rejected, 2 SolverFailed, 3 the recording could\n"
    "not be imported, 4 an output file could not be written, 5 internal error,\n"
    "64 usage error.\n";

/// One line on standard error, flushed at once.
void errLine(const QString &text)
{
    QTextStream stream(stderr);
    stream << text << '\n';
    stream.flush();
}

/// Every Qt log message goes to stderr as one line prefixed "# ", so that it
/// can never be mistaken for a progress text and Windows never diverts it to
/// the debugger.
void messageToStderr(QtMsgType, const QMessageLogContext &, const QString &message)
{
    errLine(QStringLiteral("# ") + CsvFormat::singleLine(message));
}

int usageError(const QString &error)
{
    errLine(QStringLiteral("error: ") + error);
    QTextStream stream(stderr);
    stream << kUsage;
    stream.flush();
    return kExitUsage;
}

/// Writes `bytes` as they are (no QIODevice::Text, so LF stays LF on Windows).
bool writeFile(const QString &path, const QByteArray &bytes, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size()) {
        error = QStringLiteral("could not write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

/// The recording as the application's import leaves a new session, without
/// the model: every file parsed on its own, all of them matched by session id,
/// the first file's data as the session and every further file merged in with
/// the model's merge rules. Import-time defaults (applied by the model when a
/// file creates a session) are not applied: none of them is an input of the
/// fit or of a calculation on the fit's input path.
bool importRecording(const QStringList &paths, SessionData &session, QString &error)
{
    QList<ParsedFile> files;
    for (const QString &path : paths) {
        // A fresh importer per file: its error text belongs to this file
        DataImporter importer;
        ParsedFile file;
        if (!importer.parseFile(path, file)) {
            error = path + QStringLiteral(": ") + importer.getLastError();
            return false;
        }
        files.append(std::move(file));
    }

    // The application would create one session per match id, and the fit
    // would never see both files.
    for (qsizetype i = 1; i < files.size(); ++i) {
        if (files.at(i).sessionId != files.at(0).sessionId) {
            error = QStringLiteral("%1 belongs to session '%2', %3 to '%4'")
                        .arg(paths.at(i), files.at(i).sessionId, paths.at(0), files.at(0).sessionId);
            return false;
        }
    }

    session = std::move(files[0].data);
    for (qsizetype i = 1; i < files.size(); ++i) {
        const MergePlan plan = SessionMerge::plan(session, files.at(i).data);
        if (!plan.ok()) {
            error = paths.at(i) + QStringLiteral(": ") + plan.errorWithHint();
            return false;
        }
        if (!plan.isEmpty())
            SessionMerge::apply(session, plan);
    }
    return true;
}

/// The label of one declared input as --dump-inputs writes it.
QString inputLabel(const CalcInput &input)
{
    if (input.kind == CalcInput::Kind::Measurement)
        return input.sensor + QLatin1Char('/') + input.name;
    return input.key;
}

/// --dump-inputs: one line per entry of Fusion::fitInputs(), in that order.
/// A measurement: its label, then every effective sample, comma separated
/// (the label alone when it is unavailable). An attribute: its key, a comma,
/// and the value as a session file would hold it.
QByteArray inputDump(const SessionData &session)
{
    QByteArray text;
    for (const CalcInput &input : Fusion::fitInputs()) {
        text += inputLabel(input).toUtf8();
        if (input.kind == CalcInput::Kind::Measurement) {
            const QVector<double> samples = session.getMeasurement(input.sensor, input.name);
            for (const double sample : samples) {
                text += ',';
                text += CsvFormat::formatDouble(sample);
            }
        } else {
            text += ',';
            text += CsvFormat::formatAttributeValue(session.getAttribute(input.key)).value_or(QString()).toUtf8();
        }
        text += '\n';
    }
    return text;
}

/// --csv: the derived header line, then one line per output sample with the
/// seventeen channels of Fusion::fitOutputChannels(). A plain CSV, not a
/// FlySight file: no $ prefixes, no units line. Empty (with `error` set) when
/// the channels do not share one length, which the kernel's contract rules
/// out for a Succeeded result.
QByteArray outputCsv(const Fusion::Result &result, QString &error)
{
    const QList<Fusion::FitOutputChannel> channels = Fusion::fitOutputChannels(result);
    QStringList names;
    for (const Fusion::FitOutputChannel &channel : channels) {
        names.append(channel.name);
        if (channel.samples.size() != result.time.size()) {
            error = QStringLiteral("output channel %1 has %2 samples, _time has %3")
                        .arg(channel.name).arg(channel.samples.size()).arg(result.time.size());
            return QByteArray();
        }
    }

    QByteArray text = names.join(QLatin1Char(',')).toUtf8();
    text += '\n';
    for (qsizetype row = 0; row < result.time.size(); ++row) {
        for (qsizetype column = 0; column < channels.size(); ++column) {
            if (column > 0)
                text += ',';
            text += CsvFormat::formatDouble(channels.at(column).samples.at(row));
        }
        text += '\n';
    }
    return text;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("FlySightTools"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("tools.flysight.invalid"));
    QCoreApplication::setApplicationName(QStringLiteral("fusion_runner"));
    qInstallMessageHandler(messageToStderr);

    // Tripwire: nothing here constructs a QSettings, and if a future change of
    // flysight_core did, it could land nowhere but in this directory, which is
    // removed when main() returns.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        errLine(QStringLiteral("error: could not create a temporary directory: ") + settingsDir.errorString());
        return kExitInternal;
    }
    const QString settingsPath = settingsDir.path() + QStringLiteral("/settings");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsPath);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsPath);

    // ---- arguments ----------------------------------------------------------
    // parse(), never process(): the streams and the exit codes are this tool's.
    QCommandLineParser parser;
    const QCommandLineOption csvOption(QStringLiteral("csv"), QString(), QStringLiteral("path"));
    const QCommandLineOption dumpOption(QStringLiteral("dump-inputs"), QString(), QStringLiteral("path"));
    // A QStringList, spelled out: two braced QStrings would pick the
    // (name, description) constructor and "help" would be no option at all.
    const QCommandLineOption helpOption(QStringList{QStringLiteral("h"), QStringLiteral("help")});
    parser.addOption(csvOption);
    parser.addOption(dumpOption);
    parser.addOption(helpOption);

    if (!parser.parse(app.arguments()))
        return usageError(parser.errorText());
    if (parser.isSet(helpOption)) {
        QTextStream out(stdout);
        out << kUsage;
        out.flush();
        return kExitUsage;
    }

    // One folder (TRACK.CSV and SENSOR.CSV inside it, the names the device
    // writes) or the two files. A path that does not exist is left to the
    // import, which names it in its error.
    const QStringList positionals = parser.positionalArguments();
    QStringList paths;
    if (positionals.size() == 1) {
        const QString &folder = positionals.first();
        if (QFileInfo(folder).isFile())
            return usageError(folder + QStringLiteral(" is a file; give the recording's folder, or both files"));
        paths = {folder + QStringLiteral("/TRACK.CSV"), folder + QStringLiteral("/SENSOR.CSV")};
    } else if (positionals.size() == 2) {
        for (const QString &path : positionals) {
            if (QFileInfo(path).isDir())
                return usageError(path + QStringLiteral(" is a directory; give one folder, or both files"));
        }
        paths = positionals;
    } else if (positionals.isEmpty()) {
        return usageError(QStringLiteral("no recording given"));
    } else {
        return usageError(QStringLiteral("expected one folder or two files, got %1 arguments").arg(positionals.size()));
    }
    const QString csvPath = parser.value(csvOption);
    const QString dumpPath = parser.value(dumpOption);

    // ---- registration, exactly the application's order ----------------------
    // The session's engine binds to the global registry on first read, so this
    // precedes the import. No preference provider: nothing on the fit's input
    // path declares a preference (tst_fusion_runner::noPreferenceOnTheFitPath).
    FlySight::registerBuiltInCalculations();
    FlySight::Fusion::registerFusionCalculations();

    // ---- import -------------------------------------------------------------
    SessionData session;
    QString error;
    if (!importRecording(paths, session, error)) {
        errLine(QStringLiteral("error: ") + error);
        return kExitImportFailed;
    }

    // ---- the inputs, dumped before the fit whatever the fit will say --------
    if (!dumpPath.isEmpty() && !writeFile(dumpPath, inputDump(session), error)) {
        errLine(QStringLiteral("error: ") + error);
        return kExitOutputNotWritten;
    }

    // ---- the fit ------------------------------------------------------------
    // Effective values through SessionData::getMeasurement / getAttribute are
    // the engine's resolution: what an EvaluationContext is filled from, so
    // this is what computeFit() gets. A missing sensor gives empty channels
    // and the kernel's own rejection; the engine's availability gate is not
    // reproduced here.
    const Fusion::Channels channels = Fusion::channelsFrom(
        [&session](const QString &sensor, const QString &name) { return session.getMeasurement(sensor, name); },
        [&session](const QString &key) { return session.getAttribute(key); });

    Fusion::Result result;
    try {
        // On the main thread, which the build gave a 64 MiB stack; no cancel
        // function (Ctrl-C ends the process).
        result = Fusion::run(channels, [](const QString &text) { errLine(text); }, {});
    } catch (const std::exception &e) {
        // std::bad_alloc: the one thing the kernel lets through
        errLine(QStringLiteral("error: ") + QString::fromUtf8(e.what()));
        return kExitInternal;
    }

    // ---- the diagnostics: the only thing stdout ever carries ----------------
    {
        QTextStream out(stdout);
        out << result.diagnosticsJson << '\n';
        out.flush();
    }

    // ---- outputs and the outcome -------------------------------------------
    switch (result.outcome) {
    case Fusion::Outcome::Succeeded: {
        if (!csvPath.isEmpty()) {
            const QByteArray csv = outputCsv(result, error);
            if (csv.isEmpty()) {
                errLine(QStringLiteral("error: ") + error);
                return kExitInternal;
            }
            if (!writeFile(csvPath, csv, error)) {
                errLine(QStringLiteral("error: ") + error);
                return kExitOutputNotWritten;
            }
        }
        errLine(QStringLiteral("Succeeded"));
        return kExitSucceeded;
    }
    case Fusion::Outcome::Rejected:
        errLine(QStringLiteral("Rejected: ") + result.reason);
        return kExitRejected;
    case Fusion::Outcome::SolverFailed:
        errLine(QStringLiteral("SolverFailed: ") + result.reason);
        return kExitSolverFailed;
    case Fusion::Outcome::Cancelled:
        break;
    }
    errLine(QStringLiteral("error: cancelled without a cancel function"));
    return kExitInternal;
}
