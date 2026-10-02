#include "fusion/sensornoise.h"

#include <cmath>
#include <stdexcept>

#include <QString>

#include "sensorconfiguration.h"

namespace FlySight::Fusion::Detail {

namespace {

// The conversion of the adapter (inputadapter.cpp), spelled the same way so
// that a step in rad/s is the same bits as a reading converted there.
constexpr double kRadiansPerDegree = kPi / 180;
// The output words are 16-bit two's complement: a range spans 32768 counts on
// either side of zero.
constexpr double kCountsPerRange = 32768;

// LSM6DSO, DS12140 Rev 3. Table 2: An, the accelerometer noise density in
// high-performance mode, g/sqrt(Hz), independent of the rate (note 8); the
// sensitivity LA_So is the range over 32768 counts (the printed 0.061, 0.122,
// 0.244 and 0.488 mg/LSB are these to three decimals, and the firmware writes
// counts x range / 32768). Coarsest range first, the order the lattice is
// tried in.
struct AccelerometerRange { double rangeG, densityGPerRootHz; };
constexpr AccelerometerRange kAccelerometerRanges[] = {{16, 110e-6}, {8, 80e-6}, {4, 75e-6}, {2, 70e-6}};

// Table 2: G_So, the gyro sensitivity, deg/s per count (70, 35, 17.50 and
// 8.75 mdps/LSB: the legacy correction times the range over 32768,
// docs/DATA_SCHEMA.md section 4), and Rn, the gyro noise density in
// high-performance mode, independent of the rate and the range (note 6),
// deg/s/sqrt(Hz).
struct GyroRange { double rangeDegS, sensitivityDegS; };
constexpr GyroRange kGyroRanges[] = {{2000, 70e-3}, {1000, 35e-3}, {500, 17.5e-3}, {250, 8.75e-3}};
constexpr double kGyroDensityDegSPerRootHz = 3.8e-3;

// The output data rates both sensors share (Table 2, LA_ODR and G_ODR), as the
// configuration keys write them (Table 2 prints 3332 and 6664, Table 18 417,
// 1667, 3333 and 6667), with the gyro's LPF2 cutoff at each (Table 18; the
// LPF2 is fixed by the rate, and the firmware of the default,
// SensorConfiguration::FirmwareVersion, runs no LPF1). The accelerometer's
// bandwidth is half the rate: its LPF1 cuts at ODR / 2 in high-performance
// mode (Figure 17, note 1) and its LPF2 is off (Table 65, LPF2_XL_EN = 0).
struct Rate { double hz, gyroBandwidthHz; };
constexpr Rate kRates[] = {{12.5, 4.2}, {26, 8.3}, {52, 16.6}, {104, 33.0}, {208, 66.8}, {416, 135.9},
                           {833, 295.5}, {1666, 1108.1}, {3333, 1320.7}, {6666, 1441.8}};

// Table 2, G_So%: the gyro's sensitivity tolerance. The table has no row for
// the accelerometer's, which section 4.6.1 places in the same table, so this
// one serves both.
constexpr double kSensitivityTolerance = .01;

// The last decimal the files write: five of g, three of deg/s.
constexpr double kAccelerometerUnitG = 1e-5;
constexpr double kGyroUnitDegS = 1e-3;

[[noreturn]] void throwNoEntry(const char *key, double value)
{
    throw std::invalid_argument((QStringLiteral("No datasheet entry for %1 = %2; sensor fusion unavailable")
                                     .arg(QLatin1String(key), QString::number(value))).toStdString());
}

/// The step of a range's lattice, in the units of Channels (m/s^2, deg/s):
/// the quantization step of the noise model, which converts it to kernel
/// units, and the step the lattice check tries.
double latticeStep(const AccelerometerRange &range)
{
    return range.rangeG/kCountsPerRange*kStandardGravity;
}

double latticeStep(const GyroRange &range)
{
    return range.sensitivityDegS;
}

const Rate *rateEntry(double hz)
{
    for (const Rate &rate : kRates) {
        if (rate.hz == hz)
            return &rate;
    }
    return nullptr;
}

/// The two numbers every sensor derives the same way from its datasheet
/// figures, which must already be filled.
void deriveSigmas(SensorNoise &n)
{
    n.sampleSigma = std::sqrt(n.datasheetDensity*n.datasheetDensity*n.bandwidth + n.step*n.step/12);
    n.density = n.sampleSigma*std::sqrt(1/n.rate);
    n.sensitivityTolerance = kSensitivityTolerance;
}

/// Within `tolerance` of a multiple of `step`.
bool onLattice(double value, double step, double tolerance)
{
    return std::abs(value-std::round(value/step)*step) <= tolerance;
}

bool allOnLattice(const QVector<double> &x, const QVector<double> &y, const QVector<double> &z,
                  double step, double tolerance)
{
    for (const QVector<double> *axis : {&x, &y, &z}) {
        for (double value : *axis) {
            if (!onLattice(value, step, tolerance))
                return false;
        }
    }
    return true;
}

} // namespace

ImuNoise imuNoise(const ImuConfiguration &configuration)
{
    const AccelerometerRange *accelerometer = nullptr;
    for (const AccelerometerRange &range : kAccelerometerRanges) {
        if (range.rangeG == configuration.accelFsG)
            accelerometer = &range;
    }
    if (!accelerometer)
        throwNoEntry(SensorConfiguration::AccelFsG, configuration.accelFsG);
    const GyroRange *gyro = nullptr;
    for (const GyroRange &range : kGyroRanges) {
        if (range.rangeDegS == configuration.gyroFsDegS)
            gyro = &range;
    }
    if (!gyro)
        throwNoEntry(SensorConfiguration::GyroFsDegS, configuration.gyroFsDegS);
    const Rate *accelerometerRate = rateEntry(configuration.accelOdrHz);
    if (!accelerometerRate)
        throwNoEntry(SensorConfiguration::AccelOdrHz, configuration.accelOdrHz);
    const Rate *gyroRate = rateEntry(configuration.gyroOdrHz);
    if (!gyroRate)
        throwNoEntry(SensorConfiguration::GyroOdrHz, configuration.gyroOdrHz);

    ImuNoise noise;
    noise.configuration = configuration;

    SensorNoise &a = noise.accelerometer;
    a.rate = accelerometerRate->hz;
    a.datasheetDensity = accelerometer->densityGPerRootHz*kStandardGravity;
    a.bandwidth = accelerometerRate->hz/2;
    a.step = latticeStep(*accelerometer);
    deriveSigmas(a);

    SensorNoise &g = noise.gyroscope;
    g.rate = gyroRate->hz;
    g.datasheetDensity = kGyroDensityDegSPerRootHz*kRadiansPerDegree;
    g.bandwidth = gyroRate->gyroBandwidthHz;
    g.step = latticeStep(*gyro)*kRadiansPerDegree;
    deriveSigmas(g);
    return noise;
}

double rangeShownByReadings(ImuSensor sensor, const QVector<double> &x, const QVector<double> &y,
                            const QVector<double> &z)
{
    if (sensor == ImuSensor::Accelerometer) {
        // The conversion layer multiplies g by standard gravity.
        const double tolerance = kAccelerometerUnitG*kStandardGravity;
        for (const AccelerometerRange &range : kAccelerometerRanges) {
            if (allOnLattice(x, y, z, latticeStep(range), tolerance))
                return range.rangeG;
        }
    } else {
        // The largest factor the conversion layer applies to a rate is the
        // legacy correction, the sensitivity over range / 32768 (the same at
        // every range): it maps a legacy file's counts x range / 32768 onto
        // counts x sensitivity, and its truncation error with them.
        const GyroRange &any = kGyroRanges[0];
        const double tolerance = kGyroUnitDegS*(any.sensitivityDegS/(any.rangeDegS/kCountsPerRange));
        for (const GyroRange &range : kGyroRanges) {
            if (allOnLattice(x, y, z, latticeStep(range), tolerance))
                return range.rangeDegS;
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

} // namespace FlySight::Fusion::Detail
