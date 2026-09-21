#ifndef LOCALCOORDINATECALCULATIONS_H
#define LOCALCOORDINATECALCULATIONS_H

namespace FlySight {

class CalculationRegistry;

namespace Calculations {

/// Register the recording-wide local north/east/down frame with the
/// calculation engine: builtin.local.coordinates (one calculation; attribute
/// outputs _LOCAL_ORIGIN_LAT / _LON / _HMSL / _INDEX and measurement outputs
/// Local/north, east, down, velN, velE, velD) and the two time axes
/// builtin.local.time (Local/_time) and builtin.local.systemTime
/// (Local/_system_time), which are the GNSS axes. See docs/LOCAL_COORDINATES.md.
void registerLocalCoordinateCalculations(CalculationRegistry &registry);

} // namespace Calculations
} // namespace FlySight

#endif // LOCALCOORDINATECALCULATIONS_H
