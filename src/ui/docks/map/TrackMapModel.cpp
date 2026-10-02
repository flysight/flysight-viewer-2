#include "TrackMapModel.h"

#include "sessionmodel.h"
#include "sessiondata.h"
#include "plotrangemodel.h"
#include "plotutils.h"
#include "samplecontinuity.h"
#include "calculations/timecalculations.h"

#include <QtMath>
#include <QVariantMap>
#include <QDateTime>
#include <algorithm>
#include <limits>

namespace FlySight {

static constexpr const char *kDefaultSensor = "Simplified";
static constexpr const char *kLatKey = "lat";
static constexpr const char *kLonKey = "lon";

TrackMapModel::TrackMapModel(SessionModel *sessionModel,
                             PlotRangeModel *rangeModel,
                             QObject *parent)
    : QAbstractListModel(parent)
    , m_sessionModel(sessionModel)
    , m_rangeModel(rangeModel)
{
    m_rebuildTimer.setSingleShot(true);
    m_rebuildTimer.setInterval(0);
    connect(&m_rebuildTimer, &QTimer::timeout, this, &TrackMapModel::rebuild);

    if (m_sessionModel) {
        connect(m_sessionModel, &SessionModel::modelChanged,
                this, &TrackMapModel::scheduleRebuild);
        connect(m_sessionModel, &SessionModel::visibilityChanged, this,
                [this](const QSet<QString> &, const QSet<QString> &) { scheduleRebuild(); });
    }

    if (m_rangeModel) {
        connect(m_rangeModel, &PlotRangeModel::rangeChanged,
                this, &TrackMapModel::scheduleRebuild);
    }

    // Connect to preferences system
    connect(&PreferencesManager::instance(), &PreferencesManager::preferenceChanged,
            this, &TrackMapModel::onPreferenceChanged);

    // Load initial preference value
    m_trackOpacity = PreferencesManager::instance().getValue(
        PreferenceKeys::MapTrackOpacity).toDouble();

    rebuild();
}

int TrackMapModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_tracks.size();
}

QVariant TrackMapModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tracks.size())
        return QVariant();

    const Track &t = m_tracks.at(index.row());

    switch (role) {
    case SessionIdRole:
        return t.sessionId;
    case TrackPointsRole:
        return t.runs;
    case TrackColorRole:
        return t.color;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> TrackMapModel::roleNames() const
{
    return {
        { SessionIdRole,    "sessionId" },
        { TrackPointsRole,  "trackPoints" },
        { TrackColorRole,   "trackColor" }
    };
}

QColor TrackMapModel::colorForSession(const QString &sessionId) const
{
    const uint h = qHash(sessionId);
    const int hue = static_cast<int>(h % 360);
    QColor c = QColor::fromHsv(hue, 200, 255);
    c.setAlphaF(m_trackOpacity);
    return c;
}

void TrackMapModel::onPreferenceChanged(const QString &key, const QVariant &value)
{
    if (key == PreferenceKeys::MapTrackOpacity) {
        m_trackOpacity = value.toDouble();
        rebuild(); // Rebuild tracks with new opacity
    }
}

bool TrackMapModel::computeSessionUtcRange(const SessionData &session,
                                           double *outLower, double *outUpper) const
{
    if (!m_rangeModel || !m_rangeModel->hasRange())
        return false;

    const QString refKey = m_rangeModel->referenceMarkerKey();
    const QString xVar = m_rangeModel->xVariable();
    const auto optOffset = markerOffsetSeconds(session, refKey, xVar);
    if (!optOffset.has_value())
        return false;
    const double offset = *optOffset;

    // Convert from plot coordinates to absolute time in the x-variable's domain
    double absLower = m_rangeModel->rangeLower() + offset;
    double absUpper = m_rangeModel->rangeUpper() + offset;

    // If in system time mode, convert to UTC for filtering against UTC track data
    if (xVar == QLatin1String(SessionKeys::SystemTime)) {
        auto utcLower = Calculations::systemTimeToUtc(session, absLower);
        auto utcUpper = Calculations::systemTimeToUtc(session, absUpper);
        if (!utcLower.has_value() || !utcUpper.has_value())
            return false;
        *outLower = *utcLower;
        *outUpper = *utcUpper;
    } else {
        *outLower = absLower;
        *outUpper = absUpper;
    }
    return true;
}

void TrackMapModel::scheduleRebuild()
{
    m_rebuildTimer.start();
}

void TrackMapModel::rebuild()
{
    const bool oldHasData = m_hasData;
    const int oldCount = m_tracks.size();
    const double oldCenterLat = m_centerLat;
    const double oldCenterLon = m_centerLon;
    const double oldBoundsNorth = m_boundsNorth;
    const double oldBoundsSouth = m_boundsSouth;
    const double oldBoundsEast = m_boundsEast;
    const double oldBoundsWest = m_boundsWest;

    beginResetModel();
    m_tracks.clear();

    bool haveBounds = false;
    double minLat = 0.0, maxLat = 0.0, minLon = 0.0, maxLon = 0.0;

    if (m_sessionModel) {
        for (int si = 0; si < m_sessionModel->rowCount(); ++si) {
            const SessionRow &sr = m_sessionModel->rowAt(si);
            if (!sr.isLoaded() || !sr.visible)
                continue;

            const SessionData &session = sr.session.value();

            const QVector<double> lat =
                session.getMeasurement(QString::fromLatin1(kDefaultSensor), QString::fromLatin1(kLatKey));
            const QVector<double> lon =
                session.getMeasurement(QString::fromLatin1(kDefaultSensor), QString::fromLatin1(kLonKey));
            const QVector<double> tUtc =
                session.getMeasurement(QString::fromLatin1(kDefaultSensor), QString::fromLatin1(SessionKeys::Time));

            const int n = qMin(tUtc.size(), qMin(lat.size(), lon.size()));
            if (n < 2)
                continue;

            // Compute UTC range filter for this session
            double filterLower = -std::numeric_limits<double>::infinity();
            double filterUpper = std::numeric_limits<double>::infinity();
            computeSessionUtcRange(session, &filterLower, &filterUpper);

            // The runs of the GNSS samples, by their first fix's time: a
            // simplified point belongs to the last run that starts at or
            // before it (its samples are GNSS samples)
            const QVector<double> gnssTime =
                session.getMeasurement(QStringLiteral("GNSS"), QString::fromLatin1(SessionKeys::Time));
            QVector<double> runStarts;
            for (const SampleContinuity::Run &run : SampleContinuity::runs(gnssTime))
                runStarts.append(gnssTime[qsizetype(run.begin)]);
            const auto runOf = [&runStarts](double t) {
                return qsizetype(std::upper_bound(runStarts.cbegin(), runStarts.cend(), t) - runStarts.cbegin());
            };

            QVariantList runs;

            auto includeInBounds = [&](double la, double lo) {
                if (!haveBounds) {
                    haveBounds = true;
                    minLat = maxLat = la;
                    minLon = maxLon = lo;
                } else {
                    minLat = qMin(minLat, la);
                    maxLat = qMax(maxLat, la);
                    minLon = qMin(minLon, lo);
                    maxLon = qMax(maxLon, lo);
                }
            };

            // One run of simplified points [begin, end) at a time: the range
            // filter and the edge interpolation never reach into another run
            auto addRun = [&](int begin, int end) {
                QVariantList points;

                int lastBeforeIdx = -1;
                int firstInsideIdx = -1;
                int lastInsideIdx = -1;
                int firstAfterIdx = -1;

                for (int i = begin; i < end; ++i) {
                    const double la = lat[i];
                    const double lo = lon[i];
                    const double tt = tUtc[i];

                    if (!qIsFinite(la) || !qIsFinite(lo) || !qIsFinite(tt))
                        continue;
                    if (la < -90.0 || la > 90.0 || lo < -180.0 || lo > 180.0)
                        continue;

                    if (tt < filterLower) {
                        lastBeforeIdx = i;
                        continue;
                    }

                    if (tt > filterUpper) {
                        firstAfterIdx = i;
                        break;
                    }

                    // Point is inside the visible range
                    if (firstInsideIdx < 0)
                        firstInsideIdx = i;
                    lastInsideIdx = i;

                    QVariantMap pt;
                    pt.insert(QStringLiteral("lat"), la);
                    pt.insert(QStringLiteral("lon"), lo);
                    pt.insert(QStringLiteral("t"), tt);
                    points.push_back(pt);
                    includeInBounds(la, lo);
                }

                // Interpolate at range boundaries so tracks extend to the
                // plot edges instead of stopping at the last data point.
                auto addBoundaryPoint = [&](int idxA, int idxB, double targetT, bool prepend) {
                    const double t1 = tUtc[idxA], t2 = tUtc[idxB];
                    if (t2 == t1) return;
                    const double frac = (targetT - t1) / (t2 - t1);
                    const double iLat = lat[idxA] + frac * (lat[idxB] - lat[idxA]);
                    const double iLon = lon[idxA] + frac * (lon[idxB] - lon[idxA]);

                    QVariantMap pt;
                    pt.insert(QStringLiteral("lat"), iLat);
                    pt.insert(QStringLiteral("lon"), iLon);
                    pt.insert(QStringLiteral("t"), targetT);

                    if (prepend)
                        points.prepend(pt);
                    else
                        points.push_back(pt);
                    includeInBounds(iLat, iLon);
                };

                // Lower boundary interpolation
                if (lastBeforeIdx >= 0) {
                    const int nextIdx = (firstInsideIdx >= 0) ? firstInsideIdx : firstAfterIdx;
                    if (nextIdx >= 0)
                        addBoundaryPoint(lastBeforeIdx, nextIdx, filterLower, true);
                }

                // Upper boundary interpolation
                if (firstAfterIdx >= 0) {
                    const int prevIdx = (lastInsideIdx >= 0) ? lastInsideIdx : lastBeforeIdx;
                    if (prevIdx >= 0)
                        addBoundaryPoint(prevIdx, firstAfterIdx, filterUpper, false);
                }

                if (points.size() >= 2)
                    runs.push_back(points);
            };

            // A point without a finite time stays with the run it is in (it
            // is skipped there like any invalid point)
            int begin = 0;
            qsizetype currentRun = -1;
            for (int i = 0; i < n; ++i) {
                if (!qIsFinite(tUtc[i]))
                    continue;
                const qsizetype run = runOf(tUtc[i]);
                if (currentRun >= 0 && run != currentRun) {
                    addRun(begin, i);
                    begin = i;
                }
                currentRun = run;
            }
            addRun(begin, n);

            if (runs.isEmpty())
                continue;

            const QString sessionId =
                session.getAttribute(SessionKeys::SessionId).toString();

            Track t;
            t.sessionId = sessionId;
            t.runs = std::move(runs);
            t.color = colorForSession(sessionId);
            m_tracks.push_back(std::move(t));
        }
    }

    endResetModel();

    m_hasData = !m_tracks.isEmpty();

    if (haveBounds) {
        m_boundsNorth = maxLat;
        m_boundsSouth = minLat;
        m_boundsEast  = maxLon;
        m_boundsWest  = minLon;
        m_centerLat   = (maxLat + minLat) / 2.0;
        m_centerLon   = (maxLon + minLon) / 2.0;
    } else {
        m_boundsNorth = 0.0;
        m_boundsSouth = 0.0;
        m_boundsEast  = 0.0;
        m_boundsWest  = 0.0;
        m_centerLat   = 0.0;
        m_centerLon   = 0.0;
    }

    if (oldHasData != m_hasData) emit hasDataChanged();
    if (oldCount != m_tracks.size()) emit countChanged();
    if (oldCenterLat != m_centerLat || oldCenterLon != m_centerLon)
        emit centerChanged();
    if (oldBoundsNorth != m_boundsNorth || oldBoundsSouth != m_boundsSouth ||
        oldBoundsEast != m_boundsEast || oldBoundsWest != m_boundsWest)
        emit boundsChanged();
}

} // namespace FlySight
