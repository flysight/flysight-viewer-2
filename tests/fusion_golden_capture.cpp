// fusion_golden_capture: the fusion goldens, captured from the product kernel.
//
// Runs the kernel on each of the twelve synthetic fixtures of
// tests/fusion/fusionfixtures.cpp and writes <fixture>.json,
// <fixture>.channels.txt and capture.json into the output directory, in
// exactly the formats tests/fusion/fusiongolden.cpp reads (tests/README.md,
// section 11). Each fixture is run through Detail::runPipeline() with the
// production Tuning - what Fusion::run() does - with a progress-collecting
// Checkpoint and a PipelineTrace, so one run yields the result, the progress
// texts and the trace. The kernel is pure: the tool touches nothing but the
// output directory (no settings, no logbook, no TestEnvironment).
//
// Not a test and not installed. Built in the FLYSIGHT_BUILD_FUSION_TESTS block
// of tests/CMakeLists.txt; run by hand with the Qt, GTSAM and oneTBB bin
// directories on PATH (the re-capture procedure of tests/README.md).
//
//   usage: fusion_golden_capture [--revision <text>] [<output-directory>]
//
// The output directory defaults to tests/data/fusion of this source tree;
// --revision is the repository revision recorded in capture.json (the
// procedure passes `git rev-parse HEAD`; without it null is recorded).
//
// Every fixture is captured twice in this process and the bytes compared
// before anything of it is written: a difference is fatal.
//
// Exit status: 0 ok; 1 usage or I/O error; 2 at least one fixture's outcome
// was not the expected one (the files are still written, so that they can be
// looked at; such a capture is never committed); 3 the two captures of a
// fixture differed (nothing further is written).

#include <iostream>
#include <string>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QSysInfo>

#include <gtsam/config.h>

#include "fusion/fusionpipeline.h"
#include "fusionfixtures.h"
#include "fusiongolden.h"
#include "fusiontrace.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

const int kExitOk = 0, kExitUsage = 1, kExitUnexpected = 2, kExitNondeterministic = 3;

/// One run of the kernel on one fixture, serialized.
struct Capture {
    QString outcome;        ///< "succeeded", "rejected" or "solver_failed"
    QString failure;        ///< the reason of the last two
    int rows = 0;           ///< succeeded: output samples
    double objective = 0;   ///< succeeded: the diagnostics' objective
    QByteArray json;        ///< <fixture>.json
    QByteArray channels;    ///< <fixture>.channels.txt; empty unless succeeded
};

QString outcomeName(Fusion::Outcome outcome)
{
    switch (outcome) {
    case Fusion::Outcome::Succeeded: return QStringLiteral("succeeded");
    case Fusion::Outcome::Rejected: return QStringLiteral("rejected");
    case Fusion::Outcome::SolverFailed: return QStringLiteral("solver_failed");
    case Fusion::Outcome::Cancelled: break;   // nobody asks to cancel
    }
    return QStringLiteral("cancelled");
}

Capture captureOnce(const FusionFixture &fixture)
{
    // Exactly Fusion::run(channels, progress): runPipeline with the production
    // Tuning, plus the trace seam. runPipeline never throws for these inputs
    // (every std::exception becomes a result); std::bad_alloc may propagate
    // and end the tool.
    QStringList progress;
    Fusion::Detail::PipelineTrace trace;
    const Fusion::Detail::Checkpoint checkpoint(
        [&progress](const QString &text) { progress.append(text); }, {});
    const Fusion::Result result =
        Fusion::Detail::runPipeline(toChannels(fixture), Fusion::Detail::Tuning{}, checkpoint, &trace);

    Capture capture;
    capture.outcome = outcomeName(result.outcome);
    capture.failure = result.reason;

    // The compact diagnostics string parsed and written indented: the golden
    // holds the same numbers in shortest round-trip form.
    const QJsonObject diagnostics = QJsonDocument::fromJson(result.diagnosticsJson.toUtf8()).object();
    QJsonObject golden{{QStringLiteral("fixture"), fixture.name},
                       {QStringLiteral("outcome"), capture.outcome},
                       {QStringLiteral("diagnostics"), diagnostics},
                       {QStringLiteral("progress"), QJsonArray::fromStringList(progress)}};
    if (result.outcome == Fusion::Outcome::Succeeded) {
        capture.rows = int(result.time.size());
        capture.objective = diagnostics.value(QStringLiteral("objective")).toDouble();
        golden[QStringLiteral("trace")] = traceJson(trace);
        golden[QStringLiteral("rows")] = capture.rows;
        golden[QStringLiteral("channels_file")] = fixture.name + QStringLiteral(".channels.txt");
        capture.channels = fusionChannelsText(result);
    }
    // Qt sorts the keys and indents four spaces: the committed layout.
    capture.json = QJsonDocument(golden).toJson(QJsonDocument::Indented);
    return capture;
}

QString sha256(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

/// Writes `bytes` as they are: no QIODevice::Text, so LF stays LF on Windows.
bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size()) {
        std::cerr << "cannot write " << path.toStdString() << ": "
                  << file.errorString().toStdString() << std::endl;
        return false;
    }
    return true;
}

/// SHA-256 of a source file with every CRLF replaced by LF, so a checkout with
/// core.autocrlf=true hashes the same as the committed blob.
bool hashSourceFile(const QString &path, QString *hash)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "cannot read " << path.toStdString() << ": "
                  << file.errorString().toStdString() << std::endl;
        return false;
    }
    QByteArray bytes = file.readAll();
    bytes.replace("\r\n", "\n");
    *hash = sha256(bytes);
    return true;
}

QJsonObject provenance(const QJsonValue &revision, const QMap<QString, QString> &fileHashes,
                       const QMap<QString, QString> &generatorHashes)
{
    QJsonObject compiler{{QStringLiteral("id"), QStringLiteral(FLYSIGHT_CAPTURE_COMPILER_ID)},
                         {QStringLiteral("version"), QStringLiteral(FLYSIGHT_CAPTURE_COMPILER_VERSION)},
                         {QStringLiteral("generator"), QStringLiteral(FLYSIGHT_CAPTURE_GENERATOR)},
                         {QStringLiteral("platform"), QStringLiteral(FLYSIGHT_CAPTURE_PLATFORM)},
                         {QStringLiteral("configuration"), QStringLiteral(FLYSIGHT_CAPTURE_CONFIGURATION)},
                         {QStringLiteral("cmake"), QStringLiteral(FLYSIGHT_CAPTURE_CMAKE_VERSION)}};
    // The configure-time gate of the exact tests (tests/CMakeLists.txt) reads
    // "cl_version" and compares its major.minor with the configuring compiler.
    if (QLatin1String(FLYSIGHT_CAPTURE_COMPILER_ID) == QLatin1String("MSVC"))
        compiler[QStringLiteral("cl_version")] = QStringLiteral(FLYSIGHT_CAPTURE_COMPILER_VERSION);

    const QJsonObject solver{
        {QStringLiteral("gtsam_version"), QStringLiteral(GTSAM_VERSION_STRING)},
#ifdef GTSAM_USE_TBB
        {QStringLiteral("gtsam_use_tbb"), true},
#else
        {QStringLiteral("gtsam_use_tbb"), false},
#endif
        {QStringLiteral("gtsam_enable_boost_serialization"), int(GTSAM_ENABLE_BOOST_SERIALIZATION)},
        {QStringLiteral("gtsam_use_boost_features"), int(GTSAM_USE_BOOST_FEATURES)},
        {QStringLiteral("gtsam_dir"), QStringLiteral(FLYSIGHT_CAPTURE_GTSAM_DIR)},
        {QStringLiteral("pinned_in"), QStringLiteral("cmake/SolverSuperbuild.cmake")}};

    QJsonObject hashes;
    for (auto it = fileHashes.constBegin(); it != fileHashes.constEnd(); ++it)
        hashes[it.key()] = it.value();
    QJsonObject generator;
    for (auto it = generatorHashes.constBegin(); it != generatorHashes.constEnd(); ++it)
        generator[it.key()] = it.value();

    return {{QStringLiteral("description"),
             QStringLiteral("Provenance of the fusion goldens in this directory, written by "
                            "fusion_golden_capture from the product kernel. Procedure: "
                            "tests/README.md, section 11.")},
            {QStringLiteral("captured_by"), QStringLiteral("fusion_golden_capture")},
            {QStringLiteral("capture_date"), QDate::currentDate().toString(Qt::ISODate)},
            {QStringLiteral("repository_revision"), revision},
            {QStringLiteral("machine"),
             QJsonObject{{QStringLiteral("os"), QSysInfo::prettyProductName()},
                         {QStringLiteral("cpu_architecture"), QSysInfo::currentCpuArchitecture()}}},
            {QStringLiteral("compiler"), compiler},
            {QStringLiteral("qt"), QString::fromLatin1(qVersion())},
            {QStringLiteral("solver"), solver},
            {QStringLiteral("determinism"),
             QJsonObject{{QStringLiteral("runs"), 2},
                         {QStringLiteral("byte_identical"), true},
                         {QStringLiteral("method"),
                          QStringLiteral("every fixture is captured twice in one process and the "
                                         "bytes of each file compared before it is written")}}},
            {QStringLiteral("sha256_lf"), hashes},
            {QStringLiteral("sha256_note"),
             QStringLiteral("Hashes of the files as written (LF line endings). A checkout with "
                            "core.autocrlf=true has CRLF in the working tree; the loader accepts both.")},
            {QStringLiteral("fixture_generator_sha256_lf"), generator},
            {QStringLiteral("fixture_generator_note"),
             QStringLiteral("The fixture generator the goldens were captured with. The goldens are "
                            "valid for these inputs only: if either file changes, re-capture "
                            "(tests/README.md, section 11).")}};
}

int usage()
{
    std::cerr << "usage: fusion_golden_capture [--revision <text>] [<output-directory>]" << std::endl;
    return kExitUsage;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QJsonValue revision = QJsonValue::Null;
    QString directoryPath = QStringLiteral(FLYSIGHT_FUSION_GOLDEN_DIR);
    bool haveRevision = false, haveDirectory = false;
    const QStringList arguments = app.arguments().mid(1);
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        if (argument == QStringLiteral("--revision")) {
            if (haveRevision || i + 1 >= arguments.size())
                return usage();
            revision = arguments.at(++i);
            haveRevision = true;
        } else if (argument.startsWith(QLatin1Char('-')) || haveDirectory) {
            return usage();
        } else {
            directoryPath = argument;
            haveDirectory = true;
        }
    }
    if (!haveRevision) {
        std::cerr << "no --revision given: capture.json records null; pass --revision "
                     "\"$(git rev-parse HEAD)\" before committing" << std::endl;
    }

    const QDir directory(directoryPath);
    if (!directory.exists() && !QDir().mkpath(directory.absolutePath())) {
        std::cerr << "cannot create " << directory.absolutePath().toStdString() << std::endl;
        return kExitUsage;
    }

    QMap<QString, QString> fileHashes;
    int filesWritten = 0;
    bool unexpected = false;
    for (const FusionFixture &fixture : fusionFixtures()) {
        // Two captures, compared before either is written (TBB is on; the
        // result must not depend on scheduling).
        const Capture first = captureOnce(fixture);
        const Capture second = captureOnce(fixture);
        if (first.json != second.json || first.channels != second.channels) {
            std::cerr << fixture.name.toStdString() << ": two captures differ ("
                      << (first.json != second.json ? "json" : "channels") << ")" << std::endl;
            return kExitNondeterministic;
        }

        const QString jsonFile = fixture.name + QStringLiteral(".json");
        if (!writeFile(directory.filePath(jsonFile), first.json))
            return kExitUsage;
        fileHashes[jsonFile] = sha256(first.json);
        ++filesWritten;
        if (first.outcome == QStringLiteral("succeeded")) {
            const QString channelsFile = fixture.name + QStringLiteral(".channels.txt");
            if (!writeFile(directory.filePath(channelsFile), first.channels))
                return kExitUsage;
            fileHashes[channelsFile] = sha256(first.channels);
            ++filesWritten;
        }

        std::cout << fixture.name.toStdString() << ": " << first.outcome.toStdString();
        if (first.outcome == QStringLiteral("succeeded"))
            std::cout << ", " << first.rows << " rows, objective " << first.objective;
        else
            std::cout << " (" << first.failure.toStdString() << ")";
        if ((first.outcome == QStringLiteral("succeeded")) != fixture.expectSuccess) {
            std::cout << "  ** UNEXPECTED **";
            unexpected = true;
        }
        std::cout << std::endl;
    }

    // The fixture generator the goldens are valid for.
    QMap<QString, QString> generatorHashes;
    for (const char *name : {"fusionfixtures.cpp", "fusionfixtures.h"}) {
        QString hash;
        if (!hashSourceFile(QStringLiteral(FLYSIGHT_FUSION_FIXTURE_SOURCE_DIR "/") + QLatin1String(name), &hash))
            return kExitUsage;
        generatorHashes[QStringLiteral("tests/fusion/") + QLatin1String(name)] = hash;
    }

    const QByteArray provenanceJson =
        QJsonDocument(provenance(revision, fileHashes, generatorHashes)).toJson(QJsonDocument::Indented);
    if (!writeFile(directory.filePath(QStringLiteral("capture.json")), provenanceJson))
        return kExitUsage;
    ++filesWritten;

    std::cout << "wrote " << filesWritten << " files to " << directory.absolutePath().toStdString()
              << std::endl;
    return unexpected ? kExitUnexpected : kExitOk;
}
