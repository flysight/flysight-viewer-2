#include "interpolationcalculations.h"
#include "../dependencykey.h"
#include "../engine/calculationdescriptor.h"
#include "../engine/calculationregistry.h"
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <iterator>
#include <optional>

using namespace FlySight;

void Calculations::registerInterpolationFamily(CalculationRegistry &registry)
{
    CalculationFamily family;
    family.id = QStringLiteral("builtin.interpolation");
    family.instantiate = [](const DependencyKey &name) -> std::optional<CalculationDescriptor> {
        if (name.type != DependencyKey::Type::Attribute)
            return std::nullopt;

        // 1. Parse the key: {timeAttr}:{sensor}/{timeVector}/{dataVector}
        const QString key = name.attributeKey;
        const int colonPos = key.indexOf(':');
        if (colonPos < 0)
            return std::nullopt;

        const QString timeAttrKey = key.left(colonPos);
        const QStringList parts = key.mid(colonPos + 1).split('/');
        if (parts.size() != 3)
            return std::nullopt;

        const QString sensor     = parts[0];
        const QString timeVector = parts[1];
        const QString dataVector = parts[2];

        CalculationDescriptor d;
        d.id = key;     // instance key: the full expression
        d.inputs = {
            CalcInput::attribute(timeAttrKey),
            CalcInput::measurement(sensor, timeVector),
            CalcInput::measurement(sensor, dataVector)
        };
        d.outputs = { name };
        d.compute = [key, timeAttrKey, sensor, timeVector, dataVector](const EvaluationContext &ctx)
                -> CalculationResult {
            // 2. Resolve the time attribute
            const QVariant timeVar = ctx.attribute(timeAttrKey);
            if (!timeVar.canConvert<double>())
                return CalculationResult::unavailable();
            const double markerTime = timeVar.toDouble();

            // 3. Read the measurement vectors
            const QVector<double> timeVec = ctx.measurement(sensor, timeVector);
            if (timeVec.isEmpty())
                return CalculationResult::unavailable();

            const QVector<double> dataVec = ctx.measurement(sensor, dataVector);
            if (dataVec.isEmpty() || dataVec.size() != timeVec.size())
                return CalculationResult::unavailable();

            // 4. Binary search and linear interpolation
            //    lower_bound returns cbegin() when markerTime <= first element, and
            //    cend() when markerTime > last element. Both cases mean the query
            //    falls outside the interpolatable range (we need two bracketing
            //    points). This matches interpolateAtX() in plotutils.cpp.
            auto it = std::lower_bound(timeVec.cbegin(), timeVec.cend(), markerTime);
            if (it == timeVec.cbegin() || it == timeVec.cend())
                return CalculationResult::unavailable();

            const int idx = static_cast<int>(std::distance(timeVec.cbegin(), it));
            const double t1 = timeVec[idx - 1], v1 = dataVec[idx - 1];
            const double t2 = timeVec[idx],     v2 = dataVec[idx];

            if (t2 == t1)
                return CalculationResult::unavailable();

            const double result = v1 + (v2 - v1) * (markerTime - t1) / (t2 - t1);
            return CalculationResult().setAttribute(key, result);
        };
        return d;
    };

    const bool ok = registry.registerFamily(family);
    Q_ASSERT(ok);
    Q_UNUSED(ok);
}
