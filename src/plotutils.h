#ifndef PLOTUTILS_H
#define PLOTUTILS_H

#include "momentmodel.h"

#include <QColor>
#include <QHash>
#include <QPair>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>

namespace FlySight {

class SessionData;
struct PlotValue;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Returns the offset (in seconds) of the given marker attribute for a session,
// in the coordinate space of xVariable.
// When referenceMarkerKey is empty, returns 0.0 (absolute mode).
// When xVariable is SessionKeys::Time, the offset is in UTC seconds.
// When xVariable is SessionKeys::SystemTime, the UTC offset is converted
// to system time via Calculations::utcToSystemTime.
// Returns std::nullopt if the attribute is missing / not a valid QDateTime,
// or if the conversion to system time is unavailable.
std::optional<double> markerOffsetSeconds(const SessionData &session,
                                          const QString &referenceMarkerKey,
                                          const QString &xVariable);

QString seriesDisplayName(const PlotValue &pv);

/// The colour a plot is drawn in, everywhere it is drawn: the per-plot colour
/// preference (PreferenceKeys::plotColorKey) when one is stored and valid,
/// else the registry's default. The plot, the legend, the measure tool and
/// the plot settings page all read it here, so a customized colour is the
/// same colour in each.
QColor plotColor(const PlotValue &pv);

// A sensor's time axis as the continuity rule judges it (samplecontinuity.h):
// its own _time samples and their hole threshold, NaN when the axis has none.
// Holes are judged on this axis by sample index whatever the plot's x
// variable is, since _system_time is not the sensor's own sampling. The
// threshold is a selection over the whole axis, so a reader builds this once
// per sensor for a batch of reads and passes it to each read.
struct SensorTimeAxis {
    QVector<double> time;
    double holeThreshold = kNaN;
};

SensorTimeAxis sensorTimeAxis(const SessionData &session, const QString &sensorId);

// The time axes of the sensors one batch of point reads touches (a cursor
// event of the legend, an update of the measure tool), each built on first
// use: one selection per sensor and batch, whatever the number of series read.
// It lives as long as the batch and is not kept between batches, so nothing
// about holes is stored. The sessions must outlive it and stay in place, and
// a reference of() returns is used before the next of() asks for another
// sensor (which may move the axes).
class SensorTimeAxes {
public:
    const SensorTimeAxis &of(const SessionData &session, const QString &sensorId);

private:
    QHash<QPair<const SessionData *, QString>, SensorTimeAxis> m_axes;
};

// The value of yData at x, linear between the two samples of xData that
// bracket x; NaN when x is not bracketed (at or before the first sample,
// after the last). `axis` is the sensor's time axis: when the two bracketing
// samples are the sides of a hole of it, there is no value strictly inside
// the hole, and at the sample after it the value is that sample's. A sensor
// whose _time is missing or of another length than xData has no holes here.
// The interpolation family (calculations/interpolationcalculations.cpp)
// reads a measurement at a marker the same way, on the engine.
double interpolateAtX(const QVector<double> &xData,
                      const QVector<double> &yData,
                      const SensorTimeAxis &axis,
                      double x);

// interpolateAtX() of a session's measurement against its xAxisKey, with the
// sensor's time axis `axis` (sensorTimeAxis(session, sensorId)).
double interpolateSessionMeasurement(const SessionData &session,
                                     const QString &sensorId,
                                     const QString &xAxisKey,
                                     const QString &measurementId,
                                     double x,
                                     const SensorTimeAxis &axis);

// The ground elevation the Set Ground tool sets for a click at plot x
// `xCoord`: GNSS/hMSL read at that time like any point read (NaN outside the
// samples and strictly inside a hole of GNSS/_time).
double groundElevationAt(const SessionData &session,
                         const QString &xVariable,
                         const QString &referenceMarkerKey,
                         double xCoord);

// A graph's points: what a plot draws for one measurement of one session.
struct GraphData {
    QVector<double> keys;
    QVector<double> values;
};

// The points of the graph of a session's measurement against xVariable, the
// reference offset subtracted from the keys: the samples in order, and
// between the two samples around each hole of the sensor's own _time one
// break, a NaN-valued point keyed halfway between them, which the plotting
// library draws as an interruption of the line. Nothing else is added and no
// sample is altered. Empty when the measurement is unavailable or the x
// variable differs from it in length.
GraphData graphData(const SessionData &session,
                    const QString &sensorId,
                    const QString &measurementId,
                    const QString &xVariable,
                    double referenceOffset);

// The value of a graph's line at key x, for the crosshair and the tracers:
// linear between the two points that bracket x, NaN when x is not bracketed
// and across a break (graphData()). A point read at its own key beside a
// break gives its own value, so the samples on either side of a hole read as
// themselves. [begin, end) are the graph's points in key order, each with a
// `key` and a `value` (QCPGraphData), read in place.
template <class Iterator>
double interpolateGraphAt(Iterator begin, Iterator end, double x)
{
    const Iterator upper = std::lower_bound(begin, end, x,
        [](const auto &point, double key) { return point.key < key; });
    if (upper == begin || upper == end)
        return kNaN;
    const Iterator lower = std::prev(upper);

    const double x1 = lower->key, y1 = lower->value;
    const double x2 = upper->key, y2 = upper->value;
    if (x2 == x1)
        return kNaN;
    if (std::isnan(y1) && x == x2)
        return y2;
    return y1 + (y2 - y1) * (x - x1) / (x2 - x1);
}

// A plot's value as text, for the legend, the analysis tab and the measure
// tool: converted to the display unit and rounded to the precision of its
// row's measurement type (the unit converter's), "--" for NaN, one decimal
// unconverted when the row has no type. The measurement's name plays no part.
QString formatValue(double value, const QString &measurementType);

// Format a plot x-axis value for display.
// In absolute UTC mode (no reference marker, xVariable == _time) the value
// is rendered as a UTC timestamp; otherwise it is shown as seconds.
QString formatXAxisValue(double plotX,
                         const QString &xVariable,
                         const QString &referenceMarkerKey);

// Select the highest-priority legend-visible moment from MomentModel.
// Returns a default-constructed Moment (empty id) if no suitable moment is found.
MomentModel::Moment chooseEffectiveMoment(const MomentModel *momentModel);

// Resolve a moment's UTC position for a given session.
// For Attribute-sourced moments: reads the attribute from the session.
// For MouseInput/External: returns moment.positionUtc.
std::optional<double> utcSecondsForMoment(const MomentModel::Moment &moment,
                                          const SessionData &session);

// Convert a UTC seconds value to a plot-axis x-coordinate for a given session.
// Handles system-time conversion and reference marker offsets.
std::optional<double> plotAxisXFromUtc(double utcSeconds,
                                       const QString &xVariable,
                                       const QString &referenceMarkerKey,
                                       const SessionData &session);

} // namespace FlySight

#endif // PLOTUTILS_H
