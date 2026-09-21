#include "simplificationcalculations.h"
#include "registration.h"
#include "../sessiondata.h"
#include "../dependencykey.h"
#include <QVector>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

using namespace FlySight;

namespace {

// Epsilon: 0.5 meters (sensor noise floor)
constexpr double kToleranceMetres = 0.5;

// The samples the track can use: those whose three local coordinates are all
// finite, ascending. Anything else is left out and the path is joined across
// it, so one invalid fix does not poison the track.
QVector<qsizetype> finiteSampleIndices(const QVector<double> &north,
                                       const QVector<double> &east,
                                       const QVector<double> &down)
{
    QVector<qsizetype> indices;
    indices.reserve(north.size());
    for (qsizetype i = 0; i < north.size(); ++i) {
        if (std::isfinite(north[i]) && std::isfinite(east[i]) && std::isfinite(down[i]))
            indices.append(i);
    }
    return indices;
}

// Horizontal squared distance from sample i to the segment first-last. The
// clamp makes it a distance to the segment, not to the infinite line; with
// coincident ends (a closed track, duplicate positions) it degenerates to the
// distance to that point.
double squaredDistanceToSegment(const QVector<double> &north,
                                const QVector<double> &east,
                                qsizetype first, qsizetype last, qsizetype i)
{
    const double dn = north[last] - north[first];
    const double de = east[last] - east[first];
    const double lengthSquared = dn * dn + de * de;

    const double n = north[i] - north[first];
    const double e = east[i] - east[first];
    const double t = lengthSquared > 0.0
        ? std::clamp((n * dn + e * de) / lengthSquared, 0.0, 1.0)
        : 0.0;

    const double rn = n - t * dn;
    const double re = e - t * de;
    return rn * rn + re * re;
}

// Ramer-Douglas-Peucker over the positions of the candidate samples, returning
// the candidates that survive, ascending. It retains sample indices, not
// points: two samples at one position (a stationary start or end) stay two
// samples, and nothing has to be matched back to the recording afterwards.
QVector<qsizetype> retainedIndices(const QVector<double> &north,
                                   const QVector<double> &east,
                                   const QVector<qsizetype> &candidates)
{
    // One point stays one point; two stay two, also when they coincide
    const qsizetype count = candidates.size();
    if (count <= 2)
        return candidates;

    QVector<bool> keep(count, false);
    keep[0] = true;
    keep[count - 1] = true;

    // An explicit stack of spans (positions in candidates), not recursion: a
    // long recording that spirals can nest tens of thousands deep.
    std::vector<std::pair<qsizetype, qsizetype>> pending;
    pending.emplace_back(0, count - 1);

    while (!pending.empty()) {
        const auto [first, last] = pending.back();
        pending.pop_back();

        // The furthest candidate strictly inside the span. It survives only
        // when strictly further than the tolerance, and the first of equally
        // distant candidates wins (hence ">" on an ascending scan).
        double maxDistanceSquared = kToleranceMetres * kToleranceMetres;
        qsizetype furthest = first;
        for (qsizetype k = first + 1; k < last; ++k) {
            const double distanceSquared = squaredDistanceToSegment(
                north, east, candidates[first], candidates[last], candidates[k]);
            if (distanceSquared > maxDistanceSquared) {
                maxDistanceSquared = distanceSquared;
                furthest = k;
            }
        }

        if (furthest != first) {
            keep[furthest] = true;
            pending.emplace_back(first, furthest);
            pending.emplace_back(furthest, last);
        }
    }

    QVector<qsizetype> indices;
    for (qsizetype k = 0; k < count; ++k) {
        if (keep[k])
            indices.append(candidates[k]);
    }
    return indices;
}

// values[i] for each index
QVector<double> samplesAt(const QVector<double> &values, const QVector<qsizetype> &indices)
{
    QVector<double> samples;
    samples.reserve(indices.size());
    for (const qsizetype i : indices)
        samples.append(values[i]);
    return samples;
}

} // namespace

void Calculations::registerSimplificationCalculations(CalculationRegistry &registry)
{
    // One calculation, seven measurement outputs: whichever is read first, the
    // track is simplified once. The horizontal geometry is the shared local
    // frame (Local/north, Local/east); this calculation has no projection and
    // no origin of its own, so without that frame there is no track.
    CalculationDescriptor d;
    d.id = QStringLiteral("builtin.simplified.track");
    d.inputs = {
        CalcInput::measurement("GNSS", "lat"),
        CalcInput::measurement("GNSS", "lon"),
        CalcInput::measurement("GNSS", "hMSL"),
        CalcInput::measurement("GNSS", SessionKeys::Time),
        CalcInput::measurement("Local", "north"),
        CalcInput::measurement("Local", "east"),
        CalcInput::measurement("Local", "down")
    };
    d.outputs = {
        DependencyKey::measurement("Simplified", "lat"),
        DependencyKey::measurement("Simplified", "lon"),
        DependencyKey::measurement("Simplified", "hMSL"),
        DependencyKey::measurement("Simplified", SessionKeys::Time),
        DependencyKey::measurement("Simplified", "north"),
        DependencyKey::measurement("Simplified", "east"),
        DependencyKey::measurement("Simplified", "down")
    };
    d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
        const QVector<double> lat = ctx.measurement("GNSS", "lat");
        const QVector<double> lon = ctx.measurement("GNSS", "lon");
        const QVector<double> hMSL = ctx.measurement("GNSS", "hMSL");
        const QVector<double> time = ctx.measurement("GNSS", SessionKeys::Time);
        const QVector<double> north = ctx.measurement("Local", "north");
        const QVector<double> east = ctx.measurement("Local", "east");
        const QVector<double> down = ctx.measurement("Local", "down");

        // Ragged channels have no common sample index
        const auto n = lat.size();
        if (lon.size() != n || hMSL.size() != n || time.size() != n ||
            north.size() != n || east.size() != n || down.size() != n) {
            return CalculationResult::unavailable();
        }

        // No sample with local coordinates: no track
        const QVector<qsizetype> finite = finiteSampleIndices(north, east, down);
        if (finite.isEmpty())
            return CalculationResult::unavailable();

        // Every output is the recorded sample at the same retained indices,
        // so all seven always have the same length
        const QVector<qsizetype> indices = retainedIndices(north, east, finite);
        return CalculationResult()
            .setMeasurement("Simplified", "lat", samplesAt(lat, indices))
            .setMeasurement("Simplified", "lon", samplesAt(lon, indices))
            .setMeasurement("Simplified", "hMSL", samplesAt(hMSL, indices))
            .setMeasurement("Simplified", SessionKeys::Time, samplesAt(time, indices))
            .setMeasurement("Simplified", "north", samplesAt(north, indices))
            .setMeasurement("Simplified", "east", samplesAt(east, indices))
            .setMeasurement("Simplified", "down", samplesAt(down, indices));
    };
    addCalculation(registry, d);
}
