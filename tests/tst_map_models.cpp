// The map's two models on the simplified track, driven by the real
// SessionModel: a recording without a local-frame origin has no track and no
// cursor dot and is left out of the bounds, the bounds are cleared when no
// track remains, and everything returns when the source is corrected. The
// source is corrected the way the application does it, through
// SessionModel::mergeSessions, and the models are left to rebuild from their
// own signal connections and timers. TrackMapModel and MapCursorDotModel are
// application sources compiled into this test (tests/CMakeLists.txt).

#include <memory>

#include <QSignalSpy>
#include <QtTest>

#include "builtinfixture.h"
#include "engine/calculationregistry.h"
#include "logbookcolumn.h"
#include "momentmodel.h"
#include "plotrangemodel.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"
#include "ui/docks/map/MapCursorDotModel.h"
#include "ui/docks/map/TrackMapModel.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

constexpr double T0 = DescentFixture::T0;
constexpr int Rows = 296;               // GNSS rows of the descent fixture

bool isNear(double actual, double expected)
{
    return qAbs(actual - expected) <= 1e-9;
}

} // namespace

class MapModelsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void tracksDotsAndBounds();
    void noOriginRemovesTrackAndDot();
    void boundsClearedWhenNoTrack();
    void recoversAfterSourceCorrection();
    void hiddenRecordingStaysOutAfterCorrection();
    void rangeFilterOnRecoveredTrack();

private:
    static SessionData fixture(const QByteArray &id);
    void correctSource(const QByteArray &id, double hAccValue);
    QStringList trackIds() const;
    QStringList dotIds() const;
    QVariantList trackPoints(const QString &sessionId) const;

    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<PlotRangeModel> m_range;
    std::unique_ptr<MomentModel> m_moments;
    std::unique_ptr<TrackMapModel> m_tracks;
    std::unique_ptr<MapCursorDotModel> m_dots;
    QStringList m_registryBefore;
};

void MapModelsTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();

    // One logbook column that reads stored data only, so that the model has
    // valid indexes to report without the merge warming any calculation.
    PreferencesManager &prefs = PreferencesManager::instance();
    prefs.registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumn description;
    description.type = ColumnType::SessionAttribute;
    description.attributeKey = QString::fromLatin1(SessionKeys::Description);
    LogbookColumnStore::instance().setColumns({description});

    // What the map models read in their constructors (the main window's
    // defaults). They are not core preferences.
    prefs.registerPreference(PreferenceKeys::MapLargeDotSize, 10);
    prefs.registerPreference(PreferenceKeys::MapSmallDotSize, 6);
    prefs.registerPreference(PreferenceKeys::MapTrackOpacity, 0.85);
}

// The descent fixture runs along one meridian from latitude 45.0 to 45.0295.
// Session "a" is on longitude -75, session "b" on -74.
SessionData MapModelsTest::fixture(const QByteArray &id)
{
    SessionData session = DescentFixture::load(id);
    if (id == "b")
        session.setMeasurement("GNSS", "lon", QVector<double>(Rows, -74.0));
    return session;
}

// Two visible sessions, no plot range, and one moment at the exit time, which
// lies inside the track.
void MapModelsTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    m_registryBefore = CalculationRegistry::instance().registeredIds();

    m_model = std::make_unique<SessionModel>();
    SessionData a = fixture("a");
    SessionData b = fixture("b");
    a.setVisible(true);     // a created row takes the session's visibility
    b.setVisible(true);
    m_model->mergeSessions({a, b});
    QCOMPARE(m_model->rowCount(), 2);

    m_range = std::make_unique<PlotRangeModel>();

    m_moments = std::make_unique<MomentModel>();
    MomentTraits exit;
    exit.positionSource = PositionSource::Attribute;
    exit.attributeKey = QString::fromLatin1(SessionKeys::ExitTime);
    exit.mapPresentation = MapPresentation::LargeDot;
    m_moments->registerMoment(QStringLiteral("exit"), QStringLiteral("Exit"), exit);

    m_tracks = std::make_unique<TrackMapModel>(m_model.get(), m_range.get());
    m_dots = std::make_unique<MapCursorDotModel>(m_model.get(), m_moments.get(), m_range.get());
}

// The map models go first, the session model before the registry is compared
// (a live model schedules a calculation-environment check).
void MapModelsTest::cleanup()
{
    m_dots.reset();
    m_tracks.reset();
    m_moments.reset();
    m_range.reset();
    m_model.reset();
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
}

// The application's own source-change path: the file is imported again with a
// different accuracy column, and the merge replaces that column only.
void MapModelsTest::correctSource(const QByteArray &id, double hAccValue)
{
    SessionData session = fixture(id);
    session.setMeasurement("GNSS", "hAcc", QVector<double>(Rows, hAccValue));
    m_model->mergeSessions({session});
}

QStringList MapModelsTest::trackIds() const
{
    QStringList ids;
    for (int row = 0; row < m_tracks->rowCount(); ++row)
        ids.append(m_tracks->data(m_tracks->index(row), TrackMapModel::SessionIdRole).toString());
    ids.sort();
    return ids;
}

QStringList MapModelsTest::dotIds() const
{
    QStringList ids;
    for (int row = 0; row < m_dots->rowCount(); ++row)
        ids.append(m_dots->data(m_dots->index(row), MapCursorDotModel::SessionIdRole).toString());
    ids.sort();
    return ids;
}

QVariantList MapModelsTest::trackPoints(const QString &sessionId) const
{
    for (int row = 0; row < m_tracks->rowCount(); ++row) {
        const QModelIndex index = m_tracks->index(row);
        if (m_tracks->data(index, TrackMapModel::SessionIdRole).toString() == sessionId)
            return m_tracks->data(index, TrackMapModel::TrackPointsRole).toList();
    }
    return {};
}

void MapModelsTest::tracksDotsAndBounds()
{
    QCOMPARE(trackIds(), QStringList({"a", "b"}));
    QVERIFY(m_tracks->hasData());
    QCOMPARE(m_tracks->count(), 2);
    QCOMPARE(dotIds(), QStringList({"a", "b"}));

    QVERIFY(isNear(m_tracks->boundsSouth(), 45.0));
    QVERIFY(isNear(m_tracks->boundsNorth(), 45.0295));
    QVERIFY(isNear(m_tracks->boundsWest(), -75.0));
    QVERIFY(isNear(m_tracks->boundsEast(), -74.0));
    QVERIFY(isNear(m_tracks->centerLat(), 45.01475));
    QVERIFY(isNear(m_tracks->centerLon(), -74.5));

    // The fixture is a straight line: two points per track
    for (const QString &id : {QStringLiteral("a"), QStringLiteral("b")}) {
        const QVariantList points = trackPoints(id);
        QCOMPARE(points.size(), 2);
        for (const QVariant &point : points) {
            const QVariantMap map = point.toMap();
            QCOMPARE(map.size(), 3);
            QVERIFY(map.contains(QStringLiteral("lat")));
            QVERIFY(map.contains(QStringLiteral("lon")));
            QVERIFY(map.contains(QStringLiteral("t")));
        }
        QVERIFY(isNear(points.first().toMap().value(QStringLiteral("lat")).toDouble(), 45.0));
        QVERIFY(isNear(points.last().toMap().value(QStringLiteral("lat")).toDouble(), 45.0295));
        QCOMPARE(points.first().toMap().value(QStringLiteral("t")).toDouble(), T0);
        QCOMPARE(points.last().toMap().value(QStringLiteral("t")).toDouble(), T0 + 295.0);
    }
}

// No fix of "a" reaches 10 m: it has no local frame, so no track and no dot,
// and the map fits "b" alone. The recording itself is still visible.
void MapModelsTest::noOriginRemovesTrackAndDot()
{
    correctSource("a", 10.0);

    QTRY_COMPARE(m_tracks->rowCount(), 1);
    QCOMPARE(trackIds(), QStringList({"b"}));
    QTRY_COMPARE(m_dots->rowCount(), 1);
    QCOMPARE(dotIds(), QStringList({"b"}));

    QVERIFY(m_tracks->hasData());
    QVERIFY(isNear(m_tracks->boundsWest(), -74.0));
    QVERIFY(isNear(m_tracks->boundsEast(), -74.0));
    QVERIFY(isNear(m_tracks->centerLon(), -74.0));

    const int row = m_model->getSessionRow(QStringLiteral("a"));
    QVERIFY(row >= 0);
    QVERIFY(m_model->rowAt(row).visible);
    QVERIFY(m_model->rowAt(row).isLoaded());
}

void MapModelsTest::boundsClearedWhenNoTrack()
{
    QSignalSpy hasDataSpy(m_tracks.get(), &TrackMapModel::hasDataChanged);
    QSignalSpy boundsSpy(m_tracks.get(), &TrackMapModel::boundsChanged);

    correctSource("a", 10.0);
    correctSource("b", 10.0);

    QTRY_COMPARE(m_tracks->rowCount(), 0);
    QTRY_COMPARE(m_dots->rowCount(), 0);
    QVERIFY(!m_tracks->hasData());
    QCOMPARE(m_tracks->count(), 0);

    // Cleared means exactly zero, not a small number
    QVERIFY(m_tracks->centerLat() == 0.0);
    QVERIFY(m_tracks->centerLon() == 0.0);
    QVERIFY(m_tracks->boundsNorth() == 0.0);
    QVERIFY(m_tracks->boundsSouth() == 0.0);
    QVERIFY(m_tracks->boundsEast() == 0.0);
    QVERIFY(m_tracks->boundsWest() == 0.0);

    QCOMPARE(hasDataSpy.count(), 1);
    QVERIFY(boundsSpy.count() >= 1);

    // Another rebuild changes nothing
    const int boundsEmissions = boundsSpy.count();
    m_tracks->rebuild();
    m_dots->rebuild();
    QCOMPARE(hasDataSpy.count(), 1);
    QCOMPARE(boundsSpy.count(), boundsEmissions);
    QCOMPARE(m_tracks->rowCount(), 0);
    QCOMPARE(m_dots->rowCount(), 0);
}

void MapModelsTest::recoversAfterSourceCorrection()
{
    correctSource("a", 10.0);
    correctSource("b", 10.0);
    QTRY_COMPARE(m_tracks->rowCount(), 0);
    QTRY_COMPARE(m_dots->rowCount(), 0);

    QSignalSpy hasDataSpy(m_tracks.get(), &TrackMapModel::hasDataChanged);
    QSignalSpy boundsSpy(m_tracks.get(), &TrackMapModel::boundsChanged);

    correctSource("a", 1.0);

    QTRY_COMPARE(m_tracks->rowCount(), 1);
    QCOMPARE(trackIds(), QStringList({"a"}));
    QTRY_COMPARE(m_dots->rowCount(), 1);
    QCOMPARE(dotIds(), QStringList({"a"}));
    QVERIFY(m_tracks->hasData());
    QVERIFY(isNear(m_tracks->boundsWest(), -75.0));
    QVERIFY(isNear(m_tracks->boundsEast(), -75.0));
    QVERIFY(isNear(m_tracks->boundsNorth(), 45.0295));
    QVERIFY(isNear(m_tracks->boundsSouth(), 45.0));
    QCOMPARE(hasDataSpy.count(), 1);
    QVERIFY(boundsSpy.count() >= 1);

    correctSource("b", 1.0);

    QTRY_COMPARE(m_tracks->rowCount(), 2);
    QTRY_COMPARE(m_dots->rowCount(), 2);
    QCOMPARE(trackIds(), QStringList({"a", "b"}));
    QCOMPARE(dotIds(), QStringList({"a", "b"}));
    QVERIFY(isNear(m_tracks->boundsSouth(), 45.0));
    QVERIFY(isNear(m_tracks->boundsNorth(), 45.0295));
    QVERIFY(isNear(m_tracks->boundsWest(), -75.0));
    QVERIFY(isNear(m_tracks->boundsEast(), -74.0));
    QVERIFY(isNear(m_tracks->centerLat(), 45.01475));
    QVERIFY(isNear(m_tracks->centerLon(), -74.5));
}

// A correction does not show a recording the user has hidden.
void MapModelsTest::hiddenRecordingStaysOutAfterCorrection()
{
    const int row = m_model->getSessionRow(QStringLiteral("a"));
    QVERIFY(row >= 0);
    m_model->setRowsVisibility({{row, false}});
    QTRY_COMPARE(m_tracks->rowCount(), 1);
    QCOMPARE(trackIds(), QStringList({"b"}));

    QSignalSpy resetSpy(m_tracks.get(), &TrackMapModel::modelReset);

    correctSource("a", 10.0);
    QTRY_VERIFY(resetSpy.count() >= 1);
    QCOMPARE(trackIds(), QStringList({"b"}));

    const int resets = resetSpy.count();
    correctSource("a", 1.0);
    QTRY_VERIFY(resetSpy.count() > resets);
    QCOMPARE(trackIds(), QStringList({"b"}));
    QCOMPARE(dotIds(), QStringList({"b"}));

    // The row may have moved; look it up again
    m_model->setRowsVisibility({{m_model->getSessionRow(QStringLiteral("a")), true}});
    QTRY_COMPARE(m_tracks->rowCount(), 2);
    QTRY_COMPARE(m_dots->rowCount(), 2);
    QCOMPARE(trackIds(), QStringList({"a", "b"}));
    QVERIFY(isNear(m_tracks->boundsWest(), -75.0));
}

// The boundary interpolation of a plot range works on the index-retained
// track: the exit time is T0 + 9 s, between the two retained samples.
void MapModelsTest::rangeFilterOnRecoveredTrack()
{
    correctSource("a", 10.0);
    QTRY_COMPARE(m_tracks->rowCount(), 1);
    correctSource("a", 1.0);
    QTRY_COMPARE(m_tracks->rowCount(), 2);

    QSignalSpy resetSpy(m_tracks.get(), &TrackMapModel::modelReset);
    m_range->setRange(QString::fromLatin1(SessionKeys::Time), QString::fromLatin1(SessionKeys::ExitTime),
                      -0.25, 0.25);
    QTRY_VERIFY(resetSpy.count() >= 1);
    QCOMPARE(m_tracks->rowCount(), 2);
    QCOMPARE(trackPoints(QStringLiteral("a")).size(), 2);
    QCOMPARE(trackPoints(QStringLiteral("b")).size(), 2);

    for (const QString &id : {QStringLiteral("a"), QStringLiteral("b")}) {
        const QVariantList points = trackPoints(id);
        const double first = points.first().toMap().value(QStringLiteral("t")).toDouble();
        const double last = points.last().toMap().value(QStringLiteral("t")).toDouble();
        QCOMPARE(first, T0 + 8.75);
        QCOMPARE(last, T0 + 9.25);
        QCOMPARE(last - first, 0.5);

        // 9 s into a 295 s track from 45.0 to 45.0295: 0.0001 degrees a second
        const double lat = points.first().toMap().value(QStringLiteral("lat")).toDouble();
        QVERIFY2(qAbs(lat - 45.000875) <= 1e-9, qPrintable(QString::number(lat, 'g', 17)));
    }

    QTRY_COMPARE(m_dots->rowCount(), 2);
    QCOMPARE(dotIds(), QStringList({"a", "b"}));
}

FLYSIGHT_TEST_MAIN(MapModelsTest)
#include "tst_map_models.moc"
