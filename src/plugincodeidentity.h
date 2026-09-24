#ifndef PLUGINCODEIDENTITY_H
#define PLUGINCODEIDENTITY_H

#include <optional>

#include <QByteArray>
#include <QList>
#include <QString>

namespace FlySight {

// ---------------------------------------------------------------- plug-in code identity
//
// The result version of every attribute, measurement and calculation the
// plug-in host registers: one SHA-256 digest over the plug-in code and what
// it runs on, computed once when the plug-ins are loaded. Editing, adding or
// removing any *.py file under the plug-in folder (subfolders included),
// changing the SDK file, or upgrading Python or numpy changes it.
//
// This file has no Python dependency: the host reads the ingredients and
// calls pluginCodeIdentity(); tests build ingredients by hand.
//
// The digest (the contract; do not change it without changing the version
// number in the first line) is SHA-256 over the concatenation of the
// following, all text UTF-8, <n> a decimal byte count without padding, "\n"
// a single LF:
//
//   1. "flysight-plugin-code-identity 1\n"
//   2. "python <n>\n<v>\n"   v = pythonVersion, or "none" when it is empty
//   3. "numpy <n>\n<v>\n"    v = numpyVersion, or "none" when it is empty
//   4. "sdk <n>\n<bytes>\n", or "sdk unreadable\n" when the SDK is nullopt
//   5. "files <count>\n"     the number of entries in `files`
//   6. for each file, in ascending order of name by
//      QString::compare(a, b, Qt::CaseSensitive) (ordinal, independent of
//      the order passed; entries with equal names, which the folder walk
//      never produces, are ordered unreadable first, then by their bytes):
//        "name <n>\n<name>\n", then "bytes <n>\n<bytes>\n",
//        or "unreadable\n" when the file's bytes are nullopt
//
// Every variable-length field is length-prefixed, so the encoding is
// injective. Names are paths relative to the plug-in folder with '/'
// separators ("a.py", "pkg/helper.py"); no absolute path ever enters, so
// moving the application or the plug-in folder does not change the identity.
// An unreadable file or SDK is encoded distinctly from an empty one.
//
// The result is PluginCodeIdentityPrefix followed by the 64 lower-case hex
// digits of the digest.

/// One *.py file anywhere under the plug-in folder.
struct PluginSourceFile {
    QString name;                       ///< path relative to the plug-in folder, '/' separators ("pkg/helper.py")
    std::optional<QByteArray> bytes;    ///< nullopt: exists but could not be read in full
};

struct PluginCodeIngredients {
    QList<PluginSourceFile> files;      ///< any order; the digest sorts them
    std::optional<QByteArray> sdk;      ///< the imported SDK file's bytes; nullopt: not readable
    QString pythonVersion;              ///< e.g. "3.13.3"; empty: unknown
    QString numpyVersion;               ///< e.g. "2.2.4"; empty: numpy absent / unreadable
};

inline constexpr char PluginCodeIdentityPrefix[] = "plugins-sha256:";
/// Written in place of a version that could not be read.
inline constexpr char PluginCodeIdentityUnknownVersion[] = "none";

/// The whole file, or nullopt when it cannot be opened, reading fails, or
/// fewer bytes than QFile::size() were read.
std::optional<QByteArray> readWholeFile(const QString &path);

/// Every *.py file under `pluginDir`, recursively, read with readWholeFile(),
/// sorted by name as the digest sorts. Not the host's import list: that stays
/// the top-level *.py files only (every one of which is in this list).
///
/// In each directory the files are listed with the host's flags (QDir::Files,
/// no QDir::Hidden, so file-system-hidden files are skipped). Subdirectories
/// named "__pycache__", whose name starts with '.', that the file system
/// reports hidden, or that are symbolic links or junctions are not entered
/// (so the walk cannot cycle); a symbolic link to a file is read through.
/// An empty or missing folder gives an empty list.
QList<PluginSourceFile> readPluginCodeFiles(const QString &pluginDir);

/// PluginCodeIdentityPrefix + 64 lower-case hex digits. Pure function of the
/// ingredients (see the encoding above).
QString pluginCodeIdentity(const PluginCodeIngredients &ingredients);

} // namespace FlySight

#endif // PLUGINCODEIDENTITY_H
