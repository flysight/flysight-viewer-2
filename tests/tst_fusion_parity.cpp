// Fusion kernel, through its public API only (src/fusion/fusion.h).
//
// Spec acceptance 4: for each committed synthetic fixture the ported fit
// reproduces the golden outputs captured from sensor-fusion-clean-port
// (tests/data/fusion/, procedure in tests/README.md "Fusion golden parity").
// Also the behavior the fusion calculation relies on: rejections as results,
// progress and cancellation at the reference's boundaries, determinism, and
// independence of the calling thread.
//
// Expectations are the committed goldens and literals, never a second call of
// the code under test, except in the determinism and thread tests, where
// comparing two runs is the point.
//
// FLYSIGHT_FUSION_EXACT=1 switches every numeric comparison to bit equality;
// see fusiongolden.h.

#include <memory>

#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QtTest>

#include "fusion/fusion.h"
#include "fusionfixtures.h"
#include "fusiongolden.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

const QStringList kSuccessFixtures{QStringLiteral("coarse_linear"), QStringLiteral("coarse_maneuver"),
                                   QStringLiteral("stationary_spin")};

Fusion::Result runFixture(const QString &name)
{
    return Fusion::run(toChannels(fusionFixture(name)));
}

bool sameBitsEverywhere(const QVector<double> &a, const QVector<double> &b)
{
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (!sameBits(a[i], b[i]))
            return false;
    }
    return true;
}

/// Empty when `a` and `b` are the same result bit for bit, else what differs.
QString differenceBetween(const Fusion::Result &a, const Fusion::Result &b)
{
    if (a.outcome != b.outcome)
        return QStringLiteral("outcome");
    if (a.reason != b.reason)
        return QStringLiteral("reason");
    if (a.diagnosticsJson != b.diagnosticsJson)
        return QStringLiteral("diagnosticsJson");
    for (const QString &name : fusionChannelNames()) {
        if (!sameBitsEverywhere(fusionChannel(a, name), fusionChannel(b, name)))
            return name;
    }
    return QString();
}

bool allArraysEmpty(const Fusion::Result &result)
{
    for (const QString &name : fusionChannelNames()) {
        if (!fusionChannel(result, name).isEmpty())
            return false;
    }
    return true;
}

/// Empty when every channel of `result` matches the golden, else the first
/// channel's difference.
QString channelsDifference(const Fusion::Result &result, const FusionGolden &golden,
                           ParityStatistics *statistics = nullptr)
{
    for (const QString &name : fusionChannelNames()) {
        const QString difference =
            compareSamples(name, fusionChannel(result, name), golden.channels.value(name), statistics);
        if (!difference.isEmpty())
            return difference;
    }
    return QString();
}

double largestStep(const QVector<double> &angles)
{
    double largest = 0;
    for (qsizetype i = 1; i < angles.size(); ++i)
        largest = qMax(largest, qAbs(angles[i] - angles[i - 1]));
    return largest;
}

/// The reference's wording of a progress text of the port. The port dropped
/// the historical "Heading 0 deg"; the boundaries are the same.
QString referenceProgressText(const QString &text)
{
    if (text == QStringLiteral("Starting fit"))
        return QStringLiteral("Starting heading 0 deg");
    if (text.startsWith(QStringLiteral("Pass ")))
        return QStringLiteral("Heading 0 deg, pass ") + text.mid(5);
    return text;
}

} // namespace

class FusionParityTest : public QObject {
    Q_OBJECT

private slots:
    void fixturesAreDeterministic();
    void successFixturesMatchGolden_data();
    void successFixturesMatchGolden();
    void rejectionFixturesMatchGolden_data();
    void rejectionFixturesMatchGolden();
    void progressMatchesReferenceBoundaries();
    void cancelAtEachKindOfBoundary_data();
    void cancelAtEachKindOfBoundary();
    void cancelNeverRequestedChangesNothing();
    void twoRunsAreBitIdentical();
    void workerThreadMatchesMainThread();
    void resultIsIndependentOfCallerState();
};

void FusionParityTest::fixturesAreDeterministic()
{
    const QList<FusionFixture> first = fusionFixtures(), second = fusionFixtures();
    QCOMPARE(first.size(), 12);
    QCOMPARE(second.size(), first.size());
    for (qsizetype i = 0; i < first.size(); ++i) {
        const FusionFixture &a = first[i], &b = second[i];
        QCOMPARE(a.name, b.name);
        QCOMPARE(a.originIndex, b.originIndex);
        const QVector<double> *as[] = {&a.gnssTime, &a.north, &a.east, &a.down, &a.velN, &a.velE, &a.velD,
                                       &a.hAcc, &a.vAcc, &a.sAcc, &a.imuTime, &a.ax, &a.ay, &a.az,
                                       &a.wx, &a.wy, &a.wz};
        const QVector<double> *bs[] = {&b.gnssTime, &b.north, &b.east, &b.down, &b.velN, &b.velE, &b.velD,
                                       &b.hAcc, &b.vAcc, &b.sAcc, &b.imuTime, &b.ax, &b.ay, &b.az,
                                       &b.wx, &b.wy, &b.wz};
        for (int c = 0; c < 17; ++c)
            QVERIFY2(sameBitsEverywhere(*as[c], *bs[c]), qPrintable(a.name));
    }
    // Every fixture has a golden, and the golden agrees on what it is.
    for (const FusionFixture &fixture : first) {
        const FusionGolden golden = loadFusionGolden(fixture.name);
        QCOMPARE(golden.outcome, fixture.expectSuccess ? QStringLiteral("succeeded")
                                                       : QStringLiteral("rejected"));
    }
}

void FusionParityTest::successFixturesMatchGolden_data()
{
    QTest::addColumn<QString>("name");
    for (const QString &name : kSuccessFixtures)
        QTest::newRow(qPrintable(name)) << name;
}

void FusionParityTest::successFixturesMatchGolden()
{
    QFETCH(QString, name);
    const FusionGolden golden = loadFusionGolden(name);
    QCOMPARE(golden.outcome, QStringLiteral("succeeded"));

    const Fusion::Result result = runFixture(name);
    QVERIFY2(result.outcome == Fusion::Outcome::Succeeded, qPrintable(result.reason));
    QVERIFY(result.reason.isEmpty());

    ParityStatistics statistics;
    const QString difference = channelsDifference(result, golden, &statistics);
    qInfo().noquote() << name << (exactParityRequested() ? "(exact mode):" : "(portable mode):")
                      << statistics.summary();
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    const QJsonObject diagnostics = QJsonDocument::fromJson(result.diagnosticsJson.toUtf8()).object();
    const QString jsonDifference = compareJson(QStringLiteral("diagnostics"), diagnostics, golden.diagnostics);
    QVERIFY2(jsonDifference.isEmpty(), qPrintable(jsonDifference));
    if (exactParityRequested()) {
        QCOMPARE(result.diagnosticsJson.toUtf8(),
                 QJsonDocument(golden.diagnostics).toJson(QJsonDocument::Compact));
    }

    // Unwrapped: no adjacent step is a wrap
    QVERIFY(largestStep(result.roll) <= 180.0);
    QVERIFY(largestStep(result.pitch) <= 180.0);
    QVERIFY(largestStep(result.yaw) <= 180.0);
    if (name == QStringLiteral("stationary_spin"))
        QVERIFY(qAbs(result.yaw.last() - result.yaw.first()) > 360.0);
}

void FusionParityTest::rejectionFixturesMatchGolden_data()
{
    QTest::addColumn<QString>("name");
    for (const FusionFixture &fixture : fusionFixtures()) {
        if (!fixture.expectSuccess)
            QTest::newRow(qPrintable(fixture.name)) << fixture.name;
    }
}

void FusionParityTest::rejectionFixturesMatchGolden()
{
    QFETCH(QString, name);
    const FusionGolden golden = loadFusionGolden(name);
    QCOMPARE(golden.outcome, QStringLiteral("rejected"));
    QVERIFY(golden.diagnostics.contains(QStringLiteral("failure")));

    const Fusion::Result result = runFixture(name);
    QVERIFY(result.outcome == Fusion::Outcome::Rejected);
    QCOMPARE(result.reason, golden.diagnostics.value(QStringLiteral("failure")).toString());
    QVERIFY(allArraysEmpty(result));

    const QJsonObject diagnostics = QJsonDocument::fromJson(result.diagnosticsJson.toUtf8()).object();
    QCOMPARE(diagnostics.keys(), QStringList({QStringLiteral("algorithm"), QStringLiteral("failure")}));
    QCOMPARE(diagnostics, golden.diagnostics);
}

void FusionParityTest::progressMatchesReferenceBoundaries()
{
    const QString name = QStringLiteral("coarse_maneuver");
    const FusionGolden golden = loadFusionGolden(name);

    QStringList received;
    const Fusion::Result result = Fusion::run(toChannels(fusionFixture(name)),
        [&received](const QString &text) { received.append(referenceProgressText(text)); });
    QVERIFY(result.outcome == Fusion::Outcome::Succeeded);
    QVERIFY(golden.progress.size() > 4);
    QCOMPARE(received, golden.progress);
}

void FusionParityTest::cancelAtEachKindOfBoundary_data()
{
    QTest::addColumn<int>("cancelAtCall");
    QTest::addColumn<QString>("lastText");
    QTest::newRow("before the fit") << 1 << QStringLiteral("Starting fit");
    QTest::newRow("graph construction") << 2 << QStringLiteral("Integrating IMU factors");
    QTest::newRow("first iteration") << 3 << QStringLiteral("Pass 1, iteration 1");
    QTest::newRow("fourth boundary") << 4 << QStringLiteral("Pass 1, iteration 2");
}

void FusionParityTest::cancelAtEachKindOfBoundary()
{
    QFETCH(int, cancelAtCall);
    QFETCH(QString, lastText);
    const QString name = QStringLiteral("coarse_maneuver");
    const Fusion::Channels channels = toChannels(fusionFixture(name));

    QStringList received;
    int calls = 0;
    const Fusion::Result cancelled = Fusion::run(channels,
        [&received](const QString &text) { received.append(text); },
        [&calls, cancelAtCall]() { return ++calls >= cancelAtCall; });

    QVERIFY(cancelled.outcome == Fusion::Outcome::Cancelled);
    QVERIFY(cancelled.reason.isEmpty());
    QVERIFY(cancelled.diagnosticsJson.isEmpty());
    QVERIFY(allArraysEmpty(cancelled));
    // The cancelling boundary is the last one reached: it returned promptly.
    QCOMPARE(received.size(), cancelAtCall);
    QCOMPARE(calls, cancelAtCall);
    QCOMPARE(received.last(), lastText);

    // Nothing of the abandoned run leaks into the next one.
    const Fusion::Result after = Fusion::run(channels);
    QVERIFY(after.outcome == Fusion::Outcome::Succeeded);
    const QString difference = channelsDifference(after, loadFusionGolden(name));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

void FusionParityTest::cancelNeverRequestedChangesNothing()
{
    const Fusion::Channels channels = toChannels(fusionFixture(QStringLiteral("coarse_maneuver")));
    const Fusion::Result plain = Fusion::run(channels);

    QStringList received;
    const Fusion::Result observed = Fusion::run(channels,
        [&received](const QString &text) { received.append(text); },
        []() { return false; });

    QVERIFY(plain.outcome == Fusion::Outcome::Succeeded);
    QVERIFY(!received.isEmpty());
    const QString difference = differenceBetween(plain, observed);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

void FusionParityTest::twoRunsAreBitIdentical()
{
    // The solver runs its eliminations on TBB threads (tst_fusion_kernel's
    // solverUsesTbb asserts that); the result must not depend on scheduling.
    const Fusion::Result first = runFixture(QStringLiteral("stationary_spin"));
    const Fusion::Result second = runFixture(QStringLiteral("stationary_spin"));
    QVERIFY(first.outcome == Fusion::Outcome::Succeeded);
    const QString difference = differenceBetween(first, second);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

void FusionParityTest::workerThreadMatchesMainThread()
{
    const Fusion::Channels channels = toChannels(fusionFixture(QStringLiteral("stationary_spin")));
    const Fusion::Result onMain = Fusion::run(channels);

    // What the job queue's worker does: another thread, with a 64 MiB stack.
    Fusion::Result onWorker;
    std::unique_ptr<QThread> worker(QThread::create([&] { onWorker = Fusion::run(channels); }));
    worker->setStackSize(64u * 1024u * 1024u);
    worker->start();
    QVERIFY(worker->wait());

    QVERIFY(onMain.outcome == Fusion::Outcome::Succeeded);
    const QString difference = differenceBetween(onMain, onWorker);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

void FusionParityTest::resultIsIndependentOfCallerState()
{
    const QString name = QStringLiteral("coarse_linear");
    const Fusion::Result reference = runFixture(name);

    // The result owns its data: nothing refers back to the caller's channels.
    auto original = std::make_unique<Fusion::Channels>(toChannels(fusionFixture(name)));
    const Fusion::Channels copy = *original;
    original.reset();
    const Fusion::Result fromCopy = Fusion::run(copy);
    QVERIFY(fromCopy.outcome == Fusion::Outcome::Succeeded);
    const QString difference = differenceBetween(reference, fromCopy);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    // No input at all is a rejection like any other.
    const Fusion::Result empty = Fusion::run(Fusion::Channels{});
    QVERIFY(empty.outcome == Fusion::Outcome::Rejected);
    QCOMPARE(empty.reason,
             QStringLiteral("Sensor fusion needs GNSS, IMU and shared UTC time conversion"));
    QVERIFY(allArraysEmpty(empty));
}

FLYSIGHT_TEST_MAIN(FusionParityTest)
#include "tst_fusion_parity.moc"
