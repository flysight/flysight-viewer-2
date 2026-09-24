#ifndef FILEREAD_H
#define FILEREAD_H

#include <optional>

#include <QByteArray>
#include <QString>

class QIODevice;

namespace FlySight {

/// The whole file, or nullopt when it cannot be opened (missing, a directory,
/// locked, no permission), reading fails, or fewer bytes than QFile::size()
/// reported were read (a short read). A file that reads in full but empty is
/// an empty QByteArray, never nullopt. `error`, when given, is cleared on
/// success and says why otherwise.
std::optional<QByteArray> readWholeFile(const QString &path, QString *error = nullptr);

/// The rest of an open `device`, or nullopt when reading fails or fewer than
/// `expectedSize` bytes arrive. readWholeFile() passes QFile::size(); tests
/// pass a larger size to simulate a short read, which no file system produces
/// on demand.
std::optional<QByteArray> readWholeDevice(QIODevice &device, qint64 expectedSize, QString *error = nullptr);

} // namespace FlySight

#endif // FILEREAD_H
