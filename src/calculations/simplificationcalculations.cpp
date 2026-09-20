#include "simplificationcalculations.h"
#include "../sessiondata.h"
#include "../dependencykey.h"
#include <QVector>
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/linestring.hpp>
#include <GeographicLib/LocalCartesian.hpp>
#include "registration.h"
#include <optional>
#include <vector>
#include <cmath>

using namespace FlySight;

namespace {

struct SimplifiedTrack {
    QVector<double> lat, lon, hMSL, time;
};

// Ramer-Douglas-Peucker simplification of the ground track (epsilon 0.5 m in
// a local Cartesian projection). The simplified points are a subset of the
// original points, so altitude and time are taken from the matching sample.
std::optional<SimplifiedTrack> simplifyTrack(const QVector<double> &rawLat,
                                             const QVector<double> &rawLon,
                                             const QVector<double> &rawAlt,
                                             const QVector<double> &rawTime)
{
    if (rawLat.isEmpty() || rawLat.size() != rawLon.size() ||
        rawLat.size() != rawAlt.size() || rawLat.size() != rawTime.size()) {
        return std::nullopt;
    }

    namespace bg = boost::geometry;
    using PointXY = bg::model::d2::point_xy<double>;
    using LineString = bg::model::linestring<PointXY>;

    // Project to Local Cartesian (metres), centred on the first point
    GeographicLib::LocalCartesian proj(rawLat[0], rawLon[0], rawAlt[0]);

    LineString pathInMeters;
    pathInMeters.reserve(rawLat.size());

    for (int i = 0; i < rawLat.size(); ++i) {
        double x, y, z;
        proj.Forward(rawLat[i], rawLon[i], rawAlt[i], x, y, z);

        // RDP here is a 2D simplification of the ground track, which is
        // sufficient for maps.
        bg::append(pathInMeters, PointXY(x, y));
    }

    // Epsilon: 0.5 meters (sensor noise floor)
    LineString simplifiedPath;
    bg::simplify(pathInMeters, simplifiedPath, 0.5);

    // RDP preserves vertices, so each simplified point exists in the original
    // path, in order. Search forward from the last match, comparing in
    // projected metre space to avoid floating-point fuzziness in lat/lon.
    SimplifiedTrack out;
    out.lat.reserve(simplifiedPath.size());
    out.lon.reserve(simplifiedPath.size());
    out.hMSL.reserve(simplifiedPath.size());
    out.time.reserve(simplifiedPath.size());

    size_t rawIdx = 0;
    for (const auto& pt : simplifiedPath) {
        double targetX = pt.x();
        double targetY = pt.y();

        for (; rawIdx < size_t(rawLat.size()); ++rawIdx) {
            double x, y, z;
            proj.Forward(rawLat[rawIdx], rawLon[rawIdx], rawAlt[rawIdx], x, y, z);

            if (std::abs(x - targetX) < 1e-3 && std::abs(y - targetY) < 1e-3) {
                // Match found
                out.lat.append(rawLat[rawIdx]);
                out.lon.append(rawLon[rawIdx]);
                out.hMSL.append(rawAlt[rawIdx]);
                out.time.append(rawTime[rawIdx]);
                break;
            }
        }
    }

    return out;
}

} // namespace

void Calculations::registerSimplificationCalculations(CalculationRegistry &registry)
{
    // One calculation, four measurement outputs: whichever is read first, the
    // track is simplified once.
    CalculationDescriptor d;
    d.id = QStringLiteral("builtin.simplified.track");
    d.inputs = {
        CalcInput::measurement("GNSS", "lat"),
        CalcInput::measurement("GNSS", "lon"),
        CalcInput::measurement("GNSS", "hMSL"),
        CalcInput::measurement("GNSS", SessionKeys::Time)
    };
    d.outputs = {
        DependencyKey::measurement("Simplified", "lat"),
        DependencyKey::measurement("Simplified", "lon"),
        DependencyKey::measurement("Simplified", "hMSL"),
        DependencyKey::measurement("Simplified", SessionKeys::Time)
    };
    d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
        const auto track = simplifyTrack(ctx.measurement("GNSS", "lat"),
                                         ctx.measurement("GNSS", "lon"),
                                         ctx.measurement("GNSS", "hMSL"),
                                         ctx.measurement("GNSS", SessionKeys::Time));
        if (!track)
            return CalculationResult::unavailable();
        return CalculationResult()
            .setMeasurement("Simplified", "lat", track->lat)
            .setMeasurement("Simplified", "lon", track->lon)
            .setMeasurement("Simplified", "hMSL", track->hMSL)
            .setMeasurement("Simplified", SessionKeys::Time, track->time);
    };
    addCalculation(registry, d);
}
