#include "plugincodeidentity.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include "fileread.h"

namespace FlySight {

namespace {

// Ordinal by name; equal names (never produced by the folder walk) unreadable
// first, then by bytes, so that the order never depends on the caller's.
bool sourceFileLess(const PluginSourceFile &a, const PluginSourceFile &b)
{
    const int byName = QString::compare(a.name, b.name, Qt::CaseSensitive);
    if (byName != 0)
        return byName < 0;
    if (a.bytes.has_value() != b.bytes.has_value())
        return !a.bytes.has_value();
    return a.bytes.has_value() && *a.bytes < *b.bytes;
}

void sortByName(QList<PluginSourceFile> &files)
{
    std::sort(files.begin(), files.end(), sourceFileLess);
}

// "<label> <n>\n<bytes>\n"
void addField(QCryptographicHash &hash, const char *label, const QByteArray &bytes)
{
    hash.addData(QByteArray(label) + ' ' + QByteArray::number(bytes.size()) + '\n');
    hash.addData(bytes);
    hash.addData(QByteArrayView("\n"));
}

void addVersion(QCryptographicHash &hash, const char *label, const QString &version)
{
    addField(hash, label, version.isEmpty() ? QByteArray(PluginCodeIdentityUnknownVersion) : version.toUtf8());
}

// What identifies the folder at `path` on its volume, with every symbolic
// link and junction on the way resolved: the volume serial number and file
// index on Windows, the device and inode elsewhere. A canonical path would
// not do: Qt resolves symbolic links in it but not junctions. Falls back to
// the canonical path when the identity cannot be read; empty when neither can.
QString folderKey(const QString &path)
{
#ifdef Q_OS_WIN
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), 0,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION info;
        const bool read = GetFileInformationByHandle(handle, &info) != 0;
        CloseHandle(handle);
        if (read) {
            return QStringLiteral("%1:%2:%3")
                .arg(info.dwVolumeSerialNumber)
                .arg(info.nFileIndexHigh)
                .arg(info.nFileIndexLow);
        }
    }
#else
    struct stat st;
    if (::stat(QFile::encodeName(path).constData(), &st) == 0)
        return QStringLiteral("%1:%2").arg(quint64(st.st_dev)).arg(quint64(st.st_ino));
#endif
    return QFileInfo(path).canonicalFilePath();
}

// One directory level of readPluginCodeFiles(): its *.py files, then the
// subdirectories that may be entered. `ancestors` holds folderKey() of every
// folder on the way down from the root, this one included.
void collectPythonFiles(const QDir &root, const QDir &dir, QSet<QString> &ancestors,
                        QList<PluginSourceFile> &out)
{
    // Hidden files count: only hidden folders are left out.
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.py")}, QDir::Files | QDir::Hidden);
    for (const QFileInfo &fi : files)
        out.append({root.relativeFilePath(fi.absoluteFilePath()), readWholeFile(fi.absoluteFilePath())});

    // Pruned before descending: byte-code caches, and dot-named (hidden on
    // every platform) and file-system-hidden folders, which the listing
    // leaves out. A link or junction is followed unless its target is a
    // folder on the way down, which would cycle.
    const QFileInfoList subdirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &fi : subdirs) {
        const QString name = fi.fileName();
        if (name == QLatin1String("__pycache__") || name.startsWith(QLatin1Char('.')))
            continue;
        const QString key = folderKey(fi.absoluteFilePath());
        if (key.isEmpty() || ancestors.contains(key))
            continue;
        ancestors.insert(key);
        collectPythonFiles(root, QDir(fi.absoluteFilePath()), ancestors, out);
        ancestors.remove(key);
    }
}

} // namespace

QList<PluginSourceFile> readPluginCodeFiles(const QString &pluginDir)
{
    QList<PluginSourceFile> files;
    if (pluginDir.isEmpty())
        return files;
    const QDir root(pluginDir);
    if (!root.exists())
        return files;
    QSet<QString> ancestors{folderKey(root.absolutePath())};
    collectPythonFiles(root, root, ancestors, files);
    sortByName(files);
    return files;
}

QString pluginCodeIdentity(const PluginCodeIngredients &ingredients)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("flysight-plugin-code-identity 1\n"));
    addVersion(hash, "python", ingredients.pythonVersion);
    addVersion(hash, "numpy", ingredients.numpyVersion);
    if (ingredients.sdk)
        addField(hash, "sdk", *ingredients.sdk);
    else
        hash.addData(QByteArrayView("sdk unreadable\n"));

    QList<PluginSourceFile> files = ingredients.files;
    sortByName(files);
    hash.addData("files " + QByteArray::number(files.size()) + '\n');
    for (const PluginSourceFile &file : std::as_const(files)) {
        addField(hash, "name", file.name.toUtf8());
        if (file.bytes)
            addField(hash, "bytes", *file.bytes);
        else
            hash.addData(QByteArrayView("unreadable\n"));
    }

    return QString::fromLatin1(PluginCodeIdentityPrefix) + QString::fromLatin1(hash.result().toHex());
}

} // namespace FlySight
