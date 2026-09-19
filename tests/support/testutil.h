#ifndef FLYSIGHTTEST_TESTUTIL_H
#define FLYSIGHTTEST_TESTUTIL_H

#include <cstring>
#include <utility>

#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace FlySightTest {

/// Absolute comparison with the tolerance the suites use for values that are
/// exact in the decimal sense but not in binary (62.5 * 1.14688 vs 71.68).
inline bool isNear(double a, double b)
{
    return qAbs(a - b) <= 1e-9;
}

/// Bit pattern, not ==: NaN equals NaN, and -0.0 differs from 0.0.
inline bool sameBits(double a, double b)
{
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}

/// Captures warning and critical messages for as long as it lives, INSTEAD of
/// the installed handler: nothing is printed and nothing reaches QtTest (so a
/// captured warning cannot satisfy QTest::ignoreMessage). Messages of other
/// types are dropped. Construct it after TestEnvironment::registerBuiltIns(),
/// whose registrations may warn. Not nestable: there is one capture at a time.
class WarningCapture {
public:
    WarningCapture()
    {
        messagesRef().clear();
        m_previous = qInstallMessageHandler(handler);
    }
    ~WarningCapture() { qInstallMessageHandler(m_previous); }

    /// Number of warnings captured so far
    int count() const { return int(messagesRef().size()); }
    /// Every captured warning, in order
    QStringList messages() const { return messagesRef(); }
    /// The captured warnings that contain `fragment`
    QStringList matching(const QString &fragment) const
    {
        QStringList result;
        for (const QString &w : std::as_const(messagesRef())) {
            if (w.contains(fragment))
                result.append(w);
        }
        return result;
    }
    /// Number of captured warnings that contain `fragment`
    int count(const QString &fragment) const { return int(matching(fragment).size()); }

private:
    Q_DISABLE_COPY_MOVE(WarningCapture)

    static QStringList &messagesRef()
    {
        static QStringList messages;
        return messages;
    }
    static void handler(QtMsgType type, const QMessageLogContext &, const QString &message)
    {
        if (type == QtWarningMsg || type == QtCriticalMsg)
            messagesRef().append(message);
    }

    QtMessageHandler m_previous = nullptr;
};

} // namespace FlySightTest

#endif // FLYSIGHTTEST_TESTUTIL_H
