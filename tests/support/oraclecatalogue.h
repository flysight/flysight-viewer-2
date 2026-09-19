#ifndef FLYSIGHTTEST_ORACLECATALOGUE_H
#define FLYSIGHTTEST_ORACLECATALOGUE_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>

#include "dependencykey.h"
#include "engine/calculationengine.h"
#include "sessiondata.h"

// Shared vocabulary of the session-level idempotency oracle
// (tst_session_oracle) and the workflow test: the names that are checked, the
// files that are merged, the edits that are made, and the operation log a
// failure prints. Everything here is a fixed, ordered list - a randomized test
// chooses by index, never by iterating a hash container.

namespace FlySightTest {

/// Every name the oracle checks, in a fixed order: the golden names (an output
/// of each built-in and of the interpolation family), every recorded column of
/// DescentFixture as an EFFECTIVE measurement, header and Viewer attributes,
/// interpolation keys (one malformed), the altitude attributes, and two names
/// nothing provides. _IMPORT_TIME (wall clock) is deliberately absent.
QList<FlySight::DependencyKey> oracleCatalogue();

/// One file that can be imported into, or merged with, a DescentFixture session.
struct OracleFragment {
    QString    name;        ///< op-log name, e.g. "IMU_SI"
    QString    fileName;    ///< e.g. "SENSOR.CSV"
    QByteArray bytes;
};

/// The fragment pool for one SESSION_ID, in a fixed order:
///   TRACK, SENSOR            DescentFixture::trackFile / sensorFile
///   SENSOR_V2, SENSOR_V1     SENSOR + $VAR,SCHEMA_VER,2 / 1 (escape hatch; the second to arrive conflicts)
///   IMU_ALT                  IMU only, other gyro / acceleration values
///   IMU_SI                   IMU only, released-style units (m/s^2, degC)
///   IMU_WTOTAL               SENSOR's IMU plus a recorded wTotal column (stored beats derived)
///   TRACK_SHORT              rows 0..119 of TRACK (descent incomplete)
///   BARO_ONLY                BARO only
///   CONFLICT                 SENSOR with another FIRMWARE_VER (always rejected)
/// Every IMU fragment has three rows and every GNSS fragment replaces every
/// GNSS column, so no merge is ever ragged.
QList<OracleFragment> oracleFragments(const QByteArray &sessionId);

/// Writes every fragment to <root>/<name>/24-01-01/12-00-00/<fileName> and
/// returns the paths in fragment order.
QStringList writeOracleFragments(const QList<OracleFragment> &fragments, const QString &root);

/// An attribute and the values an edit may give it. An invalid QVariant means
/// "remove the attribute".
struct OracleAttributeEdit {
    QString key;
    QList<QVariant> values;
};
QList<OracleAttributeEdit> oracleAttributeEdits();

/// What an ordinary consumer reads for `name`, in the oracle's value form.
FlySight::CalculationEngine::Value oracleRead(const FlySight::SessionData &session,
                                              const FlySight::DependencyKey &name);

/// Text of a value, for failure messages.
QString oracleDescribe(const FlySight::CalculationEngine::Value &value);

/// "#<step> <session> <op> <arguments>" lines, kept in memory and printed with
/// qWarning (the last 300 lines) when a check fails.
class OracleOpLog {
public:
    void append(int step, const QString &session, const QString &text);
    const QStringList &lines() const { return m_lines; }
    void dump() const;

private:
    QStringList m_lines;
};

/// Seeds from FLYSIGHT_ORACLE_SEEDS ("7" or "1-500"), else first..last.
QList<unsigned> oracleSeeds(unsigned first, unsigned last);
/// Operation count from FLYSIGHT_ORACLE_OPS, else `defaultOps`.
int oracleOperationCount(int defaultOps);

} // namespace FlySightTest

#endif // FLYSIGHTTEST_ORACLECATALOGUE_H
