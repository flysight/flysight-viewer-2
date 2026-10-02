#include "MapCursorDotModel.h"

#include "momentmodel.h"
#include "plotrangemodel.h"
#include "plotutils.h"
#include "samplecontinuity.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "calculations/timecalculations.h"

#include <QtMath>
#include <QDateTime>

#include <algorithm>
#include <limits>

namespace FlySight {

static constexpr const char *kDefaultSensor = "Simplified";
static constexpr const char *kLatKey = "lat";
static constexpr const char *kLonKey = "lon";


static bool isMonotonic(const QVector<double> &t, int n, bool ascending)
{
    if (n < 2)
        return false;

    for (int i = 1; i < n; ++i) {
        if (ascending) {
            if (t[i] < t[i - 1])
                return false;
        } else {
            if (t[i] > t[i - 1])
                return false;
        }
    }

    return true;
}

// True when utcSeconds lies strictly inside a hole of the GNSS samples (the
// continuity rule on GNSS/_time, never on the simplified track's spacing,
// which the simplifier thinned): the recording has no position there.
static bool insideGnssHole(const SensorTimeAxis &gnss, double utcSeconds)
{
    const auto it = std::lower_bound(gnss.time.cbegin(), gnss.time.cend(), utcSeconds);
    if (it == gnss.time.cbegin() || it == gnss.time.cend() || *it == utcSeconds)
        return false;
    return SampleContinuity::isHoleBefore(gnss.time, std::size_t(it - gnss.time.cbegin()),
                                          gnss.holeThreshold);
}

static bool sampleLatLonAtUtc(const SessionData &session, const SensorTimeAxis &gnss, double utcSeconds,
                              double *outLat, double *outLon)
{
    if (!outLat || !outLon)
        return false;
    if (insideGnssHole(gnss, utcSeconds))
        return false;

    const QVector<double> t =
        session.getMeasurement(QString::fromLatin1(kDefaultSensor), QString::fromLatin1(SessionKeys::Time));
    const QVector<double> lat =
        session.getMeasurement(QString::fromLatin1(kDefaultSensor), QString::fromLatin1(kLatKey));
    const QVector<double> lon =
        session.getMeasurement(QString::fromLatin1(kDefaultSensor), QString::fromLatin1(kLonKey));

    const int n = qMin(t.size(), qMin(lat.size(), lon.size()));
    if (n < 2)
        return false;

    const bool ascending = (t[n - 1] >= t[0]);
    if (!isMonotonic(t, n, ascending))
        return false;

    if (ascending) {
        if (utcSeconds < t[0] || utcSeconds > t[n - 1])
            return false;
    } else {
        if (utcSeconds > t[0] || utcSeconds < t[n - 1])
            return false;
    }

    int i0 = 0;
    int i1 = 1;

    if (ascending) {
        auto it = std::lower_bound(t.begin(), t.begin() + n, utcSeconds);
        if (it == t.begin() + n)
            return false;

        const int idx = static_cast<int>(it - t.begin());

        // Exact match
        if (*it == utcSeconds) {
            const double la = lat[idx];
            const double lo = lon[idx];
            if (!qIsFinite(la) || !qIsFinite(lo))
                return false;
            if (la < -90.0 || la > 90.0 || lo < -180.0 || lo > 180.0)
                return false;

            *outLat = la;
            *outLon = lo;
            return true;
        }

        if (idx == 0) {
            i0 = 0;
            i1 = 1;
        } else {
            i0 = idx - 1;
            i1 = idx;
        }
    } else {
        auto it = std::lower_bound(
            t.begin(), t.begin() + n, utcSeconds,
            [](double a, double b) { return a > b; } // t[] is descending
        );
        if (it == t.begin() + n)
            return false;

        const int idx = static_cast<int>(it - t.begin());

        // Exact match
        if (*it == utcSeconds) {
            const double la = lat[idx];
            const double lo = lon[idx];
            if (!qIsFinite(la) || !qIsFinite(lo))
                return false;
            if (la < -90.0 || la > 90.0 || lo < -180.0 || lo > 180.0)
                return false;

            *outLat = la;
            *outLon = lo;
            return true;
        }

        if (idx == 0) {
            i0 = 0;
            i1 = 1;
        } else {
            i0 = idx - 1;
            i1 = idx;
        }
    }

    const double t0 = t[i0];
    const double t1 = t[i1];
    const double la0 = lat[i0];
    const double la1 = lat[i1];
    const double lo0 = lon[i0];
    const double lo1 = lon[i1];

    if (!qIsFinite(t0) || !qIsFinite(t1) ||
        !qIsFinite(la0) || !qIsFinite(la1) ||
        !qIsFinite(lo0) || !qIsFinite(lo1))
        return false;

    if (la0 < -90.0 || la0 > 90.0 || lo0 < -180.0 || lo0 > 180.0)
        return false;
    if (la1 < -90.0 || la1 > 90.0 || lo1 < -180.0 || lo1 > 180.0)
        return false;

    const double dt = (t1 - t0);
    if (dt == 0.0) {
        // Degenerate time span: fall back to nearest neighbor
        const double d0 = qAbs(utcSeconds - t0);
        const double d1 = qAbs(utcSeconds - t1);

        if (d0 <= d1) {
            *outLat = la0;
            *outLon = lo0;
        } else {
            *outLat = la1;
            *outLon = lo1;
        }
        return true;
    }

    const double alpha = (utcSeconds - t0) / dt;

    *outLat = la0 + alpha * (la1 - la0);
    *outLon = lo0 + alpha * (lo1 - lo0);
    return true;
}

MapCursorDotModel::MapCursorDotModel(SessionModel *sessionModel, MomentModel *momentModel,
                                     PlotRangeModel *rangeModel, QObject *parent)
    : QAbstractListModel(parent)
    , m_sessionModel(sessionModel)
    , m_momentModel(momentModel)
    , m_rangeModel(rangeModel)
{
    // Zero-interval single-shot timer coalesces rapid momentsChanged bursts
    // into a single rebuild.
    m_rebuildTimer.setInterval(0);
    m_rebuildTimer.setSingleShot(true);
    connect(&m_rebuildTimer, &QTimer::timeout, this, &MapCursorDotModel::rebuild);

    if (m_sessionModel) {
        connect(m_sessionModel, &SessionModel::modelChanged,
                this, &MapCursorDotModel::scheduleRebuild);
        connect(m_sessionModel, &SessionModel::visibilityChanged, this,
                [this](const QSet<QString> &, const QSet<QString> &) { scheduleRebuild(); });
    }

    if (m_momentModel) {
        connect(m_momentModel, &MomentModel::momentsChanged,
                this, &MapCursorDotModel::scheduleRebuild);
    }

    if (m_rangeModel) {
        connect(m_rangeModel, &PlotRangeModel::rangeChanged,
                this, &MapCursorDotModel::scheduleRebuild);
    }

    // Connect to preferences system
    connect(&PreferencesManager::instance(), &PreferencesManager::preferenceChanged,
            this, &MapCursorDotModel::onPreferenceChanged);

    // Load initial dot sizes from preferences
    auto &prefs = PreferencesManager::instance();
    m_largeDotSize = prefs.getValue(PreferenceKeys::MapLargeDotSize).toDouble();
    m_smallDotSize = prefs.getValue(PreferenceKeys::MapSmallDotSize).toDouble();

    rebuild();
}

void MapCursorDotModel::scheduleRebuild()
{
    m_rebuildTimer.start();
}

int MapCursorDotModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_dots.size();
}

QVariant MapCursorDotModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_dots.size())
        return QVariant();

    const Dot &d = m_dots.at(index.row());

    switch (role) {
    case SessionIdRole:
        return d.sessionId;
    case LatRole:
        return d.lat;
    case LonRole:
        return d.lon;
    case ColorRole:
        return d.color;
    case SizeRole:
        return d.size;
    case MomentIdRole:
        return d.momentId;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> MapCursorDotModel::roleNames() const
{
    return {
        { SessionIdRole, "sessionId" },
        { LatRole,       "lat" },
        { LonRole,       "lon" },
        { ColorRole,     "color" },
        { SizeRole,      "size" },
        { MomentIdRole,  "momentId" }
    };
}

QColor MapCursorDotModel::colorForSession(const QString &sessionId)
{
    // Keep in sync with TrackMapModel::colorForSession
    const uint h = qHash(sessionId);
    const int hue = static_cast<int>(h % 360);
    QColor c = QColor::fromHsv(hue, 200, 255);

    double opacity = PreferencesManager::instance().getValue(
        PreferenceKeys::MapTrackOpacity).toDouble();
    c.setAlphaF(opacity);

    return c;
}

void MapCursorDotModel::onPreferenceChanged(const QString &key, const QVariant &value)
{
    if (key == PreferenceKeys::MapTrackOpacity) {
        rebuild();
    } else if (key == PreferenceKeys::MapLargeDotSize) {
        m_largeDotSize = value.toDouble();
        rebuild();
    } else if (key == PreferenceKeys::MapSmallDotSize) {
        m_smallDotSize = value.toDouble();
        rebuild();
    }
}

void MapCursorDotModel::rebuild()
{
    QVector<Dot> newDots;
    // Each session's GNSS time axis once for this rebuild, however many
    // moments read it
    SensorTimeAxes axes;

    if (m_sessionModel && m_momentModel) {
        const auto moments = m_momentModel->enabledMoments();

        for (const MomentModel::Moment &moment : moments) {
            // Skip moments with no map presence
            if (moment.traits.mapPresentation == MapPresentation::None)
                continue;

            // Determine dot size from map presentation
            const double dotSize = (moment.traits.mapPresentation == MapPresentation::LargeDot)
                                       ? m_largeDotSize
                                       : m_smallDotSize;

            for (int si = 0; si < m_sessionModel->rowCount(); ++si) {
                const SessionRow &sr = m_sessionModel->rowAt(si);
                if (!sr.isLoaded() || !sr.visible)
                    continue;

                const SessionData &session = sr.session.value();
                const QString sessionId = sr.sessionId;

                if (sessionId.isEmpty())
                    continue;

                double utcSeconds = 0.0;

                if (moment.traits.positionSource == PositionSource::Attribute) {
                    // Attribute-sourced moments: read UTC from session attribute
                    const QVariant attrVal = session.getAttribute(moment.traits.attributeKey);
                    if (!attrVal.canConvert<double>())
                        continue;

                    utcSeconds = attrVal.toDouble();

                    // Filter by visible plot range: skip markers outside the
                    // current plot viewport so the map stays in sync.
                    if (m_rangeModel && m_rangeModel->hasRange()) {
                        const QString xVar = m_rangeModel->xVariable();
                        const auto optOffset = markerOffsetSeconds(
                            session, m_rangeModel->referenceMarkerKey(), xVar);
                        if (optOffset.has_value()) {
                            double lo = m_rangeModel->rangeLower() + *optOffset;
                            double hi = m_rangeModel->rangeUpper() + *optOffset;
                            if (xVar == QLatin1String(SessionKeys::SystemTime)) {
                                auto utcLo = Calculations::systemTimeToUtc(session, lo);
                                auto utcHi = Calculations::systemTimeToUtc(session, hi);
                                if (utcLo.has_value() && utcHi.has_value()) {
                                    lo = *utcLo;
                                    hi = *utcHi;
                                }
                            }
                            if (utcSeconds < lo || utcSeconds > hi)
                                continue;
                        }
                    }

                } else {
                    // MouseInput or External moments: use moment.positionUtc
                    if (!moment.active)
                        continue;

                    // For MouseInput/External: check targetSessions.
                    // Empty targetSessions = auto-visible-overlap: show dots
                    // on all visible sessions that have data at this position.
                    // Non-empty targetSessions: only show for listed sessions.
                    if (!moment.targetSessions.isEmpty()
                        && !moment.targetSessions.contains(sessionId)) {
                        continue;
                    }

                    // Use per-session position when available (multi-session
                    // mouse cursor with different reference marker offsets).
                    auto psIt = moment.sessionPositions.constFind(sessionId);
                    utcSeconds = (psIt != moment.sessionPositions.constEnd())
                                     ? psIt.value()
                                     : moment.positionUtc;
                }

                double lat = 0.0;
                double lon = 0.0;
                if (!sampleLatLonAtUtc(session, axes.of(session, QStringLiteral("GNSS")), utcSeconds, &lat, &lon))
                    continue;

                // Determine color: use traits.color if valid, else colorForSession
                const QColor dotColor = moment.traits.color.isValid()
                                            ? moment.traits.color
                                            : colorForSession(sessionId);

                Dot d;
                d.sessionId = sessionId;
                d.momentId = moment.id;
                d.lat = lat;
                d.lon = lon;
                d.color = dotColor;
                d.size = dotSize;
                newDots.push_back(std::move(d));
            }
        }
    }

    // Skip the model reset if nothing changed
    if (newDots.size() == m_dots.size()) {
        bool same = true;
        for (int i = 0; i < newDots.size(); ++i) {
            const Dot &a = m_dots[i];
            const Dot &b = newDots[i];
            if (a.sessionId != b.sessionId || a.momentId != b.momentId
                || a.lat != b.lat || a.lon != b.lon
                || a.color != b.color || a.size != b.size) {
                same = false;
                break;
            }
        }
        if (same)
            return;
    }

    beginResetModel();
    m_dots = std::move(newDots);
    endResetModel();
}

} // namespace FlySight
