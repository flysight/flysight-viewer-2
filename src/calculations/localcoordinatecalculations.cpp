#include "localcoordinatecalculations.h"
#include "registration.h"
#include "../sessiondata.h"
#include "../dependencykey.h"
#include <QVector>
#include <GeographicLib/LocalCartesian.hpp>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

using namespace FlySight;

namespace {

// A position the frame can use: on the globe, with a finite height.
bool isValidPosition(double lat, double lon, double hMSL)
{
    return std::isfinite(lat) && std::abs(lat) <= 90.0
        && std::isfinite(lon) && std::abs(lon) <= 180.0
        && std::isfinite(hMSL);
}

// The origin is the first fix with a valid position and a finite horizontal
// accuracy of 0 <= hAcc < 10 m. Accuracy selects the origin only: later
// samples get coordinates whatever their accuracy. Speed and speed accuracy
// play no part.
std::optional<int> findOriginIndex(const QVector<double> &lat,
                                   const QVector<double> &lon,
                                   const QVector<double> &hMSL,
                                   const QVector<double> &hAcc)
{
    for (int i = 0; i < lat.size(); ++i) {
        if (!isValidPosition(lat[i], lon[i], hMSL[i]))
            continue;
        if (std::isfinite(hAcc[i]) && hAcc[i] >= 0.0 && hAcc[i] < 10.0)
            return i;
    }
    return std::nullopt;
}

struct LocalTrack {
    QVector<double> north, east, down, velN, velE, velD;
};

// Positions and velocities of every GNSS sample in the north/east/down frame
// at the origin. One entry per sample: an invalid position is NaN in all six
// channels at its own index, a non-finite velocity in the three velocity
// channels only.
LocalTrack transformToLocalFrame(double originLat, double originLon, double originHmsl,
                                 const QVector<double> &lat,
                                 const QVector<double> &lon,
                                 const QVector<double> &hMSL,
                                 const QVector<double> &velN,
                                 const QVector<double> &velE,
                                 const QVector<double> &velD)
{
    // CSV hMSL stands in for the ellipsoid height, without geoid correction
    // (documented approximation, docs/LOCAL_COORDINATES.md).
    const GeographicLib::LocalCartesian frame(originLat, originLon, originHmsl);

    const int n = lat.size();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    LocalTrack track;
    track.north = QVector<double>(n, nan);
    track.east = QVector<double>(n, nan);
    track.down = QVector<double>(n, nan);
    track.velN = QVector<double>(n, nan);
    track.velE = QVector<double>(n, nan);
    track.velD = QVector<double>(n, nan);

    // Forward() resizes this to 9; allocated once for the whole track
    std::vector<double> rotation(9);

    for (int i = 0; i < n; ++i) {
        if (!isValidPosition(lat[i], lon[i], hMSL[i]))
            continue;

        // GeographicLib works in east, north, up
        double east, north, up;
        frame.Forward(lat[i], lon[i], hMSL[i], east, north, up, rotation);
        track.north[i] = north;
        track.east[i] = east;
        track.down[i] = -up;

        const double vn = velN[i], ve = velE[i], vd = velD[i];
        if (!std::isfinite(vn) || !std::isfinite(ve) || !std::isfinite(vd))
            continue;

        // The recorded velocity is north/east/down at the fix. The matrix
        // (row-major) maps east/north/up at the fix to east/north/up at the
        // origin, hence e, n, -d. The operand order is part of the contract.
        const double rotatedEast  = rotation[0] * ve + rotation[1] * vn - rotation[2] * vd;
        const double rotatedNorth = rotation[3] * ve + rotation[4] * vn - rotation[5] * vd;
        const double rotatedUp    = rotation[6] * ve + rotation[7] * vn - rotation[8] * vd;
        track.velN[i] = rotatedNorth;
        track.velE[i] = rotatedEast;
        track.velD[i] = -rotatedUp;
    }
    return track;
}

} // namespace

void Calculations::registerLocalCoordinateCalculations(CalculationRegistry &registry)
{
    // The frame: one calculation, four attribute and six measurement outputs
    // in one bundle, so the origin attributes and the channels always describe
    // the same frame. Whichever output is read first, the track is projected
    // once. Markers and preferences are not inputs: nothing but the GNSS data
    // moves the frame.
    {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.local.coordinates");
        d.inputs = {
            CalcInput::measurement("GNSS", "lat"),
            CalcInput::measurement("GNSS", "lon"),
            CalcInput::measurement("GNSS", "hMSL"),
            CalcInput::measurement("GNSS", "hAcc"),
            CalcInput::measurement("GNSS", "velN"),
            CalcInput::measurement("GNSS", "velE"),
            CalcInput::measurement("GNSS", "velD")
        };
        d.outputs = {
            DependencyKey::attribute(SessionKeys::LocalOriginLat),
            DependencyKey::attribute(SessionKeys::LocalOriginLon),
            DependencyKey::attribute(SessionKeys::LocalOriginHmsl),
            DependencyKey::attribute(SessionKeys::LocalOriginIndex),
            DependencyKey::measurement("Local", "north"),
            DependencyKey::measurement("Local", "east"),
            DependencyKey::measurement("Local", "down"),
            DependencyKey::measurement("Local", "velN"),
            DependencyKey::measurement("Local", "velE"),
            DependencyKey::measurement("Local", "velD")
        };
        d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
            const QVector<double> lat = ctx.measurement("GNSS", "lat");
            const QVector<double> lon = ctx.measurement("GNSS", "lon");
            const QVector<double> hMSL = ctx.measurement("GNSS", "hMSL");
            const QVector<double> hAcc = ctx.measurement("GNSS", "hAcc");
            const QVector<double> velN = ctx.measurement("GNSS", "velN");
            const QVector<double> velE = ctx.measurement("GNSS", "velE");
            const QVector<double> velD = ctx.measurement("GNSS", "velD");

            // A ragged GNSS sensor has no frame
            const auto n = lat.size();
            if (lon.size() != n || hMSL.size() != n || hAcc.size() != n ||
                velN.size() != n || velE.size() != n || velD.size() != n) {
                return CalculationResult::unavailable();
            }

            // No qualifying fix: every output is unavailable
            const std::optional<int> origin = findOriginIndex(lat, lon, hMSL, hAcc);
            if (!origin)
                return CalculationResult::unavailable();

            // The origin is the recorded sample itself, not a rounded form
            const int i = *origin;
            const LocalTrack track = transformToLocalFrame(lat[i], lon[i], hMSL[i],
                                                           lat, lon, hMSL, velN, velE, velD);
            return CalculationResult()
                .setAttribute(SessionKeys::LocalOriginLat, lat[i])
                .setAttribute(SessionKeys::LocalOriginLon, lon[i])
                .setAttribute(SessionKeys::LocalOriginHmsl, hMSL[i])
                .setAttribute(SessionKeys::LocalOriginIndex, QVariant::fromValue(qlonglong(i)))
                .setMeasurement("Local", "north", track.north)
                .setMeasurement("Local", "east", track.east)
                .setMeasurement("Local", "down", track.down)
                .setMeasurement("Local", "velN", track.velN)
                .setMeasurement("Local", "velE", track.velE)
                .setMeasurement("Local", "velD", track.velD);
        };
        addCalculation(registry, d);
    }

    // _time for Local: the GNSS axis itself (passthrough sharing the buffer).
    // Not gated on the origin; every reader asks for the y data first.
    {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.local.time");
        d.inputs = { CalcInput::measurement("GNSS", SessionKeys::Time) };
        d.outputs = { DependencyKey::measurement("Local", SessionKeys::Time) };
        d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
            return CalculationResult().setMeasurement("Local", SessionKeys::Time,
                                                      ctx.measurement("GNSS", SessionKeys::Time));
        };
        addCalculation(registry, d);
    }

    // _system_time for Local: the GNSS axis as well. A calculation of its own,
    // because it needs the TIME sensor's fit and the positions must not.
    {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.local.systemTime");
        d.inputs = { CalcInput::measurement("GNSS", SessionKeys::SystemTime) };
        d.outputs = { DependencyKey::measurement("Local", SessionKeys::SystemTime) };
        d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
            return CalculationResult().setMeasurement("Local", SessionKeys::SystemTime,
                                                      ctx.measurement("GNSS", SessionKeys::SystemTime));
        };
        addCalculation(registry, d);
    }
}
