#include "wspcalculations.h"
#include "../sessiondata.h"
#include "../dependencykey.h"
#include "../markerregistry.h"
#include "../attributeregistry.h"
#include "../units/unitdefinitions.h"
#include "registration.h"
#include <GeographicLib/Geodesic.hpp>
#include <QColor>
#include <algorithm>
#include <optional>

using namespace FlySight;

namespace {

// Everything the window-gate computation can produce. A disengaged member is
// an output that could not be determined (a partial result).
struct WspResults {
    std::optional<double> entryTime, exitTime;
    std::optional<double> entryLat, entryLon, exitLat, exitLon;
    std::optional<double> timeResult, distResult, speedResult, sepResult;
};

// Window gate crossings and results.
//  - no entry crossing: nothing is set;
//  - entry but no exit crossing: only the three entry members;
//  - speed only when the time result is positive;
//  - SEP only when the validation window contains a sample.
WspResults computeWspResults(double topAlt, double bottomAlt, double exitTime,
                             const QVariant &ref1Var,
                             const QVector<double> &z, const QVector<double> &lat,
                             const QVector<double> &lon, const QVector<double> &time,
                             const QVector<double> &hAcc, const QVector<double> &vAcc)
{
    WspResults results;

    if (z.isEmpty() || lat.isEmpty() || lon.isEmpty() || time.isEmpty())
        return results;
    if (z.size() != time.size() || lat.size() != time.size() || lon.size() != time.size())
        return results;

    const int n = z.size();

    // Find the starting index: first sample where time >= exitTime
    int startIdx = -1;
    for (int i = 0; i < n; ++i) {
        if (time[i] >= exitTime) {
            startIdx = i;
            break;
        }
    }
    if (startIdx < 0)
        return results;

    // Find window entry (first downward crossing of topAlt starting from startIdx)
    double entryTime = 0.0;
    double entryLat  = 0.0;
    double entryLon  = 0.0;
    bool foundEntry  = false;

    for (int i = std::max(startIdx, 1); i < n; ++i) {
        if (z[i - 1] >= topAlt && z[i] < topAlt) {
            double a = (topAlt - z[i - 1]) / (z[i] - z[i - 1]);
            entryTime = time[i - 1] + a * (time[i] - time[i - 1]);
            entryLat  = lat[i - 1]  + a * (lat[i]  - lat[i - 1]);
            entryLon  = lon[i - 1]  + a * (lon[i]  - lon[i - 1]);
            foundEntry = true;
            startIdx = i; // continue search from here for the bottom gate
            break;
        }
    }

    if (!foundEntry)
        return results;     // no entry crossing: every result is unavailable

    results.entryTime = entryTime;
    results.entryLat  = entryLat;
    results.entryLon  = entryLon;

    // Find window exit (first downward crossing of bottomAlt after entry)
    double wspExitTime = 0.0;
    double exitLat     = 0.0;
    double exitLon     = 0.0;
    bool foundExit     = false;
    int exitIdx        = -1;

    for (int i = startIdx; i < n; ++i) {
        if (i < 1) continue;
        if (z[i - 1] >= bottomAlt && z[i] < bottomAlt) {
            double a = (bottomAlt - z[i - 1]) / (z[i] - z[i - 1]);
            wspExitTime = time[i - 1] + a * (time[i] - time[i - 1]);
            exitLat     = lat[i - 1]  + a * (lat[i]  - lat[i - 1]);
            exitLon     = lon[i - 1]  + a * (lon[i]  - lon[i - 1]);
            foundExit   = true;
            exitIdx     = i;
            break;
        }
    }

    if (!foundExit)
        return results;     // entry found but no exit crossing

    results.exitTime = wspExitTime;
    results.exitLat  = exitLat;
    results.exitLon  = exitLon;

    // Compute derived results
    double timeResult = wspExitTime - entryTime;
    results.timeResult = timeResult;

    double dist = 0.0;
    GeographicLib::Geodesic::WGS84().Inverse(entryLat, entryLon, exitLat, exitLon, dist);
    results.distResult = dist;

    if (timeResult > 0.0)
        results.speedResult = dist / timeResult;

    // Compute max SEP within the validation window.
    // Start: Lane Reference 1 (9 s after vertical speed first reaches 10 m/s).
    // End:   20 m below the bottom of the competition window.
    if (ref1Var.canConvert<double>() && hAcc.size() == n && vAcc.size() == n) {
        double ref1Time = ref1Var.toDouble();

        // Find first sample at or after Ref1 time
        int valStart = -1;
        for (int i = 0; i < n; ++i) {
            if (time[i] >= ref1Time) {
                valStart = i;
                break;
            }
        }

        // Search forward from exit crossing until altitude <= bottomAlt - 20
        int valEnd = exitIdx;
        for (int i = exitIdx + 1; i < n; ++i) {
            if (z[i] <= bottomAlt - 20.0)
                break;
            valEnd = i;
        }

        double maxSep = -1.0;
        if (valStart >= 0 && valStart <= valEnd) {
            for (int i = valStart; i <= valEnd; ++i) {
                double sep = 0.5127 * (2.0 * hAcc[i] + vAcc[i]);
                if (sep > maxSep)
                    maxSep = sep;
            }
        }

        if (maxSep >= 0.0)
            results.sepResult = maxSep;
    }

    return results;
}

// Registers a parameter default: no inputs, one constant output. A stored
// attribute of the same name (the user's choice) takes precedence.
void registerWspDefault(CalculationRegistry &registry, const QString &id,
                        const char *key, const QVariant &value)
{
    const QString outputKey = QString::fromLatin1(key);

    CalculationDescriptor d;
    d.id = id;
    d.outputs = { DependencyKey::attribute(outputKey) };
    d.compute = [outputKey, value](const EvaluationContext &) -> CalculationResult {
        return CalculationResult().setAttribute(outputKey, value);
    };
    Calculations::addCalculation(registry, d);
}

} // namespace

void Calculations::registerWspCalculations(CalculationRegistry &registry)
{
    // ── Group A: Parameter defaults ────────────────────────────────────

    registerWspDefault(registry, QStringLiteral("builtin.wsp.default.version"),
                       SessionKeys::WspVersion, QVariant(QStringLiteral("1.0")));
    registerWspDefault(registry, QStringLiteral("builtin.wsp.default.topAlt"),
                       SessionKeys::WspTopAlt, QVariant(2500.0));
    registerWspDefault(registry, QStringLiteral("builtin.wsp.default.bottomAlt"),
                       SessionKeys::WspBottomAlt, QVariant(1500.0));
    registerWspDefault(registry, QStringLiteral("builtin.wsp.default.task"),
                       SessionKeys::WspTask, QVariant(QStringLiteral("Time")));

    // ── Group A2: Lane Reference 1 (Ref1) ─────────────────────────────
    // Ref1 = 9 seconds after the competitor's vertical speed first
    // reaches 10 m/s (starting from exit).  Used as the validation
    // window start and, later, as a lane reference endpoint.
    {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.wsp.ref1Time");
        d.inputs = {
            CalcInput::attribute(SessionKeys::ExitTime),
            CalcInput::measurement("GNSS", "velD"),
            CalcInput::measurement("GNSS", SessionKeys::Time)
        };
        d.outputs = { DependencyKey::attribute(SessionKeys::WspRef1Time) };
        d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
            QVariant exitVar = ctx.attribute(SessionKeys::ExitTime);
            if (!exitVar.canConvert<double>())
                return CalculationResult::unavailable();
            double exitTime = exitVar.toDouble();

            QVector<double> velD = ctx.measurement("GNSS", "velD");
            QVector<double> time = ctx.measurement("GNSS", SessionKeys::Time);

            if (velD.isEmpty() || time.isEmpty() || velD.size() != time.size())
                return CalculationResult::unavailable();

            const int n = velD.size();
            const double vThreshold = 10.0;

            for (int i = 1; i < n; ++i) {
                if (time[i] < exitTime)
                    continue;
                if (velD[i - 1] < vThreshold && velD[i] >= vThreshold) {
                    double a = (vThreshold - velD[i - 1]) / (velD[i] - velD[i - 1]);
                    double crossingTime = time[i - 1] + a * (time[i] - time[i - 1]);
                    return CalculationResult().setAttribute(SessionKeys::WspRef1Time,
                                                            crossingTime + 9.0);
                }
            }

            return CalculationResult::unavailable();
        };
        addCalculation(registry, d);
    }

    // ── Group B: Window gate crossing and result calculations ──────────
    // One computation, ten outputs; a partial result leaves the outputs that
    // could not be determined unset (unavailable).
    {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.wsp.results");
        d.inputs = {
            CalcInput::attribute(SessionKeys::WspTopAlt),
            CalcInput::attribute(SessionKeys::WspBottomAlt),
            CalcInput::attribute(SessionKeys::ExitTime),
            CalcInput::attribute(SessionKeys::GroundElev),
            CalcInput::measurement("GNSS", "z"),
            CalcInput::measurement("GNSS", "lat"),
            CalcInput::measurement("GNSS", "lon"),
            CalcInput::measurement("GNSS", SessionKeys::Time),
            CalcInput::measurement("GNSS", "hAcc"),
            CalcInput::measurement("GNSS", "vAcc"),
            CalcInput::attribute(SessionKeys::WspRef1Time)
        };
        d.outputs = {
            DependencyKey::attribute(SessionKeys::WspEntryTime),
            DependencyKey::attribute(SessionKeys::WspExitTime),
            DependencyKey::attribute(SessionKeys::WspEntryLat),
            DependencyKey::attribute(SessionKeys::WspEntryLon),
            DependencyKey::attribute(SessionKeys::WspExitLat),
            DependencyKey::attribute(SessionKeys::WspExitLon),
            DependencyKey::attribute(SessionKeys::WspTimeResult),
            DependencyKey::attribute(SessionKeys::WspDistResult),
            DependencyKey::attribute(SessionKeys::WspSpeedResult),
            DependencyKey::attribute(SessionKeys::WspSepResult)
        };
        d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
            // Retrieve parameters
            QVariant topVar = ctx.attribute(SessionKeys::WspTopAlt);
            if (!topVar.canConvert<double>())
                return CalculationResult::unavailable();

            QVariant botVar = ctx.attribute(SessionKeys::WspBottomAlt);
            if (!botVar.canConvert<double>())
                return CalculationResult::unavailable();

            QVariant exitVar = ctx.attribute(SessionKeys::ExitTime);
            if (!exitVar.canConvert<double>())
                return CalculationResult::unavailable();

            QVariant geVar = ctx.attribute(SessionKeys::GroundElev);
            if (!geVar.canConvert<double>())
                return CalculationResult::unavailable();

            const WspResults r = computeWspResults(
                topVar.toDouble(), botVar.toDouble(), exitVar.toDouble(),
                ctx.attribute(SessionKeys::WspRef1Time),
                ctx.measurement("GNSS", "z"), ctx.measurement("GNSS", "lat"),
                ctx.measurement("GNSS", "lon"), ctx.measurement("GNSS", SessionKeys::Time),
                ctx.measurement("GNSS", "hAcc"), ctx.measurement("GNSS", "vAcc"));

            CalculationResult result;
            auto publish = [&result](const char *key, const std::optional<double> &value) {
                if (value)
                    result.setAttribute(key, *value);
            };
            publish(SessionKeys::WspEntryTime,   r.entryTime);
            publish(SessionKeys::WspExitTime,    r.exitTime);
            publish(SessionKeys::WspEntryLat,    r.entryLat);
            publish(SessionKeys::WspEntryLon,    r.entryLon);
            publish(SessionKeys::WspExitLat,     r.exitLat);
            publish(SessionKeys::WspExitLon,     r.exitLon);
            publish(SessionKeys::WspTimeResult,  r.timeResult);
            publish(SessionKeys::WspDistResult,  r.distResult);
            publish(SessionKeys::WspSpeedResult, r.speedResult);
            publish(SessionKeys::WspSepResult,   r.sepResult);
            return result;
        };
        addCalculation(registry, d);
    }
}

void Calculations::registerWspMetadata()
{
    // ── Group C: Marker registration (3 markers) ──────────────────────

    QVector<MarkerDefinition> defs;

    MarkerDefinition ref1Def;
    ref1Def.category       = QStringLiteral("Wingsuit Performance");
    ref1Def.displayName    = QStringLiteral("Lane Reference 1");
    ref1Def.shortLabel     = QStringLiteral("Ref1");
    ref1Def.color          = QColor(0, 128, 0);
    ref1Def.attributeKey   = SessionKeys::WspRef1Time;
    ref1Def.measurements   = {};
    ref1Def.editable       = false;
    ref1Def.groupId        = QStringLiteral("wsp");
    ref1Def.defaultEnabled = true;
    defs.append(ref1Def);

    MarkerDefinition topDef;
    topDef.category       = QStringLiteral("Wingsuit Performance");
    topDef.displayName    = QStringLiteral("Window Top");
    topDef.shortLabel     = QStringLiteral("Top");
    topDef.color          = QColor(0, 128, 0);
    topDef.attributeKey   = SessionKeys::WspEntryTime;
    topDef.measurements   = {};
    topDef.editable       = false;
    topDef.groupId        = QStringLiteral("wsp");
    topDef.defaultEnabled = true;
    defs.append(topDef);

    MarkerDefinition botDef;
    botDef.category       = QStringLiteral("Wingsuit Performance");
    botDef.displayName    = QStringLiteral("Window Bottom");
    botDef.shortLabel     = QStringLiteral("Bot");
    botDef.color          = QColor(0, 128, 0);
    botDef.attributeKey   = SessionKeys::WspExitTime;
    botDef.measurements   = {};
    botDef.editable       = false;
    botDef.groupId        = QStringLiteral("wsp");
    botDef.defaultEnabled = true;
    defs.append(botDef);

    MarkerRegistry::instance()->replaceMarkerGroup(QStringLiteral("wsp"), defs);

    // ── Group D: Attribute registry entries (3 registrations) ─────────

    auto& reg = AttributeRegistry::instance();

    reg.registerAttribute({
        QStringLiteral("Wingsuit Performance"),
        QStringLiteral("Task"),
        SessionKeys::WspTask,
        AttributeFormatType::Text,
        true,
        {}
    });

    reg.registerAttribute({
        QStringLiteral("Wingsuit Performance"),
        QStringLiteral("Time"),
        SessionKeys::WspTimeResult,
        AttributeFormatType::Double,
        false,
        MeasurementTypes::WspTime
    });

    reg.registerAttribute({
        QStringLiteral("Wingsuit Performance"),
        QStringLiteral("Distance"),
        SessionKeys::WspDistResult,
        AttributeFormatType::Double,
        false,
        MeasurementTypes::WspDistance
    });

    reg.registerAttribute({
        QStringLiteral("Wingsuit Performance"),
        QStringLiteral("Speed"),
        SessionKeys::WspSpeedResult,
        AttributeFormatType::Double,
        false,
        MeasurementTypes::WspSpeed
    });

    reg.registerAttribute({
        QStringLiteral("Wingsuit Performance"),
        QStringLiteral("SEP"),
        SessionKeys::WspSepResult,
        AttributeFormatType::Double,
        false,
        MeasurementTypes::WspSep
    });
}
