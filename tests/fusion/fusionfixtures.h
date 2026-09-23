#ifndef FLYSIGHTTEST_FUSIONFIXTURES_H
#define FLYSIGHTTEST_FUSIONFIXTURES_H

#include <QList>
#include <QString>
#include <QVector>

namespace FlySightTest {

/// One synthetic recording for the fusion kernel: the effective values of the
/// twenty-one inputs the fit consumes, as plain arrays.
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
    qint64 originIndex = 0;
    double originLat = 45.0, originLon = -75.0, originHMSL = 100.0;
    bool expectSuccess = true;                     ///< false: the recording must be rejected
};

/// All twelve fixtures: coarse_linear, coarse_maneuver, stationary_spin, then
/// the nine rejections, each a single mutation of one of the first two.
QList<FusionFixture> fusionFixtures();

/// The fixture called `name`; a default-constructed fixture (empty name) when
/// there is none.
FusionFixture fusionFixture(const QString &name);

} // namespace FlySightTest

#endif // FLYSIGHTTEST_FUSIONFIXTURES_H
