#ifndef SESSIONIMPORT_H
#define SESSIONIMPORT_H

#include <functional>

#include <QList>
#include <QString>
#include <QStringList>

#include "sessionmodel.h"

namespace FlySight {

/// The application's import flow, free of widgets so that it can be tested in
/// flysight_core: parse every file, hand the parsed files to the model in ONE
/// SessionModel::mergeSessions call (the model decides between creation and
/// merge), and report one result per file - parse failures included - with the
/// error text the failure dialog shows.
namespace SessionImport {

struct BatchResult {
    QList<MergeResult> files;                    ///< one per attempted file, input order (parse failures included)

    QStringList importedSessionIds() const;      ///< unique ids of Created / Merged / Unchanged, first-seen order
    QList<MergeResult> failures() const;         ///< input order
};

/// progress(i, total) is called before file i is parsed; returning false
/// cancels: files not yet parsed are neither imported nor reported. May be empty.
BatchResult importFiles(SessionModel &model, const QStringList &filePaths,
                        const std::function<bool(int, int)> &progress = {});

/// Text for the existing import-failure warning box; empty when there are no
/// failures. A reason (MergeResult::error) only one file has is one line,
/// "<displayPath>: <error>"; a reason several files share is given once, where
/// it first occurs, followed by its files, one indented line each. displayPath
/// is relative to baseDir when one is given (else the path as passed). Up to
/// five failures are listed in full; beyond that the count and the first ten
/// files. Each distinct MergeResult::hint follows the list once, in the order
/// first seen.
QString failureMessage(const QList<MergeResult> &failures, const QString &baseDir);

} // namespace SessionImport
} // namespace FlySight

#endif // SESSIONIMPORT_H
