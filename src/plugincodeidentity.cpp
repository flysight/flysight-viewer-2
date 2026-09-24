#include "plugincodeidentity.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

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

// One directory level of readPluginCodeFiles(): its *.py files, then the
// subdirectories that may be entered.
void collectPythonFiles(const QDir &root, const QDir &dir, QList<PluginSourceFile> &out)
{
    // The host's import listing flags: QDir::Files without QDir::Hidden.
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.py")}, QDir::Files);
    for (const QFileInfo &fi : files)
        out.append({root.relativeFilePath(fi.absoluteFilePath()), readWholeFile(fi.absoluteFilePath())});

    // Pruned before descending: byte-code caches, dot-named (hidden on every
    // platform) and file-system-hidden folders, and links, which could cycle.
    const QFileInfoList subdirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &fi : subdirs) {
        const QString name = fi.fileName();
        if (name == QLatin1String("__pycache__") || name.startsWith(QLatin1Char('.'))
            || fi.isSymLink() || fi.isJunction())
            continue;
        collectPythonFiles(root, QDir(fi.absoluteFilePath()), out);
    }
}

} // namespace

std::optional<QByteArray> readWholeFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const qint64 expected = file.size();
    QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError || bytes.size() < expected)
        return std::nullopt;
    return bytes;
}

QList<PluginSourceFile> readPluginCodeFiles(const QString &pluginDir)
{
    QList<PluginSourceFile> files;
    if (pluginDir.isEmpty())
        return files;
    const QDir root(pluginDir);
    if (!root.exists())
        return files;
    collectPythonFiles(root, root, files);
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
