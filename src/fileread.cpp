#include "fileread.h"

#include <QFile>
#include <QIODevice>

namespace FlySight {

std::optional<QByteArray> readWholeFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return std::nullopt;
    }
    return readWholeDevice(file, file.size(), error);
}

std::optional<QByteArray> readWholeDevice(QIODevice &device, qint64 expectedSize, QString *error)
{
    QByteArray bytes = device.readAll();
    // QIODevice has no error state of its own; a QFile reports one.
    const auto *file = qobject_cast<const QFileDevice *>(&device);
    if (file && file->error() != QFileDevice::NoError) {
        if (error)
            *error = file->errorString();
        return std::nullopt;
    }
    if (bytes.size() < expectedSize) {
        if (error)
            *error = QStringLiteral("short read: %1 of %2 bytes").arg(bytes.size()).arg(expectedSize);
        return std::nullopt;
    }
    if (error)
        error->clear();
    return bytes;
}

} // namespace FlySight
