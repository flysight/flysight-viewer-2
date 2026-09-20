#ifndef TIMECALCULATIONS_H
#define TIMECALCULATIONS_H

#include <optional>

namespace FlySight {

class SessionData;  // forward declaration
class CalculationRegistry;

namespace Calculations {

/// Register the time-related calculations with the calculation engine:
/// - builtin.time.fit: _TIME_FIT_A and _TIME_FIT_B (system time -> UTC linear fit)
/// - builtin.time.utc.<SENSOR>: {sensor}/_time (GNSS passthrough, others through the fit)
/// - builtin.time.system.<SENSOR>: {sensor}/_system_time (GNSS inverse fit, others passthrough)
void registerTimeCalculations(CalculationRegistry &registry);

/// Convert a single system-time value to UTC using cached linear-fit coefficients.
/// Returns std::nullopt if the fit coefficients are not available.
std::optional<double> systemTimeToUtc(const SessionData &session, double systemTime);

/// Convert a UTC value back to system time using cached linear-fit coefficients.
/// Returns std::nullopt if the fit coefficients are not available or if the fit is degenerate (a == 0).
std::optional<double> utcToSystemTime(const SessionData &session, double utc);

} // namespace Calculations
} // namespace FlySight

#endif // TIMECALCULATIONS_H
