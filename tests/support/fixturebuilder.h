#ifndef FLYSIGHTTEST_FIXTUREBUILDER_H
#define FLYSIGHTTEST_FIXTUREBUILDER_H

#include <QByteArray>
#include <QList>
#include <QString>

namespace FlySightTest {

/// Builds the bytes of a FlySight 2 ("$FLYS") file.
///
/// Every value is passed as text and emitted verbatim, so a test owns the
/// exact bytes in the file and its expectations stay literals. Nothing is
/// added implicitly: no $VAR, no $UNIT, no reformatting of numeric text.
///
/// Layout of toBytes(): "$FLYS,<version>", the $VAR lines in call order, a
/// "$COL" (and, when units were given, "$UNIT") line per sensor in call order,
/// the raw header lines, "$DATA", then the data lines in call order.
class Fs2FileBuilder {
public:
    Fs2FileBuilder &flysVersion(const QByteArray &v);                  ///< default "1" -> "$FLYS,1"
    Fs2FileBuilder &var(const QByteArray &key, const QByteArray &value);   ///< "$VAR,key,value"
    /// "$COL,name,..." plus "$UNIT,name,..."; an empty units list emits no $UNIT
    /// line, a shorter one is emitted as given.
    Fs2FileBuilder &sensor(const QByteArray &name,
                           const QList<QByteArray> &columns,
                           const QList<QByteArray> &units);
    Fs2FileBuilder &row(const QByteArray &sensor, const QByteArray &csvValues);  ///< "$IMU,3,-125,..."
    Fs2FileBuilder &rawHeaderLine(const QByteArray &line);   ///< verbatim, before $DATA
    Fs2FileBuilder &rawDataLine(const QByteArray &line);     ///< verbatim, after $DATA
    Fs2FileBuilder &lineEnding(const QByteArray &eol);       ///< default "\n"; "\r\n" allowed

    QByteArray toBytes() const;
    bool write(const QString &path) const;

private:
    struct Sensor {
        QByteArray name;
        QList<QByteArray> columns;
        QList<QByteArray> units;
    };

    QByteArray m_version = "1";
    QByteArray m_eol = "\n";
    QList<QByteArray> m_varLines;
    QList<Sensor> m_sensors;
    QList<QByteArray> m_rawHeaderLines;
    QList<QByteArray> m_dataLines;
};

/// Builds the bytes of a FlySight 1 file: a column line, a unit line, rows.
/// The column list must start with time,lat,lon,hMSL for the importer to
/// recognise the format. Lines are emitted only if requested.
class Fs1FileBuilder {
public:
    Fs1FileBuilder &columns(const QList<QByteArray> &names);
    Fs1FileBuilder &units(const QList<QByteArray> &units);
    Fs1FileBuilder &row(const QByteArray &csvValues);

    QByteArray toBytes() const;
    bool write(const QString &path) const;

private:
    QList<QByteArray> m_columns;
    QList<QByteArray> m_units;
    bool m_hasUnits = false;
    QList<QByteArray> m_rows;
};

/// Writes contents to path, replacing any existing file. True on success.
bool writeFile(const QString &path, const QByteArray &contents);

/// Returns the bytes of the file, or an empty array if it cannot be read.
QByteArray readFileBytes(const QString &path);

/// Canned, deliberately tiny fixtures. Both carry DEVICE_ID so the importer
/// never walks parent directories looking for FLYSIGHT.TXT.
namespace Fixtures {

/// SENSOR.CSV-like: IMU (gyro columns deliberately out of order) and MAG, one row each.
Fs2FileBuilder sensorFile(const QByteArray &sessionId = "test-session");

/// TRACK.CSV-like: GNSS, three rows 200 ms apart starting 2024-01-01T12:00:00.000Z.
Fs2FileBuilder trackFile(const QByteArray &sessionId = "test-session");

} // namespace Fixtures

} // namespace FlySightTest

#endif // FLYSIGHTTEST_FIXTUREBUILDER_H
