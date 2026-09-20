#ifndef IMUCALCULATIONS_H
#define IMUCALCULATIONS_H

namespace FlySight {

class CalculationRegistry;

namespace Calculations {

/// Register the IMU-based measurements with the calculation engine (ids
/// builtin.imu.<measurement>).
void registerImuCalculations(CalculationRegistry &registry);

} // namespace Calculations
} // namespace FlySight

#endif // IMUCALCULATIONS_H
