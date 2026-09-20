#include "spcalculations.h"
#include "../sessiondata.h"
#include "../dependencykey.h"
#include "../markerregistry.h"
#include "../attributeregistry.h"
#include "../units/unitdefinitions.h"
#include "registration.h"
#include <QColor>
#include <algorithm>
#include <cmath>
#include <optional>

using namespace FlySight;

namespace {

// Performance window start: where velD first reaches 10 m/s after exit, and
// the altitude AGL at that crossing.
struct SpWindowStart {
    double time;
    double alt;
};

std::optional<SpWindowStart> computeSpWindowStart(double exitTime,
                                                  const QVector<double> &velD,
                                                  const QVector<double> &time,
                                                  const QVector<double> &z)
{
    if (velD.isEmpty() || time.isEmpty() || z.isEmpty()
        || velD.size() != time.size() || z.size() != time.size())
        return std::nullopt;

    const int n = velD.size();
    const double vThreshold = 10.0;

    for (int i = 1; i < n; ++i) {
        if (time[i] < exitTime)
            continue;
        if (velD[i - 1] < vThreshold && velD[i] >= vThreshold) {
            double a = (vThreshold - velD[i - 1]) / (velD[i] - velD[i - 1]);
            SpWindowStart start;
            start.time = time[i - 1] + a * (time[i] - time[i - 1]);
            start.alt  = z[i - 1]    + a * (z[i]    - z[i - 1]);
            return start;
        }
    }

    return std::nullopt;
}

// A disengaged member is an output that could not be determined.
struct SpResults {
    std::optional<double> windowEndTime;
    std::optional<double> bestStartTime, bestEndTime, speedResult;
    std::optional<double> maxSpeedAcc;
};

SpResults computeSpResults(double perfWindowHeight, double valWindowHeight, double breakoffAlt,
                           double windowStartTime, double windowStartAlt,
                           const QVector<double> &z, const QVector<double> &time,
                           const QVector<double> &vAcc)
{
    SpResults results;

    if (z.isEmpty() || time.isEmpty())
        return results;
    if (z.size() != time.size())
        return results;

    const int n = z.size();

    // Determine performance window end altitude.
    // z is already AGL (hMSL - groundElev), so breakoffAlt is compared directly.
    // Higher altitude = reached first during descent.
    double windowEndAlt = std::max(windowStartAlt - perfWindowHeight,
                                   breakoffAlt);

    // Find window end time (first downward crossing of windowEndAlt after window start)
    double windowEndTime = -1.0;
    int windowEndIdx = -1;
    for (int i = 1; i < n; ++i) {
        if (time[i] < windowStartTime)
            continue;
        if (z[i - 1] >= windowEndAlt && z[i] < windowEndAlt) {
            double a = (windowEndAlt - z[i - 1]) / (z[i] - z[i - 1]);
            windowEndTime = time[i - 1] + a * (time[i] - time[i - 1]);
            windowEndIdx = i;
            break;
        }
    }

    if (windowEndTime < 0.0)
        return results;     // the window never closes: every result is unavailable

    results.windowEndTime = windowEndTime;

    // Find the fastest 3-second average vertical speed within the performance window.
    // For each sample i within the window, compute the altitude drop over the next 3 seconds.
    double bestSpeed = -1.0;
    double bestStartTime = 0.0;
    double bestEndTime = 0.0;

    // Find first sample at or after window start time
    int firstIdx = 0;
    for (int i = 0; i < n; ++i) {
        if (time[i] >= windowStartTime) {
            firstIdx = i;
            break;
        }
    }

    // Use a advancing-index approach for the +3s endpoint
    int j = firstIdx;
    for (int i = firstIdx; i < n; ++i) {
        if (time[i] > windowEndTime)
            break;

        double tStart = time[i];
        double tEnd = tStart + 3.0;

        // The 3s interval must fit within the performance window
        if (tEnd > windowEndTime)
            break;

        // Advance j until time[j] >= tEnd
        while (j < n - 1 && time[j] < tEnd)
            ++j;

        if (j >= n || time[j] < tEnd)
            break;

        // Interpolate altitude at tEnd
        // j is the first sample with time[j] >= tEnd
        double zEnd;
        if (j > 0 && time[j - 1] < tEnd) {
            double a = (tEnd - time[j - 1]) / (time[j] - time[j - 1]);
            zEnd = z[j - 1] + a * (z[j] - z[j - 1]);
        } else {
            zEnd = z[j];
        }

        // speed = altitude drop / 3 seconds (positive during descent)
        double speed = (z[i] - zEnd) / 3.0;

        if (speed > bestSpeed) {
            bestSpeed = speed;
            bestStartTime = tStart;
            bestEndTime = tEnd;
        }
    }

    if (bestSpeed >= 0.0) {
        results.bestStartTime = bestStartTime;
        results.bestEndTime   = bestEndTime;
        results.speedResult   = bestSpeed;
    }

    // Compute max speed accuracy within the validation window.
    // Start at the window end crossing and walk backward until altitude
    // exceeds windowEndAlt + valWindowHeight.  This avoids including
    // samples from the aircraft climb that happen to be at the same altitude.
    double valTopAlt = windowEndAlt + valWindowHeight;

    if (vAcc.size() == n && windowEndIdx > 0) {
        double maxSpeedAcc = -1.0;
        for (int i = windowEndIdx; i >= 0; --i) {
            if (z[i] > valTopAlt)
                break;
            double speedAcc = std::sqrt(2.0) * vAcc[i] / 3.0;
            if (speedAcc > maxSpeedAcc)
                maxSpeedAcc = speedAcc;
        }

        if (maxSpeedAcc >= 0.0)
            results.maxSpeedAcc = maxSpeedAcc;
    }

    return results;
}

// Registers a parameter default: no inputs, one constant output. A stored
// attribute of the same name (the user's choice) takes precedence.
void registerSpDefault(CalculationRegistry &registry, const QString &id,
                       const char *key, double value)
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

void Calculations::registerSpCalculations(CalculationRegistry &registry)
{
    // ── Group A: Parameter defaults ──────────────────────────────────────

    // Performance window height in metres (default 7400 ft = 2255.52 m)
    registerSpDefault(registry, QStringLiteral("builtin.sp.default.perfWindowHeight"),
                      SessionKeys::SpPerfWindowHeight, 7400.0 / 3.28084);

    // Validation window height in metres (default 3300 ft = 1005.84 m)
    registerSpDefault(registry, QStringLiteral("builtin.sp.default.valWindowHeight"),
                      SessionKeys::SpValWindowHeight, 3300.0 / 3.28084);

    // Breakoff altitude AGL in metres (default 5600 ft = 1706.88 m)
    registerSpDefault(registry, QStringLiteral("builtin.sp.default.breakoffAlt"),
                      SessionKeys::SpBreakoffAlt, 5600.0 / 3.28084);

    // ── Group A2: Performance window start ───────────────────────────────
    // One computation, two outputs: the crossing time and the altitude there.
    {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.sp.windowStart");
        d.inputs = {
            CalcInput::attribute(SessionKeys::ExitTime),
            CalcInput::measurement("GNSS", "velD"),
            CalcInput::measurement("GNSS", SessionKeys::Time),
            CalcInput::measurement("GNSS", "z")
        };
        d.outputs = {
            DependencyKey::attribute(SessionKeys::SpWindowStartTime),
            DependencyKey::attribute(SessionKeys::SpWindowStartAlt)
        };
        d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
            QVariant exitVar = ctx.attribute(SessionKeys::ExitTime);
            if (!exitVar.canConvert<double>())
                return CalculationResult::unavailable();

            const auto start = computeSpWindowStart(exitVar.toDouble(),
                                                    ctx.measurement("GNSS", "velD"),
                                                    ctx.measurement("GNSS", SessionKeys::Time),
                                                    ctx.measurement("GNSS", "z"));
            if (!start)
                return CalculationResult::unavailable();
            return CalculationResult()
                .setAttribute(SessionKeys::SpWindowStartTime, start->time)
                .setAttribute(SessionKeys::SpWindowStartAlt, start->alt);
        };
        addCalculation(registry, d);
    }

    // ── Group B: Result calculations ─────────────────────────────────────
    // One computation, five outputs; a partial result leaves the outputs that
    // could not be determined unset (unavailable).
    {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.sp.results");
        d.inputs = {
            CalcInput::attribute(SessionKeys::SpPerfWindowHeight),
            CalcInput::attribute(SessionKeys::SpValWindowHeight),
            CalcInput::attribute(SessionKeys::SpBreakoffAlt),
            // Carried over from the previous dependency list; not read by compute.
            CalcInput::attribute(SessionKeys::ExitTime),
            CalcInput::attribute(SessionKeys::SpWindowStartTime),
            CalcInput::attribute(SessionKeys::SpWindowStartAlt),
            CalcInput::measurement("GNSS", "z"),
            CalcInput::measurement("GNSS", SessionKeys::Time),
            CalcInput::measurement("GNSS", "vAcc")
        };
        d.outputs = {
            DependencyKey::attribute(SessionKeys::SpWindowEndTime),
            DependencyKey::attribute(SessionKeys::SpBestStartTime),
            DependencyKey::attribute(SessionKeys::SpBestEndTime),
            DependencyKey::attribute(SessionKeys::SpSpeedResult),
            DependencyKey::attribute(SessionKeys::SpMaxSpeedAcc)
        };
        d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
            // Retrieve parameters
            QVariant perfHVar = ctx.attribute(SessionKeys::SpPerfWindowHeight);
            if (!perfHVar.canConvert<double>())
                return CalculationResult::unavailable();

            QVariant valHVar = ctx.attribute(SessionKeys::SpValWindowHeight);
            if (!valHVar.canConvert<double>())
                return CalculationResult::unavailable();

            QVariant breakoffVar = ctx.attribute(SessionKeys::SpBreakoffAlt);
            if (!breakoffVar.canConvert<double>())
                return CalculationResult::unavailable();

            QVariant startTimeVar = ctx.attribute(SessionKeys::SpWindowStartTime);
            if (!startTimeVar.canConvert<double>())
                return CalculationResult::unavailable();

            QVariant startAltVar = ctx.attribute(SessionKeys::SpWindowStartAlt);
            if (!startAltVar.canConvert<double>())
                return CalculationResult::unavailable();

            const SpResults r = computeSpResults(
                perfHVar.toDouble(), valHVar.toDouble(), breakoffVar.toDouble(),
                startTimeVar.toDouble(), startAltVar.toDouble(),
                ctx.measurement("GNSS", "z"), ctx.measurement("GNSS", SessionKeys::Time),
                ctx.measurement("GNSS", "vAcc"));

            CalculationResult result;
            auto publish = [&result](const char *key, const std::optional<double> &value) {
                if (value)
                    result.setAttribute(key, *value);
            };
            publish(SessionKeys::SpWindowEndTime, r.windowEndTime);
            publish(SessionKeys::SpBestStartTime, r.bestStartTime);
            publish(SessionKeys::SpBestEndTime,   r.bestEndTime);
            publish(SessionKeys::SpSpeedResult,   r.speedResult);
            publish(SessionKeys::SpMaxSpeedAcc,   r.maxSpeedAcc);
            return result;
        };
        addCalculation(registry, d);
    }
}

void Calculations::registerSpMetadata()
{
    // ── Group C: Marker registration (4 markers) ────────────────────────

    QVector<MarkerDefinition> defs;

    MarkerDefinition windowStartDef;
    windowStartDef.category       = QStringLiteral("Speed Skydiving");
    windowStartDef.displayName    = QStringLiteral("Window Start");
    windowStartDef.shortLabel     = QStringLiteral("Start");
    windowStartDef.color          = QColor(0, 128, 0);
    windowStartDef.attributeKey   = SessionKeys::SpWindowStartTime;
    windowStartDef.measurements   = {};
    windowStartDef.editable       = false;
    windowStartDef.groupId        = QStringLiteral("sp");
    windowStartDef.defaultEnabled = true;
    defs.append(windowStartDef);

    MarkerDefinition windowEndDef;
    windowEndDef.category       = QStringLiteral("Speed Skydiving");
    windowEndDef.displayName    = QStringLiteral("Window End");
    windowEndDef.shortLabel     = QStringLiteral("End");
    windowEndDef.color          = QColor(0, 128, 0);
    windowEndDef.attributeKey   = SessionKeys::SpWindowEndTime;
    windowEndDef.measurements   = {};
    windowEndDef.editable       = false;
    windowEndDef.groupId        = QStringLiteral("sp");
    windowEndDef.defaultEnabled = true;
    defs.append(windowEndDef);

    MarkerDefinition bestStartDef;
    bestStartDef.category       = QStringLiteral("Speed Skydiving");
    bestStartDef.displayName    = QStringLiteral("Best 3s Start");
    bestStartDef.shortLabel     = QStringLiteral("3sS");
    bestStartDef.color          = QColor(0, 0, 192);
    bestStartDef.attributeKey   = SessionKeys::SpBestStartTime;
    bestStartDef.measurements   = {};
    bestStartDef.editable       = false;
    bestStartDef.groupId        = QStringLiteral("sp");
    bestStartDef.defaultEnabled = true;
    defs.append(bestStartDef);

    MarkerDefinition bestEndDef;
    bestEndDef.category       = QStringLiteral("Speed Skydiving");
    bestEndDef.displayName    = QStringLiteral("Best 3s End");
    bestEndDef.shortLabel     = QStringLiteral("3sE");
    bestEndDef.color          = QColor(0, 0, 192);
    bestEndDef.attributeKey   = SessionKeys::SpBestEndTime;
    bestEndDef.measurements   = {};
    bestEndDef.editable       = false;
    bestEndDef.groupId        = QStringLiteral("sp");
    bestEndDef.defaultEnabled = true;
    defs.append(bestEndDef);

    MarkerRegistry::instance()->replaceMarkerGroup(QStringLiteral("sp"), defs);

    // ── Group D: Attribute registry entries ──────────────────────────────

    auto& reg = AttributeRegistry::instance();

    reg.registerAttribute({
        QStringLiteral("Speed Skydiving"),
        QStringLiteral("Speed"),
        SessionKeys::SpSpeedResult,
        AttributeFormatType::Double,
        false,
        MeasurementTypes::SpSpeed
    });

    reg.registerAttribute({
        QStringLiteral("Speed Skydiving"),
        QStringLiteral("Speed Accuracy"),
        SessionKeys::SpMaxSpeedAcc,
        AttributeFormatType::Double,
        false,
        MeasurementTypes::SpSpeedAcc
    });
}
