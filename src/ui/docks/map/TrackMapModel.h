#ifndef TRACKMAPMODEL_H
#define TRACKMAPMODEL_H

#include <QAbstractListModel>
#include <QColor>
#include <QTimer>
#include <QVariantList>
#include <QVector>

#include "preferences/preferencesmanager.h"
#include "preferences/preferencekeys.h"

namespace FlySight {

class SessionModel;
class PlotRangeModel;
class SessionData;

/**
 * Exposes visible session GNSS tracks for display on a map.
 *
 * Each row corresponds to one visible session.
 * The "trackPoints" role is a QVariantList of QVariantMaps:
 *   { "lat": <double>, "lon": <double>, "t": <UTC seconds> }
 *
 * A visible recording whose "Simplified" track is unavailable (no local-frame
 * origin) or shorter than two points contributes no row and nothing to the
 * bounds. With no row, hasData is false and center and bounds are all 0.
 *
 * Center and bounds are exposed as plain doubles so that QWebChannel
 * can serialize them natively (no QGeoCoordinate / QGeoRectangle).
 */
class TrackMapModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool hasData READ hasData NOTIFY hasDataChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(double centerLat READ centerLat NOTIFY centerChanged)
    Q_PROPERTY(double centerLon READ centerLon NOTIFY centerChanged)
    Q_PROPERTY(double boundsNorth READ boundsNorth NOTIFY boundsChanged)
    Q_PROPERTY(double boundsSouth READ boundsSouth NOTIFY boundsChanged)
    Q_PROPERTY(double boundsEast READ boundsEast NOTIFY boundsChanged)
    Q_PROPERTY(double boundsWest READ boundsWest NOTIFY boundsChanged)

public:
    enum Roles {
        SessionIdRole = Qt::UserRole + 1,
        TrackPointsRole,
        TrackColorRole
    };

    explicit TrackMapModel(SessionModel *sessionModel,
                           PlotRangeModel *rangeModel = nullptr,
                           QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool hasData() const { return m_hasData; }
    int count() const { return m_tracks.size(); }
    double centerLat() const { return m_centerLat; }
    double centerLon() const { return m_centerLon; }
    double boundsNorth() const { return m_boundsNorth; }
    double boundsSouth() const { return m_boundsSouth; }
    double boundsEast() const { return m_boundsEast; }
    double boundsWest() const { return m_boundsWest; }

public slots:
    void rebuild();

private slots:
    void scheduleRebuild();
    void onPreferenceChanged(const QString &key, const QVariant &value);

signals:
    void hasDataChanged();
    void countChanged();
    void centerChanged();
    void boundsChanged();

private:
    struct Track {
        QString sessionId;
        QVariantList points; // [{lat:..., lon:...}, ...]
        QColor color;
    };

    SessionModel *m_sessionModel = nullptr;
    PlotRangeModel *m_rangeModel = nullptr;
    QVector<Track> m_tracks;

    bool computeSessionUtcRange(const SessionData &session,
                                double *outLower, double *outUpper) const;

    bool m_hasData = false;
    double m_centerLat = 0.0;
    double m_centerLon = 0.0;
    double m_boundsNorth = 0.0;
    double m_boundsSouth = 0.0;
    double m_boundsEast = 0.0;
    double m_boundsWest = 0.0;

    double m_trackOpacity = 0.85;
    QTimer m_rebuildTimer;

    QColor colorForSession(const QString &sessionId) const;
};

} // namespace FlySight

#endif // TRACKMAPMODEL_H
