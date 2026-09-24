// The plug-in code identity (src/plugincodeidentity.h): the result version of
// every plug-in registration, one SHA-256 digest over every *.py file under
// the plug-in folder, the SDK file, and the Python and numpy versions.
//
// No Python here: the digest is a pure function of its ingredients, and the
// folder walk is plain file I/O. tst_python_bridge proves that the real host
// declares this function's output on every plug-in registration. Folders are
// temporary directories; nothing is written to the source tree. The link test
// needs a symbolic link (not Windows, where QFile::link makes a shortcut file)
// or a junction (Windows only, made with "mklink /J"); it skips where neither
// can be made.

#include <algorithm>
#include <optional>
#include <utility>

#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "fileread.h"
#include "fixturebuilder.h"
#include "plugincodeidentity.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

bool isIdentity(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^plugins-sha256:[0-9a-f]{64}$"));
    return pattern.match(text).hasMatch();
}

PluginSourceFile source(const char *name, std::optional<QByteArray> bytes)
{
    return {QString::fromLatin1(name), std::move(bytes)};
}

// The ingredients the change rows start from. b.py is empty, so that
// "unreadable instead of empty" and "moved bytes" have something to act on.
PluginCodeIngredients baseIngredients()
{
    PluginCodeIngredients i;
    i.files = {source("a.py", QByteArray("x")), source("b.py", QByteArray()),
               source("pkg/helper.py", QByteArray("def h():\n    return 1\n"))};
    i.sdk = QByteArray("# the SDK\n");
    i.pythonVersion = QStringLiteral("3.13.3");
    i.numpyVersion = QStringLiteral("2.2.4");
    return i;
}

qsizetype indexOf(const QList<PluginSourceFile> &files, const char *name)
{
    for (qsizetype i = 0; i < files.size(); ++i) {
        if (files[i].name == QLatin1String(name))
            return i;
    }
    return -1;
}

// Applies the change named by a row of eachIngredientChangesIdentity to the
// pair (both start as baseIngredients()). False for an unknown row.
bool applyChange(const QString &change, PluginCodeIngredients &before, PluginCodeIngredients &after)
{
    if (change == QLatin1String("file edited")) {
        after.files[indexOf(after.files, "pkg/helper.py")].bytes = QByteArray("def h():\n    return 2\n");
    } else if (change == QLatin1String("file renamed")) {
        after.files[indexOf(after.files, "pkg/helper.py")].name = QStringLiteral("pkg/helper2.py");
    } else if (change == QLatin1String("helper added")) {
        after.files.append(source("pkg/extra.py", QByteArray("z = 3\n")));
    } else if (change == QLatin1String("file removed")) {
        after.files.removeAt(indexOf(after.files, "pkg/helper.py"));
    } else if (change == QLatin1String("sdk edited")) {
        after.sdk = QByteArray("# the SDK, edited\n");
    } else if (change == QLatin1String("sdk unreadable, not empty")) {
        before.sdk = QByteArray();
        after.sdk = std::nullopt;
    } else if (change == QLatin1String("file unreadable, not empty")) {
        after.files[indexOf(after.files, "b.py")].bytes = std::nullopt;
    } else if (change == QLatin1String("python version")) {
        after.pythonVersion = QStringLiteral("3.13.4");
    } else if (change == QLatin1String("numpy version")) {
        after.numpyVersion = QStringLiteral("2.3.0");
    } else if (change == QLatin1String("numpy absent")) {
        after.numpyVersion.clear();
    } else if (change == QLatin1String("bytes moved between files")) {
        after.files[indexOf(after.files, "a.py")].bytes = QByteArray();
        after.files[indexOf(after.files, "b.py")].bytes = QByteArray("x");
    } else {
        return false;
    }
    return true;
}

// Writes `contents` to root/relative, creating the folders on the way.
bool put(const QString &root, const QString &relative, const QByteArray &contents)
{
    const QString path = QDir(root).filePath(relative);
    return QDir().mkpath(QFileInfo(path).absolutePath()) && writeFile(path, contents);
}

// The folder of the listing criterion: five files that count (a dot-named
// one among them), five that do not.
bool writeTestFolder(const QString &root)
{
    return put(root, QStringLiteral("a.py"), "a = 1\n")
        && put(root, QStringLiteral("b.py"), "b = 1\n")
        && put(root, QStringLiteral(".dotted.py"), "dotted = 1\n")
        && put(root, QStringLiteral("notes.txt"), "notes\n")
        && put(root, QStringLiteral("sub/c.py"), "c = 1\n")
        && put(root, QStringLiteral("sub/deeper/d.py"), "d = 1\n")
        && put(root, QStringLiteral("__pycache__/x.py"), "x = 1\n")
        && put(root, QStringLiteral("__pycache__/a.cpython-313.pyc"), QByteArray("\x00\x01pyc", 5))
        && put(root, QStringLiteral("sub/__pycache__/y.py"), "y = 1\n")
        && put(root, QStringLiteral(".hidden/e.py"), "e = 1\n");
}

// The identity of a folder, with a fixed SDK and fixed versions.
QString folderIdentity(const QString &root)
{
    PluginCodeIngredients i;
    i.files = readPluginCodeFiles(root);
    i.sdk = QByteArray("# the SDK\n");
    i.pythonVersion = QStringLiteral("3.13.3");
    i.numpyVersion = QStringLiteral("2.2.4");
    return pluginCodeIdentity(i);
}

QStringList names(const QList<PluginSourceFile> &files)
{
    QStringList out;
    for (const PluginSourceFile &f : files)
        out << f.name;
    return out;
}

// A link to the folder `target` at `link`: a symbolic link, or on Windows a
// junction. Empty on success, else why it could not be made (the row skips).
QString makeFolderLink(const QString &target, const QString &link)
{
#ifdef Q_OS_WIN
    QProcess mklink;
    mklink.setStandardOutputFile(QProcess::nullDevice());   // "Junction created for ..."
    mklink.start(QStringLiteral("cmd"), {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                                         QDir::toNativeSeparators(link), QDir::toNativeSeparators(target)});
    if (!mklink.waitForFinished() || mklink.exitStatus() != QProcess::NormalExit || mklink.exitCode() != 0
        || !QFileInfo(link).isDir())
        return QStringLiteral("mklink /J failed (exit code %1)").arg(mklink.exitCode());
    return QString();
#else
    if (!QFile::link(target, link) || !QFileInfo(link).isSymLink())
        return QStringLiteral("QFile::link failed");
    return QString();
#endif
}

// Removes a link made by makeFolderLink(), never what it leads to. Done before
// the test environment removes its folder, which must not walk a link loop.
void removeFolderLink(const QString &link)
{
#ifdef Q_OS_WIN
    QDir().rmdir(link);         // RemoveDirectoryW removes the junction itself
#else
    QFile::remove(link);
#endif
}

} // namespace

class PluginIdentityTest : public QObject {
    Q_OBJECT

private slots:
    void identityIsDeterministic();
    void encodingIsPinned();
    void eachIngredientChangesIdentity_data();
    void eachIngredientChangesIdentity();
    void absentVersionUsesFixedToken();
    void readsTheFolderRecursively();
    void subfolderFileChangesIdentity();
    void pycacheAndHiddenDirectoriesAreIgnored();
    void hiddenFilesCount();
    void linkedFoldersAreReadThrough();
    void unreadableFileIsNotEmpty();
};

void PluginIdentityTest::identityIsDeterministic()
{
    const PluginCodeIngredients base = baseIngredients();
    const QString identity = pluginCodeIdentity(base);
    QVERIFY2(isIdentity(identity), qPrintable(identity));
    QCOMPARE(pluginCodeIdentity(baseIngredients()), identity);

    // The caller's order does not matter
    PluginCodeIngredients reversed = base;
    std::reverse(reversed.files.begin(), reversed.files.end());
    QCOMPARE(pluginCodeIdentity(reversed), identity);

    // Nothing at all is still an identity
    const PluginCodeIngredients nothing;
    QVERIFY(!nothing.sdk.has_value());
    QVERIFY2(isIdentity(pluginCodeIdentity(nothing)), qPrintable(pluginCodeIdentity(nothing)));
    QVERIFY(pluginCodeIdentity(nothing) != identity);
}

// The byte encoding of plugincodeidentity.h, built by hand.
void PluginIdentityTest::encodingIsPinned()
{
    const auto sha256 = [](const QByteArray &bytes) {
        return QStringLiteral("plugins-sha256:")
             + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    };

    // Passed out of order: the digest sorts by the '/'-separated relative name
    PluginCodeIngredients i;
    i.files = {source("pkg/helper.py", QByteArray("y = 2\n")), source("a.py", QByteArray("x = 1\n"))};
    i.sdk = QByteArray("sdk");
    i.pythonVersion = QStringLiteral("3.13.3");
    i.numpyVersion = QStringLiteral("2.2.4");
    const QByteArray expected =
        "flysight-plugin-code-identity 1\n"
        "python 6\n3.13.3\n"
        "numpy 5\n2.2.4\n"
        "sdk 3\nsdk\n"
        "files 2\n"
        "name 4\na.py\n"
        "bytes 6\nx = 1\n\n"
        "name 13\npkg/helper.py\n"
        "bytes 6\ny = 2\n\n";
    QCOMPARE(pluginCodeIdentity(i), sha256(expected));

    // The unreadable and unknown forms
    PluginCodeIngredients unknown;
    unknown.files = {source("a.py", std::nullopt)};
    const QByteArray expectedUnknown =
        "flysight-plugin-code-identity 1\n"
        "python 4\nnone\n"
        "numpy 4\nnone\n"
        "sdk unreadable\n"
        "files 1\n"
        "name 4\na.py\n"
        "unreadable\n";
    QCOMPARE(pluginCodeIdentity(unknown), sha256(expectedUnknown));
}

void PluginIdentityTest::eachIngredientChangesIdentity_data()
{
    QTest::addColumn<QString>("change");
    for (const char *row : {"file edited", "file renamed", "helper added", "file removed", "sdk edited",
                            "sdk unreadable, not empty", "file unreadable, not empty", "python version",
                            "numpy version", "numpy absent", "bytes moved between files"})
        QTest::newRow(row) << QString::fromLatin1(row);
}

void PluginIdentityTest::eachIngredientChangesIdentity()
{
    QFETCH(QString, change);
    PluginCodeIngredients before = baseIngredients();
    PluginCodeIngredients after = before;
    QVERIFY2(applyChange(change, before, after), qPrintable(change));

    const QString a = pluginCodeIdentity(before);
    const QString b = pluginCodeIdentity(after);
    QVERIFY(isIdentity(a));
    QVERIFY(isIdentity(b));
    QVERIFY(a != b);
}

void PluginIdentityTest::absentVersionUsesFixedToken()
{
    PluginCodeIngredients empty = baseIngredients();
    empty.numpyVersion.clear();
    PluginCodeIngredients token = baseIngredients();
    token.numpyVersion = QString::fromLatin1(PluginCodeIdentityUnknownVersion);
    QCOMPARE(token.numpyVersion, QStringLiteral("none"));
    QCOMPARE(pluginCodeIdentity(empty), pluginCodeIdentity(token));

    empty.pythonVersion.clear();
    token.pythonVersion = QStringLiteral("none");
    QCOMPARE(pluginCodeIdentity(empty), pluginCodeIdentity(token));
}

void PluginIdentityTest::readsTheFolderRecursively()
{
    const QString root = TestEnvironment::instance().newTempDir(QStringLiteral("plugins"));
    QVERIFY(writeTestFolder(root));

    const QList<PluginSourceFile> files = readPluginCodeFiles(root);
    QCOMPARE(names(files), QStringList({".dotted.py", "a.py", "b.py", "sub/c.py", "sub/deeper/d.py"}));
    const QList<QByteArray> expectedBytes{"dotted = 1\n", "a = 1\n", "b = 1\n", "c = 1\n", "d = 1\n"};
    for (qsizetype i = 0; i < files.size(); ++i) {
        QVERIFY2(files[i].bytes.has_value(), qPrintable(files[i].name));
        QCOMPARE(*files[i].bytes, expectedBytes[i]);
    }

    // A missing or empty folder: nothing
    QVERIFY(readPluginCodeFiles(QDir(root).filePath(QStringLiteral("missing"))).isEmpty());
    QVERIFY(readPluginCodeFiles(TestEnvironment::instance().newTempDir(QStringLiteral("empty"))).isEmpty());
    QVERIFY(readPluginCodeFiles(QString()).isEmpty());
}

void PluginIdentityTest::subfolderFileChangesIdentity()
{
    const QString root = TestEnvironment::instance().newTempDir(QStringLiteral("plugins"));
    QVERIFY(writeTestFolder(root));
    const QString original = folderIdentity(root);
    QVERIFY(isIdentity(original));
    QCOMPARE(folderIdentity(root), original);

    // Rewritten, one at a time; putting the bytes back restores the identity
    const QList<std::pair<QString, QByteArray>> counted{
        {QStringLiteral("a.py"), "a = 1\n"},
        {QStringLiteral(".dotted.py"), "dotted = 1\n"},
        {QStringLiteral("sub/c.py"), "c = 1\n"},
        {QStringLiteral("sub/deeper/d.py"), "d = 1\n"}};
    for (const auto &[name, bytes] : counted) {
        QVERIFY(put(root, name, bytes + "# edited\n"));
        QVERIFY2(folderIdentity(root) != original, qPrintable(name));
        QVERIFY(put(root, name, bytes));
        QCOMPARE(folderIdentity(root), original);
    }

    // Added
    QVERIFY(put(root, QStringLiteral("sub/e.py"), "e = 1\n"));
    QVERIFY(folderIdentity(root) != original);
    QVERIFY(QFile::remove(QDir(root).filePath(QStringLiteral("sub/e.py"))));
    QCOMPARE(folderIdentity(root), original);

    // Removed
    QVERIFY(QFile::remove(QDir(root).filePath(QStringLiteral("sub/c.py"))));
    QVERIFY(folderIdentity(root) != original);
}

void PluginIdentityTest::pycacheAndHiddenDirectoriesAreIgnored()
{
    const QString root = TestEnvironment::instance().newTempDir(QStringLiteral("plugins"));
    QVERIFY(writeTestFolder(root));
    const QString original = folderIdentity(root);

    for (const char *name : {"notes.txt", "__pycache__/x.py", "__pycache__/a.cpython-313.pyc",
                             "sub/__pycache__/y.py", ".hidden/e.py"}) {
        QVERIFY(put(root, QString::fromLatin1(name), "rewritten\n"));
        QVERIFY2(folderIdentity(root) == original, name);
    }

    // Added: a new file under __pycache__/, and one under a new hidden folder
    QVERIFY(put(root, QStringLiteral("__pycache__/new.py"), "new = 1\n"));
    QVERIFY(put(root, QStringLiteral("sub/__pycache__/b.cpython-313.pyc"), "pyc"));
    QVERIFY(put(root, QStringLiteral("sub/.git/hook.py"), "hook = 1\n"));
    QCOMPARE(folderIdentity(root), original);
}

// Only hidden folders are left out: a hidden *.py file counts, whether its
// name starts with '.' (hidden on every platform but Windows) or, on Windows,
// the file system marks it hidden.
void PluginIdentityTest::hiddenFilesCount()
{
    const QString root = TestEnvironment::instance().newTempDir(QStringLiteral("plugins"));
    QVERIFY(writeTestFolder(root));
    const QString original = folderIdentity(root);

    QVERIFY(put(root, QStringLiteral("sub/.helper.py"), "helper = 1\n"));
    QCOMPARE(readPluginCodeFiles(root).size(), 6);
    const QString withDotted = folderIdentity(root);
    QVERIFY(withDotted != original);
    QVERIFY(put(root, QStringLiteral("sub/.helper.py"), "helper = 2\n"));
    QVERIFY(folderIdentity(root) != withDotted);
    QVERIFY(QFile::remove(QDir(root).filePath(QStringLiteral("sub/.helper.py"))));
    QCOMPARE(folderIdentity(root), original);

#ifdef Q_OS_WIN
    const QString marked = QDir(root).filePath(QStringLiteral("marked.py"));
    QVERIFY(put(root, QStringLiteral("marked.py"), "marked = 1\n"));
    QVERIFY(SetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(marked).utf16()),
                               FILE_ATTRIBUTE_HIDDEN));
    QVERIFY(QFileInfo(marked).isHidden());
    QVERIFY(names(readPluginCodeFiles(root)).contains(QStringLiteral("marked.py")));
    QVERIFY(folderIdentity(root) != original);
    QVERIFY(SetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(marked).utf16()),
                               FILE_ATTRIBUTE_NORMAL));
#endif
}

// A linked folder (a symbolic link, or a junction on Windows) is read through
// under its own name, and a link back to a folder on the way down (a loop) is
// not entered again, so the walk ends.
void PluginIdentityTest::linkedFoldersAreReadThrough()
{
    const QString root = TestEnvironment::instance().newTempDir(QStringLiteral("plugins"));
    QVERIFY(writeTestFolder(root));
    const QString original = folderIdentity(root);

    const QString zlink = QDir(root).filePath(QStringLiteral("zlink"));
    const QString up = QDir(root).filePath(QStringLiteral("sub/deeper/up"));
    const auto removeLinks = qScopeGuard([&] {
        removeFolderLink(up);
        removeFolderLink(zlink);
    });
    const QString skip = makeFolderLink(QDir(root).filePath(QStringLiteral("sub")), zlink);
    if (!skip.isEmpty())
        QSKIP(qPrintable(skip));
    QCOMPARE(names(readPluginCodeFiles(root)),
             QStringList({".dotted.py", "a.py", "b.py", "sub/c.py", "sub/deeper/d.py", "zlink/c.py",
                          "zlink/deeper/d.py"}));
    const QString linked = folderIdentity(root);
    QVERIFY(linked != original);
    // An edit behind the link changes the identity like any other
    QVERIFY(put(root, QStringLiteral("sub/c.py"), "c = 2\n"));
    QVERIFY(folderIdentity(root) != linked);
    QVERIFY(put(root, QStringLiteral("sub/c.py"), "c = 1\n"));
    QCOMPARE(folderIdentity(root), linked);

    // A loop: sub/deeper/up leads back to the plug-in folder itself
    const QString loopSkip = makeFolderLink(root, up);
    if (!loopSkip.isEmpty())
        QSKIP(qPrintable(loopSkip));
    QCOMPARE(names(readPluginCodeFiles(root)),
             QStringList({".dotted.py", "a.py", "b.py", "sub/c.py", "sub/deeper/d.py", "zlink/c.py",
                          "zlink/deeper/d.py"}));
    QCOMPARE(folderIdentity(root), linked);
}

void PluginIdentityTest::unreadableFileIsNotEmpty()
{
    const QString root = TestEnvironment::instance().newTempDir(QStringLiteral("read"));

    QString error;
    QVERIFY(!readWholeFile(QDir(root).filePath(QStringLiteral("missing.py")), &error).has_value());
    QVERIFY(!error.isEmpty());
    QVERIFY(!readWholeFile(root).has_value());      // a directory

    const QString emptyPath = QDir(root).filePath(QStringLiteral("empty.py"));
    QVERIFY(writeFile(emptyPath, QByteArray()));
    const std::optional<QByteArray> empty = readWholeFile(emptyPath);
    QVERIFY(empty.has_value());
    QVERIFY(empty->isEmpty());

    const QString fullPath = QDir(root).filePath(QStringLiteral("full.py"));
    QVERIFY(writeFile(fullPath, "full = 1\n"));
    const std::optional<QByteArray> full = readWholeFile(fullPath);
    QVERIFY(full.has_value());
    QCOMPARE(*full, QByteArray("full = 1\n"));

    // In the digest an unreadable file is not an empty one
    PluginCodeIngredients readable;
    readable.files = {source("empty.py", QByteArray())};
    PluginCodeIngredients unreadable;
    unreadable.files = {source("empty.py", std::nullopt)};
    QVERIFY(pluginCodeIdentity(readable) != pluginCodeIdentity(unreadable));
}

FLYSIGHT_TEST_MAIN(PluginIdentityTest)
#include "tst_plugin_identity.moc"
