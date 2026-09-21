#ifndef FLYSIGHTTEST_FUSIONSESSIONS_H
#define FLYSIGHTTEST_FUSIONSESSIONS_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include "dependencykey.h"
#include "fusionfixtures.h"
#include "plotregistry.h"
#include "sessiondata.h"

// The fusion fixtures as real sessions, for tests of sensor fusion as a
// registered calculation (tst_fusion_session, tst_fusion_jobs).
//
// "Stored data always wins": a measurement with source data is served by the
// conversion layer, not by a registered calculation. A fixture session stores
// the fit's inputs under their own names (Local/north, IMU/_time, the origin
// attributes, ...) with units the conversion layer passes through unchanged,
// so the twenty-one effective inputs are bit-identical to the fixture and the
// session-level results can be held to the goldens of the kernel.

namespace FlySightTest {

/// The exact time fit every fixture session stores: utc = 1 * system + this.
constexpr double kFixtureTimeFitB = 1699999900.0;

/// Registers the fusion calculations on the global registry once per process
/// (after TestEnvironment::registerBuiltIns(), as the application does).
void registerFusionOnce();

/// Stored source data such that the 21 declared inputs read back bit-identical
/// to the fixture:
///   GNSS/time (unit "s")            -> GNSS/_time through builtin.time.utc.GNSS
///   GNSS/hAcc, vAcc ("m"), sAcc ("m/s")
///   Local/north|east|down ("m"), Local/velN|velE|velD ("m/s")
///   IMU/_time ("s"), IMU/ax|ay|az ("m/s^2"), IMU/wx|wy|wz ("deg/s")
///   stored attributes _LOCAL_ORIGIN_INDEX (qlonglong), _LOCAL_ORIGIN_LAT|LON|HMSL (double),
///   _TIME_FIT_A = "1", _TIME_FIT_B = "1699999900", SCHEMA_VER = 2 (the gyro
///   channels are read literally), SESSION_ID = sessionId, DEVICE_ID = "fusion-test"
/// Only fixtures whose columns have equal lengths per sensor may go into a
/// SessionModel (the saver refuses a ragged sensor): not reject_length.
FlySight::SessionData sessionFromFixture(const FusionFixture &fixture, const QString &sessionId);

/// The same without any IMU column (spec acceptance 11).
FlySight::SessionData sessionWithoutImu(const FusionFixture &fixture, const QString &sessionId);

/// A recording as the importer would leave it: GNSS lat/lon/hMSL/velN/velE/velD/
/// hAcc/vAcc/sAcc/time, IMU time/ax../wz, TIME time/tow/week; nothing under Local,
/// no stored origin, no stored fit. Retyped from the reference's synthetic
/// session: epoch 1700000000, 200 fixes at .1 + i*.2 s with lat = lon = 0,
/// hMSL = 100, zero velocity, hAcc = vAcc = 1, sAcc = .1; IMU system time
/// 100 + i*.01, i = 0..4000, az = -9.80665 m/s^2, the rest 0; TIME pulses
/// {100, 120, 140} with tow / tow+20 / tow+40. SCHEMA_VER = 2.
FlySight::SessionData naturalSession(const QString &sessionId);

QStringList fusionMeasurementNames();   ///< the 17 literal names, in output order

/// The seventeen "Sensor fusion" plots as PlotValues, for PlotModel::setPlots():
/// the sixteen fit measurements other than _time, plus accH, in the
/// application's order. Mirrors MainWindow::registerBuiltInPlots(), which is
/// outside the test library boundary; audit_cleanup pins that list at
/// seventeen rows. Colours are irrelevant here and left default.
QVector<FlySight::PlotValue> fusionPlots();

/// "Everything": the 17 measurements, Fusion/accH, Fusion/_system_time,
/// _FUSION_DIAGNOSTICS, and Fusion/roll interpolated at _EXIT_TIME.
QList<FlySight::DependencyKey> fusionNames();
/// The attribute name of Fusion/roll interpolated at _EXIT_TIME.
QString fusionRollAtExit();

} // namespace FlySightTest

#endif // FLYSIGHTTEST_FUSIONSESSIONS_H
