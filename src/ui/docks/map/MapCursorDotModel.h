#ifndef MAPCURSORDOTMODEL_H
#define MAPCURSORDOTMODEL_H

#include <QAbstractListModel>
#include <QColor>
#include <QHash>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QVector>

#include "preferences/preferencesmanager.h"
#include "preferences/preferencekeys.h"

namespace FlySight {

class SessionModel;
class MomentModel;
class PlotRangeModel;

/**
 * Exposes moment-driven dot markers for display on the Google Maps view.
 *
 * Dots are derived from all enabled moments with map presence, sampled from
 * the "Simplified" GNSS track (lat/lon vs SessionKeys::Time) for each
 * targeted visible session. A session whose Simplified track is unavailable
 * gets no dot.
 */
class MapCursorDotModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        SessionIdRole = Qt::UserRole + 1,
        LatRole,
        LonRole,
        ColorRole,
        SizeRole,       // dot diameter in logical pixels (e.g. 10.0 for LargeDot, 6.0 for SmallDot)
        MomentIdRole    // moment id string (for JS-side differentiation)
    };

    explicit MapCursorDotModel(SessionModel *sessionModel, MomentModel *momentModel,
                              PlotRangeModel *rangeModel, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

public slots:
    void rebuild();
    void scheduleRebuild();

private slots:
    void onPreferenceChanged(const QString &key, const QVariant &value);

private:
    struct Dot {
        QString sessionId;
        QString momentId;
        double lat = 0.0;
        double lon = 0.0;
        QColor color;
        double size = 10.0;   // dot diameter in logical pixels
    };

    SessionModel *m_sessionModel = nullptr;
    MomentModel *m_momentModel = nullptr;
    PlotRangeModel *m_rangeModel = nullptr;
    QTimer m_rebuildTimer;
    QVector<Dot> m_dots;

    double m_largeDotSize = 10.0;
    double m_smallDotSize = 6.0;

    static QColor colorForSession(const QString &sessionId);
};

} // namespace FlySight

#endif // MAPCURSORDOTMODEL_H
