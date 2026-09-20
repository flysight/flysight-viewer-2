#include "fixturebuilder.h"

#include <QFile>

namespace FlySightTest {

// ─────────────────────────────── Fs2FileBuilder

Fs2FileBuilder &Fs2FileBuilder::flysVersion(const QByteArray &v)
{
    m_version = v;
    return *this;
}

Fs2FileBuilder &Fs2FileBuilder::var(const QByteArray &key, const QByteArray &value)
{
    m_varLines.append("$VAR," + key + "," + value);
    return *this;
}

Fs2FileBuilder &Fs2FileBuilder::sensor(const QByteArray &name,
                                       const QList<QByteArray> &columns,
                                       const QList<QByteArray> &units)
{
    m_sensors.append({name, columns, units});
    return *this;
}

Fs2FileBuilder &Fs2FileBuilder::row(const QByteArray &sensor, const QByteArray &csvValues)
{
    m_dataLines.append("$" + sensor + "," + csvValues);
    return *this;
}

Fs2FileBuilder &Fs2FileBuilder::rawHeaderLine(const QByteArray &line)
{
    m_rawHeaderLines.append(line);
    return *this;
}

Fs2FileBuilder &Fs2FileBuilder::rawDataLine(const QByteArray &line)
{
    m_dataLines.append(line);
    return *this;
}

Fs2FileBuilder &Fs2FileBuilder::lineEnding(const QByteArray &eol)
{
    m_eol = eol;
    return *this;
}

QByteArray Fs2FileBuilder::toBytes() const
{
    QByteArray out;

    out += "$FLYS," + m_version + m_eol;

    for (const QByteArray &line : m_varLines)
        out += line + m_eol;

    for (const Sensor &s : m_sensors) {
        out += "$COL," + s.name;
        for (const QByteArray &c : s.columns)
            out += "," + c;
        out += m_eol;

        if (!s.units.isEmpty()) {
            out += "$UNIT," + s.name;
            for (const QByteArray &u : s.units)
                out += "," + u;
            out += m_eol;
        }
    }

    for (const QByteArray &line : m_rawHeaderLines)
        out += line + m_eol;

    out += "$DATA" + m_eol;

    for (const QByteArray &line : m_dataLines)
        out += line + m_eol;

    return out;
}

bool Fs2FileBuilder::write(const QString &path) const
{
    return writeFile(path, toBytes());
}

// ─────────────────────────────── Fs1FileBuilder

Fs1FileBuilder &Fs1FileBuilder::columns(const QList<QByteArray> &names)
{
    m_columns = names;
    return *this;
}

Fs1FileBuilder &Fs1FileBuilder::units(const QList<QByteArray> &units)
{
    m_units = units;
    m_hasUnits = true;
    return *this;
}

Fs1FileBuilder &Fs1FileBuilder::row(const QByteArray &csvValues)
{
    m_rows.append(csvValues);
    return *this;
}

QByteArray Fs1FileBuilder::toBytes() const
{
    QByteArray out;

    out += m_columns.join(',') + "\n";
    if (m_hasUnits)
        out += m_units.join(',') + "\n";
    for (const QByteArray &r : m_rows)
        out += r + "\n";

    return out;
}

bool Fs1FileBuilder::write(const QString &path) const
{
    return writeFile(path, toBytes());
}

// ─────────────────────────────── file helpers

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(contents) == contents.size();
}

QByteArray readFileBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

// ─────────────────────────────── canned fixtures

namespace Fixtures {

namespace {

Fs2FileBuilder withStandardVars(const QByteArray &sessionId)
{
    Fs2FileBuilder b;
    b.var("FIRMWARE_VER", "v2023.09.22")
     .var("SESSION_ID", sessionId)
     .var("DEVICE_ID", "test-device");
    return b;
}

} // namespace

Fs2FileBuilder sensorFile(const QByteArray &sessionId)
{
    // Legacy gyro rates: +1024, -2048 and 0 raw counts at 2000/32768 deg/s per
    // count. The gyro columns are out of order so that anything keyed on
    // column position instead of column name is caught.
    Fs2FileBuilder b = withStandardVars(sessionId);
    b.sensor("IMU",
             {"time", "wy", "ax", "wz", "wx", "temperature"},
             {"s", "deg/s", "g", "deg/s", "deg/s", "deg C"})
     .sensor("MAG",
             {"time", "x", "y", "z", "temperature"},
             {"s", "gauss", "gauss", "gauss", "deg C"})
     .row("IMU", "3,-125,1,0,62.5,40")
     .row("MAG", "3,1,0,-0.5,40");
    return b;
}

Fs2FileBuilder trackFile(const QByteArray &sessionId)
{
    Fs2FileBuilder b = withStandardVars(sessionId);
    b.sensor("GNSS",
             {"time", "lat", "lon", "hMSL", "velN", "velE", "velD", "hAcc", "vAcc", "sAcc", "numSV"},
             {"", "deg", "deg", "m", "m/s", "m/s", "m/s", "m", "m", "m", ""})
     .row("GNSS", "2024-01-01T12:00:00.000Z,45.5,-73.25,4000,10,-20,5,1.5,2.5,0.25,12")
     .row("GNSS", "2024-01-01T12:00:00.200Z,45.5001,-73.2501,3999,10.5,-20.5,5.5,1.5,2.5,0.25,12")
     .row("GNSS", "2024-01-01T12:00:00.400Z,45.5002,-73.2502,3998,11,-21,6,1.5,2.5,0.25,13");
    return b;
}

} // namespace Fixtures

} // namespace FlySightTest
