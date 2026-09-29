#include "fusionsessions.h"

#include <cmath>
#include <cstring>

#include "fusion/fusionregistration.h"
#include "sessionmodel.h"
#include "testenvironment.h"

using namespace FlySight;

namespace FlySightTest {

namespace {

void addIdentity(SessionData &session, const QString &sessionId)
{
    session.setAttribute(SessionKeys::SessionId, sessionId);
    session.setAttribute(SessionKeys::DeviceId, QStringLiteral("fusion-test"));
    // Without it the conversion layer reads the gyro channels as legacy data
    // and multiplies them by 1.14688: nothing would match the goldens.
    session.setAttribute(QStringLiteral("SCHEMA_VER"), QStringLiteral("2"));
}

void addGnssSide(SessionData &session, const FusionFixture &f, const QString &sAccName = QStringLiteral("sAcc"))
{
    session.setSourceMeasurement("GNSS", "time", f.gnssTime, "s");
    session.setSourceMeasurement("GNSS", "hAcc", f.hAcc, "m");
    session.setSourceMeasurement("GNSS", "vAcc", f.vAcc, "m");
    session.setSourceMeasurement(QStringLiteral("GNSS"), sAccName, f.sAcc, QStringLiteral("m/s"));
    session.setSourceMeasurement("Local", "north", f.north, "m");
    session.setSourceMeasurement("Local", "east", f.east, "m");
    session.setSourceMeasurement("Local", "down", f.down, "m");
    session.setSourceMeasurement("Local", "velN", f.velN, "m/s");
    session.setSourceMeasurement("Local", "velE", f.velE, "m/s");
    session.setSourceMeasurement("Local", "velD", f.velD, "m/s");

    session.setAttribute(SessionKeys::LocalOriginIndex, QVariant::fromValue(qlonglong(f.originIndex)));
    session.setAttribute(SessionKeys::LocalOriginLat, f.originLat);
    session.setAttribute(SessionKeys::LocalOriginLon, f.originLon);
    session.setAttribute(SessionKeys::LocalOriginHmsl, f.originHMSL);

    // An exact fit, as strings like the calculated ones: system = utc - b
    session.setAttribute(SessionKeys::TimeFitA, QStringLiteral("1"));
    session.setAttribute(SessionKeys::TimeFitB, QStringLiteral("1699999900"));
}

void addImuSide(SessionData &session, const FusionFixture &f)
{
    session.setSourceMeasurement("IMU", SessionKeys::Time, f.imuTime, "s");
    session.setSourceMeasurement("IMU", "ax", f.ax, "m/s^2");
    session.setSourceMeasurement("IMU", "ay", f.ay, "m/s^2");
    session.setSourceMeasurement("IMU", "az", f.az, "m/s^2");
    session.setSourceMeasurement("IMU", "wx", f.wx, "deg/s");
    session.setSourceMeasurement("IMU", "wy", f.wy, "deg/s");
    session.setSourceMeasurement("IMU", "wz", f.wz, "deg/s");
    // The device's unit text; the conversion layer serves it unchanged with
    // label degC. Conditional so that a test can build a session lacking the
    // column from a fixture copy with the array cleared.
    if (!f.imuTemperature.isEmpty())
        session.setSourceMeasurement("IMU", "temperature", f.imuTemperature, "deg C");
}

// What the Heading, Pitch and Roll plots read besides the fit's outputs: the
// GNSS course their heading is referenced to, from the fixture's velocity
// stored again under GNSS, and the exit marker, the course reference's
// default. The fit reads neither, so its inputs stay the fixture's.
void addPlotInputs(SessionData &session, const FusionFixture &f)
{
    session.setSourceMeasurement("GNSS", "velN", f.velN, "m/s");
    session.setSourceMeasurement("GNSS", "velE", f.velE, "m/s");
    session.setAttribute(SessionKeys::ExitTime, kFixtureExitTime);
}

bool sameSamples(const QVector<double> &a, const QVector<double> &b)
{
    return a.size() == b.size()
        && (a.isEmpty() || std::memcmp(a.constData(), b.constData(), sizeof(double) * size_t(a.size())) == 0);
}

// The premise of this file: what the fit will read is the fixture, bit for
// bit. Checked in debug builds (Q_ASSERT), on a copy (state only, its own
// engine), so the session handed out has computed nothing; the tests assert it
// in every build (tst_fusion_session::inputsAreBitIdenticalToFixture).
bool inputsMatchFixture(const SessionData &session, const FusionFixture &f, bool withImu)
{
    const SessionData probe = session;
    bool same = sameSamples(probe.getMeasurement("GNSS", SessionKeys::Time), f.gnssTime)
        && sameSamples(probe.getMeasurement("Local", "north"), f.north)
        && sameSamples(probe.getMeasurement("Local", "east"), f.east)
        && sameSamples(probe.getMeasurement("Local", "down"), f.down)
        && sameSamples(probe.getMeasurement("Local", "velN"), f.velN)
        && sameSamples(probe.getMeasurement("Local", "velE"), f.velE)
        && sameSamples(probe.getMeasurement("Local", "velD"), f.velD)
        && sameSamples(probe.getMeasurement("GNSS", "hAcc"), f.hAcc)
        && sameSamples(probe.getMeasurement("GNSS", "vAcc"), f.vAcc)
        && sameSamples(probe.getMeasurement("GNSS", "sAcc"), f.sAcc)
        && probe.getAttribute(SessionKeys::LocalOriginIndex).toLongLong() == f.originIndex;
    if (withImu) {
        same = same
            && sameSamples(probe.getMeasurement("IMU", SessionKeys::Time), f.imuTime)
            && sameSamples(probe.getMeasurement("IMU", "ax"), f.ax)
            && sameSamples(probe.getMeasurement("IMU", "ay"), f.ay)
            && sameSamples(probe.getMeasurement("IMU", "az"), f.az)
            && sameSamples(probe.getMeasurement("IMU", "wx"), f.wx)
            && sameSamples(probe.getMeasurement("IMU", "wy"), f.wy)
            && sameSamples(probe.getMeasurement("IMU", "wz"), f.wz)
            && sameSamples(probe.getMeasurement("IMU", "temperature"), f.imuTemperature);
    }
    return same;
}

} // namespace

void registerFusionOnce()
{
    CalculationRegistry &registry = CalculationRegistry::instance();
    if (!registry.contains(QString::fromLatin1(Fusion::FitCalculationId)))
        Fusion::registerFusionCalculations(registry);
}

SessionData sessionFromFixture(const FusionFixture &fixture, const QString &sessionId)
{
    SessionData session;
    addIdentity(session, sessionId);
    addGnssSide(session, fixture);
    addImuSide(session, fixture);
    Q_ASSERT(inputsMatchFixture(session, fixture, true));
    return session;
}

SessionData sessionWithoutImu(const FusionFixture &fixture, const QString &sessionId)
{
    SessionData session;
    addIdentity(session, sessionId);
    addGnssSide(session, fixture);
    Q_ASSERT(inputsMatchFixture(session, fixture, false));
    return session;
}

SessionData fixtureSession(const QString &fixtureName, const QString &sessionId)
{
    const FusionFixture fixture = fusionFixture(fixtureName);
    SessionData session = sessionFromFixture(fixture, sessionId);
    addPlotInputs(session, fixture);
    return session;
}

SessionData fixtureSessionWithSAccStoredAs(const QString &fixtureName, const QString &sessionId,
                                          const QString &storedAs)
{
    const FusionFixture fixture = fusionFixture(fixtureName);
    SessionData session;
    addIdentity(session, sessionId);
    addGnssSide(session, fixture, storedAs);
    addImuSide(session, fixture);
    addPlotInputs(session, fixture);
    return session;
}

SessionData naturalSession(const QString &sessionId)
{
    constexpr double epoch = 1700000000.0;
    constexpr int fixes = 200;
    constexpr int lastImuSample = 4000;

    SessionData session;
    addIdentity(session, sessionId);

    QVector<double> gnssTime;
    for (int i = 0; i < fixes; ++i)
        gnssTime.append(epoch + .1 + i * .2);
    session.setSourceMeasurement("GNSS", "time", gnssTime, "s");
    session.setSourceMeasurement("GNSS", "lat", QVector<double>(fixes, 0.0), "deg");
    session.setSourceMeasurement("GNSS", "lon", QVector<double>(fixes, 0.0), "deg");
    session.setSourceMeasurement("GNSS", "hMSL", QVector<double>(fixes, 100.0), "m");
    session.setSourceMeasurement("GNSS", "velN", QVector<double>(fixes, 0.0), "m/s");
    session.setSourceMeasurement("GNSS", "velE", QVector<double>(fixes, 0.0), "m/s");
    session.setSourceMeasurement("GNSS", "velD", QVector<double>(fixes, 0.0), "m/s");
    session.setSourceMeasurement("GNSS", "hAcc", QVector<double>(fixes, 1.0), "m");
    session.setSourceMeasurement("GNSS", "vAcc", QVector<double>(fixes, 1.0), "m");
    session.setSourceMeasurement("GNSS", "sAcc", QVector<double>(fixes, .1), "m/s");

    QVector<double> imuTime;
    for (int i = 0; i <= lastImuSample; ++i)
        imuTime.append(100 + i * .01);
    const QVector<double> zeros(imuTime.size(), 0.0);
    session.setSourceMeasurement("IMU", "time", imuTime, "s");
    session.setSourceMeasurement("IMU", "ax", zeros, "m/s^2");
    session.setSourceMeasurement("IMU", "ay", zeros, "m/s^2");
    session.setSourceMeasurement("IMU", "az", QVector<double>(imuTime.size(), -9.80665), "m/s^2");
    session.setSourceMeasurement("IMU", "wx", zeros, "deg/s");
    session.setSourceMeasurement("IMU", "wy", zeros, "deg/s");
    session.setSourceMeasurement("IMU", "wz", zeros, "deg/s");
    session.setSourceMeasurement("IMU", "temperature", QVector<double>(imuTime.size(), 25.0), "deg C");

    // GPS week and time of week of the epoch; device time 100 s is the epoch
    const double week = std::floor((epoch - 315964800) / 604800);
    const double tow = epoch - 315964800 - week * 604800;
    session.setSourceMeasurement("TIME", "time", {100, 120, 140}, "s");
    session.setSourceMeasurement("TIME", "tow", {tow, tow + 20, tow + 40}, "s");
    session.setSourceMeasurement("TIME", "week", {week, week, week}, "");
    return session;
}

SessionData syntheticFitSession(const QString &sessionId, const QHash<QString, QVector<double>> &channels)
{
    // The unit text of each fit output, as the kernel's Result documents it
    const QHash<QString, QString> units{
        {QStringLiteral("_time"), QStringLiteral("s")},
        {QStringLiteral("north"), QStringLiteral("m")}, {QStringLiteral("east"), QStringLiteral("m")},
        {QStringLiteral("down"), QStringLiteral("m")},
        {QStringLiteral("velN"), QStringLiteral("m/s")}, {QStringLiteral("velE"), QStringLiteral("m/s")},
        {QStringLiteral("velD"), QStringLiteral("m/s")},
        {QStringLiteral("accN"), QStringLiteral("m/s^2")}, {QStringLiteral("accE"), QStringLiteral("m/s^2")},
        {QStringLiteral("accD"), QStringLiteral("m/s^2")},
        {QStringLiteral("roll"), QStringLiteral("deg")}, {QStringLiteral("pitch"), QStringLiteral("deg")},
        {QStringLiteral("yaw"), QStringLiteral("deg")},
        {QStringLiteral("qx"), QString()}, {QStringLiteral("qy"), QString()},
        {QStringLiteral("qz"), QString()}, {QStringLiteral("qw"), QString()}};
    Q_ASSERT(units.size() == fusionMeasurementNames().size());

    SessionData session;
    addIdentity(session, sessionId);
    for (auto it = channels.cbegin(); it != channels.cend(); ++it) {
        Q_ASSERT(fusionMeasurementNames().contains(it.key()));
        session.setSourceMeasurement(QStringLiteral("Fusion"), it.key(), it.value(), units.value(it.key()));
    }
    return session;
}

QStringList fusionMeasurementNames()
{
    return {
        QStringLiteral("_time"),
        QStringLiteral("north"), QStringLiteral("east"), QStringLiteral("down"),
        QStringLiteral("velN"), QStringLiteral("velE"), QStringLiteral("velD"),
        QStringLiteral("accN"), QStringLiteral("accE"), QStringLiteral("accD"),
        QStringLiteral("roll"), QStringLiteral("pitch"), QStringLiteral("yaw"),
        QStringLiteral("qx"), QStringLiteral("qy"), QStringLiteral("qz"), QStringLiteral("qw")
    };
}

QVector<PlotValue> fusionPlots()
{
    struct Row { const char *name; const char *units; const char *measurement; const char *type; };
    static const Row rows[] = {
        {"Elevation",                "m",     "z",             "altitude"},
        {"Horizontal acceleration",  "m/s^2", "accH",          "acceleration"},
        {"Vertical acceleration",    "m/s^2", "accD",          "acceleration"},
        {"Along-track acceleration", "m/s^2", "accAlongTrack", "acceleration"},
        {"Cross-track acceleration", "m/s^2", "accCrossTrack", "acceleration"},
        {"Heading",                  "deg",   "bodyHeading",   "angle"},
        {"Pitch",                    "deg",   "bodyPitch",     "angle"},
        {"Roll",                     "deg",   "bodyRoll",      "angle"},
    };

    QVector<PlotValue> plots;
    for (const Row &row : rows) {
        PlotValue plot;
        plot.category = QStringLiteral("Sensor fusion");
        plot.plotName = QString::fromLatin1(row.name);
        plot.plotUnits = QString::fromLatin1(row.units);
        plot.sensorID = QStringLiteral("Fusion");
        plot.measurementID = QString::fromLatin1(row.measurement);
        plot.measurementType = QString::fromLatin1(row.type);
        plots.append(plot);
    }
    return plots;
}

QString fusionRollAtExit()
{
    return SessionData::interpolationKey(SessionKeys::ExitTime, QStringLiteral("Fusion"),
                                         SessionKeys::Time, QStringLiteral("roll"));
}

QList<DependencyKey> fusionNames()
{
    QList<DependencyKey> names;
    for (const QString &name : fusionMeasurementNames())
        names.append(DependencyKey::measurement(QStringLiteral("Fusion"), name));
    names.append(DependencyKey::measurement(QStringLiteral("Fusion"), QStringLiteral("accH")));
    names.append(DependencyKey::measurement(QStringLiteral("Fusion"), QStringLiteral("_system_time")));
    names.append(DependencyKey::attribute(QStringLiteral("_FUSION_DIAGNOSTICS")));
    names.append(DependencyKey::attribute(fusionRollAtExit()));
    return names;
}

DependencyKey fusionKey(const QString &name)
{
    return DependencyKey::measurement(QStringLiteral("Fusion"), name);
}

QString goldenDifference(const SessionData &session, const FusionGolden &golden)
{
    for (const QString &name : fusionChannelNames()) {
        const QString difference = compareSamples(name, session.getMeasurement(QStringLiteral("Fusion"), name),
                                                  golden.channels.value(name));
        if (!difference.isEmpty())
            return difference;
    }
    return QString();
}

QString addSessions(SessionModel &model, const QList<SessionData> &sessions)
{
    model.mergeSessions(sessions);
    if (model.rowCount() != int(sessions.size()))
        return QStringLiteral("the model has %1 rows for %2 sessions").arg(model.rowCount()).arg(sessions.size());
    if (!waitForIdle(model))
        return QStringLiteral("the model did not become idle");
    for (int row = 0; row < model.rowCount(); ++row) {
        if (!std::as_const(model).rowAt(row).isLoaded())
            return QStringLiteral("row %1 is not loaded").arg(row);
    }
    return QString();
}

} // namespace FlySightTest
