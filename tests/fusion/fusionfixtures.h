#ifndef FLYSIGHTTEST_FUSIONFIXTURES_H
#define FLYSIGHTTEST_FUSIONFIXTURES_H

#include <QList>
#include <QString>
#include <QVector>

namespace FlySightTest {

/// One synthetic recording for the fusion kernel: the effective values of the
/// twenty-two inputs the fit consumes, as plain arrays.
///
/// This header and its source use Qt Core and the C++ standard library only
/// and include nothing from src/. They are compiled once, into
/// flysight_fusion_test_support, and used by the tests and by the capture
/// tool fusion_golden_capture (tests/README.md, section 11), so the goldens
/// and the tests see the same inputs. The generator's bit-reproducibility
/// rules (fusionfixtures.cpp) still matter: the fixtures must be the same bits
/// on every CI compiler for the portable comparison to mean anything, and
/// capture.json records the generator's hash.
struct FusionFixture {
    QString name;                                  ///< also the golden file stem
    QVector<double> gnssTime, north, east, down, velN, velE, velD, hAcc, vAcc, sAcc;
    QVector<double> imuTime, ax, ay, az, wx, wy, wz;   ///< times UTC s; ax..az m/s^2; wx..wz deg/s
    QVector<double> imuTemperature;                ///< IMU/temperature, degC, per imuTime sample
    qint64 originIndex = 0;
    double originLat = 45.0, originLon = -75.0, originHMSL = 100.0;
    bool expectSuccess = true;                     ///< false: the recording must be rejected
};

/// The constant IMU temperature of every fixture that is not about the
/// temperature (exactly representable; no noise).
constexpr double kFixtureTemperatureDegC = 25;

/// All twelve fixtures: coarse_linear, coarse_maneuver, stationary_spin, then
/// the nine rejections, each a single mutation of one of the first two.
QList<FusionFixture> fusionFixtures();

/// The fixture called `name`; a default-constructed fixture (empty name) when
/// there is none.
FusionFixture fusionFixture(const QString &name);

/// The synthetic recordings of the initializer's tests: motion_start,
/// rest_throughout, sacc_anchor, drifting_bias. Not golden fixtures: their
/// expected values are stated in tst_fusion_kernel, and fusion_golden_capture
/// does not see them. Same bit-reproducibility rules as the rest of this file.
/// A default-constructed fixture (empty name) for any other name.
FusionFixture initializerFixture(const QString &name);

} // namespace FlySightTest

#endif // FLYSIGHTTEST_FUSIONFIXTURES_H
