#include "magcalculations.h"
#include "../sessiondata.h"
#include "../dependencykey.h"
#include "registration.h"
#include <QVector>
#include <cmath>

using namespace FlySight;

void Calculations::registerMagCalculations(CalculationRegistry &registry)
{
    // MAG total magnetic field strength
    CalculationDescriptor d;
    d.id = QStringLiteral("builtin.mag.total");
    d.inputs = {
        CalcInput::measurement("MAG", "x"),
        CalcInput::measurement("MAG", "y"),
        CalcInput::measurement("MAG", "z")
    };
    d.outputs = { DependencyKey::measurement("MAG", "total") };
    d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
        QVector<double> x = ctx.measurement("MAG", "x");
        QVector<double> y = ctx.measurement("MAG", "y");
        QVector<double> z = ctx.measurement("MAG", "z");

        if (x.isEmpty() || y.isEmpty() || z.isEmpty()) {
            qWarning() << "Cannot calculate total due to missing x, y, or z";
            return CalculationResult::unavailable();
        }

        if ((x.size() != y.size()) || (x.size() != z.size())) {
            qWarning() << "x, y, or z size mismatch";
            return CalculationResult::unavailable();
        }

        QVector<double> total;
        total.reserve(x.size());
        for(int i = 0; i < x.size(); ++i){
            total.append(std::sqrt(x[i]*x[i] + y[i]*y[i] + z[i]*z[i]));
        }
        return CalculationResult().setMeasurement("MAG", "total", total);
    };
    addCalculation(registry, d);
}
