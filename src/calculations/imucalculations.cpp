#include "imucalculations.h"
#include "../sessiondata.h"
#include "../dependencykey.h"
#include "registration.h"
#include <QVector>
#include <cmath>

using namespace FlySight;

namespace {

// Registers builtin.imu.<output>: the magnitude of the vector (x, y, z).
void registerImuMagnitude(CalculationRegistry &registry, const char *output,
                          const char *x, const char *y, const char *z)
{
    const QString outputName = QString::fromLatin1(output);
    const QString xName = QString::fromLatin1(x);
    const QString yName = QString::fromLatin1(y);
    const QString zName = QString::fromLatin1(z);

    CalculationDescriptor d;
    d.id = QStringLiteral("builtin.imu.") + outputName;
    d.inputs = {
        CalcInput::measurement("IMU", xName),
        CalcInput::measurement("IMU", yName),
        CalcInput::measurement("IMU", zName)
    };
    d.outputs = { DependencyKey::measurement("IMU", outputName) };
    d.compute = [outputName, xName, yName, zName](const EvaluationContext &ctx) -> CalculationResult {
        QVector<double> vx = ctx.measurement("IMU", xName);
        QVector<double> vy = ctx.measurement("IMU", yName);
        QVector<double> vz = ctx.measurement("IMU", zName);

        if (vx.isEmpty() || vy.isEmpty() || vz.isEmpty()) {
            qWarning() << "Cannot calculate" << outputName << "due to missing" << xName << yName << "or" << zName;
            return CalculationResult::unavailable();
        }

        if ((vx.size() != vy.size()) || (vx.size() != vz.size())) {
            qWarning() << xName << yName << "or" << zName << "size mismatch";
            return CalculationResult::unavailable();
        }

        QVector<double> total;
        total.reserve(vx.size());
        for(int i = 0; i < vx.size(); ++i){
            total.append(std::sqrt(vx[i]*vx[i] + vy[i]*vy[i] + vz[i]*vz[i]));
        }
        return CalculationResult().setMeasurement("IMU", outputName, total);
    };
    Calculations::addCalculation(registry, d);
}

} // namespace

void Calculations::registerImuCalculations(CalculationRegistry &registry)
{
    // IMU total acceleration (aTotal)
    registerImuMagnitude(registry, "aTotal", "ax", "ay", "az");

    // IMU total angular velocity (wTotal)
    registerImuMagnitude(registry, "wTotal", "wx", "wy", "wz");
}
