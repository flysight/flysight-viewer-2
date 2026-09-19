#ifndef FLYSIGHTTEST_TESTMAIN_H
#define FLYSIGHTTEST_TESTMAIN_H

#include <QCoreApplication>
#include <QHashFunctions>
#include <QString>
#include <QtTest>

#include "testenvironment.h"

/// Defines main() for a test executable.
///
/// Use this instead of QTEST_MAIN / QTEST_GUILESS_MAIN: those construct the
/// application but give no hook to isolate settings and the logbook before an
/// application singleton is first touched. The order below is what matters:
/// application object, then TestEnvironment, then the test object.
///
/// The global QHash seed is made deterministic first, before anything creates
/// a hash: QSet / QHash iteration order is then the same in every run, so a
/// failure that depends on it (invalidation order, say) reproduces when the
/// executable is run by hand exactly as it does under CTest.
///
/// QCoreApplication is sufficient for the core library (QColor,
/// QAbstractTableModel and QTimer need no GUI application) and means no
/// platform plugin has to be deployed next to the test executable.
#define FLYSIGHT_TEST_MAIN(TestClass)                                          \
    int main(int argc, char **argv)                                            \
    {                                                                          \
        QHashSeed::setDeterministicGlobalSeed();                               \
        QCoreApplication app(argc, argv);                                      \
        FlySightTest::TestEnvironment env(QStringLiteral(#TestClass));         \
        TestClass tc;                                                          \
        return QTest::qExec(&tc, argc, argv);                                  \
    }

#endif // FLYSIGHTTEST_TESTMAIN_H
