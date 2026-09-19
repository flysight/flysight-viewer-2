#include "oraclecatalogue.h"

#include <QDir>

#include "builtinfixture.h"
#include "fixturebuilder.h"

using namespace FlySight;

namespace FlySightTest {

namespace {

constexpr double T0 = DescentFixture::T0;

DependencyKey attr(const char *key)
{
    return DependencyKey::attribute(QString::fromLatin1(key));
}

void addColumns(QList<DependencyKey> &names, const char *sensor, std::initializer_list<const char *> columns)
{
    for (const char *column : columns)
        names.append(DependencyKey::measurement(QString::fromLatin1(sensor), QString::fromLatin1(column)));
}

// The header attributes every DescentFixture file carries.
Fs2FileBuilder withStandardVars(const QByteArray &sessionId)
{
    Fs2FileBuilder b;
    b.var("FIRMWARE_VER", "v2023.09.22")
     .var("SESSION_ID", sessionId)
     .var("DEVICE_ID", "test-device");
    return b;
}

// Header plus the first `rows` data rows of a file.
QByteArray firstRows(const QByteArray &bytes, int rows)
{
    QByteArray out;
    bool inData = false;
    int kept = 0;
    for (const QByteArray &line : bytes.split('\n')) {
        if (line.isEmpty())
            continue;
        if (inData) {
            if (kept >= rows)
                break;
            ++kept;
        }
        out += line + '\n';
        if (line == "$DATA")
            inData = true;
    }
    return out;
}

} // namespace

QList<DependencyKey> oracleCatalogue()
{
    QList<DependencyKey> names;

    // 1. every golden name
    for (const GoldenValue &golden : goldenValues())
        names.append(golden.name);

    // 2. every recorded column of the fixture, as an effective measurement
    addColumns(names, "GNSS", {"time", "lat", "lon", "hMSL", "velN", "velE", "velD", "hAcc", "vAcc", "sAcc", "numSV"});
    addColumns(names, "IMU", {"time", "wx", "wy", "wz", "ax", "ay", "az", "temperature"});
    addColumns(names, "MAG", {"time", "x", "y", "z", "temperature"});
    addColumns(names, "BARO", {"time", "pressure", "temperature"});
    addColumns(names, "TIME", {"time", "tow", "week"});
    addColumns(names, "VBAT", {"voltage"});

    // 3. attributes
    for (const char *key : {"SCHEMA_VER", "FIRMWARE_VER", "_DESCRIPTION", "_M", "_WSP_TOP_ALT",
                            "_WSP_BOTTOM_ALT", "_JUMPER_MASS", "_WIND_N"})
        names.append(attr(key));

    // 4. interpolation keys (the last one is malformed)
    for (const char *key : {"_M:IMU/_time/wx", "_M:IMU/_time/wTotal", "_EXIT_TIME:IMU/_time/wx",
                            "_WSP_ENTRY_TIME:GNSS/_time/z", "a:b/c"})
        names.append(attr(key));

    // 5. altitude attributes (registered and unregistered while the test runs)
    for (const char *key : {"_ALTITUDE_1000_M", "_ALTITUDE_2000_M", "_ALTITUDE_300_FT"})
        names.append(attr(key));

    // 6. names nothing provides
    names.append(DependencyKey::measurement(QStringLiteral("NOPE"), QStringLiteral("none")));
    names.append(attr("_NOPE"));

    return names;
}

QList<OracleFragment> oracleFragments(const QByteArray &sessionId)
{
    const QList<QByteArray> imuColumns = {"time", "wx", "wy", "wz", "ax", "ay", "az", "temperature"};
    const QList<QByteArray> imuUnits = {"s", "deg/s", "deg/s", "deg/s", "g", "g", "g", "deg C"};

    const QByteArray track = DescentFixture::trackFile(sessionId).toBytes();
    const QByteArray sensor = DescentFixture::sensorFile(sessionId).toBytes();

    Fs2FileBuilder imuAlt = withStandardVars(sessionId);
    imuAlt.sensor("IMU", imuColumns, imuUnits)
          .row("IMU", "10,30,40,0,0,0,2,41")
          .row("IMU", "20,60,80,0,0,0,2,41")
          .row("IMU", "30,90,120,0,0,0,2,41");

    Fs2FileBuilder imuSi = withStandardVars(sessionId);
    imuSi.sensor("IMU", imuColumns, {"s", "deg/s", "deg/s", "deg/s", "m/s^2", "m/s^2", "m/s^2", "degC"})
         .row("IMU", "10,3,4,0,0,0,9.80665,40")
         .row("IMU", "20,6,8,0,0,0,9.80665,40")
         .row("IMU", "30,9,12,0,0,0,9.80665,40");

    Fs2FileBuilder imuWTotal = withStandardVars(sessionId);
    imuWTotal.sensor("IMU", imuColumns + QList<QByteArray>{"wTotal"}, imuUnits + QList<QByteArray>{"deg/s"})
             .row("IMU", "10,3,4,0,0,0,1,40,99")
             .row("IMU", "20,6,8,0,0,0,1,40,99")
             .row("IMU", "30,9,12,0,0,0,1,40,99");

    Fs2FileBuilder baroOnly = withStandardVars(sessionId);
    baroOnly.sensor("BARO", {"time", "pressure", "temperature"}, {"s", "Pa", "deg C"})
            .row("BARO", "10,95000,15")
            .row("BARO", "20,94000,15")
            .row("BARO", "30,93000,15");

    QByteArray conflict = sensor;
    conflict.replace("v2023.09.22", "v2024.01.01");

    return {
        {QStringLiteral("TRACK"),       QStringLiteral("TRACK.CSV"),  track},
        {QStringLiteral("SENSOR"),      QStringLiteral("SENSOR.CSV"), sensor},
        {QStringLiteral("SENSOR_V2"),   QStringLiteral("SENSOR.CSV"),
         DescentFixture::sensorFile(sessionId).var("SCHEMA_VER", "2").toBytes()},
        {QStringLiteral("SENSOR_V1"),   QStringLiteral("SENSOR.CSV"),
         DescentFixture::sensorFile(sessionId).var("SCHEMA_VER", "1").toBytes()},
        {QStringLiteral("IMU_ALT"),     QStringLiteral("SENSOR.CSV"), imuAlt.toBytes()},
        {QStringLiteral("IMU_SI"),      QStringLiteral("SENSOR.CSV"), imuSi.toBytes()},
        {QStringLiteral("IMU_WTOTAL"),  QStringLiteral("SENSOR.CSV"), imuWTotal.toBytes()},
        {QStringLiteral("TRACK_SHORT"), QStringLiteral("TRACK.CSV"),  firstRows(track, 120)},
        {QStringLiteral("BARO_ONLY"),   QStringLiteral("SENSOR.CSV"), baroOnly.toBytes()},
        {QStringLiteral("CONFLICT"),    QStringLiteral("SENSOR.CSV"), conflict},
    };
}

QStringList writeOracleFragments(const QList<OracleFragment> &fragments, const QString &root)
{
    QStringList paths;
    for (const OracleFragment &fragment : fragments) {
        const QString folder = root + QLatin1Char('/') + fragment.name + QStringLiteral("/24-01-01/12-00-00");
        if (!QDir().mkpath(folder))
            qFatal("oracle: could not create %s", qPrintable(folder));
        const QString path = folder + QLatin1Char('/') + fragment.fileName;
        if (!writeFile(path, fragment.bytes))
            qFatal("oracle: could not write %s", qPrintable(path));
        paths.append(path);
    }
    return paths;
}

QList<OracleAttributeEdit> oracleAttributeEdits()
{
    const QVariant remove;
    return {
        {QStringLiteral("_GROUND_ELEV"),         {0.0, 50.0, 100.0, remove}},
        {QStringLiteral("_EXIT_TIME"),           {T0 + 12.0, T0 + 30.0, remove}},
        {QStringLiteral("_TIME_FIT_A"),          {QStringLiteral("2"), QStringLiteral("1"), remove}},
        {QStringLiteral("_FLARE_START_TIME"),    {T0 + 150.0, remove}},
        {QStringLiteral("_ANALYSIS_END_TIME"),   {T0 + 250.0, remove}},
        {QStringLiteral("_START_TIME"),          {T0 + 1.0, remove}},
        {QStringLiteral("_WSP_TOP_ALT"),         {2400.0, 3000.0, remove}},
        {QStringLiteral("_WSP_BOTTOM_ALT"),      {-50.0, 1000.0, remove}},
        {QStringLiteral("_WSP_ENTRY_TIME"),      {T0 + 45.0, remove}},
        {QStringLiteral("_SP_WINDOW_START_ALT"), {3000.0, remove}},
        {QStringLiteral("_M"),                   {T0 + 15.0, T0 + 25.0, T0 + 500.0, remove}},
        {QStringLiteral("_WIND_N"),              {0.0, 5.0}},
        {QStringLiteral("_JUMPER_MASS"),         {80.0, 1.0}},
        {QStringLiteral("_DESCRIPTION"),         {QStringLiteral("x"), QStringLiteral("y")}},
    };
}

CalculationEngine::Value oracleRead(const SessionData &session, const DependencyKey &name)
{
    CalculationEngine::Value value;
    if (name.type == DependencyKey::Type::Attribute) {
        value.attribute = session.getAttribute(name.attributeKey);
        value.available = value.attribute.isValid();
    } else {
        value.samples = session.getMeasurement(name.measurementKey.first, name.measurementKey.second);
        value.unit = session.effectiveUnit(name.measurementKey.first, name.measurementKey.second);
        value.available = session.calculationEngine().isAvailable(name);
    }
    return value;
}

QString oracleDescribe(const CalculationEngine::Value &value)
{
    if (!value.available)
        return QStringLiteral("<unavailable>");
    if (value.attribute.isValid()) {
        return QStringLiteral("%1(%2)").arg(QString::fromLatin1(value.attribute.typeName()),
                                           value.attribute.userType() == QMetaType::Double
                                               ? QString::number(value.attribute.toDouble(), 'g', 17)
                                               : value.attribute.toString());
    }
    QString text = QStringLiteral("[%1 samples, unit '%2'").arg(value.samples.size()).arg(value.unit);
    for (qsizetype i = 0; i < value.samples.size() && i < 3; ++i)
        text += QStringLiteral(", %1").arg(QString::number(value.samples.at(i), 'g', 17));
    return text + QLatin1Char(']');
}

void OracleOpLog::append(int step, const QString &session, const QString &text)
{
    m_lines.append(QStringLiteral("#%1 %2 %3").arg(step).arg(session, text));
}

void OracleOpLog::dump() const
{
    const qsizetype first = qMax<qsizetype>(0, m_lines.size() - 300);
    for (qsizetype i = first; i < m_lines.size(); ++i)
        qWarning("%s", qPrintable(m_lines.at(i)));
}

QList<unsigned> oracleSeeds(unsigned first, unsigned last)
{
    const QByteArray text = qgetenv("FLYSIGHT_ORACLE_SEEDS").trimmed();
    if (!text.isEmpty()) {
        const qsizetype dash = text.indexOf('-');
        bool okFirst = false, okLast = false;
        const unsigned a = (dash < 0 ? text : text.left(dash)).toUInt(&okFirst);
        const unsigned b = dash < 0 ? a : text.mid(dash + 1).toUInt(&okLast);
        if (okFirst && (dash < 0 || okLast) && a <= b) {
            first = a;
            last = b;
        } else {
            qFatal("FLYSIGHT_ORACLE_SEEDS: expected '<seed>' or '<first>-<last>', got '%s'", text.constData());
        }
    }
    QList<unsigned> seeds;
    for (unsigned seed = first; seed <= last; ++seed)
        seeds.append(seed);
    return seeds;
}

int oracleOperationCount(int defaultOps)
{
    bool ok = false;
    const int ops = qgetenv("FLYSIGHT_ORACLE_OPS").trimmed().toInt(&ok);
    return ok && ops > 0 ? ops : defaultOps;
}

} // namespace FlySightTest
