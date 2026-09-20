#include "plotutils.h"
#include "sessiondata.h"
#include "plotregistry.h"
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

double interpolateAtX(const QVector<double> &xData,
                      const QVector<double> &yData,
                      double x)
{
    if (xData.isEmpty() || yData.isEmpty() || xData.size() != yData.size())
        return kNaN;

    // Reject queries outside the interpolatable range — we need two
    // bracketing points. See also synthesizeInterpolation() in sessiondata.cpp.
    auto it = std::lower_bound(xData.cbegin(), xData.cend(), x);
    if (it == xData.cbegin() || it == xData.cend())
        return kNaN;

    const int idx = static_cast<int>(std::distance(xData.cbegin(), it));
    const double x1 = xData[idx - 1], y1 = yData[idx - 1];
    const double x2 = xData[idx],     y2 = yData[idx];
    if (x2 == x1)
        return kNaN;
    return y1 + (y2 - y1) * (x - x1) / (x2 - x1);
}

double interpolateSessionMeasurement(const SessionData &session,
                                     const QString &sensorId,
                                     const QString &xAxisKey,
                                     const QString &measurementId,
                                     double x)
{
    const QVector<double> xData = session.getMeasurement(sensorId, xAxisKey);
    const QVector<double> yData = session.getMeasurement(sensorId, measurementId);
    return interpolateAtX(xData, yData, x);
}

QString formatValue(double value, const QString &measurementId, const QString &measurementType)
{
    if (std::isnan(value))
        return QStringLiteral("--");

    const QString m = measurementId.toLower();
    if (m.contains(QStringLiteral("lat")) || m.contains(QStringLiteral("lon")))
        return QString::number(value, 'f', 6);

    if (!measurementType.isEmpty()) {
        double displayValue = UnitConverter::instance().convert(value, measurementType);
        int precision = UnitConverter::instance().getPrecision(measurementType);
        if (precision < 0) precision = 1;
        return QString::number(displayValue, 'f', precision);
    }

    int precision = 1;
    if (m.contains(QStringLiteral("time")))
        precision = 3;
    return QString::number(value, 'f', precision);
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

    return formatValue(plotX, QStringLiteral("_time"), QString());
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
