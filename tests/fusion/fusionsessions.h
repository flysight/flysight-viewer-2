#ifndef FLYSIGHTTEST_FUSIONSESSIONS_H
#define FLYSIGHTTEST_FUSIONSESSIONS_H

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include "dependencykey.h"
#include "fusionfixtures.h"
#include "fusiongolden.h"
#include "plotregistry.h"
#include "sessiondata.h"

// The fusion fixtures as real sessions, for tests of sensor fusion as a
// registered calculation (tst_fusion_session, tst_fusion_derived,
// tst_fusion_jobs, tst_fusion_rows), and the helpers those tests share.
//
// "Stored data always wins": a measurement with source data is served by the
// conversion layer, not by a registered calculation. A fixture session stores
// the fit's inputs under their own names (Local/north, IMU/_time, the origin
// attributes, ...) with units the conversion layer passes through unchanged,
// so the eighteen measurements and the four origin attributes are
// bit-identical to the fixture, the four configuration attributes are read
// from their constant defaults, and the session-level results can be held to
// the goldens of the kernel.

namespace FlySight {
class SessionModel;
}

namespace FlySightTest {

/// The exact time fit every fixture session stores: utc = 1 * system + this.
constexpr double kFixtureTimeFitB = 1699999900.0;

/// Registers the fusion calculations on the global registry once per process
/// (after TestEnvironment::registerBuiltIns(), as the application does).
void registerFusionOnce();

/// Stored source data such that the 22 declared inputs other than the four
/// configuration attributes read back bit-identical to the fixture (those
/// four are not stored: the session reads their constant defaults):
///   GNSS/time (unit "s")            -> GNSS/_time through builtin.time.utc.GNSS
///   GNSS/hAcc, vAcc ("m"), sAcc ("m/s")
///   Local/north|east|down ("m"), Local/velN|velE|velD ("m/s")
///   IMU/_time ("s"), IMU/ax|ay|az ("m/s^2"), IMU/wx|wy|wz ("deg/s"),
///   IMU/temperature ("deg C": the device's unit text, served as degC unchanged)
///   stored attributes _LOCAL_ORIGIN_INDEX (qlonglong), _LOCAL_ORIGIN_LAT|LON|HMSL (double),
///   _TIME_FIT_A = "1", _TIME_FIT_B = "1699999900", SCHEMA_VER = 2 (the gyro
///   channels are read literally), SESSION_ID = sessionId, DEVICE_ID = "fusion-test"
/// Only fixtures whose columns have equal lengths per sensor may go into a
/// SessionModel (the saver refuses a ragged sensor): not reject_length.
FlySight::SessionData sessionFromFixture(const FusionFixture &fixture, const QString &sessionId);

/// The epoch of every fixture's GNSS time axis, and an exit marker inside
/// every success fixture's fit.
constexpr double kFixtureEpochUtc = 1700000000.0;
constexpr double kFixtureExitTime = kFixtureEpochUtc + 1.0;

/// sessionFromFixture(fusionFixture(fixtureName), sessionId) with what the
/// Heading, Pitch and Roll plots read besides the fit: a stored exit marker at
/// kFixtureExitTime (the course reference's default) and the fixture's
/// velocity stored again as GNSS/velN and velE ("m/s": the GNSS course their
/// heading is referenced to). The fit reads neither. Elevation also needs a
/// ground elevation, which a test stores when it wants that plot.
FlySight::SessionData fixtureSession(const QString &fixtureName, const QString &sessionId = QStringLiteral("f1"));

/// fixtureSession() with the GNSS/sAcc source data stored as GNSS/<storedAs>
/// (same samples, unit "m/s"). GNSS/sAcc then has no source data and no
/// built-in candidate, so only a registered calculation can provide it
/// (tst_fusion_store's lookup test).
FlySight::SessionData fixtureSessionWithSAccStoredAs(const QString &fixtureName, const QString &sessionId,
                                                     const QString &storedAs);

/// The same without any IMU column (spec acceptance 11).
FlySight::SessionData sessionWithoutImu(const FusionFixture &fixture, const QString &sessionId);

/// A recording as the importer would leave it: GNSS lat/lon/hMSL/velN/velE/velD/
/// hAcc/vAcc/sAcc/time, IMU time/ax../wz/temperature, TIME time/tow/week; nothing under Local,
/// no stored origin, no stored fit. Retyped from the reference's synthetic
/// session: epoch 1700000000, 200 fixes at .1 + i*.2 s with lat = lon = 0,
/// hMSL = 100, zero velocity, hAcc = vAcc = 1, sAcc = .1; IMU system time
/// 100 + i*.01, i = 0..4000, az = -9.80665 m/s^2, temperature 25 degC, the rest 0; TIME pulses
/// {100, 120, 140} with tow / tow+20 / tow+40. SCHEMA_VER = 2.
FlySight::SessionData naturalSession(const QString &sessionId);

/// The opposite premise to the fixture sessions: the fit's OUTPUTS stored as
/// data, so that what is derived from them can be tested without the solver.
///  - Each entry of `channels` becomes source data Fusion/<name>; only the
///    seventeen names of fusionMeasurementNames() are allowed (Q_ASSERT).
///  - Each is stored with the unit text of that output: _time "s";
///    north|east|down "m"; velN|velE|velD "m/s"; accN|accE|accD "m/s^2";
///    roll|pitch|yaw "deg"; qx..qw "". The conversion layer passes all of
///    these through unchanged (no schema row names Fusion).
///  - A reader of Fusion/<name> therefore gets exactly these samples, and
///    resolution never reaches builtin.fusion.fit: a measurement with source
///    data is served by the source conversions, and the engine never falls
///    through from source data to a derived candidate.
///  - The session also carries the identity attributes of the fixture
///    sessions (SESSION_ID = sessionId, DEVICE_ID, SCHEMA_VER = 2) and nothing
///    else; the test stores whatever attributes a derivation reads.
///  - For engine-level tests only. It never goes into a SessionModel: its fit
///    outputs are data, and the saver would write them into a session file.
FlySight::SessionData syntheticFitSession(const QString &sessionId,
                                          const QHash<QString, QVector<double>> &channels);

QStringList fusionMeasurementNames();   ///< the 17 literal names, in output order

/// The eight "Sensor fusion" plots as PlotValues, for PlotModel::setPlots(),
/// in the application's order: Elevation (z), the horizontal, vertical (accD),
/// along-track and cross-track accelerations, and Heading, Pitch and Roll
/// (bodyHeading, bodyPitch, bodyRoll). Mirrors the application's eight rows in
/// MainWindow::registerBuiltInPlots(), which is outside the test library
/// boundary; audit_cleanup pins that list at eight rows. Colours are
/// irrelevant here and left default.
QVector<FlySight::PlotValue> fusionPlots();

/// "Everything": the 17 measurements, Fusion/accH, Fusion/_system_time,
/// _FUSION_DIAGNOSTICS, and Fusion/roll interpolated at _EXIT_TIME.
QList<FlySight::DependencyKey> fusionNames();
/// The attribute name of Fusion/roll interpolated at _EXIT_TIME.
QString fusionRollAtExit();
/// Fusion/<name> as a dependency key.
FlySight::DependencyKey fusionKey(const QString &name);

/// Empty when the seventeen published channels of the session match the
/// golden (in the mode chosen by FLYSIGHT_FUSION_EXACT), else the first
/// channel's difference.
QString goldenDifference(const FlySight::SessionData &session, const FusionGolden &golden);

/// Sessions enter the model as they do in the application (mergeSessions:
/// loaded, hidden); the saver and the column worker have finished when this
/// returns. Empty when the model then holds exactly these sessions, all
/// loaded; else what went wrong. For a model that was empty before.
[[nodiscard]] QString addSessions(FlySight::SessionModel &model, const QList<FlySight::SessionData> &sessions);

} // namespace FlySightTest

#endif // FLYSIGHTTEST_FUSIONSESSIONS_H
