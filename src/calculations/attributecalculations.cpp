#include "attributecalculations.h"
#include "../sessiondata.h"
#include "../dependencykey.h"
#include "../preferences/preferencekeys.h"
#include "registration.h"
#include <QVector>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

using namespace FlySight;

namespace {

// Analysis range: the largest monotonic descent in elevation, where a descent
// ends after `timeout` seconds without a new low, padded by `timeout` on each
// side. Returns (start, end) in UTC seconds, or nullopt when there is no descent.
std::optional<std::pair<double, double>> computeAnalysisRange(const QVector<double> &hMSL,
                                                              const QVector<double> &time,
                                                              double timeout)
{
    if (hMSL.isEmpty() || time.isEmpty() || hMSL.size() != time.size())
        return std::nullopt;

    const int n = hMSL.size();

    // Initialize current-descent tracking from the first sample
    double currentHigh = hMSL[0];
    int currentHighIdx = 0;
    double currentLow = hMSL[0];
    int currentLowIdx = 0;

    // Best descent found so far
    double bestDrop = 0.0;
    int bestHighIdx = -1;
    int bestLowIdx = -1;

    for (int i = 1; i < n; ++i) {
        if (hMSL[i] > currentHigh) {
            // New high found: reset descent tracking
            currentHigh = hMSL[i];
            currentHighIdx = i;
            currentLow = hMSL[i];
            currentLowIdx = i;
        } else if (hMSL[i] <= currentLow) {
            // New low found (equal or below)
            currentLow = hMSL[i];
            currentLowIdx = i;
        } else if (time[i] - time[currentLowIdx] > timeout) {
            // Descent has ended due to pause timeout
            double drop = currentHigh - currentLow;
            if (drop > bestDrop) {
                bestDrop = drop;
                bestHighIdx = currentHighIdx;
                bestLowIdx = currentLowIdx;
            }
            // Reset all current-state variables to this sample
            currentHigh = hMSL[i];
            currentHighIdx = i;
            currentLow = hMSL[i];
            currentLowIdx = i;
        }
    }

    // Finalize: compare the last descent against bestDrop
    double drop = currentHigh - currentLow;
    if (drop > bestDrop) {
        bestDrop = drop;
        bestHighIdx = currentHighIdx;
        bestLowIdx = currentLowIdx;
    }

    if (bestDrop > 0 && bestHighIdx >= 0) {
        // Add a grace period (equal to the pause timeout) on each side
        double startSec = std::max(time[bestHighIdx] - timeout, time[0]);
        double endSec = std::min(time[bestLowIdx] + timeout, time[n - 1]);
        return std::make_pair(startSec, endSec);
    }

    return std::nullopt;
}

// Flare detection: the ascending segment in hMSL with the greatest elevation
// gain between exit and landing, tolerating small dips within the vertical
// position accuracy band. Returns (start, end) in UTC seconds.
std::optional<std::pair<double, double>> computeFlare(double exitSec, double landingSec,
                                                      const QVector<double> &hMSL,
                                                      const QVector<double> &vAcc,
                                                      const QVector<double> &time)
{
    const int n = hMSL.size();
    if (n == 0 || vAcc.size() != n || time.size() != n)
        return std::nullopt;

    // Find the index range [iStart, iEnd) covering exit..landing
    int iStart = -1, iEnd = n;
    for (int i = 0; i < n; ++i) {
        if (iStart < 0 && time[i] >= exitSec) iStart = i;
        if (time[i] > landingSec) { iEnd = i; break; }
    }
    if (iStart < 0 || iStart >= iEnd)
        return std::nullopt;

    // Track ascending segments: look for the one with the greatest elevation gain.
    // A segment ends when elevation drops below the running high by more than 2*vAcc.
    double currentLow  = hMSL[iStart];
    int    currentLowIdx  = iStart;
    double currentHigh = hMSL[iStart];
    int    currentHighIdx = iStart;

    double bestGain = 0.0;
    int    bestLowIdx  = -1;
    int    bestHighIdx = -1;

    for (int i = iStart + 1; i < iEnd; ++i) {
        if (hMSL[i] < currentLow) {
            // New low: reset the ascending segment
            currentLow = hMSL[i];
            currentLowIdx = i;
            currentHigh = hMSL[i];
            currentHighIdx = i;
        } else if (hMSL[i] > currentHigh) {
            // Extending the ascent
            currentHigh = hMSL[i];
            currentHighIdx = i;
        } else if (hMSL[i] < currentHigh - 2.0 * vAcc[i]) {
            // Confidently below the high: ascending segment has ended
            double gain = currentHigh - currentLow;
            if (gain > bestGain) {
                bestGain = gain;
                bestLowIdx = currentLowIdx;
                bestHighIdx = currentHighIdx;
            }
            // Reset from this sample
            currentLow = hMSL[i];
            currentLowIdx = i;
            currentHigh = hMSL[i];
            currentHighIdx = i;
        }
    }

    // Finalize: check the last segment
    double gain = currentHigh - currentLow;
    if (gain > bestGain) {
        bestGain = gain;
        bestLowIdx = currentLowIdx;
        bestHighIdx = currentHighIdx;
    }

    if (bestGain <= 0 || bestLowIdx < 0)
        return std::nullopt;

    return std::make_pair(time[bestLowIdx], time[bestHighIdx]);
}

// Time of the peak of `values` between manoeuvre start and landing.
std::optional<double> timeOfMaximum(double msSec, double landingSec,
                                    const QVector<double> &values,
                                    const QVector<double> &time)
{
    if (values.isEmpty() || time.isEmpty() || values.size() != time.size())
        return std::nullopt;

    double maxValue = -std::numeric_limits<double>::max();
    int maxIdx = -1;
    for (int i = 0; i < values.size(); ++i) {
        if (time[i] < msSec) continue;
        if (time[i] > landingSec) break;
        if (values[i] > maxValue) {
            maxValue = values[i];
            maxIdx = i;
        }
    }

    if (maxIdx < 0)
        return std::nullopt;

    return time[maxIdx];
}

} // namespace

void Calculations::registerAttributeCalculations(CalculationRegistry &registry)
{
    // Analysis range: one computation, two outputs. The pause timeout is a
    // declared preference input, so changing it re-evaluates the range.
    {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.attr.analysisRange");
        d.inputs = {
            CalcInput::measurement("GNSS", "hMSL"),
            CalcInput::measurement("GNSS", SessionKeys::Time),
            CalcInput::preference(PreferenceKeys::ImportDescentPauseSeconds)
        };
        d.outputs = {
            DependencyKey::attribute(SessionKeys::AnalysisStartTime),
            DependencyKey::attribute(SessionKeys::AnalysisEndTime)
        };
        d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
            const double timeout = ctx.preference(PreferenceKeys::ImportDescentPauseSeconds).toDouble();
            const auto range = computeAnalysisRange(ctx.measurement("GNSS", "hMSL"),
                                                    ctx.measurement("GNSS", SessionKeys::Time),
                                                    timeout);
            if (!range)
                return CalculationResult::unavailable();
            return CalculationResult()
                .setAttribute(SessionKeys::AnalysisStartTime, range->first)
                .setAttribute(SessionKeys::AnalysisEndTime, range->second);
        };
        addCalculation(registry, d);
    }

    // Exit time calculation based on GNSS vertical speed threshold
    {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.attr.exitTime");
        d.inputs = {
            CalcInput::attribute(SessionKeys::AnalysisStartTime),
            CalcInput::attribute(SessionKeys::AnalysisEndTime),
            CalcInput::measurement("GNSS", "velD"),
            CalcInput::measurement("GNSS", "sAcc"),
            CalcInput::measurement("GNSS", "accD"),
            CalcInput::measurement("GNSS", SessionKeys::Time)
        };
        d.outputs = { DependencyKey::attribute(SessionKeys::ExitTime) };
        d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
            // Retrieve analysis window to constrain the search
            QVariant asVar = ctx.attribute(SessionKeys::AnalysisStartTime);
            if (!asVar.canConvert<double>())
                return CalculationResult::unavailable();
            double analysisStartSec = asVar.toDouble();

            QVariant aeVar = ctx.attribute(SessionKeys::AnalysisEndTime);
            if (!aeVar.canConvert<double>())
                return CalculationResult::unavailable();
            double analysisEndSec = aeVar.toDouble();

            // Find the first timestamp where vertical speed drops below a threshold
            QVector<double> velD = ctx.measurement("GNSS", "velD");
            QVector<double> sAcc = ctx.measurement("GNSS", "sAcc");
            QVector<double> accD = ctx.measurement("GNSS", "accD");
            QVector<double> time = ctx.measurement("GNSS", SessionKeys::Time);

            if (velD.isEmpty() || time.isEmpty() || velD.size() != time.size()
                || sAcc.size() != velD.size() || accD.size() != velD.size()) {
                qWarning() << "Insufficient data to calculate exit time.";
                return CalculationResult::unavailable();
            }

            const double vThreshold = 10.0; // Vertical speed threshold in m/s
            const double maxAccuracy = 1.0; // Maximum speed acccuracy in m/s
            const double minAcceleration = 2.5; // Minimum vertical accleration in m/s^2

            for (int i = 1; i < velD.size(); ++i) {
                if (time[i] < analysisStartSec) continue;
                if (time[i] > analysisEndSec) break;

                // Get interpolation coefficient
                const double a = (vThreshold - velD[i - 1]) / (velD[i] - velD[i - 1]);

                // Check vertical speed
                if (a < 0 || 1 < a) continue;

                // Check accuracy
                const double acc = sAcc[i - 1] + a * (sAcc[i] - sAcc[i - 1]);
                if (acc > maxAccuracy) continue;

                // Check acceleration
                const double az = accD[i - 1] + a * (accD[i] - accD[i - 1]);
                if (az < minAcceleration) continue;

                // Determine exit
                const double tExit = time[i - 1] + a * (time[i] - time[i - 1]) - vThreshold / az;
                return CalculationResult().setAttribute(SessionKeys::ExitTime, tExit);
            }

            qWarning() << "Exit time could not be determined based on current data.";
            return CalculationResult::unavailable();
        };
        addCalculation(registry, d);
    }

    // Video sync time and course reference: each defaults to exit time when
    // not explicitly set by the user. Two unrelated outputs, so two calculations.
    const QList<QPair<QString, QString>> exitTimeDefaults = {
        { QStringLiteral("builtin.attr.syncTime"),  QString::fromLatin1(SessionKeys::SyncTime) },
        { QStringLiteral("builtin.attr.courseRef"), QString::fromLatin1(SessionKeys::CourseRef) }
    };
    for (const auto &entry : exitTimeDefaults) {
        const QString outputKey = entry.second;
        CalculationDescriptor d;
        d.id = entry.first;
        d.inputs = { CalcInput::attribute(SessionKeys::ExitTime) };
        d.outputs = { DependencyKey::attribute(outputKey) };
        d.compute = [outputKey](const EvaluationContext &ctx) -> CalculationResult {
            QVariant exitVar = ctx.attribute(SessionKeys::ExitTime);
            if (!exitVar.canConvert<double>())
                return CalculationResult::unavailable();
            return CalculationResult().setAttribute(outputKey, exitVar);
        };
        addCalculation(registry, d);
    }

    // Manoeuvre start time: walk backward from the last 10 m/s crossing
    // to the local minimum in vertical speed
    {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.attr.manoeuvreStart");
        d.inputs = {
            CalcInput::attribute(SessionKeys::ExitTime),
            CalcInput::attribute(SessionKeys::LandingTime),
            CalcInput::measurement("GNSS", "velD"),
            CalcInput::measurement("GNSS", "sAcc"),
            CalcInput::measurement("GNSS", SessionKeys::Time)
        };
        d.outputs = { DependencyKey::attribute(SessionKeys::ManoeuvreStartTime) };
        d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
            // Retrieve exit time and landing time to bound the search
            QVariant exitVar = ctx.attribute(SessionKeys::ExitTime);
            if (!exitVar.canConvert<double>())
                return CalculationResult::unavailable();
            double exitSec = exitVar.toDouble();

            QVariant landVar = ctx.attribute(SessionKeys::LandingTime);
            if (!landVar.canConvert<double>())
                return CalculationResult::unavailable();
            double landingSec = landVar.toDouble();

            QVector<double> velD = ctx.measurement("GNSS", "velD");
            QVector<double> sAcc = ctx.measurement("GNSS", "sAcc");
            QVector<double> time = ctx.measurement("GNSS", SessionKeys::Time);

            if (velD.isEmpty() || time.isEmpty() || sAcc.isEmpty()
                || velD.size() != time.size() || velD.size() != sAcc.size())
                return CalculationResult::unavailable();

            const double vThreshold = 10.0; // m/s

            // Find the LAST upward crossing of the threshold between exit and landing
            int crossingIdx = -1;

            for (int i = 1; i < velD.size(); ++i) {
                if (time[i] < exitSec) continue;
                if (time[i] > landingSec) break;
                if (velD[i - 1] < vThreshold && velD[i] >= vThreshold) {
                    crossingIdx = i;
                }
            }

            if (crossingIdx < 0)
                return CalculationResult::unavailable();

            // Walk backward from the crossing to find the minimum in velD,
            // tolerating small bumps within the speed accuracy band
            int minIdx = crossingIdx - 1;
            for (int j = crossingIdx - 2; j >= 0; --j) {
                if (time[j] < exitSec) break;
                if (velD[j] < velD[minIdx]) {
                    minIdx = j;
                } else if (velD[j] > velD[minIdx] + 2.0 * sAcc[j]) {
                    break; // confidently above the minimum
                }
            }

            return CalculationResult().setAttribute(SessionKeys::ManoeuvreStartTime, time[minIdx]);
        };
        addCalculation(registry, d);
    }

    // Flare detection: one computation, two outputs. A user override of one
    // output (a stored attribute) coexists with the other calculated output.
    {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.attr.flare");
        d.inputs = {
            CalcInput::attribute(SessionKeys::ExitTime),
            CalcInput::attribute(SessionKeys::LandingTime),
            CalcInput::measurement("GNSS", "hMSL"),
            CalcInput::measurement("GNSS", "vAcc"),
            CalcInput::measurement("GNSS", SessionKeys::Time)
        };
        d.outputs = {
            DependencyKey::attribute(SessionKeys::FlareStartTime),
            DependencyKey::attribute(SessionKeys::FlareEndTime)
        };
        d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
            QVariant exitVar = ctx.attribute(SessionKeys::ExitTime);
            if (!exitVar.canConvert<double>())
                return CalculationResult::unavailable();

            QVariant landVar = ctx.attribute(SessionKeys::LandingTime);
            if (!landVar.canConvert<double>())
                return CalculationResult::unavailable();

            const auto flare = computeFlare(exitVar.toDouble(), landVar.toDouble(),
                                            ctx.measurement("GNSS", "hMSL"),
                                            ctx.measurement("GNSS", "vAcc"),
                                            ctx.measurement("GNSS", SessionKeys::Time));
            if (!flare)
                return CalculationResult::unavailable();
            return CalculationResult()
                .setAttribute(SessionKeys::FlareStartTime, flare->first)
                .setAttribute(SessionKeys::FlareEndTime, flare->second);
        };
        addCalculation(registry, d);
    }

    // Landing time: first flying-to-walking transition within the analysis window
    // "Walking" = vertical speed < 2*sAcc AND horizontal speed < 10 km/h
    //             AND elevation within 10 m of ground
    {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.attr.landingTime");
        d.inputs = {
            CalcInput::attribute(SessionKeys::AnalysisStartTime),
            CalcInput::attribute(SessionKeys::AnalysisEndTime),
            CalcInput::attribute(SessionKeys::GroundElev),
            CalcInput::measurement("GNSS", "velD"),
            CalcInput::measurement("GNSS", "velH"),
            CalcInput::measurement("GNSS", "sAcc"),
            CalcInput::measurement("GNSS", "hMSL"),
            CalcInput::measurement("GNSS", SessionKeys::Time)
        };
        d.outputs = { DependencyKey::attribute(SessionKeys::LandingTime) };
        d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
            // Retrieve analysis window to constrain the search
            QVariant asVar = ctx.attribute(SessionKeys::AnalysisStartTime);
            if (!asVar.canConvert<double>())
                return CalculationResult::unavailable();
            double analysisStartSec = asVar.toDouble();

            QVariant aeVar = ctx.attribute(SessionKeys::AnalysisEndTime);
            if (!aeVar.canConvert<double>())
                return CalculationResult::unavailable();
            double analysisEndSec = aeVar.toDouble();

            QVariant geVar = ctx.attribute(SessionKeys::GroundElev);
            if (!geVar.canConvert<double>())
                return CalculationResult::unavailable();
            double groundElev = geVar.toDouble();

            QVector<double> velD = ctx.measurement("GNSS", "velD");
            QVector<double> velH = ctx.measurement("GNSS", "velH");
            QVector<double> sAcc = ctx.measurement("GNSS", "sAcc");
            QVector<double> hMSL = ctx.measurement("GNSS", "hMSL");
            QVector<double> time = ctx.measurement("GNSS", SessionKeys::Time);

            const int n = velD.size();
            if (n == 0 || velH.size() != n || sAcc.size() != n
                || hMSL.size() != n || time.size() != n)
                return CalculationResult::unavailable();

            const double hSpeedThreshold = 10.0 / 3.6; // 10 km/h in m/s
            const double elevThreshold = 10.0; // metres above ground

            // "Walking" when all conditions are met:
            //   abs(velD) < 2*sAcc  AND  velH < 10 km/h  AND  within 10 m of ground
            auto isWalking = [&](int i) {
                return std::abs(velD[i]) < 2.0 * sAcc[i]
                    && velH[i] < hSpeedThreshold
                    && (hMSL[i] - groundElev) < elevThreshold;
            };

            // Find the FIRST flying-to-walking transition within the analysis window
            for (int i = 1; i < n; ++i) {
                if (time[i] < analysisStartSec) continue;
                if (time[i] > analysisEndSec) break;
                if (!isWalking(i - 1) && isWalking(i)) {
                    return CalculationResult().setAttribute(SessionKeys::LandingTime, time[i]);
                }
            }

            return CalculationResult::unavailable();
        };
        addCalculation(registry, d);
    }

    // Start time and duration: one candidate per sensor, tried in this order.
    // One scan of the sensor's time produces both outputs.
    const QStringList all_sensors = {"GNSS", "BARO", "HUM", "MAG", "IMU", "TIME", "VBAT"};
    for (const QString &sens : all_sensors) {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.attr.timeExtent.") + sens;
        d.inputs = { CalcInput::measurement(sens, SessionKeys::Time) };
        d.outputs = {
            DependencyKey::attribute(SessionKeys::StartTime),
            DependencyKey::attribute(SessionKeys::Duration)
        };
        d.compute = [sens](const EvaluationContext &ctx) -> CalculationResult {
            QVector<double> times = ctx.measurement(sens, SessionKeys::Time);
            if (times.isEmpty()) {
                qWarning() << "No " << sens << "/time data available to calculate start time and duration.";
                return CalculationResult::unavailable();
            }

            double minTime = *std::min_element(times.begin(), times.end());
            double maxTime = *std::max_element(times.begin(), times.end());

            CalculationResult result;
            result.setAttribute(SessionKeys::StartTime, minTime);

            // max >= min by construction, so the duration is never negative
            result.setAttribute(SessionKeys::Duration, maxTime - minTime);
            return result;
        };
        addCalculation(registry, d);
    }

    // Maximum vertical / horizontal speed time (time of the peak between
    // manoeuvre start and landing)
    struct MaxSpeedTime {
        QString id;
        QString measurement;
        QString outputKey;
    };
    const QList<MaxSpeedTime> maxSpeedTimes = {
        { QStringLiteral("builtin.attr.maxVelDTime"), QStringLiteral("velD"),
          QString::fromLatin1(SessionKeys::MaxVelDTime) },
        { QStringLiteral("builtin.attr.maxVelHTime"), QStringLiteral("velH"),
          QString::fromLatin1(SessionKeys::MaxVelHTime) }
    };
    for (const MaxSpeedTime &entry : maxSpeedTimes) {
        const QString measurement = entry.measurement;
        const QString outputKey = entry.outputKey;

        CalculationDescriptor d;
        d.id = entry.id;
        d.inputs = {
            CalcInput::attribute(SessionKeys::ManoeuvreStartTime),
            CalcInput::attribute(SessionKeys::LandingTime),
            CalcInput::measurement("GNSS", measurement),
            CalcInput::measurement("GNSS", SessionKeys::Time)
        };
        d.outputs = { DependencyKey::attribute(outputKey) };
        d.compute = [measurement, outputKey](const EvaluationContext &ctx) -> CalculationResult {
            QVariant msVar = ctx.attribute(SessionKeys::ManoeuvreStartTime);
            if (!msVar.canConvert<double>())
                return CalculationResult::unavailable();

            QVariant landVar = ctx.attribute(SessionKeys::LandingTime);
            if (!landVar.canConvert<double>())
                return CalculationResult::unavailable();

            const auto peak = timeOfMaximum(msVar.toDouble(), landVar.toDouble(),
                                            ctx.measurement("GNSS", measurement),
                                            ctx.measurement("GNSS", SessionKeys::Time));
            if (!peak)
                return CalculationResult::unavailable();
            return CalculationResult().setAttribute(outputKey, *peak);
        };
        addCalculation(registry, d);
    }

    // Ground elevation: automatic calculation by interpolating hMSL at analysis end time.
    // This serves as the data-derived default. If the user sets a value (via the
    // SetGround tool, logbook editing, or "Fixed" mode at import), that stored
    // attribute takes precedence over this calculated fallback.
    {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.attr.groundElev");
        d.inputs = {
            CalcInput::attribute(SessionKeys::AnalysisEndTime),
            CalcInput::measurement("GNSS", "hMSL"),
            CalcInput::measurement("GNSS", SessionKeys::Time)
        };
        d.outputs = { DependencyKey::attribute(SessionKeys::GroundElev) };
        d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
            QVariant aeVar = ctx.attribute(SessionKeys::AnalysisEndTime);
            if (!aeVar.canConvert<double>())
                return CalculationResult::unavailable();
            double analysisEndSec = aeVar.toDouble();

            QVector<double> hMSL = ctx.measurement("GNSS", "hMSL");
            QVector<double> time = ctx.measurement("GNSS", SessionKeys::Time);

            if (hMSL.isEmpty() || time.isEmpty() || hMSL.size() != time.size())
                return CalculationResult::unavailable();

            int n = time.size();
            if (analysisEndSec <= time[0])
                return CalculationResult().setAttribute(SessionKeys::GroundElev, hMSL[0]);
            if (analysisEndSec >= time[n - 1])
                return CalculationResult().setAttribute(SessionKeys::GroundElev, hMSL[n - 1]);

            for (int i = 1; i < n; ++i) {
                if (time[i] >= analysisEndSec) {
                    double t0 = time[i - 1];
                    double t1 = time[i];
                    double a = (analysisEndSec - t0) / (t1 - t0);
                    double groundElev = hMSL[i - 1] + a * (hMSL[i] - hMSL[i - 1]);
                    return CalculationResult().setAttribute(SessionKeys::GroundElev, groundElev);
                }
            }
            return CalculationResult::unavailable();
        };
        addCalculation(registry, d);
    }
}
