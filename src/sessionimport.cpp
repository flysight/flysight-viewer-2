#include "sessionimport.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>

#include "dataimporter.h"

namespace FlySight {
namespace SessionImport {

QStringList BatchResult::importedSessionIds() const
{
    QStringList ids;
    for (const MergeResult &result : files) {
        if (result.ok() && !result.sessionId.isEmpty() && !ids.contains(result.sessionId))
            ids.append(result.sessionId);
    }
    return ids;
}

QList<MergeResult> BatchResult::failures() const
{
    QList<MergeResult> failed;
    for (const MergeResult &result : files) {
        if (!result.ok())
            failed.append(result);
    }
    return failed;
}

BatchResult importFiles(SessionModel &model, const QStringList &filePaths,
                        const std::function<bool(int, int)> &progress)
{
    BatchResult batch;

    // Parse. A slot of `batch.files` is filled here for a parse failure and
    // after the merge for a parsed file, so the results keep the input order.
    QList<ParsedFile> parsed;
    QList<int> parsedSlots;     // index into batch.files of each parsed file

    const int total = int(filePaths.size());
    for (int i = 0; i < total; ++i) {
        if (progress && !progress(i, total))
            break;      // cancelled: the remaining files are neither imported nor reported

        const QString &filePath = filePaths.at(i);

        // A fresh importer per file: its error text belongs to this file
        DataImporter importer;
        ParsedFile file;
        if (importer.parseFile(filePath, file)) {
            parsedSlots.append(int(batch.files.size()));
            batch.files.append(MergeResult());
            parsed.append(std::move(file));
        } else {
            const QString error = importer.getLastError();
            qWarning() << "Failed to import file:" << filePath << "Error:" << error;

            MergeResult failed;
            failed.filePath = filePath;
            failed.outcome = MergeResult::Outcome::Failed;
            failed.error = error;
            batch.files.append(failed);
        }
    }

    // One merge call per batch: the model decides between creation and merge
    const QList<MergeResult> merged = model.mergeSessions(parsed);
    for (int i = 0; i < merged.size() && i < parsedSlots.size(); ++i)
        batch.files[parsedSlots.at(i)] = merged.at(i);

    return batch;
}

QString failureMessage(const QList<MergeResult> &failures, const QString &baseDir)
{
    if (failures.isEmpty())
        return QString();

    // The strings keep the context they had in MainWindow::importFiles, so
    // existing translations still apply.
    auto tr = [](const char *text) { return QCoreApplication::translate("MainWindow", text); };

    QStringList lines;
    for (const MergeResult &failure : failures) {
        QString displayPath;
        if (!baseDir.isEmpty()) {
            displayPath = QDir(baseDir).relativeFilePath(failure.filePath);
            if (displayPath == failure.filePath)
                displayPath = QFileInfo(failure.filePath).fileName();
        } else {
            displayPath = failure.filePath;
        }
        lines.append(displayPath + QStringLiteral(": ") + failure.error);
    }

    QString message = tr("Import has been completed.");
    if (lines.size() > 5) {
        message += tr("\nHowever, %1 files failed to import.").arg(lines.size());
        const int displayCount = int(qMin(lines.size(), qsizetype(10)));   // the first 10 for brevity
        QString displayed = lines.mid(0, displayCount).join(QLatin1Char('\n'));
        if (lines.size() > displayCount)
            displayed += tr("\n...and %1 more.").arg(lines.size() - displayCount);
        message += tr("\nFailed Files:\n") + displayed;
    } else {
        message += tr("\nHowever, some files failed to import:");
        message += QLatin1Char('\n') + lines.join(QLatin1Char('\n'));
    }
    return message;
}

} // namespace SessionImport
} // namespace FlySight
