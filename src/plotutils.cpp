#include "plotutils.h"
#include "samplecontinuity.h"
#include "sessiondata.h"
#include "plotregistry.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "units/unitconverter.h"
#include "calculations/timecalculations.h"

#include <QDateTime>
#include <QTimeZone>
#include <QVariant>
#include <QStringLiteral>

#include <algorithm>

namespace FlySight {

std::optional<double> markerOffsetSeconds(const SessionData &session,
                                          const QString &referenceMarkerKey,
                                          const QString &xVariable)
{
    if (referenceMarkerKey.isEmpty())
        return 0.0;
    QVariant v = session.getAttribute(referenceMarkerKey);
    if (!v.canConvert<double>())
        return std::nullopt;
    const double utcSeconds = v.toDouble();

    if (xVariable == QLatin1String(SessionKeys::SystemTime)) {
        return Calculations::utcToSystemTime(session, utcSeconds);
    }

    // Default: SessionKeys::Time or any other x-variable — return UTC seconds
    return utcSeconds;
}

QString seriesDisplayName(const PlotValue &pv)
{
    QString displayUnits = pv.plotUnits;
    if (!pv.measurementType.isEmpty()) {
        QString converted = UnitConverter::instance().getUnitLabel(pv.measurementType);
        if (!converted.isEmpty())
            displayUnits = converted;
    }
    if (!displayUnits.isEmpty())
        return QStringLiteral("%1 (%2)").arg(pv.plotName, displayUnits);
    return pv.plotName;
}

QColor plotColor(const PlotValue &pv)
{
    const QVariant stored = PreferencesManager::instance().getValue(
        PreferenceKeys::plotColorKey(pv.sensorID, pv.measurementID));
    if (stored.isValid()) {
        const QColor color = stored.value<QColor>();
        if (color.isValid())
            return color;
    }
    return pv.defaultColor;
}

SensorTimeAxis sensorTimeAxis(const SessionData &session, const QString &sensorId)
{
    SensorTimeAxis axis;
    axis.time = session.getMeasurement(sensorId, QLatin1String(SessionKeys::Time));
    axis.holeThreshold = SampleContinuity::holeThreshold(axis.time);
    return axis;
}

const SensorTimeAxis &SensorTimeAxes::of(const SessionData &session, const QString &sensorId)
{
    const std::pair<const SessionData *, QString> key(&session, sensorId);
    auto it = m_axes.find(key);
    if (it == m_axes.end())
        it = m_axes.emplace(key, sensorTimeAxis(session, sensorId)).first;
    return it->second;
}

double interpolateAtX(const QVector<double> &xData,
                      const QVector<double> &yData,
                      const SensorTimeAxis &axis,
                      double x)
{
    if (xData.isEmpty() || yData.isEmpty() || xData.size() != yData.size())
        return kNaN;

    // Reject queries outside the interpolatable range: we need two
    // bracketing points.
    auto it = std::lower_bound(xData.cbegin(), xData.cend(), x);
    if (it == xData.cbegin() || it == xData.cend())
        return kNaN;

    const int idx = static_cast<int>(std::distance(xData.cbegin(), it));
    const double x1 = xData[idx - 1], y1 = yData[idx - 1];
    const double x2 = xData[idx],     y2 = yData[idx];
    if (x2 == x1)
        return kNaN;

    // Nothing is read across a hole of the sensor's own time: the bracketing
    // pair is judged by index on its _time, whatever x is.
    if (axis.time.size() == xData.size()
        && SampleContinuity::isHoleBefore(axis.time, std::size_t(idx), axis.holeThreshold))
        return x == x2 ? y2 : kNaN;
    return y1 + (y2 - y1) * (x - x1) / (x2 - x1);
}

double interpolateSessionMeasurement(const SessionData &session,
                                     const QString &sensorId,
                                     const QString &xAxisKey,
                                     const QString &measurementId,
                                     double x,
                                     const SensorTimeAxis &axis)
{
    const QVector<double> xData = session.getMeasurement(sensorId, xAxisKey);
    const QVector<double> yData = session.getMeasurement(sensorId, measurementId);
    return interpolateAtX(xData, yData, axis, x);
}

double groundElevationAt(const SessionData &session,
                         const QString &xVariable,
                         const QString &referenceMarkerKey,
                         double xCoord)
{
    const QString sensor = QStringLiteral("GNSS");
    const double offset = markerOffsetSeconds(session, referenceMarkerKey, xVariable).value_or(0.0);
    return interpolateSessionMeasurement(session, sensor, xVariable, QStringLiteral("hMSL"),
                                         xCoord + offset, sensorTimeAxis(session, sensor));
}

GraphData graphData(const SessionData &session,
                    const QString &sensorId,
                    const QString &measurementId,
                    const QString &xVariable,
                    double referenceOffset)
{
    const QVector<double> yData = session.getMeasurement(sensorId, measurementId);
    const QVector<double> xData = session.getMeasurement(sensorId, xVariable);
    if (yData.isEmpty() || xData.size() != yData.size())
        return {};

    const SensorTimeAxis axis = sensorTimeAxis(session, sensorId);
    const bool judged = axis.time.size() == yData.size();
    const auto key = [&xData, referenceOffset](qsizetype i) {
        return referenceOffset != 0.0 ? xData[i] - referenceOffset : xData[i];
    };

    GraphData graph;
    graph.keys.reserve(xData.size());
    graph.values.reserve(yData.size());
    for (qsizetype i = 0; i < yData.size(); ++i) {
        if (judged && SampleContinuity::isHoleBefore(axis.time, std::size_t(i), axis.holeThreshold)) {
            const double before = graph.keys.last(), after = key(i);
            graph.keys.append(before + (after - before) / 2);
            graph.values.append(kNaN);
        }
        graph.keys.append(key(i));
        graph.values.append(yData[i]);
    }
    return graph;
}

QString formatValue(double value, const QString &measurementType)
{
    if (std::isnan(value))
        return QStringLiteral("--");

    // The type decides everything, as it does for a logbook column; the
    // measurement's name never enters. A substring test on the name once
    // did, and "lon" in accAlongTrack made the along-track accelerations
    // print raw to six decimals.
    if (!measurementType.isEmpty()) {
        double displayValue = UnitConverter::instance().convert(value, measurementType);
        int precision = UnitConverter::instance().getPrecision(measurementType);
        if (precision < 0) precision = 1;
        return QString::number(displayValue, 'f', precision);
    }
    return QString::number(value, 'f', 1);
}

QString formatXAxisValue(double plotX,
                         const QString &xVariable,
                         const QString &referenceMarkerKey)
{
    if (std::isnan(plotX))
        return QStringLiteral("--");

    // Absolute UTC mode: no reference marker and x-variable is _time.
    if (referenceMarkerKey.isEmpty() && xVariable == QLatin1String(SessionKeys::Time)) {
        return QDateTime::fromMSecsSinceEpoch(qint64(plotX * 1000.0), QTimeZone::UTC)
                   .toString(QStringLiteral("HH:mm:ss.zzz"));
    }

    // Seconds from the reference marker, to the millisecond like the
    // timestamps above; not the time type's precision, which is the
    // measure tool's for a duration.
    return QString::number(plotX, 'f', 3);
}

MomentModel::Moment chooseEffectiveMoment(const MomentModel *momentModel)
{
    if (!momentModel)
        return MomentModel::Moment{};

    const QVector<MomentModel::Moment> enabled = momentModel->enabledMoments();

    // 1) Prefer mouse moment when active and it has usable targets
    for (const MomentModel::Moment &m : enabled) {
        if (m.id == QStringLiteral("mouse")
            && m.traits.legendVisibility == LegendVisibility::Visible
            && m.active
            && !m.targetSessions.isEmpty()) {
            return m;
        }
    }

    // 2) Fall back to first active non-mouse moment with Visible legend
    for (const MomentModel::Moment &m : enabled) {
        if (m.id == QStringLiteral("mouse"))
            continue;
        if (m.traits.legendVisibility != LegendVisibility::Visible)
            continue;
        if (m.active)
            return m;
    }

    // 3) None
    return MomentModel::Moment{};
}

std::optional<double> utcSecondsForMoment(const MomentModel::Moment &moment,
                                          const SessionData &session)
{
    if (moment.traits.positionSource == PositionSource::Attribute) {
        // Read the position from the session's attribute
        const QVariant v = session.getAttribute(moment.traits.attributeKey);
        if (!v.canConvert<double>())
            return std::nullopt;
        return v.toDouble();
    }

    // MouseInput or External: check per-session positions first
    if (!moment.sessionPositions.isEmpty()) {
        const QString sid = session.getAttribute(SessionKeys::SessionId).toString();
        auto it = moment.sessionPositions.constFind(sid);
        if (it != moment.sessionPositions.constEnd())
            return it.value();
    }

    return moment.positionUtc;
}

std::optional<double> plotAxisXFromUtc(double utcSeconds,
                                       const QString &xVariable,
                                       const QString &referenceMarkerKey,
                                       const SessionData &session)
{
    double absoluteX = utcSeconds;

    if (xVariable == QLatin1String(SessionKeys::SystemTime)) {
        auto st = Calculations::utcToSystemTime(session, utcSeconds);
        if (!st.has_value())
            return std::nullopt;
        absoluteX = *st;
    }

    const auto optOffset = markerOffsetSeconds(session, referenceMarkerKey, xVariable);
    if (!optOffset.has_value())
        return std::nullopt;

    return absoluteX - *optOffset;
}

} // namespace FlySight
