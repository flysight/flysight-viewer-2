#ifndef FLYSIGHTTEST_TESTMAIN_H
#define FLYSIGHTTEST_TESTMAIN_H

#include <QCoreApplication>
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
/// QCoreApplication is sufficient for the core library (QColor,
/// QAbstractTableModel and QTimer need no GUI application) and means no
/// platform plugin has to be deployed next to the test executable.
#define FLYSIGHT_TEST_MAIN(TestClass)                                          \
    int main(int argc, char **argv)                                            \
    {                                                                          \
        QCoreApplication app(argc, argv);                                      \
        FlySightTest::TestEnvironment env(QStringLiteral(#TestClass));         \
        TestClass tc;                                                          \
        return QTest::qExec(&tc, argc, argv);                                  \
    }

#endif // FLYSIGHTTEST_TESTMAIN_H
