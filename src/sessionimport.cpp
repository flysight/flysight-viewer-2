#include "sessionimport.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QHash>

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

    // Files that failed for the identical reason form one group, in the order
    // in which each reason first occurs; hints are collected once each.
    struct Group {
        QString reason;
        QStringList paths;
    };
    QList<Group> groups;
    QHash<QString, int> groupOfReason;
    QStringList hints;
    for (const MergeResult &failure : failures) {
        QString displayPath;
        if (!baseDir.isEmpty()) {
            displayPath = QDir(baseDir).relativeFilePath(failure.filePath);
            if (displayPath == failure.filePath)
                displayPath = QFileInfo(failure.filePath).fileName();
        } else {
            displayPath = failure.filePath;
        }

        auto it = groupOfReason.constFind(failure.error);
        if (it == groupOfReason.constEnd()) {
            it = groupOfReason.insert(failure.error, int(groups.size()));
            groups.append({failure.error, {}});
        }
        groups[it.value()].paths.append(displayPath);

        if (!failure.hint.isEmpty() && !hints.contains(failure.hint))
            hints.append(failure.hint);
    }

    // "<file>: <reason>" for a reason only one file has; a shared reason once,
    // followed by its files. At most `fileLimit` files are listed.
    const auto listing = [&groups](int fileLimit) {
        QStringList lines;
        int shown = 0;
        for (const Group &group : groups) {
            if (shown >= fileLimit)
                break;
            if (group.paths.size() == 1) {
                lines.append(group.paths.first() + QStringLiteral(": ") + group.reason);
                ++shown;
                continue;
            }
            lines.append(group.reason);
            for (const QString &path : group.paths) {
                if (shown >= fileLimit)
                    break;
                lines.append(QStringLiteral("    ") + path);
                ++shown;
            }
        }
        return lines.join(QLatin1Char('\n'));
    };

    const int failureCount = int(failures.size());
    QString message = tr("Import has been completed.");
    if (failureCount > 5) {
        message += tr("\nHowever, %1 files failed to import.").arg(failureCount);
        const int displayCount = qMin(failureCount, 10);    // the first 10 files for brevity
        QString displayed = listing(displayCount);
        if (failureCount > displayCount)
            displayed += tr("\n...and %1 more.").arg(failureCount - displayCount);
        message += tr("\nFailed Files:\n") + displayed;
    } else {
        message += tr("\nHowever, some files failed to import:");
        message += QLatin1Char('\n') + listing(failureCount);
    }

    if (!hints.isEmpty())
        message += QStringLiteral("\n\n") + hints.join(QLatin1Char('\n'));
    return message;
}

} // namespace SessionImport
} // namespace FlySight
