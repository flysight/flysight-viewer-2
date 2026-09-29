#ifndef PLOTUTILS_H
#define PLOTUTILS_H

#include "momentmodel.h"

#include <QColor>
#include <QString>
#include <QVector>

#include <cmath>
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

double interpolateAtX(const QVector<double> &xData,
                      const QVector<double> &yData,
                      double x);

double interpolateSessionMeasurement(const SessionData &session,
                                     const QString &sensorId,
                                     const QString &xAxisKey,
                                     const QString &measurementId,
                                     double x);

QString formatValue(double value,
                    const QString &measurementId,
                    const QString &measurementType);

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
